//
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
//

#include "yb/tablet/transaction_coordinator.h"

#include <condition_variable>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/tag.hpp>

#include <boost/uuid/uuid_io.hpp>

#include "yb/client/client.h"
#include "yb/client/transaction_cleanup.h"
#include "yb/client/transaction_rpc.h"

#include "yb/common/entity_ids.h"
#include "yb/common/transaction.h"
#include "yb/common/transaction_error.h"
#include "yb/common/pgsql_error.h"

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus_util.h"
#include "yb/consensus/opid_util.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/poller.h"
#include "yb/rpc/rpc.h"

#include "yb/server/clock.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/operations/update_txn_operation.h"

#include "yb/tserver/service_util.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/tserver/tserver_service.pb.h"

#include "yb/util/enums.h"
#include "yb/util/flag_tags.h"
#include "yb/util/metrics.h"
#include "yb/util/random_util.h"
#include "yb/util/result.h"
#include "yb/util/scope_exit.h"
#include "yb/util/tsan_util.h"
#include "yb/util/yb_pg_errcodes.h"

DECLARE_uint64(transaction_heartbeat_usec);
DEFINE_double(transaction_max_missed_heartbeat_periods, 10.0 * yb::kTimeMultiplier,
              "Maximum heartbeat periods that a pending transaction can miss before the "
              "transaction coordinator expires the transaction. The total expiration time in "
              "microseconds is transaction_heartbeat_usec times "
              "transaction_max_missed_heartbeat_periods. The value passed to this flag may be "
              "fractional.");
DEFINE_uint64(transaction_check_interval_usec, 500000, "Transaction check interval in usec.");
DEFINE_uint64(transaction_resend_applying_interval_usec, 5000000,
              "Transaction resend applying interval in usec.");

DEFINE_int64(avoid_abort_after_sealing_ms, 20,
             "If transaction was only sealed, we will try to abort it not earlier than this "
                 "period in milliseconds.");

DEFINE_test_flag(uint64, inject_txn_get_status_delay_ms, 0,
                 "Inject specified delay to transaction get status requests.");

using namespace std::literals;
using namespace std::placeholders;

namespace yb {
namespace tablet {

std::chrono::microseconds GetTransactionTimeout() {
  const double timeout = GetAtomicFlag(&FLAGS_transaction_max_missed_heartbeat_periods) *
                         GetAtomicFlag(&FLAGS_transaction_heartbeat_usec);
  return timeout >= std::chrono::microseconds::max().count()
      ? std::chrono::microseconds::max()
      : std::chrono::microseconds(static_cast<int64_t>(timeout));
}

namespace {

struct NotifyApplyingData {
  TabletId tablet;
  TransactionId transaction;
  HybridTime commit_time;
  bool sealed;

  std::string ToString() const {
    return Format("{ tablet: $0 transaction: $1 commit_time: $2 sealed: $3}",
                  tablet, transaction, commit_time, sealed);
  }
};

struct ExpectedTabletBatches {
  TabletId tablet;
  size_t batches;

  std::string ToString() const {
    return Format("{ tablet: $0 batches: $1 }", tablet, batches);
  }
};

// Context for transaction state. I.e. access to external facilities required by
// transaction state to do its job.
class TransactionStateContext {
 public:
  virtual TransactionCoordinatorContext& coordinator_context() = 0;

  virtual void NotifyApplying(NotifyApplyingData data) = 0;

  virtual Counter& expired_metric() = 0;

  // Submits update transaction to the RAFT log. Returns false if was not able to submit.
  virtual MUST_USE_RESULT bool SubmitUpdateTransaction(
      std::unique_ptr<UpdateTxnOperationState> state) = 0;

  virtual void CompleteWithStatus(
      std::unique_ptr<UpdateTxnOperationState> request, Status status) = 0;

  virtual void CompleteWithStatus(UpdateTxnOperationState* request, Status status) = 0;

  virtual bool leader() const = 0;

 protected:
  ~TransactionStateContext() {}
};

std::string BuildLogPrefix(const std::string& parent_log_prefix, const TransactionId& id) {
  auto id_string = id.ToString();
  return parent_log_prefix.substr(0, parent_log_prefix.length() - 2) + " ID " + id_string + ": ";
}

// TransactionState keeps state of single transaction.
// User of this class should guarantee that it does NOT invoke methods concurrently.
class TransactionState {
 public:
  explicit TransactionState(TransactionStateContext* context,
                            const TransactionId& id,
                            HybridTime last_touch,
                            const std::string& parent_log_prefix)
      : context_(*context),
        id_(id),
        log_prefix_(BuildLogPrefix(parent_log_prefix, id)),
        last_touch_(last_touch) {
  }

  ~TransactionState() {
    DCHECK(abort_waiters_.empty());
    DCHECK(request_queue_.empty());
    DCHECK(replicating_ == nullptr) << Format("Replicating: $0", static_cast<void*>(replicating_));
  }

  // Id of transaction.
  const TransactionId& id() const {
    return id_;
  }

  // Time when we last heard from transaction. I.e. hybrid time of replicated raft log entry
  // that updates status of this transaction.
  HybridTime last_touch() const {
    return last_touch_;
  }

  // Status of transaction.
  TransactionStatus status() const {
    return status_;
  }

  // RAFT index of first RAFT log entry required by this transaction.
  int64_t first_entry_raft_index() const {
    return first_entry_raft_index_;
  }

  std::string ToString() const {
    return Format("{ id: $0 last_touch: $1 status: $2 unnotified_tablets: $3 replicating: $4 "
                      " request_queue: $5 first_entry_raft_index: $6 }",
                  id_, last_touch_, TransactionStatus_Name(status_),
                  involved_tablets_, replicating_, request_queue_, first_entry_raft_index_);
  }

  // Whether this transaction expired at specified time.
  bool ExpiredAt(HybridTime now) const {
    if (ShouldBeCommitted() || ShouldBeInStatus(TransactionStatus::SEALED)) {
      return false;
    }
    const int64_t passed = now.GetPhysicalValueMicros() - last_touch_.GetPhysicalValueMicros();
    if (std::chrono::microseconds(passed) > GetTransactionTimeout()) {
      context_.expired_metric().Increment();
      return true;
    }
    return false;
  }

  // Whether this transaction has completed.
  bool Completed() const {
    return status_ == TransactionStatus::ABORTED ||
           status_ == TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS;
  }

