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

#include "yb/tablet/operations/update_txn_operation.h"

#include "yb/consensus/consensus.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/transaction_coordinator.h"

#include "yb/util/scope_exit.h"

using namespace std::literals;

namespace yb {
namespace tablet {

void UpdateTxnOperationState::UpdateRequestFromConsensusRound() {
  VLOG_WITH_PREFIX(2) << "UpdateRequestFromConsensusRound";
  UseRequest(&consensus_round()->replicate_msg()->transaction_state());
}

consensus::ReplicateMsgPtr UpdateTxnOperation::NewReplicateMsg() {
  auto result = std::make_shared<consensus::ReplicateMsg>();
  result->set_op_type(consensus::UPDATE_TRANSACTION_OP);
  *result->mutable_transaction_state() = *state()->request();
  return result;
}

Status UpdateTxnOperation::Prepare() {
  VLOG_WITH_PREFIX(2) << "Prepare";
  return Status::OK();
}

void UpdateTxnOperation::DoStart() {
  VLOG_WITH_PREFIX(2) << "DoStart";

  HybridTime ht = state()->hybrid_time_even_if_unset();
  bool was_valid = ht.is_valid();
  if (!was_valid) {
    // Add only leader operation here, since follower operations are already registered in MVCC,
    // as soon as they received.
    state()->tablet()->mvcc_manager()->AddPending(&ht);
    state()->set_hybrid_time(ht);
  }
}

TransactionCoordinator& UpdateTxnOperation::transaction_coordinator() const {
  return *state()->tablet()->transaction_coordinator();
}

Status UpdateTxnOperation::DoReplicated(int64_t leader_term, Status* complete_status) {
  VLOG_WITH_PREFIX(2) << "Replicated";

  auto* state = this->state();
  auto scope_exit = ScopeExit([state] {
    state->tablet()->mvcc_manager()->Replicated(state->hybrid_time());
  });

  auto transaction_participant = state->tablet()->transaction_participant();
  if (transaction_participant) {
    TransactionParticipant::ReplicatedData data = {
        .leader_term = leader_term,
        .state = *state->request(),
        .op_id = state->op_id(),
        .hybrid_time = state->hybrid_time(),
        .sealed = state->request()->sealed(),
        .already_applied_to_regular_db = AlreadyAppliedToRegularDB::kFalse
    };
    return transaction_participant->ProcessReplicated(data);
  } else {
    TransactionCoordinator::ReplicatedData data = {
        leader_term,
        *state->request(),
        state->op_id(),
        state->hybrid_time()
    };
    return transaction_coordinator().ProcessReplicated(data);
  }
}

string UpdateTxnOperation::ToString() const {
  return Format("UpdateTxnOperation { state: $0 }", *state());
}

Status UpdateTxnOperation::DoAborted(const Status& status) {
  auto hybrid_time = state()->hybrid_time_even_if_unset();
  if (hybrid_time.is_valid()) {
    state()->tablet()->mvcc_manager()->Aborted(hybrid_time);
  }

  if (state()->tablet()->transaction_coordinator()) {
    LOG_WITH_PREFIX(INFO) << "Aborted";
    TransactionCoordinator::AbortedData data = {
      *state()->request(),
      state()->op_id(),
    };
    transaction_coordinator().ProcessAborted(data);
  }

  return status;
}

} // namespace tablet
} // namespace yb
