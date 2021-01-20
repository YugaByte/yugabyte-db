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
#include "yb/master/async_rpc_tasks.h"

#include "yb/common/wire_protocol.h"

#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus.proxy.h"

#include "yb/master/master.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/catalog_manager.h"

#include "yb/rpc/messenger.h"

#include "yb/tserver/tserver_admin.proxy.h"

#include "yb/util/atomic.h"
#include "yb/util/flag_tags.h"
#include "yb/util/format.h"
#include "yb/util/logging.h"
#include "yb/util/thread_restrictions.h"

DEFINE_int32(unresponsive_ts_rpc_timeout_ms, 15 * 60 * 1000,  // 15 minutes
             "After this amount of time (or after we have retried unresponsive_ts_rpc_retry_limit "
             "times, whichever happens first), the master will stop attempting to contact a tablet "
             "server in order to perform operations such as deleting a tablet.");
TAG_FLAG(unresponsive_ts_rpc_timeout_ms, advanced);

DEFINE_int32(unresponsive_ts_rpc_retry_limit, 20,
             "After this number of retries (or unresponsive_ts_rpc_timeout_ms expires, whichever "
             "happens first), the master will stop attempting to contact a tablet server in order "
             "to perform operations such as deleting a tablet.");
TAG_FLAG(unresponsive_ts_rpc_retry_limit, advanced);

DEFINE_int32(retrying_ts_rpc_max_delay_ms, 60 * 1000,
             "Maximum delay between successive attempts to contact an unresponsive tablet server");
TAG_FLAG(retrying_ts_rpc_max_delay_ms, advanced);

DEFINE_test_flag(int32, slowdown_master_async_rpc_tasks_by_ms, 0,
                 "For testing purposes, slow down the run method to take longer.");

// The flags are defined in catalog_manager.cc.
DECLARE_int32(master_ts_rpc_timeout_ms);
DECLARE_int32(tablet_creation_timeout_ms);

