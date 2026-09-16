/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include "ConnectionHandle.h"
#include "ConnectionString.h"
#include "ODBCField.h"
#include "driver.h"
#include <iostream>
#include <fstream>
#include <iterator>

static void check(bool ok) {
  if (!ok)
    throw std::runtime_error("ODBC unit assertion failed");
}
static bool invalid(const ConnectionHandle& c) {
  try {
    c.ValidateTransport();
    return false;
  } catch (const std::invalid_argument&) {
    return true;
  }
}
int main() {
  try {
    ConnectionHandle c(nullptr);
    check(!invalid(c));
    SetConnectionHandle(&c, "SSL", "true");
    check(invalid(c));
    SetConnectionHandle(&c, "sSlCa", "/certs/ca.pem");
    check(!invalid(c));
    SetConnectionHandle(&c, "SSLCERT", "/certs/client.pem");
    check(invalid(c));
    SetConnectionHandle(&c, "SSLKEY", "/certs/client.key");
    check(!invalid(c));
    SetConnectionHandle(&c, "RESTFUL", "1");
    check(invalid(c));
    SetConnectionHandle(&c, "RESTFUL", "0");
    SetConnectionHandle(&c, "SSL", "garbage");
    check(invalid(c));
    auto parsed = ParseConnectionString(
        " Driver={Apache IoTDB ODBC Driver};PWD={a;b}}c};SSLCA={C:\\certs\\ca.pem};SSL=1;");
    check(parsed.size() == 4 && parsed[1].second == "a;b}c");
    try {
      ParseConnectionString("SSLCA={broken");
      check(false);
    } catch (const std::invalid_argument&) {
    }
    SQLCHAR output[64]{};
    SQLLEN size = 0;
    ODBCField date(IoTDBDate(2024, 2, 29));
    check(date.toChar(output, sizeof(output), &size));
    check(std::string(reinterpret_cast<char*>(output)) == "2024-02-29");
    SQL_DATE_STRUCT d{};
    check(date.toDate(&d) && d.year == 2024 && d.month == 2 && d.day == 29);
    ODBCField null = ODBCField::null();
    check(null.toChar(output, sizeof(output), &size) && size == SQL_NULL_DATA);
    ODBCField unicode(std::string("\xe4\xb8\xad\xe6\x96\x87"));
    SQLWCHAR wide[16]{};
    check(unicode.toWChar(wide, sizeof(wide), &size));
    check(size == 4 && wide[0] == 0x4e2d && wide[1] == 0x6587 && wide[2] == 0);
    Field timestamp;
    timestamp.dataType = TSDataType::TIMESTAMP;
    timestamp.longV = int64_t(1234);
    SQL_TIMESTAMP_STRUCT ts{};
    check(ODBCField(timestamp).toTimestamp(&ts));
    check(ts.year == 1970 && ts.second == 1 && ts.fraction == 234000000);
    timestamp.longV = int64_t(-1);
    check(ODBCField(timestamp).toTimestamp(&ts));
    check(ts.year == 1969 && ts.month == 12 && ts.day == 31 && ts.hour == 23 && ts.minute == 59 &&
          ts.second == 59 && ts.fraction == 999000000);
    // This fails before network I/O and exercises the public diagnostic path.
    ConnectionHandle bad(nullptr);
    bad.addDiagnostic("HY000", "Stale error from a previous connection attempt");
    std::string connection = "LOGLEVEL=4;SSL=1;SSLCA=x;RESTFUL=1;PWD=do-not-log";
    check(SQLDriverConnect(&bad, nullptr, reinterpret_cast<SQLCHAR*>(&connection[0]),
                           static_cast<SQLSMALLINT>(connection.size()), nullptr, 0, nullptr,
                           SQL_DRIVER_NOPROMPT) == SQL_ERROR);
    std::string state, message;
    int native = 0;
    check(bad.getDiagnostic(1, state, message, native) && state == "08001");
    SQLCHAR sqlState[6]{}, errorMessage[512]{};
    SQLINTEGER code = 0;
    SQLSMALLINT errorLength = 0;
    check(SQLError(nullptr, &bad, nullptr, sqlState, &code, errorMessage, sizeof(errorMessage),
                   &errorLength) == SQL_SUCCESS);
    check(SQLError(nullptr, &bad, nullptr, sqlState, &code, errorMessage, sizeof(errorMessage),
                   &errorLength) == SQL_NO_DATA);
#ifndef WIN32
    ConnectionHandle dsn(nullptr);
    std::string dsnString = "DSN=ODBC_TLS_TEST;SSLCA={override;ca.pem};RESTFUL=1;";
    check(SQLDriverConnect(&dsn, nullptr, reinterpret_cast<SQLCHAR*>(&dsnString[0]), SQL_NTS,
                           nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT) == SQL_ERROR);
    check(dsn.sslConfig.useSsl && dsn.sslConfig.trustCertFilePath == "override;ca.pem" &&
          dsn.sslConfig.clientCertificateFilePath == "dsn-client.pem");
#endif
    std::ifstream log("apache_iotdb_odbc.log");
    std::string logText((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    check(logText.find("do-not-log") == std::string::npos);
    std::cout << "ODBC configuration, conversion and diagnostic tests passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