  // Applies new state to transaction.
  CHECKED_STATUS ProcessReplicated(const TransactionCoordinator::ReplicatedData& data) {
    VLOG_WITH_PREFIX(4)
        << Format("ProcessReplicated: $0, replicating: $1", data, replicating_);

    if (replicating_ != nullptr) {
      auto replicating_op_id = replicating_->consensus_round()->id();
      if (replicating_op_id.IsInitialized()) {
        if (!consensus::OpIdEquals(replicating_->consensus_round()->id(), data.op_id)) {
          LOG_WITH_PREFIX(DFATAL)
              << "Replicated unexpected operation, replicating: " << AsString(replicating_)
              << ", replicated: " << AsString(data);
        }
      } else if (data.leader_term != OpId::kUnknownTerm) {
        LOG_WITH_PREFIX(DFATAL)
            << "Leader replicated operation without op id, replicating: " << AsString(replicating_)
            << ", replicated: " << AsString(data);
      } else {
        LOG_WITH_PREFIX(INFO) << "Cancel replicating without id: " << AsString(replicating_)
                              << ", because " << AsString(data) << " was replicated";
      }
      replicating_ = nullptr;
    }

    auto status = DoProcessReplicated(data);

    if (data.leader_term == OpId::kUnknownTerm) {
      ClearRequests(STATUS(IllegalState, "Leader changed"));
    } else {
      switch(status_) {
        case TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS:
          ClearRequests(STATUS(AlreadyPresent, "Transaction committed"));
          break;
        case TransactionStatus::ABORTED:
          ClearRequests(
              STATUS(Expired, "Transaction aborted",
                     TransactionError(TransactionErrorCode::kAborted)));
          break;
        case TransactionStatus::CREATED: FALLTHROUGH_INTENDED;
        case TransactionStatus::PENDING: FALLTHROUGH_INTENDED;
        case TransactionStatus::SEALED: FALLTHROUGH_INTENDED;
        case TransactionStatus::COMMITTED: FALLTHROUGH_INTENDED;
        case TransactionStatus::APPLYING: FALLTHROUGH_INTENDED;
        case TransactionStatus::APPLIED_IN_ONE_OF_INVOLVED_TABLETS: FALLTHROUGH_INTENDED;
        case TransactionStatus::CLEANUP:
          ProcessQueue();
          break;
      }
    }

    return status;
  }

  void ProcessAborted(const TransactionCoordinator::AbortedData& data) {
    VLOG_WITH_PREFIX(4) << Format("ProcessAborted: $0, replicating: $1", data.state, replicating_);

    DCHECK(replicating_ == nullptr || !replicating_->op_id().IsInitialized() ||
           consensus::OpIdEquals(replicating_->op_id(), data.op_id));
    replicating_ = nullptr;

    // We are not leader, so could abort all queued requests.
    ClearRequests(STATUS(Aborted, "Replication failed"));
  }

  // Clear requests of this transaction.
  void ClearRequests(const Status& status) {
    VLOG_WITH_PREFIX(4) << Format("ClearRequests: $0, replicating: $1", status, replicating_);
    if (replicating_ != nullptr) {
      context_.CompleteWithStatus(replicating_, status);
      replicating_ = nullptr;
    }

    for (auto& entry : request_queue_) {
      context_.CompleteWithStatus(std::move(entry), status);
    }
    request_queue_.clear();

    NotifyAbortWaiters(status);
  }

  // Used only during transaction sealing.
  void ReplicatedAllBatchesAt(const TabletId& tablet, HybridTime last_time) {
    auto it = involved_tablets_.find(tablet);
    // We could be notified several times, so avoid double handling.
    if (it == involved_tablets_.end() || it->second.all_batches_replicated) {
      return;
    }

    // If transaction was sealed, then its commit time is max of seal record time and intent
    // replication times from all participating tablets.
    commit_time_ = std::max(commit_time_, last_time);
    --tablets_with_not_replicated_batches_;
    it->second.all_batches_replicated = true;

    if (tablets_with_not_replicated_batches_ == 0) {
      StartApply();
    }
  }

  Result<TransactionStatusResult> GetStatus(
      std::vector<ExpectedTabletBatches>* expected_tablet_batches) const {
    switch (status_) {
      case TransactionStatus::COMMITTED: FALLTHROUGH_INTENDED;
      case TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS:
        return TransactionStatusResult{TransactionStatus::COMMITTED, commit_time_};
      case TransactionStatus::SEALED:
        if (tablets_with_not_replicated_batches_ == 0) {
          return TransactionStatusResult{TransactionStatus::COMMITTED, commit_time_};
        }
        FillExpectedTabletBatches(expected_tablet_batches);
        return TransactionStatusResult{TransactionStatus::SEALED, commit_time_};
      case TransactionStatus::ABORTED:
        return TransactionStatusResult{TransactionStatus::ABORTED, HybridTime::kMax};
      case TransactionStatus::PENDING: {
        HybridTime status_ht = context_.coordinator_context().clock().Now();
        if (replicating_) {
          auto replicating_status = replicating_->request()->status();
          if (replicating_status == TransactionStatus::COMMITTED ||
              replicating_status == TransactionStatus::ABORTED) {
            auto replicating_ht = replicating_->hybrid_time_even_if_unset();
            if (replicating_ht.is_valid()) {
              status_ht = replicating_ht;
            }
          }
        }
        status_ht = std::min(status_ht, context_.coordinator_context().HtLeaseExpiration());
        return TransactionStatusResult{TransactionStatus::PENDING, status_ht.Decremented()};
      }
      case TransactionStatus::CREATED: FALLTHROUGH_INTENDED;
      case TransactionStatus::APPLYING: FALLTHROUGH_INTENDED;
      case TransactionStatus::APPLIED_IN_ONE_OF_INVOLVED_TABLETS: FALLTHROUGH_INTENDED;
      case TransactionStatus::CLEANUP:
        return STATUS_FORMAT(Corruption, "Transaction has unexpected status: $0",
                             TransactionStatus_Name(status_));
    }
    FATAL_INVALID_ENUM_VALUE(TransactionStatus, status_);
  }

  void Aborted() {
    status_ = TransactionStatus::ABORTED;
    NotifyAbortWaiters(TransactionStatusResult::Aborted());
  }

  TransactionStatusResult Abort(TransactionAbortCallback* callback) {
    if (status_ == TransactionStatus::COMMITTED ||
        status_ == TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS) {
      return TransactionStatusResult(TransactionStatus::COMMITTED, commit_time_);
    } else if (ShouldBeCommitted()) {
      return TransactionStatusResult(TransactionStatus::COMMITTED, HybridTime::kMax);
    } else if (status_ == TransactionStatus::ABORTED) {
      return TransactionStatusResult::Aborted();
    } else {
      VLOG_WITH_PREFIX(1) << "External abort request";
      CHECK_EQ(TransactionStatus::PENDING, status_);
      abort_waiters_.emplace_back(std::move(*callback));
      Abort();
      return TransactionStatusResult(TransactionStatus::PENDING, HybridTime::kMax);
    }
  }

