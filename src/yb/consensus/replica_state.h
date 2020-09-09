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
#ifndef YB_CONSENSUS_REPLICA_STATE_H
#define YB_CONSENSUS_REPLICA_STATE_H

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/atomic.hpp>

#include "yb/common/hybrid_time.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus_queue.h"
#include "yb/consensus/consensus_types.h"
#include "yb/consensus/retryable_requests.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/leader_lease.h"
#include "yb/gutil/port.h"
#include "yb/util/locks.h"
#include "yb/util/status.h"
#include "yb/util/enums.h"

namespace yb {

class HostPort;
class ReplicaState;
class ThreadPool;

namespace consensus {

class RetryableRequests;

YB_DEFINE_ENUM(SetMajorityReplicatedLeaseExpirationFlag,
               (kResetOldLeaderLease)(kResetOldLeaderHtLease));

YB_STRONGLY_TYPED_BOOL(CouldStop);

// Class that coordinates access to the replica state (independently of Role).
// This has a 1-1 relationship with RaftConsensus and is essentially responsible for
// keeping state and checking if state changes are viable.
//
// Note that, in the case of a LEADER role, there are two configuration states that
// that are tracked: a pending and a committed configuration. The "active" state is
// considered to be the pending configuration if it is non-null, otherwise the
// committed configuration is the active configuration.
//
// When a replica becomes a leader of a configuration, it sets the pending configuration to
// a new configuration declaring itself as leader and sets its "active" role to LEADER.
// It then starts up ConsensusPeers for each member of the pending configuration and
// tries to push a new configuration to the peers. Once that configuration is
// pushed to a majority of the cluster, it is considered committed and the
// replica flushes that configuration to disk as the committed configuration.
//
// Each time an operation is to be performed on the replica the appropriate LockFor*()
// method should be called. The LockFor*() methods check that the replica is in the
// appropriate state to perform the requested operation and returns the lock or return
// Status::IllegalState if that is not the case.
//
// All state reading/writing methods acquire the lock, unless suffixed by "Unlocked", in
// which case a lock should be obtained prior to calling them.
class ReplicaState {
 public:
  enum State {
    // State after the replica is built.
    kInitialized,

    // State signaling the replica accepts requests (from clients
    // if leader, from leader if follower)
    kRunning,

    // State signaling that the replica is shutting down and no longer accepting
    // new transactions or commits.
    kShuttingDown,

    // State signaling the replica is shut down and does not accept
    // any more requests.
    kShutDown
  };

  typedef std::unique_lock<std::mutex> UniqueLock;

  // split_op_id is the ID of Raft split operation requesting split of this tablet or unset.
  ReplicaState(
      ConsensusOptions options, std::string peer_uuid, std::unique_ptr<ConsensusMetadata> cmeta,
      ConsensusContext* consensus_context, SafeOpIdWaiter* safe_op_id_waiter,
      RetryableRequests* retryable_requests, const yb::OpId& split_op_id,
      std::function<void(const OpIds&)> applied_ops_tracker);

  ~ReplicaState();

  CHECKED_STATUS StartUnlocked(const OpIdPB& last_in_wal);

  // Should be used only to assert that the update_lock_ is held.
  bool IsLocked() const WARN_UNUSED_RESULT;

  // Locks a replica in preparation for StartUnlocked(). Makes
  // sure the replica is in kInitialized state.
  CHECKED_STATUS LockForStart(UniqueLock* lock) const;

  // Locks a replica down until the critical section of an append completes,
  // i.e. until the replicate message has been assigned an id and placed in
  // the log queue.
  // This also checks that the replica is in the appropriate
  // state (role) to replicate the provided operation, that the operation
  // contains a replicate message and is of the appropriate type, and returns
  // Status::IllegalState if that is not the case.
  CHECKED_STATUS LockForReplicate(UniqueLock* lock, const ReplicateMsg& msg) const;
  CHECKED_STATUS LockForReplicate(UniqueLock* lock) const;

  Status CheckIsActiveLeaderAndHasLease() const;

  // Locks a replica down until an the critical section of an update completes.
  // Further updates from the same or some other leader will be blocked until
  // this completes. This also checks that the replica is in the appropriate
  // state (role) to be updated and returns Status::IllegalState if that
  // is not the case.
  CHECKED_STATUS LockForUpdate(UniqueLock* lock) const;

  // Changes the role to non-participant and returns a lock that can be
  // used to make sure no state updates come in until Shutdown() is
  // completed.
  CHECKED_STATUS LockForShutdown(UniqueLock* lock);

  CHECKED_STATUS LockForConfigChange(UniqueLock* lock) const;

