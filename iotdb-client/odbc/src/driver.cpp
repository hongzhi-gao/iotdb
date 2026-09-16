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
#include "ODBCField.h"
#include "Pch.h"
#include "driver.h"

// Additional includes not in Pch.h
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <codecvt> // Need to support wide characters

// ODBC includes
#include <odbcinst.h>
#include <sql.h>
#include <sqltypes.h>

// Project includes
#include "DiagnosticManager.h"
#include "Log.h"
#include "rest_api_client.h"
#include "session_api_client.h"
#include "StatementHandle.h"
#include "ConnectionHandle.h"
#include "ConnectionString.h"
#include "EnvironmentHandle.h"
#include "CopyField.h"

/**
 * Copy std::string to ODBC string buffer
 * @param value input string
 * @param stringBuffer output buffer
 * @param bufferLength buffer size (in bytes)
 * @param stringLength returns actual string length (excluding null terminator)
 */
SQLRETURN setString(const std::string& value, SQLPOINTER stringBuffer, SQLSMALLINT bufferLength,
                    SQLSMALLINT* stringLength) {
  const SQLSMALLINT actualLength = static_cast<SQLSMALLINT>(
      std::min(value.size(), static_cast<size_t>(std::numeric_limits<SQLSMALLINT>::max())));

  if (stringLength) {
    *stringLength = actualLength;
  }

  if (stringBuffer && bufferLength > 0) {
    const size_t maxCopy = std::min(value.size(), static_cast<size_t>(bufferLength - 1));

    if (!value.empty()) {
      std::memcpy(stringBuffer, value.data(), maxCopy);
    }
    static_cast<char*>(stringBuffer)[maxCopy] = '\0';
  }
  return SQL_SUCCESS;
}

/**
 * Copy C-style string to ODBC string buffer
 * @param value input string (must be null-terminated)
 * @param stringBuffer output buffer
 * @param bufferLength buffer size (in bytes)
 * @param stringLength returns actual string length (excluding null terminator)
 */
SQLRETURN setString(const char* value, SQLPOINTER stringBuffer, SQLSMALLINT bufferLength,
                    SQLSMALLINT* stringLength) {
  if (!value) {
    value = "";
    return SQL_SUCCESS;
  }

  if (stringBuffer && bufferLength > 0) {
    size_t valueLen = std::strlen(value);
    size_t maxCopy = std::min(valueLen, static_cast<size_t>(bufferLength - 1));

    std::strncpy(static_cast<char*>(stringBuffer), value, maxCopy);
    static_cast<char*>(stringBuffer)[maxCopy] = '\0';

    if (stringLength) {
      *stringLength = static_cast<SQLSMALLINT>(maxCopy);
    }
  } else if (stringLength) {
    *stringLength = static_cast<SQLSMALLINT>(std::strlen(value));
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLAllocConnect(SQLHENV environmentHandle, SQLHDBC* connectionHandle) {
  logMessage("Enter SQLAllocConnect");
  // This is a deprecated function, but unixodbc needs to call it. Call SQLAllocHandle function in this function.
  SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_DBC, environmentHandle, connectionHandle);
  if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) {
    // Handle error
    std::ostringstream oss;
    oss << "SQLAllocConnect: Failed to allocate connection handle. retcode = " << retcode << "\n";
    logMessage(oss.str());
    return retcode;
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLAllocEnv(SQLHENV* environmentHandle) {
  logMessage("SQLAllocEnv");
  // Deprecated function, delegate to SQLAllocHandle for env allocation.
  SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, environmentHandle);
  if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) {
    std::ostringstream oss;
    oss << "SQLAllocEnv: Failed to allocate environment handle. retcode = " << retcode << "\n";
    logMessage(oss.str());
    return retcode;
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLAllocHandle(const SQLSMALLINT handleType, const SQLHANDLE parentHandle,
                                 SQLHANDLE* outputHandle) {
  logMessage("SQLAllocHandle: Entering.");

  std::stringstream logStream;
  logStream << "Parameters: "
            << "handleType = " << handleType << ", parentHandle = " << handleToString(parentHandle);
  logMessage(logStream.str());

  bool hasDiagnosticInfo = false;

  switch (handleType) {
  case SQL_HANDLE_ENV: {
    auto* env = new EnvironmentHandle();
    *outputHandle = env;
    logMessage("SQLAllocHandle: Allocated SQL_HANDLE_ENV");
  } break;

  case SQL_HANDLE_DBC: {
    auto* env = static_cast<EnvironmentHandle*>(parentHandle);
    if (env == nullptr) {
      logMessage("SQLAllocHandle: Invalid environment handle!");
      return SQL_INVALID_HANDLE;
    }

    auto* cnct = new ConnectionHandle(env);
    *outputHandle = cnct;
    logMessage("Allocated SQL_HANDLE_DBC");
  } break;

  case SQL_HANDLE_STMT: {
    auto* cnct = static_cast<ConnectionHandle*>(parentHandle);
    if (cnct == nullptr) {
      logMessage("SQLAllocHandle: Invalid connection handle!");
      return SQL_INVALID_HANDLE;
    }
    auto* stmt = new StatementHandle(cnct);
    stmt->appRowDesc = reinterpret_cast<SQLHANDLE>(stmt);
    stmt->impRowDesc = reinterpret_cast<SQLHANDLE>(stmt);
    *outputHandle = stmt;
    logMessage("Allocated SQL_HANDLE_STMT");
  } break;

  default:
    *outputHandle = nullptr;
    logMessage("Unhandled handle type\n");
    if (parentHandle != nullptr) {
      switch (handleType) {
      case SQL_HANDLE_DBC:
        static_cast<EnvironmentHandle*>(parentHandle)
            ->addDiagnostic("HY092", "Invalid handle type: handleType is SQL_HANDLE_DBC");
        break;
      case SQL_HANDLE_STMT:
        static_cast<ConnectionHandle*>(parentHandle)
            ->addDiagnostic("HY092", "Invalid handle type: handleType is SQL_HANDLE_STMT");
        break;
      }
      hasDiagnosticInfo = true;
    }
    return SQL_ERROR;
  }
  logMessage("Allocated handle: " + handleToString(*outputHandle));
  if (hasDiagnosticInfo) {
    logMessage("SQLAllocHandle: Exit successfully with info\n");
    return SQL_SUCCESS_WITH_INFO;
  } else {
    logMessage("SQLAllocHandle: Exit successfully\n");
    return SQL_SUCCESS;
  }
}

SQLRETURN SQL_API SQLAllocStmt(SQLHDBC connectionHandle, SQLHSTMT* statementHandle) {
  logMessage("Entering SQLAllocStmt");
  if (!connectionHandle || !statementHandle) {
    logMessage("Missing connectionHandle or statementHandle\n");
    return SQL_ERROR;
  }

  // Deprecated function, delegate to SQLAllocHandle for stmt allocation.
  SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, connectionHandle, statementHandle);
  if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) {
    std::ostringstream oss;
    oss << "SQLAllocStmt: Failed to allocate statement handle. retcode = " << retcode << "\n";
    logMessage(oss.str());
    return retcode;
  }
  logMessage("Exiting SQLAllocStmt\n");
  return SQL_SUCCESS;
}

/*
The numbering of columnNumber starts from 1, with column 0 being the bookmark column. However, IoTDB does not have a bookmark column. The numbering still starts from 1.
*/
SQLRETURN SQL_API SQLBindCol(SQLHSTMT statementHandle, SQLUSMALLINT columnNumber,
                             SQLSMALLINT targetType, SQLPOINTER targetValuePtr, SQLLEN bufferLength,
                             SQLLEN* strLen_or_IndPtr) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt->getConnection();
  logMessage(cnct, "SQLBindCol: Entering", LOG_LEVEL_TRACE);
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "Parameters: "
              << "statementHandle = " << handleToString(statementHandle)
              << ", columnNumber = " << columnNumber << ", targetType = " << targetType
              << ", targetValue = " << targetValuePtr << ", bufferLength = " << bufferLength
              << ", strLen_or_Ind = " << strLen_or_IndPtr;

    logMessage(stmt->getConnection(), logStream.str(), LOG_LEVEL_TRACE);
  }

  // Delegate to ODBCResultSet's bindColumn method
  SQLRETURN result = stmt->resultSetPtr->bindColumn(columnNumber, targetType, targetValuePtr,
                                                    bufferLength, strLen_or_IndPtr);

  logMessage(stmt->getConnection(), "SQLBindCol: Exiting", LOG_LEVEL_TRACE);
  return result;
}