  void Handle(std::unique_ptr<tablet::UpdateTxnOperationState> request) {
    auto& state = *request->request();
    VLOG_WITH_PREFIX(1) << "Handle: " << state.ShortDebugString();
    if (state.status() == TransactionStatus::APPLIED_IN_ONE_OF_INVOLVED_TABLETS) {
      auto status = AppliedInOneOfInvolvedTablets(state);
      context_.CompleteWithStatus(std::move(request), status);
      return;
    }
    if (replicating_) {
      request_queue_.push_back(std::move(request));
      return;
    }
    DoHandle(std::move(request));
  }

  // Aborts this transaction.
  void Abort() {
    if (ShouldBeCommitted()) {
      LOG_WITH_PREFIX(DFATAL) << "Transaction abort in wrong state: " << status_;
      return;
    }
    if (ShouldBeAborted()) {
      return;
    }
    if (status_ != TransactionStatus::PENDING) {
      LOG_WITH_PREFIX(DFATAL) << "Unexpected status during abort: "
                              << TransactionStatus_Name(status_);
      return;
    }
    SubmitUpdateStatus(TransactionStatus::ABORTED);
  }

  // Returns logs prefix for this transaction.
  const std::string& LogPrefix() {
    return log_prefix_;
  }

  // now_physical is just optimization to avoid querying the current time multiple times.
  void Poll(bool leader, MonoTime now_physical) {
    if (status_ != TransactionStatus::COMMITTED &&
        (status_ != TransactionStatus::SEALED || tablets_with_not_replicated_batches_ != 0)) {
      return;
    }
    if (tablets_with_not_applied_intents_ == 0) {
      if (leader && !ShouldBeInStatus(TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS)) {
        SubmitUpdateStatus(TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS);
      }
    } else if (now_physical >= resend_applying_time_) {
      if (leader) {
        for (auto& tablet : involved_tablets_) {
          if (!tablet.second.all_intents_applied) {
            context_.NotifyApplying({
                .tablet = tablet.first,
                .transaction = id_,
                .commit_time = commit_time_,
                .sealed = status_ == TransactionStatus::SEALED });
          }
        }
      }
      resend_applying_time_ = now_physical +
          std::chrono::microseconds(FLAGS_transaction_resend_applying_interval_usec);
    }
  }

  CHECKED_STATUS AppliedInOneOfInvolvedTablets(const tserver::TransactionStatePB& state) {
    if (status_ != TransactionStatus::COMMITTED && status_ != TransactionStatus::SEALED) {
      // We could ignore this request, because it will be re-send if required.
      LOG_WITH_PREFIX(DFATAL)
          << "AppliedInOneOfInvolvedTablets in wrong state: " << TransactionStatus_Name(status_)
          << ", request: " << state.ShortDebugString();
      return Status::OK();
    }

    if (state.tablets_size() != 1) {
      return STATUS_FORMAT(
          InvalidArgument, "Expected exactly one tablet in $0: $1", __func__, state);
    }

    auto it = involved_tablets_.find(state.tablets(0));
    if (it == involved_tablets_.end()) {
      LOG_WITH_PREFIX(DFATAL) << "Applied in unknown tablet: " << state.tablets(0);
      return Status::OK();
    }
    if (!it->second.all_intents_applied) {
      --tablets_with_not_applied_intents_;
      it->second.all_intents_applied = true;
      VLOG_WITH_PREFIX(4) << "Applied to " << state.tablets(0) << ", left not applied: "
                          << tablets_with_not_applied_intents_;
      if (tablets_with_not_applied_intents_ == 0) {
        SubmitUpdateStatus(TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS);
      }
    }
    return Status::OK();
  }

 private:
  // Checks whether we in specified status or going to be in this status when replication is
  // finished.
  bool ShouldBeInStatus(TransactionStatus status) const {
    if (status_ == status) {
      return true;
    }
    if (replicating_) {
      if (replicating_->request()->status() == status) {
        return true;
      }

      for (const auto& entry : request_queue_) {
        if (entry->request()->status() == status) {
          return true;
        }
      }
    }

    return false;
  }

  bool ShouldBeCommitted() const {
    return ShouldBeInStatus(TransactionStatus::COMMITTED) ||
           ShouldBeInStatus(TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS);
  }

  bool ShouldBeAborted() const {
    return ShouldBeInStatus(TransactionStatus::ABORTED);
  }

  // Process operation that was replicated in RAFT.
  CHECKED_STATUS DoProcessReplicated(const TransactionCoordinator::ReplicatedData& data) {
    switch (data.state.status()) {
      case TransactionStatus::ABORTED:
        return AbortedReplicationFinished(data);
      case TransactionStatus::SEALED:
        return SealedReplicationFinished(data);
      case TransactionStatus::COMMITTED:
        return CommittedReplicationFinished(data);
      case TransactionStatus::CREATED: FALLTHROUGH_INTENDED;
      case TransactionStatus::PENDING:
        return PendingReplicationFinished(data);
      case TransactionStatus::APPLYING:
        // APPLYING is handled separately, because it is received for transactions not managed by
        // this tablet as a transaction status tablet, but tablets that are involved in the data
        // path (receive write intents) for this transactions
        FATAL_INVALID_ENUM_VALUE(TransactionStatus, data.state.status());
      case TransactionStatus::APPLIED_IN_ONE_OF_INVOLVED_TABLETS:
        // APPLIED_IN_ONE_OF_INVOLVED_TABLETS handled w/o use of RAFT log
        FATAL_INVALID_ENUM_VALUE(TransactionStatus, data.state.status());
      case TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS:
        return AppliedInAllInvolvedTabletsReplicationFinished(data);
      case TransactionStatus::CLEANUP:
        // CLEANUP is handled separately, because it is received for transactions not managed by
        // this tablet as a transaction status tablet, but tablets that are involved in the data
        // path (receive write intents) for this transactions
        FATAL_INVALID_ENUM_VALUE(TransactionStatus, data.state.status());
    }
    FATAL_INVALID_ENUM_VALUE(TransactionStatus, data.state.status());
  }

