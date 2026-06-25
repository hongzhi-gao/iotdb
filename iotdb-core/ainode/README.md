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

## Offline / Private Deployment

Builtin model weights are **not** bundled in the AINode package; by default they
are downloaded from HuggingFace on first use. For private or air-gapped
deployments you can either point AINode at an internal mirror or run it fully
offline with pre-downloaded weights.

Relevant options in `conf/iotdb-ainode.properties`:

| Option | Meaning |
| --- | --- |
| `ain_models_dir` | Directory where model weights are stored (default `data/ainode/models`). |
| `ain_hf_endpoint` | Download endpoint. Empty = `https://huggingface.co`. Set to an internal mirror (e.g. `https://hf-mirror.com`) for private networks. |
| `ain_hf_offline` | `1` = never reach the network; only use weights already under `ain_models_dir`. |

### Pre-downloading builtin models (`pull-models`)

Run the helper on a machine **with** network access, then copy the result to the
offline node. The output layout matches exactly what AINode loads at runtime
(`<output>/builtin/<model_id>/{model.safetensors, config.json}`).

```bash
# List available builtin models
sbin/pull-models-ainode.sh --list           # sbin\windows\pull-models-ainode.bat on Windows

# Download all builtin models (optionally via a mirror)
sbin/pull-models-ainode.sh --output ./models --endpoint https://hf-mirror.com

# Download only specific models
sbin/pull-models-ainode.sh --output ./models --models timer_xl,sundial
```

Then on the offline node:

1. Copy the produced `models` directory into the node's `ain_models_dir`.
2. Set `ain_hf_offline=1` in `conf/iotdb-ainode.properties`.
3. Start AINode — builtin models load locally with no network access.