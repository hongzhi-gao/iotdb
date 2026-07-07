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

from dataclasses import dataclass
from typing import List

import torch

from iotdb.ainode.core.inference.inference_request import InferenceRequest


@dataclass(frozen=True)
class BatchResult:
    batch_inputs: torch.Tensor
    requests: List[InferenceRequest]
    batch_output_length: int
    padded_input_length: int

    def useful_input_points(self) -> int:
        return sum(
            req.batch_size * req.target_count * req.input_length
            for req in self.requests
        )

    def total_input_points(self) -> int:
        batch_size, target_count, _ = self.batch_inputs.shape
        return batch_size * target_count * self.padded_input_length

    def padding_waste_ratio(self) -> float:
        total = self.total_input_points()
        if total <= 0:
            return 0.0
        return 1.0 - self.useful_input_points() / total