  // Obtains the lock for a state read, does not check state.
  UniqueLock LockForRead() const;

  // Obtains the lock so that we can advance the majority replicated
  // index and possibly the committed index.
  // Requires that this peer is leader.
  CHECKED_STATUS LockForMajorityReplicatedIndexUpdate(
      UniqueLock* lock) const;

  // Ensure the local peer is the active leader.
  // Returns OK if leader, IllegalState otherwise.
  CHECKED_STATUS CheckActiveLeaderUnlocked(LeaderLeaseCheckMode lease_check_mode) const;

  LeaderState GetLeaderState(bool allow_stale = false) const;

  // now is used as a cache for current time. It is in/out parameter and could contain or receive
  // current time if it was used during leader state calculation.
  LeaderState GetLeaderStateUnlocked(
      LeaderLeaseCheckMode lease_check_mode = LeaderLeaseCheckMode::NEED_LEASE,
      CoarseTimePoint* now = nullptr) const;

  // Completes the Shutdown() of this replica. No more operations, local
  // or otherwise can happen after this point.
  // Called after the quiescing phase (started with LockForShutdown())
  // finishes.
  CHECKED_STATUS ShutdownUnlocked();

  // Return current consensus state summary.
  ConsensusStatePB ConsensusStateUnlocked(ConsensusConfigType type) const;

  // Returns the currently active Raft role.
  RaftPeerPB::Role GetActiveRoleUnlocked() const;

  // Returns true if there is a configuration change currently in-flight but not yet
  // committed.
  bool IsConfigChangePendingUnlocked() const;

  // Inverse of IsConfigChangePendingUnlocked(): returns OK if there is
  // currently *no* configuration change pending, and IllegalState is there *is* a
  // configuration change pending.
  CHECKED_STATUS CheckNoConfigChangePendingUnlocked() const;

  // Returns true if an operation is in this replica's log, namely:
  // - If the op's index is lower than or equal to our committed index
  // - If the op id matches an inflight op.
  // If an operation with the same index is in our log but the terms
  // are different 'term_mismatch' is set to true, it is false otherwise.
  bool IsOpCommittedOrPending(const yb::OpId& op_id, bool* term_mismatch);

  // Sets the given configuration as pending commit. Does not persist into the peers
  // metadata. In order to be persisted, SetCommittedConfigUnlocked() must be called.
  CHECKED_STATUS SetPendingConfigUnlocked(const RaftConfigPB& new_config);

  // Clears the pending config.
  CHECKED_STATUS ClearPendingConfigUnlocked();

  // Return the pending configuration, or crash if one is not set.
  const RaftConfigPB& GetPendingConfigUnlocked() const;

  // Changes the committed config for this replica. Checks that there is a
  // pending configuration and that it is equal to this one. Persists changes to disk.
  // Resets the pending configuration to null.
  CHECKED_STATUS SetCommittedConfigUnlocked(const RaftConfigPB& new_config);

  // Return the persisted configuration.
  const RaftConfigPB& GetCommittedConfigUnlocked() const;

  // Return the "active" configuration - if there is a pending configuration return it;
  // otherwise return the committed configuration.
  const RaftConfigPB& GetActiveConfigUnlocked() const;

  // Checks if the term change is legal. If so, sets 'current_term'
  // to 'new_term' and sets 'has voted' to no for the current term.
  CHECKED_STATUS SetCurrentTermUnlocked(int64_t new_term);

  // Returns the term set in the last config change round.
  const int64_t GetCurrentTermUnlocked() const;

  // Accessors for the leader of the current term.
  void SetLeaderUuidUnlocked(const std::string& uuid);
  const std::string& GetLeaderUuidUnlocked() const;
  bool HasLeaderUnlocked() const { return !GetLeaderUuidUnlocked().empty(); }
  void ClearLeaderUnlocked() { SetLeaderUuidUnlocked(""); }

  // Return whether this peer has voted in the current term.
  const bool HasVotedCurrentTermUnlocked() const;

  // Record replica's vote for the current term, then flush the consensus
  // metadata to disk.
  CHECKED_STATUS SetVotedForCurrentTermUnlocked(const std::string& uuid);

  // Return replica's vote for the current term.
  // The vote must be set; use HasVotedCurrentTermUnlocked() to check.
  const std::string& GetVotedForCurrentTermUnlocked() const;

  ConsensusContext* context() const {
    return context_;
  }

  // Returns the uuid of the peer to which this replica state belongs.
  // Safe to call with or without locks held.
  const std::string& GetPeerUuid() const;

