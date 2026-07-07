/*
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

package org.apache.iotdb.ainode.it;

import org.apache.iotdb.ainode.utils.AINodeTestUtils;
import org.apache.iotdb.ainode.utils.AINodeTestUtils.FakeModelInfo;
import org.apache.iotdb.it.env.EnvFactory;
import org.apache.iotdb.it.framework.IoTDBTestRunner;
import org.apache.iotdb.itbase.category.AIClusterIT;
import org.apache.iotdb.itbase.env.BaseEnv;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.junit.AfterClass;
import org.junit.Assert;
import org.junit.Assume;
import org.junit.BeforeClass;
import org.junit.FixMethodOrder;
import org.junit.Test;
import org.junit.experimental.categories.Category;
import org.junit.runner.RunWith;
import org.junit.runners.MethodSorters;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;
import java.sql.Types;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.TimeUnit;

import static org.apache.iotdb.ainode.utils.AINodeTestUtils.BUILTIN_LTSM_MAP;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.BUILTIN_MODEL_MAP;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.assertInferenceOutputClose;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.checkHeader;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.checkModelNotOnSpecifiedDevice;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.checkModelOnSpecifiedDevice;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.concurrentDualCallInference;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.concurrentInference;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.dynamicBatchOutputTolerance;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.errorTest;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.fetchCallInferenceOutput;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.prepareDataInTable;
import static org.apache.iotdb.ainode.utils.AINodeTestUtils.prepareDataInTree;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

/**
 * Consolidates AINodeDeviceManageIT, AINodeModelManageIT, AINodeCallInferenceIT, AINodeForecastIT,
 * AINodeInstanceManagementIT, AINodeConcurrentForecastIT, and AINodeClusterConfigIT into a single
 * class that shares one 1C1D1A cluster, avoiding 7 redundant cluster startups.
 *
 * <p>Test methods run in alphabetical order via {@link FixMethodOrder} so that {@link
 * #zzAiNodeRegisterAndRemoveMustRunLast()} (which calls {@code REMOVE AINODE} and tears the AINode
 * out of the cluster) executes after every other test. Do <strong>not</strong> add new
 * {@code @Test} methods whose names sort after {@code zz}.
 */
@RunWith(IoTDBTestRunner.class)
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
@Category({AIClusterIT.class})
public class AINodeSharedClusterIT {

  private static final Logger LOGGER = LoggerFactory.getLogger(AINodeSharedClusterIT.class);

  /** Populated after cluster startup from {@code SHOW AI_DEVICES}; defaults suit 2-GPU CI. */
  private static String targetDevicesStr = "0,1";

  private static Set<String> targetDevices = new HashSet<>(Arrays.asList("0", "1"));

  private static final String CALL_INFERENCE_SQL_TEMPLATE =
      "CALL INFERENCE(%s, \"SELECT s%d FROM root.AI LIMIT %d\", generateTime=true, outputLength=%d)";
  private static final String CALL_INFERENCE_BY_DEFAULT_SQL_TEMPLATE =
      "CALL INFERENCE(%s, \"SELECT s%d FROM root.AI LIMIT 256\")";
  private static final int DEFAULT_INPUT_LENGTH = 256;
  private static final int DEFAULT_OUTPUT_LENGTH = 48;
  private static final int LOADED_MODEL_SMOKE_INPUT_LENGTH = 96;
  private static final int LOADED_MODEL_SMOKE_OUTPUT_LENGTH = 1;
  private static final int DYNAMIC_BATCH_INPUT_A = 96;
  private static final int DYNAMIC_BATCH_INPUT_B = 104;
  private static final int DYNAMIC_BATCH_OUTPUT_LENGTH = 1;
  private static final List<String> LTSM_LOAD_DEVICE_COMBINATIONS =
      Arrays.asList("cpu", "0", "cpu,0");

  private static final String FORECAST_TABLE_FUNCTION_SQL_TEMPLATE =
      "SELECT * FROM FORECAST("
          + "model_id=>'%s', "
          + "targets=>(SELECT time, s%d FROM db.AI WHERE time<%d ORDER BY time DESC LIMIT %d) ORDER BY time, "
          + "output_start_time=>%d, "
          + "output_length=>%d, "
          + "output_interval=>%d, "
          + "timecol=>'%s'"
          + ")";

