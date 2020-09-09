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

#include "yb/tablet/operations/operation_driver.h"

#include <mutex>

#include "yb/client/client.h"
#include "yb/consensus/consensus.h"
#include "yb/gutil/strings/strcat.h"
#include "yb/master/sys_catalog_constants.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/operations/operation_tracker.h"
#include "yb/util/debug-util.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/threadpool.h"
#include "yb/util/thread_restrictions.h"
#include "yb/util/trace.h"

using namespace std::literals;

DEFINE_test_flag(int32, delay_execute_async_ms, 0,
                 "Delay execution of ExecuteAsync for specified amount of milliseconds during "
                     "tests");

namespace yb {
namespace tablet {

using namespace std::placeholders;
using std::shared_ptr;

using consensus::Consensus;
using consensus::ConsensusRound;
using consensus::ReplicateMsg;
using consensus::DriverType;
using log::Log;
using server::Clock;

////////////////////////////////////////////////////////////
// OperationDriver
////////////////////////////////////////////////////////////

OperationDriver::OperationDriver(OperationTracker *operation_tracker,
                                 Consensus* consensus,
                                 Log* log,
                                 Preparer* preparer,
                                 OperationOrderVerifier* order_verifier,
                                 TableType table_type)
    : operation_tracker_(operation_tracker),
      consensus_(consensus),
      log_(log),
      preparer_(preparer),
      order_verifier_(order_verifier),
      trace_(new Trace()),
      start_time_(MonoTime::Now()),
      replication_state_(NOT_REPLICATING),
      prepare_state_(NOT_PREPARED),
      table_type_(table_type) {
  if (Trace::CurrentTrace()) {
    Trace::CurrentTrace()->AddChildTrace(trace_.get());
  }
  DCHECK(op_id_copy_.is_lock_free());
}

Status OperationDriver::Init(std::unique_ptr<Operation>* operation, int64_t term) {
  if (operation) {
    operation_ = std::move(*operation);
  }

  if (term == OpId::kUnknownTerm) {
    if (operation_) {
      op_id_copy_.store(yb::OpId::FromPB(operation_->state()->op_id()),
                                         boost::memory_order_release);
    }
    replication_state_ = REPLICATING;
  } else {
    if (consensus_) {  // sometimes NULL in tests
      // Unretained is required to avoid a refcount cycle.
      consensus::ReplicateMsgPtr replicate_msg = operation_->NewReplicateMsg();
      mutable_state()->set_consensus_round(
        consensus_->NewRound(std::move(replicate_msg),
                             std::bind(&OperationDriver::ReplicationFinished, this, _1, _2, _3)));
      mutable_state()->consensus_round()->BindToTerm(term);
      mutable_state()->consensus_round()->SetAppendCallback(this);
    }
  }

  auto result = operation_tracker_->Add(this);
  if (!result.ok() && operation) {
    *operation = std::move(operation_);
  }

  return result;
}

yb::OpId OperationDriver::GetOpId() {
  return op_id_copy_.load(boost::memory_order_acquire);
}

const OperationState* OperationDriver::state() const {
  return operation_ != nullptr ? operation_->state() : nullptr;
}

OperationState* OperationDriver::mutable_state() {
  return operation_ != nullptr ? operation_->state() : nullptr;
}

OperationType OperationDriver::operation_type() const {
  return operation_ ? operation_->operation_type() : OperationType::kEmpty;
}

string OperationDriver::ToString() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return ToStringUnlocked();
}

string OperationDriver::ToStringUnlocked() const {
  string ret = StateString(replication_state_, prepare_state_);
  if (operation_ != nullptr) {
    ret += " " + operation_->ToString();
  } else {
    ret += "[unknown operation]";
  }
  return ret;
}


void OperationDriver::ExecuteAsync() {
  VLOG_WITH_PREFIX(4) << "ExecuteAsync()";
  TRACE_EVENT_FLOW_BEGIN0("operation", "ExecuteAsync", this);
  ADOPT_TRACE(trace());

  auto delay = GetAtomicFlag(&FLAGS_TEST_delay_execute_async_ms);
  if (delay != 0 &&
      operation_type() == OperationType::kWrite &&
      operation_->state()->tablet()->tablet_id() != master::kSysCatalogTabletId) {
    LOG(INFO) << "T " << operation_->state()->tablet()->tablet_id()
              << " Debug sleep for: " << MonoDelta(1ms * delay) << "\n" << GetStackTrace();
    std::this_thread::sleep_for(1ms * delay);
  }

  auto s = preparer_->Submit(this);

  if (operation_) {
    operation_->SubmittedToPreparer();
  }

  if (!s.ok()) {
    HandleFailure(s);
  }
}

void OperationDriver::HandleConsensusAppend(
    const yb::OpId& op_id, const yb::OpId& committed_op_id) {
  ADOPT_TRACE(trace());
  if (!StartOperation()) {
    return;
  }
  operation_->state()->LeaderInit(op_id, committed_op_id);
}

void OperationDriver::PrepareAndStartTask() {
  TRACE_EVENT_FLOW_END0("operation", "PrepareAndStartTask", this);
  Status prepare_status = PrepareAndStart();
  if (PREDICT_FALSE(!prepare_status.ok())) {
    HandleFailure(prepare_status);
  }
}

bool OperationDriver::StartOperation() {
  if (operation_) {
    operation_->Start();
    std::lock_guard<simple_spinlock> lock(lock_);
    op_id_copy_.store(yb::OpId::FromPB(operation_->state()->op_id()), boost::memory_order_release);
  }
  if (propagated_safe_time_) {
    mvcc_->SetPropagatedSafeTimeOnFollower(propagated_safe_time_);
  }
  if (!operation_) {
    operation_tracker_->Release(this, nullptr /* applied_op_ids */);
    return false;
  }
  return true;
}

Status OperationDriver::PrepareAndStart() {
  ADOPT_TRACE(trace());
  TRACE_EVENT1("operation", "PrepareAndStart", "operation", this);
  VLOG_WITH_PREFIX(4) << "PrepareAndStart()";
  // Actually prepare and start the operation.
  prepare_physical_hybrid_time_ = GetMonoTimeMicros();
  if (operation_) {
    RETURN_NOT_OK(operation_->Prepare());
  }

  // Only take the lock long enough to take a local copy of the
  // replication state and set our prepare state. This ensures that
  // exactly one of Replicate/Prepare callbacks will trigger the apply
  // phase.
  ReplicationState repl_state_copy;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(prepare_state_, NOT_PREPARED);
    repl_state_copy = replication_state_;
  }

