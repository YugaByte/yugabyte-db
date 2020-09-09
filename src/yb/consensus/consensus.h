// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
#ifndef YB_CONSENSUS_CONSENSUS_H_
#define YB_CONSENSUS_CONSENSUS_H_

#include <iosfwd>
#include <ostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional_fwd.hpp>

#include "yb/common/hybrid_time.h"

#include "yb/consensus/consensus_fwd.h"
#include "yb/consensus/consensus_types.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/opid_util.h"

#include "yb/gutil/callback.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/util/enums.h"
#include "yb/util/monotime.h"
#include "yb/util/opid.h"
#include "yb/util/status.h"
#include "yb/util/status_callback.h"
#include "yb/util/strongly_typed_bool.h"

namespace yb {

namespace log {
class Log;
}

namespace master {
class SysCatalogTable;
}

namespace server {
class Clock;
}

namespace tablet {
class TabletPeer;
}

namespace tserver {
class TabletServerErrorPB;
}

namespace consensus {

typedef int64_t ConsensusTerm;

typedef std::function<void(
    const Status& status, int64_t leader_term, OpIds* applied_op_ids)>
        ConsensusReplicatedCallback;

// After completing bootstrap, some of the results need to be plumbed through
// into the consensus implementation.
struct ConsensusBootstrapInfo {
  ConsensusBootstrapInfo();

  // The id of the last operation in the log
  OpIdPB last_id;

  // The id of the last committed operation in the log.
  OpIdPB last_committed_id;

  // The id of the split operation designated for this tablet added to Raft log.
  // See comments for ReplicateState::split_op_id_.
  OpIdPB split_op_id;

  // REPLICATE messages which were in the log with no accompanying
  // COMMIT. These need to be passed along to consensus init in order
  // to potentially commit them.
  //
  // These are owned by the ConsensusBootstrapInfo instance.
  ReplicateMsgs orphaned_replicates;

 private:
  DISALLOW_COPY_AND_ASSIGN(ConsensusBootstrapInfo);
};

struct LeaderState;

// Mode is orthogonal to pre-elections, so any combination could be used.
YB_DEFINE_ENUM(ElectionMode,
    // A normal leader election. Peers will not vote for this node
    // if they believe that a leader is alive.
    (NORMAL_ELECTION)
    // In this mode, peers will vote for this candidate even if they
    // think a leader is alive. This can be used for a faster hand-off
    // between a leader and one of its replicas.
    (ELECT_EVEN_IF_LEADER_IS_ALIVE));

// Arguments for StartElection.
struct LeaderElectionData {
  ElectionMode mode;

  // pending_commit - we should start election only after we have specified entry committed.
  const bool pending_commit = false;

  // must_be_committed_opid - only matters if pending_commit is true.
  //    If this is specified, we would wait until this entry is committed. If not specified
  //    (i.e. if this has the default OpId value) it is taken from the last call to StartElection
  //    with pending_commit = true.
  OpIdPB must_be_committed_opid = OpIdPB::default_instance();

  // originator_uuid - if election is initiated by an old leader as part of a stepdown procedure,
  //    this would contain the uuid of the old leader.
  std::string originator_uuid = std::string();

  TEST_SuppressVoteRequest suppress_vote_request = TEST_SuppressVoteRequest::kFalse;

  std::string ToString() const;
};

// The external interface for a consensus peer.
//
// Note: Even though Consensus points to Log, it needs to be destroyed
// after it. See Log class header comment for the reason why. On the other
// hand Consensus must be quiesced before closing the log, otherwise it
// will try to write to a destroyed/closed log.
//
// The order of these operations on shutdown must therefore be:
// 1 - quiesce Consensus
// 2 - close/destroy Log
// 3 - destroy Consensus
class Consensus {
 public:
  class ConsensusFaultHooks;

  Consensus() {}
  virtual ~Consensus() {}

  // Starts running the consensus algorithm.
  virtual CHECKED_STATUS Start(const ConsensusBootstrapInfo& info) = 0;

  // Returns true if consensus is running.
  virtual bool IsRunning() const = 0;

  // Emulates a leader election by simply making this peer leader.
  virtual CHECKED_STATUS EmulateElection() = 0;

  virtual CHECKED_STATUS StartElection(const LeaderElectionData& data) = 0;

