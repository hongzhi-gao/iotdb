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

from iotdb.ainode.core.exception import InferenceOverloadException
from iotdb.ainode.core.inference.batcher.batch_utils import (
    batch_group_key,
    bucket_length,
    group_requests_by_bucket,
)
from iotdb.ainode.core.inference.batcher.dynamic_batcher import DynamicBatcher
from iotdb.ainode.core.inference.inference_request import (
    InferenceRequest,
    InferenceRequestProxy,
)
from iotdb.ainode.core.inference.pool_group import PoolGroup
from iotdb.ainode.core.inference.request_scheduler.dynamic_request_scheduler import (
    DynamicRequestScheduler,
)
from iotdb.ainode.core.inference.stats.admission_stats import AdmissionStats
from iotdb.ainode.core.util.atmoic_int import AtomicInt


def _mock_last_value_forecast(
    batch_inputs: torch.Tensor, output_length: int
) -> torch.Tensor:
    last = batch_inputs[..., -1:]
    return last.expand(*batch_inputs.shape[:-1], output_length)


class BatchUtilsTest(unittest.TestCase):
    def test_bucket_length(self):
        self.assertEqual(bucket_length(50, 32), 64)
        self.assertEqual(bucket_length(64, 32), 64)
        self.assertEqual(bucket_length(65, 0), 65)

    def test_group_requests_by_bucket(self):
        req_a = InferenceRequest("a", "m", torch.zeros(1, 1, 100), output_length=48)
        req_b = InferenceRequest("b", "m", torch.zeros(1, 1, 110), output_length=40)
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

    def test_padding_waste_ratio(self):
        req_short = InferenceRequest("s", "m", torch.ones(1, 2, 100), output_length=48)
        req_long = InferenceRequest("l", "m", torch.ones(1, 2, 128), output_length=50)
        batcher = DynamicBatcher(
            input_length_bucket_size=32, output_length_bucket_size=16
        )
        result = batcher.batch_requests([req_short, req_long])
        self.assertGreater(result.padding_waste_ratio(), 0.0)
        self.assertLess(result.padding_waste_ratio(), 1.0)

    def test_padded_batch_matches_single_request_outputs(self):
        torch.manual_seed(0)
        req_short = InferenceRequest("s", "m", torch.randn(1, 2, 100), output_length=4)
        req_long = InferenceRequest("l", "m", torch.randn(1, 2, 128), output_length=4)
        batcher = DynamicBatcher(
            input_length_bucket_size=32, output_length_bucket_size=16
        )
        batch_result = batcher.batch_requests([req_short, req_long])
        batch_output = _mock_last_value_forecast(
            batch_result.batch_inputs, batch_result.batch_output_length
        )
        single_outputs = [
            _mock_last_value_forecast(req.inputs, 4) for req in (req_short, req_long)
        ]
        self.assertTrue(torch.allclose(batch_output[0], single_outputs[0]))
        self.assertTrue(torch.allclose(batch_output[1], single_outputs[1]))


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
        self.assertEqual(scheduler.consume_deadline_forced_batches(), 1)


class PoolGroupAdmissionTest(unittest.TestCase):
    def test_queue_full_rejects_request(self):
        pool_group = PoolGroup("sundial")
        waiting_queue = queue.Queue(maxsize=1)
        waiting_queue.put(
            InferenceRequest(
                "existing", "sundial", torch.zeros(1, 1, 16), output_length=4
            )
        )
        pool_group.pool_group[0] = (None, waiting_queue)
        pool_group.pool_states[0] = None
        pool_group.pool_remaining_reqs[0] = AtomicInt()
        pool_group.request_dispatcher.dispatch_request = lambda req, pool_ids: 0

        before_rejects = AdmissionStats.reject_count()
        req = InferenceRequest("new", "sundial", torch.zeros(1, 1, 16), output_length=4)
        proxy = InferenceRequestProxy("new")
        with self.assertRaises(InferenceOverloadException):
            pool_group.dispatch_request(req, proxy)
        self.assertEqual(AdmissionStats.reject_count(), before_rejects + 1)


if __name__ == "__main__":
    unittest.main()
