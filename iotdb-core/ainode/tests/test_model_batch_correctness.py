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

import os
import shutil
import unittest
from pathlib import Path

import torch

from iotdb.ainode.core.config import AINodeDescriptor
from iotdb.ainode.core.inference.batcher.dynamic_batcher import DynamicBatcher
from iotdb.ainode.core.inference.inference_request import InferenceRequest
from iotdb.ainode.core.inference.pipeline.basic_pipeline import ForecastPipeline
from iotdb.ainode.core.inference.pipeline.pipeline_loader import load_pipeline
from iotdb.ainode.core.manager.device_manager import DeviceManager
from iotdb.ainode.core.model.model_constants import ModelCategory
from iotdb.ainode.core.model.model_info import BUILTIN_HF_TRANSFORMERS_MODEL_MAP
from iotdb.ainode.core.model.utils import validate_model_files

# Mirrors AINodeSharedClusterIT dynamic batching constants.
LTSM_MODEL_IDS = sorted(BUILTIN_HF_TRANSFORMERS_MODEL_MAP.keys())
INPUT_A = 96
INPUT_B = 104
OUTPUT_LENGTH = 1
DEFAULT_TOLERANCE = 1e-2
STOCHASTIC_MODEL_TOLERANCE = 0.1
STOCHASTIC_MODEL_IDS = {"sundial"}
FIXED_SEED = 2021
MODEL_CACHE_CANDIDATES = (
    Path("/data/ainode/models"),
    Path.home() / ".cache" / "iotdb-ainode-models",
)


def _tolerance_for_model(model_id: str) -> float:
    if model_id in STOCHASTIC_MODEL_IDS:
        return STOCHASTIC_MODEL_TOLERANCE
    return DEFAULT_TOLERANCE


def _make_it_inputs(input_length: int) -> torch.Tensor:
    """Mirror root.AI.s0 values used by AINode ITs: 0..input_length-1."""
    return torch.arange(input_length, dtype=torch.float32).view(1, 1, input_length)


def _make_request(model_id: str, req_id: str, input_length: int) -> InferenceRequest:
    return InferenceRequest(
        req_id=req_id,
        model_id=model_id,
        inputs=_make_it_inputs(input_length),
        output_length=OUTPUT_LENGTH,
    )


def _run_forecast_batch(
    pipeline: ForecastPipeline,
    batcher: DynamicBatcher,
    device: torch.device,
    requests: list[InferenceRequest],
) -> list[torch.Tensor]:
    """Mirror InferenceRequestPool._run_inference_batch for forecast models."""
    torch.manual_seed(FIXED_SEED)
    backend = DeviceManager()
    batch_result = batcher.batch_requests(requests)
    batch_inputs = backend.move_tensor(batch_result.batch_inputs, device)
    batch_input_list = [
        {"targets": batch_inputs[i]} for i in range(batch_inputs.size(0))
    ]
    batch_inputs = pipeline.preprocess(
        batch_input_list,
        output_length=batch_result.batch_output_length,
        auto_adapt=True,
    )
    batch_output = pipeline.forecast(
        batch_inputs,
        output_length=batch_result.batch_output_length,
        revin=True,
    )
    batch_output_list = pipeline.postprocess(batch_output)
    stacked = torch.stack(batch_output_list, dim=0)

    outputs = []
    offset = 0
    for request in requests:
        cur_batch_size = request.batch_size
        cur_output = stacked[offset : offset + cur_batch_size]
        offset += cur_batch_size
        remaining = request.remaining_output_length()
        if cur_output.shape[-1] > remaining:
            cur_output = cur_output[..., :remaining]
        request.write_step_output(cur_output)
        outputs.append(
            request.output_tensor[:, :, : request.cur_step_idx].detach().cpu()
        )
    return outputs


def _builtin_model_dir(model_id: str) -> Path:
    return (
        Path(os.getcwd())
        / AINodeDescriptor().get_config().get_ain_models_dir()
        / ModelCategory.BUILTIN.value
        / model_id
    )


def _resolve_model_dir(model_id: str) -> Path | None:
    builtin_dir = _builtin_model_dir(model_id)
    if builtin_dir.exists():
        try:
            validate_model_files(str(builtin_dir))
            return builtin_dir
        except Exception:
            pass
    for cache_root in MODEL_CACHE_CANDIDATES:
        cached_dir = cache_root / model_id
        if not cached_dir.exists():
            continue
        try:
            validate_model_files(str(cached_dir))
            return cached_dir
        except Exception:
            continue
    return None


def _model_weights_available(model_id: str) -> bool:
    return _resolve_model_dir(model_id) is not None