  if (repl_state_copy != NOT_REPLICATING) {
    // We want to call Start() as soon as possible, because the operation already has the
    // hybrid_time assigned.
    if (!StartOperation()) {
      return Status::OK();
    }
  }

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    // No one should have modified prepare_state_ since we've read it under the lock a few lines
    // above, because PrepareAndStart should only run once per operation.
    CHECK_EQ(prepare_state_, NOT_PREPARED);
    // After this update, the ReplicationFinished callback will be able to apply this operation.
    // We can only do this after we've called Start()
    prepare_state_ = PREPARED;

    if (replication_state_ == NOT_REPLICATING) {
      replication_state_ = REPLICATING;
    }
  }

  return Status::OK();
}

OperationDriver::~OperationDriver() {
}

void OperationDriver::ReplicationFailed(const Status& replication_status) {
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    if (replication_state_ == REPLICATION_FAILED) {
      return;
    }
    CHECK_EQ(replication_state_, REPLICATING);
    operation_status_ = replication_status;
    replication_state_ = REPLICATION_FAILED;
  }
  HandleFailure();
}

void OperationDriver::HandleFailure(Status status) {
  ReplicationState repl_state_copy;

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    if (!status.ok()) {
      if (!operation_status_.ok()) {
        LOG(DFATAL) << "Operation already failed with: " << operation_status_ << ", new status: "
                    << status << ", state: " << replication_state_;
      }
      operation_status_ = status;
    } else {
      status = operation_status_;
    }
    repl_state_copy = replication_state_;
  }

  VLOG_WITH_PREFIX(2) << "Failed operation: " << status;
  CHECK(!status.ok());
  ADOPT_TRACE(trace());
  TRACE("HandleFailure($0)", status.ToString());

  switch (repl_state_copy) {
    case NOT_REPLICATING:
    case REPLICATION_FAILED:
    {
      VLOG_WITH_PREFIX(1) << "Operation " << ToString() << " failed prior to "
          "replication success: " << status;
      operation_->Aborted(status);
      operation_tracker_->Release(this, nullptr /* applied_op_ids */);
      return;
    }

    case REPLICATING:
    case REPLICATED:
    {
      LOG_WITH_PREFIX(FATAL) << "Cannot cancel operations that have already replicated"
                             << ": " << status << " operation: " << ToString();
    }
  }
}