  // We tried to step down, so you protege become leader.
  // But it failed to win election, so we should reset our withhold time and try to reelect ourself.
  // election_lost_by_uuid - uuid of protege that lost election.
  virtual CHECKED_STATUS ElectionLostByProtege(const std::string& election_lost_by_uuid) = 0;

  // Implement a LeaderStepDown() request.
  virtual CHECKED_STATUS StepDown(const LeaderStepDownRequestPB* req,
                                  LeaderStepDownResponsePB* resp) {
    return STATUS(NotSupported, "Not implemented.");
  }

  // Wait until the node has LEADER role.
  // Returns Status::TimedOut if the role is not LEADER within 'timeout'.
  virtual CHECKED_STATUS WaitUntilLeaderForTests(const MonoDelta& timeout) = 0;

  // Creates a new ConsensusRound, the entity that owns all the data
  // structures required for a consensus round, such as the ReplicateMsg.
  // ConsensusRound will also point to and increase the reference count for the provided callbacks.
  ConsensusRoundPtr NewRound(
      ReplicateMsgPtr replicate_msg,
      const ConsensusReplicatedCallback& replicated_cb);

  // Called by a Leader to replicate an entry to the state machine.
  //
  // From the leader instance perspective execution proceeds as follows:
  //
  //           Leader                               RaftConfig
  //             +                                     +
  //     1) Req->| Replicate()                         |
  //             |                                     |
  //     2)      +-------------replicate-------------->|
  //             |<---------------ACK------------------+
  //             |                                     |
  //     3)      +--+                                  |
  //           <----+ round.NotifyReplicationFinished()|
  //             |                                     |
  //     3a)     |  +------ update commitIndex ------->|
  //             |                                     |
  //
  // 1) Caller calls Replicate(), method returns immediately to the caller and
  //    runs asynchronously.
  //
  // 2) Leader replicates the entry to the peers using the consensus
  //    algorithm, proceeds as soon as a majority of voters acknowledges the
  //    entry.
  //
  // 3) Leader defers to the caller by calling ConsensusRound::NotifyReplicationFinished,
  //    which calls the ConsensusReplicatedCallback.
  //
  // 3a) The leader asynchronously notifies other peers of the new
  //     commit index, which tells them to apply the operation.
  //
  // This method can only be called on the leader, i.e. role() == LEADER

  virtual CHECKED_STATUS TEST_Replicate(const ConsensusRoundPtr& round) = 0;

  // A batch version of Replicate, which is what we try to use as much as possible for performance.
  virtual CHECKED_STATUS ReplicateBatch(ConsensusRounds* rounds) = 0;

  // Messages sent from LEADER to FOLLOWERS and LEARNERS to update their
  // state machines. This is equivalent to "AppendEntries()" in Raft
  // terminology.
  //
  // ConsensusRequestPB contains a sequence of 0 or more operations to apply
  // on the replica. If there are 0 operations the request is considered
  // 'status-only' i.e. the leader is communicating with the follower only
  // in order to pass back and forth information on watermarks (eg committed
  // operation ID, replicated op id, etc).
  //
  // If the sequence contains 1 or more operations they will be replicated
  // in the same order as the leader, and submitted for asynchronous Prepare
  // in the same order.
  //
  // The leader also provides information on the index of the latest
  // operation considered committed by consensus. The replica uses this
  // information to update the state of any pending (previously replicated/prepared)
  // transactions.
  //
  // Returns Status::OK if the response has been filled (regardless of accepting
  // or rejecting the specific request). Returns non-OK Status if a specific
  // error response could not be formed, which will result in the service
  // returning an UNKNOWN_ERROR RPC error code to the caller and including the
  // stringified Status message.
  virtual CHECKED_STATUS Update(
      ConsensusRequestPB* request,
      ConsensusResponsePB* response,
      CoarseTimePoint deadline) = 0;

  // Messages sent from CANDIDATEs to voting peers to request their vote
  // in leader election.
  virtual CHECKED_STATUS RequestVote(const VoteRequestPB* request,
                                     VoteResponsePB* response) = 0;

  // Implement a ChangeConfig() request.
  virtual CHECKED_STATUS ChangeConfig(const ChangeConfigRequestPB& req,
                                      const StdStatusCallback& client_cb,
                                      boost::optional<tserver::TabletServerErrorPB::Code>* error) {
    return STATUS(NotSupported, "Not implemented.");
  }

  // Returns the current Raft role of this instance.
  virtual RaftPeerPB::Role role() const = 0;

