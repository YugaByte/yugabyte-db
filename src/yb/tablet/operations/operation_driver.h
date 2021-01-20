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

#ifndef YB_TABLET_OPERATIONS_OPERATION_DRIVER_H
#define YB_TABLET_OPERATIONS_OPERATION_DRIVER_H

#include <string>

#include <boost/atomic.hpp>

#include "yb/consensus/consensus_types.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/walltime.h"
#include "yb/tablet/operations/operation.h"
#include "yb/util/lockfree.h"
#include "yb/util/opid.h"
#include "yb/util/status.h"
#include "yb/util/trace.h"

namespace yb {
class ThreadPool;

namespace log {
class Log;
} // namespace log

namespace tablet {
class MvccManager;
class OperationOrderVerifier;
class OperationTracker;
class OperationDriver;
class Preparer;

// Base class for operation drivers.
//
// OperationDriver classes encapsulate the logic of coordinating the execution of an operation. The
// exact triggering of the methods differs based on whether the operation is being executed on a
// leader or replica, but the general flow is:
//
//  1 - Init() is called on a newly created driver object.  If the driver is instantiated from a
//  REPLICA, then we know that the operation is already "REPLICATING" (and thus we don't need to
//  trigger replication ourself later on).
//
//  2 - ExecuteAsync() is called. This submits the operation driver to the Preparer and returns
//      immediately.
//
//  3 - PrepareAndStartTask() calls Prepare() and Start() on the operation.
//
//      Once successfully prepared, if we have not yet replicated (i.e we are leader), also triggers
//      consensus->Replicate() and changes the replication state to REPLICATING.
//
//      What happens in reality is more complicated, as Preparer tries to batch leader-side
//      operations before submitting them to consensus.
//
//      On the other hand, if we have already successfully replicated (e.g. we are the follower and
//      ConsensusCommitted() has already been called, then we can move on to ApplyOperation().
//
//  4 - The Consensus implementation calls ConsensusCommitted()
//
//      This is triggered by consensus when the commit index moves past our own OpId. On followers,
//      this can happen before Prepare() finishes, and thus we have to check whether we have already
//      done step 3. On leaders, we don't start the consensus round until after Prepare, so this
//      check always passes.
//
//      If Prepare() has already completed, then we trigger ApplyAsync().
//
//  5 - ApplyOperation() calls ApplyTask(), which then calls operation_->Apply().
//
//      When operation_->Apply() is called, changes are made to the in-memory data structures. These
//      changes are not visible to clients yet.
//
//  6 - The driver executes Finalize() which, in turn, makes operations make their changes visible
//      to other operations.  After this step the driver replies to the client if needed and the
//      operation is completed.  In-mem data structures that contain the changes made by the
//      operation can now be made durable.
//
// [1] - see 'Implementation Techniques for Main Memory Database Systems', DeWitt et. al.
//
// This class is thread safe.
class OperationDriver : public RefCountedThreadSafe<OperationDriver>,
                        public consensus::ConsensusAppendCallback,
                        public MPSCQueueEntry<OperationDriver> {

 public:
  // Construct OperationDriver. OperationDriver does not take ownership
  // of any of the objects pointed to in the constructor's arguments.
  OperationDriver(OperationTracker* operation_tracker,
                  consensus::Consensus* consensus,
                  log::Log* log,
                  Preparer* preparer,
                  OperationOrderVerifier* order_verifier,
                  TableType table_type_);

  // Perform any non-constructor initialization. Sets the operation
  // that will be executed.
  // if term == kUnknownTerm then we launch this operation as replica, otherwise
  // we are leader and operation should be bound to this term.
  CHECKED_STATUS Init(std::unique_ptr<Operation>* operation, int64_t term);

  // Returns the OpId of the operation being executed or an uninitialized
  // OpId if none has been assigned. Returns a copy and thus should not
  // be used in tight loops.
  yb::OpId GetOpId();

  // Submits the operation for execution.
  // The returned status acknowledges any error on the submission process.
  // The operation will be replied to asynchronously.
  void ExecuteAsync();

  // Aborts the operation, if possible. Since operations are executed in
  // multiple stages by multiple executors it might not be possible to stop
  // the operation immediately, but this will make sure it is aborted
  // at the next synchronization point.
  void Abort(const Status& status);

  // Callback from Consensus when replication is complete, and thus the operation
  // is considered "committed" from the consensus perspective (ie it will be
  // applied on every node, and not ever truncated from the state machine history).
  // If status is anything different from OK() we don't proceed with the apply.
  //
  // see comment in the interface for an important TODO.
  void ReplicationFinished(
      const Status& status, int64_t leader_term, OpIds* applied_op_ids);

  std::string ToString() const;

  std::string ToStringUnlocked() const;

  std::string LogPrefix() const;

  // Returns the type of the operation being executed by this driver.
  OperationType operation_type() const;

  // Returns the state of the operation being executed by this driver.
  const OperationState* state() const;

  const MonoTime& start_time() const { return start_time_; }

  Trace* trace() { return trace_.get(); }

  void HandleConsensusAppend(const yb::OpId& op_id, const yb::OpId& committed_op_id) override;

  bool is_leader_side() {
    // TODO: switch state to an atomic.
    std::lock_guard<simple_spinlock> lock(lock_);
    return replication_state_ == ReplicationState::NOT_REPLICATING;
  }

  // Actually prepare and start. In case of leader-side operations, this stops short of calling
  // Consensus::Replicate, which is the responsibility of the caller. This is being done so that
  // we can append multiple rounds to the consensus queue together.
  CHECKED_STATUS PrepareAndStart();

  // The task used to be submitted to the prepare threadpool to prepare and start the operation.
  // If PrepareAndStart() fails, calls HandleFailure. Since 07/07/2017 this is being used for
  // non-leader-side operations from Preparer, and for leader-side operations the handling
  // is a bit more complicated due to batching.
  void PrepareAndStartTask();

  // This should be called in case of a failure to submit the operation for replication.
  void ReplicationFailed(const Status& replication_status);

  // Handle a failure in any of the stages of the operation.
  // In some cases, this will end the operation and call its callback.
  // In others, where we can't recover, this will FATAL.
  void HandleFailure(Status status = Status::OK());

  consensus::Consensus* consensus() { return consensus_; }

  consensus::ConsensusRound* consensus_round() {
    return mutable_state()->consensus_round();
  }

  void SetPropagatedSafeTime(HybridTime safe_time, MvccManager* mvcc) {
    propagated_safe_time_ = safe_time;
    mvcc_ = mvcc;
  }

  int64_t SpaceUsed();

 private:
  friend class RefCountedThreadSafe<OperationDriver>;
  enum ReplicationState {
    // The operation has not yet been sent to consensus for replication
    NOT_REPLICATING,

    // Replication has been triggered (either because we are the leader and triggered it,
    // or because we are a follower and we started this operation in response to a
    // leader's call)
    REPLICATING,

    // Replication has failed, and we are certain that no other may have received the
    // operation (ie we failed before even sending the request off of our node).
    REPLICATION_FAILED,

    // Replication has succeeded.
    REPLICATED
  };

  enum PrepareState {
    NOT_PREPARED,
    PREPARED
  };

  ~OperationDriver();

  // Starts operation, returns false is we should NOT continue processing the operation.
  bool StartOperation();

  // Performs status checks and calls ApplyTask.
  CHECKED_STATUS ApplyOperation(int64_t leader_term, OpIds* applied_op_ids);

  // Calls Operation::Apply() followed by Consensus::Commit() with the
  // results from the Apply().
  void ApplyTask(int64_t leader_term, OpIds* applied_op_ids);

  // Returns the mutable state of the operation being executed by
  // this driver.
  OperationState* mutable_state();

  // Return a short string indicating where the operation currently is in the
  // state machine.
  static std::string StateString(ReplicationState repl_state,
                                 PrepareState prep_state);

  OperationTracker* const operation_tracker_;
  consensus::Consensus* const consensus_;
  log::Log* const log_;
  Preparer* const preparer_;
  OperationOrderVerifier* const order_verifier_;

  Status operation_status_;

  // Lock that synchronizes access to the operation's state.
  mutable simple_spinlock lock_;

  // A copy of the operation's OpId, set when the operation first
  // receives one from Consensus and uninitialized until then.
  // TODO(todd): we have three separate copies of this now -- in OperationState,
  // CommitMsg, and here... we should be able to consolidate!
  boost::atomic<yb::OpId> op_id_copy_{yb::OpId::Invalid()};

  // The operation to be executed by this driver.
  std::unique_ptr<Operation> operation_;

  // Trace object for tracing any operations started by this driver.
  scoped_refptr<Trace> trace_;

  const MonoTime start_time_;

  ReplicationState replication_state_;
  PrepareState prepare_state_;

  // The system monotonic time when the operation was prepared.
  // This is used for debugging only, not any actual operation ordering.
  MicrosecondsInt64 prepare_physical_hybrid_time_ = 0;

  TableType table_type_;

  MvccManager* mvcc_ = nullptr;
  HybridTime propagated_safe_time_;

  DISALLOW_COPY_AND_ASSIGN(OperationDriver);
};

}  // namespace tablet
}  // namespace yb

#endif // YB_TABLET_OPERATIONS_OPERATION_DRIVER_H
