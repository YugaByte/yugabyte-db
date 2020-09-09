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

#include "yb/tablet/operations/operation_tracker.h"

#include <algorithm>
#include <limits>
#include <vector>


#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/operations/operation_driver.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/tsan_util.h"

DEFINE_int64(tablet_operation_memory_limit_mb, 1024,
             "Maximum amount of memory that may be consumed by all in-flight "
             "operations belonging to a particular tablet. When this limit "
             "is reached, new operations will be rejected and clients will "
             "be forced to retry them. If -1, operation memory tracking is "
             "disabled.");
TAG_FLAG(tablet_operation_memory_limit_mb, advanced);

METRIC_DEFINE_gauge_uint64(tablet, all_operations_inflight,
                           "Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of operations currently in-flight, including any type.");
METRIC_DEFINE_gauge_uint64(tablet, write_operations_inflight,
                           "Write Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of write operations currently in-flight");
METRIC_DEFINE_gauge_uint64(tablet, alter_schema_operations_inflight,
                           "Change Metadata Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of change metadata operations currently in-flight");
METRIC_DEFINE_gauge_uint64(tablet, update_transaction_operations_inflight,
                           "Update Transaction Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of update transaction operations currently in-flight");
METRIC_DEFINE_gauge_uint64(tablet, snapshot_operations_inflight,
                           "Snapshot Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of snapshot operations currently in-flight");
METRIC_DEFINE_gauge_uint64(tablet, split_operations_inflight,
                           "Split Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of split operations currently in-flight");
METRIC_DEFINE_gauge_uint64(tablet, truncate_operations_inflight,
                           "Truncate Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of truncate operations currently in-flight");
METRIC_DEFINE_gauge_uint64(tablet, empty_operations_inflight,
                           "Empty Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of none operations currently in-flight");
METRIC_DEFINE_gauge_uint64(tablet, history_cutoff_operations_inflight,
                           "History Cutoff Operations In Flight",
                           yb::MetricUnit::kOperations,
                           "Number of history cutoff operations currently in-flight");

METRIC_DEFINE_counter(tablet, operation_memory_pressure_rejections,
                      "Operation Memory Pressure Rejections",
                      yb::MetricUnit::kOperations,
                      "Number of operations rejected because the tablet's "
                      "operation memory limit was reached.");

using namespace std::literals;
using std::shared_ptr;
using std::vector;

