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

import queue
import time
import unittest

import torch

from iotdb.ainode.core.inference.batcher.batch_utils import (
    batch_group_key,
    bucket_length,
    group_requests_by_bucket,
)
from iotdb.ainode.core.inference.batcher.dynamic_batcher import DynamicBatcher
from iotdb.ainode.core.inference.inference_request import InferenceRequest
from iotdb.ainode.core.inference.request_scheduler.dynamic_request_scheduler import (
    DynamicRequestScheduler,
)


class BatchUtilsTest(unittest.TestCase):
    def test_bucket_length(self):
        self.assertEqual(bucket_length(50, 32), 64)
        self.assertEqual(bucket_length(64, 32), 64)
        self.assertEqual(bucket_length(65, 0), 65)

    def test_group_requests_by_bucket(self):
        req_a = InferenceRequest("a", "m", torch.zeros(1, 1, 100), output_length=48)
        req_b = InferenceRequest("b", "m", torch.zeros(1, 1, 110), output_length=50)
        req_c = InferenceRequest("c", "m", torch.zeros(1, 1, 200), output_length=96)
        groups = group_requests_by_bucket([req_a, req_b, req_c], 32, 16)
        self.assertEqual(len(groups), 2)
        self.assertEqual(
            batch_group_key(req_a, 32, 16),
            batch_group_key(req_b, 32, 16),
        )


class DynamicBatcherTest(unittest.TestCase):
    def test_pad_and_batch(self):
        req_short = InferenceRequest("s", "m", torch.ones(1, 2, 100), output_length=48)
        req_long = InferenceRequest("l", "m", torch.ones(1, 2, 128), output_length=50)
        batcher = DynamicBatcher(
            input_length_bucket_size=32, output_length_bucket_size=16
        )
        result = batcher.batch_requests([req_short, req_long])
        self.assertEqual(result.batch_inputs.shape, (2, 2, 128))
        self.assertEqual(result.batch_output_length, 50)
        self.assertTrue(torch.all(result.batch_inputs[0, :, 100:] == 1))


class DynamicRequestSchedulerTest(unittest.TestCase):
    def test_deadline_forces_dispatch(self):
        waiting = queue.Queue()
        running = queue.Queue()
        finished = queue.Queue()
        scheduler = DynamicRequestScheduler(
            waiting,
            running,
            finished,
            pool_id=0,
            batch_waiting_window_ms=10_000,
            batch_deadline_ms=1,
            max_batch_size=8,
            max_batch_points=1_000_000,
        )
        req = InferenceRequest("r1", "m", torch.zeros(1, 1, 32), output_length=16)
        req.enqueue_time = time.time() - 1
        waiting.put(req)
        activated = scheduler.schedule_activate()
        self.assertEqual(len(activated), 1)


if __name__ == "__main__":
    unittest.main()
