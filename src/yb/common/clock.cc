// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/common/clock.h"

#include <thread>

using namespace std::literals;

DEFINE_uint64(wait_hybrid_time_sleep_interval_us, 10000,
              "Sleep interval in microseconds that will be used while waiting for specific "
                  "hybrid time.");

namespace yb {

Status WaitUntil(ClockBase* clock_base, HybridTime hybrid_time, CoarseTimePoint deadline) {
  auto ht_now = clock_base->Now();
  while (ht_now < hybrid_time) {
    if (CoarseMonoClock::now() > deadline) {
      return STATUS_FORMAT(TimedOut, "Timed out waiting for $0, now $1", deadline, ht_now);
    }
    auto delta_micros = hybrid_time.GetPhysicalValueMicros() - ht_now.GetPhysicalValueMicros();
    std::this_thread::sleep_for(
        std::max(FLAGS_wait_hybrid_time_sleep_interval_us, delta_micros) * 1us);
    ht_now = clock_base->Now();
  }

  return Status::OK();
}

} // namespace yb
