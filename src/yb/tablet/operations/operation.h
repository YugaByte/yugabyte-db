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

#ifndef YB_TABLET_OPERATIONS_OPERATION_H
#define YB_TABLET_OPERATIONS_OPERATION_H

#include <mutex>
#include <string>

#include <boost/optional/optional.hpp>

#include "yb/common/hybrid_time.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus_fwd.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/opid_util.h"
#include "yb/util/auto_release_pool.h"
#include "yb/util/locks.h"
#include "yb/util/operation_counter.h"
#include "yb/util/status.h"
#include "yb/util/memory/arena.h"

namespace yb {

struct OpId;

namespace tablet {

class Tablet;
class OperationCompletionCallback;
class OperationState;

YB_DEFINE_ENUM(
    OperationType,
    (kWrite)(kChangeMetadata)(kUpdateTransaction)(kSnapshot)(kTruncate)(kEmpty)(kHistoryCutoff)
    (kSplit));

// Base class for transactions.  There are different implementations for different types (Write,
// AlterSchema, etc.) OperationDriver implementations use Operations along with Consensus to execute
// and replicate operations in a consensus configuration.
class Operation {
 public:
  enum TraceType {
    NO_TRACE_TXNS = 0,
    TRACE_TXNS = 1
  };

  Operation(std::unique_ptr<OperationState> state,
            OperationType operation_type);

  // Returns the OperationState for this transaction.
  virtual OperationState* state() { return state_.get(); }
  virtual const OperationState* state() const { return state_.get(); }

  // Returns this transaction's type.
  OperationType operation_type() const { return operation_type_; }

  // Builds the ReplicateMsg for this transaction.
  virtual consensus::ReplicateMsgPtr NewReplicateMsg() = 0;

  // Executes the prepare phase of this transaction. The actual actions of this phase depend on the
  // transaction type, but usually are limited to what can be done without actually changing shared
  // data structures (such as the RocksDB memtable) and without side-effects.
  virtual CHECKED_STATUS Prepare() = 0;

  // Actually starts an operation, assigning a hybrid_time to the transaction.  LEADER replicas
  // execute this in or right after Prepare(), while FOLLOWER/LEARNER replicas execute this right
  // before the Apply() phase as the transaction's hybrid_time is only available on the LEADER's
  // commit message.  Once Started(), state might have leaked to other replicas/local log and the
  // transaction can't be cancelled without issuing an abort message.
  //
  // The OpId is provided for debuggability purposes.
  void Start();

  // Applies replicated operation, the actual actions of this phase depend on the
  // operation type, but usually this is the method where data-structures are changed.
  // Also it should notify callback if necessary.
  CHECKED_STATUS Replicated(int64_t leader_term);

  // Abort operation. Release resources and notify callbacks.
  void Aborted(const Status& status);

  // Each implementation should have its own ToString() method.
  virtual std::string ToString() const = 0;

  std::string LogPrefix() const;

  virtual void SubmittedToPreparer() {}

  virtual ~Operation() {}

 private:
  // Actual implementation of Replicated.
  // complete_status could be used to change completion status, i.e. callback will be invoked
  // with this status.
  virtual CHECKED_STATUS DoReplicated(int64_t leader_term, Status* complete_status) = 0;

  // Actual implementation of Aborted, should return status that should be passed to callback.
  virtual CHECKED_STATUS DoAborted(const Status& status) = 0;

  virtual void DoStart() = 0;

  // A private version of this transaction's transaction state so that we can use base
  // OperationState methods on destructors.
  std::unique_ptr<OperationState> state_;
  const OperationType operation_type_;
};

class OperationState {
 public:
  OperationState(const OperationState&) = delete;
  void operator=(const OperationState&) = delete;

  // Returns the request PB associated with this transaction. May be NULL if the transaction's state
  // has been reset.
  virtual const google::protobuf::Message* request() const { return nullptr; }

  // Sets the ConsensusRound for this transaction, if this transaction is being executed through the
  // consensus system.
  void set_consensus_round(const scoped_refptr<consensus::ConsensusRound>& consensus_round);

  // Each subclass should provide a way to update the internal reference to the Message* request, so
  // we can avoid copying the request object all the time.
  virtual void UpdateRequestFromConsensusRound() = 0;

  // Returns the ConsensusRound being used, if this transaction is being executed through the
  // consensus system or NULL if it's not.
  consensus::ConsensusRound* consensus_round() {
    return consensus_round_.get();
  }

