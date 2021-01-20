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
#ifndef YB_TABLET_MVCC_H_
#define YB_TABLET_MVCC_H_

#include <condition_variable>
#include <mutex>
#include <deque>
#include <queue>
#include <vector>
#include <iostream>

#include "yb/server/clock.h"
#include "yb/util/debug-util.h"
#include "yb/util/opid.h"
#include "yb/util/enums.h"
#include "yb/gutil/thread_annotations.h"

namespace yb {
namespace tablet {

// Allows us to keep track of how a particular value of safe time was obtained, for sanity
// checking purposes.
YB_DEFINE_ENUM(SafeTimeSource,
               (kUnknown)(kNow)(kNextInQueue)(kHybridTimeLease)(kPropagated)(kLastReplicated));

struct SafeTimeWithSource {
  HybridTime safe_time = HybridTime::kMin;
  SafeTimeSource source = SafeTimeSource::kUnknown;

  std::string ToString() const;
};

struct FixedHybridTimeLease {
  HybridTime time;
  HybridTime lease = HybridTime::kMax;

  bool empty() const {
    return lease.GetPhysicalValueMicros() >= kMaxHybridTimePhysicalMicros;
  }

  std::string ToString() const {
    return Format("{ time: $0 lease: $1 }", time, lease);
  }
};

inline std::ostream& operator<<(std::ostream& out, const FixedHybridTimeLease& ht_lease) {
  return out << ht_lease.ToString();
}

// MvccManager is used to track operations.
// When new operation is initiated its time should be added using AddPending.
// When operation is replicated or aborted, MvccManager is notified using Replicated or Aborted
// methods.
// Operations could be replicated only in the same order as they were added.
// Time of newly added operation should be after time of all previously added operations.
class MvccManager {
 public:
  // `prefix` is used for logging.
  explicit MvccManager(std::string prefix, server::ClockPtr clock);
  ~MvccManager();

  // Set special RF==1 mode flag to handle safe time requests correctly in case
  // there are no heartbeats to update internal propagated_safe_time_ correctly.
  void SetLeaderOnlyMode(bool leader_only);

  // Sets time of last replicated operation, used after bootstrap.
  void SetLastReplicated(HybridTime ht);

  // Sets safe time that was sent to us by the leader. Should be called on followers.
  void SetPropagatedSafeTimeOnFollower(HybridTime ht);

  // Updates the propagated_safe_time field to the current safe time. This should be called in the
  // majority-replicated watermark callback from Raft. If we have some read requests that were
  // initiated when this server was a follower and are waiting for the safe time to advance past
  // a certain point, they can also get unblocked by this update of propagated_safe_time.
  void UpdatePropagatedSafeTimeOnLeader(const FixedHybridTimeLease& ht_lease);

  // Adds time of new tracked operation.
  // `ht` is in-out parameter.
  // In case of replica `ht` is already assigned, in case of leader we should assign ht by
  // by ourselves.
  // We pass ht as pointer here, because clock should be accessed with locked mutex, otherwise
  // SafeTime could return time greater than added.
  //
  // OpId is being passed for the ease of debugging.
  void AddPending(HybridTime* ht);

  // Notifies that operation with appropriate time was replicated.
  // It should be first operation in queue.
  void Replicated(HybridTime ht);

  // Notifies that operation with appropriate time was aborted.
  void Aborted(HybridTime ht);

  // Returns maximum allowed timestamp to read at. No operations that are initiated after this call
  // will receive hybrid time less than what's returned, provided that `ht_lease` is set to the
  // hybrid time leader lease expiration.
  //
  // `min_allowed` - result should be greater than or equal to `min_allowed`, otherwise it tries to
  // wait until safe hybrid time to read at reaches this value or `deadline` happens. Should be
  // less than the current hybrid time.
  //
  // `ht_lease` - result should be less than or equal to `ht_lease`, unless we have replicated
  // records past it. Should be past `min_allowed`. This is normally used to pass in the hybrid time
  // leader lease expiration, which limits the range of hybrid times that the current leader has
  // authority over, and thus imposes an upper bound on the safe time.
  //
  // Returns invalid hybrid time in case it cannot satisfy provided requirements, for instance
  // because of timeout.
  HybridTime SafeTime(
      HybridTime min_allowed, CoarseTimePoint deadline, const FixedHybridTimeLease& ht_lease) const;

  HybridTime SafeTime(const FixedHybridTimeLease& ht_lease) const {
    return SafeTime(HybridTime::kMin /* min_allowed */, CoarseTimePoint::max() /* deadline */,
                    ht_lease);
  }

  HybridTime SafeTimeForFollower(HybridTime min_allowed, CoarseTimePoint deadline) const;

  // Returns time of last replicated operation.
  HybridTime LastReplicatedHybridTime() const;

  class MvccOpTrace;

  void TEST_DumpTrace(std::ostream* out);


 private:
  HybridTime DoGetSafeTime(HybridTime min_allowed,
                           CoarseTimePoint deadline,
                           const FixedHybridTimeLease& ht_lease,
                           std::unique_lock<std::mutex>* lock) const;

  const std::string& LogPrefix() const { return prefix_; }

  struct InvariantViolationLoggingHelper;
  InvariantViolationLoggingHelper InvariantViolationLogPrefix() const;

  friend std::ostream& operator<<(
      std::ostream& out, const InvariantViolationLoggingHelper& helper);

  void PopFront(std::lock_guard<std::mutex>* lock);

  std::string prefix_;
  server::ClockPtr clock_;
  mutable std::mutex mutex_;
  mutable std::condition_variable cond_;

  // An ordered queue of times of tracked operations.
  std::deque<HybridTime> queue_;

  // Priority queue (min-heap, hence std::greater<> as the "less" comparator) of aborted operations.
  // Required because we could abort operations from the middle of the queue.
  std::priority_queue<HybridTime, std::vector<HybridTime>, std::greater<>> aborted_;

  HybridTime last_replicated_ = HybridTime::kMin;

  // If we are a follower, this is the latest safe time sent by the leader to us. If we are the
  // leader, this is a safe time that gets updated every time the majority-replicated watermarks
  // change.
  HybridTime propagated_safe_time_ = HybridTime::kMin;
  // Special flag for RF==1 mode when propagated_safe_time_ can be not up-to-date.
  bool leader_only_mode_ = false;

  mutable SafeTimeWithSource max_safe_time_returned_with_lease_;
  mutable SafeTimeWithSource max_safe_time_returned_without_lease_;
  mutable SafeTimeWithSource max_safe_time_returned_for_follower_ { HybridTime::kMin };

  std::unique_ptr<MvccOpTrace> op_trace_ GUARDED_BY(mutex_);
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_MVCC_H_