namespace yb {
namespace master {

using namespace std::placeholders;

using std::string;
using std::shared_ptr;

using strings::Substitute;
using consensus::RaftPeerPB;
using tserver::TabletServerErrorPB;

// ============================================================================
//  Class PickSpecificUUID.
// ============================================================================
Status PickSpecificUUID::PickReplica(TSDescriptor** ts_desc) {
  shared_ptr<TSDescriptor> ts;
  if (!master_->ts_manager()->LookupTSByUUID(ts_uuid_, &ts)) {
    return STATUS(NotFound, "unknown tablet server id", ts_uuid_);
  }
  *ts_desc = ts.get();
  return Status::OK();
}

string ReplicaMapToString(const TabletInfo::ReplicaMap& replicas) {
  string ret = "";
  for (const auto& r : replicas) {
    if (!ret.empty()) {
      ret += ", ";
    } else {
      ret += "(";
    }
    ret += r.second.ts_desc->permanent_uuid();
  }
  ret += ")";
  return ret;
}

// ============================================================================
//  Class PickLeaderReplica.
// ============================================================================
Status PickLeaderReplica::PickReplica(TSDescriptor** ts_desc) {
  *ts_desc = VERIFY_RESULT(tablet_->GetLeader());
  return Status::OK();
}

// ============================================================================
//  Class RetryingTSRpcTask.
// ============================================================================

RetryingTSRpcTask::RetryingTSRpcTask(Master *master,
                                     ThreadPool* callback_pool,
                                     gscoped_ptr<TSPicker> replica_picker,
                                     const scoped_refptr<TableInfo>& table)
  : master_(master),
    callback_pool_(callback_pool),
    replica_picker_(replica_picker.Pass()),
    table_(table),
    start_ts_(MonoTime::Now()),
    attempt_(0),
    state_(MonitoredTaskState::kWaiting) {
  deadline_ = start_ts_;
  deadline_.AddDelta(MonoDelta::FromMilliseconds(FLAGS_unresponsive_ts_rpc_timeout_ms));
}

// Send the subclass RPC request.
Status RetryingTSRpcTask::Run() {
  VLOG_WITH_PREFIX(1) << "Start Running";
  ++attempt_;
  auto task_state = state();
  if (task_state == MonitoredTaskState::kAborted) {
    return STATUS(IllegalState, "Unable to run task because it has been aborted");
  }
  // TODO(bogdan): There is a race between scheduling and running and can cause this to fail.
  // Should look into removing the kScheduling state, if not needed, and simplifying the state
  // transitions!
  DCHECK(task_state == MonitoredTaskState::kWaiting) << "State: " << ToString(task_state);

  Status s = ResetTSProxy();
  if (!s.ok()) {
    s = s.CloneAndPrepend("Failed to reset TS proxy");
    if (s.IsExpired()) {
      TransitionToTerminalState(MonitoredTaskState::kWaiting, MonitoredTaskState::kFailed, s);
      UnregisterAsyncTask();
      return s;
    }
    if (RescheduleWithBackoffDelay()) {
      return Status::OK();
    }

    auto state = this->state();
    UnregisterAsyncTask(); // May delete this.

    if (state == MonitoredTaskState::kFailed) {
      return s;
    }
    if (state == MonitoredTaskState::kAborted) {
      return STATUS(IllegalState, "Unable to run task because it has been aborted");
    }

    LOG_WITH_PREFIX(FATAL) << "Failed to change task to MonitoredTaskState::kFailed state from "
                           << state;
  } else {
    rpc_.Reset();
  }

  // Calculate and set the timeout deadline.
  const MonoTime deadline = ComputeDeadline();
  rpc_.set_deadline(deadline);

  if (!PerformStateTransition(MonitoredTaskState::kWaiting, MonitoredTaskState::kRunning)) {
    if (state() == MonitoredTaskState::kAborted) {
      return STATUS(Aborted, "Unable to run task because it has been aborted");
    }

    LOG_WITH_PREFIX(DFATAL) <<
        "Task transition MonitoredTaskState::kWaiting -> MonitoredTaskState::kRunning failed";
    return Failed(STATUS_FORMAT(IllegalState, "Task in invalid state $0", state()));
  }

  auto slowdown_flag_val = GetAtomicFlag(&FLAGS_TEST_slowdown_master_async_rpc_tasks_by_ms);
  if (PREDICT_FALSE(slowdown_flag_val> 0)) {
    VLOG_WITH_PREFIX(1) << "Slowing down by " << slowdown_flag_val << " ms.";
    bool old_thread_restriction = ThreadRestrictions::SetWaitAllowed(true);
    SleepFor(MonoDelta::FromMilliseconds(slowdown_flag_val));
    ThreadRestrictions::SetWaitAllowed(old_thread_restriction);
    VLOG_WITH_PREFIX(2) << "Slowing down done. Resuming.";
  }
  if (!SendRequest(attempt_) && !RescheduleWithBackoffDelay()) {
    UnregisterAsyncTask();  // May call 'delete this'.
  }
  return Status::OK();
}

MonoTime RetryingTSRpcTask::ComputeDeadline() {
  MonoTime timeout = MonoTime::Now();
  timeout.AddDelta(MonoDelta::FromMilliseconds(FLAGS_master_ts_rpc_timeout_ms));
  return MonoTime::Earliest(timeout, deadline_);
}

// Abort this task and return its value before it was successfully aborted. If the task entered
// a different terminal state before we were able to abort it, return that state.
MonitoredTaskState RetryingTSRpcTask::AbortAndReturnPrevState(const Status& status) {
  auto prev_state = state();
  while (!IsStateTerminal(prev_state)) {
    auto expected = prev_state;
    if (state_.compare_exchange_weak(expected, MonitoredTaskState::kAborted)) {
      AbortIfScheduled();
      Finished(status);
      UnregisterAsyncTask();
      return prev_state;
    }
    prev_state = state();
  }
  UnregisterAsyncTask();
  return prev_state;
}

void RetryingTSRpcTask::AbortTask(const Status& status) {
  AbortAndReturnPrevState(status);
}

void RetryingTSRpcTask::RpcCallback() {
  // Defer the actual work of the callback off of the reactor thread.
  // This is necessary because our callbacks often do synchronous writes to
  // the catalog table, and we can't do synchronous IO on the reactor.
  //
  // Note: This can fail on shutdown, so just print a warning for it.
  Status s = callback_pool_->SubmitFunc(
      std::bind(&RetryingTSRpcTask::DoRpcCallback, shared_from(this)));
  if (!s.ok()) {
    WARN_NOT_OK(s, "Could not submit to queue, probably shutting down");
    AbortTask(s);
  }
}

// Handle the actual work of the RPC callback. This is run on the master's worker
// pool, rather than a reactor thread, so it may do blocking IO operations.
void RetryingTSRpcTask::DoRpcCallback() {
  if (!rpc_.status().ok()) {
    LOG_WITH_PREFIX(WARNING) << "TS " << target_ts_desc_->permanent_uuid() << ": "
                             << type_name() << " RPC failed for tablet "
                             << tablet_id() << ": " << rpc_.status().ToString();
  } else if (state() != MonitoredTaskState::kAborted) {
    HandleResponse(attempt_);  // Modifies state_.
  }

  // Schedule a retry if the RPC call was not successful.
  if (RescheduleWithBackoffDelay()) {
    return;
  }

  UnregisterAsyncTask();  // May call 'delete this'.
}

int RetryingTSRpcTask::num_max_retries() { return FLAGS_unresponsive_ts_rpc_retry_limit; }

int RetryingTSRpcTask::max_delay_ms() {
  return FLAGS_retrying_ts_rpc_max_delay_ms;
}

bool RetryingTSRpcTask::RescheduleWithBackoffDelay() {
  auto task_state = state();
  if (task_state != MonitoredTaskState::kRunning &&
      // Allow kWaiting for task(s) that have never successfully ResetTSProxy().
      task_state != MonitoredTaskState::kWaiting) {
    if (task_state != MonitoredTaskState::kComplete) {
      LOG_WITH_PREFIX(INFO) << "No reschedule for this task";
    }
    return false;
  }

  int attempt_threshold = std::numeric_limits<int>::max();
  if (NoRetryTaskType()) {
    attempt_threshold = 0;
  } else if (RetryLimitTaskType()) {
    attempt_threshold = num_max_retries();
  }

  if (attempt_ > attempt_threshold) {
    auto status = STATUS_FORMAT(
        Aborted, "Reached maximum number of retries ($0)", attempt_threshold);
    LOG_WITH_PREFIX(WARNING)
        << status << " for request " << description()
        << ", task=" << this << " state=" << state();
    TransitionToFailedState(task_state, status);
    return false;
  }

  MonoTime now = MonoTime::Now();
  // We assume it might take 10ms to process the request in the best case,
  // fail if we have less than that amount of time remaining.
  int64_t millis_remaining = deadline_.GetDeltaSince(now).ToMilliseconds() - 10;
  // Exponential backoff with jitter.
  int64_t base_delay_ms;
  if (attempt_ <= 12) {
    base_delay_ms = 1 << (attempt_ + 3);  // 1st retry delayed 2^4 ms, 2nd 2^5, etc.
  } else {
    base_delay_ms = max_delay_ms();
  }
  // Normal rand is seeded by default with 1. Using the same for rand_r seed.
  unsigned int seed = 1;
  int64_t jitter_ms = rand_r(&seed) % 50;  // Add up to 50ms of additional random delay.
  int64_t delay_millis = std::min<int64_t>(base_delay_ms + jitter_ms, millis_remaining);

  if (delay_millis <= 0) {
    auto status = STATUS(TimedOut, "Request timed out");
    LOG_WITH_PREFIX(WARNING) << status;
    TransitionToFailedState(task_state, status);
    return false;
  }

  LOG_WITH_PREFIX(INFO) << "Scheduling retry with a delay of " << delay_millis
                        << "ms (attempt = " << attempt_ << ")...";

  if (!PerformStateTransition(task_state, MonitoredTaskState::kScheduling)) {
    LOG_WITH_PREFIX(WARNING) << "Unable to mark this task as MonitoredTaskState::kScheduling";
    return false;
  }
  auto task_id = master_->messenger()->ScheduleOnReactor(
      std::bind(&RetryingTSRpcTask::RunDelayedTask, shared_from(this), _1),
      MonoDelta::FromMilliseconds(delay_millis), SOURCE_LOCATION(), master_->messenger());
  reactor_task_id_.store(task_id, std::memory_order_release);

  if (task_id == rpc::kInvalidTaskId) {
    AbortTask(STATUS(Aborted, "Messenger closing"));
    UnregisterAsyncTask();
    return false;
  }

  return TransitionToWaitingState(MonitoredTaskState::kScheduling);
}

void RetryingTSRpcTask::RunDelayedTask(const Status& status) {
  if (state() == MonitoredTaskState::kAborted) {
    UnregisterAsyncTask();  // May delete this.
    return;
  }

  if (!status.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Async tablet task failed or was cancelled: " << status;
    if (status.IsAborted() || status.IsServiceUnavailable()) {
      AbortTask(status);
    }
    UnregisterAsyncTask();  // May delete this.
    return;
  }

  string desc = description();  // Save in case we need to log after deletion.
  Status s = Run();  // May delete this.
  if (!s.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Async tablet task failed: " << s.ToString();
  }
}

void RetryingTSRpcTask::UnregisterAsyncTaskCallback() {}

Status RetryingTSRpcTask::Failed(const Status& status) {
  LOG_WITH_PREFIX(WARNING) << "Async task failed: " << status;
  Finished(status);
  UnregisterAsyncTask();
  return status;
}

void RetryingTSRpcTask::UnregisterAsyncTask() {
  // Retain a reference to the object, in case RemoveTask would have removed the last one.
  auto self = shared_from_this();
  std::unique_lock<decltype(unregister_mutex_)> lock(unregister_mutex_);
  auto s = state();
  if (!IsStateTerminal(s)) {
    LOG_WITH_PREFIX(FATAL) << "Invalid task state " << s;
  }
  end_ts_ = MonoTime::Now();
  if (table_ != nullptr) {
    table_->RemoveTask(self);
  }
  // Make sure to run the callbacks last, in case they rely on the task no longer being tracked
  // by the table.
  UnregisterAsyncTaskCallback();
}

void RetryingTSRpcTask::AbortIfScheduled() {
  auto reactor_task_id = reactor_task_id_.load(std::memory_order_acquire);
  if (reactor_task_id != rpc::kInvalidTaskId) {
    master_->messenger()->AbortOnReactor(reactor_task_id);
  }
}

Status RetryingTSRpcTask::ResetTSProxy() {
  // TODO: if there is no replica available, should we still keep the task running?
  RETURN_NOT_OK(replica_picker_->PickReplica(&target_ts_desc_));

  shared_ptr<tserver::TabletServerServiceProxy> ts_proxy;
  shared_ptr<tserver::TabletServerAdminServiceProxy> ts_admin_proxy;
  shared_ptr<consensus::ConsensusServiceProxy> consensus_proxy;
  RETURN_NOT_OK(target_ts_desc_->GetProxy(&ts_proxy));
  RETURN_NOT_OK(target_ts_desc_->GetProxy(&ts_admin_proxy));
  RETURN_NOT_OK(target_ts_desc_->GetProxy(&consensus_proxy));

  ts_proxy_.swap(ts_proxy);
  ts_admin_proxy_.swap(ts_admin_proxy);
  consensus_proxy_.swap(consensus_proxy);

  return Status::OK();
}

void RetryingTSRpcTask::TransitionToTerminalState(MonitoredTaskState expected,
                                                  MonitoredTaskState terminal_state,
                                                  const Status& status) {
  if (!PerformStateTransition(expected, terminal_state)) {
    if (terminal_state != MonitoredTaskState::kAborted && state() == MonitoredTaskState::kAborted) {
      LOG_WITH_PREFIX(WARNING) << "Unable to perform transition " << expected << " -> "
                               << terminal_state << ". Task has been aborted";
    } else {
      LOG_WITH_PREFIX(DFATAL) << "State transition " << expected << " -> "
                              << terminal_state << " failed. Current task is in an invalid state: "
                              << state();
    }
    return;
  }

  Finished(status);
}

void RetryingTSRpcTask::TransitionToFailedState(yb::MonitoredTaskState expected,
                                                const yb::Status& status) {
  TransitionToTerminalState(expected, MonitoredTaskState::kFailed, status);
}

void RetryingTSRpcTask::TransitionToCompleteState() {
  TransitionToTerminalState(
      MonitoredTaskState::kRunning, MonitoredTaskState::kComplete, Status::OK());
}

bool RetryingTSRpcTask::TransitionToWaitingState(MonitoredTaskState expected) {
  if (!PerformStateTransition(expected, MonitoredTaskState::kWaiting)) {
    // The only valid reason for state not being MonitoredTaskState is because the task got
    // aborted.
    if (state() != MonitoredTaskState::kAborted) {
      LOG_WITH_PREFIX(FATAL) << "Unable to mark task as MonitoredTaskState::kWaiting";
    }
    AbortIfScheduled();
    return false;
  } else {
    return true;
  }
}

// ============================================================================
//  Class AsyncTabletLeaderTask.
// ============================================================================
AsyncTabletLeaderTask::AsyncTabletLeaderTask(
    Master* master, ThreadPool* callback_pool, const scoped_refptr<TabletInfo>& tablet)
    : RetryingTSRpcTask(
          master, callback_pool, gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)),
          tablet->table().get()),
      tablet_(tablet) {
}

