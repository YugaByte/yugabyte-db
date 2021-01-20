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

#ifndef YB_COMMON_CLOCK_H
#define YB_COMMON_CLOCK_H

#include "yb/gutil/ref_counted.h"
#include "yb/common/hybrid_time.h"

namespace yb {

typedef std::pair<HybridTime, HybridTime> HybridTimeRange;

class ClockBase : public RefCountedThreadSafe<ClockBase> {
 public:
  // Obtains a new transaction timestamp corresponding to the current instant.
  HybridTime Now() { return NowRange().first; }

  // Obtains the hybrid_time corresponding to the current time,
  // that is maximally possible in cluster.
  HybridTime MaxGlobalNow() { return NowRange().second; }

  virtual HybridTimeRange NowRange() = 0;

  virtual void Update(const HybridTime& to_update) = 0;

  virtual ~ClockBase() {}
};

// Returns time after wait.
// Returns error on timeout.
Result<HybridTime> WaitUntil(ClockBase* clock, HybridTime hybrid_time, CoarseTimePoint deadline);

} // namespace yb

#endif // YB_COMMON_CLOCK_H
