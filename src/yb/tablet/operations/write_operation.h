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

#ifndef YB_TABLET_OPERATIONS_WRITE_OPERATION_H
#define YB_TABLET_OPERATIONS_WRITE_OPERATION_H

#include <mutex>
#include <string>
#include <vector>

#include "yb/rocksdb/db.h"
#include "yb/rocksdb/write_batch.h"

#include "yb/common/schema.h"

#include "yb/docdb/doc_operation.h"
#include "yb/docdb/intent.h"
#include "yb/docdb/lock_batch.h"
#include "yb/docdb/shared_lock_manager_fwd.h"

#include "yb/gutil/macros.h"

#include "yb/tablet/tablet.pb.h"
#include "yb/tablet/operations/operation.h"

#include "yb/util/locks.h"
#include "yb/util/operation_counter.h"

namespace yb {

namespace tserver {
class WriteRequestPB;
class WriteResponsePB;
}

namespace tablet {
class Tablet;

using docdb::LockBatch;

// A OperationState for a batch of inserts/mutates. This class holds and
// owns most everything related to a transaction, including the Replicate and Commit PB messages
//
// All the transaction related pointers are owned by this class
// and destroyed on Reset() or by the destructor.
//
// IMPORTANT: All the acquired locks will not be released unless the OperationState
// is either destroyed or Reset() or release_locks() is called. Beware of this
// or else there will be lock leaks.
//
// Used when logging to WAL in that we keep track of where inserts/updates
// were applied and add that information to the commit message that is stored
// on the WAL.
//
// NOTE: this class isn't thread safe.
class WriteOperationState : public OperationState {
 public:
  WriteOperationState(Tablet* tablet = nullptr,
                      const tserver::WriteRequestPB *request = nullptr,
                      tserver::WriteResponsePB *response = nullptr,
                      docdb::OperationKind kind = docdb::OperationKind::kWrite);
  virtual ~WriteOperationState();

  // Returns the original client request for this transaction, if there was
  // one.
  const tserver::WriteRequestPB* request() const override {
    return request_;
  }

  tserver::WriteRequestPB* mutable_request() {
    return request_;
  }

  void UpdateRequestFromConsensusRound() override;

  // Returns the prepared response to the client that will be sent when this
  // transaction is completed, if this transaction was started by a client.
  tserver::WriteResponsePB* response() {
    return response_;
  }

  // Commits the Mvcc transaction and releases the component lock. After
  // this method is called all the inserts and mutations will become
  // visible to other transactions.
  //
  // Note: request_ and response_ are set to nullptr after this method returns.
  void Commit();

  // Aborts the mvcc transaction and releases the component lock.
  // Only one of Commit() or Abort() should be called.
  void Abort();

  // The QL write operations that return rowblocks that need to be returned as RPC sidecars
  // after the transaction completes.
  std::vector<std::unique_ptr<docdb::QLWriteOperation>>* ql_write_ops() {
    return &ql_write_ops_;
  }

  // Returns PGSQL write operations.
  // TODO(neil) These ops must report number of rows that was updated, deleted, or inserted.
  std::vector<std::unique_ptr<docdb::PgsqlWriteOperation>>* pgsql_write_ops() {
    return &pgsql_write_ops_;
  }

  // Moves the given lock batch into this object so it can be unlocked when the operation is
  // complete.
  void ReplaceDocDBLocks(LockBatch&& docdb_locks) {
    std::lock_guard<simple_spinlock> l(mutex_);
    docdb_locks_ = std::move(docdb_locks);
  }

  // Releases all the DocDB locks acquired by this transaction.
  void ReleaseDocDbLocks();

  // Resets this OperationState, releasing all locks, destroying all prepared
  // writes, clearing the transaction result _and_ committing the current Mvcc
  // transaction.
  void Reset();

  std::string ToString() const override;

  docdb::OperationKind kind() const {
    return kind_;
  }

  void set_force_txn_path() {
    force_txn_path_ = true;
  }

  bool force_txn_path() const {
    return force_txn_path_;
  }

  void SetTablet(Tablet* tablet) override;

 private:
  // Reset the response, and row_ops_ (which refers to data
  // from the request). Request is owned by WriteOperation using a unique_ptr.
  // A copy is made at initialization, so we don't need to reset it.
  void ResetRpcFields();

  HybridTime WriteHybridTime() const override;

  // pointers to the rpc context, request and response, lifecycle
  // is managed by the rpc subsystem. These pointers maybe nullptr if the
  // transaction was not initiated by an RPC call.
  tserver::WriteRequestPB* request_;

  tserver::WriteResponsePB* response_;

  // The QL write operations that return rowblocks that need to be returned as RPC sidecars
  // after the transaction completes.
  std::vector<std::unique_ptr<docdb::QLWriteOperation>> ql_write_ops_;

