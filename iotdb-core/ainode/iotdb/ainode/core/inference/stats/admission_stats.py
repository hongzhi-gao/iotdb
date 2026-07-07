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

from iotdb.ainode.core.log import Logger

logger = Logger()


class AdmissionStats:
    """Tracks admission rejects in the RPC / pool-controller process."""

    _lock = threading.Lock()
    _reject_count = 0

    @classmethod
    def record_reject(cls, model_id: str, pool_id: int, queue_size: int) -> None:
        with cls._lock:
            cls._reject_count += 1
            total_rejects = cls._reject_count
        logger.warning(
            f"[InferenceStats][Admission] model={model_id}, pool={pool_id}, "
            f"queue_size={queue_size}, admission_reject_total={total_rejects}"
        )

    @classmethod
    def reject_count(cls) -> int:
        with cls._lock:
            return cls._reject_count
