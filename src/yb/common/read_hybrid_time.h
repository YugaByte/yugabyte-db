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

#ifndef YB_COMMON_READ_HYBRID_TIME_H
#define YB_COMMON_READ_HYBRID_TIME_H

#include "yb/common/clock.h"
#include "yb/common/hybrid_time.h"
#include "yb/util/compare_util.h"
#include "yb/util/tostring.h"

namespace yb {

// Hybrid time range used for read.
// Limit is the maximum time that could have existed on any server at the time the read operation
// was initiated, and is used to decide whether the read operation need to be restarted at a higher
// hybrid time than `read`.
struct ReadHybridTime {
  // Hybrid time of read operation.
  HybridTime read;

  // Read time limit, that is used for local records of requested tablet.
  HybridTime local_limit;

  // Read time limit, that is used for global entries, for instance transactions.
  HybridTime global_limit;

  // Read time limit for intents from the same transaction.
  HybridTime in_txn_limit;

  // Serial no of request that uses this read hybrid time.
  int64_t serial_no = 0;

  static ReadHybridTime Max() {
    return SingleTime(HybridTime::kMax);
  }

  static ReadHybridTime SingleTime(HybridTime value) {
    return {value, value, value, HybridTime::kMax, 0};
  }

  static ReadHybridTime FromMicros(MicrosTime micros) {
    return SingleTime(HybridTime::FromMicros(micros));
  }

  static ReadHybridTime FromUint64(uint64_t value) {
    return SingleTime(HybridTime(value));
  }

  static ReadHybridTime FromHybridTimeRange(const HybridTimeRange& range) {
    return {range.first, range.second, range.second, HybridTime::kMax, 0};
  }

  template <class PB>
  static ReadHybridTime FromReadTimePB(const PB& pb) {
    if (!pb.has_read_time()) {
      return ReadHybridTime();
    }
    return FromPB(pb.read_time());
  }

  template <class PB>
  static ReadHybridTime FromRestartReadTimePB(const PB& pb) {
    if (!pb.has_restart_read_time()) {
      return ReadHybridTime();
    }
    return FromPB(pb.restart_read_time());
  }

  template <class PB>
  static ReadHybridTime FromPB(const PB& read_time) {
    return {
      .read = HybridTime(read_time.read_ht()),
      .local_limit = HybridTime(read_time.has_local_limit_ht()
          ? read_time.local_limit_ht()
          : read_time.deprecated_max_of_read_time_and_local_limit_ht()),
      .global_limit = HybridTime(read_time.global_limit_ht()),
      // Use max hybrid time for backward compatibility.
      .in_txn_limit = read_time.in_txn_limit_ht()
          ? HybridTime(read_time.in_txn_limit_ht())
          : HybridTime::kMax,
      .serial_no = 0,
    };
  }

  template <class PB>
  void ToPB(PB* out) const {
    out->set_read_ht(read.ToUint64());
    out->set_local_limit_ht(local_limit.ToUint64());
    out->set_global_limit_ht(global_limit.ToUint64());
    out->set_in_txn_limit_ht(
        in_txn_limit.is_valid() ? in_txn_limit.ToUint64() : HybridTime::kMax.ToUint64());
    out->set_deprecated_max_of_read_time_and_local_limit_ht(std::max(local_limit, read).ToUint64());
  }

  template <class PB>
  void AddToPB(PB* pb) const {
    if (read.is_valid()) {
      ToPB(pb->mutable_read_time());
    } else {
      pb->clear_read_time();
    }
  }

  explicit operator bool() const {
    return read.is_valid();
  }

  bool operator!() const {
    return !read.is_valid();
  }

#define YB_READ_HYBRID_TIME_FIELDS read, local_limit, global_limit, in_txn_limit, serial_no

  std::string ToString() const {
    return YB_STRUCT_TO_STRING(YB_READ_HYBRID_TIME_FIELDS);
  }
};

inline std::ostream& operator<<(std::ostream& out, const ReadHybridTime& read_time) {
  return out << read_time.ToString();
}

inline bool operator==(const ReadHybridTime& lhs, const ReadHybridTime& rhs) {
  return YB_STRUCT_EQUALS(YB_READ_HYBRID_TIME_FIELDS);
}

} // namespace yb

#endif // YB_COMMON_READ_HYBRID_TIME_H