AsyncTabletLeaderTask::AsyncTabletLeaderTask(
    Master* master, ThreadPool* callback_pool, const scoped_refptr<TabletInfo>& tablet,
    const scoped_refptr<TableInfo>& table)
    : RetryingTSRpcTask(
          master, callback_pool, gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)), table),
      tablet_(tablet) {
}

std::string AsyncTabletLeaderTask::description() const {
  return type_name() + " RPC for " + tablet_->ToString();
}

TabletId AsyncTabletLeaderTask::tablet_id() const {
  return tablet_->tablet_id();
}

TabletServerId AsyncTabletLeaderTask::permanent_uuid() const {
  return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
}

// ============================================================================
//  Class AsyncCreateReplica.
// ============================================================================
AsyncCreateReplica::AsyncCreateReplica(Master *master,
                                       ThreadPool *callback_pool,
                                       const string& permanent_uuid,
                                       const scoped_refptr<TabletInfo>& tablet)
  : RetrySpecificTSRpcTask(master, callback_pool, permanent_uuid, tablet->table().get()),
    tablet_id_(tablet->tablet_id()) {
  deadline_ = start_ts_;
  deadline_.AddDelta(MonoDelta::FromMilliseconds(FLAGS_tablet_creation_timeout_ms));

  auto table_lock = tablet->table()->LockForRead();
  const SysTabletsEntryPB& tablet_pb = tablet->metadata().dirty().pb;

  req_.set_dest_uuid(permanent_uuid);
  req_.set_table_id(tablet->table()->id());
  req_.set_tablet_id(tablet->tablet_id());
  req_.set_table_type(tablet->table()->metadata().state().pb.table_type());
  req_.mutable_partition()->CopyFrom(tablet_pb.partition());
  req_.set_namespace_id(table_lock->data().pb.namespace_id());
  req_.set_namespace_name(table_lock->data().pb.namespace_name());
  req_.set_table_name(table_lock->data().pb.name());
  req_.mutable_schema()->CopyFrom(table_lock->data().pb.schema());
  req_.mutable_partition_schema()->CopyFrom(table_lock->data().pb.partition_schema());
  req_.mutable_config()->CopyFrom(tablet_pb.committed_consensus_state().config());
  req_.set_colocated(tablet_pb.colocated());
  if (table_lock->data().pb.has_index_info()) {
    req_.mutable_index_info()->CopyFrom(table_lock->data().pb.index_info());
  }
}

