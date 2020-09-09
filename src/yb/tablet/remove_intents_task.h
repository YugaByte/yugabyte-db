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

#ifndef YB_TABLET_REMOVE_INTENTS_TASK_H
#define YB_TABLET_REMOVE_INTENTS_TASK_H

#include "yb/rpc/strand.h"

#include "yb/tablet/running_transaction_context.h"

namespace yb {
namespace tablet {

// Used by RunningTransaction to remove its intents.
class RemoveIntentsTask : public rpc::StrandTask {
 public:
  RemoveIntentsTask(TransactionIntentApplier* applier,
                    TransactionParticipantContext* participant_context,
                    RunningTransactionContext* running_transaction_context,
                    const TransactionId& id);

  bool Prepare(RunningTransactionPtr transaction);
  void Run() override;
  void Done(const Status& status) override;

  virtual ~RemoveIntentsTask() = default;

 private:
  std::string LogPrefix() const;

  TransactionIntentApplier& applier_;
  TransactionParticipantContext& participant_context_;
  RunningTransactionContext& running_transaction_context_;
  TransactionId id_;
  std::atomic<bool> used_{false};
  RunningTransactionPtr transaction_;
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_REMOVE_INTENTS_TASK_H