  void DoHandle(std::unique_ptr<tablet::UpdateTxnOperationState> request) {
    const auto& state = *request->request();

    Status status;
    if (state.status() == TransactionStatus::COMMITTED) {
      status = HandleCommit();
    } else if (state.status() == TransactionStatus::PENDING) {
      if (status_ != TransactionStatus::PENDING) {
        status = STATUS_FORMAT(IllegalState,
            "Transaction in wrong state during heartbeat: $0",
            TransactionStatus_Name(status_));
      } else {
        status = Status::OK();
      }
    } else {
      status = Status::OK();
    }

    if (!status.ok()) {
      context_.CompleteWithStatus(std::move(request), std::move(status));
      return;
    }

    VLOG_WITH_PREFIX(4) << Format("DoHandle, replicating = $0", replicating_);
    replicating_ = request.get();
    auto submitted = context_.SubmitUpdateTransaction(std::move(request));
    CHECK(submitted);
  }

  CHECKED_STATUS HandleCommit() {
    auto hybrid_time = context_.coordinator_context().clock().Now();
    if (ExpiredAt(hybrid_time)) {
      auto status = STATUS(Expired, "Commit of expired transaction");
      VLOG_WITH_PREFIX(4) << status;
      Abort();
      return status;
    }
    if (status_ != TransactionStatus::PENDING) {
      return STATUS_FORMAT(IllegalState,
                           "Transaction in wrong state when starting to commit: $0",
                           TransactionStatus_Name(status_));
    }

    return Status::OK();
  }

  void SubmitUpdateStatus(TransactionStatus status) {
    VLOG_WITH_PREFIX(4) << "SubmitUpdateStatus(" << TransactionStatus_Name(status) << ")";

    tserver::TransactionStatePB state;
    state.set_transaction_id(id_.data(), id_.size());
    state.set_status(status);

    auto request = context_.coordinator_context().CreateUpdateTransactionState(&state);
    if (replicating_) {
      request_queue_.push_back(std::move(request));
    } else {
      replicating_ = request.get();
      VLOG_WITH_PREFIX(4) << Format("SubmitUpdateStatus, replicating = $0", replicating_);
      if (!context_.SubmitUpdateTransaction(std::move(request))) {
        // Was not able to submit update transaction, for instance we are not leader.
        // So we are not replicating.
        replicating_ = nullptr;
      }
    }
  }

  void ProcessQueue() {
    while (!replicating_ && !request_queue_.empty()) {
      auto request = std::move(request_queue_.front());
      request_queue_.pop_front();
      DoHandle(std::move(request));
    }
  }

  CHECKED_STATUS AbortedReplicationFinished(const TransactionCoordinator::ReplicatedData& data) {
    if (status_ != TransactionStatus::ABORTED &&
        status_ != TransactionStatus::PENDING) {
      LOG_WITH_PREFIX(DFATAL) << "Invalid status of aborted transaction: "
                              << TransactionStatus_Name(status_);
    }

    status_ = TransactionStatus::ABORTED;
    first_entry_raft_index_ = data.op_id.index();
    NotifyAbortWaiters(TransactionStatusResult::Aborted());
    return Status::OK();
  }

  CHECKED_STATUS SealedReplicationFinished(
      const TransactionCoordinator::ReplicatedData& data) {
    if (status_ != TransactionStatus::PENDING) {
      auto status = STATUS_FORMAT(
          IllegalState,
          "Unexpected status during CommittedReplicationFinished: $0",
          TransactionStatus_Name(status_));
      LOG_WITH_PREFIX(DFATAL) << status;
      return status;
    }

    last_touch_ = data.hybrid_time;
    commit_time_ = data.hybrid_time;
    // TODO(dtxn) Not yet implemented
    next_abort_after_sealing_ = CoarseMonoClock::now() + FLAGS_avoid_abort_after_sealing_ms * 1ms;
    VLOG_WITH_PREFIX(4) << "Seal time: " << commit_time_;
    status_ = TransactionStatus::SEALED;

    involved_tablets_.reserve(data.state.tablets().size());
    for (int idx = 0; idx != data.state.tablets().size(); ++idx) {
      auto tablet_batches = data.state.tablet_batches(idx);
      LOG_IF_WITH_PREFIX(DFATAL, tablet_batches == 0)
          << "Tablet without batches: " << data.state.ShortDebugString();
      ++tablets_with_not_replicated_batches_;
      InvolvedTabletState state = {
        .required_replicated_batches = static_cast<size_t>(tablet_batches),
        .all_batches_replicated = false,
        .all_intents_applied = false
      };
      involved_tablets_.emplace(data.state.tablets(idx), state);
    }

    first_entry_raft_index_ = data.op_id.index();
    return Status::OK();
  }

  CHECKED_STATUS CommittedReplicationFinished(const TransactionCoordinator::ReplicatedData& data) {
    if (status_ != TransactionStatus::PENDING) {
      auto status = STATUS_FORMAT(
          IllegalState,
          "Unexpected status during CommittedReplicationFinished: $0",
          TransactionStatus_Name(status_));
      LOG_WITH_PREFIX(DFATAL) << status;
      return status;
    }

    last_touch_ = data.hybrid_time;
    commit_time_ = data.hybrid_time;
    first_entry_raft_index_ = data.op_id.index();
    involved_tablets_.reserve(data.state.tablets().size());
    for (const auto& tablet : data.state.tablets()) {
      InvolvedTabletState state = {
        .required_replicated_batches = 0,
        .all_batches_replicated = true,
        .all_intents_applied = false
      };
      involved_tablets_.emplace(tablet, state);
    }

    status_ = TransactionStatus::COMMITTED;
    StartApply();
    return Status::OK();
  }

  CHECKED_STATUS AppliedInAllInvolvedTabletsReplicationFinished(
      const TransactionCoordinator::ReplicatedData& data) {
    if (status_ != TransactionStatus::COMMITTED && status_ != TransactionStatus::SEALED) {
      // That could happen in old version, because we could drop all entries before
      // APPLIED_IN_ALL_INVOLVED_TABLETS.
      LOG_WITH_PREFIX(DFATAL)
          << "AppliedInAllInvolvedTabletsReplicationFinished in wrong state: "
          << TransactionStatus_Name(status_) << ", request: " << data.state.ShortDebugString();
      CHECK_EQ(status_, TransactionStatus::PENDING);
    }
    VLOG_WITH_PREFIX(4) << __func__ << ", status: " << TransactionStatus_Name(status_)
                        << ", leader: " << context_.leader();
    last_touch_ = data.hybrid_time;
    status_ = TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS;
    return Status::OK();
  }

  // Used for PENDING and CREATED records. Because when we apply replicated operations they have
  // the same meaning.
  CHECKED_STATUS PendingReplicationFinished(const TransactionCoordinator::ReplicatedData& data) {
    if (context_.leader() && ExpiredAt(data.hybrid_time)) {
      VLOG_WITH_PREFIX(4) << "Expired during replication of PENDING or CREATED operations.";
      Abort();
      return Status::OK();
    }
    if (status_ != TransactionStatus::PENDING) {
      LOG_WITH_PREFIX(DFATAL) << "Bad status during " << __func__ << "(" << data.ToString()
                              << "): " << ToString();
      return Status::OK();
    }
    last_touch_ = data.hybrid_time;
    first_entry_raft_index_ = data.op_id.index();
    return Status::OK();
  }

