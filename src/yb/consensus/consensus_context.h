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

#ifndef YB_CONSENSUS_CONSENSUS_CONTEXT_H
#define YB_CONSENSUS_CONSENSUS_CONTEXT_H

#include "yb/common/common_fwd.h"

#include "yb/consensus/consensus_fwd.h"

#include "yb/util/status.h"

namespace yb {
namespace consensus {

class ConsensusContext {
 public:
  // Factory for replica transactions.
  // An implementation of this factory must be registered prior to consensus
  // start, and is used to create transactions when the consensus implementation receives
  // messages from the leader.
  //
  // Replica transactions execute the following way:
  //
  // - When a ReplicateMsg is first received from the leader, the Consensus
  //   instance creates the ConsensusRound and calls StartReplicaOperation().
  //   This will trigger the Prepare(). At the same time replica consensus
  //   instance immediately stores the ReplicateMsg in the Log. Once the replicate
  //   message is stored in stable storage an ACK is sent to the leader (i.e. the
  //   replica Consensus instance does not wait for Prepare() to finish).
  virtual CHECKED_STATUS StartReplicaOperation(
      const ConsensusRoundPtr& context, HybridTime propagated_safe_time) = 0;

  virtual void SetPropagatedSafeTime(HybridTime ht) = 0;

  virtual bool ShouldApplyWrite() = 0;

  // Performs steps to prepare request for peer.
  // For instance it could enqueue some operations to the Raft.
  //
  // Returns the current safe time, so we can send it from leaders to followers.
  virtual HybridTime PreparePeerRequest() = 0;

  // This is called every time majority-replicated watermarks (OpId / leader leases) change. This is
  // used for updating the "propagated safe time" value in MvccManager and unblocking readers
  // waiting for it to advance.
  virtual void MajorityReplicated() = 0;

  // This is called every time the Raft config was changed and replicated.
  // This is used to notify the higher layer about the config change. Currently it's
  // needed to update the internal flag in the MvccManager to return a correct safe
  // time value for a read/write operation in case of RF==1 mode.
  virtual void ChangeConfigReplicated(const RaftConfigPB& config) = 0;

  // See DB::GetCurrentVersionNumSSTFiles
  virtual uint64_t NumSSTFiles() = 0;

  // Register listener that will be invoked when number of SST files changed.
  // Listener could be set only once and then reset.
  virtual void ListenNumSSTFilesChanged(std::function<void()> listener) = 0;

  virtual ~ConsensusContext() = default;
};

} // namespace consensus
} // namespace yb

#endif // YB_CONSENSUS_CONSENSUS_CONTEXT_H