void AsyncCreateReplica::HandleResponse(int attempt) {
  if (resp_.has_error()) {
    Status s = StatusFromPB(resp_.error().status());
    if (s.IsAlreadyPresent()) {
      LOG_WITH_PREFIX(INFO) << "CreateTablet RPC for tablet " << tablet_id_
                            << " on TS " << permanent_uuid_ << " returned already present: "
                            << s;
      TransitionToCompleteState();
    } else {
      LOG_WITH_PREFIX(WARNING) << "CreateTablet RPC for tablet " << tablet_id_
                               << " on TS " << permanent_uuid_ << " failed: " << s;
    }

    return;
  }

  TransitionToCompleteState();
}

bool AsyncCreateReplica::SendRequest(int attempt) {
  ts_admin_proxy_->CreateTabletAsync(req_, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1) << "Send create tablet request to " << permanent_uuid_ << ":\n"
                      << " (attempt " << attempt << "):\n"
                      << req_.DebugString();
  return true;
}

// ============================================================================
//  Class AsyncStartElection.
// ============================================================================
AsyncStartElection::AsyncStartElection(Master *master,
                                       ThreadPool *callback_pool,
                                       const string& permanent_uuid,
                                       const scoped_refptr<TabletInfo>& tablet)
  : RetrySpecificTSRpcTask(master, callback_pool, permanent_uuid, tablet->table().get()),
    tablet_id_(tablet->tablet_id()) {
  deadline_ = start_ts_;
  deadline_.AddDelta(MonoDelta::FromMilliseconds(FLAGS_tablet_creation_timeout_ms));

  req_.set_dest_uuid(permanent_uuid_);
  req_.set_tablet_id(tablet_id_);
  req_.set_initial_election(true);
}

void AsyncStartElection::HandleResponse(int attempt) {
  if (resp_.has_error()) {
    Status s = StatusFromPB(resp_.error().status());
    if (!s.ok()) {
      LOG_WITH_PREFIX(WARNING) << "RunLeaderElection RPC for tablet " << tablet_id_
                               << " on TS " << permanent_uuid_ << " failed: " << s;
    }

    return;
  }

  TransitionToCompleteState();
}

bool AsyncStartElection::SendRequest(int attempt) {
  LOG_WITH_PREFIX(INFO) << Format(
      "Hinted Leader start election at $0 for tablet $1, attempt $2",
      permanent_uuid_, tablet_id_, attempt);
  consensus_proxy_->RunLeaderElectionAsync(req_, &resp_, &rpc_, BindRpcCallback());

  return true;
}

// ============================================================================
//  Class AsyncDeleteReplica.
// ============================================================================
void AsyncDeleteReplica::HandleResponse(int attempt) {
  if (resp_.has_error()) {
    Status status = StatusFromPB(resp_.error().status());

    // Do not retry on a fatal error
    TabletServerErrorPB::Code code = resp_.error().code();
    switch (code) {
      case TabletServerErrorPB::TABLET_NOT_FOUND:
        LOG_WITH_PREFIX(WARNING)
            << "TS " << permanent_uuid_ << ": delete failed for tablet " << tablet_id_
            << " because the tablet was not found. No further retry: "
            << status.ToString();
        TransitionToCompleteState();
        break;
      case TabletServerErrorPB::CAS_FAILED:
        LOG_WITH_PREFIX(WARNING)
            << "TS " << permanent_uuid_ << ": delete failed for tablet " << tablet_id_
            << " due to a CAS failure. No further retry: " << status.ToString();
        TransitionToCompleteState();
        break;
      case TabletServerErrorPB::WRONG_SERVER_UUID:
        LOG_WITH_PREFIX(WARNING)
            << "TS " << permanent_uuid_ << ": delete failed for tablet " << tablet_id_
            << " due to an incorrect UUID. No further retry: " << status.ToString();
        TransitionToCompleteState();
        break;
      default:
        LOG_WITH_PREFIX(WARNING)
            << "TS " << permanent_uuid_ << ": delete failed for tablet " << tablet_id_
            << " with error code " << TabletServerErrorPB::Code_Name(code)
            << ": " << status.ToString();
        break;
    }
  } else {
    if (table_) {
      LOG_WITH_PREFIX(INFO)
          << "TS " << permanent_uuid_ << ": tablet " << tablet_id_
          << " (table " << table_->ToString() << ") successfully deleted";
    } else {
      LOG_WITH_PREFIX(WARNING)
          << "TS " << permanent_uuid_ << ": tablet " << tablet_id_
          << " did not belong to a known table, but was successfully deleted";
    }
    TransitionToCompleteState();
    VLOG_WITH_PREFIX(1) << "TS " << permanent_uuid_ << ": delete complete on tablet " << tablet_id_;
  }
}