  const ConsensusOptions& GetOptions() const;

  // Aborts pending operations after, but not including 'index'. The OpId with 'index'
  // will become our new last received id. If there are pending operations with indexes
  // higher than 'index' those operations are aborted.
  CHECKED_STATUS AbortOpsAfterUnlocked(int64_t index);

  // Returns the ConsensusRound with the provided index, if there is any, or NULL
  // if there isn't.
  scoped_refptr<ConsensusRound> GetPendingOpByIndexOrNullUnlocked(int64_t index);

  // Add 'round' to the set of rounds waiting to be committed.
  CHECKED_STATUS AddPendingOperation(const scoped_refptr<ConsensusRound>& round);

  // Marks ReplicaOperations up to 'id' as majority replicated, meaning the
  // transaction may Apply() (immediately if Prepare() has completed or when Prepare()
  // completes, if not).
  // Sets last_applied_op_id to the ID of last operation applied.
  //
  // If this advanced the committed index, sets *committed_op_id_changed to true.
  CHECKED_STATUS UpdateMajorityReplicatedUnlocked(
      const OpIdPB& majority_replicated, OpIdPB* committed_op_id, bool* committed_op_id_changed,
      OpId* last_applied_op_id);

  // Advances the committed index.
  // This is a no-op if the committed index has not changed.
  // Returns in whether the operation actually advanced the index.
  Result<bool> AdvanceCommittedOpIdUnlocked(const yb::OpId& committed_op_id, CouldStop could_stop);

  // Initializes the committed index.
  // Function checks that we are in initial state, then updates committed index.
  CHECKED_STATUS InitCommittedOpIdUnlocked(const yb::OpId& committed_op_id);

  // Returns the watermark below which all operations are known to
  // be committed according to consensus.
  //
  // This must be called under a lock.
  const yb::OpId& GetCommittedOpIdUnlocked() const;

  // Returns the watermark below which all operations are known to be applied according to
  // consensus.
  const yb::OpId& GetLastAppliedOpIdUnlocked() const {
    // See comment for last_committed_op_id_ for why we return committed op ID here.
    return GetCommittedOpIdUnlocked();
  }

  // Returns the ID of the split operation requesting to split this Raft group if it has been added
  // to Raft log and uninitialized OpId otherwise.
  const yb::OpId& GetSplitOpIdUnlocked() const;

  // Makes split_op_id uninitialized. To be used on split operation abort.
  void ResetSplitOpIdUnlocked();

  // Returns true if an op from the current term has been committed.
  bool AreCommittedAndCurrentTermsSameUnlocked() const;

  // Updates the last received operation.
  // This must be called under a lock.
  void UpdateLastReceivedOpIdUnlocked(const OpIdPB& op_id);

  // Returns the last received op id. This must be called under the lock.
  const yb::OpId& GetLastReceivedOpIdUnlocked() const;

  // Returns the id of the last op received from the current leader.
  const yb::OpId& GetLastReceivedOpIdCurLeaderUnlocked() const;

  // Returns the id of the latest pending transaction (i.e. the one with the
  // latest index). This must be called under the lock.
  OpIdPB GetLastPendingOperationOpIdUnlocked() const;

  // Used by replicas to cancel pending transactions. Pending transaction are those
  // that have completed prepare/replicate but are waiting on the LEADER's commit
  // to complete. This does not cancel transactions being applied.
  CHECKED_STATUS CancelPendingOperations();

  // API to dump pending transactions. Added to debug ENG-520.
  void DumpPendingOperationsUnlocked();

  yb::OpId NewIdUnlocked();

  // Used when, for some reason, an operation that failed before it could be considered
  // a part of the state machine. Basically restores the id gen to the state it was before
  // generating 'id'. So that we reuse these ids later, when we can actually append to the
  // state machine. This makes the state machine have continuous ids for the same term, even if
  // the queue refused to add any more operations.
  // should_exists indicates whether we expect that this operation is already added.
  // Used for debugging purposes only.
  void CancelPendingOperation(const OpIdPB& id, bool should_exist);

  // Accessors for pending election op id. These must be called under a lock.
  const OpIdPB& GetPendingElectionOpIdUnlocked() { return pending_election_opid_; }
  void SetPendingElectionOpIdUnlocked(const OpIdPB& opid) { pending_election_opid_ = opid; }
  void ClearPendingElectionOpIdUnlocked() { pending_election_opid_.Clear(); }

  std::string ToString() const;
  std::string ToStringUnlocked() const;

  // A common prefix that should be in any log messages emitted,
  // identifying the tablet and peer.
  std::string LogPrefix() const;

