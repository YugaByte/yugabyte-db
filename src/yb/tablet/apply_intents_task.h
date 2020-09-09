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

#ifndef YB_TABLET_APPLY_INTENTS_TASK_H
#define YB_TABLET_APPLY_INTENTS_TASK_H

#include "yb/rpc/strand.h"

#include "yb/tablet/running_transaction_context.h"

namespace yb {
namespace tablet {

// Used by RunningTransaction to apply its intents.
class ApplyIntentsTask : public rpc::StrandTask {
 public:
  ApplyIntentsTask(TransactionIntentApplier* applier,
                   RunningTransactionContext* running_transaction_context,
                   const TransactionApplyData* apply_data);

  // Returns true if task was successfully prepared and could be submitted to the thread pool.
  bool Prepare(RunningTransactionPtr transaction);
  void Run() override;
  void Done(const Status& status) override;

  virtual ~ApplyIntentsTask() = default;

 private:
  std::string LogPrefix() const;

  TransactionIntentApplier& applier_;
  RunningTransactionContext& running_transaction_context_;
  const TransactionApplyData& apply_data_;

  // Whether this task was already in use or not.
  // Helps to avoid multiple submissions of the same task.
  // The task can be submitted only once, so this flag never reverts its state to false.
  std::atomic<bool> used_{false};
  RunningTransactionPtr transaction_;
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_APPLY_INTENTS_TASK_H