  void NotifyAbortWaiters(const Result<TransactionStatusResult>& result) {
    for (auto& waiter : abort_waiters_) {
      waiter(result);
    }
    abort_waiters_.clear();
  }

  void StartApply() {
    VLOG_WITH_PREFIX(4) << __func__ << ", commit time: " << commit_time_ << ", involved tablets: "
                        << AsString(involved_tablets_);
    resend_applying_time_ = MonoTime::Now() +
        std::chrono::microseconds(FLAGS_transaction_resend_applying_interval_usec);
    tablets_with_not_applied_intents_ = involved_tablets_.size();
    if (context_.leader()) {
      for (const auto& tablet : involved_tablets_) {
        context_.NotifyApplying({
            .tablet = tablet.first,
            .transaction = id_,
            .commit_time = commit_time_,
            .sealed = status_ == TransactionStatus::SEALED});
      }
    }
    NotifyAbortWaiters(TransactionStatusResult(TransactionStatus::COMMITTED, commit_time_));
  }

  void FillExpectedTabletBatches(
      std::vector<ExpectedTabletBatches>* expected_tablet_batches) const {
    if (!expected_tablet_batches) {
      return;
    }

    for (const auto& tablet_id_and_state : involved_tablets_) {
      if (!tablet_id_and_state.second.all_batches_replicated) {
        expected_tablet_batches->push_back(ExpectedTabletBatches{
            tablet_id_and_state.first,
            tablet_id_and_state.second.required_replicated_batches});
      }
    }
  }

  TransactionStateContext& context_;
  const TransactionId id_;
  const std::string log_prefix_;
  TransactionStatus status_ = TransactionStatus::PENDING;
  HybridTime last_touch_;
  // It should match last_touch_, but it is possible that because of some code errors it
  // would not be so. To add stability we introduce a separate field for it.
  HybridTime commit_time_;

  // If transaction was only sealed, we will try to abort it not earlier than this time.
  CoarseTimePoint next_abort_after_sealing_;

  struct InvolvedTabletState {
    // How many batches should be replicated at this tablet.
    size_t required_replicated_batches = 0;

    // True if this tablet already replicated all batches.
    bool all_batches_replicated = false;

    // True if this tablet already applied all intents.
    bool all_intents_applied = false;

    std::string ToString() const {
      return Format("{ required_replicated_batches: $0 all_batches_replicated: $1 "
                        "all_intents_applied: $2 }",
                    required_replicated_batches, all_batches_replicated, all_intents_applied);
    }
  };

  // Tablets participating in this transaction.
  std::unordered_map<TabletId, InvolvedTabletState> involved_tablets_;
  // Number of tablets that have not yet replicated all batches.
  size_t tablets_with_not_replicated_batches_ = 0;
  // Number of tablets that have not yet applied intents.
  size_t tablets_with_not_applied_intents_ = 0;
  // Don't resend applying until this time.
  MonoTime resend_applying_time_;
  int64_t first_entry_raft_index_ = std::numeric_limits<int64_t>::max();

  // The operation that we a currently replicating in RAFT.
  // It is owned by TransactionDriver (that will be renamed to OperationDriver).
  tablet::UpdateTxnOperationState* replicating_ = nullptr;
  std::deque<std::unique_ptr<tablet::UpdateTxnOperationState>> request_queue_;

  std::vector<TransactionAbortCallback> abort_waiters_;
};

struct CompleteWithStatusEntry {
  std::unique_ptr<UpdateTxnOperationState> holder;
  UpdateTxnOperationState* request;
  Status status;
};

// Contains actions that should be executed after lock in transaction coordinator is released.
struct PostponedLeaderActions {
  int64_t leader_term = OpId::kUnknownTerm;
  // List of tablets with transaction id, that should be notified that this transaction
  // is applying.
  std::vector<NotifyApplyingData> notify_applying;
  // List of update transaction records, that should be replicated via RAFT.
  std::vector<std::unique_ptr<UpdateTxnOperationState>> updates;

  std::vector<CompleteWithStatusEntry> complete_with_status;

  void Swap(PostponedLeaderActions* other) {
    std::swap(leader_term, other->leader_term);
    notify_applying.swap(other->notify_applying);
    updates.swap(other->updates);
    complete_with_status.swap(other->complete_with_status);
  }

  bool leader() const {
    return leader_term != OpId::kUnknownTerm;
  }
};

} // namespace

// Real implementation of transaction coordinator, as in PImpl idiom.
class TransactionCoordinator::Impl : public TransactionStateContext {
 public:
  Impl(const std::string& permanent_uuid,
       TransactionCoordinatorContext* context,
       Counter* expired_metric)
      : context_(*context),
        expired_metric_(*expired_metric),
        log_prefix_(consensus::MakeTabletLogPrefix(context->tablet_id(), permanent_uuid)),
        poller_(log_prefix_, std::bind(&Impl::Poll, this)) {
  }

  virtual ~Impl() {
    Shutdown();
  }

  void Shutdown() {
    poller_.Shutdown();
    rpcs_.Shutdown();
  }

  CHECKED_STATUS GetStatus(const google::protobuf::RepeatedPtrField<std::string>& transaction_ids,
                           CoarseTimePoint deadline,
                           tserver::GetTransactionStatusResponsePB* response) {
    AtomicFlagSleepMs(&FLAGS_TEST_inject_txn_get_status_delay_ms);
    auto leader_term = context_.LeaderTerm();
    PostponedLeaderActions postponed_leader_actions;
    {
      std::unique_lock<std::mutex> lock(managed_mutex_);
      postponed_leader_actions_.leader_term = leader_term;
      for (const auto& transaction_id : transaction_ids) {
        auto id = VERIFY_RESULT(FullyDecodeTransactionId(transaction_id));

        auto it = managed_transactions_.find(id);
        std::vector<ExpectedTabletBatches> expected_tablet_batches;
        auto txn_status_with_ht = it != managed_transactions_.end()
            ? VERIFY_RESULT(it->GetStatus(&expected_tablet_batches))
            : TransactionStatusResult(TransactionStatus::ABORTED, HybridTime::kMax);
        VLOG_WITH_PREFIX(4) << __func__ << ": " << id << " => " << txn_status_with_ht;
        if (txn_status_with_ht.status == TransactionStatus::SEALED) {
          // TODO(dtxn) Avoid concurrent resolve
          txn_status_with_ht = VERIFY_RESULT(ResolveSealedStatus(
              id, txn_status_with_ht.status_time, expected_tablet_batches,
              /* abort_if_not_replicated = */ false, &lock));
        }
        response->add_status(txn_status_with_ht.status);
        response->add_status_hybrid_time(txn_status_with_ht.status_time.ToUint64());
      }
      postponed_leader_actions.Swap(&postponed_leader_actions_);
    }

    ExecutePostponedLeaderActions(&postponed_leader_actions);
    return Status::OK();
  }

