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

#include "yb/tablet/operations/write_operation.h"

#include <algorithm>
#include <vector>

#include <boost/optional.hpp>

#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/numbers.h"
#include "yb/gutil/walltime.h"
#include "yb/rpc/rpc_context.h"
#include "yb/server/hybrid_clock.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/debug-util.h"
#include "yb/util/flag_tags.h"
#include "yb/util/locks.h"
#include "yb/util/trace.h"

DEFINE_test_flag(int32, tablet_inject_latency_on_apply_write_txn_ms, 0,
                 "How much latency to inject when a write operation is applied.");
DEFINE_test_flag(bool, tablet_pause_apply_write_ops, false,
                 "Pause applying of write operations.");
TAG_FLAG(tablet_inject_latency_on_apply_write_txn_ms, runtime);
TAG_FLAG(tablet_pause_apply_write_ops, runtime);

namespace yb {
namespace tablet {

using std::lock_guard;
using std::mutex;
using std::unique_ptr;
using consensus::ReplicateMsg;
using consensus::DriverType;
using consensus::WRITE_OP;
using tserver::TabletServerErrorPB;
using tserver::WriteRequestPB;
using tserver::WriteResponsePB;
using strings::Substitute;

WriteOperation::WriteOperation(
    std::unique_ptr<WriteOperationState> state, int64_t term, MonoTime deadline,
    WriteOperationContext* context)
    : Operation(std::move(state), OperationType::kWrite),
      context_(*context), term_(term), deadline_(deadline), start_time_(MonoTime::Now()) {
}

consensus::ReplicateMsgPtr WriteOperation::NewReplicateMsg() {
  auto result = std::make_shared<ReplicateMsg>();
  result->set_op_type(WRITE_OP);
  result->set_allocated_write_request(state()->mutable_request());
  return result;
}

Status WriteOperation::Prepare() {
  TRACE_EVENT0("txn", "WriteOperation::Prepare");
  return Status::OK();
}

void WriteOperation::DoStart() {
  TRACE("Start()");
  state()->tablet()->StartOperation(state());
}

// FIXME: Since this is called as a void in a thread-pool callback,
// it seems pointless to return a Status!
Status WriteOperation::Apply(int64_t leader_term) {
  TRACE_EVENT0("txn", "WriteOperation::Apply");
  TRACE("APPLY: Starting");

  if (PREDICT_FALSE(
          ANNOTATE_UNPROTECTED_READ(FLAGS_tablet_inject_latency_on_apply_write_txn_ms) > 0)) {
      TRACE("Injecting $0ms of latency due to --tablet_inject_latency_on_apply_write_txn_ms",
            FLAGS_tablet_inject_latency_on_apply_write_txn_ms);
      SleepFor(MonoDelta::FromMilliseconds(FLAGS_tablet_inject_latency_on_apply_write_txn_ms));
  } else {
    TEST_PAUSE_IF_FLAG(tablet_pause_apply_write_ops);
  }

  Tablet* tablet = state()->tablet();

  tablet->ApplyRowOperations(state());

  return Status::OK();
}

void WriteOperation::Finish(OperationResult result) {
  TRACE_EVENT0("txn", "WriteOperation::Finish");
  if (PREDICT_FALSE(result == Operation::ABORTED)) {
    TRACE("FINISH: aborting operation");
    state()->Abort();
    return;
  }

  DCHECK_EQ(result, Operation::COMMITTED);
  // Now that all of the changes have been applied and the commit is durable
  // make the changes visible to readers.
  TRACE("FINISH: making edits visible");
  state()->Commit();

  TabletMetrics* metrics = tablet()->metrics();
  if (metrics && state()->has_completion_callback()) {
    auto op_duration_usec = MonoTime::Now().GetDeltaSince(start_time_).ToMicroseconds();
    metrics->write_op_duration_client_propagated_consistency->Increment(op_duration_usec);
  }
}

string WriteOperation::ToString() const {
  MonoTime now(MonoTime::Now());
  MonoDelta d = now.GetDeltaSince(start_time_);
  WallTime abs_time = WallTime_Now() - d.ToSeconds();
  string abs_time_formatted;
  StringAppendStrftime(&abs_time_formatted, "%Y-%m-%d %H:%M:%S", (time_t)abs_time, true);
  return Substitute("WriteOperation { start_time: $0 state: $1 }",
                    abs_time_formatted, state()->ToString());
}

void WriteOperation::DoStartSynchronization(const Status& status) {
  std::unique_ptr<WriteOperation> self(this);
  // If a restart read is required, then we return this fact to caller and don't perform the write
  // operation.
  if (restart_read_ht_.is_valid()) {
    auto restart_time = state()->response()->mutable_restart_read_time();
    restart_time->set_read_ht(restart_read_ht_.ToUint64());
    auto local_limit = context_.ReportReadRestart();
    restart_time->set_local_limit_ht(local_limit.ToUint64());
    // Global limit is ignored by caller, so we don't set it.
    state()->CompleteWithStatus(Status::OK());
    return;
  }

  if (!status.ok()) {
    state()->CompleteWithStatus(status);
    return;
  }

  context_.Submit(std::move(self), term_);
}

WriteOperationState::WriteOperationState(Tablet* tablet,
                                         const tserver::WriteRequestPB *request,
                                         tserver::WriteResponsePB *response)
    : OperationState(tablet),
      // We need to copy over the request from the RPC layer, as we're modifying it in the tablet
      // layer.
      request_(request ? new WriteRequestPB(*request) : nullptr),
      response_(response) {
}

void WriteOperationState::Abort() {
  if (hybrid_time_.is_valid()) {
    tablet()->mvcc_manager()->Aborted(hybrid_time_);
  }

  ReleaseDocDbLocks();

  // After aborting, we may respond to the RPC and delete the
  // original request, so null them out here.
  ResetRpcFields();
}

void WriteOperationState::UpdateRequestFromConsensusRound() {
  request_ = consensus_round()->replicate_msg()->mutable_write_request();
}

void WriteOperationState::Commit() {
  tablet()->mvcc_manager()->Replicated(hybrid_time_);
  ReleaseDocDbLocks();

  // After committing, we may respond to the RPC and delete the
  // original request, so null them out here.
  ResetRpcFields();
}

void WriteOperationState::ReleaseDocDbLocks() {
  // Free DocDB multi-level locks.
  docdb_locks_.Reset();
}

WriteOperationState::~WriteOperationState() {
  Reset();
  // Ownership is with the Round object, if one exists, else with us.
  if (!consensus_round() && request_ != nullptr) {
    delete request_;
  }
}

void WriteOperationState::Reset() {
  hybrid_time_ = HybridTime::kInvalid;
}

void WriteOperationState::ResetRpcFields() {
  std::lock_guard<simple_spinlock> l(mutex_);
  response_ = nullptr;
}

string WriteOperationState::ToString() const {
  string ts_str;
  if (has_hybrid_time()) {
    ts_str = hybrid_time().ToString();
  } else {
    ts_str = "<unassigned>";
  }

  return Substitute("WriteOperationState $0 [op_id=($1), ts=$2]",
                    this,
                    op_id().ShortDebugString(),
                    ts_str);
}

}  // namespace tablet
}  // namespace yb
