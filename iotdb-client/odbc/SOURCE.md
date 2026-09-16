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
# Source provenance and dependencies

ODBC source imported from `git@gitlab-eco.timecho.com:r-d/collector/iotdb-odbc-2.git`,
local commit `21db94e` (Merge branch `feat/WindowsConfig` into `main`).
This is a source snapshot, not a Git history merge. The original repository is
retained. Remote freshness was not checked successfully because SSH authentication
failed in the migration environment.

The driver, Windows configuration resources and C++/Python examples were imported.
Old installers, binaries, IDE caches, screenshots and obsolete build instructions
were excluded. The REST dependencies retain their original vendored sources:
curl's CMake library build subset and the JSON single header plus license.

| Component | Version/source | Distribution |
| --- | --- | --- |
| C++ Session | same Apache IoTDB checkout | statically linked |
| Thrift | version pinned by client-cpp (currently 0.24.0) | statically linked; Apache-2.0 |
| OpenSSL | checksum-pinned client-cpp source (currently 3.5.8) | statically linked; Apache-2.0 |
| Boost | client-cpp Thrift build headers | Boost Software License 1.0 |
| curl | upstream ODBC vendored snapshot, reports 8.4.0-DEV | statically linked; curl license |
| nlohmann/json | 3.11.3 | header-only; MIT |
| GCC runtime (Linux) | release build toolchain | static; GCC Runtime Library Exception |

The curl version label is inherited from the source snapshot; it is not a claim
that this snapshot equals an upstream release. Third-party updates are separate
from the migration. Licenses are shipped under `licenses/`; the repository
LICENSE covers Apache-2.0 components. C++ SDK package descriptions refer to its
standalone shared-library packaging; the table above describes ODBC packaging.
