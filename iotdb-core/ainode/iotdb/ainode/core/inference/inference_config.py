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

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Optional

import psutil
import torch

from iotdb.ainode.core.constant import (
    AINODE_INFERENCE_EXTRA_MEMORY_RATIO,
    AINODE_INFERENCE_INPUT_LENGTH_BUCKET_SIZE,
    AINODE_INFERENCE_MAX_ACTIVATE_SIZE,
    AINODE_INFERENCE_MAX_MEMORY_BYTES,
    AINODE_INFERENCE_MAX_STEP_SIZE,
    AINODE_INFERENCE_MODEL_MEM_USAGE_MAP,
    AINODE_INFERENCE_OUTPUT_LENGTH_BUCKET_SIZE,
    AINODE_INFERENCE_SCHEDULING_PROFILE,
    AINODE_INFERENCE_SCHEDULING_PROFILES,
)
from iotdb.ainode.core.log import Logger

logger = Logger()

REFERENCE_MODEL_ID = "sundial"
BYTES_PER_MB = 1024**2
BYTES_PER_GB = 1024**3


@dataclass(frozen=True)
class ResolvedInferenceConfig:
    memory_usage_ratio: float
    scheduling_profile: str
    batch_interval_ms: int
    batch_waiting_window_ms: int
    batch_deadline_ms: int
    max_batch_size: int
    max_batch_points: int
    input_length_bucket_size: int
    output_length_bucket_size: int
    max_memory_bytes: int
    max_activate_size: int
    max_step_size: int
    max_queue_size: int
    extra_memory_ratio: float
    model_mem_usage_map: Dict[str, int]
    device_type: str
    total_memory_bytes: int
    inference_budget_bytes: int
    recommended_pool_count: int


def detect_primary_device_type() -> str:
    if torch.cuda.is_available() and torch.cuda.device_count() > 0:
        return "cuda"
    return "cpu"


def get_device_total_memory_bytes(device_type: str) -> int:
    if device_type == "cuda":
        _, total = torch.cuda.mem_get_info(0)
        return int(total)
    return int(psutil.virtual_memory().total)


def default_memory_usage_ratio(device_type: str, total_memory_bytes: int) -> float:
    if device_type != "cuda":
        return 0.25
    total_gb = total_memory_bytes / BYTES_PER_GB
    if total_gb < 10:
        return 0.45
    if total_gb < 18:
        return 0.50
    return 0.55


def resolve_scheduling_profile(profile: str) -> Dict[str, int]:
    normalized = (profile or AINODE_INFERENCE_SCHEDULING_PROFILE).strip().lower()
    if normalized not in AINODE_INFERENCE_SCHEDULING_PROFILES:
        logger.warning(
            f"Unknown ain_inference_scheduling_profile '{profile}', "
            "fall back to 'balanced'"
        )
        normalized = "balanced"
    return dict(AINODE_INFERENCE_SCHEDULING_PROFILES[normalized])


def recommend_max_batch_size(
    device_type: str, total_memory_bytes: int, memory_usage_ratio: float
) -> int:
    total_gb = total_memory_bytes / BYTES_PER_GB
    if device_type == "cuda":
        if total_gb < 10:
            base = 16
        elif total_gb < 18:
            base = 32
        elif total_gb < 26:
            base = 48
        else:
            base = 64
        # Tighter memory budget → smaller batches (baseline ratio 0.5).
        return max(8, int(base * min(memory_usage_ratio / 0.5, 1.0)))
    if total_gb <= 8:
        base = 4
    elif total_gb <= 16:
        base = 8
    else:
        base = 16
    return max(2, int(base * min(memory_usage_ratio / 0.25, 1.0)))


def recommend_max_batch_points(max_batch_size: int) -> int:
    return max(4096, max_batch_size * 2048)