  // The PGSQL write operations that return rowblocks that need to be returned as RPC sidecars
  // after the transaction completes.
  std::vector<std::unique_ptr<docdb::PgsqlWriteOperation>> pgsql_write_ops_;

  // Store the ids that have been locked for DocDB transaction. They need to be released on commit
  // or if an error happens.
  LockBatch docdb_locks_;

  docdb::OperationKind kind_;

  // True if we know that this operation is on a transactional table so make sure we go through the
  // transactional codepath.
  bool force_txn_path_ = false;

  DISALLOW_COPY_AND_ASSIGN(WriteOperationState);
};

class WriteOperationContext {
 public:
  // When operation completes, its callback is executed.
  virtual void Submit(std::unique_ptr<Operation> operation, int64_t term) = 0;
  virtual Result<HybridTime> ReportReadRestart() = 0;

  virtual ~WriteOperationContext() {}
};

// Executes a write transaction.
class WriteOperation : public Operation {
 public:
  WriteOperation(std::unique_ptr<WriteOperationState> operation_state, int64_t term,
                 ScopedOperation preparing_token,
                 CoarseTimePoint deadline, WriteOperationContext* context);

  ~WriteOperation() = default;

  WriteOperationState* state() override {
    return down_cast<WriteOperationState*>(Operation::state());
  }

  const WriteOperationState* state() const override {
    return down_cast<const WriteOperationState*>(Operation::state());
  }

  consensus::ReplicateMsgPtr NewReplicateMsg() override;

  // Executes a Prepare for a write transaction
  //
  // Decodes the operations in the request PB and acquires row locks for each of the
  // affected rows.
  CHECKED_STATUS Prepare() override;

  std::string ToString() const override;

  tserver::WriteRequestPB* request() {
    return state()->mutable_request();
  }

  tserver::WriteResponsePB* response() {
    return state()->response();
  }

  ReadHybridTime read_time() {
    return ReadHybridTime::FromReadTimePB(*request());
  }

  HybridTime restart_read_ht() const {
    return restart_read_ht_;
  }

  void SetRestartReadHt(HybridTime value) {
    restart_read_ht_ = value;
  }

  CoarseTimePoint deadline() const {
    return deadline_;
  }

  docdb::DocOperations& doc_ops() {
    return doc_ops_;
  }

  static void StartSynchronization(
      std::unique_ptr<WriteOperation> operation, const Status& status) {
    // We release here, because DoStartSynchronization takes ownership on this.
    operation.release()->DoStartSynchronization(status);
  }

  bool force_txn_path() const {
    return state()->force_txn_path();
  }

  void UseSubmitToken(ScopedRWOperation&& token) {
    submit_token_ = std::move(token);
  }

 private:
  friend class DelayedApplyOperation;

  // Actually starts the Mvcc transaction and assigns a hybrid_time to this transaction.
  void DoStart() override;
  void DoStartSynchronization(const Status& status);

  // Executes an Apply for a write transaction.
  //
  // Actually applies inserts/mutates into the tablet. After these start being
  // applied, the transaction must run to completion as there is currently no
  // means of undoing an update.
  //
  // After completing the inserts/mutates, the row locks and the mvcc transaction
  // can be released, allowing other transactions to update the same rows.
  // However the component lock must not be released until the commit msg, which
  // indicates where each of the inserts/mutates were applied, is persisted to
  // stable storage. Because of this ApplyTask must enqueue a CommitTask before
  // releasing both the row locks and deleting the MvccTransaction as we need to
  // make sure that Commits that touch the same set of rows are persisted in
  // order, for recovery.
  // This, of course, assumes that commits are executed in the same order they
  // are placed in the queue (but not necessarily in the same order of the
  // original requests) which is already a requirement of the consensus
  // algorithm.
  // Commits the mvcc transaction and updates the metrics.
  CHECKED_STATUS DoReplicated(int64_t leader_term, Status* complete_status) override;

  // Aborts the mvcc transaction.
  CHECKED_STATUS DoAborted(const Status& status) override;

  void SubmittedToPreparer() override {
    preparing_token_ = ScopedOperation();
  }

  const int64_t term_;
  ScopedOperation preparing_token_;
  ScopedRWOperation submit_token_;
  const CoarseTimePoint deadline_;
  WriteOperationContext* const context_;

  // this transaction's start time
  MonoTime start_time_;

  HybridTime restart_read_ht_;

  docdb::DocOperations doc_ops_;

  Tablet* tablet() { return state()->tablet(); }

  DISALLOW_COPY_AND_ASSIGN(WriteOperation);
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_OPERATIONS_WRITE_OPERATION_H
