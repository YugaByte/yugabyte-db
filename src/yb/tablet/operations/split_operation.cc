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

#include "yb/tablet/operations/split_operation.h"

#include "yb/consensus/consensus.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/tablet_splitter.h"

using namespace std::literals;

namespace yb {
namespace tablet {

SplitOperationState::SplitOperationState(
    Tablet* tablet, consensus::RaftConsensus* consensus_for_abort,
    TabletSplitter* tablet_splitter, const tserver::SplitTabletRequestPB* request)
    : OperationState(tablet),
      consensus_(consensus_for_abort),
      tablet_splitter_(tablet_splitter),
      request_(request) {
  if (!tablet_splitter) {
    LOG(FATAL) << "tablet_splitter is not set for tablet " << tablet->tablet_id();
  }
}

std::string SplitOperationState::ToString() const {
  auto req = request();
  return Format("SplitOperationState [$0]", !req ? "(none)"s : req->ShortDebugString());
}

void SplitOperationState::UpdateRequestFromConsensusRound() {
  request_ = consensus_round()->replicate_msg()->mutable_split_request();
}

consensus::ReplicateMsgPtr SplitOperation::NewReplicateMsg() {
  auto result = std::make_shared<consensus::ReplicateMsg>();
  result->set_op_type(consensus::SPLIT_OP);
  *result->mutable_split_request() = *state()->request();
  return result;
}

Status SplitOperation::Prepare() {
  VLOG_WITH_PREFIX(2) << "Prepare";
  return Status::OK();
}

void SplitOperation::DoStart() {
  VLOG_WITH_PREFIX(2) << "DoStart";
  state()->TrySetHybridTimeFromClock();
}

Status SplitOperation::DoAborted(const Status& status) {
  VLOG_WITH_PREFIX(2) << "DoAborted";
  return status;
}

Status SplitOperation::DoReplicated(int64_t leader_term, Status* complete_status) {
  VLOG_WITH_PREFIX(2) << "Apply";
  return state()->tablet_splitter().ApplyTabletSplit(state());
}

std::string SplitOperation::ToString() const {
  return Format("SplitOperation { state: $0 }", *state());
}

}  // namespace tablet
}  // namespace yb
