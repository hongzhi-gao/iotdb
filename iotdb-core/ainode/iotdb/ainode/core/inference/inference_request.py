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
from typing import Any, Optional

import torch

from iotdb.ainode.core.log import Logger
from iotdb.ainode.core.util.atmoic_int import AtomicInt

logger = Logger()


class InferenceRequestState:
    WAITING = "waiting"
    RUNNING = "running"
    FINISHED = "finished"


class InferenceRequest:
    def __init__(
        self,
        req_id: str,
        model_id: str,
        inputs: torch.Tensor,
        output_length: int = 96,
        **infer_kwargs,
    ):
        while inputs.ndim < 3:
            inputs = inputs.unsqueeze(0)

        self.req_id = req_id
        self.model_id = model_id
        self.inputs = inputs
        self.infer_kwargs = infer_kwargs
        self.output_length = output_length

        self.batch_size = inputs.size(0)
        self.target_count = inputs.size(1)
        self.input_length = inputs.size(2)
        self.state = InferenceRequestState.WAITING
        self.cur_step_idx = 0  # Current write position in the output step index
        self.assigned_pool_id = -1  # The pool handling this request
        self.assigned_device_id = -1  # The device handling this request

        now = time.time()
        self.enqueue_time: float = now
        self.activate_time: Optional[float] = None
        self.finish_time: Optional[float] = None

        # Preallocate output buffer [batch_size, target_count, output_length]
        self.output_tensor = torch.zeros(
            self.batch_size, self.target_count, output_length, device="cpu"
        )

    def mark_running(self):
        self.state = InferenceRequestState.RUNNING
        if self.activate_time is None:
            self.activate_time = time.time()

    def mark_finished(self):
        self.state = InferenceRequestState.FINISHED
        if self.finish_time is None:
            self.finish_time = time.time()

    def is_finished(self) -> bool:
        return (
            self.state == InferenceRequestState.FINISHED
            or self.cur_step_idx >= self.output_length
        )

    def remaining_output_length(self) -> int:
        return max(0, self.output_length - self.cur_step_idx)

    def waited_ms(self, now: Optional[float] = None) -> float:
        reference = now if now is not None else time.time()
        return max(0.0, (reference - self.enqueue_time) * 1000)

    def is_past_deadline(self, deadline_ms: float, now: Optional[float] = None) -> bool:
        return self.waited_ms(now) >= deadline_ms

    def batch_points(self) -> int:
        return self.batch_size * self.target_count * self.input_length

    def write_step_output(self, step_output: torch.Tensor):
        while step_output.ndim < 3:
            step_output = step_output.unsqueeze(0)

        batch_size, target_count, step_size = step_output.shape
        end_idx = self.cur_step_idx + step_size

        if end_idx > self.output_length:
            self.output_tensor[:, :, self.cur_step_idx :] = step_output[
                :, :, : self.output_length - self.cur_step_idx
            ]
            self.cur_step_idx = self.output_length
        else:
            self.output_tensor[:, :, self.cur_step_idx : end_idx] = step_output
            self.cur_step_idx = end_idx

        if self.is_finished():
            self.mark_finished()

    def get_final_output(self) -> torch.Tensor:
        return self.output_tensor[:, :, : self.cur_step_idx]


class InferenceRequestProxy:
    """
    Wrap the raw request for handling multiprocess processing.
    """

    def __init__(self, req_id: str):
        self.req_id = req_id
        self.result = None
        self._counter: AtomicInt = None
        self._lock = threading.Lock()
        self._condition = threading.Condition(self._lock)

    def set_result(self, result: Any):
        with self._lock:
            self.result = result
            if self._counter is not None:
                self._counter.decrement_and_get()
            self._condition.notify_all()

    def set_counter(self, counter: AtomicInt):
        with self._lock:
            self._counter = counter

    def wait_for_result(self) -> Any:
        with self._lock:
            self._condition.wait()
            return self.result
