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
import logging
import os
from enum import Enum

from iotdb.thrift.common.ttypes import TEndPoint

IOTDB_AINODE_HOME = os.getenv("IOTDB_AINODE_HOME", "")
AINODE_VERSION_INFO = "UNKNOWN"
AINODE_BUILD_INFO = "UNKNOWN"
AINODE_CONF_DIRECTORY_NAME = os.path.join(IOTDB_AINODE_HOME, "conf")
AINODE_CONF_FILE_NAME = "iotdb-ainode.properties"
AINODE_CONF_GIT_FILE_NAME = "git.properties"
AINODE_CONF_POM_FILE_NAME = "pom.properties"
AINODE_SYSTEM_FILE_NAME = "system.properties"

# AINode cluster configuration
AINODE_CLUSTER_NAME = "defaultCluster"
AINODE_TARGET_CONFIG_NODE_LIST = TEndPoint("127.0.0.1", 10710)
AINODE_RPC_ADDRESS = "127.0.0.1"
AINODE_RPC_PORT = 10810
AINODE_CLUSTER_INGRESS_ADDRESS = "127.0.0.1"
AINODE_CLUSTER_INGRESS_PORT = 6667
AINODE_CLUSTER_INGRESS_USERNAME = "root"
AINODE_CLUSTER_INGRESS_PASSWORD = "root"

# RPC config
AINODE_THRIFT_COMPRESSION_ENABLED = False
DEFAULT_RECONNECT_TIMEOUT = 20
DEFAULT_RECONNECT_TIMES = 3

# AINode inference configuration
AINODE_INFERENCE_SCHEDULING_PROFILE = "balanced"
AINODE_INFERENCE_SCHEDULING_PROFILES = {
    "latency": {
        "batch_interval_ms": 5,
        "batch_waiting_window_ms": 5,
        "batch_deadline_ms": 200,
    },
    "balanced": {
        "batch_interval_ms": 15,
        "batch_waiting_window_ms": 15,
        "batch_deadline_ms": 500,
    },
    "throughput": {
        "batch_interval_ms": 30,
        "batch_waiting_window_ms": 40,
        "batch_deadline_ms": 1500,
    },
}

# Legacy defaults kept for deprecated direct overrides and tests.
AINODE_INFERENCE_BATCH_INTERVAL_IN_MS = 15
AINODE_INFERENCE_BATCH_WAITING_WINDOW_IN_MS = 15
AINODE_INFERENCE_BATCH_DEADLINE_IN_MS = 500
AINODE_INFERENCE_MAX_BATCH_SIZE = 32
AINODE_INFERENCE_MAX_BATCH_POINTS = 65536
AINODE_INFERENCE_INPUT_LENGTH_BUCKET_SIZE = 32
AINODE_INFERENCE_OUTPUT_LENGTH_BUCKET_SIZE = 16
AINODE_INFERENCE_STATS_LOG_INTERVAL_IN_SEC = 60
AINODE_INFERENCE_MAX_OUTPUT_LENGTH = 2880
AINODE_INFERENCE_MAX_MEMORY_BYTES = 1 << 30
AINODE_INFERENCE_MAX_ACTIVATE_SIZE = 64
AINODE_INFERENCE_MAX_STEP_SIZE = 64
AINODE_INFERENCE_MAX_QUEUE_SIZE = None

# TODO: Should be optimized
AINODE_INFERENCE_MODEL_MEM_USAGE_MAP = {
    "sundial": 1036 * 1024**2,  # 1036 MiB
    "timer_xl": 856 * 1024**2,  # 856 MiB
    "chronos2": 1200 * 1024**2,  # 1200 MiB
}  # the memory usage of each model in bytes

# None means auto-detect at startup by device memory tier.
AINODE_INFERENCE_MEMORY_USAGE_RATIO = None
AINODE_INFERENCE_EXTRA_MEMORY_RATIO = (
    1.2  # the overhead ratio for inference, used to estimate the pool size
)

AINODE_MODELS_DIR = os.path.join(IOTDB_AINODE_HOME, "data/ainode/models")
AINODE_MODELS_BUILTIN_DIR = "iotdb.ainode.core.model"
AINODE_SYSTEM_DIR = os.path.join(IOTDB_AINODE_HOME, "data/ainode/system")
AINODE_LOG_DIR = os.path.join(IOTDB_AINODE_HOME, "logs")

# AINode log
LOG_FILE_TYPE = ["all", "info", "warn", "error"]
AINODE_LOG_FILE_NAME_PREFIX = "log_ainode_"
AINODE_LOG_FILE_LEVELS = [logging.DEBUG, logging.INFO, logging.WARNING, logging.ERROR]
DEFAULT_LOG_LEVEL = logging.INFO
INFERENCE_LOG_FILE_NAME_PREFIX_TEMPLATE = (
    "log_inference_rank_{}_"  # example: log_inference_rank_0_all.log
)

DEFAULT_CHUNK_SIZE = 8192


class TSStatusCode(Enum):
    SUCCESS_STATUS = 200
    REDIRECTION_RECOMMEND = 400
    CONFIG_NODE_LEADER_WARMING_UP = 1014
    MODEL_EXISTED_ERROR = 1503
    MODEL_NOT_EXIST_ERROR = 1504
    CREATE_MODEL_ERROR = 1505
    DROP_BUILTIN_MODEL_ERROR = 1506
    DROP_MODEL_ERROR = 1507
    UNAVAILABLE_AI_DEVICE_ERROR = 1508
    LOAD_MODEL_ERROR = 1509
    UNLOAD_MODEL_ERROR = 1510

    INVALID_URI_ERROR = 1511
    INVALID_INFERENCE_CONFIG = 1512
    INFERENCE_INTERNAL_ERROR = 1520
    INFERENCE_OVERLOAD_ERROR = 1521

    AINODE_INTERNAL_ERROR = 1599  # In case somebody too lazy to add a new error code

    def get_status_code(self) -> int:
        return self.value
