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

#ifndef YB_TABLET_CLEANUP_ABORTS_TASK_H
#define YB_TABLET_CLEANUP_ABORTS_TASK_H

#include "yb/rpc/strand.h"

#include "yb/common/transaction.h"

#include "yb/tablet/tablet_fwd.h"

namespace yb {
namespace tablet {

// Removes intents for specified transaction ids.
// Transaction should be previously aborted, if transaction was committed, then it is ignored.
class CleanupAbortsTask : public rpc::StrandTask {
 public:
  CleanupAbortsTask(TransactionIntentApplier* applier,
                    TransactionIdSet&& transactions_to_cleanup,
                    TransactionParticipantContext* participant_context,
                    TransactionStatusManager* status_manager,
                    const std::string& log_prefix);

  void Prepare(std::shared_ptr<CleanupAbortsTask> cleanup_task);
  void Run() override;
  void Done(const Status& status) override;

  virtual ~CleanupAbortsTask();

 private:
  const std::string& LogPrefix();
  void FilterTransactions();

  TransactionIntentApplier* applier_;
  TransactionIdSet transactions_to_cleanup_;
  TransactionParticipantContext& participant_context_;
  TransactionStatusManager& status_manager_;
  const std::string& log_prefix_;
  std::shared_ptr<CleanupAbortsTask> retain_self_;
  std::mutex mutex_;
  std::condition_variable cond_;
  std::vector<TransactionId> erased_transactions_;
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_CLEANUP_ABORTS_TASK_H
