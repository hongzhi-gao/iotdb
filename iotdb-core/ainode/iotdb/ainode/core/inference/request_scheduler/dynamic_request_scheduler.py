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

import time
from queue import Empty
from typing import List

import psutil
import torch

from iotdb.ainode.core.config import AINodeDescriptor
from iotdb.ainode.core.inference.batcher.batch_utils import (
    batch_points,
    group_requests_by_bucket,
)
from iotdb.ainode.core.inference.inference_request import InferenceRequest
from iotdb.ainode.core.inference.request_scheduler.abstract_request_scheduler import (
    AbstractRequestScheduler,
)
from iotdb.ainode.core.log import Logger

logger = Logger()


class DynamicRequestScheduler(AbstractRequestScheduler):
    """
    Deadline-aware scheduler with batch waiting window and length-bucket batching.
    """

    def __init__(
        self,
        waiting_queue,
        running_queue,
        finished_queue,
        pool_id,
        max_memory_bytes=1 << 30,
        max_activate_size=None,
        max_step_size=None,
        batch_waiting_window_ms=None,
        batch_deadline_ms=None,
        max_batch_size=None,
        max_batch_points=None,
        input_length_bucket_size=None,
        output_length_bucket_size=None,
    ):
        super().__init__(waiting_queue, running_queue, finished_queue)
        config = AINodeDescriptor().get_config()
        self.max_memory_bytes = (
            max_memory_bytes
            if max_memory_bytes is not None
            else config.get_ain_inference_max_memory_bytes()
        )
        self.max_activate_size = (
            max_activate_size
            if max_activate_size is not None
            else config.get_ain_inference_max_activate_size()
        )
        self.max_step_size = (
            max_step_size
            if max_step_size is not None
            else config.get_ain_inference_max_step_size()
        )
        self.batch_waiting_window_ms = (
            batch_waiting_window_ms
            if batch_waiting_window_ms is not None
            else config.get_ain_inference_batch_waiting_window_in_ms()
        )
        self.batch_deadline_ms = (
            batch_deadline_ms
            if batch_deadline_ms is not None
            else config.get_ain_inference_batch_deadline_in_ms()
        )
        self.max_batch_size = (
            max_batch_size
            if max_batch_size is not None
            else config.get_ain_inference_max_batch_size()
        )
        self.max_batch_points = (
            max_batch_points
            if max_batch_points is not None
            else config.get_ain_inference_max_batch_points()
        )
        self.input_length_bucket_size = (
            input_length_bucket_size
            if input_length_bucket_size is not None
            else config.get_ain_inference_input_length_bucket_size()
        )
        self.output_length_bucket_size = (
            output_length_bucket_size
            if output_length_bucket_size is not None
            else config.get_ain_inference_output_length_bucket_size()
        )
        self.pool_id = pool_id
        self.device = None
        self._pending_waiting: List[InferenceRequest] = []
        self._pending_running: List[InferenceRequest] = []
        self._deadline_forced_batches = 0

    def consume_deadline_forced_batches(self) -> int:
        count = self._deadline_forced_batches
        self._deadline_forced_batches = 0
        return count

    def memory_is_available(self):
        if self.device is None:
            return True
        if "cuda" in self.device.type:
            available, total = torch.cuda.mem_get_info(self.device)
        elif "cpu" in self.device.type:
            memory = psutil.virtual_memory()
            available = memory.available
            total = memory.total
        else:
            logger.warning(
                f"[Inference] Unsupported device type: {self.device.type}. "
                "Memory checks will not be performed."
            )
            return True
        logger.debug(
            f"[Inference][Device-{self.device}][Pool-{self.pool_id}] "
            f"Memory available: {available/1024**2:.2f} MB, Total memory: {total/1024**2:.2f} MB, "
            f"Required free memory: {self.max_memory_bytes/1024**2:.2f} MB"
        )
        return available > self.max_memory_bytes

    def _drain_queue(self, queue, buffer: List[InferenceRequest]):
        while not queue.empty():
            try:
                buffer.append(queue.get_nowait())
            except Empty:
                break

    def _should_dispatch_batch(
        self, batch: List[InferenceRequest], now: float, force: bool = False
    ) -> bool:
        if not batch:
            return False
        if force:
            return True
        oldest_wait_ms = min(req.waited_ms(now) for req in batch)
        if oldest_wait_ms >= self.batch_waiting_window_ms:
            return True
        if any(req.is_past_deadline(self.batch_deadline_ms, now) for req in batch):
            return True
        if len(batch) >= self.max_batch_size:
            return True
        if batch_points(batch) >= self.max_batch_points:
            return True
        return False

    def _build_dispatch_batches(
        self,
        requests: List[InferenceRequest],
        now: float,
        max_total: int,
        force_deadline: bool = False,
    ) -> tuple[List[InferenceRequest], List[InferenceRequest]]:
        if not requests:
            return [], []

        grouped = group_requests_by_bucket(
            requests,
            self.input_length_bucket_size,
            self.output_length_bucket_size,
        )
        for group in grouped:
            group.sort(key=lambda req: req.enqueue_time)

        dispatched: List[InferenceRequest] = []
        remaining = list(requests)
        remaining_ids = {req.req_id for req in remaining}

        grouped.sort(
            key=lambda group: (
                not any(
                    req.is_past_deadline(self.batch_deadline_ms, now) for req in group
                ),
                min(req.enqueue_time for req in group),
            )
        )

        for group in grouped:
            if len(dispatched) >= max_total or not self.memory_is_available():
                break
            batch: List[InferenceRequest] = []
            for req in group:
                if req.req_id not in remaining_ids:
                    continue
                candidate = batch + [req]
                if len(candidate) > self.max_batch_size:
                    break
                if batch_points(candidate) > self.max_batch_points:
                    break
                batch = candidate
                if len(dispatched) + len(batch) >= max_total:
                    break

            if not batch:
                continue

            force = force_deadline and any(
                req.is_past_deadline(self.batch_deadline_ms, now) for req in batch
            )
            if self._should_dispatch_batch(batch, now, force=force):
                if any(
                    req.is_past_deadline(self.batch_deadline_ms, now) for req in batch
                ):
                    self._deadline_forced_batches += 1
                dispatched.extend(batch)
                for req in batch:
                    remaining_ids.discard(req.req_id)

        remaining = [req for req in remaining if req.req_id in remaining_ids]
        return dispatched, remaining

    def schedule_activate(self) -> list:
        self._drain_queue(self.waiting_queue, self._pending_waiting)
        if not self._pending_waiting:
            return []

        now = time.time()
        activated, self._pending_waiting = self._build_dispatch_batches(
            self._pending_waiting,
            now,
            self.max_activate_size,
        )
        return activated

    def schedule_step(self) -> list:
        self._drain_queue(self.running_queue, self._pending_running)
        if not self._pending_running:
            return []

        now = time.time()
        stepped, self._pending_running = self._build_dispatch_batches(
            self._pending_running,
            now,
            self.max_step_size,
            force_deadline=True,
        )
        return stepped

    def form_inference_batches(
        self, requests: List[InferenceRequest]
    ) -> List[List[InferenceRequest]]:
        if not requests:
            return []

        grouped = group_requests_by_bucket(
            requests,
            self.input_length_bucket_size,
            self.output_length_bucket_size,
        )
        batches: List[List[InferenceRequest]] = []
        for group in grouped:
            group.sort(key=lambda req: req.enqueue_time)
            current: List[InferenceRequest] = []
            for req in group:
                candidate = current + [req]
                if len(candidate) > self.max_batch_size:
                    if current:
                        batches.append(current)
                    current = [req]
                    continue
                if batch_points(candidate) > self.max_batch_points:
                    if current:
                        batches.append(current)
                    current = [req]
                    continue
                current = candidate
            if current:
                batches.append(current)
        return batches

    def requeue_running(self, requests: List[InferenceRequest]):
        self._pending_running.extend(requests)

    def flush_pending_to_queues(self):
        for req in self._pending_waiting:
            self.waiting_queue.put(req)
        self._pending_waiting.clear()
        for req in self._pending_running:
            self.running_queue.put(req)
        self._pending_running.clear()