  // Returns the leader status (see LeaderStatus type description for details).
  // If leader is ready, then also returns term, otherwise OpId::kUnknownTerm is returned.
  //
  // allow_stale could be used to avoid refreshing cache, when we are OK to read slightly outdated
  // value.
  virtual LeaderState GetLeaderState(bool allow_stale = false) const = 0;

  LeaderStatus GetLeaderStatus(bool allow_stale = false) const;
  int64_t LeaderTerm() const;

  // Returns the uuid of this peer.
  virtual std::string peer_uuid() const = 0;

  // Returns the id of the tablet whose updates this consensus instance helps coordinate.
  virtual std::string tablet_id() const = 0;

  // Returns a copy of the committed state of the Consensus system. Also allows returning the
  // leader lease status captured under the same lock.
  virtual ConsensusStatePB ConsensusState(
      ConsensusConfigType type,
      LeaderLeaseStatus* leader_lease_status = nullptr) const = 0;

  // Returns a copy of the committed state of the Consensus system, assuming caller holds the needed
  // locks.
  virtual ConsensusStatePB ConsensusStateUnlocked(
      ConsensusConfigType type,
      LeaderLeaseStatus* leader_lease_status = nullptr) const = 0;

  // Returns a copy of the current committed Raft configuration.
  virtual RaftConfigPB CommittedConfig() const = 0;

  virtual void DumpStatusHtml(std::ostream& out) const = 0;

  void SetFaultHooks(const std::shared_ptr<ConsensusFaultHooks>& hooks);

  const std::shared_ptr<ConsensusFaultHooks>& GetFaultHooks() const;

  // Stops running the consensus algorithm.
  virtual void Shutdown() = 0;

  // Returns the last OpId (either received or committed, depending on the 'type' argument) that the
  // Consensus implementation knows about.  Primarily used for testing purposes.
  Result<yb::OpId> GetLastOpId(OpIdType type);

  virtual yb::OpId GetLastReceivedOpId() = 0;

  virtual yb::OpId GetLastCommittedOpId() = 0;

  virtual yb::OpId GetLastAppliedOpId() = 0;

  // Return the ID of the split operation requesting to split this Raft group if it has been added
  // to Raft log and uninitialized OpId otherwise.
  virtual yb::OpId GetSplitOpId() = 0;

  // Assuming we are the leader, wait until we have a valid leader lease (i.e. the old leader's
  // lease has expired, and we have replicated a new lease that has not expired yet).
  virtual CHECKED_STATUS WaitForLeaderLeaseImprecise(CoarseTimePoint deadline) = 0;

  // Check that this Consensus is a leader and has lease, returns Status::OK in this case.
  // Otherwise error status is returned.
  virtual CHECKED_STATUS CheckIsActiveLeaderAndHasLease() const = 0;

  // Returns majority replicated ht lease, so we know that after leader change
  // operations would not be added with hybrid time below this lease.
  //
  // `min_allowed` - result should be greater or equal to `min_allowed`, otherwise
  // it tries to wait until ht lease reaches this value or `deadline` happens.
  //
  // Returns 0 if timeout happened.
  virtual MicrosTime MajorityReplicatedHtLeaseExpiration(
      MicrosTime min_allowed, CoarseTimePoint deadline) const = 0;

  // Read majority replicated messages for CDC producer.
  virtual Result<ReadOpsResult> ReadReplicatedMessagesForCDC(const yb::OpId& from,
                                                             int64_t* repl_index) = 0;

  virtual void UpdateCDCConsumerOpId(const yb::OpId& op_id) = 0;

 protected:
  friend class RefCountedThreadSafe<Consensus>;
  friend class tablet::TabletPeer;
  friend class master::SysCatalogTable;


  // Fault hooks for tests. In production code this will always be null.
  std::shared_ptr<ConsensusFaultHooks> fault_hooks_;

  enum HookPoint {
    PRE_START,
    POST_START,
    PRE_CONFIG_CHANGE,
    POST_CONFIG_CHANGE,
    PRE_REPLICATE,
    POST_REPLICATE,
    PRE_COMMIT,
    POST_COMMIT,
    PRE_UPDATE,
    POST_UPDATE,
    PRE_SHUTDOWN,
    POST_SHUTDOWN
  };

  CHECKED_STATUS ExecuteHook(HookPoint point);

  enum State {
    kNotInitialized,
    kInitializing,
    kConfiguring,
    kRunning,
  };