bool AsyncDeleteReplica::SendRequest(int attempt) {
  tserver::DeleteTabletRequestPB req;
  req.set_dest_uuid(permanent_uuid_);
  req.set_tablet_id(tablet_id_);
  req.set_reason(reason_);
  req.set_delete_type(delete_type_);
  if (cas_config_opid_index_less_or_equal_) {
    req.set_cas_config_opid_index_less_or_equal(*cas_config_opid_index_less_or_equal_);
  }

  ts_admin_proxy_->DeleteTabletAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1) << "Send delete tablet request to " << permanent_uuid_
                      << " (attempt " << attempt << "):\n"
                      << req.DebugString();
  return true;
}

void AsyncDeleteReplica::UnregisterAsyncTaskCallback() {
  // Only notify if we are in a success state.
  if (state() == MonitoredTaskState::kComplete) {
    master_->catalog_manager()->NotifyTabletDeleteFinished(permanent_uuid_, tablet_id_, table());
  }
}

// ============================================================================
//  Class AsyncAlterTable.
// ============================================================================
void AsyncAlterTable::HandleResponse(int attempt) {
  if (resp_.has_error()) {
    Status status = StatusFromPB(resp_.error().status());

    LOG_WITH_PREFIX(WARNING) << "TS " << permanent_uuid() << " failed: "
                             << status << " for version " << schema_version_;

    // Do not retry on a fatal error
    switch (resp_.error().code()) {
      case TabletServerErrorPB::TABLET_NOT_FOUND:
      case TabletServerErrorPB::MISMATCHED_SCHEMA:
      case TabletServerErrorPB::TABLET_HAS_A_NEWER_SCHEMA:
        TransitionToCompleteState();
        break;
      default:
        break;
    }
  } else {
    TransitionToCompleteState();
    VLOG_WITH_PREFIX(1)
        << "TS " << permanent_uuid() << " completed: for version " << schema_version_;
  }

  server::UpdateClock(resp_, master_->clock());

  if (state() == MonitoredTaskState::kComplete) {
    // TODO: proper error handling here. Not critical, since TSHeartbeat will retry on failure.
    WARN_NOT_OK(master_->catalog_manager()->HandleTabletSchemaVersionReport(
        tablet_.get(), schema_version_, table()),
        yb::Format(
            "$0 for $1 failed while running AsyncAlterTable::HandleResponse. response $2",
            description(), tablet_->ToString(), resp_.DebugString()));
  } else {
    VLOG_WITH_PREFIX(1) << "Task is not completed " << tablet_->ToString() << " for version "
                        << schema_version_;
  }
}

bool AsyncAlterTable::SendRequest(int attempt) {
  VLOG_WITH_PREFIX(1) << "Send alter table request to " << permanent_uuid() << " for "
                      << tablet_->tablet_id() << " waiting for a read lock.";
  auto l = table_->LockForRead();
  VLOG_WITH_PREFIX(1) << "Send alter table request to " << permanent_uuid() << " for "
                      << tablet_->tablet_id() << " obtained the read lock.";

  tserver::ChangeMetadataRequestPB req;
  req.set_schema_version(l->data().pb.version());
  req.set_dest_uuid(permanent_uuid());
  req.set_tablet_id(tablet_->tablet_id());
  req.set_alter_table_id(table_->id());

  if (l->data().pb.has_wal_retention_secs()) {
    req.set_wal_retention_secs(l->data().pb.wal_retention_secs());
  }

  req.mutable_schema()->CopyFrom(l->data().pb.schema());
  req.set_new_table_name(l->data().pb.name());
  req.mutable_indexes()->CopyFrom(l->data().pb.indexes());
  req.set_propagated_hybrid_time(master_->clock()->Now().ToUint64());

  schema_version_ = l->data().pb.version();

  l->Unlock();

  ts_admin_proxy_->AlterSchemaAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1)
      << "Send alter table request to " << permanent_uuid() << " for " << tablet_->tablet_id()
      << " (attempt " << attempt << "):\n" << req.DebugString();
  return true;
}

bool AsyncBackfillDone::SendRequest(int attempt) {
  VLOG_WITH_PREFIX(1)
      << "Send alter table request to " << permanent_uuid() << " for " << tablet_->tablet_id()
      << " version " << schema_version_ << " waiting for a read lock.";
  auto l = table_->LockForRead();
  VLOG_WITH_PREFIX(1)
      << "Send alter table request to " << permanent_uuid() << " for " << tablet_->tablet_id()
      << " version " << schema_version_ << " obtained the read lock.";

  tserver::ChangeMetadataRequestPB req;
  req.set_dest_uuid(permanent_uuid());
  req.set_tablet_id(tablet_->tablet_id());
  req.set_propagated_hybrid_time(master_->clock()->Now().ToUint64());
  req.set_mark_backfill_done(true);
  schema_version_ = l->data().pb.version();
  l->Unlock();

  ts_admin_proxy_->BackfillDoneAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1)
      << "Send backfill done request to " << permanent_uuid() << " for " << tablet_->tablet_id()
      << " (attempt " << attempt << "):\n" << req.DebugString();
  return true;
}

// ============================================================================
//  Class AsyncCopartitionTable.
// ============================================================================
AsyncCopartitionTable::AsyncCopartitionTable(Master *master,
                                             ThreadPool* callback_pool,
                                             const scoped_refptr<TabletInfo>& tablet,
                                             const scoped_refptr<TableInfo>& table)
    : RetryingTSRpcTask(master,
                        callback_pool,
                        gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)),
                        table.get()),
      tablet_(tablet), table_(table) {
}

string AsyncCopartitionTable::description() const {
  return "Copartition Table RPC for tablet " + tablet_->ToString()
          + " for " + table_->ToString();
}

TabletId AsyncCopartitionTable::tablet_id() const {
  return tablet_->tablet_id();
}

TabletServerId AsyncCopartitionTable::permanent_uuid() const {
  return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
}

// TODO(sagnik): modify this to fill all relevant fields for the AsyncCopartition request.
bool AsyncCopartitionTable::SendRequest(int attempt) {

  tserver::CopartitionTableRequestPB req;
  req.set_dest_uuid(permanent_uuid());
  req.set_tablet_id(tablet_->tablet_id());
  req.set_table_id(table_->id());
  req.set_table_name(table_->name());

  ts_admin_proxy_->CopartitionTableAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1) << "Send copartition table request to " << permanent_uuid()
                      << " (attempt " << attempt << "):\n" << req.DebugString();
  return true;
}

