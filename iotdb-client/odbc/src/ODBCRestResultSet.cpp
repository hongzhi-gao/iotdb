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
#include "ODBCRestResultSet.h"

// Project includes
#include "Log.h"

// Standard includes
#include <string>
#include <vector>
#include <sstream>
#include "StatementHandle.h"

ODBCRestResultSet::ODBCRestResultSet(StatementHandle* stmtHandle) : ODBCResultSet(stmtHandle) {
  // Initialize member variables
}

ODBCRestResultSet::~ODBCRestResultSet() {
  // Destructor - shared_ptr will handle cleanup automatically
}

// Implement REST data loading
void ODBCRestResultSet::loadData(const nlohmann::json& jsonData) {
  logMessage(stmt->getConnection(), "ODBCRestResultSet::loadData: Starting", LOG_LEVEL_TRACE);
  ConnectionHandle* cnct = stmt->getConnection();

  // Clear existing data
  columnNames.clear();
  columnTypes.clear();
  data.clear();
  numRows = 0;
  numColumns = 0;

  if (stmt->getConnection()->isTableModel) {
    logMessage(cnct, "ODBCRestResultSet::loadData: Parsing JSON response for table model",
               LOG_LEVEL_TRACE);
    parseJsonResponseTable(jsonData);
  } else {
    logMessage(cnct, "ODBCRestResultSet::loadData: Parsing JSON response for tree model",
               LOG_LEVEL_TRACE);
    parseJsonResponseTree(jsonData);
  }

  logMessage(cnct, "ODBCRestResultSet::loadData: Exiting", LOG_LEVEL_DEBUG);
}

// Parse JSON response for tree model
void ODBCRestResultSet::parseJsonResponseTree(const nlohmann::json& jsonResponse) {
  logMessage(stmt->getConnection(), "ODBCRestResultSet::parseJsonResponseTree: Starting",
             LOG_LEVEL_TRACE);
  ConnectionHandle* cnct = stmt->getConnection();

  isMetaData = jsonResponse["expressions"].is_null();
  if (isMetaData) {
    logMessage(cnct, "ODBCRestResultSet::parseJsonResponseTree: Processing metadata response",
               LOG_LEVEL_TRACE);
    // Metadata response - tree model: values[columnIndex][rowIndex]
    // Parse column names
    if (jsonResponse.contains("column_names")) {
      for (const auto& colName : jsonResponse["column_names"]) {
        columnNames.push_back(colName.get<std::string>());
      }
    }

    logMessage(cnct, "ODBCRestResultSet::parseJsonResponseTree: Parsing data values",
               LOG_LEVEL_TRACE);
    // Parse data values
    if (jsonResponse.contains("values") && jsonResponse["values"].is_array()) {
      const auto& values = jsonResponse["values"];
      if (!values.empty() && values[0].is_array()) {
        // For metadata, values[columnIndex] contains data for each column
        numRows = values[0].size();
        numColumns = columnNames.size();

        for (size_t row = 0; row < numRows; ++row) {
          std::vector<ODBCField> rowData;
          for (size_t col = 0; col < numColumns; ++col) {
            ODBCField field;
            if (col < values.size() && row < values[col].size()) {
              const auto& cell = values[col][row];
              // Create ODBCField based on JSON value type
              if (cell.is_null()) {
                field = ODBCField(Field(TSDataType::UNKNOWN));
              } else if (cell.is_string()) {
                field.setValue(cell.get<std::string>());
              } else if (cell.is_number_integer()) {
                field.setValue(static_cast<int64_t>(cell.get<long long>()));
              } else if (cell.is_number_float()) {
                field.setValue(cell.get<double>());
              } else if (cell.is_boolean()) {
                field.setValue(cell.get<bool>());
              } else {
                // For other types, store as string
                field.setValue(cell.dump());
              }
            } else {
              // Null/empty field
              field = ODBCField(Field(TSDataType::UNKNOWN));
            }
            rowData.push_back(field);
          }
          data.push_back(rowData);
        }
      }
    }
  } else {
    logMessage(cnct, "ODBCRestResultSet::parseJsonResponseTree: Processing timeseries response",
               LOG_LEVEL_TRACE);
    // Timeseries data response - tree model: values[columnIndex][rowIndex], timestamps[rowIndex]
    // Parse column names (expressions)
    if (jsonResponse.contains("expressions")) {
      for (const auto& expr : jsonResponse["expressions"]) {
        columnNames.push_back(expr.get<std::string>());
      }
    }

    // For timeseries, add timestamp column at the beginning
    columnNames.insert(columnNames.begin(), "timestamp");
    columnTypes.insert(columnTypes.begin(), "TIMESTAMP");

    // Parse timestamps and values
    if (jsonResponse.contains("timestamps") && jsonResponse.contains("values")) {
      const auto& timestamps = jsonResponse["timestamps"];
      const auto& values = jsonResponse["values"];

      numRows = timestamps.size();
      numColumns = columnNames.size(); // includes timestamp column

      for (size_t row = 0; row < numRows; ++row) {
        std::vector<ODBCField> rowData;

        // Add timestamp
        ODBCField timestampField;
        timestampField.setValue(timestamps[row].get<int64_t>());
        rowData.push_back(timestampField);

        // Add values for each expression
        for (size_t col = 0; col < columnNames.size() - 1;
             ++col) { // -1 because timestamp is already added
          ODBCField field;
          if (col < values.size() && row < values[col].size()) {
            const auto& cell = values[col][row];
            if (cell.is_null()) {
              field = ODBCField(Field(TSDataType::UNKNOWN));
            } else if (cell.is_string()) {
              field.setValue(cell.get<std::string>());
            } else if (cell.is_number_integer()) {
              field.setValue(static_cast<int64_t>(cell.get<long long>()));
            } else if (cell.is_number_float()) {
              field.setValue(cell.get<double>());
            } else if (cell.is_boolean()) {
              field.setValue(cell.get<bool>());
            } else {
              // For other types, store as string
              field.setValue(cell.dump());
            }
          } else {
            field = ODBCField(Field(TSDataType::UNKNOWN));
          }
          rowData.push_back(field);
        }

        data.push_back(rowData);
      }
    }
  }

  logMessage(cnct, "ODBCRestResultSet::parseJsonResponseTree: Exiting", LOG_LEVEL_DEBUG);
}