 private:
  DISALLOW_COPY_AND_ASSIGN(Consensus);
};

YB_DEFINE_ENUM(StateChangeReason,
    (INVALID_REASON)
    (TABLET_PEER_STARTED)
    (CONSENSUS_STARTED)
    (NEW_LEADER_ELECTED)
    (FOLLOWER_NO_OP_COMPLETE)
    (LEADER_CONFIG_CHANGE_COMPLETE)
    (FOLLOWER_CONFIG_CHANGE_COMPLETE));

// Context provided for callback on master/tablet-server peer state change for post processing
// e.g., update in-memory contents.
struct StateChangeContext {

  const StateChangeReason reason;

  explicit StateChangeContext(StateChangeReason in_reason)
      : reason(in_reason) {
  }

  StateChangeContext(StateChangeReason in_reason, bool is_locked)
      : reason(in_reason),
        is_config_locked_(is_locked) {
  }

  StateChangeContext(StateChangeReason in_reason, string uuid)
      : reason(in_reason),
        new_leader_uuid(uuid) {
  }

  StateChangeContext(StateChangeReason in_reason,
                     ChangeConfigRecordPB change_rec,
                     string remove = "")
      : reason(in_reason),
        change_record(change_rec),
        remove_uuid(remove) {
  }

  bool is_config_locked() const {
    return is_config_locked_;
  }

  ~StateChangeContext() {}

  std::string ToString() const {
    switch (reason) {
      case StateChangeReason::TABLET_PEER_STARTED:
        return "Started TabletPeer";
      case StateChangeReason::CONSENSUS_STARTED:
        return "RaftConsensus started";
      case StateChangeReason::NEW_LEADER_ELECTED:
        return strings::Substitute("New leader $0 elected", new_leader_uuid);
      case StateChangeReason::FOLLOWER_NO_OP_COMPLETE:
        return "Replicate of NO_OP complete on follower";
      case StateChangeReason::LEADER_CONFIG_CHANGE_COMPLETE:
        return strings::Substitute("Replicated change config $0 round complete on leader",
          change_record.ShortDebugString());
      case StateChangeReason::FOLLOWER_CONFIG_CHANGE_COMPLETE:
        return strings::Substitute("Config change $0 complete on follower",
          change_record.ShortDebugString());
      case StateChangeReason::INVALID_REASON: FALLTHROUGH_INTENDED;
      default:
        return "INVALID REASON";
    }
  }

  // Auxiliary info for some of the reasons above.
  // Value is filled when the change reason is NEW_LEADER_ELECTED.
  const string new_leader_uuid;

  // Value is filled when the change reason is LEADER/FOLLOWER_CONFIG_CHANGE_COMPLETE.
  const ChangeConfigRecordPB change_record;

  // Value is filled when the change reason is LEADER_CONFIG_CHANGE_COMPLETE
  // and it is a REMOVE_SERVER, then that server's uuid is saved here by the master leader.
  const string remove_uuid;

  // If this is true, the call-stack above has taken the lock for the raft consensus state. Needed
  // in SysCatalogStateChanged for master to not re-get the lock. Not used for tserver callback.
  // Note that the state changes using the UpdateConsensus() mechanism always hold the lock, so
  // defaulting to true as they are majority. For ones that do not hold the lock, setting
  // it to false in their constructor suffices currently, so marking it const.
  const bool is_config_locked_ = true;
};

// Context for a consensus round on the LEADER side, typically created as an
// out-parameter of Consensus::Append.
// This class is ref-counted because we want to ensure it stays alive for the
// duration of the Operation when it is associated with a Operation, while
// we also want to ensure it has a proper lifecycle when a ConsensusRound is
// pushed that is not associated with a Tablet transaction.
class ConsensusRound : public RefCountedThreadSafe<ConsensusRound> {
 public:
  static constexpr int64_t kUnboundTerm = -1;

  // Ctor used for leader transactions. Leader transactions can and must specify the
  // callbacks prior to initiating the consensus round.
  ConsensusRound(Consensus* consensus,
                 ReplicateMsgPtr replicate_msg,
                 ConsensusReplicatedCallback replicated_cb);

  // Ctor used for follower/learner transactions. These transactions do not use the
  // replicate callback and the commit callback is set later, after the transaction
  // is actually started.
  ConsensusRound(Consensus* consensus, ReplicateMsgPtr replicate_msg);

  std::string ToString() const {
    return replicate_msg_->ShortDebugString();
  }