// TODO(sagnik): modify this to handle the AsyncCopartition Response and retry fail as necessary.
void AsyncCopartitionTable::HandleResponse(int attempt) {
  LOG_WITH_PREFIX(INFO) << "master can't handle server responses yet";
}

// ============================================================================
//  Class AsyncTruncate.
// ============================================================================
void AsyncTruncate::HandleResponse(int attempt) {
  if (resp_.has_error()) {
    const Status s = StatusFromPB(resp_.error().status());
    const TabletServerErrorPB::Code code = resp_.error().code();
    LOG_WITH_PREFIX(WARNING)
        << "TS " << permanent_uuid() << ": truncate failed for tablet " << tablet_id()
        << " with error code " << TabletServerErrorPB::Code_Name(code) << ": " << s;
  } else {
    VLOG_WITH_PREFIX(1)
        << "TS " << permanent_uuid() << ": truncate complete on tablet " << tablet_id();
    TransitionToCompleteState();
  }

  server::UpdateClock(resp_, master_->clock());
}

bool AsyncTruncate::SendRequest(int attempt) {
  tserver::TruncateRequestPB req;
  req.set_tablet_id(tablet_id());
  req.set_propagated_hybrid_time(master_->clock()->Now().ToUint64());
  ts_proxy_->TruncateAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1) << "Send truncate tablet request to " << permanent_uuid()
                      << " (attempt " << attempt << "):\n" << req.DebugString();
  return true;
}

// ============================================================================
//  Class CommonInfoForRaftTask.
// ============================================================================
CommonInfoForRaftTask::CommonInfoForRaftTask(
    Master* master, ThreadPool* callback_pool, const scoped_refptr<TabletInfo>& tablet,
    const consensus::ConsensusStatePB& cstate, const string& change_config_ts_uuid)
    : RetryingTSRpcTask(
          master, callback_pool, gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)),
          tablet->table()),
      tablet_(tablet),
      cstate_(cstate),
      change_config_ts_uuid_(change_config_ts_uuid) {
  deadline_ = MonoTime::Max();  // Never time out.
}

TabletId CommonInfoForRaftTask::tablet_id() const {
  return tablet_->tablet_id();
}

TabletServerId CommonInfoForRaftTask::permanent_uuid() const {
  return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
}

// ============================================================================
//  Class AsyncChangeConfigTask.
// ============================================================================
string AsyncChangeConfigTask::description() const {
  return strings::Substitute(
      "$0 RPC for tablet $1 on peer $2 with cas_config_opid_index $3", type_name(),
      tablet_->tablet_id(), permanent_uuid(), cstate_.config().opid_index());
}

bool AsyncChangeConfigTask::SendRequest(int attempt) {
  // Bail if we're retrying in vain.
  int64_t latest_index;
  {
    auto tablet_lock = tablet_->LockForRead();
    latest_index = tablet_lock->data().pb.committed_consensus_state().config().opid_index();
    // Adding this logic for a race condition that occurs in this scenario:
    // 1. CatalogManager receives a DeleteTable request and sends DeleteTablet requests to the
    // tservers, but doesn't yet update the tablet in memory state to not running.
    // 2. The CB runs and sees that this tablet is still running, sees that it is over-replicated
    // (since the placement now dictates it should have 0 replicas),
    // but before it can send the ChangeConfig RPC to a tserver.
    // 3. That tserver processes the DeleteTablet request.
    // 4. The ChangeConfig RPC now returns tablet not found,
    // which prompts an infinite retry of the RPC.
    bool tablet_running = tablet_lock->data().is_running();
    if (!tablet_running) {
      AbortTask(STATUS(Aborted, "Tablet is not running"));
      return false;
    }
  }
  if (latest_index > cstate_.config().opid_index()) {
    auto status = STATUS_FORMAT(
        Aborted,
        "Latest config for has opid_index of $0 while this task has opid_index of $1",
        latest_index, cstate_.config().opid_index());
    LOG_WITH_PREFIX(INFO) << status;
    AbortTask(status);
    return false;
  }

  // Logging should be covered inside based on failure reasons.
  auto prepare_status = PrepareRequest(attempt);
  if (!prepare_status.ok()) {
    AbortTask(prepare_status);
    return false;
  }

  consensus_proxy_->ChangeConfigAsync(req_, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1) << "Task " << description() << " sent request:\n" << req_.DebugString();
  return true;
}

void AsyncChangeConfigTask::HandleResponse(int attempt) {
  if (!resp_.has_error()) {
    TransitionToCompleteState();
    LOG_WITH_PREFIX(INFO) << Substitute(
        "Change config succeeded on leader TS $0 for tablet $1 with type $2 for replica $3",
        permanent_uuid(), tablet_->tablet_id(), type_name(), change_config_ts_uuid_);
    return;
  }

  Status status = StatusFromPB(resp_.error().status());

  // Do not retry on some known errors, otherwise retry forever or until cancelled.
  switch (resp_.error().code()) {
    case TabletServerErrorPB::CAS_FAILED:
    case TabletServerErrorPB::ADD_CHANGE_CONFIG_ALREADY_PRESENT:
    case TabletServerErrorPB::REMOVE_CHANGE_CONFIG_NOT_PRESENT:
    case TabletServerErrorPB::NOT_THE_LEADER:
      LOG_WITH_PREFIX(WARNING) << "ChangeConfig() failed on leader " << permanent_uuid()
                               << ". No further retry: " << status.ToString();
      TransitionToCompleteState();
      break;
    default:
      LOG_WITH_PREFIX(INFO) << "ChangeConfig() failed on leader " << permanent_uuid()
                            << " due to error "
                            << TabletServerErrorPB::Code_Name(resp_.error().code())
                            << ". This operation will be retried. Error detail: "
                            << status.ToString();
      break;
  }
}

