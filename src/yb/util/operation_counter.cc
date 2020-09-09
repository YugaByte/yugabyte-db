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

#include "yb/util/operation_counter.h"

#include <thread>

#include <glog/logging.h>

#include "yb/gutil/strings/substitute.h"

#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/logging.h"

using namespace std::literals;

using strings::Substitute;

namespace yb {

OperationCounter::OperationCounter(const std::string& log_prefix) : log_prefix_(log_prefix) {
}

void OperationCounter::Shutdown() {
  auto wait_start = CoarseMonoClock::now();
  auto last_report = wait_start;
  for (;;) {
    auto value = value_.load(std::memory_order_acquire);
    if (value == 0) {
      break;
    }
    auto now = CoarseMonoClock::now();
    if (now > last_report + std::chrono::seconds(10)) {
      LOG_WITH_PREFIX(WARNING)
          << "Long wait for scope counter shutdown " << value << ": " << AsString(now - wait_start);
      last_report = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void OperationCounter::Release() {
  value_.fetch_sub(1, std::memory_order_acq_rel);
}

void OperationCounter::Acquire() {
  value_.fetch_add(1, std::memory_order_acq_rel);
}

ScopedOperation::ScopedOperation(OperationCounter* counter) : counter_(counter) {
  counter->Acquire();
}

namespace {

// Using upper bits of counter as special flags.
constexpr uint64_t kStopDelta = 1ull << 63u;
constexpr uint64_t kDisabledDelta = 1ull << 48u;
constexpr uint64_t kOpCounterMask = kDisabledDelta - 1;
constexpr uint64_t kDisabledCounterMask = ~kOpCounterMask;

}

// Return pending operations counter value only.
uint64_t RWOperationCounter::GetOpCounter() const {
  return Get() & kOpCounterMask;
}

uint64_t RWOperationCounter::Update(uint64_t delta) {
  uint64_t result = counters_.fetch_add(delta, std::memory_order::memory_order_acq_rel) + delta;
  VLOG(2) << "[" << this << "] Update(" << static_cast<int64_t>(delta) << "), result = " << result;
  // Ensure that there is no underflow in either counter.
  DCHECK_EQ((result & (kStopDelta >> 1u)), 0); // Counter of DisableAndWaitForOps() calls.
  DCHECK_EQ((result & (kDisabledDelta >> 1u)), 0); // Counter of pending operations.
  return result;
}

bool RWOperationCounter::WaitMutexAndIncrement(CoarseTimePoint deadline) {
  if (deadline == CoarseTimePoint()) {
    deadline = CoarseMonoClock::now() + 10ms;
  }
  for (;;) {
    std::unique_lock<decltype(disable_)> lock(disable_, deadline);
    if (!lock.owns_lock()) {
      return false;
    }

    if (Increment()) {
      return true;
    }

    if (counters_.load(std::memory_order_acquire) & kStopDelta) {
      return false;
    }
  }
}

void RWOperationCounter::Enable(Unlock unlock, Stop was_stop) {
  Update(-(was_stop ? kStopDelta : kDisabledDelta));
  if (unlock) {
    UnlockExclusiveOpMutex();
  }
}

bool RWOperationCounter::Increment() {
  if (Update(1) & kDisabledCounterMask) {
    Update(-1);
    return false;
  }

  return true;
}

Status RWOperationCounter::DisableAndWaitForOps(const CoarseTimePoint& deadline, Stop stop) {
  LongOperationTracker long_operation_tracker(__func__, 1s);

  const auto start_time = CoarseMonoClock::now();
  std::unique_lock<decltype(disable_)> lock(disable_, deadline);
  if (!lock.owns_lock()) {
    return STATUS(TimedOut, "Timed out waiting to disable the resource exclusively");
  }

  Update(stop ? kStopDelta : kDisabledDelta);
  auto status = WaitForOpsToFinish(start_time, deadline);
  if (!status.ok()) {
    Enable(Unlock::kFalse, stop);
    return status;
  }

  lock.release();
  return Status::OK();
}

// The implementation is based on OperationTracker::WaitForAllToFinish.
Status RWOperationCounter::WaitForOpsToFinish(
    const CoarseTimePoint& start_time, const CoarseTimePoint& deadline) {
  const auto complain_interval = 1s;
  int64_t num_pending_ops = 0;
  int num_complaints = 0;
  auto wait_time = 250us;

  while ((num_pending_ops = GetOpCounter()) > 0) {
    auto now = CoarseMonoClock::now();
    auto waited_time = now - start_time;
    if (now > deadline) {
      return STATUS_FORMAT(
          TimedOut,
          "Timed out waiting for all pending operations to complete. "
              "$0 transactions pending. Waited for $1",
          num_pending_ops, waited_time);
    }
    if (waited_time > num_complaints * complain_interval) {
      LOG(WARNING) << Format("Waiting for $0 pending operations to complete now for $1",
                             num_pending_ops, waited_time);
      num_complaints++;
    }
    std::this_thread::sleep_until(std::min(deadline, now + wait_time));
    wait_time = std::min(wait_time * 5 / 4, 1000000us);
  }

  return Status::OK();
}

ScopedRWOperation::ScopedRWOperation(RWOperationCounter* counter, const CoarseTimePoint& deadline)
    : data_{counter, counter ? counter->resource_name() : ""
#ifndef NDEBUG
            , LongOperationTracker("ScopedRWOperation", 1s)
#endif
      } {
  if (counter != nullptr) {
    // The race condition between IsReady() and Increment() is OK, because we are checking if
    // anyone has started an exclusive operation since we did the increment, and don't proceed
    // with this shared-ownership operation in that case.
    if (!counter->Increment() && !counter->WaitMutexAndIncrement(deadline)) {
      data_.counter_ = nullptr;
    }
  }
}

ScopedRWOperation::~ScopedRWOperation() {
  Reset();
}

void ScopedRWOperation::Reset() {
  if (data_.counter_ != nullptr) {
    data_.counter_->Decrement();
    data_.counter_ = nullptr;
  }
  data_.resource_name_ = "";
}

ScopedRWOperationPause::ScopedRWOperationPause(
    RWOperationCounter* counter, const CoarseTimePoint& deadline, Stop stop) {
  if (counter != nullptr) {
    data_.status_ = counter->DisableAndWaitForOps(deadline, stop);
    if (data_.status_.ok()) {
      data_.counter_ = counter;
    }
  }
  data_.was_stop_ = stop;
}

ScopedRWOperationPause::~ScopedRWOperationPause() {
  Reset();
}

void ScopedRWOperationPause::Reset() {
  if (data_.counter_ != nullptr) {
    data_.counter_->Enable(Unlock(data_.status_.ok()), data_.was_stop_);
    // Prevent from the destructor calling Enable again.
    data_.counter_ = nullptr;
  }
}

void ScopedRWOperationPause::ReleaseMutexButKeepDisabled() {
  CHECK_OK(data_.status_);
  CHECK_NOTNULL(data_.counter_);
  CHECK(data_.was_stop_);
  data_.counter_->UnlockExclusiveOpMutex();
  // Make sure the destructor has no effect when it runs.
  data_.counter_ = nullptr;
}

}  // namespace yb