  const ReplicateMsgPtr& replicate_msg() {
    return replicate_msg_;
  }

  // Returns the id of the (replicate) operation this context
  // refers to. This is only set _after_ Consensus::Replicate(context).
  OpIdPB id() const {
    return replicate_msg_->id();
  }

  // Register a callback that is called by Consensus to notify that the round
  // is considered either replicated, if 'status' is OK(), or that it has
  // permanently failed to replicate if 'status' is anything else. If 'status'
  // is OK() then the operation can be applied to the state machine, otherwise
  // the operation should be aborted.
  void SetConsensusReplicatedCallback(ConsensusReplicatedCallback replicated_cb) {
    replicated_cb_ = std::move(replicated_cb);
  }

  void SetAppendCallback(ConsensusAppendCallback* append_cb) {
    append_cb_ = append_cb;
  }

  ConsensusAppendCallback* append_callback() {
    return append_cb_;
  }

  // If a continuation was set, notifies it that the round has been replicated.
  void NotifyReplicationFinished(
      const Status& status, int64_t leader_term, OpIds* applied_op_ids);

  // Binds this round such that it may not be eventually executed in any term
  // other than 'term'.
  // See CheckBoundTerm().
  void BindToTerm(int64_t term) {
    DCHECK_EQ(bound_term_, kUnboundTerm);
    bound_term_ = term;
  }

  // Check for a rare race in which an operation is submitted to the LEADER in some term,
  // then before the operation is prepared, the replica loses its leadership, receives
  // more operations as a FOLLOWER, and then regains its leadership. We detect this case
  // by setting the ConsensusRound's "bound term" when it is first submitted to the
  // PREPARE queue, and validate that the term is still the same when we have finished
  // preparing it. See KUDU-597 for details.
  //
  // If this round has not been bound to any term, this is a no-op.
  CHECKED_STATUS CheckBoundTerm(int64_t current_term) const;

  int64_t bound_term() { return bound_term_; }

 private:
  friend class RaftConsensusQuorumTest;
  friend class RefCountedThreadSafe<ConsensusRound>;

  ~ConsensusRound() {}

  Consensus* consensus_;
  // This round's replicate message.
  ReplicateMsgPtr replicate_msg_;

  // The continuation that will be called once the transaction is
  // deemed committed/aborted by consensus.
  ConsensusReplicatedCallback replicated_cb_;

  // The leader term that this round was submitted in. CheckBoundTerm()
  // ensures that, when it is eventually replicated, the term has not
  // changed in the meantime.
  //
  // Set to -1 if no term has been bound.
  int64_t bound_term_ = kUnboundTerm;

  ConsensusAppendCallback* append_cb_ = nullptr;
};

class Consensus::ConsensusFaultHooks {
 public:
  virtual CHECKED_STATUS PreStart() { return Status::OK(); }
  virtual CHECKED_STATUS PostStart() { return Status::OK(); }
  virtual CHECKED_STATUS PreConfigChange() { return Status::OK(); }
  virtual CHECKED_STATUS PostConfigChange() { return Status::OK(); }
  virtual CHECKED_STATUS PreReplicate() { return Status::OK(); }
  virtual CHECKED_STATUS PostReplicate() { return Status::OK(); }
  virtual CHECKED_STATUS PreUpdate() { return Status::OK(); }
  virtual CHECKED_STATUS PostUpdate() { return Status::OK(); }
  virtual CHECKED_STATUS PreShutdown() { return Status::OK(); }
  virtual CHECKED_STATUS PostShutdown() { return Status::OK(); }
  virtual ~ConsensusFaultHooks() {}
};

class SafeOpIdWaiter {
 public:
  virtual yb::OpId WaitForSafeOpIdToApply(const yb::OpId& op_id) = 0;

 protected:
  ~SafeOpIdWaiter() {}
};

struct LeaderState {
  LeaderStatus status;
  int64_t term;
  MonoDelta remaining_old_leader_lease;

  LeaderState& MakeNotReadyLeader(LeaderStatus status);

  bool ok() const {
    return status == LeaderStatus::LEADER_AND_READY;
  }

  CHECKED_STATUS CreateStatus() const;
};

inline CHECKED_STATUS MoveStatus(LeaderState&& state) {
  return state.CreateStatus();
}

} // namespace consensus
} // namespace yb

#endif // YB_CONSENSUS_CONSENSUS_H_