  private static final List<FakeModelInfo> CONCURRENT_FORECAST_MODELS =
      Arrays.asList(
          new FakeModelInfo("sundial", "sundial", "builtin", "active"),
          new FakeModelInfo("timer_xl", "timer", "builtin", "active"),
          new FakeModelInfo("chronos2", "t5", "builtin", "active"));
  private static final String CONCURRENT_FORECAST_SQL_TEMPLATE =
      "SELECT * FROM FORECAST(model_id=>'%s', targets=>(SELECT time,s FROM concurrent_db.AI) ORDER BY time, output_length=>%d)";

  @BeforeClass
  public static void setUp() throws Exception {
    EnvFactory.getEnv().initClusterEnvironment(1, 1);
    initializeTargetDevicesFromCluster();
    prepareDataInTree();
    prepareDataInTable();
    prepareDataForConcurrentForecast();
  }

  private static void initializeTargetDevicesFromCluster() throws SQLException {
    List<String> cudaDevices = new ArrayList<>();
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement();
        ResultSet resultSet = statement.executeQuery("SHOW AI_DEVICES")) {
      while (resultSet.next()) {
        if ("cuda".equals(resultSet.getString(2))) {
          cudaDevices.add(resultSet.getString(1));
        }
      }
    }
    Collections.sort(cudaDevices);
    if (cudaDevices.isEmpty()) {
      targetDevicesStr = "cpu";
      targetDevices = new HashSet<>();
    } else {
      targetDevicesStr = String.join(",", cudaDevices);
      targetDevices = new HashSet<>(cudaDevices);
    }
    LOGGER.info("AINode IT target CUDA devices: {}", targetDevicesStr);
  }

  private static boolean isUserDefinedModelFixtureAvailable(String ciPath) {
    return new File(ciPath, "config.json").exists();
  }

  private static String userDefinedModelUri(String ciPath) {
    return "file://" + ciPath;
  }

  private static String readModelTypeFromConfig(String localPath) {
    File configFile = new File(localPath, "config.json");
    if (!configFile.exists()) {
      throw new IllegalStateException("No config.json at " + localPath);
    }
    try (FileReader reader = new FileReader(configFile, StandardCharsets.UTF_8)) {
      JsonObject config = JsonParser.parseReader(reader).getAsJsonObject();
      return config.get("model_type").getAsString();
    } catch (IOException e) {
      throw new IllegalStateException("Failed to read config.json from " + localPath, e);
    }
  }

  private static FakeModelInfo userDefinedModelInfo(String modelId, String ciPath) {
    String modelType = readModelTypeFromConfig(ciPath);
    return new FakeModelInfo(modelId, modelType, "user_defined", "active");
  }

  private static void dropUserDefinedModelIfExists(Statement statement, String modelId)
      throws SQLException {
    final String showSql = String.format("SHOW MODELS %s", modelId);
    try (ResultSet resultSet = statement.executeQuery(showSql)) {
      if (resultSet.next()) {
        statement.execute(String.format("DROP MODEL %s", modelId));
      }
    }
  }

  @AfterClass
  public static void tearDown() throws Exception {
    EnvFactory.getEnv().cleanClusterEnvironment();
  }

  // ========== DeviceManage tests ==========

  @Test
  public void showAIDeviceTestInTree() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      showAIDevicesTest(statement);
    }
  }

  @Test
  public void showAIDeviceTestInTable() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      showAIDevicesTest(statement);
    }
  }

  private void showAIDevicesTest(Statement statement) throws SQLException {
    final String showSql = "SHOW AI_DEVICES";
    try (ResultSet resultSet = statement.executeQuery(showSql)) {
      ResultSetMetaData resultSetMetaData = resultSet.getMetaData();
      checkHeader(resultSetMetaData, "DeviceId,DeviceType");
      java.util.Map<String, String> devices = new java.util.LinkedHashMap<>();
      while (resultSet.next()) {
        devices.put(resultSet.getString(1), resultSet.getString(2));
      }
      Assert.assertTrue("SHOW AI_DEVICES must include cpu", devices.containsKey("cpu"));
      Assert.assertEquals("cpu", devices.get("cpu"));
      long cudaDeviceCount = devices.values().stream().filter(type -> "cuda".equals(type)).count();
      for (int i = 0; i < cudaDeviceCount; i++) {
        String deviceId = String.valueOf(i);
        Assert.assertTrue("Missing CUDA device " + deviceId, devices.containsKey(deviceId));
        Assert.assertEquals("cuda", devices.get(deviceId));
      }
    }
  }

  // ========== ModelManage tests ==========

  @Test
  public void userDefinedModelManagementTestInTree() throws SQLException, InterruptedException {
    Assume.assumeTrue(
        "CI user-defined model fixtures are unavailable",
        isUserDefinedModelFixtureAvailable("/data/chronos2")
            && isUserDefinedModelFixtureAvailable("/data/mantis"));
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      dropUserDefinedModelIfExists(statement, "user_chronos");
      dropUserDefinedModelIfExists(statement, "user_mantis");

      FakeModelInfo modelInfo = userDefinedModelInfo("user_chronos", "/data/chronos2");
      registerUserDefinedModel(statement, modelInfo, userDefinedModelUri("/data/chronos2"));
      callInferenceTest(statement, modelInfo);
      dropUserDefinedModel(statement, modelInfo.getModelId());

      modelInfo = userDefinedModelInfo("user_mantis", "/data/mantis");
      registerUserDefinedModel(statement, modelInfo, userDefinedModelUri("/data/mantis"));
      dropUserDefinedModel(statement, modelInfo.getModelId());
    }
  }

  @Test
  public void userDefinedModelManagementTestInTable() throws SQLException, InterruptedException {
    Assume.assumeTrue(
        "CI user-defined model fixtures are unavailable",
        isUserDefinedModelFixtureAvailable("/data/chronos2")
            && isUserDefinedModelFixtureAvailable("/data/mantis"));
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      dropUserDefinedModelIfExists(statement, "user_chronos");
      dropUserDefinedModelIfExists(statement, "user_mantis");

      FakeModelInfo modelInfo = userDefinedModelInfo("user_chronos", "/data/chronos2");
      registerUserDefinedModel(statement, modelInfo, userDefinedModelUri("/data/chronos2"));
      forecastTableFunctionTest(statement, modelInfo);
      dropUserDefinedModel(statement, modelInfo.getModelId());

      modelInfo = userDefinedModelInfo("user_mantis", "/data/mantis");
      registerUserDefinedModel(statement, modelInfo, userDefinedModelUri("/data/mantis"));
      dropUserDefinedModel(statement, modelInfo.getModelId());
    }
  }

  // ========== BuiltinModel tests ==========

  @Test
  public void dropBuiltInModelErrorTestInTree() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      errorTest(statement, "drop model sundial", "1506: Cannot delete built-in model: sundial");
    }
  }

  @Test
  public void dropBuiltInModelErrorTestInTable() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      errorTest(statement, "drop model sundial", "1506: Cannot delete built-in model: sundial");
    }
  }

  @Test
  public void showBuiltInModelTestInTree() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      showBuiltInModelTest(statement);
    }
  }

  @Test
  public void showBuiltInModelTestInTable() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      showBuiltInModelTest(statement);
    }
  }

  private void showBuiltInModelTest(Statement statement) throws SQLException {
    int built_in_model_count = 0;
    final String showSql = "SHOW MODELS";
    try (ResultSet resultSet = statement.executeQuery(showSql)) {
      ResultSetMetaData resultSetMetaData = resultSet.getMetaData();
      checkHeader(resultSetMetaData, "ModelId,ModelType,Category,State");
      while (resultSet.next()) {
        built_in_model_count++;
        FakeModelInfo modelInfo =
            new FakeModelInfo(
                resultSet.getString(1),
                resultSet.getString(2),
                resultSet.getString(3),
                resultSet.getString(4));
        assertTrue(BUILTIN_MODEL_MAP.containsKey(modelInfo.getModelId()));
        assertEquals(BUILTIN_MODEL_MAP.get(modelInfo.getModelId()), modelInfo);
      }
    }
    assertEquals(BUILTIN_MODEL_MAP.size(), built_in_model_count);
  }

  // ========== CallInference tests ==========

  @Test
  public void callInferenceTest() throws SQLException {
    for (AINodeTestUtils.FakeModelInfo modelInfo : BUILTIN_MODEL_MAP.values()) {
      try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
          Statement statement = connection.createStatement()) {
        callInferenceTest(statement, modelInfo);
        callInferenceByDefaultTest(statement, modelInfo);
        callInferenceErrorTest(statement, modelInfo);
      }
    }
  }

  public static void callInferenceTest(Statement statement, AINodeTestUtils.FakeModelInfo modelInfo)
      throws SQLException {
    for (int i = 0; i < 4; i++) {
      String callInferenceSQL =
          String.format(
              CALL_INFERENCE_SQL_TEMPLATE,
              modelInfo.getModelId(),
              i,
              DEFAULT_INPUT_LENGTH,
              DEFAULT_OUTPUT_LENGTH);
      try (ResultSet resultSet = statement.executeQuery(callInferenceSQL)) {
        ResultSetMetaData resultSetMetaData = resultSet.getMetaData();
        checkHeader(resultSetMetaData, "Time,output");
        Assert.assertEquals(Types.DOUBLE, resultSetMetaData.getColumnType(2));
        int count = 0;
        while (resultSet.next()) {
          resultSet.getDouble("output");
          count++;
        }
        Assert.assertEquals(DEFAULT_OUTPUT_LENGTH, count);
      }
    }
  }

  public static void callInferenceByDefaultTest(
      Statement statement, AINodeTestUtils.FakeModelInfo modelInfo) throws SQLException {
    for (int i = 0; i < 4; i++) {
      String callInferenceSQL =
          String.format(CALL_INFERENCE_BY_DEFAULT_SQL_TEMPLATE, modelInfo.getModelId(), i);
      try (ResultSet resultSet = statement.executeQuery(callInferenceSQL)) {
        ResultSetMetaData resultSetMetaData = resultSet.getMetaData();
        checkHeader(resultSetMetaData, "output");
        Assert.assertEquals(Types.DOUBLE, resultSetMetaData.getColumnType(1));
        int count = 0;
        while (resultSet.next()) {
          resultSet.getDouble("output");
          count++;
        }
        Assert.assertTrue(count > 0);
      }
    }
  }

  public static void callInferenceErrorTest(
      Statement statement, AINodeTestUtils.FakeModelInfo modelInfo) {
    String multiVariateSQL =
        String.format(
            "CALL INFERENCE(%s, \"SELECT s0,s1 FROM root.AI LIMIT 128\", generateTime=true, outputLength=10)",
            modelInfo.getModelId());
    errorTest(
        statement,
        multiVariateSQL,
        "701: Call inference function should not contain more than one input column, found [2] input columns.");
  }

  // ========== Forecast tests ==========

  @Test
  public void forecastTableFunctionTest() throws SQLException {
    for (AINodeTestUtils.FakeModelInfo modelInfo : BUILTIN_MODEL_MAP.values()) {
      try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
          Statement statement = connection.createStatement()) {
        forecastTableFunctionTest(statement, modelInfo);
      }
    }
  }

  public static void forecastTableFunctionTest(
      Statement statement, AINodeTestUtils.FakeModelInfo modelInfo) throws SQLException {
    for (int i = 0; i < 4; i++) {
      String forecastTableFunctionSQL =
          String.format(
              FORECAST_TABLE_FUNCTION_SQL_TEMPLATE,
              modelInfo.getModelId(),
              i,
              5760,
              2880,
              5760,
              96,
              1,
              "time");
      try (ResultSet resultSet = statement.executeQuery(forecastTableFunctionSQL)) {
        int count = 0;
        while (resultSet.next()) {
          count++;
        }
        Assert.assertTrue(count > 0);
      }
    }
  }

  @Test
  public void forecastTableFunctionErrorTest() throws SQLException {
    for (AINodeTestUtils.FakeModelInfo modelInfo : BUILTIN_MODEL_MAP.values()) {
      try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
          Statement statement = connection.createStatement()) {
        forecastTableFunctionErrorTest(statement, modelInfo);
      }
    }
  }

  public static void forecastTableFunctionErrorTest(
      Statement statement, AINodeTestUtils.FakeModelInfo modelInfo) throws SQLException {
    String invalidOutputStartTimeSQL =
        String.format(
            FORECAST_TABLE_FUNCTION_SQL_TEMPLATE,
            modelInfo.getModelId(),
            0,
            5760,
            2880,
            5759,
            96,
            1,
            "time");
    errorTest(
        statement,
        invalidOutputStartTimeSQL,
        "701: The OUTPUT_START_TIME should be greater than the maximum timestamp of target time series. Expected greater than [5759] but found [5759].");

    String invalidOutputLengthSQLWithZero =
        String.format(
            FORECAST_TABLE_FUNCTION_SQL_TEMPLATE,
            modelInfo.getModelId(),
            0,
            5760,
            2880,
            5760,
            0,
            1,
            "time");
    errorTest(
        statement, invalidOutputLengthSQLWithZero, "701: OUTPUT_LENGTH should be greater than 0");

    String invalidOutputLengthSQLWithOutOfRange =
        String.format(
            FORECAST_TABLE_FUNCTION_SQL_TEMPLATE,
            modelInfo.getModelId(),
            0,
            5760,
            2880,
            5760,
            2881,
            1,
            "time");
    errorTest(
        statement,
        invalidOutputLengthSQLWithOutOfRange,
        "1599: Error occurred while executing forecast:[Attribute output_length expect value between 1 and 2880, got 2881 instead.]");

    String invalidOutputIntervalSQL =
        String.format(
            FORECAST_TABLE_FUNCTION_SQL_TEMPLATE,
            modelInfo.getModelId(),
            0,
            5760,
            2880,
            5760,
            96,
            -1,
            "time");
    errorTest(statement, invalidOutputIntervalSQL, "701: OUTPUT_INTERVAL should be greater than 0");

    String invalidTimecolSQL2 =
        String.format(
            FORECAST_TABLE_FUNCTION_SQL_TEMPLATE,
            modelInfo.getModelId(),
            0,
            5760,
            2880,
            5760,
            96,
            1,
            "s0");
    errorTest(
        statement, invalidTimecolSQL2, "701: The type of the column [s0] is not as expected.");
  }

  // ========== Concurrent forecast tests ==========

  @Test
  public void dynamicBatchingOutputConsistencyTest() throws SQLException, InterruptedException {
    Assume.assumeTrue(
        "CUDA devices required for loaded-model inference pool", !targetDevices.isEmpty());
    final String devices = targetDevices.iterator().next();
    try (Connection treeConnection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement treeStatement = treeConnection.createStatement()) {
      for (FakeModelInfo modelInfo : BUILTIN_LTSM_MAP.values()) {
        final String modelId = modelInfo.getModelId();
        treeStatement.execute(String.format("LOAD MODEL %s TO DEVICES '%s'", modelId, devices));
        try {
          checkModelOnSpecifiedDevice(treeStatement, modelId, devices);
          final double tolerance = dynamicBatchOutputTolerance(modelId);
          final double[] singleSameA =
              fetchCallInferenceOutput(
                  treeStatement, modelId, 0, DYNAMIC_BATCH_INPUT_A, DYNAMIC_BATCH_OUTPUT_LENGTH);
          final double[] singleSameB =
              fetchCallInferenceOutput(
                  treeStatement, modelId, 0, DYNAMIC_BATCH_INPUT_A, DYNAMIC_BATCH_OUTPUT_LENGTH);
          final double[][] sameLengthBatched =
              concurrentDualCallInference(
                  modelId,
                  new int[] {0, 0},
                  new int[] {DYNAMIC_BATCH_INPUT_A, DYNAMIC_BATCH_INPUT_A},
                  DYNAMIC_BATCH_OUTPUT_LENGTH);
          assertInferenceOutputClose(singleSameA, sameLengthBatched[0], tolerance);
          assertInferenceOutputClose(singleSameB, sameLengthBatched[1], tolerance);

          final double[] singleLonger =
              fetchCallInferenceOutput(
                  treeStatement, modelId, 0, DYNAMIC_BATCH_INPUT_B, DYNAMIC_BATCH_OUTPUT_LENGTH);
          final double[][] mixedLengthBatched =
              concurrentDualCallInference(
                  modelId,
                  new int[] {0, 0},
                  new int[] {DYNAMIC_BATCH_INPUT_A, DYNAMIC_BATCH_INPUT_B},
                  DYNAMIC_BATCH_OUTPUT_LENGTH);
          // Shorter requests are padded before revin normalization in a mixed batch.
          assertInferenceOutputClose(singleLonger, mixedLengthBatched[1], tolerance);
          LOGGER.info(
              "Dynamic batching output consistency passed for model {} on device {}",
              modelId,
              devices);
        } finally {
          treeStatement.execute(
              String.format("UNLOAD MODEL %s FROM DEVICES '%s'", modelId, devices));
          checkModelNotOnSpecifiedDevice(treeStatement, modelId, devices);
        }
      }
    }
  }

  @Test
  public void largeTimeSeriesModelLoadInferenceAndForecastTest()
      throws SQLException, InterruptedException {
    try (Connection treeConnection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement treeStatement = treeConnection.createStatement();
        Connection tableConnection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement tableStatement = tableConnection.createStatement()) {
      for (FakeModelInfo modelInfo : BUILTIN_LTSM_MAP.values()) {
        for (String devices : LTSM_LOAD_DEVICE_COMBINATIONS) {
          loadRunAndUnloadModelOnDevices(
              treeStatement, tableStatement, modelInfo.getModelId(), devices);
        }
      }
    }
  }

  private void loadRunAndUnloadModelOnDevices(
      Statement treeStatement, Statement tableStatement, String modelId, String devices)
      throws SQLException, InterruptedException {
    boolean loadSubmitted = false;
    try {
      treeStatement.execute(String.format("LOAD MODEL %s TO DEVICES '%s'", modelId, devices));
      loadSubmitted = true;
      checkModelOnSpecifiedDevice(treeStatement, modelId, devices);
      assertLoadedModelCallInferenceSucceeds(treeStatement, modelId);
      assertLoadedModelForecastSucceeds(tableStatement, modelId);
    } finally {
      if (loadSubmitted) {
        treeStatement.execute(String.format("UNLOAD MODEL %s FROM DEVICES '%s'", modelId, devices));
        checkModelNotOnSpecifiedDevice(treeStatement, modelId, devices);
      }
    }
  }

  private void assertLoadedModelCallInferenceSucceeds(Statement statement, String modelId)
      throws SQLException {
    String callInferenceSQL =
        String.format(
            CALL_INFERENCE_SQL_TEMPLATE,
            modelId,
            0,
            LOADED_MODEL_SMOKE_INPUT_LENGTH,
            LOADED_MODEL_SMOKE_OUTPUT_LENGTH);
    try (ResultSet resultSet = statement.executeQuery(callInferenceSQL)) {
      ResultSetMetaData resultSetMetaData = resultSet.getMetaData();
      checkHeader(resultSetMetaData, "Time,output");
      Assert.assertEquals(Types.DOUBLE, resultSetMetaData.getColumnType(2));
      int count = 0;
      while (resultSet.next()) {
        resultSet.getDouble("output");
        count++;
      }
      Assert.assertEquals(LOADED_MODEL_SMOKE_OUTPUT_LENGTH, count);
    }
  }

  private void assertLoadedModelForecastSucceeds(Statement statement, String modelId)
      throws SQLException {
    String forecastTableFunctionSQL =
        String.format(
            FORECAST_TABLE_FUNCTION_SQL_TEMPLATE,
            modelId,
            0,
            5760,
            LOADED_MODEL_SMOKE_INPUT_LENGTH,
            5760,
            LOADED_MODEL_SMOKE_OUTPUT_LENGTH,
            1,
            "time");
    try (ResultSet resultSet = statement.executeQuery(forecastTableFunctionSQL)) {
      int count = 0;
      while (resultSet.next()) {
        count++;
      }
      Assert.assertEquals(LOADED_MODEL_SMOKE_OUTPUT_LENGTH, count);
    }
  }

  @Test
  public void concurrentForecastTest() throws SQLException, InterruptedException {
    for (FakeModelInfo modelInfo : CONCURRENT_FORECAST_MODELS) {
      concurrentGPUForecastTest(modelInfo, targetDevicesStr);
      // TODO: Enable cpu test after optimize memory consumption
      // concurrentGPUForecastTest(modelInfo, "cpu");
    }
  }

  private void concurrentGPUForecastTest(FakeModelInfo modelInfo, String devices)
      throws SQLException, InterruptedException {
    final int forecastLength = 512;
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      final String forecastSQL =
          String.format(CONCURRENT_FORECAST_SQL_TEMPLATE, modelInfo.getModelId(), forecastLength);
      final int threadCnt = 10;
      // PR CI keeps a concurrency smoke check; nightly/daily can dial this up if regressions
      // appear.
      final int loop = 10;
      statement.execute(
          String.format("LOAD MODEL %s TO DEVICES '%s'", modelInfo.getModelId(), devices));
      checkModelOnSpecifiedDevice(statement, modelInfo.getModelId(), devices);
      long startTime = System.currentTimeMillis();
      concurrentInference(statement, forecastSQL, threadCnt, loop, forecastLength);
      long endTime = System.currentTimeMillis();
      LOGGER.info(
          String.format(
              "Model %s concurrent inference %d reqs (%d threads, %d loops) in GPU takes time: %dms",
              modelInfo.getModelId(), threadCnt * loop, threadCnt, loop, endTime - startTime));
      statement.execute(
          String.format("UNLOAD MODEL %s FROM DEVICES '%s'", modelInfo.getModelId(), devices));
      checkModelNotOnSpecifiedDevice(statement, modelInfo.getModelId(), devices);
    }
  }

  private static void prepareDataForConcurrentForecast() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      statement.execute("CREATE DATABASE concurrent_db");
      statement.execute("CREATE TABLE concurrent_db.AI (s DOUBLE FIELD)");
      for (int i = 0; i < 2880; i++) {
        statement.addBatch(
            String.format(
                "INSERT INTO concurrent_db.AI(time, s) VALUES(%d, %f)",
                i, Math.sin(i * Math.PI / 1440)));
        if ((i + 1) % 500 == 0) {
          statement.executeBatch();
          statement.clearBatch();
        }
      }
      statement.executeBatch();
    }
  }

  // ========== InstanceManagement tests ==========

  @Test
  public void instanceBasicManagementTestInTreeModel() throws SQLException, InterruptedException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      instanceBasicManagementTest(statement);
    }
  }

  @Test
  public void instanceBasicManagementTestInTableModel() throws SQLException, InterruptedException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      instanceBasicManagementTest(statement);
    }
  }

  private void instanceBasicManagementTest(Statement statement)
      throws SQLException, InterruptedException {
    try (ResultSet resultSet = statement.executeQuery("SHOW AI_DEVICES")) {
      ResultSetMetaData resultSetMetaData = resultSet.getMetaData();
      checkHeader(resultSetMetaData, "DeviceId,DeviceType");
      final Set<String> resultDevices = new HashSet<>();
      while (resultSet.next()) {
        resultDevices.add(resultSet.getString("DeviceId"));
      }
      Set<String> expected = new HashSet<>(targetDevices);
      expected.add("cpu");
      Assert.assertEquals(expected, resultDevices);
    }

    statement.execute(String.format("LOAD MODEL sundial TO DEVICES '%s'", targetDevicesStr));
    checkModelOnSpecifiedDevice(statement, "sundial", targetDevicesStr);
    statement.execute(String.format("UNLOAD MODEL sundial FROM DEVICES '%s'", targetDevicesStr));
    checkModelNotOnSpecifiedDevice(statement, "sundial", targetDevicesStr);

    statement.execute(String.format("LOAD MODEL timer_xl TO DEVICES '%s'", targetDevicesStr));
    checkModelOnSpecifiedDevice(statement, "timer_xl", targetDevicesStr);
    statement.execute(String.format("UNLOAD MODEL timer_xl FROM DEVICES '%s'", targetDevicesStr));
    checkModelNotOnSpecifiedDevice(statement, "timer_xl", targetDevicesStr);
  }

  @Test
  public void instanceFailTestInTreeModel() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      instanceFailTest(statement);
    }
  }

  @Test
  public void instanceFailTestInTableModel() throws SQLException {
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      instanceFailTest(statement);
    }
  }

  private void instanceFailTest(Statement statement) {
    errorTest(
        statement,
        "LOAD MODEL unknown TO DEVICES 'cpu,0,1'",
        "1504: Model [unknown] is not registered yet. You can use 'SHOW MODELS' to retrieve the available models.");
    errorTest(
        statement,
        "LOAD MODEL sundial TO DEVICES '999'",
        "1508: AIDevice ID [999] is not available. You can use 'SHOW AI_DEVICES' to retrieve the available devices.");
    errorTest(
        statement,
        "UNLOAD MODEL sundial FROM DEVICES '999'",
        "1508: AIDevice ID [999] is not available. You can use 'SHOW AI_DEVICES' to retrieve the available devices.");
    errorTest(
        statement,
        "LOAD MODEL sundial TO DEVICES '0,0'",
        "1509: Device ID list contains duplicate entries.");
    errorTest(
        statement,
        "UNLOAD MODEL sundial FROM DEVICES '0,0'",
        "1510: Device ID list contains duplicate entries.");
  }

  // ========== AINode lifecycle (must run last) ==========

  /**
   * REMOVE AINODE permanently tears the AINode out of the cluster, so this test must run after
   * every other {@code @Test} in the class. The {@code zz} prefix combined with {@link
   * FixMethodOrder} on the class keeps it last under alphabetical sorting; do not add new test
   * methods whose names sort after this one.
   */
  @Test
  public void zzAiNodeRegisterAndRemoveMustRunLast() throws SQLException {
    final String showSql = "SHOW AINODES";
    final String title = "NodeID,Status,InternalAddress,InternalPort";

    // Verify AINode exists via both dialects before removal
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      verifyAINodeExists(statement, showSql, title);
    }
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      verifyAINodeExists(statement, showSql, title);
    }

    // Remove AINode
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TREE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      statement.execute("REMOVE AINODE");
      waitForAINodeRemoval(statement, showSql, title);
    }

    // Verify removal is visible via table dialect as well
    try (Connection connection = EnvFactory.getEnv().getConnection(BaseEnv.TABLE_SQL_DIALECT);
        Statement statement = connection.createStatement()) {
      try (ResultSet resultSet = statement.executeQuery(showSql)) {
        checkHeader(resultSet.getMetaData(), title);
        int count = 0;
        while (resultSet.next()) {
          count++;
        }
        assertEquals(0, count);
      }
    }
  }

  private static void verifyAINodeExists(Statement statement, String showSql, String title)
      throws SQLException {
    try (ResultSet resultSet = statement.executeQuery(showSql)) {
      checkHeader(resultSet.getMetaData(), title);
      int count = 0;
      while (resultSet.next()) {
        assertEquals("2", resultSet.getString(1));
        assertEquals("Running", resultSet.getString(2));
        count++;
      }
      assertEquals(1, count);
    }
  }

  private static void waitForAINodeRemoval(Statement statement, String showSql, String title)
      throws SQLException {
    for (int retry = 0; retry < 500; retry++) {
      try (ResultSet resultSet = statement.executeQuery(showSql)) {
        checkHeader(resultSet.getMetaData(), title);
        int count = 0;
        while (resultSet.next()) {
          count++;
        }
        if (count == 0) {
          return;
        }
      }
      try {
        Thread.sleep(1000);
      } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
      }
    }
    Assert.fail("The target AINode is not removed successfully after all retries.");
  }

  // ========== Helper methods (from ModelManageIT) ==========

  private static void registerUserDefinedModel(
      Statement statement, AINodeTestUtils.FakeModelInfo modelInfo, String uri)
      throws SQLException, InterruptedException {
    String modelId = modelInfo.getModelId();
    String modelType = modelInfo.getModelType();
    String category = modelInfo.getCategory();
    final String CREATE_MODEL_TEMPLATE = "create model %s using uri \"%s\"";
    final String alterConfigSQL = "set configuration \"trusted_uri_pattern\"='.*'";
    final String registerSql = String.format(CREATE_MODEL_TEMPLATE, modelId, uri);
    final String showSql = String.format("SHOW MODELS %s", modelId);
    statement.execute(alterConfigSQL);
    statement.execute(registerSql);
    boolean loading = true;
    for (int retryCnt = 0; retryCnt < 100; retryCnt++) {
      try (ResultSet resultSet = statement.executeQuery(showSql)) {
        ResultSetMetaData resultSetMetaData = resultSet.getMetaData();
        checkHeader(resultSetMetaData, "ModelId,ModelType,Category,State");
        while (resultSet.next()) {
          String resultModelId = resultSet.getString(1);
          String resultModelType = resultSet.getString(2);
          String resultCategory = resultSet.getString(3);
          String state = resultSet.getString(4);
          assertEquals(modelId, resultModelId);
          assertEquals(modelType, resultModelType);
          assertEquals(category, resultCategory);
          if (state.equals("active")) {
            loading = false;
          } else if (state.equals("loading")) {
            break;
          } else {
            fail("Unexpected status of model: " + state);
          }
        }
      }
      if (!loading) {
        break;
      }
      TimeUnit.SECONDS.sleep(1);
    }
    assertFalse(loading);
  }

  private static void dropUserDefinedModel(Statement statement, String modelId)
      throws SQLException {
    final String showSql = String.format("SHOW MODELS %s", modelId);
    final String dropSql = String.format("DROP MODEL %s", modelId);
    statement.execute(dropSql);
    try (ResultSet resultSet = statement.executeQuery(showSql)) {
      ResultSetMetaData resultSetMetaData = resultSet.getMetaData();
      checkHeader(resultSetMetaData, "ModelId,ModelType,Category,State");
      int count = 0;
      while (resultSet.next()) {
        count++;
      }
      assertEquals(0, count);
    }
  }
}
