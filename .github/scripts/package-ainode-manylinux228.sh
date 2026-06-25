#!/usr/bin/env bash
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with this
# work for additional information regarding copyright ownership.  The ASF
# licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Build the AINode distribution inside manylinux_2_28 so the PyInstaller bundle
# links against glibc 2.28 (compatible with RHEL8 / Ubuntu 20.04+ / Kylin V10 /
# UOS and newer). Verifies the bundle's max required GLIBC symbol is <= 2.28 and
# smoke-tests the frozen binary offline.
set -euxo pipefail

MACHINE=$(uname -m)
case "${MACHINE}" in
  x86_64)
    JDK_API_ARCH=linux/x64
    DEFAULT_CLASSIFIER=linux-x86_64-glibc2.28
    ;;
  aarch64)
    JDK_API_ARCH=linux/aarch64
    DEFAULT_CLASSIFIER=linux-aarch64-glibc2.28
    ;;
  *)
    echo "Unsupported architecture: ${MACHINE}" >&2
    exit 1
    ;;
esac
PACKAGE_CLASSIFIER="${PACKAGE_CLASSIFIER:-${DEFAULT_CLASSIFIER}}"

# --- Python 3.11 -----------------------------------------------------------
# manylinux images ship multiple CPythons under /opt/python. pyproject pins
# python >=3.11,<3.12, and build_binary.py is invoked by Maven as `python3`.
PY311_BIN=/opt/python/cp311-cp311/bin
if [[ ! -x "${PY311_BIN}/python" ]]; then
  echo "ERROR: CPython 3.11 not found in manylinux image at ${PY311_BIN}" >&2
  exit 1
fi
export PATH="${PY311_BIN}:${PATH}"
python --version
python3 --version

# --- JDK 17 for Maven ------------------------------------------------------
JAVA_HOME=/opt/jdk-17
if [[ ! -x "${JAVA_HOME}/bin/java" ]]; then
  curl -fsSL -o /tmp/jdk17.tar.gz "https://api.adoptium.net/v3/binary/latest/17/ga/${JDK_API_ARCH}/jdk/hotspot/normal/eclipse?project=jdk"
  rm -rf /opt/jdk-17*
  mkdir -p /opt
  tar xf /tmp/jdk17.tar.gz -C /opt
  JH=$(find /opt -maxdepth 1 -type d -name 'jdk-17*' -print -quit)
  ln -sfn "${JH}" /opt/jdk-17
fi
export JAVA_HOME=/opt/jdk-17
export PATH="${JAVA_HOME}/bin:${PATH}"
java -version

# Optional: UPX shrinks the bundle. ainode.spec sets upx=True, but PyInstaller
# silently skips UPX compression when the binary is not on PATH, so it is not a
# hard requirement. Install it when available for a smaller artifact.
if command -v dnf >/dev/null 2>&1; then
  dnf install -y upx || echo "upx not available; PyInstaller will skip UPX compression"
fi

cd "${GITHUB_WORKSPACE:?GITHUB_WORKSPACE is not set}"

# Build the AINode convenience binary. The `with-ainode` profile pulls in the
# iotdb-ainode module (which runs build_binary.py: poetry install + PyInstaller)
# and assembles apache-iotdb-<version>-ainode-bin.zip under distribution/target.
./mvnw clean package -P with-ainode -pl distribution -am -DskipTests \
  -Dspotless.skip=true

ZIP=$(find distribution/target -maxdepth 1 -type f -name "apache-iotdb-*-ainode-bin.zip" -print -quit)
if [[ -z "${ZIP}" ]]; then
  echo "ERROR: could not find AINode package zip under distribution/target" >&2
  exit 1
fi
echo "Built: ${ZIP}"

# --- glibc baseline check --------------------------------------------------
# Inspect the frozen 'ainode' bootloader (compiled in this container) plus every
# bundled native .so. torch/numpy/scipy wheels are manylinux_2_28, so the whole
# bundle must stay <= glibc 2.28.
UNPACK=/tmp/ainode-pkg-smoke
rm -rf "${UNPACK}"; mkdir -p "${UNPACK}"
unzip -q -o "${ZIP}" -d "${UNPACK}"
AIN_EXE=$(find "${UNPACK}" -type f -name ainode -path "*/lib/*" -print -quit)
if [[ -z "${AIN_EXE}" ]]; then
  echo "ERROR: frozen 'ainode' executable not found under lib/ in the package" >&2
  exit 1
fi
LIB_DIR=$(dirname "${AIN_EXE}")

echo "=== Build host glibc ==="
ldd --version 2>&1 | sed -n '1p'

echo "=== Scanning GLIBC symbols in the bundle (this may take a moment) ==="
max_glibc=$(
  {
    objdump -T "${AIN_EXE}" 2>/dev/null || true
    find "${LIB_DIR}" -type f -name "*.so*" -exec objdump -T {} + 2>/dev/null || true
  } | grep -oE "GLIBC_[0-9.]+" | sed "s/GLIBC_//" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1
)
echo "max_glibc=${max_glibc}"
if [[ -z "${max_glibc}" ]]; then
  echo "ERROR: could not determine max GLIBC version from the bundle" >&2
  exit 1
fi
if awk -v max="${max_glibc}" 'BEGIN { exit !(max > 2.28) }'; then
  echo "ERROR: AINode bundle requires glibc > 2.28 (max=${max_glibc})" >&2
  exit 1
fi
echo "glibc compatibility check passed (max=${max_glibc} <= 2.28)"

# --- Smoke test ------------------------------------------------------------
# 'pull-models --list' drives the frozen binary end-to-end: it imports the whole
# iotdb.ainode.core package (proving the PyInstaller bundle resolves) and exits 0
# without touching the network or requiring a running cluster.
"${AIN_EXE}" pull-models --list

echo "AINode manylinux_2_28 package build + checks passed (${PACKAGE_CLASSIFIER}): ${ZIP}"
