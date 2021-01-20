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

#ifndef YB_TABLET_TABLET_RETENTION_POLICY_H_
#define YB_TABLET_TABLET_RETENTION_POLICY_H_

#include "yb/docdb/docdb_compaction_filter.h"
#include "yb/server/clock.h"

#include "yb/tablet/tablet_fwd.h"

namespace yb {
namespace tablet {

// History retention policy used by a tablet. It is based on pending reads and a fixed retention
// interval configured by the user.
class TabletRetentionPolicy : public docdb::HistoryRetentionPolicy {
 public:
  explicit TabletRetentionPolicy(server::ClockPtr clock, const RaftGroupMetadata* metadata);

  docdb::HistoryRetentionDirective GetRetentionDirective() override;

  // Tries to update history cutoff to proposed value, not allowing it to decrease.
  // Returns new committed history cutoff value.
  HybridTime UpdateCommittedHistoryCutoff(HybridTime new_value);

  // Returns history cutoff for propagation.
  // It is used at tablet leader while creating request for peer.
  // Invalid hybrid time is returned when history cutoff should not be propagated.
  // For instance it could happen if we already have big enough history cutoff or propagated it
  // recently.
  HybridTime HistoryCutoffToPropagate(HybridTime last_write_ht);

  // Register/Unregister a read operation, with an associated timestamp, for the purpose of
  // tracking the oldest read point.
  CHECKED_STATUS RegisterReaderTimestamp(HybridTime timestamp);
  void UnregisterReaderTimestamp(HybridTime timestamp);

  void EnableHistoryCutoffPropagation(bool value);

 private:
  bool ShouldRetainDeleteMarkersInMajorCompaction() const;
  HybridTime EffectiveHistoryCutoff() REQUIRES(mutex_);

  // Check proposed history cutoff against other restrictions (for instance min reading timestamp),
  // and returns most close value that satisfy them.
  HybridTime SanitizeHistoryCutoff(HybridTime proposed_history_cutoff) REQUIRES(mutex_);

  const std::string& LogPrefix() const {
    return log_prefix_;
  }

  const server::ClockPtr clock_;
  const RaftGroupMetadata& metadata_;
  const std::string log_prefix_;

  mutable std::mutex mutex_;
  // Set of active read timestamps.
  std::multiset<HybridTime> active_readers_ GUARDED_BY(mutex_);
  HybridTime committed_history_cutoff_ GUARDED_BY(mutex_) = HybridTime::kMin;
  CoarseTimePoint next_history_cutoff_propagation_ GUARDED_BY(mutex_) = CoarseTimePoint::min();
  int disable_counter_ GUARDED_BY(mutex_) = 0;
};

class HistoryCutoffPropagationDisabler {
 public:
  explicit HistoryCutoffPropagationDisabler(TabletRetentionPolicy* policy) : policy_(policy) {
    policy_->EnableHistoryCutoffPropagation(false);
  }

  ~HistoryCutoffPropagationDisabler() {
    policy_->EnableHistoryCutoffPropagation(true);
  }

 private:
  TabletRetentionPolicy* policy_;
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_TABLET_RETENTION_POLICY_H_
