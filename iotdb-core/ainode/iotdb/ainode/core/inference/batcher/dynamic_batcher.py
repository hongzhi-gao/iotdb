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

from typing import List

import torch

from iotdb.ainode.core.config import AINodeDescriptor
from iotdb.ainode.core.inference.batcher.abstract_batcher import AbstractBatcher
from iotdb.ainode.core.inference.batcher.batch_result import BatchResult
from iotdb.ainode.core.inference.batcher.batch_utils import (
    max_batch_input_length,
    max_batch_output_length,
)
from iotdb.ainode.core.inference.inference_request import InferenceRequest


class DynamicBatcher(AbstractBatcher):
    """
    Dynamic batcher that pads inputs within a length bucket and merges requests
    with compatible target/output shapes.
    """

    def __init__(
        self,
        input_length_bucket_size: int = None,
        output_length_bucket_size: int = None,
    ):
        super().__init__()
        config = AINodeDescriptor().get_config()
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

    def batch_request(self, reqs: List[InferenceRequest]) -> torch.Tensor:
        return self.batch_requests(reqs).batch_inputs

    def batch_requests(self, reqs: List[InferenceRequest]) -> BatchResult:
        if not reqs:
            raise ValueError("No requests provided to batch_requests.")

        first_target_count = reqs[0].target_count
        for i, req in enumerate(reqs):
            if req.target_count != first_target_count:
                raise ValueError(
                    f"All requests must have the same target_count, "
                    f"but request 0 has {first_target_count} "
                    f"and request {i} has {req.target_count}"
                )

        padded_input_length = max_batch_input_length(reqs)
        batch_output_length = max_batch_output_length(reqs)
        padded_inputs = []
        for req in reqs:
            padded_inputs.append(self._pad_input(req.inputs, padded_input_length))

        batch_inputs = torch.cat(padded_inputs, dim=0)
        return BatchResult(
            batch_inputs=batch_inputs,
            requests=reqs,
            batch_output_length=batch_output_length,
            padded_input_length=padded_input_length,
        )

    @staticmethod
    def _pad_input(inputs: torch.Tensor, target_length: int) -> torch.Tensor:
        if inputs.size(-1) == target_length:
            return inputs
        if inputs.size(-1) > target_length:
            return inputs[..., :target_length]

        pad_size = target_length - inputs.size(-1)
        pad_value = inputs[..., -1:].expand(*inputs.shape[:-1], pad_size)
        return torch.cat([inputs, pad_value], dim=-1)
