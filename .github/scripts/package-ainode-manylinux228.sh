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

# --- Python 3.11 with a shared libpython -----------------------------------
# manylinux's /opt/python interpreters are built WITHOUT --enable-shared (they
# exist to build wheels), so PyInstaller cannot use them: it needs libpython*.so.
# Install the AlmaLinux 8 system python3.11 instead (ships libpython3.11.so /
# Py_ENABLE_SHARED=1) and make `python3`/`python` resolve to it, since
# build_binary.py is invoked by Maven as `python3` and creates its venv from it.
if command -v dnf >/dev/null 2>&1; then
  dnf install -y python3.11 python3.11-devel python3.11-pip
else
  yum install -y python3.11 python3.11-devel python3.11-pip
fi
PYSHIM=/opt/py311-shim
mkdir -p "${PYSHIM}"
ln -sf /usr/bin/python3.11 "${PYSHIM}/python3"
ln -sf /usr/bin/python3.11 "${PYSHIM}/python"
export PATH="${PYSHIM}:${PATH}"
python --version
python3 --version
# Fail fast if the interpreter lacks a shared libpython (PyInstaller hard
# requirement) before spending minutes installing torch.
python3 - <<'PYEOF'
import sys, sysconfig
shared = sysconfig.get_config_var("Py_ENABLE_SHARED")
print("Py_ENABLE_SHARED:", shared, "LDLIBRARY:", sysconfig.get_config_var("LDLIBRARY"))
if not shared:
    sys.exit("ERROR: python3.11 was built without a shared libpython; PyInstaller cannot use it")
PYEOF

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

# Self-heal a cached venv built without a shared libpython (e.g. restored from a
# previous run that used manylinux's static /opt/python). build_binary.py reuses
# an existing venv as-is, so a stale broken venv would keep failing PyInstaller.
# GitHub caches are immutable per key, so removing it here is the reliable fix.
VENV_DIR="${HOME}/.cache/iotdb-ainode-build/ainode"
VENV_PY="${VENV_DIR}/bin/python"
if [[ -x "${VENV_PY}" ]]; then
  if ! "${VENV_PY}" -c 'import sysconfig, sys; sys.exit(0 if sysconfig.get_config_var("Py_ENABLE_SHARED") else 1)'; then
    echo "Removing stale venv at ${VENV_DIR} (built without shared libpython)"
    rm -rf "${VENV_DIR}"
  fi
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
