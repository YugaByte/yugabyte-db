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

#include "yb/tablet/running_transaction.h"

#include "yb/client/transaction_rpc.h"

#include "yb/common/pgsql_error.h"

#include "yb/util/flag_tags.h"
#include "yb/util/tsan_util.h"
#include "yb/util/yb_pg_errcodes.h"

using namespace std::placeholders;
using namespace std::literals;

DEFINE_test_flag(uint64, transaction_delay_status_reply_usec_in_tests, 0,
                 "For tests only. Delay handling status reply by specified amount of usec.");

DEFINE_int64(transaction_abort_check_interval_ms, 5000 * yb::kTimeMultiplier,
             "Interval to check whether running transaction was aborted.");

DEFINE_int64(transaction_abort_check_timeout_ms, 30000 * yb::kTimeMultiplier,
             "Timeout used when checking for aborted transactions.");

namespace yb {
namespace tablet {

RunningTransaction::RunningTransaction(TransactionMetadata metadata,
                                       const TransactionalBatchData& last_batch_data,
                                       OneWayBitmap&& replicated_batches,
                                       HybridTime base_time_for_abort_check_ht_calculation,
                                       RunningTransactionContext* context)
    : metadata_(std::move(metadata)),
      last_batch_data_(last_batch_data),
      replicated_batches_(std::move(replicated_batches)),
      context_(*context),
      remove_intents_task_(&context->applier_, &context->participant_context_, context,
                           metadata_.transaction_id),
      get_status_handle_(context->rpcs_.InvalidHandle()),
      abort_handle_(context->rpcs_.InvalidHandle()),
      apply_intents_task_(&context->applier_, context, &apply_data_),
      abort_check_ht_(base_time_for_abort_check_ht_calculation.AddDelta(
                          1ms * FLAGS_transaction_abort_check_interval_ms)) {
}

RunningTransaction::~RunningTransaction() {
  context_.rpcs_.Abort({&get_status_handle_, &abort_handle_});
}

void RunningTransaction::AddReplicatedBatch(
  size_t batch_idx, boost::container::small_vector_base<uint8_t>* encoded_replicated_batches) {
  VLOG_WITH_PREFIX(4) << __func__ << "(" << batch_idx << ")";
  replicated_batches_.Set(batch_idx);
  encoded_replicated_batches->push_back(docdb::ValueTypeAsChar::kBitSet);
  replicated_batches_.EncodeTo(encoded_replicated_batches);
}

void RunningTransaction::BatchReplicated(const TransactionalBatchData& value) {
  VLOG_WITH_PREFIX(4) << __func__ << "(" << value.ToString() << ")";
  last_batch_data_ = value;
}

void RunningTransaction::SetLocalCommitTime(HybridTime time) {
  local_commit_time_ = time;
  last_known_status_hybrid_time_ = local_commit_time_;
  last_known_status_ = TransactionStatus::COMMITTED;
}

void RunningTransaction::Aborted() {
  VLOG_WITH_PREFIX(4) << __func__ << "()";

  last_known_status_ = TransactionStatus::ABORTED;
  last_known_status_hybrid_time_ = HybridTime::kMax;
}

void RunningTransaction::RequestStatusAt(const StatusRequest& request,
                                         std::unique_lock<std::mutex>* lock) {
  DCHECK_LE(request.global_limit_ht, HybridTime::kMax);
  DCHECK_LE(request.read_ht, request.global_limit_ht);

  if (last_known_status_hybrid_time_ > HybridTime::kMin) {
    auto transaction_status =
        GetStatusAt(request.global_limit_ht, last_known_status_hybrid_time_, last_known_status_);
    // If we don't have status at global_limit_ht, then we should request updated status.
    if (transaction_status) {
      HybridTime last_known_status_hybrid_time = last_known_status_hybrid_time_;
      lock->unlock();
      request.callback(
          TransactionStatusResult{*transaction_status, last_known_status_hybrid_time});
      return;
    }
  }
  bool was_empty = status_waiters_.empty();
  status_waiters_.push_back(request);
  if (!was_empty) {
    return;
  }
  auto request_id = context_.NextRequestIdUnlocked();
  auto shared_self = shared_from_this();

  VLOG_WITH_PREFIX(4) << Format(
      "Existing status knowledge ($0, $1) does not satisfy requested: $2, sending: $3",
      TransactionStatus_Name(last_known_status_), last_known_status_hybrid_time_, request,
      request_id);

  lock->unlock();
  SendStatusRequest(request_id, shared_self);
}

bool RunningTransaction::WasAborted() const {
  return last_known_status_ == TransactionStatus::ABORTED;
}

CHECKED_STATUS RunningTransaction::CheckAborted() const {
  if (WasAborted()) {
    return MakeAbortedStatus(id());
  }
  return Status::OK();
}

void RunningTransaction::Abort(client::YBClient* client,
                               TransactionStatusCallback callback,
                               std::unique_lock<std::mutex>* lock) {
  if (last_known_status_ == TransactionStatus::ABORTED ||
      last_known_status_ == TransactionStatus::COMMITTED) {
    // Transaction is already in final state, so no reason to send abort request.
    VLOG_WITH_PREFIX(3) << "Abort shortcut: " << last_known_status_;
    TransactionStatusResult status{last_known_status_, last_known_status_hybrid_time_};
    lock->unlock();
    callback(status);
    return;
  }
  bool was_empty = abort_waiters_.empty();
  abort_waiters_.push_back(std::move(callback));
  lock->unlock();
  VLOG_WITH_PREFIX(3) << "Abort request: " << was_empty;
  if (!was_empty) {
    return;
  }
  tserver::AbortTransactionRequestPB req;
  req.set_tablet_id(metadata_.status_tablet);
  req.set_transaction_id(metadata_.transaction_id.data(), metadata_.transaction_id.size());
  req.set_propagated_hybrid_time(context_.participant_context_.Now().ToUint64());
  context_.rpcs_.RegisterAndStart(
      client::AbortTransaction(
          TransactionRpcDeadline(),
          nullptr /* tablet */,
          client,
          &req,
          std::bind(&RunningTransaction::AbortReceived, this, _1, _2, shared_from_this())),
      &abort_handle_);
}

std::string RunningTransaction::ToString() const {
  return Format("{ metadata: $0 last_batch_data: $1 replicated_batches: $2 local_commit_time: $3 "
                    "last_known_status: $4 last_known_status_hybrid_time: $5 }",
                metadata_, last_batch_data_, replicated_batches_, local_commit_time_,
                TransactionStatus_Name(last_known_status_), last_known_status_hybrid_time_);
}

void RunningTransaction::ScheduleRemoveIntents(const RunningTransactionPtr& shared_self) {
  if (remove_intents_task_.Prepare(shared_self)) {
    context_.participant_context_.StrandEnqueue(&remove_intents_task_);
    VLOG_WITH_PREFIX(1) << "Intents should be removed asynchronously";
  }
}

boost::optional<TransactionStatus> RunningTransaction::GetStatusAt(
    HybridTime time,
    HybridTime last_known_status_hybrid_time,
    TransactionStatus last_known_status) {
  switch (last_known_status) {
    case TransactionStatus::ABORTED:
      return TransactionStatus::ABORTED;
    case TransactionStatus::COMMITTED:
      return last_known_status_hybrid_time > time
          ? TransactionStatus::PENDING
          : TransactionStatus::COMMITTED;
    case TransactionStatus::PENDING:
      if (last_known_status_hybrid_time >= time) {
        return TransactionStatus::PENDING;
      }
      return boost::none;
    default:
      FATAL_INVALID_ENUM_VALUE(TransactionStatus, last_known_status);
  }
}

void RunningTransaction::SendStatusRequest(
    int64_t serial_no, const RunningTransactionPtr& shared_self) {
  tserver::GetTransactionStatusRequestPB req;
  req.set_tablet_id(metadata_.status_tablet);
  req.add_transaction_id()->assign(
      pointer_cast<const char*>(metadata_.transaction_id.data()), metadata_.transaction_id.size());
  req.set_propagated_hybrid_time(context_.participant_context_.Now().ToUint64());
  context_.rpcs_.RegisterAndStart(
      client::GetTransactionStatus(
          TransactionRpcDeadline(),
          nullptr /* tablet */,
          context_.participant_context_.client_future().get(),
          &req,
          std::bind(&RunningTransaction::StatusReceived, this, _1, _2, serial_no, shared_self)),
      &get_status_handle_);
}

void RunningTransaction::StatusReceived(
    const Status& status,
    const tserver::GetTransactionStatusResponsePB& response,
    int64_t serial_no,
    const RunningTransactionPtr& shared_self) {
  auto delay_usec = FLAGS_TEST_transaction_delay_status_reply_usec_in_tests;
  if (delay_usec > 0) {
    context_.delayer().Delay(
        MonoTime::Now() + MonoDelta::FromMicroseconds(delay_usec),
        std::bind(&RunningTransaction::DoStatusReceived, this, status, response,
                  serial_no, shared_self));
  } else {
    DoStatusReceived(status, response, serial_no, shared_self);
  }
}

bool RunningTransaction::UpdateStatus(
    TransactionStatus transaction_status, HybridTime time_of_status) {
  // Check for local_commit_time_ is not required for correctness, but useful for optimization.
  // So we could avoid unnecessary actions.
  if (local_commit_time_) {
    return false;
  }

  last_known_status_hybrid_time_ = time_of_status;

  if (transaction_status == last_known_status_) {
    return false;
  }

  last_known_status_ = transaction_status;

  return transaction_status == TransactionStatus::ABORTED;
}

void RunningTransaction::DoStatusReceived(const Status& status,
                                          const tserver::GetTransactionStatusResponsePB& response,
                                          int64_t serial_no,
                                          const RunningTransactionPtr& shared_self) {
  VLOG_WITH_PREFIX(4) << __func__ << "(" << status << ", " << response.ShortDebugString() << ", "
                      << serial_no << ")";

  if (response.has_propagated_hybrid_time()) {
    context_.participant_context_.UpdateClock(HybridTime(response.propagated_hybrid_time()));
  }

  context_.rpcs_.Unregister(&get_status_handle_);
  decltype(status_waiters_) status_waiters;
  HybridTime time_of_status;
  TransactionStatus transaction_status;
  const bool ok = status.ok();
  int64_t new_request_id = -1;
  {
    MinRunningNotifier min_running_notifier(&context_.applier_);
    std::unique_lock<std::mutex> lock(context_.mutex_);
    if (!ok) {
      status_waiters_.swap(status_waiters);
      lock.unlock();
      for (const auto& waiter : status_waiters) {
        waiter.callback(status);
      }
      return;
    }

    if (response.status_hybrid_time().size() == 1) {
      time_of_status = HybridTime(response.status_hybrid_time()[0]);
    } else {
      LOG(DFATAL) << "Wrong number of status hybrid time entries, exactly one entry expected: "
                  << response.ShortDebugString();
      time_of_status = HybridTime::kMin;
    }

    if (response.status().size() == 1) {
      transaction_status = response.status(0);
    } else {
      LOG(DFATAL) << "Wrong number of status entries, exactly one entry expected: "
                  << response.ShortDebugString();
      transaction_status = TransactionStatus::PENDING;
    }

    if (UpdateStatus(transaction_status, time_of_status)) {
      context_.EnqueueRemoveUnlocked(id(), &min_running_notifier);
    }

    time_of_status = last_known_status_hybrid_time_;
    transaction_status = last_known_status_;

    status_waiters = ExtractFinishedStatusWaitersUnlocked(
        serial_no, time_of_status, transaction_status);
    if (!status_waiters_.empty()) {
      new_request_id = context_.NextRequestIdUnlocked();
      VLOG_WITH_PREFIX(4) << "Waiters still present, send new status request: " << new_request_id;
    }
  }
  if (new_request_id >= 0) {
    SendStatusRequest(new_request_id, shared_self);
  }
  NotifyWaiters(serial_no, time_of_status, transaction_status, status_waiters);
}

std::vector<StatusRequest> RunningTransaction::ExtractFinishedStatusWaitersUnlocked(
    int64_t serial_no, HybridTime time_of_status, TransactionStatus transaction_status) {
  if (transaction_status == TransactionStatus::ABORTED) {
    return std::move(status_waiters_);
  }
  std::vector<StatusRequest> result;
  result.reserve(status_waiters_.size());
  auto w = status_waiters_.begin();
  for (auto it = status_waiters_.begin(); it != status_waiters_.end(); ++it) {
    if (it->serial_no <= serial_no ||
        GetStatusAt(it->global_limit_ht, time_of_status, transaction_status) ||
        time_of_status < it->read_ht) {
      result.push_back(std::move(*it));
    } else {
      if (w != it) {
        *w = std::move(*it);
      }
      ++w;
    }
  }
  status_waiters_.erase(w, status_waiters_.end());
  return result;
}

void RunningTransaction::NotifyWaiters(int64_t serial_no, HybridTime time_of_status,
                                       TransactionStatus transaction_status,
                                       const std::vector<StatusRequest>& status_waiters) {
  for (const auto& waiter : status_waiters) {
    auto status_for_waiter = GetStatusAt(
        waiter.global_limit_ht, time_of_status, transaction_status);
    if (status_for_waiter) {
      // We know status at global_limit_ht, so could notify waiter.
      waiter.callback(TransactionStatusResult{*status_for_waiter, time_of_status});
    } else if (time_of_status >= waiter.read_ht) {
      // It means that between read_ht and global_limit_ht transaction was pending.
      // It implies that transaction was not committed before request was sent.
      // We could safely respond PENDING to caller.
      LOG_IF_WITH_PREFIX(DFATAL, waiter.serial_no > serial_no)
          << "Notify waiter with request id greater than id of status request: "
          << waiter.serial_no << " vs " << serial_no;
      waiter.callback(TransactionStatusResult{TransactionStatus::PENDING, time_of_status});
    } else {
      waiter.callback(STATUS(TryAgain,
          Format("Cannot determine transaction status with read_ht $0, and global_limit_ht $1, "
                 "last known: $2 at $3", waiter.read_ht, waiter.global_limit_ht,
                 TransactionStatus_Name(transaction_status), time_of_status), Slice(),
          PgsqlError(YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE) ));
    }
  }
}

Result<TransactionStatusResult> RunningTransaction::MakeAbortResult(
    const Status& status,
    const tserver::AbortTransactionResponsePB& response) {
  if (!status.ok()) {
    return status;
  }

  HybridTime status_time = response.has_status_hybrid_time()
       ? HybridTime(response.status_hybrid_time())
       : HybridTime::kInvalid;
  return TransactionStatusResult{response.status(), status_time};
}

void RunningTransaction::AbortReceived(const Status& status,
                                       const tserver::AbortTransactionResponsePB& response,
                                       const RunningTransactionPtr& shared_self) {
  if (response.has_propagated_hybrid_time()) {
    context_.participant_context_.UpdateClock(HybridTime(response.propagated_hybrid_time()));
  }

  decltype(abort_waiters_) abort_waiters;
  auto result = MakeAbortResult(status, response);

  VLOG_WITH_PREFIX(3) << "AbortReceived: " << yb::ToString(result);

  {
    MinRunningNotifier min_running_notifier(&context_.applier_);
    std::lock_guard<std::mutex> lock(context_.mutex_);
    context_.rpcs_.Unregister(&abort_handle_);
    abort_waiters_.swap(abort_waiters);
    // kMax status_time means that this status is not yet replicated and could be rejected.
    // So we could use it as reply to Abort, but cannot store it as transaction status.
    if (result.ok() && result->status_time != HybridTime::kMax) {
      last_known_status_hybrid_time_ = result->status_time;
      if (last_known_status_ != result->status) {
        last_known_status_ = result->status;
        if (result->status == TransactionStatus::ABORTED) {
          context_.EnqueueRemoveUnlocked(id(), &min_running_notifier);
        }
      }
    }
  }
  for (const auto& waiter : abort_waiters) {
    waiter(result);
  }
}

std::string RunningTransaction::LogPrefix() const {
  return Format(
      "$0 ID $1: ", context_.LogPrefix().substr(0, context_.LogPrefix().length() - 2), id());
}

Status MakeAbortedStatus(const TransactionId& id) {
  return STATUS(
      TryAgain, Format("Transaction aborted: $0", id), Slice(),
      PgsqlError(YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE));
}

void RunningTransaction::SetApplyData(const docdb::ApplyTransactionState& apply_state,
                                      const TransactionApplyData* data) {
  apply_state_ = apply_state;
  bool active = apply_state_.active();

  if (data) {
    apply_data_ = *data;
    apply_data_.apply_state = &apply_state_;

    LOG_IF_WITH_PREFIX(DFATAL, local_commit_time_ != data->commit_ht)
        << "Commit time does not match: " << local_commit_time_ << " vs " << data->commit_ht;

    if (apply_intents_task_.Prepare(shared_from_this())) {
      context_.participant_context_.StrandEnqueue(&apply_intents_task_);
    } else {
      LOG_WITH_PREFIX(DFATAL) << "Unable to prepare apply intents task";
    }
  }

  if (!active) {
    VLOG_WITH_PREFIX(3) << "Finished applying intents";

    MinRunningNotifier min_running_notifier(&context_.applier_);
    std::lock_guard<std::mutex> lock(context_.mutex_);
    context_.RemoveUnlocked(id(), "applied large"s, &min_running_notifier);
  }
}

bool RunningTransaction::ProcessingApply() const {
  return apply_state_.active();
}

void RunningTransaction::UpdateAbortCheckHT(HybridTime now, UpdateAbortCheckHTMode mode) {
  if (last_known_status_ == TransactionStatus::ABORTED ||
      last_known_status_ == TransactionStatus::COMMITTED) {
    abort_check_ht_ = HybridTime::kMax;
    return;
  }
  // When we send a status request, we schedule the transaction status to be re-checked around the
  // same time the request is supposed to time out. When we get a status response (normal case, no
  // timeout), we go back to the normal interval of re-checking the status of this transaction.
  auto delta_ms = mode == UpdateAbortCheckHTMode::kStatusRequestSent
      ? FLAGS_transaction_abort_check_timeout_ms
      : FLAGS_transaction_abort_check_interval_ms;
  abort_check_ht_ = now.AddDelta(1ms * delta_ms);
}

} // namespace tablet
} // namespace yb