  Result<TransactionStatusResult> ResolveSealedStatus(
      const TransactionId& transaction_id,
      HybridTime commit_time,
      const std::vector<ExpectedTabletBatches>& expected_tablet_batches,
      bool abort_if_not_replicated,
      std::unique_lock<std::mutex>* lock) {
    VLOG_WITH_PREFIX(4)
        << __func__ << ", txn: " << transaction_id << ", commit time: " << commit_time
        << ", expected tablet batches: " << AsString(expected_tablet_batches)
        << ", abort if not replicated: " << abort_if_not_replicated;

    auto deadline = TransactionRpcDeadline();
    auto now_ht = context_.clock().Now();
    CountDownLatch latch(expected_tablet_batches.size());
    std::vector<HybridTime> write_hybrid_times(expected_tablet_batches.size());
    {
      lock->unlock();
      auto scope_exit = ScopeExit([lock] {
        if (lock) {
          lock->lock();
        }
      });
      size_t idx = 0;
      for (const auto& p : expected_tablet_batches) {
        tserver::GetTransactionStatusAtParticipantRequestPB req;
        req.set_tablet_id(p.tablet);
        req.set_transaction_id(
            pointer_cast<const char*>(transaction_id.data()), transaction_id.size());
        req.set_propagated_hybrid_time(now_ht.ToUint64());
        if (abort_if_not_replicated) {
          req.set_required_num_replicated_batches(p.batches);
        }

        auto handle = rpcs_.Prepare();
        if (handle != rpcs_.InvalidHandle()) {
          *handle = GetTransactionStatusAtParticipant(
              deadline,
              nullptr /* remote_tablet */,
              context_.client_future().get(),
              &req,
              [this, handle, idx, &write_hybrid_times, &expected_tablet_batches, &latch,
               &transaction_id, &p](
                  const Status& status,
                  const tserver::GetTransactionStatusAtParticipantResponsePB& resp) {
                client::UpdateClock(resp, &context_);
                rpcs_.Unregister(handle);

                VLOG_WITH_PREFIX(4)
                    << "TXN: " << transaction_id << " batch status at " << p.tablet << ": "
                    << "idx: " << idx << ", resp: " << resp.ShortDebugString() << ", expected: "
                    << expected_tablet_batches[idx].batches;
                if (status.ok()) {
                  if (resp.aborted()) {
                    write_hybrid_times[idx] = HybridTime::kMin;
                  } else if (resp.num_replicated_batches() ==
                                 expected_tablet_batches[idx].batches) {
                    write_hybrid_times[idx] = HybridTime(resp.status_hybrid_time());
                    LOG_IF_WITH_PREFIX(DFATAL, !write_hybrid_times[idx].is_valid())
                        << "Received invalid hybrid time when all batches were replicated: "
                        << resp.ShortDebugString();
                  }
                }
                latch.CountDown();
              });
          (**handle).SendRpc();
        } else {
          latch.CountDown();
        }
        ++idx;
      }
      latch.Wait();
    }

    auto txn_it = managed_transactions_.find(transaction_id);
    if (txn_it == managed_transactions_.end()) {
      // Transaction was completed (aborted/committed) during this procedure.
      return TransactionStatusResult{TransactionStatus::PENDING, commit_time.Decremented()};
    }

    for (size_t idx = 0; idx != expected_tablet_batches.size(); ++idx) {
      if (write_hybrid_times[idx] == HybridTime::kMin) {
        managed_transactions_.modify(txn_it, [](TransactionState& state) {
          state.Aborted();
        });
      } else if (write_hybrid_times[idx].is_valid()) {
        managed_transactions_.modify(
            txn_it, [idx, &expected_tablet_batches, &write_hybrid_times](TransactionState& state) {
          state.ReplicatedAllBatchesAt(
              expected_tablet_batches[idx].tablet, write_hybrid_times[idx]);
        });
      }
    }
    auto result = VERIFY_RESULT(txn_it->GetStatus(/* expected_tablet_batches = */ nullptr));
    if (result.status != TransactionStatus::SEALED) {
      VLOG_WITH_PREFIX(4) << "TXN: " << transaction_id << " status resolved: "
                          << TransactionStatus_Name(result.status);
      return result;
    }

    VLOG_WITH_PREFIX(4) << "TXN: " << transaction_id << " status NOT resolved";
    return TransactionStatusResult{TransactionStatus::PENDING, result.status_time.Decremented()};
  }

  void Abort(const std::string& transaction_id, int64_t term, TransactionAbortCallback callback) {
    AtomicFlagSleepMs(&FLAGS_TEST_inject_txn_get_status_delay_ms);

    auto id = FullyDecodeTransactionId(transaction_id);
    if (!id.ok()) {
      callback(id.status());
      return;
    }

    PostponedLeaderActions actions;
    {
      std::unique_lock<std::mutex> lock(managed_mutex_);
      auto it = managed_transactions_.find(*id);
      if (it == managed_transactions_.end()) {
        lock.unlock();
        callback(TransactionStatusResult::Aborted());
        return;
      }
      postponed_leader_actions_.leader_term = term;
      boost::optional<TransactionStatusResult> status;
      managed_transactions_.modify(it, [&status, &callback](TransactionState& state) {
        status = state.Abort(&callback);
      });
      if (callback) {
        lock.unlock();
        callback(*status);
        return;
      }
      actions.Swap(&postponed_leader_actions_);
    }

    ExecutePostponedLeaderActions(&actions);
  }

  size_t test_count_transactions() {
    std::lock_guard<std::mutex> lock(managed_mutex_);
    return managed_transactions_.size();
  }

  CHECKED_STATUS ProcessReplicated(const ReplicatedData& data) {
    auto id = FullyDecodeTransactionId(data.state.transaction_id());
    if (!id.ok()) {
      return std::move(id.status());
    }

    PostponedLeaderActions actions;
    Status result;
    {
      std::lock_guard<std::mutex> lock(managed_mutex_);
      postponed_leader_actions_.leader_term = data.leader_term;
      auto it = GetTransaction(*id, data.state.status(), data.hybrid_time);
      if (it == managed_transactions_.end()) {
        return Status::OK();
      }
      managed_transactions_.modify(it, [&result, &data](TransactionState& state) {
        result = state.ProcessReplicated(data);
      });
      CheckCompleted(it);
      actions.Swap(&postponed_leader_actions_);
    }
    ExecutePostponedLeaderActions(&actions);

    VLOG_WITH_PREFIX(1) << "Processed: " << data.ToString();
    return result;
  }