// Parse JSON response for table model
void ODBCRestResultSet::parseJsonResponseTable(const nlohmann::json& jsonResponse) {
  logMessage(stmt->getConnection(), "ODBCRestResultSet::parseJsonResponseTable: Starting",
             LOG_LEVEL_TRACE);
  ConnectionHandle* cnct = stmt->getConnection();

  isMetaData = true; // Table model is always treated as metadata-like

  // Parse column names
  if (jsonResponse.contains("column_names")) {
    logMessage(cnct, "ODBCRestResultSet::parseJsonResponseTable: Parsing column names",
               LOG_LEVEL_TRACE);
    for (const auto& colName : jsonResponse["column_names"]) {
      columnNames.push_back(colName.get<std::string>());
    }
  }

  // Parse column types
  if (jsonResponse.contains("data_types")) {
    logMessage(cnct, "ODBCRestResultSet::parseJsonResponseTable: Parsing column types",
               LOG_LEVEL_TRACE);
    for (const auto& colType : jsonResponse["data_types"]) {
      columnTypes.push_back(colType.get<std::string>());
    }
  }

  // Parse data values - table model: values[rowIndex][columnIndex]
  if (jsonResponse.contains("values") && jsonResponse["values"].is_array()) {
    logMessage(cnct, "ODBCRestResultSet::parseJsonResponseTable: Parsing data values",
               LOG_LEVEL_TRACE);
    const auto& values = jsonResponse["values"];
    numRows = values.size();

    for (const auto& row : values) {
      std::vector<ODBCField> rowData;
      if (row.is_array()) {
        for (const auto& cell : row) {
          ODBCField field;
          if (cell.is_null()) {
            field = ODBCField(Field(TSDataType::UNKNOWN));
          } else if (cell.is_string()) {
            field.setValue(cell.get<std::string>());
          } else if (cell.is_number_integer()) {
            field.setValue(static_cast<int64_t>(cell.get<long long>()));
          } else if (cell.is_number_float()) {
            field.setValue(cell.get<double>());
          } else if (cell.is_boolean()) {
            field.setValue(cell.get<bool>());
          } else {
            // For other types, store as string
            field.setValue(cell.dump());
          }
          rowData.push_back(field);
        }
      }
      data.push_back(rowData);
    }

    numColumns = columnNames.size();
  }

  logMessage(cnct, "ODBCRestResultSet::parseJsonResponseTable: Exiting", LOG_LEVEL_TRACE);
}