// ============================================================================
//  Class AsyncAddServerTask.
// ============================================================================
Status AsyncAddServerTask::PrepareRequest(int attempt) {
  // Select the replica we wish to add to the config.
  // Do not include current members of the config.
  unordered_set<string> replica_uuids;
  for (const RaftPeerPB& peer : cstate_.config().peers()) {
    InsertOrDie(&replica_uuids, peer.permanent_uuid());
  }
  TSDescriptorVector ts_descs;
  master_->ts_manager()->GetAllLiveDescriptors(&ts_descs);
  shared_ptr<TSDescriptor> replacement_replica;
  for (auto ts_desc : ts_descs) {
    if (ts_desc->permanent_uuid() == change_config_ts_uuid_) {
      // This is given by the client, so we assume it is a well chosen uuid.
      replacement_replica = ts_desc;
      break;
    }
  }
  if (PREDICT_FALSE(!replacement_replica)) {
    auto status = STATUS_FORMAT(
        TimedOut, "Could not find desired replica $0 in live set", change_config_ts_uuid_);
    LOG_WITH_PREFIX(WARNING) << status;
    return status;
  }

  req_.set_dest_uuid(permanent_uuid());
  req_.set_tablet_id(tablet_->tablet_id());
  req_.set_type(consensus::ADD_SERVER);
  req_.set_cas_config_opid_index(cstate_.config().opid_index());
  RaftPeerPB* peer = req_.mutable_server();
  peer->set_permanent_uuid(replacement_replica->permanent_uuid());
  peer->set_member_type(member_type_);
  TSRegistrationPB peer_reg = replacement_replica->GetRegistration();

  if (peer_reg.common().private_rpc_addresses().empty()) {
    auto status = STATUS_FORMAT(
        IllegalState, "Candidate replacement $0 has no registered rpc address: $1",
        replacement_replica->permanent_uuid(), peer_reg);
    YB_LOG_EVERY_N(WARNING, 100) << LogPrefix() << status;
    return status;
  }

  TakeRegistration(peer_reg.mutable_common(), peer);

  return Status::OK();
}

// ============================================================================
//  Class AsyncRemoveServerTask.
// ============================================================================
Status AsyncRemoveServerTask::PrepareRequest(int attempt) {
  bool found = false;
  for (const RaftPeerPB& peer : cstate_.config().peers()) {
    if (change_config_ts_uuid_ == peer.permanent_uuid()) {
      found = true;
    }
  }

  if (!found) {
    auto status = STATUS_FORMAT(
        NotFound, "Asked to remove TS with uuid $0 but could not find it in config peers!",
        change_config_ts_uuid_);
    LOG_WITH_PREFIX(WARNING) << status;
    return status;
  }

  req_.set_dest_uuid(permanent_uuid());
  req_.set_tablet_id(tablet_->tablet_id());
  req_.set_type(consensus::REMOVE_SERVER);
  req_.set_cas_config_opid_index(cstate_.config().opid_index());
  RaftPeerPB* peer = req_.mutable_server();
  peer->set_permanent_uuid(change_config_ts_uuid_);

  return Status::OK();
}

// ============================================================================
//  Class AsyncTryStepDown.
// ============================================================================
Status AsyncTryStepDown::PrepareRequest(int attempt) {
  LOG_WITH_PREFIX(INFO) << Substitute("Prep Leader step down $0, leader_uuid=$1, change_ts_uuid=$2",
                                      attempt, permanent_uuid(), change_config_ts_uuid_);
  if (attempt > 1) {
    return STATUS(RuntimeError, "Retry is not allowed");
  }

  // If we were asked to remove the server even if it is the leader, we have to call StepDown, but
  // only if our current leader is the server we are asked to remove.
  if (permanent_uuid() != change_config_ts_uuid_) {
    auto status = STATUS_FORMAT(
        IllegalState,
        "Incorrect state config leader $0 does not match target uuid $1 for a leader step down op",
        permanent_uuid(), change_config_ts_uuid_);
    LOG_WITH_PREFIX(WARNING) << status;
    return status;
  }

  stepdown_req_.set_dest_uuid(change_config_ts_uuid_);
  stepdown_req_.set_tablet_id(tablet_->tablet_id());
  if (!new_leader_uuid_.empty()) {
    stepdown_req_.set_new_leader_uuid(new_leader_uuid_);
  }

  return Status::OK();
}

bool AsyncTryStepDown::SendRequest(int attempt) {
  auto prepare_status = PrepareRequest(attempt);
  if (!prepare_status.ok()) {
    AbortTask(prepare_status);
    return false;
  }

  LOG_WITH_PREFIX(INFO) << Substitute("Stepping down leader $0 for tablet $1",
                                      change_config_ts_uuid_, tablet_->tablet_id());
  consensus_proxy_->LeaderStepDownAsync(
      stepdown_req_, &stepdown_resp_, &rpc_, BindRpcCallback());

  return true;
}

void AsyncTryStepDown::HandleResponse(int attempt) {
  if (!rpc_.status().ok()) {
    AbortTask(rpc_.status());
    LOG_WITH_PREFIX(WARNING) << Substitute(
        "Got error on stepdown for tablet $0 with leader $1, attempt $2 and error $3",
        tablet_->tablet_id(), permanent_uuid(), attempt, rpc_.status().ToString());

    return;
  }

  TransitionToCompleteState();
  const bool stepdown_failed = stepdown_resp_.error().status().code() != AppStatusPB::OK;
  LOG_WITH_PREFIX(INFO) << Format(
      "Leader step down done attempt=$0, leader_uuid=$1, change_uuid=$2, "
      "error=$3, failed=$4, should_remove=$5 for tablet $6.",
      attempt, permanent_uuid(), change_config_ts_uuid_, stepdown_resp_.error(),
      stepdown_failed, should_remove_, tablet_->tablet_id());

  if (stepdown_failed) {
    tablet_->RegisterLeaderStepDownFailure(change_config_ts_uuid_,
        MonoDelta::FromMilliseconds(stepdown_resp_.has_time_since_election_failure_ms() ?
                                    stepdown_resp_.time_since_election_failure_ms() : 0));
  }

  if (should_remove_) {
    auto task = std::make_shared<AsyncRemoveServerTask>(
        master_, callback_pool_, tablet_, cstate_, change_config_ts_uuid_);

    tablet_->table()->AddTask(task);
    Status status = task->Run();
    WARN_NOT_OK(status, "Failed to send new RemoveServer request");
  }
}

