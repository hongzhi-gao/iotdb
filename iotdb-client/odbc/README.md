<!--

    Licensed to the Apache Software Foundation (ASF) under one
    or more contributor license agreements.  See the NOTICE file
    distributed with this work for additional information
    regarding copyright ownership.  The ASF licenses this file
    to you under the Apache License, Version 2.0 (the
    "License"); you may not use this file except in compliance
    with the License.  You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing,
    software distributed under the License is distributed on an
    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
    KIND, either express or implied.  See the License for the
    specific language governing permissions and limitations
    under the License.

-->
# IoTDB ODBC driver

The driver builds directly against `../client-cpp`. Each release contains one
driver library with the Session client, Thrift, OpenSSL and curl linked statically.
It does not require a separately installed C++ SDK, OpenSSL or VC redistributable.
The OS libraries and ODBC manager/installer remain system prerequisites.

## Build

Use CMake 3.20+, a C++14 compiler and the C++ SDK build prerequisites. Windows
requires Visual Studio 2022 x64, Strawberry Perl and winflexbison3. Linux requires
unixODBC development headers. Dependency archives use the C++ SDK cache under
`../client-cpp/third-party`; `-DIOTDB_OFFLINE=ON` requires a populated cache.

```sh
cmake -S iotdb-client/odbc -B iotdb-client/odbc/target/build -DCMAKE_BUILD_TYPE=Release
cmake --build iotdb-client/odbc/target/build --config Release --parallel 4
ctest --test-dir iotdb-client/odbc/target/build -C Release --output-on-failure
cd iotdb-client/odbc/target/build
cpack -C Release
```

On Windows pass `-A x64` to configure. Maven also supports
`./mvnw package -P with-odbc -pl iotdb-client/odbc`; CMake and CPack must be on PATH.
`-DskipTests` skips test execution. Builds do not register the driver on the host.

For Linux release packages use the same manylinux 2.28 baseline as the C++ SDK:

```sh
source .github/scripts/native-linux-env.sh
docker run --rm -v "$PWD:/workspace" -w /workspace "$NATIVE_LINUX_IMAGE" \
  bash .github/scripts/package-odbc-manylinux228.sh
```

The release script builds all dependencies in this environment, runs tests and
rejects unexpected shared-library dependencies or GLIBC requirements above 2.28.
A build on a newer host is a development build, not evidence of glibc 2.28 compatibility.
Release scope is Linux x86_64 and Windows x64 (Windows 10/11, Server 2016+).
Packages and SHA-512 checksums are written into the CMake build directory.

## Install

Windows: extract the release, then run `install.bat "C:\Program Files\Apache IoTDB ODBC Driver"`
from an administrator command prompt. Use the **64-bit** ODBC administrator to
configure a DSN. `uninstall.bat` unregisters the driver, preserving DSNs and files.

Linux: install unixODBC, extract the release, update `Driver` in `conf/odbcinst.ini`
to the absolute library path, then register with `odbcinst -i -d -f conf/odbcinst.ini`.
Copy the example DSN into your user `odbc.ini` and provide your credentials.

## Connection and encryption

Existing `SERVER`, `PORT`, `UID`, `PWD`, `DATABASE`, `ISTABLEMODEL`, `LOGLEVEL`,
`SESSIONTIMEOUTMS` and `BATCHSIZE` parameters remain available. `RESTFUL=1` selects
the retained HTTP REST implementation; specify its HTTP port explicitly.
The default is Session RPC on port 6667. Explicit connection attributes override
DSN values. Values containing semicolons can be braced, with `}}` escaping `}`.

```text
DRIVER={Apache IoTDB ODBC Driver};SERVER=localhost;PORT=6667;UID=root;PWD={your password};DATABASE=information_schema;SSL=1;SSLCA={C:\certs\ca.pem};
```

| Attribute | Default | Meaning |
| --- | --- | --- |
| SSL | 0 | Enable Session TLS; accepts 0/1, false/true, no/yes, off/on |
| SSLCA | empty | Trusted CA file in PEM format; required for TLS |
| SSLCERT | empty | PEM client certificate for mutual TLS |
| SSLKEY | empty | PEM client private key; required together with SSLCERT |

These attributes are available in connection strings, DSNs and the Windows setup
dialog. Certificates are external files readable by the application account;
they are not compiled into the driver. The server must enable the corresponding
RPC TLS mode. The client verifies trust and server identity and never falls back
to plaintext. Connection testing and queries use the same Session construction.
Certificate attributes with SSL disabled are rejected. REST combined with these
TLS options is rejected: HTTPS is outside this migration's scope.

## Validation

CTest covers configuration, type conversions, diagnostics and loading through the
system ODBC manager. The manager test deliberately rejects an invalid TLS/REST
combination without contacting a server. Windows tests use a temporary,
process-local registry sandbox and do not install a system ODBC driver.
To test a running server:

```sh
export IOTDB_ODBC_CONNECTION='SERVER=localhost;PORT=6667;UID=root;PWD=your-password;ISTABLEMODEL=0'
target/build/test/odbc_manager /absolute/path/libapache_iotdb_odbc.so
```

Set `IOTDB_ODBC_QUERY` to exercise a query and `IOTDB_ODBC_EXPECT_FAILURE=1` for
negative connection tests. For plain/TLS/mTLS server phases, run
`python test/run_e2e.py --help`; use a disposable server distribution and test data.
The runner requires Java 17+, `keytool`, Python 3 and (on Linux) `lsof` or `netstat`.
Set `IOTDB_TEST_PASSWORD` to the test server password. It copies the supplied
distribution, refuses occupied test ports and removes the temporary copy afterward.
See `VALIDATION.md` for checks actually performed during migration.
