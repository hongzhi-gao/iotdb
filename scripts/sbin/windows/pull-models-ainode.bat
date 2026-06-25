@REM
@REM Licensed to the Apache Software Foundation (ASF) under one
@REM or more contributor license agreements.  See the NOTICE file
@REM distributed with this work for additional information
@REM regarding copyright ownership.  The ASF licenses this file
@REM to you under the Apache License, Version 2.0 (the
@REM "License"); you may not use this file except in compliance
@REM with the License.  You may obtain a copy of the License at
@REM
@REM     http://www.apache.org/licenses/LICENSE-2.0
@REM
@REM Unless required by applicable law or agreed to in writing,
@REM software distributed under the License is distributed on an
@REM "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
@REM KIND, either express or implied.  See the License for the
@REM specific language governing permissions and limitations
@REM under the License.
@REM

@echo off

echo ```````````````````````````
echo Pulling IoTDB AINode builtin models
echo ```````````````````````````

pushd %~dp0..\..
if NOT DEFINED IOTDB_AINODE_HOME set IOTDB_AINODE_HOME=%cd%
popd

set ain_ainode_executable=%IOTDB_AINODE_HOME%\lib\ainode

@REM Forwards all arguments (e.g. --output, --models, --endpoint, --list) to the
@REM bundled binary. Run this on a networked machine, then copy the produced models
@REM directory to the offline node's ain_models_dir and set ain_hf_offline=1.
%ain_ainode_executable% pull-models %*
