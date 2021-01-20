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

#ifndef YB_TABLET_RUNNING_TRANSACTION_H
#define YB_TABLET_RUNNING_TRANSACTION_H

#include <memory>

#include "yb/docdb/docdb.h"

#include "yb/tablet/apply_intents_task.h"
#include "yb/tablet/remove_intents_task.h"
#include "yb/tablet/transaction_participant.h"

#include "yb/tserver/tserver_service.pb.h"

#include "yb/util/bitmap.h"

namespace yb {
namespace tablet {

YB_DEFINE_ENUM(UpdateAbortCheckHTMode, (kStatusRequestSent)(kStatusResponseReceived));

// Represents transaction running at transaction participant.
class RunningTransaction : public std::enable_shared_from_this<RunningTransaction> {
 public:
  RunningTransaction(TransactionMetadata metadata,
                     const TransactionalBatchData& last_batch_data,
                     OneWayBitmap&& replicated_batches,
                     HybridTime base_time_for_abort_check_ht_calculation,
                     RunningTransactionContext* context);

  ~RunningTransaction();

  const TransactionId& id() const {
    return metadata_.transaction_id;
  }

  HybridTime start_ht() const {
    return metadata_.start_time;
  }

  HybridTime abort_check_ht() const {
    return abort_check_ht_;
  }

  MUST_USE_RESULT bool UpdateStatus(
      TransactionStatus transaction_status, HybridTime time_of_status);

  void UpdateAbortCheckHT(HybridTime now, UpdateAbortCheckHTMode mode);

  const TransactionMetadata& metadata() const {
    return metadata_;
  }

  const TransactionalBatchData& last_batch_data() const {
    return last_batch_data_;
  }

  size_t num_replicated_batches() const {
    return replicated_batches_.CountSet();
  }

  const OneWayBitmap& replicated_batches() const {
    return replicated_batches_;
  }

  HybridTime local_commit_time() const {
    return local_commit_time_;
  }

  void SetLocalCommitTime(HybridTime time);
  void AddReplicatedBatch(
      size_t batch_idx, boost::container::small_vector_base<uint8_t>* encoded_replicated_batches);
  void BatchReplicated(const TransactionalBatchData& value);
  void RequestStatusAt(const StatusRequest& request,
                       std::unique_lock<std::mutex>* lock);
  bool WasAborted() const;
  CHECKED_STATUS CheckAborted() const;
  void Aborted();

  void Abort(client::YBClient* client,
             TransactionStatusCallback callback,
             std::unique_lock<std::mutex>* lock);

  std::string ToString() const;
  void ScheduleRemoveIntents(const RunningTransactionPtr& shared_self);

  // Sets apply state for this transaction.
  // If data is not null, then apply intents task will be initiated if was not previously started.
  void SetApplyData(const docdb::ApplyTransactionState& apply_state,
                    const TransactionApplyData* data = nullptr);

  // Whether this transactions is currently applying intents.
  bool ProcessingApply() const;

  std::string LogPrefix() const;

 private:
  static boost::optional<TransactionStatus> GetStatusAt(
      HybridTime time,
      HybridTime last_known_status_hybrid_time,
      TransactionStatus last_known_status);

  void SendStatusRequest(int64_t serial_no, const RunningTransactionPtr& shared_self);

  void StatusReceived(const Status& status,
                      const tserver::GetTransactionStatusResponsePB& response,
                      int64_t serial_no,
                      const RunningTransactionPtr& shared_self);

  void DoStatusReceived(const Status& status,
                        const tserver::GetTransactionStatusResponsePB& response,
                        int64_t serial_no,
                        const RunningTransactionPtr& shared_self);

  // Extracts status waiters from status_waiters_ that could be notified at this point.
  // Extracted waiters also removed from status_waiters_.
  std::vector<StatusRequest> ExtractFinishedStatusWaitersUnlocked(
      int64_t serial_no, HybridTime time_of_status, TransactionStatus transaction_status);

  // Notify provided status waiters.
  void NotifyWaiters(int64_t serial_no, HybridTime time_of_status,
                     TransactionStatus transaction_status,
                     const std::vector<StatusRequest>& status_waiters);

  static Result<TransactionStatusResult> MakeAbortResult(
      const Status& status,
      const tserver::AbortTransactionResponsePB& response);

  void AbortReceived(const Status& status,
                     const tserver::AbortTransactionResponsePB& response,
                     const RunningTransactionPtr& shared_self);

  TransactionMetadata metadata_;
  TransactionalBatchData last_batch_data_;
  OneWayBitmap replicated_batches_;
  RunningTransactionContext& context_;
  RemoveIntentsTask remove_intents_task_;
  HybridTime local_commit_time_ = HybridTime::kInvalid;

  TransactionStatus last_known_status_ = TransactionStatus::CREATED;
  HybridTime last_known_status_hybrid_time_ = HybridTime::kMin;
  std::vector<StatusRequest> status_waiters_;
  rpc::Rpcs::Handle get_status_handle_;
  rpc::Rpcs::Handle abort_handle_;
  std::vector<TransactionStatusCallback> abort_waiters_;

  TransactionApplyData apply_data_;
  docdb::ApplyTransactionState apply_state_;
  ApplyIntentsTask apply_intents_task_;

  // Time of the next check whether this transaction has been aborted.
  HybridTime abort_check_ht_;
};

CHECKED_STATUS MakeAbortedStatus(const TransactionId& id);

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_RUNNING_TRANSACTION_H