def _link_builtin_models_from_cache() -> None:
    cache_root = next((path for path in MODEL_CACHE_CANDIDATES if path.exists()), None)
    if cache_root is None:
        return
    builtin_root = _builtin_model_dir("").parent
    builtin_root.mkdir(parents=True, exist_ok=True)
    for model_id in LTSM_MODEL_IDS:
        source = cache_root / model_id
        target = builtin_root / model_id
        if not source.exists():
            continue
        try:
            validate_model_files(str(source))
        except Exception:
            continue
        if target.exists() or target.is_symlink():
            try:
                validate_model_files(str(target))
                continue
            except Exception:
                if target.is_dir() and not target.is_symlink():
                    shutil.rmtree(target)
                else:
                    target.unlink(missing_ok=True)
        target.symlink_to(source.resolve())


def _assert_close(
    testcase: unittest.TestCase,
    expected: torch.Tensor,
    actual: torch.Tensor,
    model_id: str,
    message: str,
) -> None:
    tolerance = _tolerance_for_model(model_id)
    testcase.assertTrue(
        torch.allclose(expected, actual, atol=tolerance, rtol=tolerance),
        msg=f"{model_id}: {message} (max diff={(expected - actual).abs().max().item():.6f}, "
        f"tolerance={tolerance})",
    )


class ModelBatchCorrectnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not torch.cuda.is_available():
            raise unittest.SkipTest(
                "CUDA is required for real-model batch correctness tests"
            )
        _link_builtin_models_from_cache()
        cls.device = torch.device("cuda:0")
        cls.available_models = [
            model_id
            for model_id in LTSM_MODEL_IDS
            if _model_weights_available(model_id)
        ]
        if not cls.available_models:
            raise unittest.SkipTest(
                "No built-in LTSM model weights found; "
                "populate ~/.cache/iotdb-ainode-models or /data/ainode/models"
            )
        torch.manual_seed(FIXED_SEED)
        torch.backends.cudnn.deterministic = True
        torch.backends.cudnn.benchmark = False

    def _load_pipeline(self, model_id: str) -> ForecastPipeline:
        model_info = BUILTIN_HF_TRANSFORMERS_MODEL_MAP[model_id]
        pipeline = load_pipeline(model_info, self.device)
        if not isinstance(pipeline, ForecastPipeline):
            self.skipTest(f"Model {model_id} is not a forecast pipeline")
        if hasattr(pipeline, "model"):
            pipeline.model.eval()
        return pipeline

    def test_same_length_dynamic_batch_matches_single_request(self):
        """Mirror IT concurrent dual inference with equal input lengths (96+96)."""
        batcher = DynamicBatcher()
        for model_id in self.available_models:
            with self.subTest(model=model_id):
                try:
                    pipeline = self._load_pipeline(model_id)
                except Exception as exc:
                    self.skipTest(f"Model {model_id} is unavailable locally: {exc}")

                single_a = _run_forecast_batch(
                    pipeline,
                    batcher,
                    self.device,
                    [_make_request(model_id, "solo-a", INPUT_A)],
                )[0]
                single_b = _run_forecast_batch(
                    pipeline,
                    batcher,
                    self.device,
                    [_make_request(model_id, "solo-b", INPUT_A)],
                )[0]
                batched_a, batched_b = _run_forecast_batch(
                    pipeline,
                    batcher,
                    self.device,
                    [
                        _make_request(model_id, "batch-a", INPUT_A),
                        _make_request(model_id, "batch-b", INPUT_A),
                    ],
                )
                _assert_close(
                    self,
                    single_a,
                    batched_a,
                    model_id,
                    "same-length request A mismatch between single and padded batch",
                )
                _assert_close(
                    self,
                    single_b,
                    batched_b,
                    model_id,
                    "same-length request B mismatch between single and padded batch",
                )

    def test_mixed_length_dynamic_batch_longest_request_matches_single(self):
        """Mirror IT mixed-length batching (96+104); only the longer request is asserted.

        Shorter requests are padded to the batch max length before revin normalization,
        so their outputs may legitimately differ from an unpadded single request.
        """
        batcher = DynamicBatcher()
        for model_id in self.available_models:
            with self.subTest(model=model_id):
                try:
                    pipeline = self._load_pipeline(model_id)
                except Exception as exc:
                    self.skipTest(f"Model {model_id} is unavailable locally: {exc}")

                single_b = _run_forecast_batch(
                    pipeline,
                    batcher,
                    self.device,
                    [_make_request(model_id, "solo-b", INPUT_B)],
                )[0]
                _, batched_b = _run_forecast_batch(
                    pipeline,
                    batcher,
                    self.device,
                    [
                        _make_request(model_id, "batch-a", INPUT_A),
                        _make_request(model_id, "batch-b", INPUT_B),
                    ],
                )
                _assert_close(
                    self,
                    single_b,
                    batched_b,
                    model_id,
                    "mixed-length longer request mismatch between single and padded batch",
                )


if __name__ == "__main__":
    unittest.main()
