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

#include "yb/tablet/remove_intents_task.h"

#include "yb/tablet/running_transaction.h"

namespace yb {
namespace tablet {

RemoveIntentsTask::RemoveIntentsTask(TransactionIntentApplier* applier,
                                     TransactionParticipantContext* participant_context,
                                     RunningTransactionContext* running_transaction_context,
                                     const TransactionId& id)
    : applier_(*applier), participant_context_(*participant_context),
      running_transaction_context_(*running_transaction_context), id_(id) {}

bool RemoveIntentsTask::Prepare(RunningTransactionPtr transaction) {
  bool expected = false;
  if (!used_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return false;
  }

  transaction_ = std::move(transaction);
  LOG_IF_WITH_PREFIX(DFATAL, transaction_->ProcessingApply())
      << "Remove intents for transaction that is processing apply";
  return true;
}

void RemoveIntentsTask::Run() {
  VLOG_WITH_PREFIX(2) << "Remove intents";

  RemoveIntentsData data;
  participant_context_.GetLastReplicatedData(&data);
  auto status = applier_.RemoveIntents(data, id_);
  LOG_IF_WITH_PREFIX(WARNING, !status.ok())
      << "Failed to remove intents of aborted transaction : " << status;
  VLOG_WITH_PREFIX(2) << "Removed intents";
}

void RemoveIntentsTask::Done(const Status& status) {
  WARN_NOT_OK(status, "Remove intents task failed");
  transaction_.reset();
}

std::string RemoveIntentsTask::LogPrefix() const {
  return transaction_ ? transaction_->LogPrefix() : running_transaction_context_.LogPrefix();
}

} // namespace tablet
} // namespace yb
