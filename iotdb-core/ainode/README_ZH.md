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

# Apache IoTDB AINode

## 离线 / 私有化部署

内置模型权重**不**随 AINode 安装包分发，默认在首次使用时从 HuggingFace 下载。
在私有化或内网隔离环境中，可以让 AINode 指向内网镜像，或在预先下载好权重后完全离线运行。

`conf/iotdb-ainode.properties` 中的相关配置：

| 配置项 | 含义 |
| --- | --- |
| `ain_models_dir` | 模型权重存放目录（默认 `data/ainode/models`）。 |
| `ain_hf_endpoint` | 下载源。留空表示 `https://huggingface.co`；内网可指向镜像，如 `https://hf-mirror.com`。 |
| `ain_hf_offline` | 设为 `1` 时不访问网络，只使用 `ain_models_dir` 下已有的权重。 |

### 预下载内置模型（`pull-models`）

在**有网**的机器上运行该工具，再把结果拷贝到离线节点。产出的目录结构与 AINode
运行时加载的路径完全一致（`<output>/builtin/<model_id>/{model.safetensors, config.json}`）。

```bash
# 列出可用的内置模型
sbin/pull-models-ainode.sh --list           # Windows 为 sbin\windows\pull-models-ainode.bat

# 下载全部内置模型（可选走镜像）
sbin/pull-models-ainode.sh --output ./models --endpoint https://hf-mirror.com

# 仅下载指定模型
sbin/pull-models-ainode.sh --output ./models --models timer_xl,sundial
```

随后在离线节点上：

1. 将产出的 `models` 目录拷贝到该节点的 `ain_models_dir`。
2. 在 `conf/iotdb-ainode.properties` 中设置 `ain_hf_offline=1`。
3. 启动 AINode —— 内置模型从本地加载，全程不访问网络。