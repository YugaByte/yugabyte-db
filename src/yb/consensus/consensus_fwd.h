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

#ifndef YB_CONSENSUS_CONSENSUS_FWD_H
#define YB_CONSENSUS_CONSENSUS_FWD_H

#include "yb/gutil/ref_counted.h"
#include "yb/util/enums.h"
#include "yb/util/strongly_typed_bool.h"

namespace yb {
namespace consensus {

class Consensus;
class ConsensusContext;
class PeerProxyFactory;
class PeerMessageQueue;
class RaftConfigPB;
class RaftConsensus;
class ReplicateMsg;
class ReplicateMsgsHolder;
class RetryableRequests;
class SafeOpIdWaiter;
class VoteRequestPB;
class VoteResponsePB;

struct ConsensusOptions;
struct ConsensusBootstrapInfo;
struct LeaderState;
struct ReadOpsResult;
struct RetryableRequestsCounts;
struct StateChangeContext;

class ConsensusRound;
typedef scoped_refptr<ConsensusRound> ConsensusRoundPtr;
typedef std::vector<ConsensusRoundPtr> ConsensusRounds;

class ConsensusServiceProxy;
typedef std::unique_ptr<ConsensusServiceProxy> ConsensusServiceProxyPtr;

class LeaderElection;
typedef scoped_refptr<LeaderElection> LeaderElectionPtr;

class PeerProxy;
typedef std::unique_ptr<PeerProxy> PeerProxyPtr;

struct LeaderElectionData;

// The elected Leader (this peer) can be in not-ready state because it's not yet synced.
// The state reflects the real leader status: not-leader, leader-not-ready, leader-ready.
// Not-ready status means that the leader is not ready to serve up-to-date read requests.
YB_DEFINE_ENUM(
    LeaderStatus,
    (NOT_LEADER)
    (LEADER_BUT_NO_OP_NOT_COMMITTED)
    (LEADER_BUT_OLD_LEADER_MAY_HAVE_LEASE)
    (LEADER_BUT_NO_MAJORITY_REPLICATED_LEASE)
    (LEADER_AND_READY));

typedef int64_t ConsensusTerm;

typedef std::shared_ptr<ReplicateMsg> ReplicateMsgPtr;
typedef std::vector<ReplicateMsgPtr> ReplicateMsgs;

YB_STRONGLY_TYPED_BOOL(TEST_SuppressVoteRequest);
YB_STRONGLY_TYPED_BOOL(PreElection);

} // namespace consensus
} // namespace yb

#endif // YB_CONSENSUS_CONSENSUS_FWD_H
