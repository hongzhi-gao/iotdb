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

import unittest
from unittest import mock

from iotdb.ainode.core.inference.inference_config import (
    default_memory_usage_ratio,
    recommend_max_batch_size,
    recommend_max_queue_size,
    resolve_inference_config,
    resolve_scheduling_profile,
)


class InferenceConfigTest(unittest.TestCase):
    def test_scheduling_profiles(self):
        latency = resolve_scheduling_profile("latency")
        balanced = resolve_scheduling_profile("balanced")
        throughput = resolve_scheduling_profile("throughput")
        self.assertLess(latency["batch_deadline_ms"], balanced["batch_deadline_ms"])
        self.assertLess(balanced["batch_deadline_ms"], throughput["batch_deadline_ms"])
        self.assertEqual(
            resolve_scheduling_profile("unknown")["batch_deadline_ms"], 500
        )

    def test_default_memory_usage_ratio(self):
        eight_gb = 8 * 1024**3
        sixteen_gb = 16 * 1024**3
        twenty_four_gb = 24 * 1024**3
        self.assertEqual(default_memory_usage_ratio("cuda", eight_gb), 0.45)
        self.assertEqual(default_memory_usage_ratio("cuda", sixteen_gb), 0.50)
        self.assertEqual(default_memory_usage_ratio("cuda", twenty_four_gb), 0.55)
        self.assertEqual(default_memory_usage_ratio("cpu", sixteen_gb), 0.25)

    @mock.patch(
        "iotdb.ainode.core.inference.inference_config.get_device_total_memory_bytes",
        return_value=16 * 1024**3,
    )
    @mock.patch(
        "iotdb.ainode.core.inference.inference_config.detect_primary_device_type",
        return_value="cuda",
    )
    def test_auto_resolve_gpu_defaults(self, _device, _memory):
        config = resolve_inference_config(
            memory_usage_ratio=None,
            scheduling_profile="balanced",
            max_batch_size=None,
            batch_interval_ms=None,
            batch_waiting_window_ms=None,
            batch_deadline_ms=None,
            max_batch_points=None,
            input_length_bucket_size=None,
            output_length_bucket_size=None,
            max_memory_bytes=None,
            max_activate_size=None,
            max_step_size=None,
            max_queue_size=None,
            extra_memory_ratio=None,
            model_mem_usage_map=None,
        )
        self.assertEqual(config.memory_usage_ratio, 0.50)
        self.assertEqual(config.scheduling_profile, "balanced")
        self.assertEqual(config.batch_interval_ms, 15)
        self.assertEqual(config.max_batch_size, 32)
        self.assertEqual(config.max_queue_size, 256)
        self.assertGreater(config.max_batch_points, 0)
        self.assertGreater(config.recommended_pool_count, 0)

    @mock.patch(
        "iotdb.ainode.core.inference.inference_config.get_device_total_memory_bytes",
        return_value=32 * 1024**3,
    )
    @mock.patch(
        "iotdb.ainode.core.inference.inference_config.detect_primary_device_type",
        return_value="cpu",
    )
    def test_explicit_overrides_win(self, _device, _memory):
        config = resolve_inference_config(
            memory_usage_ratio=0.3,
            scheduling_profile="latency",
            max_batch_size=8,
            batch_interval_ms=99,
            batch_waiting_window_ms=88,
            batch_deadline_ms=77,
            max_batch_points=1234,
            input_length_bucket_size=16,
            output_length_bucket_size=8,
            max_memory_bytes=999,
            max_activate_size=11,
            max_step_size=12,
            max_queue_size=99,
            extra_memory_ratio=1.5,
            model_mem_usage_map={"sundial": 1024},
        )
        self.assertEqual(config.memory_usage_ratio, 0.3)
        self.assertEqual(config.batch_interval_ms, 99)
        self.assertEqual(config.max_batch_size, 8)
        self.assertEqual(config.max_queue_size, 99)
        self.assertEqual(config.max_batch_points, 1234)
        self.assertEqual(config.max_memory_bytes, 999)
        self.assertEqual(config.max_activate_size, 11)
        self.assertEqual(config.max_step_size, 12)

    def test_recommend_max_batch_size_tiers(self):
        self.assertEqual(recommend_max_batch_size("cuda", 8 * 1024**3, 0.45), 14)
        self.assertEqual(recommend_max_batch_size("cuda", 16 * 1024**3, 0.50), 32)
        self.assertEqual(recommend_max_batch_size("cuda", 24 * 1024**3, 0.55), 48)
        self.assertEqual(recommend_max_batch_size("cpu", 8 * 1024**3, 0.25), 4)

    def test_recommend_max_queue_size(self):
        self.assertEqual(recommend_max_queue_size(32), 256)
        self.assertEqual(recommend_max_queue_size(4), 64)


if __name__ == "__main__":
    unittest.main()
