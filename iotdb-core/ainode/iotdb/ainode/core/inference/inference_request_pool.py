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

import random
import threading
import time
from enum import Enum

import numpy as np
import torch
import torch.multiprocessing as mp

from iotdb.ainode.core.config import AINodeDescriptor
from iotdb.ainode.core.constant import INFERENCE_LOG_FILE_NAME_PREFIX_TEMPLATE
from iotdb.ainode.core.inference.batcher.dynamic_batcher import DynamicBatcher
from iotdb.ainode.core.inference.inference_request import InferenceRequest
from iotdb.ainode.core.inference.pipeline.basic_pipeline import (
    ChatPipeline,
    ClassificationPipeline,
    ForecastPipeline,
)
from iotdb.ainode.core.inference.pipeline.pipeline_loader import load_pipeline
from iotdb.ainode.core.inference.request_scheduler.dynamic_request_scheduler import (
    DynamicRequestScheduler,
)
from iotdb.ainode.core.inference.stats.inference_stats import InferenceStatsCollector
from iotdb.ainode.core.log import Logger
from iotdb.ainode.core.manager.device_manager import DeviceManager
from iotdb.ainode.core.model.model_storage import ModelInfo


class PoolState(Enum):
    INITIALIZING = "INITIALIZING"
    RUNNING = "RUNNING"
    STOPPING = "STOPPING"


