/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

package org.apache.iotdb.commons.pipe.sink.limiter;

import org.junit.Assert;
import org.junit.Test;

public class PipeRateLimiterUtilsTest {

  @Test
  public void testLargeBatchIsSplitAtRateLimit() {
    // A 1 GiB batch at 1 MiB/s must never reserve 1 GiB of future permits at once.
    Assert.assertEquals(
        1024 * 1024,
        PipeRateLimiterUtils.nextAcquirePermits(1024L * 1024 * 1024, 1024 * 1024));
    Assert.assertEquals(2, PipeRateLimiterUtils.nextAcquirePermits(100, 2.9));
  }

  @Test
  public void testVeryLowRateStillMakesPositiveSizedRequests() {
    Assert.assertEquals(1, PipeRateLimiterUtils.nextAcquirePermits(100, 0.25));
    Assert.assertEquals(1, PipeRateLimiterUtils.nextAcquirePermits(1, 100));
  }

  @Test
  public void testLargeRateAndLongByteCountStayWithinIntRange() {
    Assert.assertEquals(
        Integer.MAX_VALUE,
        PipeRateLimiterUtils.nextAcquirePermits(Long.MAX_VALUE, Double.MAX_VALUE));
    Assert.assertEquals(10, PipeRateLimiterUtils.nextAcquirePermits(10, Double.MAX_VALUE));
  }
}
