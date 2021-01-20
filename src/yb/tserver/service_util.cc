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

#include "yb/tserver/service_util.h"

#include "yb/consensus/consensus.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metrics.h"

#include "yb/tserver/tserver_error.h"

namespace yb {
namespace tserver {

void SetupErrorAndRespond(TabletServerErrorPB* error,
                          const Status& s,
                          TabletServerErrorPB::Code code,
                          rpc::RpcContext* context) {
  // Generic "service unavailable" errors will cause the client to retry later.
  if (code == TabletServerErrorPB::UNKNOWN_ERROR) {
    if (s.IsServiceUnavailable()) {
      TabletServerDelay delay(s);
      if (!delay.value().Initialized()) {
        context->RespondRpcFailure(rpc::ErrorStatusPB::ERROR_SERVER_TOO_BUSY, s);
        return;
      }
    }
    consensus::ConsensusError consensus_error(s);
    if (consensus_error.value() == consensus::ConsensusErrorPB::TABLET_SPLIT) {
      code = TabletServerErrorPB::TABLET_SPLIT;
    }
  }

  StatusToPB(s, error->mutable_status());
  error->set_code(code);
  context->RespondSuccess();
}

void SetupErrorAndRespond(TabletServerErrorPB* error,
                          const Status& s,
                          rpc::RpcContext* context) {
  auto ts_error = TabletServerError::FromStatus(s);
  SetupErrorAndRespond(
      error, s, ts_error ? ts_error->value() : TabletServerErrorPB::UNKNOWN_ERROR, context);
}

Result<int64_t> LeaderTerm(const tablet::TabletPeer& tablet_peer) {
  std::shared_ptr<consensus::Consensus> consensus = tablet_peer.shared_consensus();
  if (!consensus) {
    auto state = tablet_peer.state();
    if (state != tablet::RaftGroupStatePB::SHUTDOWN) {
      // Should not happen.
      return STATUS(IllegalState, "Tablet peer does not have consensus, but in $0 state",
                    tablet::RaftGroupStatePB_Name(state));
    }
    return STATUS(Aborted, "Tablet peer was closed");
  }
  auto leader_state = consensus->GetLeaderState();

  VLOG(1) << Format(
      "Check for tablet $0 peer $1. Peer role is $2. Leader status is $3.",
      tablet_peer.tablet_id(), tablet_peer.permanent_uuid(),
      consensus->role(), to_underlying(leader_state.status));

  if (!leader_state.ok()) {
    typedef consensus::LeaderStatus LeaderStatus;
    auto status = leader_state.CreateStatus();
    switch (leader_state.status) {
      case LeaderStatus::NOT_LEADER: FALLTHROUGH_INTENDED;
      case LeaderStatus::LEADER_BUT_NO_MAJORITY_REPLICATED_LEASE:
        // We are returning a NotTheLeader as opposed to LeaderNotReady, because there is a chance
        // that we're a partitioned-away leader, and the client needs to do another leader lookup.
        return status.CloneAndAddErrorCode(TabletServerError(TabletServerErrorPB::NOT_THE_LEADER));
      case LeaderStatus::LEADER_BUT_NO_OP_NOT_COMMITTED: FALLTHROUGH_INTENDED;
      case LeaderStatus::LEADER_BUT_OLD_LEADER_MAY_HAVE_LEASE:
        return status.CloneAndAddErrorCode(TabletServerError(
            TabletServerErrorPB::LEADER_NOT_READY_TO_SERVE));
      case LeaderStatus::LEADER_AND_READY:
        LOG(FATAL) << "Unexpected status: " << to_underlying(leader_state.status);
    }
    FATAL_INVALID_ENUM_VALUE(LeaderStatus, leader_state.status);
  }

  return leader_state.term;
}

void LeaderTabletPeer::FillTabletPeer(TabletPeerTablet source) {
  peer = std::move(source.tablet_peer);
  tablet = std::move(source.tablet);
}

bool LeaderTabletPeer::FillTerm(TabletServerErrorPB* error, rpc::RpcContext* context) {
  auto leader_term = LeaderTerm(*peer);
  if (!leader_term.ok()) {
    auto tablet = peer->shared_tablet();
    if (tablet) {
      // It could happen that tablet becomes nullptr due to shutdown.
      tablet->metrics()->not_leader_rejections->Increment();
    }
    SetupErrorAndRespond(error, leader_term.status(), context);
    return false;
  }
  this->leader_term = *leader_term;

  return true;
}

} // namespace tserver
} // namespace yb