  // Checks that 'current' correctly follows 'previous'. Specifically it checks
  // that the term is the same or higher and that the index is sequential.
  static CHECKED_STATUS CheckOpInSequence(const yb::OpId& previous, const yb::OpId& current);

  // Return the current state of this object.
  // The update_lock_ must be held.
  ReplicaState::State state() const;

  // Update the point in time we have to wait until before starting to act as a leader in case
  // we win an election.
  void UpdateOldLeaderLeaseExpirationOnNonLeaderUnlocked(
      const CoarseTimeLease& lease, const PhysicalComponentLease& ht_lease);

  void SetMajorityReplicatedLeaseExpirationUnlocked(
      const MajorityReplicatedData& majority_replicated_data,
      EnumBitSet<SetMajorityReplicatedLeaseExpirationFlag> flags);

  // Checks two conditions:
  // - That the old leader definitely does not have a lease.
  // - That this leader has a committed lease.
  LeaderLeaseStatus GetLeaderLeaseStatusUnlocked(
      MonoDelta* remaining_old_leader_lease = nullptr, CoarseTimePoint* now = nullptr) const;

  LeaderLeaseStatus GetHybridTimeLeaseStatusAtUnlocked(MicrosTime micros_time) const;

  // Get the remaining duration of the old leader's lease. Optionally, return the current time in
  // the "now" output parameter. In case the old leader's lease has already expired or is not known,
  // returns an uninitialized MonoDelta value.
  MonoDelta RemainingOldLeaderLeaseDuration(CoarseTimePoint* now = nullptr) const;

  const PhysicalComponentLease& old_leader_ht_lease() const {
    return old_leader_ht_lease_;
  }

  const CoarseTimeLease& old_leader_lease() const {
    return old_leader_lease_;
  }

  bool MajorityReplicatedLeaderLeaseExpired(CoarseTimePoint* now = nullptr) const;

  bool MajorityReplicatedHybridTimeLeaseExpiredAt(MicrosTime hybrid_time) const;

  // Get the current majority-replicated hybrid time leader lease expiration time as a microsecond
  // timestamp.
  // @param min_allowed - will wait until the majority-replicated hybrid time leader lease reaches
  //                      at least this microsecond timestamp.
  // @param deadline - won't wait past this deadline.
  // @return leader lease or 0 if timed out.
  MicrosTime MajorityReplicatedHtLeaseExpiration(
      MicrosTime min_allowed, CoarseTimePoint deadline) const;

  // The on-disk size of the consensus metadata.
  uint64_t OnDiskSize() const;

  yb::OpId MinRetryableRequestOpId();

  RestartSafeCoarseMonoClock& Clock();

  RetryableRequestsCounts TEST_CountRetryableRequests();

  void SetLeaderNoOpCommittedUnlocked(bool value);

 private:
  typedef std::deque<ConsensusRoundPtr> PendingOperations;

  template <class Policy>
  LeaderLeaseStatus GetLeaseStatusUnlocked(Policy policy) const;

  // Apply pending operations beginning at iter up to and including committed_op_id.
  // Updates last_committed_op_id_ to committed_op_id.
  CHECKED_STATUS ApplyPendingOperationsUnlocked(
      const yb::OpId& committed_op_id, CouldStop could_stop);

  void SetLastCommittedIndexUnlocked(const yb::OpId& committed_op_id);

  // Applies committed config change.
  void ApplyConfigChangeUnlocked(const ConsensusRoundPtr& round);

  void NotifyReplicationFinishedUnlocked(
      const ConsensusRoundPtr& round, const Status& status, int64_t leader_term,
      OpIds* applied_op_ids);

  consensus::LeaderState RefreshLeaderStateCacheUnlocked(
      CoarseTimePoint* now) const ATTRIBUTE_NONNULL(2);

  PendingOperations::iterator FindPendingOperation(int64_t index);

  const ConsensusOptions options_;

  // The UUID of the local peer.
  const std::string peer_uuid_;

  mutable std::mutex update_lock_;
  mutable std::condition_variable cond_;

  // Consensus metadata persistence object.
  std::unique_ptr<ConsensusMetadata> cmeta_;

  // Used by the LEADER. This is the index of the next operation generated
  // by this LEADER.
  int64_t next_index_ = 0;

  // Queue of pending operations. Ordered by growing operation index.
  PendingOperations pending_operations_;

  // When we receive a message from a remote peer telling us to start a operation, we use
  // this factory to start it.
  ConsensusContext* context_;

  // Used to wait for safe op id during apply of committed entries.
  SafeOpIdWaiter* safe_op_id_waiter_;