void OperationDriver::ReplicationFinished(
    const Status& status, int64_t leader_term, OpIds* applied_op_ids) {
  auto op_id_local = DCHECK_NOTNULL(mutable_state()->consensus_round())->id();
  DCHECK(!status.ok() || op_id_local.IsInitialized());
  op_id_copy_.store(yb::OpId::FromPB(op_id_local), boost::memory_order_release);

  PrepareState prepare_state_copy;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    mutable_state()->mutable_op_id()->CopyFrom(op_id_local);
    CHECK_EQ(replication_state_, REPLICATING);
    if (status.ok()) {
      replication_state_ = REPLICATED;
    } else {
      replication_state_ = REPLICATION_FAILED;
      operation_status_ = status;
    }
    prepare_state_copy = prepare_state_;
  }

  // If we have prepared and replicated, we're ready to move ahead and apply this operation.
  // Note that if we set the state to REPLICATION_FAILED above, ApplyOperation() will actually abort
  // the operation, i.e. ApplyTask() will never be called and the operation will never be applied to
  // the tablet.
  if (prepare_state_copy != PrepareState::PREPARED) {
    LOG(DFATAL) << "Replicating an operation that has not been prepared: " << AsString(this);

    LOG(ERROR) << "Attempting to wait for the operation to be prepared";

    // This case should never happen, but if it happens we are trying to survive.
    for (;;) {
      std::this_thread::sleep_for(1ms);
      PrepareState prepare_state;
      {
        std::lock_guard<simple_spinlock> lock(lock_);
        prepare_state = prepare_state_;
        if (prepare_state == PrepareState::PREPARED) {
          break;
        }
      }
      YB_LOG_EVERY_N_SECS(WARNING, 1)
          << "Waiting for the operation to be prepared, current state: " << prepare_state;
    }
  }

  // We likely need to do cleanup if this fails so for now just
  // CHECK_OK
  CHECK_OK(ApplyOperation(leader_term, applied_op_ids));
}

void OperationDriver::Abort(const Status& status) {
  CHECK(!status.ok());

  ReplicationState repl_state_copy;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    repl_state_copy = replication_state_;
    operation_status_ = status;
  }

  // If the state is not NOT_REPLICATING we abort immediately and the operation
  // will never be replicated.
  // In any other state we just set the operation status, if the operation's
  // Apply hasn't started yet this prevents it from starting, but if it has then
  // the operation runs to completion.
  if (repl_state_copy == NOT_REPLICATING) {
    HandleFailure();
  }
}