  const consensus::ConsensusRound* consensus_round() const {
    return consensus_round_.get();
  }

  std::string ConsensusRoundAsString() const;

  Tablet* tablet() const {
    return tablet_;
  }

  virtual void SetTablet(Tablet* tablet) {
    tablet_ = tablet;
  }

  void set_completion_callback(std::unique_ptr<OperationCompletionCallback> completion_clbk) {
    completion_clbk_ = std::move(completion_clbk);
  }

  // Sets a heap object to be managed by this transaction's AutoReleasePool.
  template<class T>
  T* AddToAutoReleasePool(T* t) {
    return pool_.Add(t);
  }

  // Sets an array heap object to be managed by this transaction's AutoReleasePool.
  template<class T>
  T* AddArrayToAutoReleasePool(T* t) {
    return pool_.AddArray(t);
  }

  // Each implementation should have its own ToString() method.
  virtual std::string ToString() const = 0;

  std::string LogPrefix() const;

  // Sets the hybrid_time for the transaction
  void set_hybrid_time(const HybridTime& hybrid_time);

  // If this operation does not have hybrid time yet, then it will be inited from clock.
  void TrySetHybridTimeFromClock();

  HybridTime hybrid_time() const {
    std::lock_guard<simple_spinlock> l(mutex_);
    DCHECK(hybrid_time_.is_valid());
    return hybrid_time_;
  }

  HybridTime hybrid_time_even_if_unset() const {
    std::lock_guard<simple_spinlock> l(mutex_);
    return hybrid_time_;
  }

  bool has_hybrid_time() const {
    std::lock_guard<simple_spinlock> l(mutex_);
    return hybrid_time_.is_valid();
  }

  // Returns hybrid time that should be used for storing this operation result in RocksDB.
  // For instance it could be different from hybrid_time() for CDC.
  virtual HybridTime WriteHybridTime() const;

  OpIdPB* mutable_op_id() {
    return &op_id_;
  }

  const OpIdPB& op_id() const {
    return op_id_;
  }

  bool has_completion_callback() const {
    return completion_clbk_ != nullptr;
  }

  void CompleteWithStatus(const Status& status) const;
  void SetError(const Status& status, tserver::TabletServerErrorPB::Code code) const;

  // Initialize operation at leader side.
  // op_id - operation id.
  // committed_op_id - current committed operation id.
  void LeaderInit(const OpId& op_id, const OpId& committed_op_id);

  virtual ~OperationState();

 protected:
  explicit OperationState(Tablet* tablet);

  // The tablet peer that is coordinating this transaction.
  Tablet* tablet_;

  // Optional callback to be called once the transaction completes.
  std::unique_ptr<OperationCompletionCallback> completion_clbk_;

  AutoReleasePool pool_;

  // This transaction's hybrid_time. Protected by mutex_.
  HybridTime hybrid_time_;

  // The clock error when hybrid_time_ was read.
  uint64_t hybrid_time_error_ = 0;

  // This OpId stores the canonical "anchor" OpId for this transaction.
  OpIdPB op_id_;

  scoped_refptr<consensus::ConsensusRound> consensus_round_;

  // Lock that protects access to operation state.
  mutable simple_spinlock mutex_;
};

template <class Request, class BaseState = OperationState>
class OperationStateBase : public BaseState {
 public:
  OperationStateBase(Tablet* tablet, const Request* request)
      : BaseState(tablet), request_(request) {}

  explicit OperationStateBase(Tablet* tablet)
      : OperationStateBase(tablet, nullptr) {}

  const Request* request() const override {
    return request_.load(std::memory_order_acquire);
  }

  Request* AllocateRequest() {
    request_holder_ = std::make_unique<Request>();
    request_.store(request_holder_.get(), std::memory_order_release);
    return request_holder_.get();
  }

  tserver::TabletSnapshotOpRequestPB* ReleaseRequest() {
    return request_holder_.release();
  }

  void TakeRequest(Request* request) {
    request_holder_.reset(new Request);
    request_.store(request_holder_.get(), std::memory_order_release);
    request_holder_->Swap(request);
  }

  std::string ToString() const override {
    return Format("{ request: $0 consensus_round: $1 }",
                  request_.load(std::memory_order_acquire), BaseState::ConsensusRoundAsString());
  }

 protected:
  void UseRequest(const Request* request) {
    request_.store(request, std::memory_order_release);
  }