  // The id of the last received operation, which corresponds to the last entry
  // written to the local log. Operations whose id is lower than or equal to
  // this id do not need to be resent by the leader. This is not guaranteed to
  // be monotonically increasing due to the possibility for log truncation and
  // aborted operations when a leader change occurs.
  yb::OpId last_received_op_id_;

  // Same as last_received_op_id_ but only includes operations sent by the
  // current leader. The "term" in this op may not actually match the current
  // term, since leaders may replicate ops from prior terms.
  //
  // As an implementation detail, this field is reset to MinumumOpId() every
  // time there is a term advancement on the local node, to simplify the logic
  // involved in resetting this every time a new node becomes leader.
  yb::OpId last_received_op_id_current_leader_;

  // The ID of the operation that was last committed. Initialized to MinimumOpId().
  // NOTE: due to implementation details at this and lower layers all operations up to
  // last_committed_op_id_ are guaranteed to be already applied.
  yb::OpId last_committed_op_id_;

  // The id of the split operation requesting to split this tablet. This is set when split
  // operation is added to log and cleared if this operation is aborted.
  // Apply of tablet split operation does not change split_op_id_.
  //
  // Note: In tablets created as a result of split operation split_op_id_ is uninitialized until
  // split operation requesting to split them will be added to their Raft log.
  // After n-th split the latest after-split tablet created could have n split operations in its
  // Raft log, but split_op_id will be uninitialized, because all these split operations are
  // designated for "ancestors" of this tablet, but not for this tablet itself.
  yb::OpId split_op_id_;

  // If set, a leader election is pending upon the specific op id commitment to this peer's log.
  OpIdPB pending_election_opid_;

  State state_ = State::kInitialized;

  // When a follower becomes the leader, it uses this field to wait out the old leader's lease
  // before accepting writes or serving up-to-date reads. This is also used by candidates by
  // granting a vote. We compute the amount of time the new leader has to wait to make sure the old
  // leader's lease has expired.
  //
  // This is marked mutable because it can be reset on to MonoTime::kMin on the read path after the
  // deadline has passed, so that we avoid querying the clock unnecessarily from that point on.
  mutable CoarseTimeLease old_leader_lease_;

  // The same as old_leader_lease_ but for hybrid time.
  mutable PhysicalComponentLease old_leader_ht_lease_;

  // LEADER only: the latest committed lease expiration deadline for the current leader. The leader
  // is allowed to serve up-to-date reads and accept writes only while the current time is less than
  // this. However, the leader might manage to replicate a lease extension without losing its
  // leadership.
  CoarseTimePoint majority_replicated_lease_expiration_;

  // LEADER only: the latest committed hybrid time lease expiration deadline for the current leader.
  // The leader is allowed to add new log entries only when lease of old leader is expired.
  std::atomic<MicrosTime> majority_replicated_ht_lease_expiration_{
      PhysicalComponentLease::NoneValue()};

  RetryableRequests retryable_requests_;

  // This leader is ready to serve only if NoOp was successfully committed
  // after the new leader successful election.
  bool leader_no_op_committed_ = false;

  std::function<void(const OpIds&)> applied_ops_tracker_;

  struct LeaderStateCache {
    static constexpr size_t kStatusBits = 3;
    static_assert(kLeaderStatusMapSize <= (1 << kStatusBits),
                  "Leader status does not fit into kStatusBits");

    static constexpr uint64_t kStatusMask = (1 << kStatusBits) -1;

    // Packed status consists on LeaderStatus and an extra value.
    // Extra value meaning depends on actual status:
    // LEADER_AND_READY: leader term.
    // LEADER_BUT_OLD_LEADER_MAY_HAVE_LEASE: number of microseconds in remaining_old_leader_lease.
    uint64_t packed_status;
    CoarseTimePoint expire_at;

    LeaderStateCache() noexcept {}

    LeaderStatus status() const {
      return static_cast<LeaderStatus>(packed_status & kStatusMask);
    }

    uint64_t extra_value() const {
      return packed_status >> kStatusBits;
    }

    void Set(LeaderStatus status, uint64_t extra_value, CoarseTimePoint expire_at_) {
      DCHECK_EQ(extra_value << kStatusBits >> kStatusBits, extra_value);
      packed_status = static_cast<uint64_t>(status) | (extra_value << kStatusBits);
      expire_at = expire_at_;
    }
  };

  mutable boost::atomic<LeaderStateCache> leader_state_cache_;
};

}  // namespace consensus
}  // namespace yb

#endif // YB_CONSENSUS_REPLICA_STATE_H_
