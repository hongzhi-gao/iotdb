# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# =============================================================================
# FetchSystemOpenSSL.cmake  (IOTDB_SSL_PROVIDER=SYSTEM)
#
# Link against the host OpenSSL installation. TLS only — TLCP/NTLS is disabled
# at compile time. Release packages and CI use bundled Tongsuo instead.
# =============================================================================

find_package(OpenSSL REQUIRED)

if(NOT OPENSSL_VERSION_MAJOR VERSION_GREATER_EQUAL 3)
    message(WARNING
            "[OpenSSL] system provider: OpenSSL ${OPENSSL_VERSION} found; "
            "OpenSSL 3.x is recommended")
endif()

set(IOTDB_SSL_BUNDLE_RUNTIME OFF CACHE INTERNAL "Do not bundle system SSL into SDK zip")

message(STATUS "[OpenSSL] using system SSL provider at ${OPENSSL_ROOT_DIR}")