def recommend_max_memory_bytes(
    total_memory_bytes: int, memory_usage_ratio: float, recommended_pools: int
) -> int:
    inference_budget = int(total_memory_bytes * memory_usage_ratio)
    if recommended_pools <= 0:
        return AINODE_INFERENCE_MAX_MEMORY_BYTES
    per_pool = inference_budget // max(recommended_pools, 1)
    return max(
        256 * BYTES_PER_MB, min(AINODE_INFERENCE_MAX_MEMORY_BYTES, per_pool // 4)
    )


def recommend_activate_step_size(max_batch_size: int) -> int:
    return min(AINODE_INFERENCE_MAX_ACTIVATE_SIZE, max(16, max_batch_size * 2))


def recommend_max_queue_size(max_batch_size: int) -> int:
    # vLLM-style bounded admission: allow several batches worth of waiting requests.
    return max(64, max_batch_size * 8)


def estimate_recommended_pool_count(
    device_type: str,
    total_memory_bytes: int,
    memory_usage_ratio: float,
    extra_memory_ratio: float,
    model_mem_usage_map: Dict[str, int],
) -> int:
    if REFERENCE_MODEL_ID not in model_mem_usage_map:
        return 1
    free_mem = total_memory_bytes
    if device_type == "cuda":
        free_mem, _ = torch.cuda.mem_get_info(0)
    mem_usage = int(model_mem_usage_map[REFERENCE_MODEL_ID] * extra_memory_ratio)
    if mem_usage <= 0:
        return 1
    size = int((free_mem * memory_usage_ratio) // mem_usage)
    return max(1, size)


def resolve_inference_config(
    *,
    memory_usage_ratio: Optional[float],
    scheduling_profile: str,
    max_batch_size: Optional[int],
    batch_interval_ms: Optional[int],
    batch_waiting_window_ms: Optional[int],
    batch_deadline_ms: Optional[int],
    max_batch_points: Optional[int],
    input_length_bucket_size: Optional[int],
    output_length_bucket_size: Optional[int],
    max_memory_bytes: Optional[int],
    max_activate_size: Optional[int],
    max_step_size: Optional[int],
    max_queue_size: Optional[int],
    extra_memory_ratio: Optional[float],
    model_mem_usage_map: Optional[Dict[str, int]],
) -> ResolvedInferenceConfig:
    device_type = detect_primary_device_type()
    total_memory_bytes = get_device_total_memory_bytes(device_type)
    resolved_memory_ratio = (
        memory_usage_ratio
        if memory_usage_ratio is not None
        else default_memory_usage_ratio(device_type, total_memory_bytes)
    )
    resolved_extra_ratio = (
        extra_memory_ratio
        if extra_memory_ratio is not None
        else AINODE_INFERENCE_EXTRA_MEMORY_RATIO
    )
    resolved_model_map = (
        dict(model_mem_usage_map)
        if model_mem_usage_map is not None
        else dict(AINODE_INFERENCE_MODEL_MEM_USAGE_MAP)
    )
    profile_name = (scheduling_profile or AINODE_INFERENCE_SCHEDULING_PROFILE).lower()
    profile_values = resolve_scheduling_profile(profile_name)

    resolved_batch_interval = (
        batch_interval_ms
        if batch_interval_ms is not None
        else profile_values["batch_interval_ms"]
    )
    resolved_waiting_window = (
        batch_waiting_window_ms
        if batch_waiting_window_ms is not None
        else profile_values["batch_waiting_window_ms"]
    )
    resolved_deadline = (
        batch_deadline_ms
        if batch_deadline_ms is not None
        else profile_values["batch_deadline_ms"]
    )
    resolved_max_batch_size = (
        max_batch_size
        if max_batch_size is not None
        else recommend_max_batch_size(
            device_type, total_memory_bytes, resolved_memory_ratio
        )
    )
    resolved_max_batch_points = (
        max_batch_points
        if max_batch_points is not None
        else recommend_max_batch_points(resolved_max_batch_size)
    )
    resolved_input_bucket = (
        input_length_bucket_size
        if input_length_bucket_size is not None
        else AINODE_INFERENCE_INPUT_LENGTH_BUCKET_SIZE
    )
    resolved_output_bucket = (
        output_length_bucket_size
        if output_length_bucket_size is not None
        else AINODE_INFERENCE_OUTPUT_LENGTH_BUCKET_SIZE
    )
    recommended_pools = estimate_recommended_pool_count(
        device_type,
        total_memory_bytes,
        resolved_memory_ratio,
        resolved_extra_ratio,
        resolved_model_map,
    )
    resolved_max_memory_bytes = (
        max_memory_bytes
        if max_memory_bytes is not None
        else recommend_max_memory_bytes(
            total_memory_bytes, resolved_memory_ratio, recommended_pools
        )
    )
    resolved_activate_size = (
        max_activate_size
        if max_activate_size is not None
        else recommend_activate_step_size(resolved_max_batch_size)
    )
    resolved_step_size = (
        max_step_size
        if max_step_size is not None
        else recommend_activate_step_size(resolved_max_batch_size)
    )
    resolved_max_queue_size = (
        max_queue_size
        if max_queue_size is not None
        else recommend_max_queue_size(resolved_max_batch_size)
    )
    inference_budget_bytes = int(total_memory_bytes * resolved_memory_ratio)

    return ResolvedInferenceConfig(
        memory_usage_ratio=resolved_memory_ratio,
        scheduling_profile=profile_name,
        batch_interval_ms=resolved_batch_interval,
        batch_waiting_window_ms=resolved_waiting_window,
        batch_deadline_ms=resolved_deadline,
        max_batch_size=resolved_max_batch_size,
        max_batch_points=resolved_max_batch_points,
        input_length_bucket_size=resolved_input_bucket,
        output_length_bucket_size=resolved_output_bucket,
        max_memory_bytes=resolved_max_memory_bytes,
        max_activate_size=resolved_activate_size,
        max_step_size=resolved_step_size,
        max_queue_size=resolved_max_queue_size,
        extra_memory_ratio=resolved_extra_ratio,
        model_mem_usage_map=resolved_model_map,
        device_type=device_type,
        total_memory_bytes=total_memory_bytes,
        inference_budget_bytes=inference_budget_bytes,
        recommended_pool_count=recommended_pools,
    )


def format_bytes(num_bytes: int) -> str:
    if num_bytes >= BYTES_PER_GB:
        return f"{num_bytes / BYTES_PER_GB:.1f} GB"
    return f"{num_bytes / BYTES_PER_MB:.0f} MB"


def log_inference_config_summary(config: ResolvedInferenceConfig) -> None:
    logger.info(
        f"[InferenceConfig] device={config.device_type}, "
        f"total_memory={format_bytes(config.total_memory_bytes)}, "
        f"inference_budget={format_bytes(config.inference_budget_bytes)} "
        f"(memory_usage_ratio={config.memory_usage_ratio:.2f}), "
        f"scheduling_profile={config.scheduling_profile}, "
        f"batch_interval/window/deadline="
        f"{config.batch_interval_ms}/{config.batch_waiting_window_ms}/"
        f"{config.batch_deadline_ms} ms, "
        f"max_batch_size={config.max_batch_size}, "
        f"max_batch_points={config.max_batch_points}, "
        f"recommended_pools({REFERENCE_MODEL_ID})={config.recommended_pool_count}, "
        f"scheduler_reserve={format_bytes(config.max_memory_bytes)}, "
        f"activate/step={config.max_activate_size}/{config.max_step_size}, "
        f"max_queue_size={config.max_queue_size}, "
        f"input/output_bucket="
        f"{config.input_length_bucket_size}/{config.output_length_bucket_size}"
    )
