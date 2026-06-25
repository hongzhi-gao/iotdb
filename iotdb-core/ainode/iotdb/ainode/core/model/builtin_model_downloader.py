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

"""
Offline helper to pre-download builtin model weights for private/air-gapped
deployments.

Run this on a machine WITH network access, then copy the produced directory to
the offline node's ``ain_models_dir`` and start AINode with ``ain_hf_offline=1``.

The output layout matches exactly what AINode loads at runtime
(``model_storage._download_model_if_necessary``):

    <output>/builtin/<model_id>/model.safetensors
    <output>/builtin/<model_id>/config.json

The custom modeling code (``modeling_*.py`` / ``configuration_*.py``) is shipped
inside the AINode package itself, so only the weights and config are fetched.

This module is intentionally lightweight (no torch / no config singleton) so it
can run standalone via ``python -m iotdb.ainode.core.model.builtin_model_downloader``
and is also exposed as the ``ainode pull-models`` subcommand of the packaged
binary.
"""

import argparse
import os
import sys

from huggingface_hub import hf_hub_download

from iotdb.ainode.core.model.model_constants import (
    CONFIG_JSON,
    MODEL_SAFETENSORS,
    ModelCategory,
)
from iotdb.ainode.core.model.model_info import BUILTIN_HF_TRANSFORMERS_MODEL_MAP

# Files the runtime fetches per builtin model. Keep in sync with
# model_storage._download_model_if_necessary so the offline pre-check passes.
_REQUIRED_FILES = (MODEL_SAFETENSORS, CONFIG_JSON)


def download_builtin_models(output_dir, model_ids=None, endpoint=None):
    """Download builtin model weights into the AINode on-disk layout.

    Args:
        output_dir: target models directory; files are written under
            ``<output_dir>/builtin/<model_id>/``.
        model_ids: optional list of builtin model ids; defaults to all.
        endpoint: optional download endpoint / mirror (e.g. https://hf-mirror.com).
            Empty / None means the huggingface_hub default.

    Returns:
        The list of model ids that were processed.
    """
    endpoint = endpoint or None
    available = BUILTIN_HF_TRANSFORMERS_MODEL_MAP

    if model_ids:
        unknown = [m for m in model_ids if m not in available]
        if unknown:
            raise ValueError(
                "Unknown builtin model id(s): {}. Available: {}".format(
                    ", ".join(unknown), ", ".join(available)
                )
            )
        targets = list(model_ids)
    else:
        targets = list(available)

    builtin_root = os.path.join(output_dir, ModelCategory.BUILTIN.value)
    for model_id in targets:
        repo_id = available[model_id].repo_id
        model_dir = os.path.join(builtin_root, model_id)
        os.makedirs(model_dir, exist_ok=True)
        print("==> {} (repo: {}) -> {}".format(model_id, repo_id, model_dir))
        for filename in _REQUIRED_FILES:
            target_path = os.path.join(model_dir, filename)
            if os.path.exists(target_path):
                print("    [skip] {} already exists".format(filename))
                continue
            print("    [get ] {} ...".format(filename))
            hf_hub_download(
                repo_id=repo_id,
                filename=filename,
                local_dir=model_dir,
                endpoint=endpoint,
            )
    return targets


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="ainode pull-models",
        description=(
            "Download builtin AINode model weights for offline/private deployment. "
            "Run on a networked machine, then copy the output directory into "
            "ain_models_dir on the offline node and start AINode with ain_hf_offline=1."
        ),
    )
    parser.add_argument(
        "-o",
        "--output",
        default=os.path.join("data", "ainode", "models"),
        help=(
            "Target models directory (default: data/ainode/models). "
            "Files are written under <output>/builtin/<model_id>/."
        ),
    )
    parser.add_argument(
        "-m",
        "--models",
        default="",
        help="Comma-separated builtin model ids to download (default: all).",
    )
    parser.add_argument(
        "-e",
        "--endpoint",
        default=os.environ.get("HF_ENDPOINT", ""),
        help=(
            "Download endpoint / mirror, e.g. https://hf-mirror.com "
            "(default: HF_ENDPOINT env var, otherwise huggingface.co)."
        ),
    )
    parser.add_argument(
        "-l",
        "--list",
        action="store_true",
        help="List available builtin models and exit.",
    )
    args = parser.parse_args(argv)

    if args.list:
        print("Available builtin HuggingFace models:")
        for model_id, info in BUILTIN_HF_TRANSFORMERS_MODEL_MAP.items():
            print("  {:<12} {}".format(model_id, info.repo_id))
        return 0

    model_ids = [m.strip() for m in args.models.split(",") if m.strip()] or None
    try:
        download_builtin_models(
            output_dir=args.output,
            model_ids=model_ids,
            endpoint=args.endpoint or None,
        )
    except Exception as e:
        print("ERROR: failed to download builtin models: {}".format(e), file=sys.stderr)
        return 1

    builtin_root = os.path.join(args.output, ModelCategory.BUILTIN.value)
    print()
    print("Done. Builtin model weights are under: {}".format(builtin_root))
    print("Next steps for offline deployment:")
    print(
        "  1) Copy the '{}' directory to the offline node's ain_models_dir.".format(
            args.output
        )
    )
    print("  2) Set ain_hf_offline=1 in conf/iotdb-ainode.properties.")
    print("  3) Start AINode; builtin models load locally with no network access.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
