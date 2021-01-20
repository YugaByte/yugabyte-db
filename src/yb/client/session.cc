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

#include "yb/client/session.h"

#include "yb/client/batcher.h"
#include "yb/client/client.h"
#include "yb/client/error.h"
#include "yb/client/error_collector.h"
#include "yb/client/yb_op.h"

#include "yb/common/consistent_read_point.h"

DEFINE_int32(client_read_write_timeout_ms, 60000, "Timeout for client read and write operations.");

namespace yb {
namespace client {

using internal::AsyncRpcMetrics;
using internal::Batcher;
using internal::ErrorCollector;

using std::shared_ptr;

YBSession::YBSession(YBClient* client, const scoped_refptr<ClockBase>& clock)
    : client_(client),
      read_point_(clock ? std::make_unique<ConsistentReadPoint>(clock) : nullptr),
      error_collector_(new ErrorCollector()),
      timeout_(MonoDelta::FromMilliseconds(FLAGS_client_read_write_timeout_ms)),
      hybrid_time_for_write_(HybridTime::kInvalid) {
  const auto metric_entity = client_->metric_entity();
  async_rpc_metrics_ = metric_entity ? std::make_shared<AsyncRpcMetrics>(metric_entity) : nullptr;
}

void YBSession::SetReadPoint(const Restart restart) {
  DCHECK_NOTNULL(read_point_.get());
  if (restart && read_point_->IsRestartRequired()) {
    read_point_->Restart();
  } else {
    read_point_->SetCurrentReadTime();
  }
}

void YBSession::SetReadPoint(const ReadHybridTime& read_time) {
  read_point_->SetReadTime(read_time, {} /* local_limits */);
}

bool YBSession::IsRestartRequired() {
  auto rp = read_point();
  return rp && rp->IsRestartRequired();
}

void YBSession::DeferReadPoint() {
  read_point_->Defer();
}

void YBSession::SetTransaction(YBTransactionPtr transaction) {
  transaction_ = std::move(transaction);
  internal::BatcherPtr old_batcher;
  old_batcher.swap(batcher_);
  if (old_batcher) {
    LOG_IF(DFATAL, old_batcher->HasPendingOperations()) << "SetTransaction with non empty batcher";
    old_batcher->Abort(STATUS(Aborted, "Transaction changed"));
  }
}

void YBSession::SetRejectionScoreSource(RejectionScoreSourcePtr rejection_score_source) {
  if (batcher_) {
    batcher_->SetRejectionScoreSource(rejection_score_source);
  }
  rejection_score_source_ = std::move(rejection_score_source);
}

YBSession::~YBSession() {
  WARN_NOT_OK(Close(true), "Closed Session with pending operations.");
}

void YBSession::Abort() {
  if (batcher_ && batcher_->HasPendingOperations()) {
    batcher_->Abort(STATUS(Aborted, "Batch aborted"));
    batcher_.reset();
  }
}

void YBSession::Reset() {
  Abort();
  error_collector_->ClearErrors();
  is_failed_ = false;
}

Status YBSession::Close(bool force) {
  if (batcher_) {
    if (batcher_->HasPendingOperations() && !force) {
      return STATUS(IllegalState, "Could not close. There are pending operations.");
    }
    batcher_->Abort(STATUS(Aborted, "Batch aborted"));
    batcher_.reset();
  }
  return Status::OK();
}

void YBSession::SetTimeout(MonoDelta timeout) {
  CHECK_GE(timeout, MonoDelta::kZero);
  timeout_ = timeout;
  if (batcher_) {
    batcher_->SetTimeout(timeout);
  }
}

Status YBSession::Flush() {
  RETURN_NOT_OK(CheckIfFailed());
  Synchronizer s;
  FlushAsync(s.AsStatusFunctor());
  return s.Wait();
}

void YBSession::FlushAsync(StatusFunctor callback) {
  // Swap in a new batcher to start building the next batch.
  // Save off the old batcher.
  //
  // Send off any buffered data. Important to do this outside of the lock
  // since the callback may itself try to take the lock, in the case that
  // the batch fails "inline" on the same thread.

  internal::BatcherPtr old_batcher;
  old_batcher.swap(batcher_);
  if (old_batcher) {
    {
      std::lock_guard<simple_spinlock> l(lock_);
      flushed_batchers_.insert(old_batcher);
    }
    old_batcher->set_allow_local_calls_in_curr_thread(allow_local_calls_in_curr_thread_);
    old_batcher->FlushAsync(std::move(callback));
  } else {
    callback(Status::OK());
  }
}

std::future<Status> YBSession::FlushFuture() {
  return MakeFuture<Status>([this](auto callback) { this->FlushAsync(std::move(callback)); });
}

Status YBSession::ReadSync(std::shared_ptr<YBOperation> yb_op) {
  Synchronizer s;
  ReadAsync(std::move(yb_op), s.AsStatusFunctor());
  return s.Wait();
}

void YBSession::ReadAsync(std::shared_ptr<YBOperation> yb_op, StatusFunctor callback) {
  CHECK(yb_op->read_only());
  CHECK_OK(Apply(std::move(yb_op)));
  FlushAsync(std::move(callback));
}

YBClient* YBSession::client() const {
  return client_;
}

void YBSession::FlushFinished(internal::BatcherPtr batcher) {
  std::lock_guard<simple_spinlock> l(lock_);
  CHECK_EQ(flushed_batchers_.erase(batcher), 1);
}

bool YBSession::allow_local_calls_in_curr_thread() const {
  return allow_local_calls_in_curr_thread_;
}

void YBSession::set_allow_local_calls_in_curr_thread(bool flag) {
  allow_local_calls_in_curr_thread_ = flag;
}

void YBSession::SetInTxnLimit(HybridTime value) {
  auto* rp = read_point();
  LOG_IF(DFATAL, rp == nullptr)
      << __FUNCTION__ << "(" << value << ") called on YBSession " << this
      << " but read point is null";
  if (rp) {
    rp->SetInTxnLimit(value);
  }
}

ConsistentReadPoint* YBSession::read_point() {
  return transaction_ ? &transaction_->read_point() : read_point_.get();
}

void YBSession::SetHybridTimeForWrite(const HybridTime ht) {
  hybrid_time_for_write_ = ht;
  if (batcher_) {
    batcher_->SetHybridTimeForWrite(hybrid_time_for_write_);
  }
}

internal::Batcher& YBSession::Batcher() {
  if (!batcher_) {
    batcher_.reset(new internal::Batcher(
        client_, error_collector_.get(), shared_from_this(), transaction_, read_point(),
        force_consistent_read_));
    if (timeout_.Initialized()) {
      batcher_->SetTimeout(timeout_);
    }
    batcher_->SetRejectionScoreSource(rejection_score_source_);
    if (hybrid_time_for_write_.is_valid()) {
      batcher_->SetHybridTimeForWrite(hybrid_time_for_write_);
    }
  }
  return *batcher_;
}

Status YBSession::CheckIfFailed() {
  return is_failed_ ? STATUS(IllegalState, "Session failed") : Status::OK();
}

Status YBSession::Apply(YBOperationPtr yb_op) {
  RETURN_NOT_OK(CheckIfFailed());
  Status s = Batcher().Add(yb_op);
  if (!PREDICT_FALSE(s.ok())) {
    is_failed_ = true;
    error_collector_->AddError(yb_op, s);
    return s;
  }

  return Status::OK();
}

Status YBSession::ApplyAndFlush(YBOperationPtr yb_op) {
  RETURN_NOT_OK(Apply(std::move(yb_op)));

  return Flush();
}

Status YBSession::Apply(const std::vector<YBOperationPtr>& ops) {
  RETURN_NOT_OK(CheckIfFailed());
  auto& batcher = Batcher();
  for (const auto& op : ops) {
    Status s = batcher.Add(op);
    if (!PREDICT_FALSE(s.ok())) {
      is_failed_ = true;
      error_collector_->AddError(op, s);
      return s;
    }
  }
  return Status::OK();
}

Status YBSession::ApplyAndFlush(
    const std::vector<YBOperationPtr>& ops, VerifyResponse verify_response) {
  RETURN_NOT_OK(Apply(ops));
  RETURN_NOT_OK(Flush());

  if (verify_response) {
    for (const auto& op : ops) {
      if (!op->succeeded()) {
        return STATUS_FORMAT(RuntimeError, "Operation failed: ", op);
      }
    }
  }

  return Status::OK();
}

int YBSession::CountBufferedOperations() const {
  return batcher_ ? batcher_->CountBufferedOperations() : 0;
}

bool YBSession::HasPendingOperations() const {
  if (batcher_ && batcher_->HasPendingOperations()) {
    return true;
  }
  std::lock_guard<simple_spinlock> l(lock_);
  for (const auto& b : flushed_batchers_) {
    if (b->HasPendingOperations()) {
      return true;
    }
  }
  return false;
}

int YBSession::CountPendingErrors() const {
  return error_collector_->CountErrors();
}

CollectedErrors YBSession::GetAndClearPendingErrors() {
  return error_collector_->GetAndClearErrors();
}

void YBSession::SetForceConsistentRead(ForceConsistentRead value) {
  force_consistent_read_ = value;
  if (batcher_) {
    batcher_->SetForceConsistentRead(value);
  }
}

} // namespace client
} // namespace yb