  void ProcessAborted(const AbortedData& data) {
    auto id = FullyDecodeTransactionId(data.state.transaction_id());
    if (!id.ok()) {
      LOG_WITH_PREFIX(DFATAL) << "Abort of transaction with bad id "
                              << data.state.ShortDebugString() << ": " << id.status();
      return;
    }

    PostponedLeaderActions actions;
    {
      std::lock_guard<std::mutex> lock(managed_mutex_);
      postponed_leader_actions_.leader_term = OpId::kUnknownTerm;
      auto it = managed_transactions_.find(*id);
      if (it == managed_transactions_.end()) {
        LOG_WITH_PREFIX(WARNING) << "Aborted operation for unknown transaction: " << *id;
        return;
      }
      managed_transactions_.modify(
          it, [&](TransactionState& ts) {
            ts.ProcessAborted(data);
          });
      CheckCompleted(it);
      actions.Swap(&postponed_leader_actions_);
    }
    ExecutePostponedLeaderActions(&actions);

    VLOG_WITH_PREFIX(1) << "Aborted, state: " << data.state.ShortDebugString()
                        << ", op id: " << data.op_id;
  }

  void Start() {
    poller_.Start(
        &context_.client_future().get()->messenger()->scheduler(),
        std::chrono::microseconds(kTimeMultiplier * FLAGS_transaction_check_interval_usec));
  }

  void Handle(std::unique_ptr<tablet::UpdateTxnOperationState> request, int64_t term) {
    auto& state = *request->request();
    auto id = FullyDecodeTransactionId(state.transaction_id());
    if (!id.ok()) {
      LOG(WARNING) << "Failed to decode id from " << state.ShortDebugString() << ": " << id;
      request->CompleteWithStatus(id.status());
      return;
    }

    PostponedLeaderActions actions;
    {
      std::unique_lock<std::mutex> lock(managed_mutex_);
      postponed_leader_actions_.leader_term = term;
      auto it = managed_transactions_.find(*id);
      if (it == managed_transactions_.end()) {
        if (state.status() == TransactionStatus::CREATED) {
          it = managed_transactions_.emplace(
              this, *id, context_.clock().Now(), log_prefix_).first;
        } else {
          lock.unlock();
          YB_LOG_HIGHER_SEVERITY_WHEN_TOO_MANY(INFO, WARNING, 1s, 50)
              << LogPrefix() << "Request to unknown transaction " << id << ": "
              << state.ShortDebugString();
          auto status = STATUS(Expired, "Transaction expired or aborted by a conflict",
                               PgsqlError(YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE));
          status = status.CloneAndAddErrorCode(TransactionError(TransactionErrorCode::kAborted));
          request->CompleteWithStatus(status);
          return;
        }
      }

      managed_transactions_.modify(it, [&request](TransactionState& state) {
        state.Handle(std::move(request));
      });
      postponed_leader_actions_.Swap(&actions);
    }

    ExecutePostponedLeaderActions(&actions);
  }

  int64_t PrepareGC(std::string* details) {
    std::lock_guard<std::mutex> lock(managed_mutex_);
    if (!managed_transactions_.empty()) {
      auto& txn = *managed_transactions_.get<FirstEntryIndexTag>().begin();
      if (details) {
        *details += Format("Transaction coordinator: $0\n", txn);
      }
      return txn.first_entry_raft_index();
    }
    return std::numeric_limits<int64_t>::max();
  }

  // Returns logs prefix for this transaction coordinator.
  const std::string& LogPrefix() {
    return log_prefix_;
  }

  std::string DumpTransactions() {
    std::string result;
    std::lock_guard<std::mutex> lock(managed_mutex_);
    for (const auto& txn : managed_transactions_) {
      result += txn.ToString();
      result += "\n";
    }
    return result;
  }

 private:
  class LastTouchTag;
  class FirstEntryIndexTag;

  typedef boost::multi_index_container<TransactionState,
      boost::multi_index::indexed_by <
          boost::multi_index::hashed_unique <
              boost::multi_index::const_mem_fun<TransactionState,
                                                const TransactionId&,
                                                &TransactionState::id>
          >,
          boost::multi_index::ordered_non_unique <
              boost::multi_index::tag<LastTouchTag>,
              boost::multi_index::const_mem_fun<TransactionState,
                                                HybridTime,
                                                &TransactionState::last_touch>
          >,
          boost::multi_index::ordered_non_unique <
              boost::multi_index::tag<FirstEntryIndexTag>,
              boost::multi_index::const_mem_fun<TransactionState,
                                                int64_t,
                                                &TransactionState::first_entry_raft_index>
          >
      >
  > ManagedTransactions;

  void ExecutePostponedLeaderActions(PostponedLeaderActions* actions) {
    for (const auto& p : actions->complete_with_status) {
      p.request->CompleteWithStatus(p.status);
    }

    if (!actions->leader()) {
      return;
    }

    if (!actions->notify_applying.empty()) {
      auto deadline = TransactionRpcDeadline();
      for (const auto& action : actions->notify_applying) {
        VLOG_WITH_PREFIX(3) << "Notify applying: " << action.ToString();

        tserver::UpdateTransactionRequestPB req;
        req.set_tablet_id(action.tablet);
        auto& state = *req.mutable_state();
        state.set_transaction_id(action.transaction.data(), action.transaction.size());
        state.set_status(TransactionStatus::APPLYING);
        state.add_tablets(context_.tablet_id());
        state.set_commit_hybrid_time(action.commit_time.ToUint64());
        state.set_sealed(action.sealed);

        auto handle = rpcs_.Prepare();
        if (handle != rpcs_.InvalidHandle()) {
          *handle = UpdateTransaction(
              deadline,
              nullptr /* remote_tablet */,
              context_.client_future().get(),
              &req,
              [this, handle, txn_id = action.transaction, tablet = action.tablet]
                  (const Status& status, const tserver::UpdateTransactionResponsePB& resp) {
                client::UpdateClock(resp, &context_);
                rpcs_.Unregister(handle);
                LOG_IF_WITH_PREFIX(WARNING, !status.ok()) << "Failed to send apply: " << status;
                // Tablet was deleted, so we should mark it as applied to cleanup transaction.
                if (status.IsNotFound()) {
                  std::lock_guard<std::mutex> lock(managed_mutex_);
                  auto it = managed_transactions_.find(txn_id);
                  if (it != managed_transactions_.end()) {
                    managed_transactions_.modify(it, [&tablet](TransactionState& state) {
                      tserver::TransactionStatePB transaction_state;
                      transaction_state.add_tablets(tablet);
                      WARN_NOT_OK(state.AppliedInOneOfInvolvedTablets(transaction_state),
                                  "AppliedInOneOfInvolvedTablets for removed tabled failed: ");
                    });
                  }
                }
              });
          (**handle).SendRpc();
        }
      }
    }

    for (auto& update : actions->updates) {
      context_.SubmitUpdateTransaction(std::move(update), actions->leader_term);
    }
  }