 private:
  std::unique_ptr<Request> request_holder_;
  std::atomic<const Request*> request_;
};

class ExclusiveSchemaOperationStateBase : public OperationState {
 public:
  template <class... Args>
  explicit ExclusiveSchemaOperationStateBase(Args&&... args)
      : OperationState(std::forward<Args>(args)...) {}

  // Release the acquired schema lock.
  void ReleasePermitToken();

  void UsePermitToken(ScopedRWOperationPause&& token) {
    permit_token_ = std::move(token);
  }

 private:
  // Used to pause write operations from being accepted while alter is in progress.
  ScopedRWOperationPause permit_token_;
};

template <class Request>
class ExclusiveSchemaOperationState :
    public OperationStateBase<Request, ExclusiveSchemaOperationStateBase> {
 public:
  template <class... Args>
  explicit ExclusiveSchemaOperationState(Args&&... args)
      : OperationStateBase<Request, ExclusiveSchemaOperationStateBase>(
            std::forward<Args>(args)...) {}

  void Finish() {
    ExclusiveSchemaOperationStateBase::ReleasePermitToken();

    // Make the request NULL since after this operation commits
    // the request may be deleted at any moment.
    OperationStateBase<Request, ExclusiveSchemaOperationStateBase>::UseRequest(nullptr);
  }
};

// A parent class for the callback that gets called when transactions complete.
//
// This must be set in the OperationState if the transaction initiator is to be notified of when a
// transaction completes. The callback belongs to the transaction context and is deleted along with
// it.
//
// NOTE: this is a concrete class so that we can use it as a default implementation which avoids
// callers having to keep checking for NULL.
class OperationCompletionCallback {
 public:

  OperationCompletionCallback();

  // Allows to set an error for this transaction and a mapping to a server level code.  Calling this
  // method does not mean the transaction is completed.
  void set_error(const Status& status, tserver::TabletServerErrorPB::Code code);

  void set_error(const Status& status);

  bool has_error() const;

  const Status& status() const;

  const tserver::TabletServerErrorPB::Code error_code() const;

  // Subclasses should override this.
  virtual void OperationCompleted() = 0;

  void CompleteWithStatus(const Status& status) {
    set_error(status);
    OperationCompleted();
  }

  virtual ~OperationCompletionCallback();

 protected:
  Status status_;
  tserver::TabletServerErrorPB::Code code_;
};

// OperationCompletionCallback implementation that can be waited on.  Helper to make async
// transactions, sync.  This is templated to accept any response PB that has a TabletServerError
// 'error' field and to set the error before performing the latch countdown.  The callback does
// *not* take ownership of either latch or response.
template<class LatchPtr, class ResponsePBPtr>
class LatchOperationCompletionCallback : public OperationCompletionCallback {
 public:
  explicit LatchOperationCompletionCallback(LatchPtr latch,
                                            ResponsePBPtr response)
    : latch_(std::move(latch)),
      response_(std::move(response)) {
  }

  virtual void OperationCompleted() override {
    if (!status_.ok()) {
      StatusToPB(status_, response_->mutable_error()->mutable_status());
    }
    latch_->CountDown();
  }

 private:
  LatchPtr latch_;
  ResponsePBPtr response_;
};

template<class LatchPtr, class ResponsePBPtr>
std::unique_ptr<LatchOperationCompletionCallback<LatchPtr, ResponsePBPtr>>
    MakeLatchOperationCompletionCallback(
        LatchPtr latch, ResponsePBPtr response) {
  return std::make_unique<LatchOperationCompletionCallback<LatchPtr, ResponsePBPtr>>(
      std::move(latch), std::move(response));
}

class SynchronizerOperationCompletionCallback : public OperationCompletionCallback {
 public:
  explicit SynchronizerOperationCompletionCallback(Synchronizer* synchronizer)
    : synchronizer_(DCHECK_NOTNULL(synchronizer)) {}

  void OperationCompleted() override {
    synchronizer_->StatusCB(status());
  }

 private:
  Synchronizer* synchronizer_;
};

class WeakSynchronizerOperationCompletionCallback : public OperationCompletionCallback {
 public:
  explicit WeakSynchronizerOperationCompletionCallback(std::weak_ptr<Synchronizer> synchronizer)
      : synchronizer_(std::move(synchronizer)) {}

  void OperationCompleted() override {
    auto synchronizer = synchronizer_.lock();
    if (synchronizer) {
      synchronizer->StatusCB(status());
    }
  }

 private:
  std::weak_ptr<Synchronizer> synchronizer_;
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_OPERATIONS_OPERATION_H
