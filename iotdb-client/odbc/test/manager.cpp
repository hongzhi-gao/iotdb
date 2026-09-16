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
#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <codecvt>
#include <locale>
#include <stdexcept>
#include <algorithm>

#ifdef _WIN32
// Windows' driver manager requires a registered name. Redirect HKLM for this
// process only, using a disposable user-owned key; never install a system driver.
class DriverRegistrySandbox {
  HKEY root_ = nullptr;
  std::string path_;
  bool redirected_ = false;

public:
  explicit DriverRegistrySandbox(std::string dll) {
    std::replace(dll.begin(), dll.end(), '/', '\\');
    path_ = "Software\\IoTDBOdbcTest-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64());
    if (RegCreateKeyExA(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                        KEY_ALL_ACCESS, nullptr, &root_, nullptr) != ERROR_SUCCESS)
      throw std::runtime_error("Cannot create temporary registry sandbox");
    auto write = [&](const char* path, const char* name, const std::string& value) {
      HKEY key = nullptr;
      LONG rc = RegCreateKeyExA(root_, path, 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                                nullptr, &key, nullptr);
      if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExA(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>(value.size() + 1));
        RegCloseKey(key);
      }
      return rc == ERROR_SUCCESS;
    };
    if (!write("Software\\ODBC\\ODBCINST.INI\\IoTDBMigrationTest", "Driver", dll) ||
        !write("Software\\ODBC\\ODBCINST.INI\\ODBC Drivers", "IoTDBMigrationTest", "Installed") ||
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, root_) != ERROR_SUCCESS) {
      RegCloseKey(root_);
      RegDeleteTreeA(HKEY_CURRENT_USER, path_.c_str());
      throw std::runtime_error("Cannot configure process-local ODBC registry");
    }
    redirected_ = true;
  }
  void restore() {
    if (redirected_)
      RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    redirected_ = false;
  }
  ~DriverRegistrySandbox() {
    restore();
    if (root_)
      RegCloseKey(root_);
    RegDeleteTreeA(HKEY_CURRENT_USER, path_.c_str());
  }
};
#endif

static std::string utf8Environment(const char* name) {
#ifdef _WIN32
  std::string key(name);
  std::wstring wideKey(key.begin(), key.end());
  const wchar_t* value = _wgetenv(wideKey.c_str());
  return value ? std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes(value) : "";
#else
  const char* value = std::getenv(name);
  return value ? value : "";
#endif
}

// Without IOTDB_ODBC_CONNECTION, reject a TLS/REST mismatch without network I/O.
// With it, connect to a real server and execute IOTDB_ODBC_QUERY or SHOW VERSION.
int runTest(int argc, char** argv) {
  if (argc != 2)
    return 2;
  std::string driver = argv[1];
#ifdef _WIN32
  DriverRegistrySandbox registry(driver);
  driver = "IoTDBMigrationTest";
#endif
  SQLHENV env = nullptr;
  SQLHDBC dbc = nullptr;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env)))
    return 1;
  SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc)))
    return 1;
  std::string optionsText = utf8Environment("IOTDB_ODBC_CONNECTION");
  const char* options = optionsText.empty() ? nullptr : optionsText.c_str();
#ifdef _WIN32
  if (options) {
    // Attach the driver to this manager handle without opening a socket. Restore
    // HKLM before real I/O so CryptoAPI and Winsock see their normal OS settings.
    std::string prime = "DRIVER={" + driver + "};SSL=invalid;";
    SQLDriverConnect(dbc, nullptr, reinterpret_cast<SQLCHAR*>(&prime[0]), SQL_NTS, nullptr, 0,
                     nullptr, SQL_DRIVER_NOPROMPT);
    registry.restore();
  }
#endif
  std::string conn = std::string("DRIVER={") + driver + "};SSL=0;" +
                     (options ? options : "RESTFUL=1;SSL=1;SSLCA=missing.pem");
  SQLRETURN rc = SQLDriverConnect(dbc, nullptr, reinterpret_cast<SQLCHAR*>(&conn[0]), SQL_NTS,
                                  nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
  bool expectedFailure = !options || std::getenv("IOTDB_ODBC_EXPECT_FAILURE");
  bool ok = false;
  if (expectedFailure) {
    SQLCHAR state[6]{}, message[1024]{};
    SQLINTEGER native{};
    SQLSMALLINT length{};
    SQLGetDiagRec(SQL_HANDLE_DBC, dbc, 1, state, &native, message, sizeof(message), &length);
    ok = rc == SQL_ERROR && std::string(reinterpret_cast<char*>(state)) == "08001";
    if (!ok)
      std::cerr << state << ": " << message << '\n';
  } else if (SQL_SUCCEEDED(rc)) {
    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
    std::string queryText = utf8Environment("IOTDB_ODBC_QUERY");
    const char* query = queryText.empty() ? nullptr : queryText.c_str();
    std::string sql = query ? query : "SHOW VERSION";
    rc = SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(&sql[0]), SQL_NTS);
    if (SQL_SUCCEEDED(rc)) {
      SQLSMALLINT columns = 0;
      SQLNumResultCols(stmt, &columns);
      ok = columns == 0;
      int rows = 0;
      bool foundValue = false;
      std::string expectedText = utf8Environment("IOTDB_ODBC_EXPECT_VALUE");
      const char* expectedValue = expectedText.empty() ? nullptr : expectedText.c_str();
      while (SQL_SUCCEEDED(rc = SQLFetch(stmt))) {
        ++rows;
        for (SQLUSMALLINT col = 1; col <= columns; ++col) {
          SQLCHAR value[4096]{};
          SQLLEN length{};
          if (!SQL_SUCCEEDED(SQLGetData(stmt, col, SQL_C_CHAR, value, sizeof(value), &length)))
            return 1;
          if (length != SQL_NULL_DATA && expectedValue &&
              std::string(reinterpret_cast<char*>(value)) == expectedValue)
            foundValue = true;
        }
      }
      if (columns > 0)
        ok = rc == SQL_NO_DATA && rows > 0;
      if (const char* expectedRows = std::getenv("IOTDB_ODBC_EXPECT_ROWS"))
        ok = ok && rows == std::stoi(expectedRows);
      if (expectedValue)
        ok = ok && foundValue;
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLDisconnect(dbc);
  } else {
    SQLCHAR state[6]{}, message[1024]{};
    SQLINTEGER native{};
    SQLSMALLINT length{};
    SQLGetDiagRec(SQL_HANDLE_DBC, dbc, 1, state, &native, message, sizeof(message), &length);
    std::cerr << state << ": " << message << '\n';
  }
  SQLFreeHandle(SQL_HANDLE_DBC, dbc);
  SQLFreeHandle(SQL_HANDLE_ENV, env);
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  try {
    return runTest(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
