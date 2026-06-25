#!/bin/bash
#
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

echo ---------------------------
echo Pulling IoTDB AINode builtin models
echo ---------------------------

IOTDB_AINODE_HOME="$(cd "`dirname "$0"`"/..; pwd)"
export IOTDB_AINODE_HOME

ain_ainode_executable="$IOTDB_AINODE_HOME/lib/ainode"

# Forwards all arguments (e.g. --output, --models, --endpoint, --list) to the
# bundled binary. Run this on a networked machine, then copy the produced models
# directory to the offline node's ain_models_dir and set ain_hf_offline=1.
exec "$ain_ainode_executable" pull-models "$@"
