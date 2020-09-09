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

#ifndef YB_TABLET_CLEANUP_INTENTS_TASK_H
#define YB_TABLET_CLEANUP_INTENTS_TASK_H

#include "yb/common/transaction.h"

#include "yb/rpc/strand.h"

#include "yb/tablet/tablet_fwd.h"

namespace yb {
namespace tablet {

// Used by TransactionParticipant to remove intents of specified transaction.
class CleanupIntentsTask : public rpc::StrandTask {
 public:
  CleanupIntentsTask(
      TransactionParticipantContext* participant_context, TransactionIntentApplier* applier,
      const TransactionId& id);

  void Prepare(std::shared_ptr<CleanupIntentsTask> self);

  void Run() override;

  void Done(const Status& status) override;

  virtual ~CleanupIntentsTask() = default;

 private:
  TransactionParticipantContext& participant_context_;
  TransactionIntentApplier& applier_;
  TransactionId id_;
  std::shared_ptr<CleanupIntentsTask> retain_self_;
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_CLEANUP_INTENTS_TASK_H