namespace yb {
namespace tablet {

using strings::Substitute;

#define MINIT(x) x(METRIC_##x.Instantiate(entity))
#define GINIT(x) x(METRIC_##x.Instantiate(entity, 0))
#define INSTANTIATE(upper, lower) \
  operations_inflight[to_underlying(OperationType::BOOST_PP_CAT(k, upper))] = \
      BOOST_PP_CAT(BOOST_PP_CAT(METRIC_, lower), _operations_inflight).Instantiate(entity, 0);
OperationTracker::Metrics::Metrics(const scoped_refptr<MetricEntity>& entity)
    : GINIT(all_operations_inflight),
      MINIT(operation_memory_pressure_rejections) {
  INSTANTIATE(Write, write);
  INSTANTIATE(ChangeMetadata, alter_schema);
  INSTANTIATE(UpdateTransaction, update_transaction);
  INSTANTIATE(Snapshot, snapshot);
  INSTANTIATE(Split, split);
  INSTANTIATE(Truncate, truncate);
  INSTANTIATE(Empty, empty);
  INSTANTIATE(HistoryCutoff, history_cutoff);
  static_assert(8 == kElementsInOperationType, "Init metrics for all operation types");
}
#undef INSTANTIATE
#undef GINIT
#undef MINIT

OperationTracker::OperationTracker(const std::string& log_prefix)
    : log_prefix_(log_prefix) {
}

OperationTracker::~OperationTracker() {
  std::lock_guard<std::mutex> lock(mutex_);
  CHECK_EQ(pending_operations_.size(), 0);
  if (mem_tracker_) {
    mem_tracker_->UnregisterFromParent();
  }
}

Status OperationTracker::Add(OperationDriver* driver) {
  int64_t driver_mem_footprint = driver->SpaceUsed();
  if (mem_tracker_ && !mem_tracker_->TryConsume(driver_mem_footprint)) {
    if (metrics_) {
      metrics_->operation_memory_pressure_rejections->Increment();
    }

    // May be null in unit tests.
    Tablet* tablet = driver->state()->tablet();

    string msg = Substitute(
        "Operation failed, tablet $0 operation memory consumption ($1) "
        "has exceeded its limit ($2) or the limit of an ancestral tracker",
        tablet ? tablet->tablet_id() : "(unknown)",
        mem_tracker_->consumption(), mem_tracker_->limit());

    YB_LOG_EVERY_N_SECS(WARNING, 1) << msg << THROTTLE_MSG;

    return STATUS(ServiceUnavailable, msg);
  }

  IncrementCounters(*driver);

  // Cache the operation memory footprint so we needn't refer to the request
  // again, as it may disappear between now and then.
  State st;
  st.memory_footprint = driver_mem_footprint;
  std::lock_guard<std::mutex> lock(mutex_);
  CHECK(pending_operations_.emplace(driver, st).second);
  return Status::OK();
}

void OperationTracker::IncrementCounters(const OperationDriver& driver) const {
  if (!metrics_) {
    return;
  }

  metrics_->all_operations_inflight->Increment();
  metrics_->operations_inflight[to_underlying(driver.operation_type())]->Increment();
}

void OperationTracker::DecrementCounters(const OperationDriver& driver) const {
  if (!metrics_) {
    return;
  }

  DCHECK_GT(metrics_->all_operations_inflight->value(), 0);
  metrics_->all_operations_inflight->Decrement();
  auto index = to_underlying(driver.operation_type());
  DCHECK_GT(metrics_->operations_inflight[index]->value(), 0);
  metrics_->operations_inflight[index]->Decrement();
}

void OperationTracker::Release(OperationDriver* driver, OpIds* applied_op_ids) {
  DecrementCounters(*driver);

  State st;
  yb::OpId op_id = driver->GetOpId();
  OperationType operation_type = driver->operation_type();
  bool notify;
  {
    // Remove the operation from the map, retaining the state for use
    // below.
    std::lock_guard<std::mutex> lock(mutex_);
    st = FindOrDie(pending_operations_, driver);
    if (PREDICT_FALSE(pending_operations_.erase(driver) != 1)) {
      LOG_WITH_PREFIX(FATAL) << "Could not remove pending operation from map: "
          << driver->ToStringUnlocked();
    }
    notify = pending_operations_.empty();
  }
  if (notify) {
    cond_.notify_all();
  }

  if (mem_tracker_ && st.memory_footprint) {
    mem_tracker_->Release(st.memory_footprint);
  }

  if (operation_type != OperationType::kEmpty) {
    if (applied_op_ids) {
      applied_op_ids->push_back(op_id);
    } else if (post_tracker_) {
      post_tracker_(op_id);
    }
  }
}

std::vector<scoped_refptr<OperationDriver>> OperationTracker::GetPendingOperations() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return GetPendingOperationsUnlocked();
}

std::vector<scoped_refptr<OperationDriver>> OperationTracker::GetPendingOperationsUnlocked() const {
  std::vector<scoped_refptr<OperationDriver>> result;
  result.reserve(pending_operations_.size());
  for (const auto& e : pending_operations_) {
    result.push_back(e.first);
  }
  return result;
}


int OperationTracker::GetNumPendingForTests() const {
  std::lock_guard<std::mutex> l(mutex_);
  return pending_operations_.size();
}

void OperationTracker::WaitForAllToFinish() const {
  // Wait indefinitely.
  CHECK_OK(WaitForAllToFinish(MonoDelta::FromNanoseconds(std::numeric_limits<int64_t>::max())));
}

Status OperationTracker::WaitForAllToFinish(const MonoDelta& timeout) const
    NO_THREAD_SAFETY_ANALYSIS {
  const MonoDelta kComplainInterval = 1000ms * kTimeMultiplier;
  MonoDelta wait_time = 250ms * kTimeMultiplier;
  int num_complaints = 0;
  MonoTime start_time = MonoTime::Now();
  auto operations = GetPendingOperations();
  if (operations.empty()) {
    return Status::OK();
  }
  for (;;) {
    MonoDelta diff = MonoTime::Now().GetDeltaSince(start_time);
    if (diff.MoreThan(timeout)) {
      return STATUS(TimedOut, Substitute("Timed out waiting for all operations to finish. "
                                         "$0 operations pending. Waited for $1",
                                         operations.size(), diff.ToString()));
    }
    if (diff > kComplainInterval * num_complaints) {
      LOG_WITH_PREFIX(WARNING)
          << Format("OperationTracker waiting for $0 outstanding operations to"
                        " complete now for $1", operations.size(), diff);
      num_complaints++;
    }
    wait_time = std::min<MonoDelta>(wait_time * 5 / 4, 1s);

    LOG_WITH_PREFIX(INFO) << "Dumping currently running operations: ";
    for (scoped_refptr<OperationDriver> driver : operations) {
      LOG_WITH_PREFIX(INFO) << driver->ToString();
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (pending_operations_.empty()) {
        break;
      }
      if (cond_.wait_for(lock, wait_time.ToSteadyDuration()) == std::cv_status::no_timeout &&
          pending_operations_.empty()) {
        break;
      }
      operations = GetPendingOperationsUnlocked();
    }
  }
  return Status::OK();
}

void OperationTracker::StartInstrumentation(
    const scoped_refptr<MetricEntity>& metric_entity) {
  metrics_.reset(new Metrics(metric_entity));
}

void OperationTracker::StartMemoryTracking(
    const shared_ptr<MemTracker>& parent_mem_tracker) {
  if (FLAGS_tablet_operation_memory_limit_mb != -1) {
    mem_tracker_ = MemTracker::CreateTracker(
        FLAGS_tablet_operation_memory_limit_mb * 1024 * 1024,
        "operation_tracker",
        parent_mem_tracker);
  }
}

}  // namespace tablet
}  // namespace yb