Status OperationDriver::ApplyOperation(
    int64_t leader_term, OpIds* applied_op_ids) {
  {
    std::unique_lock<simple_spinlock> lock(lock_);
    DCHECK_EQ(prepare_state_, PREPARED);
    if (operation_status_.ok()) {
      DCHECK_EQ(replication_state_, REPLICATED);
      order_verifier_->CheckApply(op_id_copy_.load(boost::memory_order_relaxed).index,
                                  prepare_physical_hybrid_time_);
    } else {
      DCHECK_EQ(replication_state_, REPLICATION_FAILED);
      DCHECK(!operation_status_.ok());
      lock.unlock();
      HandleFailure();
      return Status::OK();
    }
  }

  TRACE_EVENT_FLOW_BEGIN0("operation", "ApplyTask", this);

  // RocksDB-backed tables require that we apply changes in the same order they appear in the Raft
  // log.
  ApplyTask(leader_term, applied_op_ids);
  return Status::OK();
}

void OperationDriver::ApplyTask(int64_t leader_term, OpIds* applied_op_ids) {
  TRACE_EVENT_FLOW_END0("operation", "ApplyTask", this);
  ADOPT_TRACE(trace());

#ifndef NDEBUG
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    DCHECK_EQ(replication_state_, REPLICATED);
    DCHECK_EQ(prepare_state_, PREPARED);
  }
#endif

  // We need to ref-count ourself, since Commit() may run very quickly
  // and end up calling Finalize() while we're still in this code.
  scoped_refptr<OperationDriver> ref(this);

  {
    auto status = operation_->Replicated(leader_term);
    LOG_IF_WITH_PREFIX(FATAL, !status.ok()) << "Apply failed: " << status;
    operation_tracker_->Release(this, applied_op_ids);
  }
}

std::string OperationDriver::StateString(ReplicationState repl_state,
                                           PrepareState prep_state) {
  string state_str;
  switch (repl_state) {
    case NOT_REPLICATING:
      StrAppend(&state_str, "NR-");  // For Not Replicating
      break;
    case REPLICATING:
      StrAppend(&state_str, "R-");  // For Replicating
      break;
    case REPLICATION_FAILED:
      StrAppend(&state_str, "RF-");  // For Replication Failed
      break;
    case REPLICATED:
      StrAppend(&state_str, "RD-");  // For Replication Done
      break;
    default:
      LOG(DFATAL) << "Unexpected replication state: " << repl_state;
  }
  switch (prep_state) {
    case PREPARED:
      StrAppend(&state_str, "P");
      break;
    case NOT_PREPARED:
      StrAppend(&state_str, "NP");
      break;
    default:
      LOG(DFATAL) << "Unexpected prepare state: " << prep_state;
  }
  return state_str;
}

std::string OperationDriver::LogPrefix() const {
  ReplicationState repl_state_copy;
  PrepareState prep_state_copy;
  std::string ts_string;
  OperationType operation_type;

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    repl_state_copy = replication_state_;
    prep_state_copy = prepare_state_;
    ts_string = state() && state()->has_hybrid_time()
        ? state()->hybrid_time().ToString() : "No hybrid_time";
    operation_type = this->operation_type();
  }

  string state_str = StateString(repl_state_copy, prep_state_copy);
  // We use the tablet and the peer (T, P) to identify ts and tablet and the hybrid_time (Ts) to
  // (help) identify the operation. The state string (S) describes the state of the operation.
  return Format("T $0 P $1 S $2 Ts $3 $4: ",
                // consensus_ is NULL in some unit tests.
                PREDICT_TRUE(consensus_) ? consensus_->tablet_id() : "(unknown)",
                PREDICT_TRUE(consensus_) ? consensus_->peer_uuid() : "(unknown)",
                state_str, ts_string, operation_type);
}

int64_t OperationDriver::SpaceUsed() {
  if (!operation_) {
    return 0;
  }
  auto consensus_round = operation_->state()->consensus_round();
  if (consensus_round) {
    return consensus_round->replicate_msg()->SpaceUsedLong();
  }
  return state()->request()->SpaceUsedLong();
}

}  // namespace tablet
}  // namespace yb
