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

#include "yb/common/consistent_read_point.h"
#include "yb/client/transaction.h"

#include "yb/util/debug-util.h"

namespace yb {

ConsistentReadPoint::ConsistentReadPoint(const scoped_refptr<ClockBase>& clock)
    : clock_(clock) {
}

void ConsistentReadPoint::SetReadTime(
    const ReadHybridTime& read_time, HybridTimeMap&& local_limits) {
  read_time_ = read_time;
  restart_read_ht_ = read_time_.read;
  local_limits_ = std::move(local_limits);
  restarts_.clear();
}

void ConsistentReadPoint::SetCurrentReadTime() {
  read_time_ = ReadHybridTime::FromHybridTimeRange(clock_->NowRange());
  restart_read_ht_ = read_time_.read;
  local_limits_.clear();
  restarts_.clear();
}

ReadHybridTime ConsistentReadPoint::GetReadTime(const TabletId& tablet) const {
  ReadHybridTime read_time = read_time_;
  if (read_time) {
    // Use the local limit for the tablet but no earlier than the read time we want.
    const auto it = local_limits_.find(tablet);
    if (it != local_limits_.end()) {
      read_time.local_limit = it->second;
    }
  }
  return read_time;
}

void ConsistentReadPoint::RestartRequired(const TabletId& tablet,
                                          const ReadHybridTime& restart_time) {
  DCHECK(read_time_) << "Unexpected restart without a read time set";
  std::unique_lock<std::mutex> lock(mutex_);
  restart_read_ht_.MakeAtLeast(restart_time.read);
  // We should inherit per-tablet restart time limits before restart, doing it lazily.
  if (restarts_.empty()) {
    restarts_ = local_limits_;
  }
  auto emplace_result = restarts_.emplace(tablet, restart_time.local_limit);
  bool inserted = emplace_result.second;
  if (!inserted) {
    auto& existing_local_limit = emplace_result.first->second;
    existing_local_limit = std::min(existing_local_limit, restart_time.local_limit);
  }
}

bool ConsistentReadPoint::IsRestartRequired() const {
  return !restarts_.empty();
}

void ConsistentReadPoint::Restart() {
  local_limits_ = std::move(restarts_);
  read_time_.read = restart_read_ht_;
}

void ConsistentReadPoint::Defer() {
  read_time_.read = read_time_.global_limit;
}

void ConsistentReadPoint::UpdateClock(HybridTime propagated_hybrid_time) {
  clock_->Update(propagated_hybrid_time);
}

HybridTime ConsistentReadPoint::Now() const {
  return clock_->Now();
}

void ConsistentReadPoint::PrepareChildTransactionData(ChildTransactionDataPB* data) const {
  read_time_.AddToPB(data);
  auto& local_limits = *data->mutable_local_limits();
  for (const auto& entry : local_limits_) {
    typedef std::remove_reference<decltype(*local_limits.begin())>::type PairType;
    local_limits.insert(PairType(entry.first, entry.second.ToUint64()));
  }
}

void ConsistentReadPoint::FinishChildTransactionResult(
    HadReadTime had_read_time, ChildTransactionResultPB* result) const {
  if (IsRestartRequired()) {
    result->set_restart_read_ht(restart_read_ht_.ToUint64());
    auto& restarts = *result->mutable_read_restarts();
    for (const auto& restart : restarts_) {
      typedef std::remove_reference<decltype(*restarts.begin())>::type PairType;
      restarts.insert(PairType(restart.first, restart.second.ToUint64()));
    }
  } else {
    result->set_restart_read_ht(HybridTime::kInvalid.ToUint64());
  }

  if (!had_read_time && read_time_) {
    read_time_.ToPB(result->mutable_used_read_time());
  }
}

void ConsistentReadPoint::ApplyChildTransactionResult(const ChildTransactionResultPB& result) {
  if (result.has_used_read_time()) {
    LOG_IF(DFATAL, read_time_)
        << "Read time already picked (" << read_time_
        << ", but child result contains used read time: "
        << result.used_read_time().ShortDebugString();
    read_time_ = ReadHybridTime::FromPB(result.used_read_time());
    restart_read_ht_ = read_time_.read;
  }

  HybridTime restart_read_ht(result.restart_read_ht());
  if (restart_read_ht.is_valid()) {
    ReadHybridTime read_time;
    read_time.read = restart_read_ht;
    for (const auto& restart : result.read_restarts()) {
      read_time.local_limit = HybridTime(restart.second);
      RestartRequired(restart.first, read_time);
    }
  }
}

void ConsistentReadPoint::SetInTxnLimit(HybridTime value) {
  read_time_.in_txn_limit = value;
}

ConsistentReadPoint& ConsistentReadPoint::operator=(ConsistentReadPoint&& other) {
  clock_.swap(other.clock_);
  read_time_ = std::move(other.read_time_);
  restart_read_ht_ = std::move(other.restart_read_ht_);
  local_limits_ = std::move(other.local_limits_);
  restarts_ = std::move(other.restarts_);
  return *this;
}

} // namespace yb