// ============================================================================
//  Class AsyncAddTableToTablet.
// ============================================================================
AsyncAddTableToTablet::AsyncAddTableToTablet(
    Master* master, ThreadPool* callback_pool, const scoped_refptr<TabletInfo>& tablet,
    const scoped_refptr<TableInfo>& table)
    : RetryingTSRpcTask(
          master, callback_pool, gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)), table.get()),
      tablet_(tablet),
      table_(table),
      tablet_id_(tablet->tablet_id()) {
  req_.set_tablet_id(tablet->id());
  auto& add_table = *req_.mutable_add_table();
  add_table.set_table_id(table_->id());
  add_table.set_table_name(table_->name());
  add_table.set_table_type(table_->GetTableType());
  {
    auto l = table->LockForRead();
    add_table.set_schema_version(l->data().pb.version());
    *add_table.mutable_schema() = l->data().pb.schema();
    *add_table.mutable_partition_schema() = l->data().pb.partition_schema();
  }
}

string AsyncAddTableToTablet::description() const {
  return Substitute("AddTableToTablet RPC ($0) ($1)", table_->ToString(), tablet_->ToString());
}

void AsyncAddTableToTablet::HandleResponse(int attempt) {
  if (!rpc_.status().ok()) {
    AbortTask(rpc_.status());
    LOG_WITH_PREFIX(WARNING) << Substitute(
        "Got error when adding table $0 to tablet $1, attempt $2 and error $3",
        table_->ToString(), tablet_->ToString(), attempt, rpc_.status().ToString());
    return;
  }
  if (resp_.has_error()) {
    LOG_WITH_PREFIX(WARNING) << "AddTableToTablet() responded with error code "
                             << TabletServerErrorPB_Code_Name(resp_.error().code());
    switch (resp_.error().code()) {
      case TabletServerErrorPB::LEADER_NOT_READY_TO_SERVE: FALLTHROUGH_INTENDED;
      case TabletServerErrorPB::NOT_THE_LEADER:
        TransitionToWaitingState(MonitoredTaskState::kRunning);
        break;
      default:
        TransitionToCompleteState();
        break;
    }

    return;
  }

  TransitionToCompleteState();
}

bool AsyncAddTableToTablet::SendRequest(int attempt) {
  ts_admin_proxy_->AddTableToTabletAsync(req_, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1)
      << "Send AddTableToTablet request (attempt " << attempt << "):\n" << req_.DebugString();
  return true;
}

// ============================================================================
//  Class AsyncRemoveTableFromTablet.
// ============================================================================
AsyncRemoveTableFromTablet::AsyncRemoveTableFromTablet(
    Master* master, ThreadPool* callback_pool, const scoped_refptr<TabletInfo>& tablet,
    const scoped_refptr<TableInfo>& table)
    : RetryingTSRpcTask(
          master, callback_pool, gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)), table.get()),
      table_(table),
      tablet_(tablet),
      tablet_id_(tablet->tablet_id()) {
  req_.set_tablet_id(tablet->id());
  req_.set_remove_table_id(table->id());
}

string AsyncRemoveTableFromTablet::description() const {
  return Substitute("RemoveTableFromTablet RPC ($0) ($1)", table_->ToString(), tablet_->ToString());
}

void AsyncRemoveTableFromTablet::HandleResponse(int attempt) {
  if (!rpc_.status().ok()) {
    AbortTask(rpc_.status());
    LOG_WITH_PREFIX(WARNING) << Substitute(
        "Got error when removing table $0 from tablet $1, attempt $2 and error $3",
        table_->ToString(), tablet_->ToString(), attempt, rpc_.status().ToString());
    return;
  }
  if (resp_.has_error()) {
    LOG_WITH_PREFIX(WARNING) << "RemoveTableFromTablet() responded with error code "
                             << TabletServerErrorPB_Code_Name(resp_.error().code());
    switch (resp_.error().code()) {
      case TabletServerErrorPB::LEADER_NOT_READY_TO_SERVE: FALLTHROUGH_INTENDED;
      case TabletServerErrorPB::NOT_THE_LEADER:
        TransitionToWaitingState(MonitoredTaskState::kRunning);
        break;
      default:
        TransitionToCompleteState();
        break;
    }
  } else {
    TransitionToCompleteState();
  }
}

bool AsyncRemoveTableFromTablet::SendRequest(int attempt) {
  ts_admin_proxy_->RemoveTableFromTabletAsync(req_, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1) << "Send RemoveTableFromTablet request (attempt " << attempt << "):\n"
                      << req_.DebugString();
  return true;
}

// ============================================================================
//  Class AsyncSplitTablet.
// ============================================================================
AsyncSplitTablet::AsyncSplitTablet(
    Master* master, ThreadPool* callback_pool, const scoped_refptr<TabletInfo>& tablet,
    const std::array<TabletId, kNumSplitParts>& new_tablet_ids,
    const std::string& split_encoded_key, const std::string& split_partition_key)
    : AsyncTabletLeaderTask(master, callback_pool, tablet) {
  req_.set_tablet_id(tablet_id());
  req_.set_new_tablet1_id(new_tablet_ids[0]);
  req_.set_new_tablet2_id(new_tablet_ids[1]);
  req_.set_split_encoded_key(split_encoded_key);
  req_.set_split_partition_key(split_partition_key);
}

void AsyncSplitTablet::HandleResponse(int attempt) {
  if (resp_.has_error()) {
    const Status s = StatusFromPB(resp_.error().status());
    const TabletServerErrorPB::Code code = resp_.error().code();
    LOG_WITH_PREFIX(WARNING) << "TS " << permanent_uuid() << ": split (attempt " << attempt
                             << ") failed for tablet " << tablet_id() << " with error code "
                             << TabletServerErrorPB::Code_Name(code) << ": " << s;
  } else {
    VLOG_WITH_PREFIX(1)
        << "TS " << permanent_uuid() << ": split complete on tablet " << tablet_id();
    TransitionToCompleteState();
  }

  server::UpdateClock(resp_, master_->clock());
}

bool AsyncSplitTablet::SendRequest(int attempt) {
  req_.set_dest_uuid(permanent_uuid());
  req_.set_propagated_hybrid_time(master_->clock()->Now().ToUint64());
  ts_admin_proxy_->SplitTabletAsync(req_, &resp_, &rpc_, BindRpcCallback());
  VLOG_WITH_PREFIX(1)
      << "Sent split tablet request to " << permanent_uuid() << " (attempt " << attempt << "):\n"
      << req_.DebugString();
  return true;
}

}  // namespace master
}  // namespace yb
