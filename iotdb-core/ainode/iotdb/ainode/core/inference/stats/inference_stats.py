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

import threading
import time
from typing import Iterable, List, Optional

from iotdb.ainode.core.config import AINodeDescriptor
from iotdb.ainode.core.inference.batcher.batch_result import BatchResult
from iotdb.ainode.core.inference.inference_request import InferenceRequest
from iotdb.ainode.core.log import Logger

logger = Logger()


def _percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    sorted_values = sorted(values)
    index = int(round((pct / 100.0) * (len(sorted_values) - 1)))
    return sorted_values[index]


class InferenceStatsCollector:
    """
    Collects inference latency, QPS, batch-size distribution, and batching
    signals for a pool.
    """

    def __init__(
        self,
        pool_id: int,
        model_id: str,
        device,
        log_interval_in_sec: Optional[int] = None,
    ):
        config = AINodeDescriptor().get_config()
        self.pool_id = pool_id
        self.model_id = model_id
        self.device = device
        self.log_interval_in_sec = (
            log_interval_in_sec
            if log_interval_in_sec is not None
            else config.get_ain_inference_stats_log_interval_in_sec()
        )
        self._lock = threading.Lock()
        self._latencies_ms: List[float] = []
        self._batch_sizes: List[int] = []
        self._padding_waste_ratios: List[float] = []
        self._queue_depths: List[int] = []
        self._deadline_forced_batches = 0
        self._completed_requests = 0
        self._window_start = time.time()
        self._last_log_time = self._window_start

    def record_deadline_forced_batches(self, count: int):
        if count <= 0:
            return
        with self._lock:
            self._deadline_forced_batches += count

    def record_batch(
        self,
        requests: Iterable[InferenceRequest],
        batch_size: int,
        batch_result: Optional[BatchResult] = None,
        queue_depth: int = 0,
    ):
        finished_at = time.time()
        with self._lock:
            self._batch_sizes.append(batch_size)
            self._queue_depths.append(queue_depth)
            if batch_result is not None:
                self._padding_waste_ratios.append(batch_result.padding_waste_ratio())
            for req in requests:
                if req.finish_time is None:
                    req.finish_time = finished_at
                latency_ms = (req.finish_time - req.enqueue_time) * 1000
                self._latencies_ms.append(latency_ms)
                self._completed_requests += 1
            self._maybe_log_locked(finished_at)

    def _maybe_log_locked(self, now: float):
        if now - self._last_log_time < self.log_interval_in_sec:
            return
        self._emit_summary_locked(now)
        self._latencies_ms.clear()
        self._batch_sizes.clear()
        self._padding_waste_ratios.clear()
        self._queue_depths.clear()
        self._deadline_forced_batches = 0
        self._window_start = now
        self._last_log_time = now

    def _emit_summary_locked(self, now: float):
        elapsed = max(now - self._window_start, 1e-6)
        qps = self._completed_requests / elapsed
        avg_padding_waste = (
            sum(self._padding_waste_ratios) / len(self._padding_waste_ratios)
            if self._padding_waste_ratios
            else None
        )
        avg_queue_depth = (
            sum(self._queue_depths) / len(self._queue_depths)
            if self._queue_depths
            else None
        )
        if not self._latencies_ms:
            logger.info(
                f"[InferenceStats][{self.device}][Pool-{self.pool_id}][Model-{self.model_id}] "
                f"qps={qps:.2f}, completed={self._completed_requests}, "
                f"latency_p50/p95/p99=NA, batch_size_avg=NA, "
                f"padding_waste_avg={self._fmt_optional(avg_padding_waste)}, "
                f"queue_depth_avg={self._fmt_optional(avg_queue_depth)}, "
                f"deadline_forced_batches={self._deadline_forced_batches}"
            )
            self._completed_requests = 0
            return

        avg_batch_size = sum(self._batch_sizes) / len(self._batch_sizes)
        logger.info(
            f"[InferenceStats][{self.device}][Pool-{self.pool_id}][Model-{self.model_id}] "
            f"qps={qps:.2f}, completed={self._completed_requests}, "
            f"latency_p50={_percentile(self._latencies_ms, 50):.2f}ms, "
            f"latency_p95={_percentile(self._latencies_ms, 95):.2f}ms, "
            f"latency_p99={_percentile(self._latencies_ms, 99):.2f}ms, "
            f"batch_size_avg={avg_batch_size:.2f}, batch_count={len(self._batch_sizes)}, "
            f"padding_waste_avg={self._fmt_optional(avg_padding_waste)}, "
            f"queue_depth_avg={self._fmt_optional(avg_queue_depth)}, "
            f"deadline_forced_batches={self._deadline_forced_batches}"
        )
        self._completed_requests = 0

    @staticmethod
    def _fmt_optional(value: Optional[float]) -> str:
        if value is None:
            return "NA"
        return f"{value:.4f}"