SQLRETURN SQL_API SQLBindParam(SQLHSTMT statementHandle, SQLUSMALLINT parameterNumber,
                               SQLSMALLINT valueType, SQLSMALLINT parameterType,
                               SQLULEN lengthPrecision, SQLSMALLINT parameterScale,
                               SQLPOINTER parameterValue, SQLLEN* strLen_or_Ind) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLBindParam: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLBindParam is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLBindParam Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLCancel(SQLHSTMT statementHandle) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLCancel: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLCancel is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLCancel Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLCancelHandle(SQLSMALLINT handleType, SQLHANDLE inputHandle) {
  if (!inputHandle) {
    logMessage(nullptr, "SQLCancelHandle: Invalid handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  logMessage("SQLCancelHandle is not implemented");
  if (handleType == SQL_HANDLE_STMT) {
    const auto stmt = static_cast<StatementHandle*>(inputHandle);
    stmt->addDiagnostic("IM001", "SQLCancelHandle Function not implemented");
    return SQL_ERROR;
  } else if (handleType == SQL_HANDLE_DBC) {
    const auto cnct = static_cast<ConnectionHandle*>(inputHandle);
    cnct->addDiagnostic("IM001", "SQLCancelHandle Function not implemented");
    return SQL_ERROR;
  } else {
    logMessage("SQLCancelHandle: Invalid handle type");
    return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLCloseCursor(SQLHSTMT statementHandle) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLCloseCursor: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLCloseCursor is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLCloseCursor Function not implemented");
  }
  return SQL_ERROR;
}

// This function obtains the column name for the specified column number in the result set.
SQLRETURN getColName(StatementHandle* stmt, const SQLUSMALLINT columnNumber,
                     const SQLPOINTER stringBuffer, const SQLSMALLINT bufferLength,
                     SQLSMALLINT* stringLength) {
  ConnectionHandle* cnct = stmt->getConnection();
  logMessage(cnct, "Entering getColName", LOG_LEVEL_TRACE);

  // Use ODBCResultSet to get column names (columnNumber is 1-based, getColumnName uses 0-based)
  std::string columnExpression = stmt->resultSetPtr->getColumnName(columnNumber - 1);

  // Check if a valid column name is obtained
  if (columnExpression.empty() && columnNumber > stmt->resultSetPtr->getNumColumns()) {
    std::string errorMsg = "Requested column " + std::to_string(columnNumber) + " but only " +
                           std::to_string(stmt->resultSetPtr->getNumColumns()) +
                           " columns available";
    logMessage(cnct, errorMsg, LOG_LEVEL_ERROR);
    stmt->addDiagnostic("07009", "Invalid column number");
    return SQL_ERROR;
  }

  SQLSMALLINT actualLength = static_cast<SQLSMALLINT>(columnExpression.size());
  // Set StringLength to the length of the column name
  if (stringLength != nullptr) {
    *stringLength = actualLength;
    logMessage(cnct, "Set string length to " + std::to_string(*stringLength), LOG_LEVEL_DEBUG);
  }

  // Copy columnExpression to stringBuffer
  if (stringBuffer != nullptr) {
    SQLSMALLINT lengthToCopy = (bufferLength > 0)
                                   ? std::min(static_cast<SQLSMALLINT>(actualLength),
                                              static_cast<SQLSMALLINT>(bufferLength - 1))
                                   : 0;
    if (lengthToCopy > 0) {
      std::memcpy(stringBuffer, columnExpression.c_str(), lengthToCopy);
    }

    if (bufferLength > 0) {
      static_cast<char*>(stringBuffer)[lengthToCopy] = '\0';
    }

    if (lengthToCopy < actualLength) {
      std::string truncatedString =
          lengthToCopy == 0 ? "[Empty string]" : columnExpression.substr(0, lengthToCopy);
      logMessage(cnct,
                 "Returning Column name truncated: " + truncatedString + " (original: '" +
                     columnExpression + "')",
                 LOG_LEVEL_WARN);
    } else {
      logMessage(cnct, "Returning column name: " + std::string(static_cast<char*>(stringBuffer)),
                 LOG_LEVEL_DEBUG);
    }
  }

  logMessage(cnct, "Exiting getColName", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

// Map 10 IoTDB data types to ODBC data types, output result via numericAttribute.
void colTypeMapping(ConnectionHandle* cnct, std::string iotdbDataType,
                    const SQLUSMALLINT columnNumber, SQLSMALLINT* numericAttribute) {
  static const std::unordered_map<std::string, SQLSMALLINT> typeMap = {
      {"BOOLEAN", SQL_BIT},    {"INT32", SQL_INTEGER},      {"INT64", SQL_BIGINT},
      {"FLOAT", SQL_REAL},     {"DOUBLE", SQL_DOUBLE},      {"TEXT", SQL_LONGVARCHAR},
      {"STRING", SQL_VARCHAR}, {"BLOB", SQL_LONGVARBINARY}, {"TIMESTAMP", SQL_BIGINT},
      {"DATE", SQL_DATE}};

  auto it = typeMap.find(iotdbDataType);
  if (it != typeMap.end()) {
    *numericAttribute = it->second;
    static const std::unordered_map<SQLSMALLINT, std::string> typeNameMap = {
        {SQL_BIT, "SQL_BIT"},
        {SQL_INTEGER, "SQL_INTEGER"},
        {SQL_BIGINT, "SQL_BIGINT"},
        {SQL_REAL, "SQL_REAL"},
        {SQL_DOUBLE, "SQL_DOUBLE"},
        {SQL_LONGVARCHAR, "SQL_LONGVARCHAR"},
        {SQL_VARCHAR, "SQL_VARCHAR"},
        {SQL_LONGVARBINARY, "SQL_LONGVARBINARY"},
        {SQL_TIMESTAMP, "SQL_TIMESTAMP"},
        {SQL_DATE, "SQL_DATE"}};

    if (isLogLevelEnabled(cnct, LOG_LEVEL_DEBUG)) {
      std::string typeName = typeNameMap.count(*numericAttribute)
                                 ? typeNameMap.at(*numericAttribute)
                                 : "UNKNOWN_TYPE_CODE";

      std::string logMsg = "colTypeMapping: Column " + std::to_string(columnNumber) +
                           ": IoTDB type '" + iotdbDataType + "' mapped to ODBC type '" + typeName +
                           "'";
      logMessage(cnct, logMsg, LOG_LEVEL_DEBUG);
    }
  } else {
    *numericAttribute = SQL_UNKNOWN_TYPE;

    if (isLogLevelEnabled(cnct, LOG_LEVEL_DEBUG)) {
      std::string logMsg = "colTypeMapping: Column " + std::to_string(columnNumber) +
                           ": Unknown IoTDB type '" + iotdbDataType +
                           "' mapped to SQL_UNKNOWN_TYPE";
      logMessage(cnct, logMsg, LOG_LEVEL_DEBUG);
    }
  }
}

// Abstract function to get column type based on column number
SQLRETURN getColType(StatementHandle* stmt, const SQLUSMALLINT columnNumber,
                     SQLSMALLINT* numericAttribute) {
  ConnectionHandle* cnct = stmt->getConnection();
  logMessage(cnct, "getColType: Entering", LOG_LEVEL_TRACE);

  if (!numericAttribute) {
    logMessage(cnct, "getColType: numericAttribute is null", LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY009", "Invalid use of null pointer");
    return SQL_ERROR;
  }

  if (!stmt->resultSetPtr->getIsMetaData() || stmt->getConnection()->isTableModel) {
    logMessage(cnct, "getColType: Using column types from ODBCResultSet for type mapping",
               LOG_LEVEL_DEBUG);

    // Table model: get type directly from ODBCResultSet column types
    const std::string iotdbDataType = stmt->resultSetPtr->getColumnType(columnNumber - 1);
    if (iotdbDataType.empty()) {
      logMessage(cnct,
                 "getColType: Failed to get data type for column " + std::to_string(columnNumber),
                 LOG_LEVEL_ERROR);
      stmt->addDiagnostic("07009", "Invalid column number");
      return SQL_ERROR;
    }
    colTypeMapping(cnct, iotdbDataType, columnNumber, numericAttribute);

  } else {
    // Metadata mode: determine type by examining actual data from ODBCResultSet
    logMessage(cnct, "getColType: Metadata mode - determining type from data content",
               LOG_LEVEL_DEBUG);

    bool foundFlag = false;
    int numRows = stmt->resultSetPtr->getNumRows();
    for (int curRow = 0; curRow < numRows; curRow++) {
      const ODBCField& value = stmt->resultSetPtr->getValue(curRow, columnNumber - 1);
      if (value.isNull()) {
        continue;
      }

      switch (value.getDataType()) {
      case TSDataType::BOOLEAN:
        *numericAttribute = SQL_BIT;
        break;
      case TSDataType::INT32:
        *numericAttribute = SQL_INTEGER;
        break;
      case TSDataType::INT64:
        *numericAttribute = SQL_BIGINT;
        break;
      case TSDataType::FLOAT:
        *numericAttribute = SQL_REAL;
        break;
      case TSDataType::DOUBLE:
        *numericAttribute = SQL_DOUBLE;
        break;
      case TSDataType::TEXT:
        *numericAttribute = SQL_LONGVARCHAR;
        break;
      case TSDataType::STRING:
        *numericAttribute = SQL_VARCHAR;
        break;
      case TSDataType::DATE:
        *numericAttribute = SQL_VARCHAR;
        break;
      case TSDataType::BLOB:
        *numericAttribute = SQL_LONGVARBINARY;
        break;
      case TSDataType::TIMESTAMP:
        *numericAttribute = SQL_BIGINT;
        break;
      default:
        *numericAttribute = SQL_UNKNOWN_TYPE;
        break;
      }
      foundFlag = true;
    }

    if (!foundFlag) {
      // All rows are empty/null
      *numericAttribute = SQL_UNKNOWN_TYPE; // Default to UNKNOWN_TYPE for null columns
      logMessage(cnct,
                 "getColType: All values empty for column " + std::to_string(columnNumber) +
                     ", defaulting to SQL_VARCHAR",
                 LOG_LEVEL_DEBUG);
    }
  }

  logMessage(cnct, "getColType: Exiting", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN getColumnSize(SQLSMALLINT odbcType, SQLULEN* columnSize) {
  /*
     * Map an ODBC SQL type to its “column size” (display width in characters,
     * not bytes) according to
     * https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/column-size
     */
  if (columnSize == nullptr) {
    return SQL_ERROR;
  }

  switch (odbcType) {
  case SQL_CHAR:
  case SQL_VARCHAR:
  case SQL_LONGVARCHAR:
    *columnSize = 2147483647; // IoTDB places no length limit on strings
    break;

  case SQL_BIT:
    *columnSize = 1;
    break;
  case SQL_TINYINT:
    *columnSize = 3;
    break;
  case SQL_SMALLINT:
    *columnSize = 5;
    break;
  case SQL_INTEGER:
    *columnSize = 10;
    break;
  case SQL_BIGINT:
    *columnSize = 19;
    break;
  case SQL_REAL:
    *columnSize = 7;
    break;
  case SQL_FLOAT:
  case SQL_DOUBLE:
    *columnSize = 15;
    break;

  case SQL_LONGVARBINARY:
    *columnSize = 2147483647;
    break;

  case SQL_DATE:
    *columnSize = 10; // yyyy-mm-dd
    break;
  case SQL_TIMESTAMP:
    *columnSize = 23; // yyyy-mm-dd hh:mm:ss.nnn
    break;

  case SQL_DECIMAL:
  case SQL_NUMERIC:
    // format: [sign][integer digits].[fractional digits]
    // assume max precision 38, scale 10
    *columnSize = 40; // 38 digits + decimal point + optional sign
    break;

  case SQL_TIME:
    *columnSize = 8; // hh:mm:ss
    break;

  case SQL_GUID:
    *columnSize = 36; // canonical form: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

  case SQL_UNKNOWN_TYPE:
    return SQL_ERROR;

  default:
    return SQL_ERROR;
  }

  return SQL_SUCCESS;
}

SQLRETURN getNullable(StatementHandle* stmt, const SQLUSMALLINT columnNumber,
                      SQLSMALLINT* nullable) {
  // Retrieve the connection handle for logging purposes
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;

  // Validate input parameters
  if (nullable == nullptr) {
    logMessage(cnct, "getNullable: Output pointer is null", LOG_LEVEL_ERROR);
    if (stmt)
      stmt->addDiagnostic("HY009", "Invalid use of null pointer");
    return SQL_ERROR;
  }

  if (stmt == nullptr) {
    logMessage(cnct, "getNullable: Statement handle is null", LOG_LEVEL_ERROR);
    return SQL_ERROR;
  }

  if (stmt->resultSetPtr == nullptr || stmt->resultSetPtr->isEmpty()) {
    logMessage(cnct, "getNullable: Result set is null or empty", LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY000", "No query result available");
    return SQL_ERROR;
  }

  // Log debug information
  logMessage(cnct, "getNullable: Checking nullability for column " + std::to_string(columnNumber),
             LOG_LEVEL_DEBUG);

  // The first column (non-metadata timestamp) must be NOT NULL; all others may be NULL
  if ((!stmt->resultSetPtr->getIsMetaData() && columnNumber == 1) ||
      (stmt->getConnection()->isTableModel &&
       stmt->resultSetPtr->getColumnType(columnNumber - 1) == "TIMESTAMP")) {
    *nullable = SQL_NO_NULLS;
    logMessage(cnct,
               "getNullable: Column " + std::to_string(columnNumber) +
                   " is non-nullable (TIMESTAMP)",
               LOG_LEVEL_DEBUG);

    // NOTE: For table models, the REST API does not expose the category of each column
    // to determine whether it is a TIME column; we therefore rely on the data type
    // being TIMESTAMP. However, a table may contain multiple TIMESTAMP columns,
    // only one of which is the true TIME column. Until the REST API is updated,
    // this limitation is hard to resolve cleanly.
    // One potential workaround is to use "DESC <TABLE_NAME>" to retrieve each
    // column's category directly.
  } else {
    *nullable = SQL_NULLABLE;
    logMessage(cnct, "getNullable: Column " + std::to_string(columnNumber) + " is nullable",
               LOG_LEVEL_DEBUG);
  }

  logMessage(cnct, "getNullable: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN getColTypeName(StatementHandle* stmt, const SQLUSMALLINT columnNumber,
                         const SQLPOINTER stringBuffer, const SQLSMALLINT bufferLength,
                         SQLSMALLINT* stringLength) {
  ConnectionHandle* cnct = stmt->getConnection();
  logMessage(cnct, "getColTypeName: Entering", LOG_LEVEL_TRACE);

  if (!stringBuffer) {
    logMessage(cnct, "getColTypeName: stringBuffer is null", LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY009", "Invalid use of null pointer");
    return SQL_ERROR;
  }

  if (!stmt->resultSetPtr->getIsMetaData() || stmt->getConnection()->isTableModel) {
    logMessage(cnct, "getColTypeName: Using column types from ODBCResultSet for type name mapping",
               LOG_LEVEL_DEBUG);

    if (stmt->getConnection()->isTableModel) {
      // Table model: get type name directly from ODBCResultSet column types
      const std::string iotdbDataType = stmt->resultSetPtr->getColumnType(columnNumber - 1);
      if (iotdbDataType.empty()) {
        logMessage(cnct,
                   "getColTypeName: Failed to get data type for column " +
                       std::to_string(columnNumber),
                   LOG_LEVEL_ERROR);
        stmt->addDiagnostic("07009", "Invalid column number");
        return SQL_ERROR;
      }
      SQLRETURN result = setString(iotdbDataType, stringBuffer, bufferLength, stringLength);
      if (result != SQL_SUCCESS) {
        logMessage(
            cnct, "getColTypeName: Failed to set string for column " + std::to_string(columnNumber),
            LOG_LEVEL_ERROR);
        return result;
      }
      logMessage(cnct,
                 "getColTypeName: " + iotdbDataType + " returned for column " +
                     std::to_string(columnNumber),
                 LOG_LEVEL_DEBUG);
    } else {
      // Tree model: not metadata, determine type name from column types
      if (columnNumber == 1) {
        SQLRETURN result = setString("TIMESTAMP", stringBuffer, bufferLength, stringLength);
        if (result != SQL_SUCCESS) {
          logMessage(cnct,
                     "getColTypeName: Failed to set string for column " +
                         std::to_string(columnNumber),
                     LOG_LEVEL_ERROR);
          return result;
        }
        logMessage(cnct,
                   "getColTypeName: TIMESTAMP returned for column " + std::to_string(columnNumber),
                   LOG_LEVEL_DEBUG);
      } else {
        const std::string iotdbDataType = stmt->resultSetPtr->getColumnType(columnNumber - 1);
        if (iotdbDataType.empty()) {
          logMessage(cnct,
                     "getColTypeName: Failed to get data type for column " +
                         std::to_string(columnNumber),
                     LOG_LEVEL_ERROR);
          stmt->addDiagnostic("07009", "Invalid column number");
          return SQL_ERROR;
        }
        SQLRETURN result = setString(iotdbDataType, stringBuffer, bufferLength, stringLength);
        if (result != SQL_SUCCESS) {
          logMessage(cnct,
                     "getColTypeName: Failed to set string for column " +
                         std::to_string(columnNumber),
                     LOG_LEVEL_ERROR);
          return result;
        }
        logMessage(cnct,
                   "getColTypeName: " + iotdbDataType + " returned for column " +
                       std::to_string(columnNumber),
                   LOG_LEVEL_DEBUG);
      }
    }
  } else {
    // Metadata mode: determine type name by examining actual data from ODBCResultSet
    logMessage(cnct, "getColTypeName: Metadata mode - determining type name from data content",
               LOG_LEVEL_DEBUG);

    bool foundFlag = false;
    int numRows = stmt->resultSetPtr->getNumRows();
    for (int curRow = 0; curRow < numRows; curRow++) {
      const ODBCField& value = stmt->resultSetPtr->getValue(curRow, columnNumber - 1);
      if (value.isNull()) {
        continue;
      }
      SQLRETURN result;
      switch (value.getDataType()) {
      case TSDataType::BOOLEAN:
        result = setString("BOOLEAN", stringBuffer, bufferLength, stringLength);
        break;
      case TSDataType::FLOAT:
        result = setString("FLOAT", stringBuffer, bufferLength, stringLength);
        break;
      case TSDataType::DOUBLE:
        result = setString("DOUBLE", stringBuffer, bufferLength, stringLength);
        break;
      case TSDataType::STRING:
        result = setString("STRING", stringBuffer, bufferLength, stringLength);
        break;
      case TSDataType::INT32:
        result = setString("INT32", stringBuffer, bufferLength, stringLength);
        break;
      case TSDataType::INT64:
        result = setString("INT64", stringBuffer, bufferLength, stringLength);
        break;
      case TSDataType::TIMESTAMP:
        result = setString("TIMESTAMP", stringBuffer, bufferLength, stringLength);
        break;
      case TSDataType::DATE:
        result = setString("DATE", stringBuffer, bufferLength, stringLength);
        break;
      case TSDataType::BLOB:
        result = setString("BLOB", stringBuffer, bufferLength, stringLength);
        break;
      default:
        result = setString("", stringBuffer, bufferLength, stringLength);
        break;
      }
      foundFlag = true;
    }

    if (!foundFlag) {
      // All rows are empty/null
      SQLRETURN result = setString("", stringBuffer, bufferLength, stringLength);
      if (result != SQL_SUCCESS) {
        logMessage(cnct,
                   "getColTypeName: Failed to set empty string for null column " +
                       std::to_string(columnNumber),
                   LOG_LEVEL_ERROR);
        return result;
      }
      logMessage(cnct,
                 "getColTypeName: Empty string returned for null column " +
                     std::to_string(columnNumber),
                 LOG_LEVEL_DEBUG);
    }
  }

  logMessage(cnct, "getColTypeName: Exiting", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN getPrecision(SQLSMALLINT odbcType, SQLULEN* precision) {
  if (precision == nullptr) {
    logMessage("getPrecision: precision pointer is nullptr");
    return SQL_SUCCESS;
  }

  switch (odbcType) {
  case SQL_BIT:     // BOOLEAN
    *precision = 1; // Boolean value fixed to 1 bit
    break;

  case SQL_INTEGER:  // INT32
    *precision = 10; // 32-bit integer maximum 10 digits (2,147,483,647)
    break;

  case SQL_BIGINT:   // INT64
    *precision = 19; // 64-bit integer maximum 19 digits (9,223,372,036,854,775,807)
    break;

  case SQL_REAL:     // FLOAT
    *precision = 23; // IEEE 754 single precision has 23 bits of binary precision
    break;

  case SQL_DOUBLE:   // DOUBLE
    *precision = 52; // IEEE 754 double precision has 52 bits of binary precision
    break;

  case SQL_VARCHAR:
  case SQL_LONGVARCHAR:
  case SQL_LONGVARBINARY:
  case SQL_TIMESTAMP:
  case SQL_DATE:
    *precision = 0;
    break;

  default:
    *precision = 0; // Unknown type defaults to 0
    break;
  }
  return SQL_SUCCESS;
}

SQLRETURN getPrecRadix(SQLSMALLINT odbcType, SQLULEN* radix) {
  if (radix == nullptr) {
    logMessage("getPrecRadix: precision pointer is nullptr");
    return SQL_SUCCESS;
  }

  switch (odbcType) {
  case SQL_REAL:
  case SQL_DOUBLE:
  case SQL_BIT: // BOOLEAN
    *radix = 2; // Boolean value fixed to 1 bit
    break;
  case SQL_INTEGER:
  case SQL_BIGINT: // INT64
    *radix = 10;   // 64-bit integer maximum 19 digits (9,223,372,036,854,775,807)
    break;
  case SQL_VARCHAR:
  case SQL_LONGVARCHAR:
  case SQL_LONGVARBINARY:
  case SQL_TIMESTAMP:
  case SQL_DATE:
    *radix = 0;
    break;

  default:
    *radix = 0; // Unknown type defaults to 0
    break;
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColAttribute(const SQLHSTMT statementHandle, const SQLUSMALLINT columnNumber,
                                  const SQLUSMALLINT fieldIdentifier,
                                  const SQLPOINTER characterAttribute,
                                  const SQLSMALLINT bufferLength, SQLSMALLINT* stringLength,
                                  SQLLEN* numericAttribute) {
  // Get connection handle for logging
  StatementHandle* stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;

  logMessage(cnct, "SQLColAttribute: Entering", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLColAttribute parameters: "
              << "StatementHandle = " << handleToString(statementHandle)
              << ", ColumnNumber = " << columnNumber << ", FieldIdentifier = " << fieldIdentifier
              << ", CharacterAttribute = " << characterAttribute
              << ", BufferLength = " << bufferLength;

    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  if (statementHandle == nullptr) {
    logMessage(cnct, "SQLColAttribute: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  if (stmt->resultSetPtr == nullptr || stmt->resultSetPtr->isEmpty()) {
    logMessage(cnct, "SQLColAttribute: Missing result set", LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY000", "No query result available");
    return SQL_ERROR;
  }

  // Validate column number
  if (columnNumber < 1 || columnNumber > stmt->resultSetPtr->getNumColumns()) {
    logMessage(cnct,
               "SQLColAttribute: Invalid column number: " + std::to_string(columnNumber) +
                   ", max columns: " + std::to_string(stmt->resultSetPtr->getNumColumns()),
               LOG_LEVEL_ERROR);
    stmt->addDiagnostic("07009", "Invalid column number");
    return SQL_ERROR;
  }

  // Initialize output parameters
  if (characterAttribute) {
    memset(characterAttribute, 0, bufferLength);
  }
  if (stringLength) {
    *stringLength = 0;
  }
  if (numericAttribute) {
    *numericAttribute = 0;
  }

  // Handle based on fieldIdentifier
  SQLRETURN sqlreturn;
  SQLSMALLINT columnType;
  SQLLEN octetLength;
  SQLLEN scale;
  switch (fieldIdentifier) {
  case SQL_DESC_LABEL:
    logMessage(cnct, "SQLColAttribute: Processing SQL_DESC_LABEL", LOG_LEVEL_DEBUG);
    if (characterAttribute) {
      // Get column name as label
      sqlreturn = getColName(stmt, columnNumber, characterAttribute, bufferLength, stringLength);
      if (sqlreturn != SQL_SUCCESS) {
        logMessage(cnct, "SQLColAttribute: Failed to get column name for label", LOG_LEVEL_ERROR);
        return sqlreturn;
      }
    }
    break;

  case SQL_DESC_COUNT:
    logMessage(cnct, "SQLColAttribute: Processing SQL_DESC_COUNT", LOG_LEVEL_DEBUG);
    {
      int columnCount = 0;
      if ((!stmt->getConnection()->isTableModel) && (!stmt->resultSetPtr->getIsMetaData())) {
        // Tree model with time series
        columnCount = stmt->resultSetPtr->getNumColumns() + 1; // +1 for timestamp column
      } else {
        // Table model or metadata
        columnCount = stmt->resultSetPtr->getNumColumns();
      }

      if (numericAttribute) {
        *numericAttribute = static_cast<SQLLEN>(columnCount);
      }
      logMessage(cnct, "SQLColAttribute: Column count = " + std::to_string(columnCount),
                 LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_DESC_NAME:
    logMessage(cnct, "SQLColAttribute: Processing SQL_DESC_NAME", LOG_LEVEL_DEBUG);
    sqlreturn = getColName(stmt, columnNumber, characterAttribute, bufferLength, stringLength);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column name", LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    break;

  case SQL_COLUMN_TYPE:
  case SQL_DESC_TYPE:
    logMessage(cnct, "SQLColAttribute: Processing SQL_COLUMN_TYPE/SQL_DESC_TYPE", LOG_LEVEL_DEBUG);
    if (!numericAttribute) {
      logMessage(cnct, "SQLColAttribute: numericAttribute is null for type request",
                 LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY009", "Invalid use of null pointer");
      return SQL_ERROR;
    }
    sqlreturn = getColType(stmt, columnNumber, reinterpret_cast<SQLSMALLINT*>(numericAttribute));
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type", LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    break;

  case SQL_COLUMN_DISPLAY_SIZE:
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_COLUMN_DISPLAY_SIZE for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);
    {
      // First get the column type
      columnType = SQL_UNKNOWN_TYPE;
      sqlreturn = getColType(stmt, columnNumber, &columnType);
      if (sqlreturn != SQL_SUCCESS) {
        logMessage(cnct, "SQLColAttribute: Failed to get column type for display size",
                   LOG_LEVEL_ERROR);
        return sqlreturn;
      }

      SQLLEN displaySize = 0;
      switch (columnType) {
      case SQL_CHAR:
      case SQL_VARCHAR:
      case SQL_LONGVARCHAR:
        // For string types, use the actual length if available, otherwise default
        displaySize = 255; // Default for string types
        break;

      case SQL_DECIMAL:
      case SQL_NUMERIC:
        displaySize = 20; // Reasonable default for decimal types
        break;

      case SQL_INTEGER:
        displaySize = 11; // Max display size for a signed 32-bit integer (-2147483648)
        break;

      case SQL_SMALLINT:
        displaySize = 6; // Max display size for a signed 16-bit integer (-32768)
        break;

      case SQL_BIGINT:
        displaySize = 20; // Max display size for a signed 64-bit integer
        break;

      case SQL_FLOAT:
      case SQL_DOUBLE:
      case SQL_REAL:
        displaySize = 24; // Typical display size for floating-point numbers
        break;

      case SQL_BIT:
        displaySize = 1; // 0 or 1
        break;

      case SQL_TYPE_DATE:
        displaySize = 10; // YYYY-MM-DD
        break;

      case SQL_TYPE_TIME:
        displaySize = 8; // HH:MM:SS
        break;

      case SQL_TYPE_TIMESTAMP:
        displaySize = 23; // YYYY-MM-DD HH:MM:SS.sss
        break;

      default:
        displaySize = 0; // Unknown type
        logMessage(cnct,
                   "SQLColAttribute: Unknown column type for display size: " +
                       std::to_string(columnType),
                   LOG_LEVEL_WARN);
        break;
      }

      if (numericAttribute) {
        *numericAttribute = displaySize;
        logMessage(cnct, "SQLColAttribute: Display size set to " + std::to_string(displaySize),
                   LOG_LEVEL_DEBUG);
      }

      if (characterAttribute && bufferLength > 0) {
        const std::string displaySizeStr = std::to_string(displaySize);
        const size_t copyLen =
            std::min(static_cast<size_t>(bufferLength - 1), displaySizeStr.length());
        std::memcpy(characterAttribute, displaySizeStr.c_str(), copyLen);
        static_cast<char*>(characterAttribute)[copyLen] = '\0';

        if (stringLength) {
          *stringLength = static_cast<SQLSMALLINT>(displaySizeStr.length());
        }
      }
    }
    break;

  case SQL_COLUMN_UNSIGNED: {
    // FieldIdentifier == 8
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_COLUMN_UNSIGNED for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);

    // Default: Column is signed unless explicitly determined otherwise
    SQLLEN isUnsigned = SQL_DESC_UNSIGNED;

    columnType = SQL_UNKNOWN_TYPE;
    sqlreturn = getColType(stmt, columnNumber, &columnType);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type for unsigned check",
                 LOG_LEVEL_ERROR);
      return sqlreturn;
    }

    switch (columnType) {
    case SQL_BIT: // BIT type is effectively unsigned
      isUnsigned = SQL_TRUE;
      break;

    default:
      isUnsigned = SQL_FALSE; // Most types are signed by default
      break;
    }

    if (numericAttribute) {
      *numericAttribute = isUnsigned;
      logMessage(cnct, "SQLColAttribute: Unsigned attribute set to " + std::to_string(isUnsigned),
                 LOG_LEVEL_DEBUG);
    }

    if (characterAttribute && bufferLength > 0) {
      // Return a textual representation
      const char* unsignedStr = (isUnsigned == SQL_TRUE) ? "TRUE" : "FALSE";
      const size_t copyLen =
          std::min(static_cast<size_t>(bufferLength - 1), std::strlen(unsignedStr));
      std::memcpy(characterAttribute, unsignedStr, copyLen);
      static_cast<char*>(characterAttribute)[copyLen] = '\0';

      if (stringLength) {
        *stringLength = static_cast<SQLSMALLINT>(std::strlen(unsignedStr));
      }
    }
    break;
  }

  case SQL_DESC_OCTET_LENGTH: // Byte length
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_DESC_OCTET_LENGTH for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);

    // Get the column type to determine octet length
    columnType = SQL_UNKNOWN_TYPE;
    sqlreturn = getColType(stmt, columnNumber, &columnType);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type for octet length",
                 LOG_LEVEL_ERROR);
      return sqlreturn;
    }

    octetLength = 0;
    switch (columnType) {
    case SQL_BIT:
      octetLength = 1;
      break;
    case SQL_TINYINT:
      octetLength = 1;
      break;
    case SQL_SMALLINT:
      octetLength = 2;
      break;
    case SQL_INTEGER:
      octetLength = 4;
      break;
    case SQL_BIGINT:
      octetLength = 8;
      break;
    case SQL_REAL:
      octetLength = 4;
      break;
    case SQL_FLOAT:
    case SQL_DOUBLE:
      octetLength = 8;
      break;
    case SQL_TYPE_DATE:
      octetLength = 6; // Size of DATE_STRUCT
      break;
    case SQL_TYPE_TIME:
      octetLength = 6; // Size of TIME_STRUCT
      break;
    case SQL_TYPE_TIMESTAMP:
      octetLength = 16; // Size of TIMESTAMP_STRUCT
      break;
    default:
      // For string types, use a reasonable default or calculate based on data
      octetLength = 255;
      break;
    }

    if (numericAttribute) {
      *numericAttribute = octetLength;
    }
    break;

  case SQL_DESC_PRECISION: // Precision
  {
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_DESC_PRECISION for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);
    columnType = SQL_UNKNOWN_TYPE;
    sqlreturn = getColType(stmt, columnNumber, &columnType);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type for precision", LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    SQLULEN value;
    sqlreturn = getPrecision(columnType, &value);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get precision", LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    if (numericAttribute) {
      *numericAttribute = value;
    }
    break;
  }

  case SQL_DESC_SCALE: // Scale
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_DESC_SCALE for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);

    // Get the column type to determine scale
    columnType = SQL_UNKNOWN_TYPE;
    sqlreturn = getColType(stmt, columnNumber, &columnType);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type for scale", LOG_LEVEL_ERROR);
      return sqlreturn;
    }

    scale = 0;
    switch (columnType) {
    case SQL_DECIMAL:
    case SQL_NUMERIC:
      // For decimal types, you might need to get the actual scale from metadata
      scale = 0; // Default scale for decimal types
      break;
    default:
      // For other types, scale is not applicable
      scale = 0;
      break;
    }

    if (numericAttribute) {
      *numericAttribute = scale;
    }
    break;

  case SQL_DESC_TYPE_NAME:
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_DESC_TYPE_NAME for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);
    sqlreturn = getColTypeName(stmt, columnNumber, characterAttribute, bufferLength, stringLength);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type name", LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    break;

  case SQL_DESC_NULLABLE: {
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_DESC_NULLABLE for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);
    SQLSMALLINT value = 0;
    sqlreturn = getNullable(stmt, columnNumber, &value);
    if (numericAttribute) {
      *numericAttribute = value;
    }
    break;
  }

  case SQL_DESC_LENGTH: {
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_DESC_LENGTH for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);
    SQLSMALLINT type = SQL_UNKNOWN_TYPE;
    sqlreturn = getColType(stmt, columnNumber, &type);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type for length", LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    SQLULEN value;
    sqlreturn = getColumnSize(type, &value);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get length", LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    if (numericAttribute) {
      *numericAttribute = value;
    }
    break;
  }

  case SQL_DESC_NUM_PREC_RADIX: {
    logMessage(cnct,
               "SQLColAttribute: Processing SQL_DESC_NUM_PREC_RADIX for column " +
                   std::to_string(columnNumber),
               LOG_LEVEL_DEBUG);
    SQLSMALLINT type = SQL_UNKNOWN_TYPE;
    sqlreturn = getColType(stmt, columnNumber, &type);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type for precision radix",
                 LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    SQLULEN value;
    sqlreturn = getPrecRadix(type, &value);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLColAttribute: Failed to get column type for precision radix",
                 LOG_LEVEL_ERROR);
      return sqlreturn;
    }
    if (numericAttribute) {
      *numericAttribute = value;
    }
    break;
  }

  default:
    logMessage(cnct,
               "SQLColAttribute: Unsupported field identifier: " + std::to_string(fieldIdentifier),
               LOG_LEVEL_WARN);
    stmt->addDiagnostic("HY000", "Unsupported field identifier");
    return SQL_ERROR;
  }

  logMessage(cnct, "SQLColAttribute: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColumns(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                             SQLSMALLINT nameLength1, SQLCHAR* schemaName, SQLSMALLINT nameLength2,
                             SQLCHAR* tableName, SQLSMALLINT nameLength3, SQLCHAR* columnName,
                             SQLSMALLINT nameLength4) {
  /*
    Currently only supports table model.
    In tree model case, this function is incomplete: it will treat the four strings as NULL directly.
    */
  if (statementHandle == nullptr) {
    logMessage("Invalid statementHandle. returning.\n");
    return SQL_INVALID_HANDLE;
  }
  StatementHandle* stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt->getConnection();
  logMessage(cnct, "Entering SQLColumns", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "Parameters: "
              << "statementHandle = " << statementHandle
              << ", catalogName = " << (catalogName ? (const char*)catalogName : "nullptr")
              << ", nameLength1 = " << nameLength1
              << ", schemaName = " << (schemaName ? (const char*)schemaName : "nullptr")
              << ", nameLength2 = " << nameLength2
              << ", tableName = " << (tableName ? (const char*)tableName : "nullptr")
              << ", nameLength3 = " << nameLength3
              << ", columnName = " << (columnName ? (const char*)columnName : "nullptr")
              << ", nameLength4 = " << nameLength4;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  if (catalogName == nullptr)
    nameLength1 = 0;
  if (schemaName == nullptr)
    nameLength2 = 0;
  if (tableName == nullptr)
    nameLength3 = 0;
  if (columnName == nullptr)
    nameLength4 = 0;

  if ((catalogName && nameLength1 < 0 && nameLength1 != SQL_NTS)) {
    logMessage(cnct, "Invalid nameLength1 (" + std::to_string(nameLength1) + ") for catalogName.",
               LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY090", "Invalid string or buffer length");
    return SQL_ERROR;
  }
  if ((schemaName && nameLength2 < 0 && nameLength2 != SQL_NTS)) {
    logMessage(cnct, "Invalid nameLength2 (" + std::to_string(nameLength2) + ") for schemaName.",
               LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY090", "Invalid string or buffer length");
    return SQL_ERROR;
  }
  if ((tableName && nameLength3 < 0 && nameLength3 != SQL_NTS)) {
    logMessage(cnct, "Invalid nameLength3 (" + std::to_string(nameLength3) + ") for tableName.",
               LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY090", "Invalid string or buffer length");
    return SQL_ERROR;
  }
  if ((columnName && nameLength4 < 0 && nameLength4 != SQL_NTS)) {
    logMessage(cnct, "Invalid nameLength4 (" + std::to_string(nameLength4) + ") for columnName.",
               LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY090", "Invalid string or buffer length");
    return SQL_ERROR;
  }

  if (catalogName && nameLength1 == SQL_NTS)
    nameLength1 = (SQLSMALLINT)strlen((const char*)catalogName);
  if (schemaName && nameLength2 == SQL_NTS)
    nameLength2 = (SQLSMALLINT)strlen((const char*)schemaName);
  if (tableName && nameLength3 == SQL_NTS)
    nameLength3 = (SQLSMALLINT)strlen((const char*)tableName);
  if (columnName && nameLength4 == SQL_NTS)
    nameLength4 = (SQLSMALLINT)strlen((const char*)columnName);

  if (cnct->isTableModel) {

    std::string sqlCommand =
        "SELECT "
        "database AS TABLE_CAT, "
        "'' AS TABLE_SCHEM, "
        "table_name AS TABLE_NAME, "
        "column_name AS COLUMN_NAME, "
        "CASE "
        "WHEN datatype = 'BOOLEAN' THEN 16 "
        "WHEN datatype = 'INT32' THEN 4 "
        "WHEN datatype = 'INT64' THEN -5 "
        "WHEN datatype = 'FLOAT' THEN 7 "
        "WHEN datatype = 'DOUBLE' THEN 8 "
        "WHEN datatype = 'TEXT' THEN -1 "
        "WHEN datatype = 'STRING' THEN 12 "
        "WHEN datatype = 'BLOB' THEN -4 "
        "WHEN datatype = 'TIMESTAMP' THEN -5 "
        "WHEN datatype = 'DATE' THEN 91 "
        "ELSE 0 "
        "END AS DATA_TYPE, "
        "datatype AS TYPE_NAME, "
        "CASE "
        "WHEN datatype = 'BOOLEAN' THEN 1 "
        "WHEN datatype = 'INT32' THEN 10 "
        "WHEN datatype = 'INT64' THEN 19 "
        "WHEN datatype = 'FLOAT' THEN 24 "
        "WHEN datatype = 'DOUBLE' THEN 53 "
        "WHEN datatype = 'TEXT' THEN 0 "
        "WHEN datatype = 'STRING' THEN 0 "
        "WHEN datatype = 'BLOB' THEN 0 "
        "WHEN datatype = 'TIMESTAMP' THEN 23 "
        "WHEN datatype = 'DATE' THEN 10 "
        "ELSE 0 "
        "END AS COLUMN_SIZE, "
        "CASE "
        "WHEN datatype = 'BOOLEAN' THEN 1 "
        "WHEN datatype = 'INT32' THEN 4 "
        "WHEN datatype = 'INT64' THEN 8 "
        "WHEN datatype = 'FLOAT' THEN 4 "
        "WHEN datatype = 'DOUBLE' THEN 8 "
        "WHEN datatype = 'TEXT' THEN 0 "
        "WHEN datatype = 'STRING' THEN 0 "
        "WHEN datatype = 'BLOB' THEN 0 "
        "WHEN datatype = 'TIMESTAMP' THEN 16 "
        "WHEN datatype = 'DATE' THEN 6 "
        "ELSE 0 "
        "END AS BUFFER_LENGTH, "
        "CASE WHEN datatype = 'TIMESTAMP' THEN 3 ELSE 0 END AS DECIMAL_DIGITS, "
        "CASE "
        "WHEN datatype IN ('FLOAT', 'DOUBLE') THEN 2 "
        "WHEN datatype IN ('INT32', 'INT64') THEN 10 "
        "END AS NUM_PREC_RADIX, "
        "CASE "
        "WHEN category = 'TIME' THEN 0 "
        "ELSE 1 "
        "END AS NULLABLE, "
        "comment AS REMARKS, "
        "CASE WHEN 1=0 THEN 0 END AS COLUMN_DEF, "
        "CASE "
        "WHEN datatype = 'BOOLEAN' THEN -7 "
        "WHEN datatype = 'INT32' THEN 4 "
        "WHEN datatype = 'INT64' THEN -5 "
        "WHEN datatype = 'FLOAT' THEN 7 "
        "WHEN datatype = 'DOUBLE' THEN 8 "
        "WHEN datatype = 'TEXT' THEN -1 "
        "WHEN datatype = 'STRING' THEN 12 "
        "WHEN datatype = 'BLOB' THEN -4 "
        "WHEN datatype = 'TIMESTAMP' THEN 9 "
        "WHEN datatype = 'DATE' THEN 9 "
        "ELSE 0 "
        "END AS SQL_DATA_TYPE, "
        "CASE "
        "WHEN datatype = 'DATE' THEN 1 "
        "WHEN datatype = 'TIMESTAMP' THEN 3 "
        "END AS SQL_DATETIME_SUB, "
        "CASE "
        "WHEN datatype IN ('TEXT', 'STRING', 'BLOB') THEN 0 "
        "END AS CHAR_OCTET_LENGTH, "
        "0 AS ORDINAL_POSITION, "
        "CASE WHEN category = 'TIME' THEN 'NO' ELSE 'YES' END AS IS_NULLABLE "
        "FROM information_schema.columns WHERE ";

    bool shouldAddAnd = false;
    if (nameLength1 > 0) {
      sqlCommand += std::string("database LIKE '") +
                    std::string(reinterpret_cast<const char*>(catalogName), nameLength1) +
                    std::string("' ");
      shouldAddAnd = true;
    }
    if (nameLength3 > 0) {
      if (shouldAddAnd) {
        sqlCommand += std::string("AND ");
      }
      sqlCommand += std::string("table_name LIKE '") +
                    std::string(reinterpret_cast<const char*>(tableName), nameLength3) +
                    std::string("' ");
      shouldAddAnd = true;
    }
    if (nameLength4 > 0) {
      if (shouldAddAnd) {
        sqlCommand += std::string("AND ");
      }
      sqlCommand += std::string("column_name LIKE '") +
                    std::string(reinterpret_cast<const char*>(columnName), nameLength4) +
                    std::string("' ");
      shouldAddAnd = true;
    }

    SQLRETURN ret = IoTDB_ExecDirect(stmt, sqlCommand);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      logMessage(cnct, "SQLColumns: Query completed unsuccessfully.  Exiting.\n", LOG_LEVEL_ERROR);
      return ret;
    }

    if (stmt->resultSetPtr == nullptr || stmt->resultSetPtr->isEmpty()) {
      logMessage(cnct, "SQLColumns: Missing query response. Exiting.\n", LOG_LEVEL_ERROR);
      return SQL_ERROR;
    }

    // Get only the actually used column indices
    int decimalDigitsIdx = stmt->resultSetPtr->findColumnIndex("DECIMAL_DIGITS");
    int typeNameIdx = stmt->resultSetPtr->findColumnIndex("TYPE_NAME");
    int ordinalPositionIdx = stmt->resultSetPtr->findColumnIndex("ORDINAL_POSITION");

    // Check if required columns are found
    if (decimalDigitsIdx == -1 || typeNameIdx == -1 || ordinalPositionIdx == -1) {
      logMessage(cnct, "SQLColumns: Required columns not found in result set. Exiting.\n",
                 LOG_LEVEL_ERROR);
      return SQL_ERROR;
    }

    // Process each row of data
    int numRows = stmt->resultSetPtr->getNumRows();
    for (int rowIdx = 0; rowIdx < numRows; ++rowIdx) {
      // ORDINAL_POSITION: Set incremental position
      stmt->resultSetPtr->setValue(rowIdx, ordinalPositionIdx, ODBCField(rowIdx + 1));

      // DECIMAL_DIGITS: Adjust based on data type
      std::string typeName = stmt->resultSetPtr->getValue(rowIdx, typeNameIdx).toString();
      if (typeName == "TIMESTAMP") {
        stmt->resultSetPtr->setValue(rowIdx, decimalDigitsIdx,
                                     ODBCField(3)); // Timestamp decimal digits (microseconds)
      } else {
        stmt->resultSetPtr->setValue(rowIdx, decimalDigitsIdx,
                                     ODBCField::null()); // Set other types to null
      }
    }

    logMessage(cnct, "Query completed successfully. Exiting SQLColumns.\n", LOG_LEVEL_TRACE);
    return ret;
  } else {
    logMessage(cnct, "SQLColumns: Handling tree model", LOG_LEVEL_TRACE);
    std::string catalogStr =
        catalogName ? std::string(reinterpret_cast<const char*>(catalogName), nameLength1) : "";
    std::string tableStr =
        tableName ? std::string(reinterpret_cast<const char*>(tableName), nameLength3) : "";
    std::string columnStr =
        columnName ? std::string(reinterpret_cast<const char*>(columnName), nameLength4) : "";

    std::string sqlCommand = "SHOW TIMESERIES ";
    bool catalogEmpty = catalogStr.empty() || catalogStr == "%" || catalogStr == SQL_ALL_CATALOGS;
    bool tableEmpty = tableStr.empty() || tableStr == "%";
    bool columnEmpty = columnStr.empty() || columnStr == "%";

    if (!catalogEmpty && tableEmpty && columnEmpty) {
      sqlCommand += catalogStr + ".**";
    } else if (!catalogEmpty && !tableEmpty && columnEmpty) {
      sqlCommand += catalogStr + "." + tableStr + ".**";
    } else if (!catalogEmpty && !tableEmpty && !columnEmpty) {
      sqlCommand += catalogStr + "." + tableStr + "." + columnStr;
    } else {
      // do nothing
    }

    logMessage(cnct, "SQLColumns: Executing query: " + sqlCommand, LOG_LEVEL_DEBUG);
    SQLRETURN ret = SQLExecDirect(statementHandle, (SQLCHAR*)sqlCommand.data(), SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      logMessage(cnct, "SQLColumns: Query completed unsuccessfully", LOG_LEVEL_ERROR);
      return ret;
    }

    // Transform SHOW TIMESERIES result to SQLColumns format (same as information_schema.columns)
    if (stmt->resultSetPtr && !stmt->resultSetPtr->isEmpty()) {
      int idxTimeseries = stmt->resultSetPtr->findColumnIndex("Timeseries");
      int idxDatabase = stmt->resultSetPtr->findColumnIndex("Database");
      int idxDataType = stmt->resultSetPtr->findColumnIndex("DataType");
      if (idxTimeseries >= 0 && idxDatabase >= 0 && idxDataType >= 0) {
        const int numRows = stmt->resultSetPtr->getNumRows();
        std::vector<std::string> savedTimeseries(static_cast<size_t>(numRows));
        std::vector<std::string> savedDatabase(static_cast<size_t>(numRows));
        std::vector<std::string> savedDataType(static_cast<size_t>(numRows));
        for (int r = 0; r < numRows; ++r) {
          savedTimeseries[r] = stmt->resultSetPtr->getValue(r, idxTimeseries).toString();
          savedDatabase[r] = stmt->resultSetPtr->getValue(r, idxDatabase).toString();
          savedDataType[r] = stmt->resultSetPtr->getValue(r, idxDataType).toString();
        }

        static const std::vector<std::string> sqlColNames = {
            "TABLE_CAT",        "TABLE_SCHEM",    "TABLE_NAME",       "COLUMN_NAME",
            "DATA_TYPE",        "TYPE_NAME",      "COLUMN_SIZE",      "BUFFER_LENGTH",
            "DECIMAL_DIGITS",   "NUM_PREC_RADIX", "NULLABLE",         "REMARKS",
            "COLUMN_DEF",       "SQL_DATA_TYPE",  "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH",
            "ORDINAL_POSITION", "IS_NULLABLE"};
        static const std::vector<std::string> sqlColTypes = {
            "STRING", "STRING", "STRING", "STRING", "INT32", "STRING", "INT32", "INT32", "INT32",
            "INT32",  "INT32",  "STRING", "INT32",  "INT32", "INT32",  "INT32", "INT32", "STRING"};

        stmt->resultSetPtr->columnNames = sqlColNames;
        stmt->resultSetPtr->columnTypes = sqlColTypes;
        stmt->resultSetPtr->numColumns = 18;
        stmt->resultSetPtr->numRows = numRows;

        // Parse first row's TABLE_CAT/TABLE_NAME for the Time row (when numRows > 0)
        std::string firstTableCat;
        std::string firstTableName;
        if (numRows > 0) {
          const std::string& ts0 = savedTimeseries[0];
          const std::string& db0 = savedDatabase[0];
          firstTableCat = db0;
          std::string tableNamePart;
          if (!db0.empty() && ts0.size() > db0.size() && ts0.compare(0, db0.size(), db0) == 0 &&
              ts0[db0.size()] == '.') {
            tableNamePart = ts0.substr(db0.size() + 1);
          } else {
            tableNamePart = ts0;
          }
          size_t lastDot = tableNamePart.rfind('.');
          firstTableName = (lastDot != std::string::npos) ? tableNamePart.substr(0, lastDot) : "";
        }

        auto dataTypeToOdbc = [](const std::string& dt) -> std::pair<int, int> {
          if (dt == "BOOLEAN")
            return {16, -7};
          if (dt == "INT32")
            return {4, 4};
          if (dt == "INT64")
            return {-5, -5};
          if (dt == "FLOAT")
            return {7, 7};
          if (dt == "DOUBLE")
            return {8, 8};
          if (dt == "TEXT")
            return {-1, -1};
          if (dt == "STRING")
            return {12, 12};
          if (dt == "BLOB")
            return {-4, -4};
          if (dt == "TIMESTAMP")
            return {-5, 9};
          if (dt == "DATE")
            return {91, 9};
          return {0, 0};
        };
        auto columnSize = [](const std::string& dt) -> int {
          if (dt == "BOOLEAN")
            return 1;
          if (dt == "INT32")
            return 10;
          if (dt == "INT64")
            return 19;
          if (dt == "FLOAT")
            return 24;
          if (dt == "DOUBLE")
            return 53;
          if (dt == "TEXT" || dt == "STRING" || dt == "BLOB")
            return 0;
          if (dt == "TIMESTAMP")
            return 23;
          if (dt == "DATE")
            return 10;
          return 0;
        };
        auto bufferLength = [](const std::string& dt) -> int {
          if (dt == "BOOLEAN")
            return 1;
          if (dt == "INT32")
            return 4;
          if (dt == "INT64")
            return 8;
          if (dt == "FLOAT")
            return 4;
          if (dt == "DOUBLE")
            return 8;
          if (dt == "TEXT" || dt == "STRING" || dt == "BLOB")
            return 0;
          if (dt == "TIMESTAMP")
            return 16;
          if (dt == "DATE")
            return 6;
          return 0;
        };

        for (int rowIdx = 0; rowIdx < numRows; ++rowIdx) {
          const std::string& timeseries = savedTimeseries[rowIdx];
          const std::string& database = savedDatabase[rowIdx];
          const std::string& dataType = savedDataType[rowIdx];

          std::string tableNamePart;
          std::string columnNamePart;
          if (!database.empty() && timeseries.size() > database.size() &&
              timeseries.compare(0, database.size(), database) == 0 &&
              timeseries[database.size()] == '.') {
            tableNamePart = timeseries.substr(database.size() + 1);
          } else {
            tableNamePart = timeseries;
          }
          size_t lastDot = tableNamePart.rfind('.');
          if (lastDot != std::string::npos) {
            columnNamePart = tableNamePart.substr(lastDot + 1);
            tableNamePart = tableNamePart.substr(0, lastDot);
          } else {
            columnNamePart = tableNamePart;
            tableNamePart = "";
          }

          auto odbcTypes = dataTypeToOdbc(dataType);
          int colSize = columnSize(dataType);
          int bufLen = bufferLength(dataType);

          std::vector<ODBCField> row;
          row.push_back(ODBCField(database));        // TABLE_CAT
          row.push_back(ODBCField(std::string(""))); // TABLE_SCHEM
          row.push_back(ODBCField(tableNamePart));   // TABLE_NAME
          row.push_back(ODBCField(columnNamePart));  // COLUMN_NAME
          row.push_back(ODBCField(odbcTypes.first)); // DATA_TYPE
          row.push_back(ODBCField(dataType));        // TYPE_NAME
          row.push_back(ODBCField(colSize));         // COLUMN_SIZE
          row.push_back(ODBCField(bufLen));          // BUFFER_LENGTH
          if (dataType == "TIMESTAMP") {
            row.push_back(ODBCField(3)); // DECIMAL_DIGITS
          } else {
            row.push_back(ODBCField::null()); // DECIMAL_DIGITS
          }
          if (dataType == "FLOAT" || dataType == "DOUBLE") {
            row.push_back(ODBCField(2)); // NUM_PREC_RADIX
          } else if (dataType == "INT32" || dataType == "INT64") {
            row.push_back(ODBCField(10)); // NUM_PREC_RADIX
          } else {
            row.push_back(ODBCField::null()); // NUM_PREC_RADIX
          }
          row.push_back(ODBCField(1));                // NULLABLE
          row.push_back(ODBCField(std::string("")));  // REMARKS
          row.push_back(ODBCField::null());           // COLUMN_DEF
          row.push_back(ODBCField(odbcTypes.second)); // SQL_DATA_TYPE
          if (dataType == "DATE") {
            row.push_back(ODBCField(1)); // SQL_DATETIME_SUB
          } else if (dataType == "TIMESTAMP") {
            row.push_back(ODBCField(3)); // SQL_DATETIME_SUB
          } else {
            row.push_back(ODBCField::null()); // SQL_DATETIME_SUB
          }
          if (dataType == "TEXT" || dataType == "STRING" || dataType == "BLOB") {
            row.push_back(ODBCField(0)); // CHAR_OCTET_LENGTH
          } else {
            row.push_back(ODBCField::null()); // CHAR_OCTET_LENGTH
          }
          row.push_back(ODBCField(rowIdx + 1));         // ORDINAL_POSITION
          row.push_back(ODBCField(std::string("YES"))); // IS_NULLABLE

          stmt->resultSetPtr->data[rowIdx] = std::move(row);
        }

        // Prepend one row for the Time column (INT64)
        std::vector<ODBCField> timeRow;
        timeRow.push_back(ODBCField(firstTableCat));        // TABLE_CAT
        timeRow.push_back(ODBCField(std::string("")));      // TABLE_SCHEM
        timeRow.push_back(ODBCField(firstTableName));       // TABLE_NAME
        timeRow.push_back(ODBCField(std::string("Time")));  // COLUMN_NAME
        timeRow.push_back(ODBCField(-5));                   // DATA_TYPE (INT64)
        timeRow.push_back(ODBCField(std::string("INT64"))); // TYPE_NAME
        timeRow.push_back(ODBCField(19));                   // COLUMN_SIZE
        timeRow.push_back(ODBCField(8));                    // BUFFER_LENGTH
        timeRow.push_back(ODBCField::null());               // DECIMAL_DIGITS
        timeRow.push_back(ODBCField(10));                   // NUM_PREC_RADIX
        timeRow.push_back(ODBCField(1));                    // NULLABLE
        timeRow.push_back(ODBCField(std::string("")));      // REMARKS
        timeRow.push_back(ODBCField::null());               // COLUMN_DEF
        timeRow.push_back(ODBCField(-5));                   // SQL_DATA_TYPE
        timeRow.push_back(ODBCField::null());               // SQL_DATETIME_SUB
        timeRow.push_back(ODBCField::null());               // CHAR_OCTET_LENGTH
        timeRow.push_back(ODBCField(1));                    // ORDINAL_POSITION
        timeRow.push_back(ODBCField(std::string("YES")));   // IS_NULLABLE

        stmt->resultSetPtr->data.insert(stmt->resultSetPtr->data.begin(), std::move(timeRow));
        stmt->resultSetPtr->numRows = numRows + 1;
        for (int i = 1; i <= numRows; ++i) {
          stmt->resultSetPtr->setValue(i, 16, ODBCField(i + 1)); // ORDINAL_POSITION
        }
      }
    }

    stmt->resultSetPtr->outputTable();

    logMessage(cnct, "SQLColumns: Query completed successfully", LOG_LEVEL_INFO);
    logMessage(cnct, "SQLColumns: Exiting", LOG_LEVEL_TRACE);
    return SQL_SUCCESS;
  }
}

SQLRETURN SQL_API SQLCompleteAsync(SQLSMALLINT handleType, SQLHANDLE handle,
                                   RETCODE* asyncRetCodePtr) {
  if (!handle) {
    logMessage(nullptr, "SQLCompleteAsync: Invalid handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  logMessage(nullptr, "SQLCompleteAsync is not implemented", LOG_LEVEL_ERROR);
  if (handle) {
    auto odbcHandle = static_cast<ODBCHandle*>(handle);
    odbcHandle->addDiagnostic("IM001", "SQLCompleteAsync Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLConnect(SQLHDBC connectionHandle, SQLCHAR* serverName, SQLSMALLINT nameLength1,
                             SQLCHAR* userName, SQLSMALLINT nameLength2, SQLCHAR* password,
                             SQLSMALLINT nameLength3) {
  if (!connectionHandle) {
    logMessage(nullptr, "SQLConnect: Invalid connection handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto cnct = static_cast<ConnectionHandle*>(connectionHandle);
  logMessage(cnct, "SQLConnect: Entering", LOG_LEVEL_TRACE);

  std::string dsnName;
  if (serverName) {
    if (nameLength1 == SQL_NTS)
      dsnName = reinterpret_cast<const char*>(serverName);
    else if (nameLength1 > 0)
      dsnName = std::string(reinterpret_cast<const char*>(serverName), nameLength1);
  }
  logMessage(cnct, "SQLConnect: DSN='" + dsnName + "'", LOG_LEVEL_INFO);

  if (dsnName.empty()) {
    logMessage(cnct, "SQLConnect: DSN name is empty", LOG_LEVEL_ERROR);
    cnct->addDiagnostic("IM002", "Data source name not found and no default driver specified");
    return SQL_ERROR;
  }

  cnct->LoadDsnFromOdbcIni(dsnName);

  if (userName) {
    std::string uid;
    if (nameLength2 == SQL_NTS)
      uid = reinterpret_cast<const char*>(userName);
    else if (nameLength2 > 0)
      uid = std::string(reinterpret_cast<const char*>(userName), nameLength2);
    if (!uid.empty()) {
      cnct->userName = uid;
      logMessage(cnct, "SQLConnect: UID overridden from parameter: " + uid, LOG_LEVEL_DEBUG);
    }
  }

  if (password) {
    std::string pwd;
    if (nameLength3 == SQL_NTS)
      pwd = reinterpret_cast<const char*>(password);
    else if (nameLength3 > 0)
      pwd = std::string(reinterpret_cast<const char*>(password), nameLength3);
    if (!pwd.empty()) {
      cnct->password = pwd;
      logMessage(cnct, "SQLConnect: PWD overridden from parameter", LOG_LEVEL_DEBUG);
    }
  }

  if (cnct->isTableModel && cnct->database.empty()) {
    logMessage(cnct, "SQLConnect: Database not set, using 'information_schema' as default",
               LOG_LEVEL_INFO);
    cnct->database = "information_schema";
  }

  if (isLogLevelEnabled(cnct, LOG_LEVEL_INFO)) {
    std::ostringstream oss;
    oss << "SQLConnect: Connecting with Server=" << cnct->serverHostName
        << ", Port=" << cnct->serverPort << ", UID=" << cnct->userName
        << ", Database=" << cnct->database
        << ", TableModel=" << (cnct->isTableModel ? "true" : "false");
    logMessage(cnct, oss.str(), LOG_LEVEL_INFO);
  }

  SQLRETURN returnCode;
  if (cnct->useRestful) {
    logMessage(cnct, "SQLConnect: Connecting using RESTful API", LOG_LEVEL_DEBUG);
    returnCode = IoTDB_DriverConnect_Rest(cnct);
  } else {
    logMessage(cnct, "SQLConnect: Connecting using Session API", LOG_LEVEL_DEBUG);
    returnCode = IoTDB_DriverConnect_Session(cnct);
  }

  logMessage(cnct, "SQLConnect: Exiting with return code: " + std::to_string(returnCode),
             LOG_LEVEL_TRACE);
  return returnCode;
}

SQLRETURN SQL_API SQLCopyDesc(SQLHDESC sourceDescHandle, SQLHDESC targetDescHandle) {
  if (!sourceDescHandle && !targetDescHandle) {
    logMessage(nullptr, "SQLCopyDesc: Invalid descriptor handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  logMessage(nullptr, "SQLCopyDesc is not implemented", LOG_LEVEL_ERROR);
  SQLHANDLE handle = targetDescHandle ? targetDescHandle : sourceDescHandle;
  if (handle) {
    auto odbcHandle = static_cast<ODBCHandle*>(handle);
    odbcHandle->addDiagnostic("IM001", "SQLCopyDesc Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLDataSources(SQLHENV environmentHandle, SQLUSMALLINT direction,
                                 SQLCHAR* serverName, SQLSMALLINT bufferLength1,
                                 SQLSMALLINT* nameLength1Ptr, SQLCHAR* description,
                                 SQLSMALLINT bufferLength2, SQLSMALLINT* nameLength2Ptr) {
  if (!environmentHandle) {
    logMessage(nullptr, "SQLDataSources: Invalid environment handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  logMessage(nullptr, "SQLDataSources is not implemented", LOG_LEVEL_ERROR);
  auto env = static_cast<EnvironmentHandle*>(environmentHandle);
  if (env) {
    env->addDiagnostic("IM001", "SQLDataSources Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLDescribeCol(SQLHSTMT statementHandle, SQLUSMALLINT columnNumber,
                                 SQLCHAR* columnName, SQLSMALLINT bufferLength,
                                 SQLSMALLINT* nameLength, SQLSMALLINT* dataType,
                                 SQLULEN* columnSize, SQLSMALLINT* decimalDigits,
                                 SQLSMALLINT* nullable) {
  // Most content copied from SQLColAttribute
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLDescribeCol: Entering", LOG_LEVEL_TRACE);

  // Prepare log message for the parameters
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLDescribeCol parameters: "
              << "StatementHandle = " << handleToString(statementHandle)
              << ", ColumnNumber = " << columnNumber << ", ColumnName = "
              << (columnName ? reinterpret_cast<const char*>(columnName) : "nullptr")
              << ", BufferLength = " << bufferLength
              << ", NameLength = " << (nameLength ? std::to_string(*nameLength) : "nullptr")
              << ", DataType = " << (dataType ? std::to_string(*dataType) : "nullptr")
              << ", ColumnSize = " << (columnSize ? std::to_string(*columnSize) : "nullptr")
              << ", DecimalDigits = "
              << (decimalDigits ? std::to_string(*decimalDigits) : "nullptr")
              << ", Nullable = " << (nullable ? std::to_string(*nullable) : "nullptr");
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  if (statementHandle == nullptr) {
    logMessage(cnct, "SQLDescribeCol: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  if (stmt->resultSetPtr == nullptr || stmt->resultSetPtr->isEmpty()) {
    logMessage(cnct, "SQLDescribeCol: Missing result set", LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY000", "No query result available");
    return SQL_ERROR;
  }

  // Validate column number
  if (columnNumber < 1 || columnNumber > stmt->resultSetPtr->getNumColumns()) {
    logMessage(cnct,
               "SQLDescribeCol: Invalid column number: " + std::to_string(columnNumber) +
                   ", max columns: " + std::to_string(stmt->resultSetPtr->getNumColumns()),
               LOG_LEVEL_ERROR);
    stmt->addDiagnostic("07009", "Invalid column number");
    return SQL_ERROR;
  }

  // Column name
  SQLRETURN sqlreturn = getColName(stmt, columnNumber, columnName, bufferLength, nameLength);
  if (sqlreturn != SQL_SUCCESS) {
    logMessage(cnct, "SQLDescribeCol: Failed to get column name", LOG_LEVEL_ERROR);
    return sqlreturn;
  }

  // Data type
  if (dataType) {
    sqlreturn = getColType(stmt, columnNumber, dataType);
    logMessage(cnct, "SQLDescribeCol: Data type set to " + std::to_string(*dataType),
               LOG_LEVEL_TRACE);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLDescribeCol: Failed to get column type", LOG_LEVEL_ERROR);
      return sqlreturn;
    }
  }

  // Column size
  if (columnSize) {
    sqlreturn = getColumnSize(*dataType, columnSize);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLDescribeCol: Failed to get column size", LOG_LEVEL_ERROR);
      return sqlreturn;
    }

    logMessage(cnct, "SQLDescribeCol: Column size set to " + std::to_string(*columnSize),
               LOG_LEVEL_DEBUG);
  }

  if (decimalDigits) {
    // This field is only applicable for fixed-point values.
    // For timestamps with sub-second precision and SQL_DECIMAL/SQL_NUMERIC types,
    // it should have a non-zero value; for other cases, it should be 0 or NULL.
    *decimalDigits = 0;
  }

  // Nullable
  if (nullable) {
    sqlreturn = getNullable(stmt, columnNumber, nullable);
    if (sqlreturn != SQL_SUCCESS) {
      logMessage(cnct, "SQLDescribeCol: Failed to get nullable attribute", LOG_LEVEL_ERROR);
      return sqlreturn;
    }

    const char* nullableStr = (*nullable == SQL_NULLABLE)   ? "NULLABLE"
                              : (*nullable == SQL_NO_NULLS) ? "NOT NULL"
                                                            : "UNKNOWN";
    logMessage(cnct, "SQLDescribeCol: Nullable attribute set to " + std::string(nullableStr),
               LOG_LEVEL_DEBUG);
  }

  logMessage(cnct, "SQLDescribeCol: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDisconnect(SQLHDBC connectionHandle) {
  if (!connectionHandle) {
    logMessage(nullptr, "SQLDisconnect: Invalid connection handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto cnct = static_cast<ConnectionHandle*>(connectionHandle);
  try {
    cnct->CloseSession();
    return SQL_SUCCESS;
  } catch (const std::exception& e) {
    cnct->addDiagnostic("08003", e.what());
    return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLEndTran(SQLSMALLINT handleType, SQLHANDLE handle, SQLSMALLINT completionType) {
  if (!handle) {
    logMessage(nullptr, "SQLEndTran: Invalid handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  auto* baseHandle = static_cast<ODBCHandle*>(handle);

  EnvironmentHandle* env = nullptr;
  ConnectionHandle* cnct = nullptr;
  StatementHandle* stmt = nullptr;
  if (handleType == SQL_HANDLE_ENV) {
    if (baseHandle && baseHandle->getHandleType() != SQL_HANDLE_ENV) {
      baseHandle->addDiagnostic("HY092", "Invalid handle type for SQLEndTran: expected ENV");
      return SQL_INVALID_HANDLE;
    }
    env = static_cast<EnvironmentHandle*>(handle);
  } else if (handleType == SQL_HANDLE_DBC) {
    if (baseHandle && baseHandle->getHandleType() != SQL_HANDLE_DBC) {
      baseHandle->addDiagnostic("HY092", "Invalid handle type for SQLEndTran: expected DBC");
      return SQL_INVALID_HANDLE;
    }
    cnct = static_cast<ConnectionHandle*>(handle);
    env = cnct ? cnct->getEnvironment() : nullptr;
  } else if (handleType == SQL_HANDLE_STMT) {
    if (baseHandle && baseHandle->getHandleType() != SQL_HANDLE_STMT) {
      baseHandle->addDiagnostic("HY092", "Invalid handle type for SQLEndTran: expected STMT");
      return SQL_INVALID_HANDLE;
    }
    stmt = static_cast<StatementHandle*>(handle);
    cnct = stmt ? stmt->getConnection() : nullptr;
    env = cnct ? cnct->getEnvironment() : nullptr;
  }

  if (cnct && isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    std::string handleTypeStr = "UNKNOWN";
    switch (handleType) {
    case SQL_HANDLE_ENV:
      handleTypeStr = "ENV";
      break;
    case SQL_HANDLE_DBC:
      handleTypeStr = "DBC";
      break;
    case SQL_HANDLE_STMT:
      handleTypeStr = "STMT";
      break;
    default:
      handleTypeStr = "UNKNOWN";
      break;
    }
    std::string completionTypeStr = "UNKNOWN";
    switch (completionType) {
    case SQL_COMMIT:
      completionTypeStr = "COMMIT";
      break;
    case SQL_ROLLBACK:
      completionTypeStr = "ROLLBACK";
      break;
    }
    logStream << "SQLEndTran parameters: "
              << ", handleType = " << handleTypeStr << "(" << handleType << ")"
              << ", handle = " << handleToString(handle)
              << ", completionType = " << completionTypeStr << "(" << completionType << ")";
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  // Determine which handle to attach diagnostics to.
  // Prefer connection diagnostics (most callers use DBC/STMT); fall back to stmt/env when needed.
  ODBCHandle* diagTarget = nullptr;
  if (cnct) {
    diagTarget = cnct;
  } else if (stmt) {
    diagTarget = stmt;
  } else if (env) {
    diagTarget = env;
  } else if (baseHandle) {
    diagTarget = baseHandle;
  }

  // Validate handleType
  if (handleType != SQL_HANDLE_ENV && handleType != SQL_HANDLE_DBC &&
      handleType != SQL_HANDLE_STMT) {
    logMessage(cnct, "SQLEndTran: Invalid HandleType", LOG_LEVEL_ERROR);
    if (diagTarget) {
      diagTarget->addDiagnostic("HY092", "Invalid attribute/option identifier");
    }
    return SQL_ERROR;
  }

  // Validate completionType
  if (completionType != SQL_COMMIT && completionType != SQL_ROLLBACK) {
    logMessage(cnct, "SQLEndTran: Invalid CompletionType", LOG_LEVEL_ERROR);
    if (diagTarget) {
      diagTarget->addDiagnostic("HY012", "Invalid transaction operation code");
    }
    return SQL_ERROR;
  }

  // IoTDB reports SQL_TXN_CAPABLE = SQL_TC_NONE (no transactions).
  // Per ODBC spec, drivers/data sources that do not support transactions should return SQL_SUCCESS
  // for SQLEndTran. We additionally return SQL_SUCCESS_WITH_INFO for ROLLBACK to inform callers
  // that the request is ignored.
  if (completionType == SQL_COMMIT) {
    logMessage(cnct, "SQLEndTran: COMMIT requested; IoTDB has no transactions, no-op",
               LOG_LEVEL_TRACE);
    return SQL_SUCCESS;
  }

  // completionType == SQL_ROLLBACK
  logMessage(cnct, "SQLEndTran: ROLLBACK requested; IoTDB has no transactions, ignored",
             LOG_LEVEL_DEBUG);
  if (diagTarget) {
    diagTarget->addDiagnostic("HYC00", "IoTDB does not support transactions; rollback ignored");
  }
  return SQL_SUCCESS_WITH_INFO;
}

SQLRETURN SQLError(SQLHENV environmentHandle, // Input: Environment handle
                   SQLHDBC connectionHandle,  // Input: Connection handle
                   SQLHSTMT statementHandle,  // Input: Statement handle
                   SQLCHAR* sqlstate,         // Output: SQLSTATE code
                   SQLINTEGER* nativeError,   // Output: Native error code
                   SQLCHAR* messageText,      // Output: Error message text
                   SQLSMALLINT bufferLength,  // Input: Message text buffer length
                   SQLSMALLINT* textLength    // Output: Actual length of message text
) {
  // Determine the appropriate connection handle for logging
  ConnectionHandle* cnct = nullptr;
  if (statementHandle != SQL_NULL_HSTMT) {
    const auto stmt = reinterpret_cast<StatementHandle*>(statementHandle);
    cnct = stmt->getConnection();
  } else if (connectionHandle != SQL_NULL_HDBC) {
    cnct = reinterpret_cast<ConnectionHandle*>(connectionHandle);
  }

  logMessage(cnct, "SQLError: Entering", LOG_LEVEL_TRACE);

  // Log input parameters if TRACE level is enabled
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLError parameters: "
              << "EnvironmentHandle = " << environmentHandle
              << ", ConnectionHandle = " << connectionHandle
              << ", StatementHandle = " << statementHandle
              << ", SqlState = " << (sqlstate ? reinterpret_cast<const char*>(sqlstate) : "nullptr")
              << ", NativeError = " << (nativeError ? std::to_string(*nativeError) : "nullptr")
              << ", MessageText = "
              << (messageText ? reinterpret_cast<const char*>(messageText) : "nullptr")
              << ", BufferLength = " << bufferLength
              << ", TextLength = " << (textLength ? std::to_string(*textLength) : "nullptr");
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  SQLHANDLE handle;
  SQLSMALLINT handle_type;

  // Determine handle type and handle
  if (statementHandle != SQL_NULL_HSTMT) {
    handle = statementHandle;
    handle_type = SQL_HANDLE_STMT;
    logMessage(cnct, "SQLError: Using statement handle", LOG_LEVEL_DEBUG);
  } else if (connectionHandle != SQL_NULL_HDBC) {
    handle = connectionHandle;
    handle_type = SQL_HANDLE_DBC;
    logMessage(cnct, "SQLError: Using connection handle", LOG_LEVEL_DEBUG);
  } else if (environmentHandle != SQL_NULL_HENV) {
    handle = environmentHandle;
    handle_type = SQL_HANDLE_ENV;
    logMessage(cnct, "SQLError: Using environment handle", LOG_LEVEL_DEBUG);
  } else {
    logMessage(cnct, "SQLError: No valid handle provided", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE; // Invalid handle
  }

  logMessage(cnct, "SQLError: Calling SQLGetDiagRec", LOG_LEVEL_DEBUG);

  // Call SQLGetDiagRec to retrieve error information
  SQLRETURN retcode = SQLGetDiagRec(handle_type, handle, 1, sqlstate, nativeError, messageText,
                                    bufferLength, textLength);
  if (SQL_SUCCEEDED(retcode))
    static_cast<ODBCHandle*>(handle)->consumeFirstDiagnostic();

  // Log the result of SQLGetDiagRec
  if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
    std::stringstream resultStream;
    resultStream << "SQLError: Retrieved error - SQLSTATE: "
                 << (sqlstate ? reinterpret_cast<const char*>(sqlstate) : "null")
                 << ", NativeError: " << (nativeError ? std::to_string(*nativeError) : "null")
                 << ", Message: "
                 << (messageText ? reinterpret_cast<const char*>(messageText) : "null");
    logMessage(cnct, resultStream.str(), LOG_LEVEL_DEBUG);
  } else if (retcode == SQL_NO_DATA) {
    logMessage(cnct, "SQLError: No error information available", LOG_LEVEL_DEBUG);
  } else {
    logMessage(cnct, "SQLError: SQLGetDiagRec failed", LOG_LEVEL_ERROR);
  }

  // Return appropriate SQLError return value based on SQLGetDiagRec's return code
  logMessage(cnct, "SQLError: Exiting", LOG_LEVEL_TRACE);

  switch (retcode) {
  case SQL_SUCCESS:
  case SQL_SUCCESS_WITH_INFO:
    return SQL_SUCCESS;
  case SQL_NO_DATA:
    return SQL_NO_DATA;
  default:
    return SQL_ERROR;
  }
}

/**
 * Safely converts SQLCHAR array to std::string with proper handling of unsigned char
 *
 * This function ensures that unsigned char values are properly converted to char
 * without sign extension issues, making it safe for all character values (0-255).
 *
 * @param statementText Pointer to SQLCHAR array (unsigned char*)
 * @param textLength Length of the text or SQL_NTS for null-terminated string
 * @return std::string containing the converted text, or empty string on error
 */
std::string ConvertSQLCHARToString(SQLCHAR* statementText, SQLINTEGER textLength) {
  if (statementText == nullptr) {
    return std::string();
  }

  // Determine the actual length
  size_t length = 0;
  if (textLength == SQL_NTS) {
    // Calculate length for null-terminated string
    const SQLCHAR* ptr = statementText;
    while (*ptr != '\0') {
      ++length;
      ++ptr;
    }
  } else if (textLength > 0) {
    length = static_cast<size_t>(textLength);
  } else {
    return std::string(); // Invalid length
  }

  // Convert each character safely
  std::string result;
  result.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    // Safe conversion: static_cast ensures proper value preservation
    result.push_back(static_cast<char>(statementText[i]));
  }

  return result;
}

/**
 * If the SQL is a SELECT whose first column is exactly "Time" followed by a comma,
 * removes that "Time," so IoTDB receives a SELECT without the Time column.
 * (IoTDB tree model always returns a time column; PowerBI adds Time to the SELECT
 * list, which the server does not accept.) Only the leading column name "Time" is
 * matched; other columns or "Timestamp" etc. are left unchanged.
 * @param sql Statement to modify in place; unchanged if pattern does not match.
 */
static void StripLeadingTimeColumnFromSelect(std::string& sql) {
  size_t i = 0;
  while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\r' || sql[i] == '\n')) {
    ++i;
  }
  if (i + 6 > sql.size())
    return;
  const char* p = sql.c_str() + i;
  if ((p[0] == 's' || p[0] == 'S') && (p[1] == 'e' || p[1] == 'E') &&
      (p[2] == 'l' || p[2] == 'L') && (p[3] == 'e' || p[3] == 'E') &&
      (p[4] == 'c' || p[4] == 'C') && (p[5] == 't' || p[5] == 'T')) {
    i += 6;
  } else {
    return;
  }
  while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\r' || sql[i] == '\n')) {
    ++i;
  }
  if (i + 4 > sql.size())
    return;
  p = sql.c_str() + i;
  if ((p[0] != 't' && p[0] != 'T') || (p[1] != 'i' && p[1] != 'I') ||
      (p[2] != 'm' && p[2] != 'M') || (p[3] != 'e' && p[3] != 'E')) {
    return;
  }
  size_t timeStart = i;
  i += 4;
  if (i < sql.size()) {
    char c = sql[i];
    if (c != ',' && c != ' ' && c != '\t' && c != '\r' && c != '\n')
      return;
  }
  while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\r' || sql[i] == '\n')) {
    ++i;
  }
  if (i >= sql.size() || sql[i] != ',')
    return;
  sql.erase(timeStart, i - timeStart + 1);
}

// saveStatementText: Optional, defaults to true
SQLRETURN IoTDB_ExecDirect(StatementHandle* const stmt, const std::string& statementText,
                           bool saveStatementText) {
  ConnectionHandle* cnct = stmt->getConnection();
  logMessage(cnct, "IoTDB_ExecDirect: Entering", LOG_LEVEL_TRACE);

  std::string sqlForServer = statementText;
  StripLeadingTimeColumnFromSelect(sqlForServer);
  if (saveStatementText) {
    stmt->statementText = sqlForServer;
    logMessage(cnct, "IoTDB_ExecDirect: Saved statement text", LOG_LEVEL_DEBUG);
  }

  // Choose API based on connection settings
  if (cnct->useRestful) {
    logMessage(cnct, "IoTDB_ExecDirect: Using RESTful API", LOG_LEVEL_DEBUG);
    return IoTDB_ExecDirect_Rest(stmt, sqlForServer);
  } else {
    logMessage(cnct, "IoTDB_ExecDirect: Using Session API", LOG_LEVEL_DEBUG);
    return IoTDB_ExecDirect_Session(stmt, sqlForServer);
  }
}

SQLRETURN SQL_API SQLExecDirect(SQLHSTMT statementHandle, SQLCHAR* statementText,
                                SQLINTEGER textLength) {
  StatementHandle* stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLExecDirect: Entering", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLExecDirect parameters: "
              << "statementHandle = " << handleToString(statementHandle) << ", statementText = "
              << (statementText ? reinterpret_cast<const char*>(statementText) : "nullptr")
              << ", textLength = " << textLength;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  if (!statementHandle) {
    logMessage(cnct, "SQLExecDirect: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  // Convert SQLCHAR to std::string
  std::string sqlStatement = ConvertSQLCHARToString(statementText, textLength);
  logMessage(cnct, "SQLExecDirect: Executing statement: " + sqlStatement, LOG_LEVEL_DEBUG);

  const SQLRETURN returnCode = IoTDB_ExecDirect(stmt, sqlStatement);
  if (returnCode != SQL_SUCCESS) {
    logMessage(cnct, "SQLExecDirect: Execution failed with code: " + std::to_string(returnCode),
               LOG_LEVEL_ERROR);
  } else {
    logMessage(cnct, "SQLExecDirect: Execution completed successfully", LOG_LEVEL_INFO);
  }

  logMessage(cnct, "SQLExecDirect: Exiting", LOG_LEVEL_TRACE);
  return returnCode;
}

SQLRETURN SQL_API SQLExecute(SQLHSTMT statementHandle) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLExecute: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLExecute is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLExecute Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN fillColBindBuffer(StatementHandle* stmt, SQLUSMALLINT col, const ODBCField& value) {
  BindColInfo* binding = stmt->resultSetPtr->getBindColInfo(col);
  if (!binding || !binding->isBound) {
    return SQL_ERROR;
  }
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;

  if (value.isNull()) {
    if (binding->strLen_or_IndPtr) {
      *binding->strLen_or_IndPtr = SQL_NULL_DATA;
    } else {
      stmt->addDiagnostic("22002", "Indicator variable required but not supplied");
      return SQL_ERROR;
    }

    if (binding->targetValuePtr && binding->bufferLength > 0) {
      if (binding->targetType == SQL_C_CHAR) {
        static_cast<char*>(binding->targetValuePtr)[0] = '\0';
      } else if (binding->targetType == SQL_C_WCHAR) {
        static_cast<SQLWCHAR*>(binding->targetValuePtr)[0] = 0;
      }
    }
    return SQL_SUCCESS;
  }

  return CopyFieldToTarget(stmt, value, binding->targetType, binding->targetValuePtr,
                           binding->bufferLength, binding->strLen_or_IndPtr, true, col);
}

SQLRETURN SQL_API SQLFetch(SQLHSTMT statementHandle) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;

  logMessage(cnct, "SQLFetch: Entering", LOG_LEVEL_TRACE);

  // Prepare log message for the parameters
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLFetch parameters: "
              << "statementHandle = " << handleToString(statementHandle);
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  // In SQLAllocHandle we're allocating a NULL_Handle, so I disabled this.
  if (!statementHandle) {
    logMessage(cnct, "SQLFetch: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  if (stmt->resultSetPtr == nullptr) {
    logMessage(cnct, "SQLFetch: No result set available", LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY000", "No result set available");
    return SQL_ERROR;
  }

  // Progress to the next row
  stmt->curRow = (stmt->curRow < 0) ? 0 : (stmt->curRow + 1);
  logMessage(cnct, "SQLFetch: Moving to row " + std::to_string(stmt->curRow), LOG_LEVEL_DEBUG);

  // If we've reached the end, check if we can load more data
  if (stmt->curRow == stmt->resultSetPtr->getNumRows()) {
    logMessage(cnct, "SQLFetch: Reached end of current result set", LOG_LEVEL_DEBUG);

    // Choose streaming method based on API type
    if (cnct->useRestful) {
      // REST API: fetch next batch from server by modifying SQL query
      logMessage(cnct, "SQLFetch: Fetching next batch using REST API streaming", LOG_LEVEL_DEBUG);
      SQLSMALLINT ret = streamNextBatch(stmt);
      if (ret == SQL_NO_DATA) {
        logMessage(cnct, "SQLFetch: No data available from REST API streaming", LOG_LEVEL_INFO);
        return ret;
      }
      if (ret != SQL_SUCCESS) {
        logMessage(cnct,
                   "SQLFetch: Failed to fetch next batch from REST API with code: " +
                       std::to_string(ret),
                   LOG_LEVEL_ERROR);
        return ret;
      }
      stmt->curRow = 0;
      logMessage(cnct, "SQLFetch: Reset to first row of new REST batch", LOG_LEVEL_DEBUG);
    } else {
      // Session API: load next batch from existing SessionDataSet
      logMessage(cnct, "SQLFetch: Loading next batch from Session API", LOG_LEVEL_DEBUG);
      SQLSMALLINT ret = streamNextBatch(stmt);
      if (ret == SQL_NO_DATA) {
        logMessage(cnct, "SQLFetch: No data available from Session API streaming", LOG_LEVEL_INFO);
        return ret;
      }
      if (ret != SQL_SUCCESS) {
        logMessage(cnct,
                   "SQLFetch: Failed to fetch next batch from Session API with code: " +
                       std::to_string(ret),
                   LOG_LEVEL_ERROR);
        return ret;
      }
      stmt->curRow = 0;
      logMessage(cnct, "SQLFetch: Reset to first row of new Session batch", LOG_LEVEL_DEBUG);
    }
  }

  // col = 0 refers to bookmark column, which IoTDB don't have one, so it's skipped.
  for (SQLUSMALLINT col = 1; col <= stmt->resultSetPtr->getNumColumns(); ++col) {
    BindColInfo* binding = stmt->resultSetPtr->getBindColInfo(col);

    if (!binding || !binding->isBound) {
      logMessage(cnct, "SQLFetch: Column " + std::to_string(col) + " is not bound, skipping",
                 LOG_LEVEL_DEBUG);
      continue;
    }

    const ODBCField& value = stmt->resultSetPtr->getValue(stmt->curRow, col - 1);
    logMessage(cnct, "SQLFetch: Using string data for column " + std::to_string(col),
               LOG_LEVEL_DEBUG);
    SQLRETURN rc = fillColBindBuffer(stmt, col, value);

    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      stmt->addDiagnostic("01000", "Error occurred when filling the buffer");
      logMessage(cnct, "SQLFetch: Data conversion failed for column " + std::to_string(col),
                 LOG_LEVEL_ERROR);
      return rc;
    }

    logMessage(cnct, "SQLFetch: Successfully filled buffer for column " + std::to_string(col),
               LOG_LEVEL_DEBUG);
  }

  logMessage(cnct, "SQLFetch: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLFetchScroll(SQLHSTMT statementHandle, SQLSMALLINT fetchOrientation,
                                 SQLLEN fetchOffset) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLFetchScroll: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLFetchScroll is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLFetchScroll Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLFreeConnect(SQLHDBC connectionHandle) {
  logMessage("SQLFreeConnect");
  auto cnct = static_cast<ConnectionHandle*>(connectionHandle);
  if (cnct) {
    cnct->CloseSession();
  }
  SQLRETURN retcode = SQLFreeHandle(SQL_HANDLE_DBC, connectionHandle);
  if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) {
    std::ostringstream oss;
    oss << "SQLFreeConnect: Failed to free connection handle. retcode = " << retcode << "\n";
    logMessage(oss.str());
    return retcode;
  }
  return retcode;
}

SQLRETURN SQL_API SQLFreeEnv(SQLHENV environmentHandle) {
  logMessage("SQLFreeEnv");
  // Deprecated function, delegate to SQLAllocHandle for env allocation.
  SQLRETURN retcode = SQLFreeHandle(SQL_HANDLE_ENV, environmentHandle);
  if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) {
    std::ostringstream oss;
    oss << "SQLAllocEnv: Failed to free environment handle. retcode = " << retcode << "\n";
    logMessage(oss.str());
    return retcode;
  }
  return retcode;
}

SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT handleType, SQLHANDLE handle) {
  logMessage("Entering SQLFreeHandle");

  // Check if handle is NULL
  if (!handle) {
    logMessage("SQLFreeHandle: handle is already NULL.");
    return SQL_INVALID_HANDLE; // Invalid handle
  }

  // Check if handleType is valid
  if (handleType != SQL_HANDLE_ENV && handleType != SQL_HANDLE_DBC &&
      handleType != SQL_HANDLE_STMT && handleType != SQL_HANDLE_DESC) {
    logMessage("SQLFreeHandle: invalid handle type.");
    return SQL_ERROR; // Invalid handle type
  }

  // Convert handle to ODBCHandle type
  ODBCHandle* odbcHandle = static_cast<ODBCHandle*>(handle);

  // Check if handle has already been freed
  if (odbcHandle->isHandleFreed()) {
    logMessage("SQLFreeHandle: handle is already freed.");
    return SQL_SUCCESS; // Handle already freed, return success directly
  }

  // Mark handle as freed
  odbcHandle->markAsFreed();

  // Release resources based on handleType
  switch (handleType) {
  case SQL_HANDLE_ENV: {
    logMessage("SQLFreeHandle: freeing environment handle.");
    delete static_cast<EnvironmentHandle*>(handle);
    break;
  }
  case SQL_HANDLE_DBC: {
    logMessage("SQLFreeHandle: freeing connection handle.");
    delete static_cast<ConnectionHandle*>(handle);
    break;
  }
  case SQL_HANDLE_STMT: {
    logMessage("SQLFreeHandle: freeing statement handle.");
    delete static_cast<StatementHandle*>(handle);
    break;
  }
  default: {
    logMessage("SQLFreeHandle: unknown handle type.");
    return SQL_ERROR; // Unknown handle type
  }
  }

  logMessage("Exiting SQLFreeHandle");
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT statementHandle, SQLUSMALLINT option) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLFreeStmt: Entering", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLFreeStmt parameters: "
              << "statementHandle = " << handleToString(statementHandle) << ", option = " << option;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  if (!statementHandle) {
    logMessage(cnct, "SQLFreeStmt: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  switch (option) {
  case SQL_CLOSE: {
    logMessage(cnct, "SQLFreeStmt: Processing SQL_CLOSE option", LOG_LEVEL_DEBUG);
    // Close cursor and reset result set
    stmt->curRow = -1; // Reset current row
    stmt->ClearResultSet();
    logMessage(cnct, "SQLFreeStmt: Cursor closed and result set reset", LOG_LEVEL_DEBUG);
    break;
  }

  case SQL_UNBIND: {
    logMessage(cnct, "SQLFreeStmt: Processing SQL_UNBIND option", LOG_LEVEL_DEBUG);
    // Unbind columns
    if (stmt->resultSetPtr) {
      stmt->resultSetPtr->unbindColumns();
    }
    logMessage(cnct, "SQLFreeStmt: Column bindings released", LOG_LEVEL_DEBUG);
    break;
  }

  case SQL_RESET_PARAMS: {
    logMessage(cnct, "SQLFreeStmt: Processing SQL_RESET_PARAMS option", LOG_LEVEL_DEBUG);
    // Unbind parameters (not yet supported)
    logMessage(cnct, "SQLFreeStmt: Parameter reset requested but not supported yet",
               LOG_LEVEL_WARN);
    break;
  }

  case SQL_DROP: {
    logMessage(cnct, "SQLFreeStmt: Processing SQL_DROP option", LOG_LEVEL_DEBUG);
    // This option is deprecated in ODBC 3.0, recommend using SQLFreeHandle instead
    logMessage(cnct, "SQLFreeStmt: SQL_DROP is deprecated in ODBC 3.0, use SQLFreeHandle instead",
               LOG_LEVEL_WARN);
    break;
  }

  default: {
    logMessage(cnct, "SQLFreeStmt: Unrecognized option: " + std::to_string(option), LOG_LEVEL_WARN);
    stmt->addDiagnostic("HY092", "Invalid option value");
    return SQL_ERROR;
  }
  }
  logMessage(cnct, "SQLFreeStmt: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC connectionHandle, SQLINTEGER attribute,
                                    SQLPOINTER valuePtr, SQLINTEGER bufferLength,
                                    SQLINTEGER* stringLength) {
  ConnectionHandle* cnct = static_cast<ConnectionHandle*>(connectionHandle);

  logMessage(cnct, "SQLGetConnectAttr: Entering", LOG_LEVEL_TRACE);

  // Only build parameter log message if TRACE level is enabled
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLGetConnectAttr parameters: "
              << "ConnectionHandle = " << handleToString(connectionHandle)
              << ", Attribute = " << attribute << ", ValuePtr = " << valueToString(valuePtr)
              << ", BufferLength = " << bufferLength
              << ", StringLengthPtr = " << valueToString(stringLength);
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  // In SQLAllocHandle we're allocating a NULL_Handle, so I disabled this.
  if (!connectionHandle) {
    logMessage(cnct, "SQLGetConnectAttr: Invalid connection handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  switch (attribute) {
  // Connection metadata attributes
  case SQL_ATTR_CONNECTION_DEAD:
    logMessage(cnct, "SQLGetConnectAttr: Processing SQL_ATTR_CONNECTION_DEAD", LOG_LEVEL_DEBUG);
    if (valuePtr) {
      *((SQLUINTEGER*)valuePtr) = SQL_CD_FALSE; // Assume connection is alive
      logMessage(cnct, "SQLGetConnectAttr: Connection status set to ALIVE", LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_CONNECTION_TIMEOUT:
    logMessage(cnct, "SQLGetConnectAttr: Processing SQL_ATTR_CONNECTION_TIMEOUT", LOG_LEVEL_DEBUG);
    if (valuePtr) {
      *((SQLUINTEGER*)valuePtr) = cnct->timeoutConnection;
      logMessage(cnct,
                 "SQLGetConnectAttr: Connection timeout set to " +
                     std::to_string(cnct->timeoutConnection),
                 LOG_LEVEL_DEBUG);
    }
    break;

  // Transaction control attributes
  case SQL_ATTR_AUTOCOMMIT:
    logMessage(cnct, "SQLGetConnectAttr: Processing SQL_ATTR_AUTOCOMMIT", LOG_LEVEL_DEBUG);
    if (valuePtr) {
      *((SQLUINTEGER*)valuePtr) = SQL_AUTOCOMMIT_ON; // Default to auto-commit
      logMessage(cnct, "SQLGetConnectAttr: Auto-commit set to ON", LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_TXN_ISOLATION:
    logMessage(cnct, "SQLGetConnectAttr: Processing SQL_ATTR_TXN_ISOLATION", LOG_LEVEL_DEBUG);
    if (valuePtr) {
      *((SQLUINTEGER*)valuePtr) = SQL_TXN_READ_COMMITTED; // Default isolation level
      logMessage(cnct, "SQLGetConnectAttr: Transaction isolation set to READ_COMMITTED",
                 LOG_LEVEL_DEBUG);
    }
    break;

  // Catalog/schema attributes
  case SQL_ATTR_CURRENT_CATALOG:
    logMessage(cnct, "SQLGetConnectAttr: Processing SQL_ATTR_CURRENT_CATALOG", LOG_LEVEL_DEBUG);
    if (valuePtr && bufferLength > 0) {
      const char* currentDb = (cnct->database).c_str();
      strncpy((char*)valuePtr, currentDb, bufferLength - 1);
      ((char*)valuePtr)[bufferLength - 1] = '\0';
      if (stringLength) {
        *stringLength = strlen(currentDb);
      }
      logMessage(cnct, "SQLGetConnectAttr: Current catalog set to " + cnct->database,
                 LOG_LEVEL_DEBUG);
    }
    break;

  // ODBC behavior control attributes
  case SQL_ATTR_ODBC_CURSORS:
    logMessage(cnct, "SQLGetConnectAttr: Processing SQL_ATTR_ODBC_CURSORS", LOG_LEVEL_DEBUG);
    if (valuePtr) {
      *((SQLUINTEGER*)valuePtr) = SQL_CUR_USE_DRIVER; // Use driver cursors
      logMessage(cnct, "SQLGetConnectAttr: ODBC cursor type set to USE_DRIVER", LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_ACCESS_MODE:
    logMessage(cnct, "SQLGetConnectAttr: Processing SQL_ATTR_ACCESS_MODE", LOG_LEVEL_DEBUG);
    if (valuePtr) {
      *((SQLUINTEGER*)valuePtr) = SQL_MODE_READ_WRITE; // Default read-write mode
      logMessage(cnct, "SQLGetConnectAttr: Access mode set to READ_WRITE", LOG_LEVEL_DEBUG);
    }
    break;

  default:
    logMessage(cnct, "SQLGetConnectAttr: Unsupported attribute: " + std::to_string(attribute),
               LOG_LEVEL_WARN);
    cnct->addDiagnostic("HY092", "Invalid attribute/option identifier");
    return SQL_ERROR;
  }

  // Update StringLength if required
  if (stringLength) {
    *stringLength = 0; // Update with actual size if applicable
  }

  logMessage(cnct, "SQLGetConnectAttr: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetConnectOption(SQLHDBC connectionHandle, SQLUSMALLINT option,
                                      SQLPOINTER value) {
  if (!connectionHandle) {
    logMessage(nullptr, "SQLGetConnectOption: Invalid connection handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  const auto cnct = static_cast<ConnectionHandle*>(connectionHandle);
  logMessage(cnct, "SQLGetConnectOption is not implemented", LOG_LEVEL_ERROR);
  if (cnct) {
    cnct->addDiagnostic("IM001", "SQLGetConnectOption Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLGetCursorName(SQLHSTMT statementHandle, SQLCHAR* cursorName,
                                   SQLSMALLINT bufferLength, SQLSMALLINT* nameLengthPtr) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLGetCursorName: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLGetCursorName is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLGetCursorName Function not implemented");
  }
  return SQL_ERROR;
}

// Currently still using JSON format to get data. TODO: Modify after optimizing storage format.
SQLRETURN SQL_API SQLGetData(const SQLHSTMT statementHandle, const SQLUSMALLINT columnNumber,
                             const SQLSMALLINT targetType, const SQLPOINTER targetValue,
                             const SQLLEN bufferLength, SQLLEN* strLenOrInd) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;

  logMessage(cnct, "SQLGetData: Entering", LOG_LEVEL_TRACE);

  // Prepare log message for the parameters
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLGetData parameters: "
              << "statementHandle = " << handleToString(statementHandle)
              << ", columnNumber = " << columnNumber
              << ", targetType = " << CDataTypeName(targetType) << ", targetValue = " << targetValue
              << ", bufferLength = " << bufferLength << ", strLenOrInd = " << strLenOrInd;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  // In SQLAllocHandle we're allocating a NULL_Handle, so I disabled this.
  if (!statementHandle) {
    logMessage(cnct, "SQLGetData: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  logMessage(cnct, "SQLGetData: stmt->curRow = " + std::to_string(stmt->curRow), LOG_LEVEL_TRACE);
  if (stmt->curRow < 0) {
    logMessage(cnct, "SQLGetData: Current row is negative, setting to 0", LOG_LEVEL_WARN);
    stmt->curRow = 0;
  }

  if (stmt->curRow != stmt->lastGetDataRow) {
    stmt->lastGetDataRow = stmt->curRow;
    stmt->lastGetDataCol = 0;
    const int numCols = stmt->resultSetPtr ? stmt->resultSetPtr->getNumColumns() : 0;
    stmt->getDataOffsets.assign(static_cast<size_t>(numCols + 1), 0);
  }

  const ODBCField* valuePtr = nullptr;

  // Retrieve data as string and parse as JSON (works for both Session and REST APIs with our new approach)
  logMessage(cnct, "SQLGetData: Retrieving data for processing", LOG_LEVEL_DEBUG);

  try {
    valuePtr = &stmt->resultSetPtr->getValue(stmt->curRow, columnNumber - 1);
    if (valuePtr->isNull()) {
      logMessage(cnct,
                 "SQLGetData: NULL data retrieved for column " + std::to_string(columnNumber) +
                     ", row " + std::to_string(stmt->curRow),
                 LOG_LEVEL_DEBUG);
      if (strLenOrInd != nullptr) {
        logMessage(cnct, "SQLGetData: Setting strLenOrInd to SQL_NULL_DATA", LOG_LEVEL_DEBUG);
        *strLenOrInd = SQL_NULL_DATA; // Complies with ODBC specification, marks data as NULL
        return SQL_SUCCESS;
      } else {
        // Indicator pointer not provided, triggers ODBC standard error: 22002
        logMessage(cnct,
                   "SQLGetData: Error - Indicator variable (strLenOrInd) required but not supplied "
                   "for NULL value",
                   LOG_LEVEL_ERROR);
        stmt->addDiagnostic("22002", "Indicator variable required but not supplied");
        return SQL_ERROR;
      }
    } else {
      logMessage(cnct, "SQLGetData: Successfully retrieved value", LOG_LEVEL_DEBUG);
    }
  } catch (const std::exception& e) {
    logMessage(cnct, "SQLGetData: Unexpected error during data retrieval: " + std::string(e.what()),
               LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY000", "Failed to retrieve data value");
    return SQL_ERROR;
  }

  if (isLogLevelEnabled(cnct, LOG_LEVEL_DEBUG)) {
    std::stringstream logStream;
    logStream << "SQLGetData: Retrieved value = " << valuePtr->toString();
    logMessage(cnct, logStream.str(), LOG_LEVEL_DEBUG);
  }

  if (columnNumber != stmt->lastGetDataCol &&
      stmt->getDataOffsets.size() > static_cast<size_t>(columnNumber)) {
    stmt->getDataOffsets[columnNumber] = 0;
  }
  stmt->lastGetDataCol = columnNumber;

  SQLSMALLINT effectiveTargetType = (targetType == SQL_C_DEFAULT)
                                        ? stmt->resultSetPtr->getDefaultCTypeForColumn(columnNumber)
                                        : targetType;

  SQLRETURN copyResult = SQL_SUCCESS;
  if (effectiveTargetType == SQL_C_CHAR || effectiveTargetType == SQL_C_WCHAR ||
      effectiveTargetType == SQL_C_BINARY) {
    copyResult = CopyFieldToTargetVariable(stmt, *valuePtr, effectiveTargetType,
                                           const_cast<SQLPOINTER>(targetValue), bufferLength,
                                           strLenOrInd, columnNumber);
  } else {
    copyResult =
        CopyFieldToTarget(stmt, *valuePtr, effectiveTargetType, const_cast<SQLPOINTER>(targetValue),
                          bufferLength, strLenOrInd, false, 0);
  }
  if (copyResult != SQL_SUCCESS && copyResult != SQL_SUCCESS_WITH_INFO &&
      copyResult != SQL_NO_DATA) {
    logMessage(cnct,
               "SQLGetData: Unknown target type: " + CDataTypeName(targetType) +
                   ", returning SQL_ERROR",
               LOG_LEVEL_TRACE);
    return copyResult;
  }

  logMessage(cnct, "SQLGetData: Exiting successfully", LOG_LEVEL_TRACE);
  return copyResult;
}

SQLRETURN SQL_API SQLGetDescField(SQLHDESC descriptorHandle, SQLSMALLINT recNumber,
                                  SQLSMALLINT fieldIdentifier, SQLPOINTER value,
                                  SQLINTEGER bufferLength, SQLINTEGER* stringLength) {
  if (!descriptorHandle) {
    logMessage(nullptr, "SQLGetDescField: Invalid descriptor handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  logMessage(nullptr, "SQLGetDescField is not implemented", LOG_LEVEL_ERROR);
  if (descriptorHandle) {
    auto odbcHandle = static_cast<ODBCHandle*>(descriptorHandle);
    odbcHandle->addDiagnostic("IM001", "SQLGetDescField Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLGetDescRec(SQLHDESC descriptorHandle, SQLSMALLINT recNumber, SQLCHAR* name,
                                SQLSMALLINT bufferLength, SQLSMALLINT* stringLengthPtr,
                                SQLSMALLINT* typePtr, SQLSMALLINT* subTypePtr, SQLLEN* lengthPtr,
                                SQLSMALLINT* precisionPtr, SQLSMALLINT* scalePtr,
                                SQLSMALLINT* nullablePtr) {
  if (!descriptorHandle) {
    logMessage(nullptr, "SQLGetDescRec: Invalid descriptor handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  logMessage(nullptr, "SQLGetDescRec is not implemented", LOG_LEVEL_ERROR);
  if (descriptorHandle) {
    auto odbcHandle = static_cast<ODBCHandle*>(descriptorHandle);
    odbcHandle->addDiagnostic("IM001", "SQLGetDescRec Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLGetDiagField(const SQLSMALLINT handleType, const SQLHANDLE handle,
                                  const SQLSMALLINT recNumber, const SQLSMALLINT diagIdentifier,
                                  const SQLPOINTER diagInfoPtr, const SQLSMALLINT bufferLength,
                                  SQLSMALLINT* stringLengthPtr) {
  ConnectionHandle* cnct = nullptr;
  if (handleType == SQL_HANDLE_DBC && handle) {
    cnct = static_cast<ConnectionHandle*>(handle);
  } else if (handleType == SQL_HANDLE_STMT && handle) {
    const auto stmt = static_cast<StatementHandle*>(handle);
    cnct = stmt->getConnection();
  }
  // Log entry into SQLGetDiagField
  logMessage("Entering SQLGetDiagField");

  // Prepare a log message for the parameters
  std::stringstream logStream;
  logStream << "Parameters: "
            << "handleType = " << handleType << ", handle = " << handleToString(handle)
            << ", recNumber = " << recNumber << ", diagIdentifier = " << diagIdentifier
            << ", bufferLength = " << bufferLength;
  logMessage(logStream.str());

  auto* odbcHandle = static_cast<ODBCHandle*>(handle);
  std::string sqlState, message;
  int nativeError;

  const auto diagnostic = odbcHandle->getDiagnostic(recNumber, sqlState, message, nativeError);
  if (!diagnostic) {
    logMessage("Exiting SQLGetDiagField: Couldn't find diagnostic information\n");
    return SQL_NO_DATA;
  }

  // Dummy responses for some common DiagIdentifiers
  switch (diagIdentifier) {
  case SQL_DIAG_SQLSTATE:
    if (diagInfoPtr && bufferLength > 0) {
      std::snprintf(static_cast<char*>(diagInfoPtr), bufferLength, "%s", sqlState.c_str());
      if (stringLengthPtr) {
        *stringLengthPtr = static_cast<SQLSMALLINT>(sqlState.length());
      }
      logMessage("SQLGetDiagField returning SQL_DIAG_SQLSTATE = " + sqlState);
    }
    break;

  case SQL_DIAG_NATIVE:
    if (diagInfoPtr) {
      *static_cast<SQLINTEGER*>(diagInfoPtr) = 0; // No error
      logMessage("SQLGetDiagField returning SQL_DIAG_NATIVE = 0");
    }
    break;

  case SQL_DIAG_MESSAGE_TEXT:
    if (diagInfoPtr && bufferLength > 0) {
      std::snprintf(static_cast<char*>(diagInfoPtr), bufferLength, "%s", message.c_str());
      if (stringLengthPtr) {
        *stringLengthPtr = static_cast<SQLSMALLINT>(std::strlen(message.c_str()));
      }
      logMessage("SQLGetDiagField returning SQL_DIAG_MESSAGE_TEXT = " + std::string(message));
    }
    break;

    // Tell the client which concept to use when interpreting the status codes
  case SQL_DIAG_CLASS_ORIGIN:
    if (diagInfoPtr && bufferLength > 0) {
      // Return "ISO 9075" for standard SQL classes and "ODBC 3.0" for custom ones.
      std::snprintf(static_cast<char*>(diagInfoPtr), bufferLength, "%s", "ISO 9075");
      static_cast<char*>(diagInfoPtr)[bufferLength - 1] = '\0'; // Null-terminate
      if (stringLengthPtr) {
        *stringLengthPtr = static_cast<SQLSMALLINT>(std::strlen("ISO 9075"));
      }
      logMessage("SQLGetDiagField returning SQL_DIAG_CLASS_ORIGIN = ISO 9075");
    }
    break;

    // Tell the client which sub-concept to use when interpreting the status codes
  case SQL_DIAG_SUBCLASS_ORIGIN:
    if (diagInfoPtr && bufferLength > 0) {
      // Return "ISO 9075" for standard SQL classes and "ODBC 3.0" for custom ones.
      std::snprintf(static_cast<char*>(diagInfoPtr), bufferLength, "%s", "ISO 9075");
      static_cast<char*>(diagInfoPtr)[bufferLength - 1] = '\0'; // Null-terminate
      if (stringLengthPtr) {
        *stringLengthPtr = static_cast<SQLSMALLINT>(std::strlen("ISO 9075"));
      }
      logMessage("SQLGetDiagField returning SQL_DIAG_SUBCLASS_ORIGIN = ISO 9075");
    }
    break;

  case SQL_DIAG_CONNECTION_NAME:
    if (diagInfoPtr && bufferLength > 0) {
      std::snprintf(static_cast<char*>(diagInfoPtr), bufferLength, "%s", "DefaultConnection");
      static_cast<char*>(diagInfoPtr)[bufferLength - 1] = '\0'; // Null-terminate
      if (stringLengthPtr) {
        *stringLengthPtr = static_cast<SQLSMALLINT>(std::strlen("DefaultConnection"));
      }
      logMessage("SQLGetDiagField returning SQL_DIAG_CONNECTION_NAME = DefaultConnection");
    }
    break;

  case SQL_DIAG_SERVER_NAME:
    if (diagInfoPtr && bufferLength > 0) {
      std::snprintf(static_cast<char*>(diagInfoPtr), bufferLength, "%s", "ApacheIoTDBServer");
      static_cast<char*>(diagInfoPtr)[bufferLength - 1] = '\0'; // Null-terminate
      if (stringLengthPtr) {
        *stringLengthPtr = static_cast<SQLSMALLINT>(std::strlen("ApacheIoTDBServer"));
      }
      logMessage("SQLGetDiagField returning SQL_DIAG_SERVER_NAME = ApacheIoTDBServer");
    }
    break;

  default:
    logMessage("SQLGetDiagField received unsupported DiagIdentifier, returning SQL_ERROR\n");
    return SQL_ERROR;
  }

  // Log exit from SQLGetDiagField
  logMessage("Exiting SQLGetDiagField\n");
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT handleType, SQLHANDLE handle, SQLSMALLINT recNumber,
                                SQLCHAR* sqlStatePtr, SQLINTEGER* nativeErrorPtr,
                                SQLCHAR* messageTextPtr, SQLSMALLINT bufferLength,
                                SQLSMALLINT* textLengthPtr) {
  logMessage("Entering SQLGetDiagRec");
  std::stringstream logStream;
  logStream << "Parameters: "
            << "handleType = " << handleType << ", handle = " << handleToString(handle)
            << ", recNumber = " << recNumber
            << ", sqlStatePtr = " << (sqlStatePtr ? sqlCharToString(sqlStatePtr) : "NULL")
            << ", nativeErrorPtr = " << (nativeErrorPtr ? *nativeErrorPtr : 0)
            << ", messageTextPtr = " << (messageTextPtr ? sqlCharToString(messageTextPtr) : "NULL")
            << ", bufferLength = " << bufferLength
            << ", textLengthPtr = " << (textLengthPtr ? *textLengthPtr : 0);
  logMessage(logStream.str());

  auto* odbcHandle = static_cast<ODBCHandle*>(handle);
  std::string sqlState, message;
  int nativeError;

  const auto diagnostic = odbcHandle->getDiagnostic(recNumber, sqlState, message, nativeError);
  if (!diagnostic) {
    logMessage("Exiting SQLGetDiagRec: Couldn't find diagnostic information\n");
    return SQL_NO_DATA;
  }

  if (sqlStatePtr) {
    logMessage("SQLGetDiagRec returning sqlState = " + std::string(sqlState));
    std::snprintf(reinterpret_cast<char*>(sqlStatePtr), 6, "%s", sqlState.c_str());
  }
  if (nativeErrorPtr) {
    logMessage("SQLGetDiagRec returning nativeError = " + std::to_string(nativeError));
    *nativeErrorPtr = nativeError;
  }
  if (messageTextPtr) {
    logMessage("SQLGetDiagRec returning messageText = " + std::string(message));
    std::snprintf(reinterpret_cast<char*>(messageTextPtr), bufferLength, "%s", message.c_str());
  }
  if (textLengthPtr) {
    *textLengthPtr = static_cast<SQLSMALLINT>(message.length());
  }

  logMessage("Exiting SQLGetDiagRec\n");
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV environmentHandle, SQLINTEGER attribute, SQLPOINTER value,
                                SQLINTEGER bufferLength, SQLINTEGER* stringLength) {
  if (!environmentHandle) {
    logMessage(nullptr, "SQLGetEnvAttr: Invalid environment handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto env = static_cast<EnvironmentHandle*>(environmentHandle);
  logMessage(nullptr, "SQLGetEnvAttr is not implemented", LOG_LEVEL_ERROR);
  if (env) {
    env->addDiagnostic("IM001", "SQLGetEnvAttr Function not implemented");
  }
  return SQL_ERROR;
}

/*
See the following link for data type mapping:
https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/c-data-types?view=sql-server-ver16

SQL_C_USHORT = SQLUSMALLINT = unsigned short int (default)
SQL_C_ULONG = SQLUINTEGER = unsigned long int
SQL_C_SSHORT = SQLSMALLINT = short int
SQL_C_SLONG = SQLINTEGER = long int
*/
void SQLGetInfoSetNumeric(ConnectionHandle* cnct, std::string name, SQLUINTEGER value,
                          SQLPOINTER infoValuePtr, SQLSMALLINT valueType,
                          SQLSMALLINT* stringLengthPtr) {
  logMessage(cnct, "SQLGetInfoSetNumeric: name = " + name, LOG_LEVEL_TRACE);
  if (!infoValuePtr) {
    logMessage(cnct,
               "SQLGetInfoSetString: infoValuePtr = nullptr. Application is probing for required "
               "buffer size.",
               LOG_LEVEL_DEBUG);
    if (stringLengthPtr) {
      // Return required buffer size in bytes for the numeric type
      switch (valueType) {
      case SQL_C_USHORT:
        *stringLengthPtr = sizeof(SQLUSMALLINT);
        break;
      case SQL_C_ULONG:
        *stringLengthPtr = sizeof(SQLUINTEGER);
        break;
      case SQL_C_SSHORT:
        *stringLengthPtr = sizeof(SQLSMALLINT);
        break;
      case SQL_C_SLONG:
        *stringLengthPtr = sizeof(SQLINTEGER);
        break;
      default:
        logMessage(cnct, "SQLGetInfoSetNumeric: Unsupported valueType during probing",
                   LOG_LEVEL_ERROR);
        *stringLengthPtr = 0;
      }
    }
    return;
  }

  switch (valueType) {
  case SQL_C_USHORT:
    *static_cast<SQLUSMALLINT*>(infoValuePtr) = static_cast<SQLUSMALLINT>(value);
    break;

  case SQL_C_ULONG:
    *static_cast<SQLUINTEGER*>(infoValuePtr) = value;
    break;

  case SQL_C_SSHORT:
    *static_cast<SQLSMALLINT*>(infoValuePtr) = static_cast<SQLSMALLINT>(value);
    break;

  case SQL_C_SLONG:
    *static_cast<SQLINTEGER*>(infoValuePtr) = static_cast<SQLINTEGER>(value);
    break;

  default:
    logMessage(cnct, "SQLGetInfoSetNumeric: Invalid valueType\n", LOG_LEVEL_TRACE);
  }

  if (stringLengthPtr) {
    switch (valueType) {
    case SQL_C_USHORT:
      *stringLengthPtr = sizeof(SQLUSMALLINT);
      break;
    case SQL_C_ULONG:
      *stringLengthPtr = sizeof(SQLUINTEGER);
      break;
    case SQL_C_SSHORT:
      *stringLengthPtr = sizeof(SQLSMALLINT);
      break;
    case SQL_C_SLONG:
      *stringLengthPtr = sizeof(SQLINTEGER);
      break;
    default:
      *stringLengthPtr = 0;
    }
  }

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::ostringstream oss;
    oss << "SQLGetInfoSetNumeric returning " << name << " = " << value;
    logMessage(cnct, oss.str(), LOG_LEVEL_TRACE);
  }
}

void SQLGetInfoSetString(ConnectionHandle* cnct, std::string name, std::string value,
                         SQLPOINTER infoValuePtr, SQLSMALLINT bufferLength,
                         SQLSMALLINT* stringLengthPtr) {
  logMessage(cnct, "SQLGetInfoSetString: name = " + name, LOG_LEVEL_TRACE);

  if (!infoValuePtr) {
    logMessage(cnct,
               "SQLGetInfoSetString: infoValuePtr = nullptr. Application is probing for required "
               "buffer size.",
               LOG_LEVEL_DEBUG);
    if (stringLengthPtr) {
      *stringLengthPtr = static_cast<SQLSMALLINT>(value.size() + 1);
    }
    return;
  }

  if (bufferLength <= 0) {
    logMessage(cnct, "SQLGetInfoSetString: bufferLength <= 0. Exiting.", LOG_LEVEL_ERROR);
    return;
  }

  size_t max_copy = std::min(value.size(), static_cast<size_t>(bufferLength - 1));
  std::strncpy(static_cast<char*>(infoValuePtr), value.data(), max_copy);
  static_cast<char*>(infoValuePtr)[max_copy] = '\0';

  if (stringLengthPtr) {
    // return the actual length of answer, not the copied length.
    *stringLengthPtr = static_cast<SQLSMALLINT>(value.size());
  }

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::ostringstream oss;
    oss << "SQLGetInfoSetString returning " << name << " = " << value;
    logMessage(cnct, oss.str(), LOG_LEVEL_TRACE);
  }
}

std::string getServerVersion(SQLHDBC connectionHandle) {
  ConnectionHandle* cnct = static_cast<ConnectionHandle*>(connectionHandle);
  logMessage(cnct, "getServerVersion: Entering.", LOG_LEVEL_TRACE);

  // Create a temporary statement handle for the query
  StatementHandle* stmt = new StatementHandle(cnct);
  SQLRETURN ret = IoTDB_ExecDirect(stmt, "show version");

  if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
    logMessage(cnct, "getServerVersion: Failed to execute show version query", LOG_LEVEL_ERROR);
    delete stmt;
    return "Unknown";
  }

  if (stmt->resultSetPtr == nullptr || stmt->resultSetPtr->isEmpty()) {
    logMessage(cnct, "getServerVersion: No result set available", LOG_LEVEL_ERROR);
    delete stmt;
    return "Unknown";
  }

  // Get the version string from the first cell
  std::string version = stmt->resultSetPtr->getValue(0, 0).toString();
  if (version.empty()) {
    logMessage(cnct, "getServerVersion: Empty version string", LOG_LEVEL_WARN);
    version = "Unknown";
  }

  logMessage(cnct, "getServerVersion: Retrieved version: " + version, LOG_LEVEL_DEBUG);

  // Clean up
  delete stmt;

  logMessage(cnct, "getServerVersion: Exiting.", LOG_LEVEL_TRACE);
  return version;
}

std::string formatVersion(const std::string& ver) {
  std::istringstream iss(ver);
  std::string token;
  std::vector<std::string> parts;
  while (std::getline(iss, token, '.')) {
    parts.push_back(token);
  }
  std::ostringstream oss;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0)
      oss << ".";
    oss << std::setw(2) << std::setfill('0') << std::stoi(parts[i]);
  }
  return oss.str();
}

SQLRETURN SQL_API SQLGetInfo(SQLHDBC connectionHandle, SQLUSMALLINT infoType,
                             SQLPOINTER infoValuePtr, SQLSMALLINT bufferLength,
                             SQLSMALLINT* stringLengthPtr) {
  /*
    About this function:
    https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetinfo-function?view=sql-server-ver16
    */
  ConnectionHandle* cnct = static_cast<ConnectionHandle*>(connectionHandle);
  logMessage(cnct, "Entering SQLGetInfo", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "Parameters: "
              << "connectionHandle = " << handleToString(connectionHandle)
              << ", infoValuePtr = " << infoValuePtr << ", infoType = " << infoType
              << ", bufferLength = " << bufferLength << ", stringLengthPtr = " << stringLengthPtr;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  SQLUSMALLINT value = 0;

  if (!connectionHandle) {
    logMessage(cnct, "SQLGetInfo: Invalid connectionHandle", LOG_LEVEL_ERROR);
    return SQL_ERROR;
  }

  switch (infoType) {
  case SQL_DRIVER_NAME: // 6 sqlext.h
    SQLGetInfoSetString(cnct, "SQL_DRIVER_NAME", "Apache IoTDB Driver", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_DRIVER_ODBC_VER: // 77 sqlext.h
    SQLGetInfoSetString(cnct, "SQL_DRIVER_ODBC_VER", "03.80", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_DRIVER_VER: // 7 sqlext.h
    SQLGetInfoSetString(cnct, "SQL_DRIVER_VER", DRIVER_VERSION, infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_DATABASE_NAME: // 16 sqlext.h
    SQLGetInfoSetString(cnct, "SQL_DATABASE_NAME", cnct->database, infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_ACTIVE_CONNECTIONS: // 0 sqlext.h (Redefinition of SQL_MAX_DRIVER_CONNECTIONS from sql.h)
    // not able to get this value from IoTDB configure. setting it as 10
    SQLGetInfoSetNumeric(cnct, "SQL_ACTIVE_CONNECTIONS", 10, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_MAX_TABLE_NAME_LEN: // 35 sql.h
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_TABLE_NAME_LEN", 64, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr); // Example value
    break;

  case SQL_ACTIVE_STATEMENTS: // 1 sqlext.h (Redefinition of SQL_MAX_CONCURRENT_ACTIVITIES from swl.h)
    SQLGetInfoSetNumeric(cnct, "SQL_ACTIVE_STATEMENTS", 10, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr); // Assume no active statements initially
    break;

// iODBC only seems to support ODBC 3.0
#ifdef SQL_OV_ODBC3_80
  case SQL_ASYNC_DBC_FUNCTIONS: // 10023 sqlext.h
    SQLGetInfoSetNumeric(cnct, "SQL_ASYNC_DBC_FUNCTIONS", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    // Most drivers do not support this
    break;

  case SQL_ASYNC_NOTIFICATION: // 10025 sqlext.h
    SQLGetInfoSetNumeric(cnct, "SQL_ASYNC_NOTIFICATION", SQL_ASYNC_NOTIFICATION_NOT_CAPABLE,
                         infoValuePtr, SQL_C_USHORT, stringLengthPtr);
    // Default for simpler drivers
    break;
#endif

  case SQL_CURSOR_COMMIT_BEHAVIOR: // 23
    SQLGetInfoSetNumeric(cnct, "SQL_CURSOR_COMMIT_BEHAVIOR", SQL_CB_CLOSE, infoValuePtr,
                         SQL_C_USHORT, stringLengthPtr);
    // Specifies what happens to cursors on a commit.
    // Possible values:
    // SQL_CB_CLOSE: Cursors are closed on commit.
    // SQL_CB_DELETE: Cursors are closed and rows are deleted on commit.
    // SQL_CB_PRESERVE: Cursors remain open on commit.
    break;

  case SQL_CURSOR_ROLLBACK_BEHAVIOR: // 24
    SQLGetInfoSetNumeric(cnct, "SQL_CURSOR_ROLLBACK_BEHAVIOR", SQL_CB_CLOSE, infoValuePtr,
                         SQL_C_USHORT, stringLengthPtr);
    // Specifies what happens to cursors on a rollback.
    // Possible values:
    // SQL_CB_CLOSE: Cursors are closed on rollback.
    // SQL_CB_DELETE: Cursors are closed and rows are deleted on rollback.
    // SQL_CB_PRESERVE: Cursors remain open on rollback.
    break;

  case SQL_GETDATA_EXTENSIONS: // 81
    SQLGetInfoSetNumeric(cnct, "SQL_GETDATA_EXTENSIONS", SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER,
                         infoValuePtr, SQL_C_USHORT, stringLengthPtr);
    // Indicates which `SQLGetData` features are supported.
    // Possible values (bitmask):
    // SQL_GD_BLOCK: Allows block fetch with SQLGetData.
    // SQL_GD_BOUND: Allows data to be retrieved to bound columns and buffers.
    // SQL_GD_ANY_COLUMN: Allows retrieval of data from any column.
    // SQL_GD_ANY_ORDER: Allows retrieval of data in any order of columns.
    break;

  case SQL_DTC_TRANSITION_COST: // 1750
    SQLGetInfoSetNumeric(cnct, "SQL_DTC_TRANSITION_COST", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    // Cost (in milliseconds) for a Distributed Transaction Coordinator (DTC) transaction transition.
    // This value should be adjusted based on the driver and system's transaction coordination cost.
    break;

  case SQL_DESCRIBE_PARAMETER: // 10002
    SQLGetInfoSetString(cnct, "SQL_DESCRIBE_PARAMETER", "N", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_DBMS_NAME: // 17
    // Return the name of the data management system. Returned Apache IoTDB
    SQLGetInfoSetString(cnct, "SQL_DBMS_NAME", "Apache IoTDB", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_DBMS_VER: // 18
  {
    // Return the current database version. TODO: Need to query to get the actual version.
    // Now set to 1.3.3 for quick PowerBI availability verification
    std::string versionStr = getServerVersion(connectionHandle);
    versionStr = formatVersion(versionStr);
    SQLGetInfoSetString(cnct, "SQL_DBMS_VER", versionStr, infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;
  }

  case SQL_IDENTIFIER_QUOTE_CHAR: // 29
    // Identifier quote character. For IoTDB tree model this character is backtick (`), for table model it's "
    if (cnct->isTableModel) {
      SQLGetInfoSetString(cnct, "SQL_IDENTIFIER_QUOTE_CHAR", "\"", infoValuePtr, bufferLength,
                          stringLengthPtr);
    } else {
      SQLGetInfoSetString(cnct, "SQL_IDENTIFIER_QUOTE_CHAR", " ", infoValuePtr, bufferLength,
                          stringLengthPtr);
    }
    break;

  case SQL_CATALOG_NAME: //10003
                         /*
    If the server supports catalog names, returns "Y"; if not, returns "N".
    Drivers conforming to SQL-92 Full level standard will always return "Y".
     */
    SQLGetInfoSetString(cnct, "SQL_CATALOG_NAME", "Y", infoValuePtr, bufferLength, stringLengthPtr);
    break;

  case SQL_SCHEMA_USAGE: // 91
    // TODO: Verify if this return value is correct: Need to validate if using database as catalog is reasonable, mainly depends on whether ODBC can accept structure without SCHEMA.
    SQLGetInfoSetNumeric(cnct, "SQL_SCHEMA_USAGE", 0, infoValuePtr, SQL_C_USHORT, stringLengthPtr);
    break;
    /*
        This attribute is a byte mask.
        SQL_SU_DML_STATEMENTS mode can be used for all Data Manipulation Language (DML) statements:
        • SELECT, INSERT, UPDATE, DELETE
        • If supported, also includes SELECT FOR UPDATE and positioned update/delete statements.

        SQL_SU_PROCEDURE_INVOCATION mode can be used for ODBC stored procedure call statements (such as {CALL schema.proc_name}).

        SQL_SU_TABLE_DEFINITION mode can be used for all table definition statements:
        • CREATE TABLE, CREATE VIEW
        • ALTER TABLE, DROP TABLE, DROP VIEW.

        SQL_SU_INDEX_DEFINITION mode can be used for all index definition statements:
        • CREATE INDEX, DROP INDEX.

        SQL_SU_PRIVILEGE_DEFINITION mode can be used for all privilege definition statements:
        • GRANT, REVOKE.

         */

  case SQL_CATALOG_USAGE: // 92
    // Available masks are the same as SQL_SCHEMA_USAGE, but oriented towards catalog.
    // TODO: Verify if this return value is correct
    value = SQL_SU_DML_STATEMENTS | SQL_SU_TABLE_DEFINITION;
    SQLGetInfoSetNumeric(cnct, "SQL_CATALOG_USAGE", value, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_CATALOG_NAME_SEPARATOR: // 41
    // Returns the character or string that the data source uses to separate catalog names from subsequent or preceding qualified name elements.
    SQLGetInfoSetString(cnct, "SQL_CATALOG_NAME_SEPARATOR", ".", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_CATALOG_LOCATION: // 114
    // This value indicates the position of the catalog name in the full table name:
    // SQL_CL_START Catalog name is at the beginning of the table name (like file system path style)
    SQLGetInfoSetNumeric(cnct, "SQL_CATALOG_LOCATION", SQL_CL_START, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_SQL_CONFORMANCE:
    // TODO: Not sure if this is correct
    SQLGetInfoSetNumeric(cnct, "SQL_SQL_CONFORMANCE", SQL_SC_SQL92_ENTRY, infoValuePtr,
                         SQL_C_USHORT, stringLengthPtr);
    break;
    /*
    This value is used to identify the SQL standard level supported by the driver, mainly including the following four compliance levels:

        Return Value	Description
        SQL_SC_SQL92_ENTRY	Conforms to SQL-92 Entry Level standard
        SQL_SC_SQL92_INTERMEDIATE	Conforms to SQL-92 Intermediate Level standard
        SQL_SC_SQL92_FULL	Conforms to SQL-92 Full Level standard
        SQL_SC_FIPS127_2_TRANSITIONAL	Conforms to FIPS 127-2 Transitional Level standard
    */

  case SQL_MAX_COLUMNS_IN_ORDER_BY: //99
                                    // TODO: Not sure if this is correct
    // This value specifies the maximum number of columns allowed in ORDER BY clause
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_COLUMNS_IN_ORDER_BY", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_MAX_IDENTIFIER_LEN: // 10005
                               // TODO: Not sure if this is correct
    // Maximum identifier length attribute
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_IDENTIFIER_LEN", 64, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_MAX_COLUMNS_IN_GROUP_BY: // 97
                                    // TODO: Not sure if this is correct
    // This value specifies the maximum number of columns allowed in a GROUP BY clause
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_COLUMNS_IN_GROUP_BY", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_MAX_COLUMNS_IN_SELECT: // 100
                                  // TODO: Not sure if this is correct
    // This attribute specifies the maximum number of columns that can be returned in a SELECT statement
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_COLUMNS_IN_SELECT", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_ORDER_BY_COLUMNS_IN_SELECT: // 90
    // "Y" if columns in ORDER BY clause must be in select list; otherwise "N".
    SQLGetInfoSetString(cnct, "SQL_ORDER_BY_COLUMNS_IN_SELECT", "N", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_STRING_FUNCTIONS: // 50
    // Scalar string functions supported by the driver and associated data source
    value = SQL_FN_STR_LENGTH;
    SQLGetInfoSetNumeric(cnct, "SQL_STRING_FUNCTIONS", value, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_AGGREGATE_FUNCTIONS: // 169
                                // TODO: Not sure if this is correct
    // Identifies the types of aggregate functions supported by the driver
    SQLGetInfoSetNumeric(cnct, "SQL_AGGREGATE_FUNCTIONS", SQL_AF_ALL, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_SQL92_PREDICATES: // 160
    // TODO: Not sure if this is correct. Temporarily added all elements from SQL-92 Entry Level standard.
    // Predicates supported in SELECT statements
    value = SQL_SP_BETWEEN | SQL_SP_COMPARISON | SQL_SP_EXISTS | SQL_SP_IN | SQL_SP_ISNOTNULL |
            SQL_SP_ISNULL | SQL_SP_LIKE | SQL_SP_UNIQUE | SQL_SP_QUANTIFIED_COMPARISON;
    SQLGetInfoSetNumeric(cnct, "SQL_AGGREGATE_FUNCTIONS", value, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_SQL92_RELATIONAL_JOIN_OPERATORS: // 161
    // Relational join operators supported in SELECT statements
    value = SQL_SRJO_CROSS_JOIN | SQL_SRJO_INNER_JOIN | SQL_SRJO_LEFT_OUTER_JOIN |
            SQL_SRJO_RIGHT_OUTER_JOIN;
    SQLGetInfoSetNumeric(cnct, "SQL_SQL92_RELATIONAL_JOIN_OPERATORS", value, infoValuePtr,
                         SQL_C_USHORT, stringLengthPtr);
    break;

  case SQL_SQL92_VALUE_EXPRESSIONS: // 165
    value = SQL_SVE_CASE | SQL_SVE_COALESCE;
    SQLGetInfoSetNumeric(cnct, "SQL_SQL92_VALUE_EXPRESSIONS", value, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_COLUMN_ALIAS: // 87
    SQLGetInfoSetString(cnct, "SQL_COLUMN_ALIAS", "Y", infoValuePtr, bufferLength, stringLengthPtr);
    break;

  case SQL_GROUP_BY: // 88
                     // TODO: Not sure if this is correct。
    value = SQL_GB_GROUP_BY_CONTAINS_SELECT;
    SQLGetInfoSetNumeric(cnct, "SQL_GROUP_BY", value, infoValuePtr, SQL_C_USHORT, stringLengthPtr);
    break;

  case SQL_NUMERIC_FUNCTIONS: // 49
    value = SQL_FN_NUM_ABS | SQL_FN_NUM_ACOS | SQL_FN_NUM_ASIN | SQL_FN_NUM_ATAN |
            SQL_FN_NUM_CEILING | SQL_FN_NUM_COS | SQL_FN_NUM_DEGREES | SQL_FN_NUM_EXP |
            SQL_FN_NUM_FLOOR | SQL_FN_NUM_LOG | SQL_FN_NUM_LOG10 | SQL_FN_NUM_PI |
            SQL_FN_NUM_POWER | SQL_FN_NUM_RADIANS | SQL_FN_NUM_ROUND | SQL_FN_NUM_SIGN |
            SQL_FN_NUM_SIN | SQL_FN_NUM_SQRT | SQL_FN_NUM_TAN;
    SQLGetInfoSetNumeric(cnct, "SQL_NUMERIC_FUNCTIONS", value, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_TIMEDATE_FUNCTIONS: // 52
                               // TODO: Not sure if this is correct。
    value = SQL_FN_TD_CURRENT_TIMESTAMP | SQL_FN_TD_NOW;
    SQLGetInfoSetNumeric(cnct, "SQL_TIMEDATE_FUNCTIONS", value, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_SYSTEM_FUNCTIONS: // 51
                             // TODO: Not sure if this is correct。
    SQLGetInfoSetNumeric(cnct, "SQL_SYSTEM_FUNCTIONS", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_TIMEDATE_ADD_INTERVALS:
    // TODO: Not sure if this is correct. It seems IoTDB's time calculation method is completely different from SQL standard.
    // This attribute identifies the time interval types supported by the driver and data source through bitmask, used for TIMESTAMPADD scalar function in time arithmetic operations.

    SQLGetInfoSetNumeric(cnct, "SQL_TIMEDATE_ADD_INTERVALS", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_TIMEDATE_DIFF_INTERVALS:
    // TODO: Not sure if this is correct. It seems IoTDB's time calculation method is completely different from SQL standard.
    // This attribute identifies the timestamp intervals supported by the driver and associated data source through bitmask for the TIMESTAMPDIFF scalar function.
    SQLGetInfoSetNumeric(cnct, "SQL_TIMEDATE_DIFF_INTERVALS", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_CONCAT_NULL_BEHAVIOR:
    // Indicates how the data source handles concatenation of NULL valued character data type columns with non-NULL valued character data type columns.
    // SQL_CB_NULL = Result is NULL value.
    // SQL_CB_NON_NULL = Result is concatenation of non-NULL valued column or columns.
    // Drivers conforming to SQL-92 Entry Level standard will always return SQL_CB_NULL.
    SQLGetInfoSetNumeric(cnct, "SQL_CONCAT_NULL_BEHAVIOR", SQL_CB_NULL, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_CATALOG_TERM:
    // A character string that gives the name that the data source vendor uses for catalogs, such as "database" or "directory". This string can be upper case, lower case, or mixed case.
    SQLGetInfoSetString(cnct, "SQL_CATALOG_TERM", "database", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_SCHEMA_TERM:
    // A character string that gives the name that the data source vendor uses for schemas, which can be upper case, lower case, or mixed case.
    // Returns an empty string if the data source does not support schemas.
    SQLGetInfoSetString(cnct, "SQL_SCHEMA_TERM", "", infoValuePtr, bufferLength, stringLengthPtr);
    break;

  case SQL_ODBC_INTERFACE_CONFORMANCE:
    // A SQLUINTEGER value that indicates the level of ODBC 3.x interface conformance of the driver.
    /*
        SQL_OIC_CORE: The minimum level that all ODBC drivers should conform to. This level includes basic interface elements such as connection functions, functions for preparing and executing SQL statements, basic result set metadata functions, basic catalog functions, etc.
        SQL_OIC_LEVEL1: Includes core standard compliance level features, plus scrollable cursors, bookmarks, positioned updates and deletes, etc.
        SQL_OIC_LEVEL2: Includes Level 1 standard compliance level features, plus advanced features such as sensitive cursors; update, delete and refresh by bookmarks; stored procedure support; catalog functions for primary and foreign keys; multi-catalog support, etc.
     */
    SQLGetInfoSetNumeric(cnct, "SQL_ODBC_INTERFACE_CONFORMANCE", SQL_OIC_CORE, infoValuePtr,
                         SQL_C_USHORT, stringLengthPtr);
    break;

  case SQL_SEARCH_PATTERN_ESCAPE:
    /*
    A character string that specifies the escape character supported by the driver, which allows using pattern matching metacharacters underscore (_) and percent (%) as literal characters in search patterns.
    This escape character only applies to catalog function parameters that support search strings. If this string is empty, the driver does not support search pattern escape characters.
    */
    SQLGetInfoSetString(cnct, "SQL_SEARCH_PATTERN_ESCAPE", "\\", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_CONVERT_FUNCTIONS:
    /*
    A SQLUINTEGER bitmask that enumerates the scalar conversion functions supported by the driver and associated data source.
    The following bitmasks are used to determine which conversion functions are supported:
    SQL_FN_CVT_CAST
    SQL_FN_CVT_CONVERT
    */
    SQLGetInfoSetNumeric(cnct, "SQL_CONVERT_FUNCTIONS", SQL_FN_CVT_CAST, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_CONVERT_BIGINT:
  case SQL_CONVERT_BINARY:
  case SQL_CONVERT_BIT:
  case SQL_CONVERT_CHAR:
  case SQL_CONVERT_DATE:
  case SQL_CONVERT_DECIMAL:
  case SQL_CONVERT_DOUBLE:
  case SQL_CONVERT_FLOAT:
  case SQL_CONVERT_GUID:
  case SQL_CONVERT_INTEGER:
  case SQL_CONVERT_LONGVARCHAR:
  case SQL_CONVERT_NUMERIC:
  case SQL_CONVERT_REAL:
  case SQL_CONVERT_SMALLINT:
  case SQL_CONVERT_TIME:
  case SQL_CONVERT_TIMESTAMP:
  case SQL_CONVERT_TINYINT:
  case SQL_CONVERT_VARBINARY:
  case SQL_CONVERT_VARCHAR:
  case SQL_CONVERT_LONGVARBINARY:
    /*
    This series of attributes identifies the data type conversion capabilities supported by the data source through bitmasks, used for the CONVERT() scalar function. Each SQL_CONVERT_<type> corresponds to conversion support for a specific data type.

    The mask content is composed of multiple SQL_CVT_<type> ORed together.

    TODO: Not sure if this result is correct. Because IoTDB seems to not support the convert function
     */
    SQLGetInfoSetNumeric(cnct, "SQL_CONVERT_xxx", 0, infoValuePtr, SQL_C_USHORT, stringLengthPtr);
    break;

  case SQL_CONVERT_WCHAR:
  case SQL_CONVERT_WLONGVARCHAR:
  case SQL_CONVERT_WVARCHAR:
    /*
        These three seem to be in the same series as the above. The corresponding SQL_CVT_<type> also exists.
        But these three things cannot be found in the official documentation. These macros can only be found in header files.
     */
    SQLGetInfoSetNumeric(cnct, "SQL_CONVERT_xxx_wide", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_SPECIAL_CHARACTERS:
    /* A string containing all special characters that can be included in identifiers
    besides letters, numbers, and underscores. Returns empty string if none.
    */
    SQLGetInfoSetString(cnct, "SQL_SPECIAL_CHARACTERS", "", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case 180:
    // I haven't found what attribute 180 corresponds to, but bufferLength=4 indicates it's a numeric attribute
    SQLGetInfoSetNumeric(cnct, "Infotype 180", 0, infoValuePtr, SQL_C_USHORT, stringLengthPtr);
    break;

    /********
     * The following parameters have been deprecated in ODBC 3.0,
     * but Excel uses ODBC 2.0, so support is needed for Excel compatibility ********/

  case SQL_POS_OPERATIONS: // 79
    // Bitmask indicating the types of operations supported by the data source through the SQLSetPos function.
    // SQLSetPos not implemented yet so return 0. TODO
    SQLGetInfoSetNumeric(cnct, "SQL_POS_OPERATIONS", 0, infoValuePtr, SQL_C_SLONG, stringLengthPtr);
    break;

  case SQL_STATIC_SENSITIVITY: // 83
    // Indicates whether the application can detect changes made to static or keyset-driven cursors by SQLSetPos or positioned update/delete statements
    // SQLSetPos not implemented yet so return 0. TODO
    SQLGetInfoSetNumeric(cnct, "SQL_STATIC_SENSITIVITY", 0, infoValuePtr, SQL_C_SLONG,
                         stringLengthPtr);
    break;

  case SQL_LOCK_TYPES: // 78
    // Bitmask enumerating the lock types supported in the fLock argument of SQLSetPos function
    // SQLSetPos not implemented yet so return 0. TODO
    SQLGetInfoSetNumeric(cnct, "SQL_LOCK_TYPES", 0, infoValuePtr, SQL_C_SLONG, stringLengthPtr);
    break;

  case SQL_TXN_ISOLATION_OPTION: // 72。
    // Bitmask indicating the transaction isolation levels supported by the driver and data source. Each bit corresponds to a specific isolation level
    // IoTDB has no transaction concept
    SQLGetInfoSetNumeric(cnct, "SQL_TXN_ISOLATION_OPTION", 0, infoValuePtr, SQL_C_ULONG,
                         stringLengthPtr);
    break;

  case SQL_BOOKMARK_PERSISTENCE: // 82
    // Bitmask enumerating the operations in which bookmarks remain valid
    SQLGetInfoSetNumeric(cnct, "SQL_BOOKMARK_PERSISTENCE", 0, infoValuePtr, SQL_C_ULONG,
                         stringLengthPtr);
    break;

  case SQL_SCROLL_OPTIONS: // 44:
    // Bitmask enumerating the scroll options supported for scrollable cursors.
    SQLGetInfoSetNumeric(cnct, "SQL_SCROLL_OPTIONS", SQL_SO_FORWARD_ONLY | SQL_SO_STATIC,
                         infoValuePtr, SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_SCROLL_CONCURRENCY: // 43
    // Bitmask enumerating the concurrency control options supported by cursors
    SQLGetInfoSetNumeric(cnct, "SQL_SCROLL_CONCURRENCY", SQL_SCCO_READ_ONLY, infoValuePtr,
                         SQL_C_SLONG, stringLengthPtr);
    break;

  case SQL_DYNAMIC_CURSOR_ATTRIBUTES1:
    // Bitmask describing the attributes of dynamic cursors supported by the driver.
    SQLGetInfoSetNumeric(cnct, "SQL_DYNAMIC_CURSOR_ATTRIBUTES1",
                         SQL_CA1_NEXT | SQL_CA1_ABSOLUTE | SQL_CA1_RELATIVE, infoValuePtr,
                         SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_KEYSET_CURSOR_ATTRIBUTES1:
    // Bitmask describing the attributes of keyset-driven cursors supported by the driver. This bitmask contains the first set of attributes; see SQL_KEYSET_CURSOR_ATTRIBUTES2 for the second set.
    SQLGetInfoSetNumeric(cnct, "SQL_KEYSET_CURSOR_ATTRIBUTES1",
                         SQL_CA1_NEXT | SQL_CA1_ABSOLUTE | SQL_CA1_RELATIVE, infoValuePtr,
                         SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_STATIC_CURSOR_ATTRIBUTES1:
    // Bitmask describing the attributes of static cursors supported by the driver. This bitmask contains the first set of attributes; see SQL_STATIC_CURSOR_ATTRIBUTES2 for the second set.
    SQLGetInfoSetNumeric(cnct, "SQL_STATIC_CURSOR_ATTRIBUTES1",
                         SQL_CA1_NEXT | SQL_CA1_ABSOLUTE | SQL_CA1_RELATIVE, infoValuePtr,
                         SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1: // 146
      // Bitmask describing the attributes of forward-only cursors supported by the driver. This bitmask contains the first set of attributes; see SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2 for the second set.
    SQLGetInfoSetNumeric(cnct, "SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1",
                         SQL_CA1_NEXT | SQL_CA1_ABSOLUTE | SQL_CA1_RELATIVE, infoValuePtr,
                         SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_KEYSET_CURSOR_ATTRIBUTES2:
    // Bitmask describing the attributes of keyset-driven cursors supported by the driver. This bitmask contains the second set of attributes; see SQL_KEYSET_CURSOR_ATTRIBUTES1 for the first set.
    SQLGetInfoSetNumeric(cnct, "SQL_KEYSET_CURSOR_ATTRIBUTES2", SQL_CA2_READ_ONLY_CONCURRENCY,
                         infoValuePtr, SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_STATIC_CURSOR_ATTRIBUTES2:
    // Bitmask describing the attributes of static cursors supported by the driver. This bitmask contains the second set of attributes; see SQL_STATIC_CURSOR_ATTRIBUTES1 for the first set.
    SQLGetInfoSetNumeric(cnct, "SQL_KEYSET_CURSOR_ATTRIBUTES2", SQL_STATIC_CURSOR_ATTRIBUTES2,
                         infoValuePtr, SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_NEED_LONG_DATA_LEN:
    // A character string: "Y" if the data source needs to have the length of long data values (data type SQL_LONGVARCHAR, SQL_LONGVARBINARY, or a data source-specific long data type) before sending them to the data source; "N" if it does not.
    SQLGetInfoSetString(cnct, "SQL_NEED_LONG_DATA_LEN", "N", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_TXN_CAPABLE:
    // A SQLUSMALLINT value describing transaction support in the driver or data source
    SQLGetInfoSetNumeric(cnct, "SQL_TXN_CAPABLE", SQL_TC_NONE, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr); //iotdb has no transactions
    break;

  case SQL_NON_NULLABLE_COLUMNS:
    // A SQLUSMALLINT value specifying whether the data source supports NOT NULL constraints in columns
    SQLGetInfoSetNumeric(cnct, "SQL_NON_NULLABLE_COLUMNS", SQL_NNC_NON_NULL, infoValuePtr,
                         SQL_C_USHORT, stringLengthPtr); //iotdb has no transactions
    break;

  case SQL_DATA_SOURCE_NAME:
    // A character string representing the data source name used during connection. If the application called SQLConnect, this is the value of the szDSN parameter. If the application called SQLDriverConnect or SQLBrowseConnect, this is the value of the DSN keyword in the connection string passed to the driver. If the connection string does not contain a DSN keyword (for example, when it contains a DRIVER keyword), this is an empty string.
    // TODO: Needs to be modified
    SQLGetInfoSetString(cnct, "SQL_DATA_SOURCE_NAME", "Apache IoTDB Driver", infoValuePtr,
                        bufferLength, stringLengthPtr);
    break;

  case SQL_DATA_SOURCE_READ_ONLY:
    // A character string. "Y" if the data source is set to read-only mode; "N" otherwise.
    // This characteristic is only related to the data source itself, not the driver used to access the data source. A driver that supports read-write operations can be used with a read-only data source. If a driver is read-only, all its data sources must be read-only and must return SQL_DATA_SOURCE_READ_ONLY.
    SQLGetInfoSetString(cnct, "SQL_DATA_SOURCE_READ_ONLY", "Y", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_IDENTIFIER_CASE:
    // A SQLUSMALLINT value indicating case sensitivity.
    /*
    SQL_IC_UPPER     = Identifiers in SQL are case-insensitive and stored in uppercase in system catalogs.
    SQL_IC_LOWER     = Identifiers in SQL are case-insensitive and stored in lowercase in system catalogs.
    SQL_IC_SENSITIVE = Identifiers in SQL are case-sensitive and stored in mixed case in system catalogs.
    SQL_IC_MIXED     = Identifiers in SQL are case-insensitive and stored in mixed case in system catalogs.
    */
    SQLGetInfoSetNumeric(cnct, "SQL_IC_LOWER", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr); // Tested
    break;

  case SQL_MAX_INDEX_SIZE:
    // A SQLUINTEGER value specifying the maximum number of bytes allowed in the combined fields of an index. This value is set to zero if no limit is specified or the limit is unknown.
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_INDEX_SIZE", 0, infoValuePtr, SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_MAX_ROW_SIZE:
    /*
    A SQLUINTEGER value that specifies the maximum length of a single row in a table. If no limit is specified or the limit is unknown, this value is set to zero.
    Drivers conforming to FIPS Entry level standards will return at least 2,000. Drivers conforming to FIPS Intermediate level standards will return at least 8,000.
    */
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_ROW_SIZE", 0, infoValuePtr, SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_MAX_ROW_SIZE_INCLUDES_LONG:
    /*
    A character string: "Y" if the returned SQL_MAX_ROW_SIZE maximum row size information includes the length of all SQL_LONGVARCHAR and SQL_LONGVARBINARY type columns in the row; otherwise "N". */
    SQLGetInfoSetString(cnct, "SQL_MAX_ROW_SIZE_INCLUDES_LONG", "SQL_IC_MIXED", infoValuePtr,
                        bufferLength, stringLengthPtr);
    break;

  case SQL_MAX_TABLES_IN_SELECT:
    /*
    A SQLUSMALLINT value that specifies the maximum number of tables allowed in the FROM clause of a SELECT statement. If no limit is specified or the limit is unknown, this value is set to zero.
    Drivers conforming to FIPS Entry level standards will return at least 15. Drivers conforming to FIPS Intermediate level standards will return at least 50.
    */
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_ROW_SIZE", 0, infoValuePtr, SQL_C_USHORT, stringLengthPtr);
    break;

  case SQL_NULL_COLLATION:
    /*A SQLUSMALLINT value that specifies the sort position of NULL values in result sets.
    The options do not consider adjustable NULL value placement, so we can only choose to place them at the end based on IoTDB's default behavior.
    */
    SQLGetInfoSetNumeric(cnct, "SQL_NULL_COLLATION", SQL_NC_END, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_PROCEDURE_TERM:
    // A character string giving the name that the data source vendor uses for "procedure".
    // TODO: Not sure if IoTDB has the concept of procedure.
    SQLGetInfoSetString(cnct, "SQL_PROCEDURE_TERM", "PROCEDURE", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_QUOTED_IDENTIFIER_CASE:
    // A SQLUSMALLINT value describing the case sensitivity and storage of quoted identifiers in SQL
    // In SQL-92 standard they are case sensitive, but IoTDB seems not to be sensitive.
    SQLGetInfoSetNumeric(cnct, "SQL_ODBC_SQL_CONFORMANCE", SQL_IC_LOWER, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_ODBC_SQL_CONFORMANCE:
    /*A SQLSMALLINT value that indicates the level of SQL grammar supported by the driver. Specific SQL grammar conformance levels can be found in Appendix C: SQL Grammar of the ODBC specification. (Only found the minimal definition, the other two levels were not found)
    TODO: Not sure if this is correct.
    */
    SQLGetInfoSetNumeric(cnct, "SQL_ODBC_SQL_CONFORMANCE", SQL_OSC_MINIMUM, infoValuePtr,
                         SQL_C_SSHORT, stringLengthPtr);
    break;

  case SQL_INTEGRITY:
    // A string: "Y" if the data source supports Integrity Enhancement Facility; "N" if it does not.
    // Includes entity integrity, referential integrity, domain integrity, user-defined integrity.
    // TODO: Not sure if this is correct.
    SQLGetInfoSetString(cnct, "SQL_INTEGRITY", "N", infoValuePtr, bufferLength, stringLengthPtr);
    break;

  case SQL_SUBQUERIES:
    // A SQLUINTEGER bitmask enumerating the types of predicates supported in subqueries
    // Drivers conforming to SQL-92 Entry Level standard always return a bitmask with all these bits set.
    // However, IoTDB does not support correlated subqueries.

    SQLGetInfoSetNumeric(cnct, "SQL_SUBQUERIES",
                         SQL_SQ_COMPARISON | SQL_SQ_EXISTS | SQL_SQ_IN | SQL_SQ_QUANTIFIED,
                         infoValuePtr, SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_TABLE_TERM: // 45
    // The keyword in SQL that represents "table". Drivers conforming to SQL-92 Entry Level standard will always return "table".
    SQLGetInfoSetString(cnct, "SQL_TABLE_TERM", "table", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_USER_NAME:
    SQLGetInfoSetString(cnct, "SQL_USER_NAME", cnct->userName.c_str(), infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_MULT_RESULT_SETS:
    // A character string: "Y" if the data source supports multiple result sets; "N" if it does not.
    SQLGetInfoSetString(cnct, "SQL_MULT_RESULT_SETS", "N", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_SERVER_NAME:
    // A character string representing the server name as determined by the actual data source; this information is useful when connecting using SQLConnect, SQLDriverConnect, and SQLBrowseConnect when a data source name is used.
    // But does IoTDB have the concept of server name? TODO: Not sure if this is correct.
    SQLGetInfoSetString(cnct, "SQL_SERVER_NAME", "Apache IoTDB", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_ACCESSIBLE_PROCEDURES:
    // A character string: "Y" if the user can execute all procedures returned by SQLProcedures; "N" if there may be procedures the user cannot execute.
    SQLGetInfoSetString(cnct, "SQL_ACCESSIBLE_PROCEDURES", "N", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_ACCESSIBLE_TABLES: // 19
    // A character string: "Y" if the user has SELECT privileges on all tables returned by SQLTables; "N" if there may be tables the user cannot access.
    SQLGetInfoSetString(cnct, "SQL_ACCESSIBLE_TABLES", "N", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_OJ_CAPABILITIES: // 115
    // A SQLUINTEGER bitmask enumerating the types of outer joins supported by the driver and data source.
    SQLGetInfoSetNumeric(cnct, "SQL_OJ_CAPABILITIES",
                         SQL_OJ_FULL | SQL_OJ_NESTED | SQL_OJ_NOT_ORDERED, infoValuePtr,
                         SQL_C_ULONG, stringLengthPtr);
    break;

  case SQL_PROCEDURES: // 21
    // A character string: "Y" if the data source supports stored procedures and the driver supports ODBC procedure call syntax; "N" otherwise.
    SQLGetInfoSetString(cnct, "SQL_PROCEDURES", "N", infoValuePtr, bufferLength, stringLengthPtr);
    break;

  case SQL_LIKE_ESCAPE_CLAUSE: // 113
    // A character string: "Y" if the data source supports defining escape characters for percent (%) and underscore (_) characters in LIKE predicates and the driver supports ODBC-defined LIKE predicate escape character syntax; "N" otherwise.
    // IoTDB does not support LIKE.
    SQLGetInfoSetString(cnct, "SQL_LIKE_ESCAPE_CLAUSE", "N", infoValuePtr, bufferLength,
                        stringLengthPtr);
    break;

  case SQL_MAX_COLUMNS_IN_INDEX: // 98
    // A SQLUSMALLINT value specifying the maximum number of columns allowed in an index. This value is set to zero if no limit is specified or the limit is unknown.
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_COLUMNS_IN_INDEX", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_MAX_COLUMNS_IN_TABLE: // 101
    // A SQLUSMALLINT value specifying the maximum number of columns allowed in a table. This value is set to zero if no limit is specified or the limit is unknown.
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_COLUMNS_IN_TABLE", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_OUTER_JOINS: // 38
    // No detailed description found. It should be missing from Microsoft's official documentation. Based on similar references, it indicates whether outer joins are supported.
    SQLGetInfoSetString(cnct, "SQL_OUTER_JOINS", "Y", infoValuePtr, bufferLength, stringLengthPtr);
    break;

  case SQL_FILE_USAGE: // 84
    // A SQLUSMALLINT value that indicates how a single-tier driver directly handles files in the data source
    // The driver is not a single-tier driver.
    SQLGetInfoSetNumeric(cnct, "SQL_FILE_USAGE", SQL_FILE_NOT_SUPPORTED, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_DEFAULT_TXN_ISOLATION: // 26
    // A SQLUINTEGER value that indicates the default transaction isolation level supported by the driver or data source, returns zero if the data source does not support transactions.
    SQLGetInfoSetNumeric(cnct, "SQL_DEFAULT_TXN_ISOLATION", 0, infoValuePtr, SQL_C_ULONG,
                         stringLengthPtr);
    break;

  case SQL_MAX_OWNER_NAME_LEN:
    /*
    An SQLUSMALLINT value that specifies the maximum length of a schema name in the data source. If there is no maximum length or the length is unknown, this value is set to zero.

    An FIPS Entry level-conformant driver will return at least 18. An FIPS Intermediate level-conformant driver will return at least 128.

    This InfoType has been renamed for ODBC 3.0 from the ODBC 2.0 InfoType SQL_MAX_OWNER_NAME_LEN.

    there's no schema in IoTDB. the "Database" = "catalog". returning 0.
    */
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_OWNER_NAME_LEN", 0, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  case SQL_MAX_CATALOG_NAME_LEN:
    SQLGetInfoSetNumeric(cnct, "SQL_MAX_CATALOG_NAME_LEN", 64, infoValuePtr, SQL_C_USHORT,
                         stringLengthPtr);
    break;

  default:
    logMessage(cnct, "SQLGetInfo: received unsupported InfoType:" + std::to_string(infoType) + "\n",
               LOG_LEVEL_ERROR);
    return SQL_ERROR;
  }

  logMessage(cnct, "SQLGetInfo: Exiting\n", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT statementHandle, SQLINTEGER attribute, SQLPOINTER value,
                                 SQLINTEGER bufferLength, SQLINTEGER* stringLength) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLGetStmtAttr: Entering", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLGetStmtAttr parameters: "
              << "statementHandle = " << handleToString(statementHandle)
              << ", attribute = " << attribute << ", value = " << valueToString(value)
              << ", bufferLength = " << bufferLength << ", stringLength = " << stringLength;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  // In SQLAllocHandle we're allocating a NULL_Handle, so I disabled this.
  if (!statementHandle) {
    logMessage(cnct, "SQLGetStmtAttr: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  switch (attribute) {
  case SQL_ATTR_APP_ROW_DESC:
    // Return descriptor handle if applicable
    logMessage(cnct, "SQLGetStmtAttr: Processing SQL_ATTR_APP_ROW_DESC", LOG_LEVEL_DEBUG);
    if (value) {
      *static_cast<SQLHANDLE*>(value) =
          stmt->appRowDesc; // Replace with actual descriptor handle if available
      logMessage(cnct, "SQLGetStmtAttr: Returning application row descriptor handle",
                 LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_APP_PARAM_DESC:
    // Return descriptor handle if applicable
    logMessage(cnct, "SQLGetStmtAttr: Processing SQL_ATTR_APP_PARAM_DESC", LOG_LEVEL_DEBUG);
    if (value) {
      *static_cast<SQLHANDLE*>(value) =
          stmt->appRowDesc; // Replace with actual descriptor handle if available
      logMessage(cnct, "SQLGetStmtAttr: Returning application parameter descriptor handle",
                 LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_IMP_ROW_DESC:
    // Return descriptor handle if applicable
    logMessage(cnct, "SQLGetStmtAttr: Processing SQL_ATTR_IMP_ROW_DESC", LOG_LEVEL_DEBUG);
    if (value) {
      *static_cast<SQLHANDLE*>(value) =
          stmt->impRowDesc; // Replace with actual descriptor handle if available
      logMessage(cnct, "SQLGetStmtAttr: Returning implementation row descriptor handle",
                 LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_IMP_PARAM_DESC:
    // Return descriptor handle if applicable
    logMessage(cnct, "SQLGetStmtAttr: Processing SQL_ATTR_IMP_PARAM_DESC", LOG_LEVEL_DEBUG);
    if (value) {
      *static_cast<SQLHANDLE*>(value) =
          stmt->impRowDesc; // Replace with actual descriptor handle if available
      logMessage(cnct, "SQLGetStmtAttr: Returning implementation parameter descriptor handle",
                 LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_CURSOR_SCROLLABLE:
    logMessage(cnct, "SQLGetStmtAttr: Processing SQL_ATTR_CURSOR_SCROLLABLE", LOG_LEVEL_DEBUG);
    if (value) {
      *static_cast<SQLULEN*>(value) = SQL_NONSCROLLABLE; // Example: default to non-scrollable
      logMessage(cnct, "SQLGetStmtAttr: Cursor scrollable set to SQL_NONSCROLLABLE",
                 LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_CURSOR_SENSITIVITY:
    logMessage(cnct, "SQLGetStmtAttr: Processing SQL_ATTR_CURSOR_SENSITIVITY", LOG_LEVEL_DEBUG);
    if (value) {
      *static_cast<SQLULEN*>(value) = SQL_UNSPECIFIED; // Example: unspecified sensitivity
      logMessage(cnct, "SQLGetStmtAttr: Cursor sensitivity set to SQL_UNSPECIFIED",
                 LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_ROW_BIND_TYPE:
    logMessage(cnct, "SQLGetStmtAttr: Processing SQL_ATTR_ROW_BIND_TYPE", LOG_LEVEL_DEBUG);
    if (value) {
      *static_cast<SQLULEN*>(value) = SQL_BIND_BY_COLUMN; // Example: default to column binding
      logMessage(cnct, "SQLGetStmtAttr: Row bind type set to SQL_BIND_BY_COLUMN", LOG_LEVEL_DEBUG);
    }
    break;

  case SQL_ATTR_ROW_ARRAY_SIZE:
  case SQL_ATTR_ROWS_FETCHED_PTR:
  case SQL_ATTR_ROW_STATUS_PTR:
  case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
  case SQL_ATTR_PARAM_STATUS_PTR:
  case SQL_ATTR_PARAMS_PROCESSED_PTR:
    break;

  default:
    logMessage(cnct, "SQLGetStmtAttr: Unsupported attribute: " + std::to_string(attribute),
               LOG_LEVEL_WARN);
    stmt->addDiagnostic("HY092", "Invalid attribute/option identifier");
    return SQL_ERROR;
  }

  // Update StringLength if required
  if (stringLength) {
    *stringLength = 0; // Update with actual size if applicable
  }

  logMessage(cnct, "SQLGetStmtAttr: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetStmtOption(SQLHSTMT statementHandle, SQLUSMALLINT option,
                                   SQLPOINTER value) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLGetStmtOption: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLGetStmtOption is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLGetStmtOption Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLGetTypeInfo(SQLHSTMT statementHandle, SQLSMALLINT dataType) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  if (!statementHandle) {
    logMessage(cnct, "SQLGetTypeInfo: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  logMessage(cnct, "SQLGetTypeInfo: Entering", LOG_LEVEL_TRACE);
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLGetTypeInfo parameters:"
              << "statementHandle = " << handleToString(statementHandle)
              << ", dataType = " << dataType;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }
  // Create ODBCResultSet with type information
  if (!stmt->resultSetPtr) {
    stmt->AllocateSessionResultSet();
  }

  // Define the type information data
  std::vector<std::string> columnNames = {
      "TYPE_NAME",        "DATA_TYPE",          "COLUMN_SIZE",       "LITERAL_PREFIX",
      "LITERAL_SUFFIX",   "CREATE_PARAMS",      "NULLABLE",          "CASE_SENSITIVE",
      "SEARCHABLE",       "UNSIGNED_ATTRIBUTE", "FIXED_PREC_SCALE",  "AUTO_UNIQUE_VALUE",
      "LOCAL_TYPE_NAME",  "MINIMUM_SCALE",      "MAXIMUM_SCALE",     "SQL_DATA_TYPE",
      "SQL_DATETIME_SUB", "NUM_PREC_RADIX",     "INTERVAL_PRECISION"};

  std::vector<std::string> columnTypes = {
      "TEXT",  "INT32", "INT32", "TEXT",  "TEXT",  "TEXT",  "INT32", "INT32", "INT32", "INT32",
      "INT32", "INT32", "TEXT",  "INT32", "INT32", "INT32", "INT32", "INT32", "INT32"};

  ODBCField null = ODBCField::null();
  std::vector<std::vector<ODBCField>> typeData = {
      {"BOOLEAN", -7, 1, null, null, null, 1, 0, 2, null, 1, null, "BOOLEAN", null, null, -7, null,
       null, null},
      {"INT64", -5, 19, null, null, null, 1, 0, 2, 0, 1, 0, "INT64", 0, 0, -5, null, 10, null},
      {"BLOB", -4, 0, null, null, null, 1, 0, 1, null, 0, null, "BLOB", null, null, -4, null, null,
       null},
      {"TEXT", -1, 0, null, null, null, 1, 1, 1, null, 0, null, "TEXT", null, null, -1, null, null,
       null},
      {"INT32", 4, 10, null, null, null, 1, 0, 2, 0, 1, 0, "INT32", 0, 0, 4, null, 10, null},
      {"FLOAT", 7, 24, null, null, null, 1, 0, 2, 0, 0, 0, "FLOAT", null, null, 7, null, 2, null},
      {"DOUBLE", 8, 53, null, null, null, 1, 0, 2, 0, 0, 0, "DOUBLE", null, null, 8, null, 2, null},
      {"STRING", 12, 0, null, null, "length", 1, 1, 1, null, 0, null, "STRING", null, null, 12,
       null, null, null},
      {"DATE", 91, 10, null, null, null, 1, 0, 2, null, 1, null, "DATE", 0, 0, 9, 1, null, null},
      {"TIMESTAMP", 93, 23, null, null, null, 1, 0, 2, 1, 1, 0, "TIMESTAMP", 3, 3, 9, 3, null,
       null}};

  // Filter data if specific dataType is requested
  if (dataType != SQL_ALL_TYPES) {
    std::vector<std::vector<ODBCField>> filteredData;
    for (const auto& row : typeData) {
      if (row.size() > 1) {
        try {
          SQLSMALLINT rowDataType = 0;
          bool ret = row[1].toSShort(&rowDataType);
          if (ret && rowDataType == dataType) {
            filteredData.push_back(row);
          }
        } catch (const std::exception&) {
          // Skip rows that can't be parsed
          continue;
        }
      }
    }
    typeData = filteredData;
  }

  // Set up ODBCResultSet with the data
  stmt->resultSetPtr->columnNames = columnNames;
  stmt->resultSetPtr->columnTypes = columnTypes;
  stmt->resultSetPtr->data = typeData;
  stmt->resultSetPtr->numRows = typeData.size();
  stmt->resultSetPtr->numColumns = columnNames.size();
  stmt->resultSetPtr->isMetaData = true;

  logMessage(cnct,
             "SQLGetTypeInfo: Created ODBCResultSet with " + std::to_string(typeData.size()) +
                 " type records",
             LOG_LEVEL_DEBUG);
  stmt->resultSetPtr->outputTable();
  logMessage(cnct, "SQLGetTypeInfo: Exiting.\n", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT statementHandle, SQLSMALLINT* columnCount) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;

  logMessage(cnct, "SQLNumResultCols: Entering", LOG_LEVEL_TRACE);

  // Prepare log message for the parameters
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLNumResultCols parameters: "
              << "statementHandle = " << handleToString(statementHandle);
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  // In SQLAllocHandle we're allocating a NULL_Handle, so I disabled this.
  if (!statementHandle) {
    logMessage(cnct, "SQLNumResultCols: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  if (stmt->resultSetPtr == nullptr || stmt->resultSetPtr->isEmpty()) {
    logMessage(cnct, "SQLNumResultCols: Missing result set, returning 0", LOG_LEVEL_ERROR);
    *columnCount = 0;
    return SQL_SUCCESS;
  }

  if (!columnCount) {
    logMessage(cnct, "SQLNumResultCols: columnCount pointer is null", LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY009", "Invalid use of null pointer");
    return SQL_ERROR;
  }

  *columnCount = stmt->resultSetPtr->getNumColumns();
  logMessage(cnct, "SQLNumResultCols: Returning column count: " + std::to_string(*columnCount),
             LOG_LEVEL_DEBUG);

  logMessage(cnct, "SQLNumResultCols: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLParamData(SQLHSTMT statementHandle, SQLPOINTER* value) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLParamData: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLParamData is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLParamData Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLPrepare(SQLHSTMT statementHandle, SQLCHAR* statementText,
                             SQLINTEGER textLength) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLPrepare: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLPrepare is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLPrepare Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLPutData(SQLHSTMT statementHandle, SQLPOINTER data, SQLLEN strLen_or_Ind) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLPutData: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLPutData is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLPutData Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLRowCount(SQLHSTMT statementHandle, SQLLEN* rowCount) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLRowCount: Entering", LOG_LEVEL_TRACE);

  // Prepare log message for the parameters
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLRowCount parameters: "
              << "statementHandle = " << handleToString(statementHandle);
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  if (!statementHandle) {
    logMessage(cnct, "SQLRowCount: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  if (stmt->isQuery == false || stmt->resultSetPtr == nullptr || stmt->resultSetPtr->isEmpty()) {
    logMessage(cnct, "SQLRowCount: NonQuery statement or missing result set, returning -1",
               LOG_LEVEL_WARN);
    *rowCount = 0;
    return SQL_SUCCESS;
  }

  *rowCount = stmt->resultSetPtr->getNumRows();
  logMessage(cnct, "SQLRowCount: Returning row count: " + std::to_string(*rowCount),
             LOG_LEVEL_DEBUG);

  logMessage(cnct, "SQLRowCount: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetConnectAttr(const SQLHDBC connectionHandle, const SQLINTEGER attribute,
                                    SQLPOINTER value, const SQLINTEGER stringLength) {
  ConnectionHandle* cnct = static_cast<ConnectionHandle*>(connectionHandle);

  logMessage(cnct, "SQLSetConnectAttr: Entering", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLSetConnectAttr parameters: "
              << "connectionHandle = " << handleToString(connectionHandle)
              << ", attribute = " << attribute << ", value = " << valueToString(value)
              << ", stringLength = " << stringLength;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  if (cnct == nullptr) {
    logMessage("SQLSetConnectAttr: Invalid ConnectionHandle");
    return SQL_INVALID_HANDLE;
  }

  // Handle supported attributes
  switch (attribute) {
  case SQL_ATTR_AUTOCOMMIT:
    logMessage(cnct, "SQLSetConnectAttr: Processing SQL_ATTR_AUTOCOMMIT", LOG_LEVEL_DEBUG);
    if (value == reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON)) {
      cnct->autoCommit = true;
      logMessage(cnct, "SQLSetConnectAttr: Autocommit enabled", LOG_LEVEL_DEBUG);
    } else if (value == reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF)) {
      cnct->autoCommit = false;
      logMessage(cnct, "SQLSetConnectAttr: Autocommit disabled", LOG_LEVEL_DEBUG);
    } else {
      logMessage(cnct, "SQLSetConnectAttr: Invalid value for SQL_ATTR_AUTOCOMMIT", LOG_LEVEL_ERROR);
      cnct->addDiagnostic("HY024", "Invalid attribute value");
      return SQL_ERROR;
    }
    break;

  case SQL_ATTR_CONNECTION_TIMEOUT:
    logMessage(cnct, "SQLSetConnectAttr: Processing SQL_ATTR_CONNECTION_TIMEOUT", LOG_LEVEL_DEBUG);
    if (value) {
      const SQLUINTEGER timeout = *static_cast<SQLUINTEGER*>(value);
      cnct->timeoutConnection = timeout;
      logMessage(cnct,
                 "SQLSetConnectAttr: Connection timeout set to " + std::to_string(timeout) +
                     " seconds",
                 LOG_LEVEL_DEBUG);
    } else {
      logMessage(cnct, "SQLSetConnectAttr: Invalid value for SQL_ATTR_CONNECTION_TIMEOUT",
                 LOG_LEVEL_ERROR);
      cnct->addDiagnostic("HY024", "Invalid attribute value");
      return SQL_ERROR;
    }
    break;

  case SQL_ATTR_LOGIN_TIMEOUT:
    logMessage(cnct, "SQLSetConnectAttr: Processing SQL_ATTR_LOGIN_TIMEOUT", LOG_LEVEL_DEBUG);
    if (value) {
      const SQLUINTEGER timeout = static_cast<SQLUINTEGER>(reinterpret_cast<uintptr_t>(value));
      cnct->timeoutLogin = timeout;
      logMessage(cnct,
                 "SQLSetConnectAttr: Login timeout set to " + std::to_string(timeout) + " seconds",
                 LOG_LEVEL_DEBUG);
    } else {
      logMessage(cnct, "SQLSetConnectAttr: Invalid value for SQL_ATTR_LOGIN_TIMEOUT",
                 LOG_LEVEL_ERROR);
      cnct->addDiagnostic("HY024", "Invalid attribute value");
      return SQL_ERROR;
    }
    break;

  case SQL_ATTR_CURRENT_CATALOG: {
    logMessage(cnct, "SQLSetConnectAttr: Processing SQL_ATTR_CURRENT_CATALOG", LOG_LEVEL_DEBUG);
    if (!value) {
      logMessage(cnct, "SQLSetConnectAttr: NULL value for SQL_ATTR_CURRENT_CATALOG",
                 LOG_LEVEL_ERROR);
      cnct->addDiagnostic("HY024", "Invalid attribute value");
      return SQL_ERROR;
    }
    std::string databaseName(static_cast<const char*>(value), stringLength);
    if (databaseName.length() > 64) {
      logMessage(cnct, "SQLSetConnectAttr: Catalog name too long (max 64 characters)",
                 LOG_LEVEL_ERROR);
      cnct->addDiagnostic("HY090", "String data, right truncation");
      return SQL_ERROR;
    }
    bool isNameValid = true;
    for (size_t i = 0; i < databaseName.length(); i++) {
      char c = databaseName[i];
      if (!(isalnum(c) || c == '.')) {
        isNameValid = false;
        break;
      }
    }
    if (!isNameValid) {
      logMessage(cnct, "SQLSetConnectAttr: Invalid character in catalog name: " + databaseName,
                 LOG_LEVEL_ERROR);
      cnct->addDiagnostic("HY000", "Invalid catalog name format");
      return SQL_ERROR;
    }
    cnct->database = std::string(databaseName);
    if (cnct->isTableModel) {
      logMessage(cnct, "SQLSetConnectAttr: Setting current database: USE " + cnct->database,
                 LOG_LEVEL_DEBUG);
      SQLRETURN sqlreturn;
      SQLHANDLE statementHandle = nullptr;
      SQLAllocHandle(SQL_HANDLE_STMT, connectionHandle, &statementHandle);
      StatementHandle* stmt = (StatementHandle*)statementHandle;
      sqlreturn = IoTDB_ExecDirect(stmt, ("USE " + cnct->database).c_str());
      if (sqlreturn != SQL_SUCCESS && sqlreturn != SQL_SUCCESS_WITH_INFO) {
        logMessage(cnct, "SQLSetConnectAttr: Setting current catalog failed!", LOG_LEVEL_ERROR);
        return sqlreturn;
      }
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }
    logMessage(cnct, "SQLSetConnectAttr: Set current catalog (= database) to " + cnct->database,
               LOG_LEVEL_DEBUG);
    break;
  }

  default:
    logMessage(cnct,
               "SQLSetConnectAttr: Unsupported attribute " + std::to_string(attribute) +
                   ", returning SQL_ERROR",
               LOG_LEVEL_ERROR);
    cnct->addDiagnostic("HY092", "Invalid attribute/option identifier");
    return SQL_ERROR; // Return error for unsupported attributes
  }

  logMessage(cnct, "SQLSetConnectAttr: Exiting successfully\n", LOG_LEVEL_WARN);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetConnectOption(SQLHDBC connectionHandle, SQLUSMALLINT option,
                                      SQLULEN value) {
  if (!connectionHandle) {
    logMessage(nullptr, "SQLSetConnectOption: Invalid connection handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto cnct = static_cast<ConnectionHandle*>(connectionHandle);
  logMessage(cnct, "SQLSetConnectOption is not implemented", LOG_LEVEL_ERROR);
  if (cnct) {
    cnct->addDiagnostic("IM001", "SQLSetConnectOption Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLSetCursorName(SQLHSTMT statementHandle, SQLCHAR* cursorName,
                                   SQLSMALLINT nameLength) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLSetCursorName: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLSetCursorName is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLSetCursorName Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLSetDescField(SQLHDESC descriptorHandle, SQLSMALLINT recNumber,
                                  SQLSMALLINT fieldIdentifier, SQLPOINTER value,
                                  SQLINTEGER bufferLength) {
  if (!descriptorHandle) {
    logMessage(nullptr, "SQLSetDescField: Invalid descriptor handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  logMessage(nullptr, "SQLSetDescField is not implemented", LOG_LEVEL_ERROR);
  if (descriptorHandle) {
    auto odbcHandle = static_cast<ODBCHandle*>(descriptorHandle);
    odbcHandle->addDiagnostic("IM001", "SQLSetDescField Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLSetDescRec(SQLHDESC descriptorHandle, SQLSMALLINT recNumber, SQLSMALLINT type,
                                SQLSMALLINT subType, SQLLEN length, SQLSMALLINT precision,
                                SQLSMALLINT scale, SQLPOINTER data, SQLLEN* stringLength,
                                SQLLEN* indicator) {
  if (!descriptorHandle) {
    logMessage(nullptr, "SQLSetDescRec: Invalid descriptor handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  logMessage(nullptr, "SQLSetDescRec is not implemented", LOG_LEVEL_ERROR);
  if (descriptorHandle) {
    auto odbcHandle = static_cast<ODBCHandle*>(descriptorHandle);
    odbcHandle->addDiagnostic("IM001", "SQLSetDescRec Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLSetEnvAttr(const SQLHENV environmentHandle, const SQLINTEGER attribute,
                                SQLPOINTER value, const SQLINTEGER stringLength) {
  EnvironmentHandle* env = static_cast<EnvironmentHandle*>(environmentHandle);
  logMessage(nullptr, "SQLSetEnvAttr: Entering", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(nullptr, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLSetEnvAttr parameters: "
              << "environmentHandle = " << handleToString(environmentHandle)
              << ", attribute = " << attribute << ", valuePtr address = " << value
              << ", stringLength = " << stringLength;
    logMessage(nullptr, logStream.str(), LOG_LEVEL_TRACE);
  }

  switch (attribute) {

  // In this case the client is passing int it's version of ODBC API.
  // We use that to double-check if this is supported by this driver.
  // We save it in the env, as we might need to respond differently
  // based on the clients API version.
  case SQL_ATTR_ODBC_VERSION:
    logMessage(nullptr, "SQLSetEnvAttr: Processing SQL_ATTR_ODBC_VERSION", LOG_LEVEL_DEBUG);
    if (value) {
      const SQLINTEGER odbcVersion = static_cast<SQLINTEGER>(reinterpret_cast<uintptr_t>(value));
      // Check for supported ODBC versions
      // We currently support 3.0 and 3.8
      if (odbcVersion == SQL_OV_ODBC3) {
        // Store the version in your environment structure
        const auto env = static_cast<EnvironmentHandle*>(environmentHandle);
        env->odbcVersion = odbcVersion;
        logMessage(nullptr, "SQLSetEnvAttr: ODBC version set to 3.0", LOG_LEVEL_INFO);
        return SQL_SUCCESS;
      }
// iODBC only seems to support ODBC 3.0
#ifdef SQL_OV_ODBC3_80
      if (odbcVersion == SQL_OV_ODBC3_80) {
        // Store the version in your environment structure
        const auto env = static_cast<EnvironmentHandle*>(environmentHandle);
        env->odbcVersion = odbcVersion;
        logMessage(nullptr, "SQLSetEnvAttr: ODBC version set to 3.8", LOG_LEVEL_INFO);
        return SQL_SUCCESS;
      }
#endif
      // MS Excel only support ODBC 2.0
      if (odbcVersion == SQL_OV_ODBC2) {
        const auto env = static_cast<EnvironmentHandle*>(environmentHandle);
        env->odbcVersion = odbcVersion;
        logMessage(nullptr, "SQLSetEnvAttr: ODBC version set to 2.0", LOG_LEVEL_INFO);
        return SQL_SUCCESS;
      }

      logMessage(nullptr, "SQLSetEnvAttr: Empty value for ODBC version", LOG_LEVEL_ERROR);
      env->addDiagnostic("HY009", "Invalid use of null pointer");
      return SQL_ERROR;
    }
    logMessage("Empty Value\n");
    return SQL_ERROR;

  case SQL_ATTR_CONNECTION_POOLING:
    logMessage(nullptr, "SQLSetEnvAttr: Processing SQL_ATTR_CONNECTION_POOLING", LOG_LEVEL_DEBUG);
    logMessage(nullptr, "SQLSetEnvAttr: Connection pooling is not supported", LOG_LEVEL_INFO);
    return SQL_SUCCESS;

  default:
    logMessage(nullptr, "SQLSetEnvAttr: Unsupported attribute: " + std::to_string(attribute),
               LOG_LEVEL_WARN);
    env->addDiagnostic("HY092", "Invalid attribute/option identifier");
    return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLSetParam(SQLHSTMT statementHandle, SQLUSMALLINT parameterNumber,
                              SQLSMALLINT valueType, SQLSMALLINT parameterType,
                              SQLULEN lengthPrecision, SQLSMALLINT parameterScale,
                              SQLPOINTER parameterValue, SQLLEN* strLen_or_Ind) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLSetParam: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLSetParam is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLSetParam Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT statementHandle, SQLINTEGER attribute, SQLPOINTER value,
                                 SQLINTEGER stringLength) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;

  logMessage(cnct, "SQLSetStmtAttr: Entering", LOG_LEVEL_TRACE);

  // Only build parameter log message if TRACE level is enabled
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLSetStmtAttr parameters: "
              << "statementHandle = " << handleToString(statementHandle)
              << ", attribute = " << attribute << ", value = " << valueToString(value)
              << ", stringLength = " << stringLength;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  // In SQLSetStmtAttr we're allocating a NULL_Handle, so I disabled this.
  if (!statementHandle) {
    logMessage(cnct, "SQLSetStmtAttr: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  switch (attribute) {
  case SQL_QUERY_TIMEOUT:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_QUERY_TIMEOUT", LOG_LEVEL_DEBUG);
    // TODO: Implement query timeout functionality
    logMessage(cnct, "SQLSetStmtAttr: Query timeout setting is not yet implemented",
               LOG_LEVEL_WARN);
    break;

  case SQL_ATTR_APP_ROW_DESC:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_APP_ROW_DESC", LOG_LEVEL_DEBUG);
    // TODO: Implement application row descriptor functionality
    logMessage(cnct, "SQLSetStmtAttr: Application row descriptor setting is not yet implemented",
               LOG_LEVEL_WARN);
    break;

  case SQL_ATTR_APP_PARAM_DESC:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_APP_PARAM_DESC", LOG_LEVEL_DEBUG);
    // TODO: Implement application parameter descriptor functionality
    logMessage(cnct,
               "SQLSetStmtAttr: Application parameter descriptor setting is not yet implemented",
               LOG_LEVEL_WARN);
    break;

  case SQL_ATTR_ROWS_FETCHED_PTR: {
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_ROWS_FETCHED_PTR", LOG_LEVEL_DEBUG);
    if (value == nullptr) {
      // Power BI passes value==nullptr for unknown reasons.
      // We can't modify Power BI's code, so we ignore this error condition.
      logMessage(cnct, "SQLSetStmtAttr: value is nullptr (Power BI compatibility)",
                 LOG_LEVEL_DEBUG);
      break;
    }
    if (stringLength != sizeof(SQLULEN)) {
      logMessage(cnct, "SQLSetStmtAttr: Warning - stringLength != sizeof(SQLULEN)", LOG_LEVEL_WARN);
    }
    // Store the pointer to the rows fetched counter
    stmt->rowsFetchedPtr = static_cast<SQLULEN*>(value);
    logMessage(cnct, "SQLSetStmtAttr: Rows fetched pointer set", LOG_LEVEL_DEBUG);
    break;
  }

  case SQL_ATTR_CURSOR_TYPE:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_CURSOR_TYPE", LOG_LEVEL_DEBUG);
    // Validate if cursor type is valid
    if (value != (SQLPOINTER)SQL_CURSOR_FORWARD_ONLY && value != (SQLPOINTER)SQL_CURSOR_STATIC &&
        value != (SQLPOINTER)SQL_CURSOR_KEYSET_DRIVEN && value != (SQLPOINTER)SQL_CURSOR_DYNAMIC) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid cursor type specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid cursor type");
      return SQL_ERROR;
    }
    stmt->cursorType = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: Cursor type set successfully", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_CONCURRENCY:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_CONCURRENCY", LOG_LEVEL_DEBUG);
    // Validate if concurrency type is valid
    if (value != (SQLPOINTER)SQL_CONCUR_READ_ONLY && value != (SQLPOINTER)SQL_CONCUR_LOCK &&
        value != (SQLPOINTER)SQL_CONCUR_ROWVER && value != (SQLPOINTER)SQL_CONCUR_VALUES) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid concurrency type specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid concurrency type");
      return SQL_ERROR;
    }
    stmt->concurrency = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: Concurrency type set successfully", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_CURSOR_SCROLLABLE:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_CURSOR_SCROLLABLE", LOG_LEVEL_DEBUG);
    // Validate if scrollable
    if (value != (SQLPOINTER)SQL_NONSCROLLABLE && value != (SQLPOINTER)SQL_SCROLLABLE) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid scrollable option specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid scrollable option");
      return SQL_ERROR;
    }
    stmt->scrollable = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: Cursor scrollable setting applied", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_CURSOR_SENSITIVITY:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_CURSOR_SENSITIVITY", LOG_LEVEL_DEBUG);
    // Validate sensitivity setting
    if (value != (SQLPOINTER)SQL_UNSPECIFIED && value != (SQLPOINTER)SQL_INSENSITIVE &&
        value != (SQLPOINTER)SQL_SENSITIVE) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid cursor sensitivity specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid cursor sensitivity");
      return SQL_ERROR;
    }
    stmt->sensitivity = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: Cursor sensitivity set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_USE_BOOKMARKS:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_USE_BOOKMARKS", LOG_LEVEL_DEBUG);
    // Set whether to use bookmarks
    if (value != (SQLPOINTER)SQL_UB_OFF && value != (SQLPOINTER)SQL_UB_VARIABLE &&
        value != (SQLPOINTER)SQL_UB_FIXED) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid bookmark option specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid bookmark option");
      return SQL_ERROR;
    }
    stmt->useBookmarks = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: Bookmark usage setting applied", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_FETCH_BOOKMARK_PTR:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_FETCH_BOOKMARK_PTR", LOG_LEVEL_DEBUG);
    // Store bookmark pointer
    stmt->bookmarkPtr = static_cast<SQLULEN*>(value);
    logMessage(cnct, "SQLSetStmtAttr: Bookmark pointer set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_ROW_ARRAY_SIZE:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_ROW_ARRAY_SIZE", LOG_LEVEL_DEBUG);
    // Set row array size
    stmt->rowArraySize = (SQLULEN)value;
    logMessage(cnct, "SQLSetStmtAttr: Row array size set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_ROW_BIND_TYPE:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_ROW_BIND_TYPE", LOG_LEVEL_DEBUG);
    // Set row bind type
    stmt->rowBindType = reinterpret_cast<SQLULEN>(value);
    logMessage(cnct, "SQLSetStmtAttr: Row bind type set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_PARAMSET_SIZE:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_PARAMSET_SIZE", LOG_LEVEL_DEBUG);
    // Set parameter set size
    stmt->paramSetSize = (SQLULEN)value;
    logMessage(cnct, "SQLSetStmtAttr: Parameter set size set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_PARAMS_PROCESSED_PTR:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_PARAMS_PROCESSED_PTR", LOG_LEVEL_DEBUG);
    // Store processed parameter pointer
    stmt->paramsProcessedPtr = static_cast<SQLULEN*>(value);
    logMessage(cnct, "SQLSetStmtAttr: Params processed pointer set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_PARAM_STATUS_PTR:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_PARAM_STATUS_PTR", LOG_LEVEL_DEBUG);
    // Store parameter status pointer
    stmt->paramStatusPtr = static_cast<SQLUSMALLINT*>(value);
    logMessage(cnct, "SQLSetStmtAttr: Parameter status pointer set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_MAX_ROWS:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_MAX_ROWS", LOG_LEVEL_DEBUG);
    // Set maximum return row count
    stmt->maxRows = (SQLULEN)value;
    logMessage(cnct, "SQLSetStmtAttr: Max rows set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_NOSCAN:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_NOSCAN", LOG_LEVEL_DEBUG);
    // Set whether to scan
    if (value != (SQLPOINTER)SQL_NOSCAN && value != (SQLPOINTER)SQL_UNNAMED) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid noscan option specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid noscan option");
      return SQL_ERROR;
    }
    stmt->noScan = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: NoScan option set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_SIMULATE_CURSOR:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_SIMULATE_CURSOR", LOG_LEVEL_DEBUG);
    // TODO: Implement cursor simulation functionality
    logMessage(cnct, "SQLSetStmtAttr: Cursor simulation setting is not yet implemented",
               LOG_LEVEL_WARN);
    break;

  case SQL_ATTR_RETRIEVE_DATA:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_RETRIEVE_DATA", LOG_LEVEL_DEBUG);
    // Set whether to retrieve data
    if (value != (SQLPOINTER)SQL_RD_ON && value != (SQLPOINTER)SQL_RD_OFF) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid retrieve data option specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid retrieve data option");
      return SQL_ERROR;
    }
    stmt->retrieveData = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: Retrieve data option set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_METADATA_ID:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_METADATA_ID", LOG_LEVEL_DEBUG);
    // Set metadata ID mode
    if (value != (SQLPOINTER)SQL_TRUE && value != (SQLPOINTER)SQL_FALSE) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid metadata ID option specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid metadata ID option");
      return SQL_ERROR;
    }
    stmt->metadataId = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: Metadata ID mode set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_ASYNC_STMT_EVENT:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_ASYNC_STMT_EVENT", LOG_LEVEL_DEBUG);
    // TODO: Implement async statement event functionality
    // Note: This is for asynchronous statement operations
    logMessage(cnct, "SQLSetStmtAttr: Async statement event setting is not yet implemented",
               LOG_LEVEL_WARN);
    break;

  case SQL_ATTR_ENABLE_AUTO_IPD:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_ENABLE_AUTO_IPD", LOG_LEVEL_DEBUG);
    // Set whether to enable automatic IPD
    if (value != (SQLPOINTER)SQL_TRUE && value != (SQLPOINTER)SQL_FALSE) {
      logMessage(cnct, "SQLSetStmtAttr: Invalid auto IPD option specified", LOG_LEVEL_ERROR);
      stmt->addDiagnostic("HY024", "Invalid auto IPD option");
      return SQL_ERROR;
    }
    stmt->enableAutoIPD = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value));
    logMessage(cnct, "SQLSetStmtAttr: Auto IPD setting applied", LOG_LEVEL_DEBUG);
    break;

  // Note: The following attributes are not fully implemented as their exact behavior
  // may vary by database system or require additional context
  case SQL_ATTR_IMP_ROW_DESC:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_IMP_ROW_DESC", LOG_LEVEL_DEBUG);
    // TODO: Implement implementation row descriptor functionality
    // Note: This typically points to an implementation descriptor
    logMessage(cnct,
               "SQLSetStmtAttr: Implementation row descriptor setting is not yet fully implemented",
               LOG_LEVEL_WARN);
    break;

  case SQL_ATTR_IMP_PARAM_DESC:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_IMP_PARAM_DESC", LOG_LEVEL_DEBUG);
    // TODO: Implement implementation parameter descriptor functionality
    // Note: This typically points to an implementation descriptor
    logMessage(
        cnct,
        "SQLSetStmtAttr: Implementation parameter descriptor setting is not yet fully implemented",
        LOG_LEVEL_WARN);
    break;

  case SQL_ATTR_ROW_BIND_OFFSET_PTR:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_ROW_BIND_OFFSET_PTR", LOG_LEVEL_DEBUG);
    // Store pointer to row bind offset
    stmt->rowBindOffsetPtr = static_cast<SQLULEN*>(value);
    logMessage(cnct, "SQLSetStmtAttr: Row bind offset pointer set", LOG_LEVEL_DEBUG);
    break;

  case SQL_ATTR_MAX_LENGTH:
    logMessage(cnct, "SQLSetStmtAttr: Processing SQL_ATTR_MAX_LENGTH", LOG_LEVEL_DEBUG);
    // Set maximum length for data retrieval
    stmt->maxLength = (SQLULEN)value;
    logMessage(cnct, "SQLSetStmtAttr: Max length set", LOG_LEVEL_DEBUG);
    break;

  default:
    logMessage(cnct, "SQLSetStmtAttr: Unsupported attribute: " + std::to_string(attribute),
               LOG_LEVEL_WARN);
    stmt->addDiagnostic("HY092", "Invalid attribute/option identifier");
    return SQL_ERROR;
  }

  logMessage(cnct, "SQLSetStmtAttr: Exiting successfully", LOG_LEVEL_TRACE);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetStmtOption(SQLHSTMT statementHandle, SQLUSMALLINT option, SQLULEN value) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLSetStmtOption: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLSetStmtOption is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLSetStmtOption Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLSpecialColumns(SQLHSTMT statementHandle, SQLUSMALLINT identifierType,
                                    SQLCHAR* catalogName, SQLSMALLINT nameLength1,
                                    SQLCHAR* schemaName, SQLSMALLINT nameLength2,
                                    SQLCHAR* tableName, SQLSMALLINT nameLength3, SQLUSMALLINT scope,
                                    SQLUSMALLINT nullable) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLSpecialColumns: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLSpecialColumns is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLSpecialColumns Function not implemented");
  }
  return SQL_ERROR;
}

SQLRETURN SQL_API SQLStatistics(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                                SQLSMALLINT nameLength1, SQLCHAR* schemaName,
                                SQLSMALLINT nameLength2, SQLCHAR* tableName,
                                SQLSMALLINT nameLength3, SQLUSMALLINT unique,
                                SQLUSMALLINT reserved) {
  if (!statementHandle) {
    logMessage(nullptr, "SQLStatistics: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLStatistics is not implemented", LOG_LEVEL_ERROR);
  if (stmt) {
    stmt->addDiagnostic("IM001", "SQLStatistics Function not implemented");
  }
  return SQL_ERROR;
}

/*
when name is nullptr, return ""
dont deal with the % case here
 */
static std::string convertStringForSQLTables(SQLCHAR* name, SQLSMALLINT length) {
  std::string result;
  if (name == nullptr) {
    return result;
  }

  size_t len;
  if (length == SQL_NTS) {
    len = strlen(reinterpret_cast<char*>(name));
  } else if (length > 0) {
    len = static_cast<size_t>(length);
  } else {
    len = 0;
  }

  if (len > 0) {
    result.assign(reinterpret_cast<char*>(name), len);
    size_t pos = result.find('\0');
    if (pos != std::string::npos) {
      result = result.substr(0, pos);
    }
  }

  return result;
}

SQLRETURN SQL_API SQLTables(SQLHSTMT statementHandle, SQLCHAR* catalogName, SQLSMALLINT nameLength1,
                            SQLCHAR* schemaName, SQLSMALLINT nameLength2, SQLCHAR* tableName,
                            SQLSMALLINT nameLength3, SQLCHAR* tableType, SQLSMALLINT nameLength4) {
  /*
    Currently only supports table model.
    In tree model, the function is incomplete: it will treat all four strings as NULL.
    */

  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;
  logMessage(cnct, "SQLTables: Entering", LOG_LEVEL_TRACE);
  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLTables parameters: "
              << "statementHandle = " << statementHandle
              << ", catalogName = " << (catalogName ? (const char*)catalogName : "nullptr")
              << ", nameLength1 = " << nameLength1
              << ", schemaName = " << (schemaName ? (const char*)schemaName : "nullptr")
              << ", nameLength2 = " << nameLength2
              << ", tableName = " << (tableName ? (const char*)tableName : "nullptr")
              << ", nameLength3 = " << nameLength3
              << ", tableType = " << (tableType ? (const char*)tableType : "nullptr")
              << ", nameLength4 = " << nameLength4;

    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  std::string catalogStr = convertStringForSQLTables(catalogName, nameLength1);
  std::string schemaStr = convertStringForSQLTables(schemaName, nameLength2);
  std::string tableStr = convertStringForSQLTables(tableName, nameLength3);
  std::string tableTypeStr = convertStringForSQLTables(tableType, nameLength4);

  if (statementHandle == nullptr) {
    logMessage(cnct, "SQLTables: Invalid statement handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  if ((catalogName && nameLength1 < 0 && nameLength1 != SQL_NTS) ||
      (schemaName && nameLength2 < 0 && nameLength2 != SQL_NTS) ||
      (tableName && nameLength3 < 0 && nameLength3 != SQL_NTS) ||
      (tableType && nameLength4 < 0 && nameLength4 != SQL_NTS)) {
    logMessage(cnct, "SQLTables: Invalid name length", LOG_LEVEL_ERROR);
    stmt->addDiagnostic("HY090", "Invalid string or buffer length");
    return SQL_ERROR;
  }

  if ((catalogStr.empty() || catalogStr == "%" || catalogStr == SQL_ALL_CATALOGS) &&
      !cnct->database.empty()) {
    // This part is to adapt to Excel actions.
    // Not sure if this fits the ODBC standard
    // because it's not specified in the document.

    // 2026.1.1: this seems to conflict to PowerBI's behavior. temporarily disabled.
    logMessage(cnct,
               "SQLTables: catalogStr is empty and not set to cnct->database:" + cnct->database,
               LOG_LEVEL_TRACE);
  }

  if (schemaName != nullptr && nameLength2 > 0) {
    logMessage(cnct, "SQLTables: Schemas are not supported", LOG_LEVEL_WARN);
    stmt->addDiagnostic("HYC00", "Schemas are not supported. Use CatalogName parameter instead");
    return SQL_ERROR;
  }

  // handling special cases

  // Helper function to check if string equals a special constant
  auto checkSpecialCase = [](const SQLCHAR* str, SQLSMALLINT length, const char* constant) -> bool {
    if (!str)
      return false;
    size_t constLen = std::strlen(constant);
    return (size_t)length == constLen && std::memcmp(str, constant, constLen) == 0;
  };

  // SQL_ALL_CATALOGS: CatalogName is SQL_ALL_CATALOGS and SchemaName and TableName are empty strings
  // Based on PowerBI testing, when SchemaName and TableName are passed as empty strings, it matches this special case, while passing nullptr wants to get all tables. So removed the nullptr condition.
  if (checkSpecialCase(catalogName, nameLength1, SQL_ALL_CATALOGS) &&
      (checkSpecialCase(schemaName, nameLength2, "")) &&
      (checkSpecialCase(tableName, nameLength3, ""))) {
    logMessage(cnct, "SQLTables: Handling SQL_ALL_CATALOGS case", LOG_LEVEL_INFO);
    const char* sqlCommand = "SHOW DATABASES";
    SQLRETURN ret = SQLExecDirect(statementHandle, (SQLCHAR*)sqlCommand, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      logMessage(cnct, "SQLTables: Query for SQL_ALL_CATALOGS failed", LOG_LEVEL_ERROR);
      return ret;
    }
    // Modify the response to SQLTables formats using ODBCResultSet
    stmt->resultSetPtr->setColumnName(0, "TABLE_CAT");
    stmt->resultSetPtr->setColumnName(1, "TABLE_SCHEM");
    stmt->resultSetPtr->setColumnName(2, "TABLE_NAME");
    stmt->resultSetPtr->setColumnName(3, "TABLE_TYPE");
    stmt->resultSetPtr->setColumnName(4, "REMARKS");

    // Set data types for columns 2-4 to TEXT
    stmt->resultSetPtr->setColumnType(2, "TEXT");
    stmt->resultSetPtr->setColumnType(3, "TEXT");
    stmt->resultSetPtr->setColumnType(4, "TEXT");

    // Remove rows where first column is "information_schema"
    size_t removedCount =
        stmt->resultSetPtr->removeRowsIf([](const std::vector<ODBCField>& row) -> bool {
          return !row.empty() && row[0].toString() == "information_schema";
        });

    // Set columns 1-4 to null/empty for remaining rows
    int numRows = stmt->resultSetPtr->getNumRows();
    for (int rowIdx = 0; rowIdx < numRows; ++rowIdx) {
      // Check if row has exactly 5 columns
      if (stmt->resultSetPtr->getNumColumns() != 5) {
        logMessage(cnct, "SQLTables: Result set format wrong - expected 5 columns!",
                   LOG_LEVEL_ERROR);
        stmt->addDiagnostic("HY000", "SQLTables: Invalid result set format - expected 5 columns");
        return SQL_ERROR;
      }
      // Set columns 1-4 to empty (null equivalent)
      for (int colIdx = 1; colIdx < 5; ++colIdx) {
        stmt->resultSetPtr->setValue(rowIdx, colIdx, "");
      }
    }

    // output the final resultSet
    logMessage(cnct, "SQLTables: Outputting final result set for SQL_ALL_CATALOGS",
               LOG_LEVEL_DEBUG);
    stmt->resultSetPtr->outputTable();

    logMessage(cnct,
               "SQLTables: Processed result set, removed " + std::to_string(removedCount) +
                   " information_schema rows",
               LOG_LEVEL_DEBUG);
    logMessage(cnct, "SQLTables: SQL_ALL_CATALOGS query completed successfully", LOG_LEVEL_INFO);
    return SQL_SUCCESS;
  }

  if (cnct->isTableModel) {
    logMessage(cnct, "SQLTables: Handling table model", LOG_LEVEL_TRACE);
    std::string sqlCommand = "SELECT database AS TABLE_CAT, '' AS TABLE_SCHEM, table_name AS "
                             "TABLE_NAME, 'TABLE' AS TABLE_TYPE, comment AS REMARKS FROM "
                             "information_schema.tables WHERE database != 'information_schema'";
    // Apply catalog filter condition
    if (!catalogStr.empty() && catalogStr != "%" && catalogStr != SQL_ALL_CATALOGS) {
      // Escape special characters (% and _), avoid accidental matches in LIKE statements
      sqlCommand += " AND database LIKE '" + catalogStr + "'";
    }
    // Apply table filter condition
    if (!tableStr.empty() && tableStr != "%") {
      // Escape special characters (% and _), avoid accidental matches in LIKE statements
      sqlCommand += " AND table_name LIKE '" + tableStr + "'";
    }

    logMessage(cnct, "SQLTables: Executing query: " + sqlCommand, LOG_LEVEL_DEBUG);
    SQLRETURN ret = SQLExecDirect(statementHandle, (SQLCHAR*)sqlCommand.data(), SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      logMessage(cnct, "SQLTables: Query completed unsuccessfully", LOG_LEVEL_ERROR);
      return ret;
    }

    logMessage(cnct, "SQLTables: Query completed successfully", LOG_LEVEL_INFO);
    logMessage(cnct, "SQLTables: Exiting", LOG_LEVEL_TRACE);
    return SQL_SUCCESS;
  } else {
    logMessage(cnct, "SQLTables: Handling tree model", LOG_LEVEL_TRACE);
    std::string sqlCommand = "SHOW DEVICES ";
    bool catalogEmpty = catalogStr.empty() || catalogStr == "%" || catalogStr == SQL_ALL_CATALOGS;
    bool tableEmpty = tableStr.empty() || tableStr == "%";
    if (catalogEmpty && tableEmpty) {
      // do nothing
    } else if (catalogEmpty && !tableEmpty) {
      sqlCommand += tableStr;
    } else if (!catalogEmpty && tableEmpty) {
      sqlCommand += catalogStr + ".**";
    } else {
      sqlCommand += tableStr;
    }
    sqlCommand += " WITH DATABASE";

    logMessage(cnct, "SQLTables: Executing query: " + sqlCommand, LOG_LEVEL_DEBUG);
    SQLRETURN ret = SQLExecDirect(statementHandle, (SQLCHAR*)sqlCommand.data(), SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      logMessage(cnct, "SQLTables: Query completed unsuccessfully", LOG_LEVEL_ERROR);
      return ret;
    }

    // Transform SHOW DEVICES result (Device, Database, IsAligned, Template, TTL(ms)) to SQLTables format
    if (stmt->resultSetPtr && stmt->resultSetPtr->getNumColumns() >= 5) {
      stmt->resultSetPtr->setColumnName(0, "TABLE_CAT");
      stmt->resultSetPtr->setColumnName(1, "TABLE_SCHEM");
      stmt->resultSetPtr->setColumnName(2, "TABLE_NAME");
      stmt->resultSetPtr->setColumnName(3, "TABLE_TYPE");
      stmt->resultSetPtr->setColumnName(4, "REMARKS");

      for (int colIdx = 0; colIdx < 5; ++colIdx) {
        stmt->resultSetPtr->setColumnType(colIdx, "STRING");
      }

      int idxDevice = stmt->resultSetPtr->findColumnIndex("Device");
      int idxDatabase = stmt->resultSetPtr->findColumnIndex("Database");
      if (idxDevice < 0)
        idxDevice = 0;
      if (idxDatabase < 0)
        idxDatabase = 1;

      int numRows = stmt->resultSetPtr->getNumRows();
      for (int rowIdx = 0; rowIdx < numRows; ++rowIdx) {
        std::string deviceStr = stmt->resultSetPtr->getValue(rowIdx, idxDevice).toString();
        std::string databaseStr = stmt->resultSetPtr->getValue(rowIdx, idxDatabase).toString();
        // TABLE_NAME: strip database prefix (e.g. "root.full.fulldevice" -> "fulldevice" when databaseStr is "root.full")
        logMessage(cnct,
                   "SQLTables: TABLE_NAME: deviceStr: " + deviceStr +
                       ", databaseStr: " + databaseStr,
                   LOG_LEVEL_TRACE);
        if (!databaseStr.empty() && deviceStr.size() > databaseStr.size() &&
            deviceStr.compare(0, databaseStr.size(), databaseStr) == 0 &&
            deviceStr[databaseStr.size()] == '.') {
          logMessage(cnct, "SQLTables: Stripping database prefix", LOG_LEVEL_TRACE);
          deviceStr = deviceStr.substr(databaseStr.size() + 1);
        }
        stmt->resultSetPtr->setValue(rowIdx, 0, ODBCField(databaseStr));          // TABLE_CAT
        stmt->resultSetPtr->setValue(rowIdx, 1, ODBCField(std::string("")));      // TABLE_SCHEM
        stmt->resultSetPtr->setValue(rowIdx, 2, ODBCField(deviceStr));            // TABLE_NAME
        stmt->resultSetPtr->setValue(rowIdx, 3, ODBCField(std::string("TABLE"))); // TABLE_TYPE
        stmt->resultSetPtr->setValue(rowIdx, 4, ODBCField(std::string("")));      // REMARKS
      }
    }

    logMessage(cnct, "SQLTables: Query completed successfully", LOG_LEVEL_INFO);
    logMessage(cnct, "SQLTables: Exiting", LOG_LEVEL_TRACE);
    return SQL_SUCCESS;
  }
}

SQLRETURN SQL_API SQLTransact(SQLHENV environmentHandle, SQLHDBC connectionHandle,
                              SQLUSMALLINT completionType) {
  if (!environmentHandle && !connectionHandle) {
    logMessage(nullptr, "SQLTransact: Invalid environment/connection handle", LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }
  auto env = static_cast<EnvironmentHandle*>(environmentHandle);
  auto cnct = static_cast<ConnectionHandle*>(connectionHandle);
  logMessage(nullptr, "SQLTransact is not implemented", LOG_LEVEL_ERROR);
  if (env) {
    env->addDiagnostic("IM001", "SQLTransact Function not implemented");
  } else if (cnct) {
    cnct->addDiagnostic("IM001", "SQLTransact Function not implemented");
  }
  return SQL_ERROR;
}

// sqlext.h

SQLRETURN SQL_API SQLDriverConnect(SQLHDBC connectionHandle, SQLHWND windowHandle,
                                   SQLCHAR* inConnectionString,
                                   SQLSMALLINT inConnectionStringLength,
                                   SQLCHAR* outConnectionString, SQLSMALLINT bufferLength,
                                   SQLSMALLINT* outConnectionStringLengthPtr,
                                   SQLUSMALLINT driverCompletion) {
  ConnectionHandle* cnct = static_cast<ConnectionHandle*>(connectionHandle);
  if (!cnct)
    return SQL_INVALID_HANDLE;
  cnct->clearDiagnostics();
  logMessage(cnct, "SQLDriverConnect: Entering", LOG_LEVEL_TRACE);

  if (isLogLevelEnabled(cnct, LOG_LEVEL_TRACE)) {
    std::stringstream logStream;
    logStream << "SQLDriverConnect parameters: "
              << "connectionHandle = " << handleToString(connectionHandle)
              << ", windowHandle = " << handleToString(windowHandle)
              << ", inConnectionStringLength = " << inConnectionStringLength
              << ", bufferLength = " << bufferLength << ", driverCompletion = " << driverCompletion;
    logMessage(cnct, logStream.str(), LOG_LEVEL_TRACE);
  }

  if (!inConnectionString || inConnectionStringLength == 0) {
    logMessage(cnct, "SQLDriverConnect: Connection string is empty or null", LOG_LEVEL_ERROR);
    cnct->addDiagnostic("08001", "Invalid connection string");
    return SQL_ERROR;
  }

  if (inConnectionStringLength < 0 && inConnectionStringLength != SQL_NTS) {
    cnct->addDiagnostic("HY090", "Invalid connection string length");
    return SQL_ERROR;
  }
  std::string connectionString((const char*)inConnectionString,
                               inConnectionStringLength == SQL_NTS
                                   ? strlen((const char*)inConnectionString)
                                   : static_cast<size_t>(inConnectionStringLength));
  try {

    // --- Phase 1: first pass to extract DSN name and detect DRIVER keyword ---
    auto attributes = ParseConnectionString(connectionString);
    std::string dsnName;
    bool hasDriverKeyword = false;
    {
      for (const auto& attribute : attributes) {
        std::string key = attribute.first;
        std::string keyLower = key;
        std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (keyLower == "dsn") {
          dsnName = attribute.second;
        } else if (keyLower == "driver") {
          hasDriverKeyword = true;
        }
      }
    }
    logMessage(cnct,
               "SQLDriverConnect: Extracted DSN='" + dsnName +
                   "', hasDriver=" + (hasDriverKeyword ? "true" : "false"),
               LOG_LEVEL_DEBUG);

    // --- Phase 2: if DSN is specified and no DRIVER keyword, load from ODBC.INI ---
    if (!dsnName.empty() && !hasDriverKeyword) {
      logMessage(cnct,
                 "SQLDriverConnect: Loading DSN '" + dsnName + "' from ODBC.INI as base config",
                 LOG_LEVEL_INFO);
      cnct->LoadDsnFromOdbcIni(dsnName);
    }

    // --- Phase 3: parse connection string again to override DSN values ---
    logMessage(cnct, "SQLDriverConnect: Applying connection string overrides", LOG_LEVEL_TRACE);
    {
      for (const auto& attribute : attributes)
        SetConnectionHandle(cnct, attribute.first, attribute.second);
    }

    if (isLogLevelEnabled(cnct, LOG_LEVEL_INFO)) {
      std::ostringstream oss;
      oss << "SQLDriverConnect: Final config - Server=" << cnct->serverHostName
          << ", Port=" << cnct->serverPort << ", UID=" << cnct->userName
          << ", Database=" << cnct->database
          << ", TableModel=" << (cnct->isTableModel ? "true" : "false")
          << ", LogLevel=" << cnct->logLevel << ", SessionTimeout=" << cnct->sessionTimeoutMs
          << ", BatchSize=" << cnct->batchSize;
      logMessage(cnct, oss.str(), LOG_LEVEL_INFO);
    }

    if (outConnectionString && bufferLength > 0) {
      std::snprintf((char*)outConnectionString, bufferLength, "%s", connectionString.c_str());
      if (outConnectionStringLengthPtr) {
        *outConnectionStringLengthPtr = (SQLSMALLINT)strlen((char*)outConnectionString);
      }
      logMessage(cnct, "SQLDriverConnect: Copied connection string to output buffer",
                 LOG_LEVEL_DEBUG);
    }

    if (cnct->isTableModel && cnct->database.empty()) {
      logMessage(cnct, "SQLDriverConnect: Database not set, using 'information_schema' as default",
                 LOG_LEVEL_INFO);
      cnct->database = "information_schema";
    }

    cnct->ValidateTransport();
    SQLRETURN returnCode;
    if (cnct->useRestful) {
      logMessage(cnct, "SQLDriverConnect: Connecting using RESTful API", LOG_LEVEL_DEBUG);
      returnCode = IoTDB_DriverConnect_Rest(cnct);
    } else {
      logMessage(cnct, "SQLDriverConnect: Connecting using Session API", LOG_LEVEL_DEBUG);
      returnCode = IoTDB_DriverConnect_Session(cnct);
    }

    logMessage(cnct, "SQLDriverConnect: Exiting with return code: " + std::to_string(returnCode),
               LOG_LEVEL_TRACE);
    return returnCode;
  } catch (const std::exception& e) {
    cnct->addDiagnostic("08001", e.what());
    return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLMoreResults(SQLHSTMT statementHandle) {
  const auto stmt = static_cast<StatementHandle*>(statementHandle);
  ConnectionHandle* cnct = stmt ? stmt->getConnection() : nullptr;

  logMessage(cnct, "SQLMoreResults: Entering", LOG_LEVEL_TRACE);

  if (!statementHandle) {
    logMessage(cnct, "SQLMoreResults: Invalid statement handle, returning SQL_INVALID_HANDLE",
               LOG_LEVEL_ERROR);
    return SQL_INVALID_HANDLE;
  }

  logMessage(cnct, "SQLMoreResults: Only support one result set. Returning SQL_NO_DATA",
             LOG_LEVEL_DEBUG);
  return SQL_NO_DATA;
}

#ifdef WIN32
#include "setup/setup_dialog.h"

extern "C" BOOL APIENTRY ConfigDSN(HWND hwndParent, WORD fRequest, LPCSTR lpszDriver,
                                   LPCSTR lpszAttributes) {
  logMessage(nullptr, "ConfigDSN: Entering", LOG_LEVEL_INFO);
  return ShowDSNDialog(hwndParent, fRequest, lpszDriver, lpszAttributes);
}
#endif