  ManagedTransactions::iterator GetTransaction(const TransactionId& id,
                                               TransactionStatus status,
                                               HybridTime hybrid_time) {
    auto it = managed_transactions_.find(id);
    if (it == managed_transactions_.end()) {
      if (status != TransactionStatus::APPLIED_IN_ALL_INVOLVED_TABLETS) {
        it = managed_transactions_.emplace(this, id, hybrid_time, log_prefix_).first;
        VLOG_WITH_PREFIX(1) << Format("Added: $0", *it);
      }
    }
    return it;
  }

  TransactionCoordinatorContext& coordinator_context() override {
    return context_;
  }

  void NotifyApplying(NotifyApplyingData data) override {
    if (!leader()) {
      LOG_WITH_PREFIX(WARNING) << __func__ << " at non leader: " << data.ToString();
      return;
    }
    postponed_leader_actions_.notify_applying.push_back(std::move(data));
  }

  MUST_USE_RESULT bool SubmitUpdateTransaction(
      std::unique_ptr<UpdateTxnOperationState> state) override {
    if (postponed_leader_actions_.leader()) {
      postponed_leader_actions_.updates.push_back(std::move(state));
      return true;
    } else {
      auto status = STATUS(IllegalState, "Submit update transaction on non leader");
      VLOG_WITH_PREFIX(1) << status;
      state->CompleteWithStatus(status);
      return false;
    }
  }

  void CompleteWithStatus(
      std::unique_ptr<UpdateTxnOperationState> request, Status status) override {
    auto ptr = request.get();
    postponed_leader_actions_.complete_with_status.push_back({
        std::move(request), ptr, std::move(status)});
  }

  void CompleteWithStatus(UpdateTxnOperationState* request, Status status) override {
    postponed_leader_actions_.complete_with_status.push_back({
        nullptr /* holder */, request, std::move(status)});
  }

  bool leader() const override {
    return postponed_leader_actions_.leader();
  }

  Counter& expired_metric() override {
    return expired_metric_;
  }

  void Poll() {
    auto now = context_.clock().Now();

    auto leader_term = context_.LeaderTerm();
    PostponedLeaderActions actions;
    {
      std::lock_guard<std::mutex> lock(managed_mutex_);
      postponed_leader_actions_.leader_term = leader_term;

      auto& index = managed_transactions_.get<LastTouchTag>();

      for (auto it = index.begin(); it != index.end() && it->ExpiredAt(now);) {
        if (it->status() == TransactionStatus::ABORTED) {
          it = index.erase(it);
        } else {
          bool modified = index.modify(it, [](TransactionState& state) {
            VLOG(4) << state.LogPrefix() << "Cleanup expired transaction";
            state.Abort();
          });
          DCHECK(modified);
          ++it;
        }
      }
      auto now_physical = MonoTime::Now();
      for (auto& transaction : managed_transactions_) {
        const_cast<TransactionState&>(transaction).Poll(
            leader_term != OpId::kUnknownTerm, now_physical);
      }
      postponed_leader_actions_.Swap(&actions);
    }
    ExecutePostponedLeaderActions(&actions);
  }

  void CheckCompleted(ManagedTransactions::iterator it) {
    if (it->Completed()) {
      auto status = STATUS_FORMAT(Expired, "Transaction completed: $0", *it);
      VLOG_WITH_PREFIX(1) << status;
      managed_transactions_.modify(it, [&status](TransactionState& state) {
        state.ClearRequests(status);
      });
      managed_transactions_.erase(it);
    }
  }

  TransactionCoordinatorContext& context_;
  Counter& expired_metric_;
  const std::string log_prefix_;

  std::mutex managed_mutex_;
  ManagedTransactions managed_transactions_;

  // Actions that should be executed after mutex is unlocked.
  PostponedLeaderActions postponed_leader_actions_;

  rpc::Poller poller_;
  rpc::Rpcs rpcs_;
};

TransactionCoordinator::TransactionCoordinator(const std::string& permanent_uuid,
                                               TransactionCoordinatorContext* context,
                                               Counter* expired_metric)
    : impl_(new Impl(permanent_uuid, context, expired_metric)) {
}

TransactionCoordinator::~TransactionCoordinator() {
}

Status TransactionCoordinator::ProcessReplicated(const ReplicatedData& data) {
  return impl_->ProcessReplicated(data);
}

void TransactionCoordinator::ProcessAborted(const AbortedData& data) {
  impl_->ProcessAborted(data);
}

int64_t TransactionCoordinator::PrepareGC(std::string* details) {
  return impl_->PrepareGC(details);
}

size_t TransactionCoordinator::test_count_transactions() const {
  return impl_->test_count_transactions();
}

void TransactionCoordinator::Handle(
    std::unique_ptr<tablet::UpdateTxnOperationState> request, int64_t term) {
  impl_->Handle(std::move(request), term);
}

void TransactionCoordinator::Start() {
  impl_->Start();
}

void TransactionCoordinator::Shutdown() {
  impl_->Shutdown();
}

Status TransactionCoordinator::GetStatus(
    const google::protobuf::RepeatedPtrField<std::string>& transaction_ids,
    CoarseTimePoint deadline,
    tserver::GetTransactionStatusResponsePB* response) {
  return impl_->GetStatus(transaction_ids, deadline, response);
}

void TransactionCoordinator::Abort(const std::string& transaction_id,
                                   int64_t term,
                                   TransactionAbortCallback callback) {
  impl_->Abort(transaction_id, term, std::move(callback));
}

std::string TransactionCoordinator::DumpTransactions() {
  return impl_->DumpTransactions();
}

std::string TransactionCoordinator::ReplicatedData::ToString() const {
  return Format("{ leader_term: $0 state: $1 op_id: $2 hybrid_time: $3 }",
                leader_term, state, op_id, hybrid_time);
}

} // namespace tablet
} // namespace yb
