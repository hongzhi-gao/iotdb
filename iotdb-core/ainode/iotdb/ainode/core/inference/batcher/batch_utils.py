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

from collections import defaultdict
from typing import Iterable, List, Tuple

from iotdb.ainode.core.inference.inference_request import InferenceRequest


def bucket_length(length: int, bucket_size: int) -> int:
    if bucket_size <= 0:
        return length
    return ((length + bucket_size - 1) // bucket_size) * bucket_size


def batch_group_key(
    req: InferenceRequest,
    input_length_bucket_size: int,
    output_length_bucket_size: int,
) -> Tuple[int, int, int]:
    return (
        req.target_count,
        bucket_length(req.input_length, input_length_bucket_size),
        bucket_length(req.output_length, output_length_bucket_size),
    )


def group_requests_by_bucket(
    requests: Iterable[InferenceRequest],
    input_length_bucket_size: int,
    output_length_bucket_size: int,
) -> List[List[InferenceRequest]]:
    grouped = defaultdict(list)
    for req in requests:
        grouped[
            batch_group_key(req, input_length_bucket_size, output_length_bucket_size)
        ].append(req)
    return list(grouped.values())


def batch_points(requests: Iterable[InferenceRequest]) -> int:
    return sum(req.batch_points() for req in requests)


def max_batch_output_length(requests: Iterable[InferenceRequest]) -> int:
    return max(req.output_length for req in requests)


def max_batch_input_length(requests: Iterable[InferenceRequest]) -> int:
    return max(req.input_length for req in requests)
