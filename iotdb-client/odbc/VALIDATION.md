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
# Migration validation

Local validation on 2026-09-16, source snapshot `21db94e`, against the in-tree
C++ SDK. These are development validation results, not certification of every
supported operating-system release or ODBC application.

## Build and packaging

| Check | Result |
| --- | --- |
| Linux x86_64, manylinux_2_28, GCC 14 | Release driver builds; both CTests pass |
| Linux ABI | Highest required symbol version is GLIBC_2.28; no RPATH/RUNPATH |
| Linux direct shared dependencies | libc, libm, libdl, libpthread, libodbcinst and the system ELF loader only |
| Clean AlmaLinux 8, Ubuntu 22.04, Ubuntu 24.04 | Packaged driver loads through unixODBC and executes SHOW VERSION against a test server |
| Windows x64, Visual Studio 2022 / MSVC 19.43 | Release driver builds; both CTests pass |
| Windows dependencies | x64 DLL imports only Windows system libraries, not third-party DLLs or the VC redistributable |
| Existing C++ SDK shared mode | Build, install, examples and session_utils_tests pass (260 assertions, 7 cases) |

ODBC statically links Session, Thrift, OpenSSL and curl, plus the compiler runtime.
Linux glibc and the platform ODBC manager/installer remain external. Package
checks do not imply that arbitrary builds on newer Linux hosts meet this baseline.

## Functional and encryption checks

Linux and Windows x64 tests against a local Apache IoTDB 2.0.11-SNAPSHOT
distribution passed:

- Session plain, TLS and mutual TLS, through the system ODBC manager.
- Tree and table models: create, insert, query and cleanup.
- Three rows with fetch size two to exercise paging; Chinese text, NULL and DATE.
- Retained HTTP REST path in both models.
- Rejection of missing/untrusted CA, plaintext against a TLS endpoint,
  missing/invalid client certificate, and a trusted server certificate whose
  identity does not match the requested hostname.

Unit tests additionally cover braced connection strings, TLS option validation,
DSN overrides on Linux, diagnostic enumeration, UTF-16 conversion, pre-epoch
timestamps, clearing stale connection diagnostics on retries and avoiding
password disclosure in connection logs.

## Limits and remaining release checks

Live protocol checks used a local Apache IoTDB distribution. Rerun
`test/run_e2e.py` against the intended release artifact before production
acceptance.

Interactive Windows DSN dialog operation and administrator installation/removal
have not been manually exercised. Tests do not permanently register a driver.
Windows 10 and Server 2016 were not separately available for runtime testing.
The GitHub Actions Linux/Windows package jobs and three clean Linux runtime jobs
passed during migration.
HTTPS for REST and a general ODBC conformance audit are outside this migration.
The vendored curl snapshot is retained from the original repository; upgrading
third-party dependencies should be a separate reviewed change.
