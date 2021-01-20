// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#ifndef YB_SERVER_HYBRID_CLOCK_H_
#define YB_SERVER_HYBRID_CLOCK_H_

#include <atomic>
#include <string>
#if !defined(__APPLE__)
#include <sys/timex.h>
#endif // !defined(__APPLE__)

#include <boost/atomic.hpp>

#include "yb/gutil/ref_counted.h"
#include "yb/server/clock.h"
#include "yb/util/locks.h"
#include "yb/util/metrics.h"
#include "yb/util/physical_time.h"

namespace yb {
namespace server {

struct HybridClockComponents {
  // The last clock read/update, in microseconds.
  MicrosTime last_usec = 0;

  // The next logical value to be assigned to a hybrid time.
  LogicalTimeComponent logical = 0;

  HybridClockComponents() noexcept {}

  HybridClockComponents(MicrosTime last_usec_, LogicalTimeComponent logical_)
      : last_usec(last_usec_),
        logical(logical_) {
  }

  HybridClockComponents(HybridClockComponents&& other) = default;
  HybridClockComponents(const HybridClockComponents& other) = default;

  bool operator< (const HybridClockComponents& o) const {
    return last_usec < o.last_usec || (last_usec == o.last_usec && logical < o.logical);
  }

  bool operator<= (const HybridClockComponents& o) const {
    return last_usec < o.last_usec || (last_usec == o.last_usec && logical <= o.logical);
  }

  void HandleLogicalComponentOverflow();

  std::string ToString() const;
};

std::ostream& operator<<(std::ostream& out, const HybridClockComponents& components);

// The HybridTime clock.
//
// HybridTime should not be used on a distributed cluster running on OS X hosts,
// since NTP clock error is not available.
class HybridClock : public Clock {
 public:
  HybridClock();
  explicit HybridClock(PhysicalClockPtr clock);
  explicit HybridClock(const std::string& time_source);

  CHECKED_STATUS Init() override;

  HybridTimeRange NowRange() override;

  // Updates the clock with a hybrid_time originating on another machine.
  void Update(const HybridTime& to_update) override;

  void RegisterMetrics(const scoped_refptr<MetricEntity>& metric_entity) override;

  // Obtains the hybrid_time corresponding to the current time and the associated
  // error in micros. This may fail if the clock is unsynchronized or synchronized
  // but the error is too high and, since we can't do anything about it,
  // LOG(FATAL)'s in that case.
  void NowWithError(HybridTime* hybrid_time, uint64_t* max_error_usec);

  // Static encoding/decoding methods for hybrid_times. Public mostly
  // for testing/debugging purposes.

  // Returns the logical value embedded in 'hybrid_time'
  static LogicalTimeComponent GetLogicalValue(const HybridTime& hybrid_time);

  // Returns the physical value embedded in 'hybrid_time', in microseconds.
  static MicrosTime GetPhysicalValueMicros(const HybridTime& hybrid_time);

  // Returns the physical value embedded in 'hybrid_time', in nanoseconds.
  static uint64_t GetPhysicalValueNanos(const HybridTime& hybrid_time);

  // Obtains a new HybridTime with the logical value zeroed out.
  static HybridTime HybridTimeFromMicroseconds(uint64_t micros);

  // Obtains a new HybridTime that embeds both the physical and logical values.
  static HybridTime HybridTimeFromMicrosecondsAndLogicalValue(
      MicrosTime micros, LogicalTimeComponent logical_value);

  // Creates a new hybrid_time whose physical time is GetPhysicalValue(original) +
  // 'micros_to_add' and which retains the same logical value.
  static HybridTime AddPhysicalTimeToHybridTime(const HybridTime& original,
                                                const MonoDelta& to_add);

  // Given two hybrid times, determines whether the delta between end and begin them is higher,
  // lower or equal to the given delta and returns 1, -1 and 0 respectively. Note that if end <
  // begin we return -1.
  static int CompareHybridClocksToDelta(const HybridTime& begin, const HybridTime& end,
                                        const MonoDelta& delta);

  static void RegisterProvider(std::string name, PhysicalClockProvider provider);

  // Enables check whether clock skew within configured bounds.
  static void EnableClockSkewControl();

  const PhysicalClockPtr& TEST_clock() { return clock_; }

 private:
  enum State {
    kNotInitialized,
    kInitialized
  };

  // Used to get the hybrid_time for metrics.
  uint64_t NowForMetrics();

  // Used to get the current error, for metrics.
  uint64_t ErrorForMetrics();

  // Used to get the current error, for metrics.
  int64_t SkewForMetrics();

  PhysicalClockPtr clock_;
  boost::atomic<HybridClockComponents> components_{HybridClockComponents(0, 0)};
  State state_ = kNotInitialized;

  // Clock metrics are set to detach to their last value. This means
  // that, during our destructor, we'll need to access other class members
  // declared above this. Hence, this member must be declared last.
  FunctionGaugeDetacher metric_detacher_;
};

}  // namespace server
}  // namespace yb

#endif /* YB_SERVER_HYBRID_CLOCK_H_ */