class InferenceRequestPool(mp.Process):
    """
    The request pool to handle inference for a specific model.
    """

    FIX_SEED = 2021
    WARMUP_INPUT_LENGTH = 96
    WARMUP_OUTPUT_LENGTH = 1
    WAITING_INTERVAL_IN_MS = (
        AINodeDescriptor().get_config().get_ain_inference_batch_interval_in_ms()
    )

    def __init__(
        self,
        pool_id: int,
        model_info: ModelInfo,
        device: torch.device,
        request_queue: mp.Queue,
        result_queue: mp.Queue,
        ready_event,
        **pool_kwargs,
    ):
        super().__init__()
        self.pool_id = pool_id
        self.model_info = model_info
        self.pool_kwargs = pool_kwargs
        self.ready_event = ready_event
        self.device = device

        self._threads = []
        self._waiting_queue = request_queue
        self._running_queue = mp.Queue()
        self._finished_queue = result_queue
        self._request_scheduler = DynamicRequestScheduler(
            self._waiting_queue,
            self._running_queue,
            self._finished_queue,
            self.pool_id,
        )
        self._batcher = DynamicBatcher()
        self._stats = None
        self._stop_event = mp.Event()

        self._backend = None
        self._inference_pipeline = None
        self._logger = None

        random.seed(self.FIX_SEED)
        torch.manual_seed(self.FIX_SEED)
        np.random.seed(self.FIX_SEED)

    def _activate_requests(self):
        requests = self._request_scheduler.schedule_activate()
        for request in requests:
            request.mark_running()
            self._running_queue.put(request)
            self._logger.debug(
                f"[Inference][{self.device}][Pool-{self.pool_id}][Req-{request.req_id}] "
                f"Request is activated with inputs shape {request.inputs.shape}"
            )

    def _requests_activate_loop(self):
        while not self._stop_event.is_set():
            time.sleep(self.WAITING_INTERVAL_IN_MS / 1000)
            self._activate_requests()

    def _run_inference_batch(
        self, requests: list[InferenceRequest], record_results: bool = True
    ):
        batch_result = self._batcher.batch_requests(requests)
        batch_inputs = self._backend.move_tensor(batch_result.batch_inputs, self.device)
        batch_input_list = []
        for i in range(batch_inputs.size(0)):
            batch_input_list.append({"targets": batch_inputs[i]})
        batch_inputs = self._inference_pipeline.preprocess(
            batch_input_list,
            output_length=batch_result.batch_output_length,
            auto_adapt=True,
        )
        if isinstance(self._inference_pipeline, ForecastPipeline):
            batch_output = self._inference_pipeline.forecast(
                batch_inputs,
                output_length=batch_result.batch_output_length,
                revin=True,
            )
        elif isinstance(self._inference_pipeline, ClassificationPipeline):
            batch_output = self._inference_pipeline.classify(batch_inputs)
        elif isinstance(self._inference_pipeline, ChatPipeline):
            batch_output = self._inference_pipeline.chat(batch_inputs)
        else:
            batch_output = None
            self._logger.error("[Inference] Unsupported pipeline type.")
        batch_output_list = self._inference_pipeline.postprocess(batch_output)
        batch_output = torch.stack([output for output in batch_output_list], dim=0)

        offset = 0
        finished_requests = []
        for request in requests:
            request.output_tensor = self._backend.move_tensor(
                request.output_tensor, self.device
            )
            cur_batch_size = request.batch_size
            cur_output = batch_output[offset : offset + cur_batch_size]
            offset += cur_batch_size
            remaining = request.remaining_output_length()
            if cur_output.shape[-1] > remaining:
                cur_output = cur_output[..., :remaining]
            request.write_step_output(cur_output)

            if request.is_finished():
                request.output_tensor = request.output_tensor.cpu()
                if record_results:
                    self._finished_queue.put(request)
                    finished_requests.append(request)
                self._logger.debug(
                    f"[Inference][{self.device}][Pool-{self.pool_id}][ID-{request.req_id}] "
                    "Request is finished"
                )
            else:
                self._waiting_queue.put(request)
                self._logger.debug(
                    f"[Inference][{self.device}][Pool-{self.pool_id}][ID-{request.req_id}] "
                    "Request is not finished, re-queueing"
                )

        if finished_requests and record_results:
            self._stats.record_batch(finished_requests, len(requests))

    def _step(self):
        all_requests: list[InferenceRequest] = self._request_scheduler.schedule_step()
        if not all_requests:
            return

        for requests in self._request_scheduler.form_inference_batches(all_requests):
            self._run_inference_batch(requests)

    def _warmup(self):
        dummy_inputs = torch.randn(
            1,
            1,
            self.WARMUP_INPUT_LENGTH,
            dtype=torch.float32,
        )
        warmup_request = InferenceRequest(
            req_id="warmup",
            model_id=self.model_info.model_id,
            inputs=dummy_inputs,
            output_length=self.WARMUP_OUTPUT_LENGTH,
        )
        try:
            self._run_inference_batch([warmup_request], record_results=False)
            if self.device.type == "cuda":
                torch.cuda.synchronize(self.device)
            self._logger.info(
                f"[Inference][{self.device}][Pool-{self.pool_id}] "
                f"Warmup inference completed for model {self.model_info.model_id}"
            )
        except Exception as exc:
            self._logger.warning(
                f"[Inference][{self.device}][Pool-{self.pool_id}] "
                f"Warmup inference failed (non-fatal): {exc}"
            )

    def _requests_execute_loop(self):
        while not self._stop_event.is_set():
            time.sleep(self.WAITING_INTERVAL_IN_MS / 1000)
            self._step()

    def run(self):
        self._logger = Logger(
            INFERENCE_LOG_FILE_NAME_PREFIX_TEMPLATE.format(self.device)
        )
        self._backend = DeviceManager()
        self._request_scheduler.device = self.device
        self._stats = InferenceStatsCollector(
            pool_id=self.pool_id,
            model_id=self.model_info.model_id,
            device=self.device,
        )
        self._inference_pipeline = load_pipeline(self.model_info, self.device)
        self._warmup()
        self.ready_event.set()

        activate_daemon = threading.Thread(
            target=self._requests_activate_loop, daemon=True
        )
        self._threads.append(activate_daemon)
        activate_daemon.start()
        execute_daemon = threading.Thread(
            target=self._requests_execute_loop, daemon=True
        )
        self._threads.append(execute_daemon)
        execute_daemon.start()
        self._logger.info(
            f"[Inference][{self.device}][Pool-{self.pool_id}] "
            f"InferenceRequestPool for model {self.model_info.model_id} is activated."
        )
        for thread in self._threads:
            thread.join()
        self._logger.info(
            f"[Inference][{self.device}][Pool-{self.pool_id}] "
            f"InferenceRequestPool for model {self.model_info.model_id} exited cleanly."
        )

    def stop(self):
        self._stop_event.set()
        if self._request_scheduler is not None:
            self._request_scheduler.flush_pending_to_queues()
