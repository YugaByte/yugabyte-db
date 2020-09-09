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

#include "yb/tablet/cleanup_aborts_task.h"

#include "yb/tablet/transaction_participant.h"

using namespace std::literals;

DECLARE_uint64(aborted_intent_cleanup_ms);

namespace yb {
namespace tablet {

CleanupAbortsTask::CleanupAbortsTask(TransactionIntentApplier* applier,
                                     TransactionIdSet&& transactions_to_cleanup,
                                     TransactionParticipantContext* participant_context,
                                     TransactionStatusManager* status_manager,
                                     const std::string& log_prefix)
    : applier_(applier), transactions_to_cleanup_(std::move(transactions_to_cleanup)),
      participant_context_(*participant_context),
      status_manager_(*status_manager),
      log_prefix_(log_prefix) {}

void CleanupAbortsTask::Prepare(std::shared_ptr<CleanupAbortsTask> cleanup_task) {
  retain_self_ = std::move(cleanup_task);
}

void CleanupAbortsTask::Run() {
  size_t initial_number_of_transactions = transactions_to_cleanup_.size();

  FilterTransactions();

  if (transactions_to_cleanup_.empty()) {
    LOG_WITH_PREFIX(INFO)
        << "Nothing to cleanup of " << initial_number_of_transactions << " transactions";
    return;
  }

  // Update now to reflect time on the coordinator
  auto now = participant_context_.Now();

  // The calls to RequestStatusAt would have updated the local clock of the participant.
  // Wait for the propagated time to reach the current hybrid time.
  const MonoDelta kMaxTotalSleep = 10s;
  auto safetime = applier_->ApplierSafeTime(now, CoarseMonoClock::now() + kMaxTotalSleep);
  if (!safetime) {
    LOG_WITH_PREFIX(WARNING) << "Tablet application did not catch up in: " << kMaxTotalSleep;
    return;
  }

  for (const TransactionId& transaction_id : transactions_to_cleanup_) {
    // If transaction is committed, no action required
    // TODO(dtxn) : Do batch processing of transactions,
    // because LocalCommitTime will acquire lock per each call.
    auto commit_time = status_manager_.LocalCommitTime(transaction_id);
    if (commit_time.is_valid()) {
      transactions_to_cleanup_.erase(transaction_id);
    }
  }

  RemoveIntentsData data;
  participant_context_.GetLastReplicatedData(&data);
  WARN_NOT_OK(applier_->RemoveIntents(data, transactions_to_cleanup_),
              "RemoveIntents for transaction cleanup in compaction failed.");
  LOG_WITH_PREFIX(INFO)
      << "Number of aborted transactions cleaned up: " << transactions_to_cleanup_.size()
      << " of " << initial_number_of_transactions;
}

void CleanupAbortsTask::Done(const Status& status) {
  transactions_to_cleanup_.clear();
  retain_self_ = nullptr;
}

CleanupAbortsTask::~CleanupAbortsTask() {
  LOG_WITH_PREFIX(INFO) << "Cleanup Aborts Task finished.";
}

const std::string& CleanupAbortsTask::LogPrefix() {
  return log_prefix_;
}

void CleanupAbortsTask::FilterTransactions() {
  size_t left_wait = transactions_to_cleanup_.size();
  std::unique_lock<std::mutex> lock(mutex_);

  auto now = participant_context_.Now();
  auto tid = std::this_thread::get_id();

  for (const TransactionId& transaction_id : transactions_to_cleanup_) {
    VLOG_WITH_PREFIX(1) << "Checking if transaction needs to be cleaned up: " << transaction_id;

    // If transaction is committed, no action required
    auto commit_time = status_manager_.LocalCommitTime(transaction_id);
    if (commit_time.is_valid()) {
      auto warning_time = commit_time.AddMilliseconds(FLAGS_aborted_intent_cleanup_ms);
      if (now >= warning_time) {
        LOG_WITH_PREFIX(WARNING)
            << "Transaction Id: " << transaction_id << " committed too long ago.";
      }
      VLOG_WITH_PREFIX(2) << "Transaction committed, should not cleanup: " << transaction_id;
      erased_transactions_.push_back(transaction_id);
      --left_wait;
      continue;
    }

    static const std::string kRequestReason = "cleanup"s;
    // Get transaction status
    StatusRequest request = {
        &transaction_id,
        now,
        now,
        0, // serial no. Could use 0 here, because read_ht == global_limit_ht.
        // So we cannot accept status with time >= read_ht and < global_limit_ht.
        &kRequestReason,
        TransactionLoadFlags{},
        [transaction_id, this, &left_wait, tid](Result<TransactionStatusResult> result) {
          std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
          if (tid != std::this_thread::get_id()) {
            lock.lock();
          }
          // Best effort
          // Status of abort will result in cleanup of intents
          if ((result.ok() && result->status == TransactionStatus::ABORTED) ||
              (!result.ok() && result.status().IsNotFound())) {
            VLOG_WITH_PREFIX(3) << "Transaction being cleaned " << transaction_id << ".";
          } else {
            this->erased_transactions_.push_back(transaction_id);
            VLOG_WITH_PREFIX(2) << "Transaction not aborted, should not cleanup: "
                                << transaction_id << ", " << result;
          }
          if (--left_wait == 0) {
            cond_.notify_one();
          }
        }
    };
    status_manager_.RequestStatusAt(request);
  }

  cond_.wait(lock, [&left_wait] { return left_wait == 0; });

  for (const auto& transaction_id : erased_transactions_) {
    transactions_to_cleanup_.erase(transaction_id);
  }
}

} // namespace tablet
} // namespace yb
