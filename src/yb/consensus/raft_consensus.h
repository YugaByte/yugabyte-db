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

#ifndef YB_CONSENSUS_RAFT_CONSENSUS_H_
#define YB_CONSENSUS_RAFT_CONSENSUS_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional_fwd.hpp>

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus_queue.h"

#include "yb/rpc/scheduler.h"

#include "yb/util/opid.h"
#include "yb/util/random.h"
#include "yb/util/result.h"

DECLARE_int32(leader_lease_duration_ms);
DECLARE_int32(ht_lease_duration_ms);

namespace yb {

typedef std::lock_guard<simple_spinlock> Lock;
typedef gscoped_ptr<Lock> ScopedLock;

class Counter;
class HostPort;
class ThreadPool;
class ThreadPoolToken;

namespace server {
class Clock;
}

namespace rpc {
class PeriodicTimer;
}
namespace consensus {
class ConsensusMetadata;
class Peer;
class PeerProxyFactory;
class PeerManager;
class ReplicaState;
struct ElectionResult;

constexpr int32_t kDefaultLeaderLeaseDurationMs = 2000;

YB_STRONGLY_TYPED_BOOL(WriteEmpty);
YB_STRONGLY_TYPED_BOOL(PreElected);

YB_DEFINE_ENUM(RejectMode, (kNone)(kAll)(kNonEmpty));

class RaftConsensus : public std::enable_shared_from_this<RaftConsensus>,
                      public Consensus,
                      public PeerMessageQueueObserver,
                      public SafeOpIdWaiter {
 public:
  class ConsensusFaultHooks;

  // Creates RaftConsensus.
  // split_op_info is the parameters of split tablet Raft operation requesting split of this
  // tablet or unset.
  static std::shared_ptr<RaftConsensus> Create(
    const ConsensusOptions& options,
    std::unique_ptr<ConsensusMetadata> cmeta,
    const RaftPeerPB& local_peer_pb,
    const scoped_refptr<MetricEntity>& metric_entity,
    const scoped_refptr<server::Clock>& clock,
    ConsensusContext* consensus_context,
    rpc::Messenger* messenger,
    rpc::ProxyCache* proxy_cache,
    const scoped_refptr<log::Log>& log,
    const std::shared_ptr<MemTracker>& server_mem_tracker,
    const std::shared_ptr<MemTracker>& parent_mem_tracker,
    const Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk,
    TableType table_type,
    ThreadPool* raft_pool,
    RetryableRequests* retryable_requests,
    const SplitOpInfo& split_op_info);

  // Creates RaftConsensus.
  // split_op_info is the parameters of split tablet Raft operation requesting split of this
  // tablet or unset.
  RaftConsensus(
    const ConsensusOptions& options,
    std::unique_ptr<ConsensusMetadata> cmeta,
    std::unique_ptr<PeerProxyFactory> peer_proxy_factory,
    std::unique_ptr<PeerMessageQueue> queue,
    std::unique_ptr<PeerManager> peer_manager,
    std::unique_ptr<ThreadPoolToken> raft_pool_token,
    const scoped_refptr<MetricEntity>& metric_entity,
    const std::string& peer_uuid,
    const scoped_refptr<server::Clock>& clock,
    ConsensusContext* consensus_context,
    const scoped_refptr<log::Log>& log,
    std::shared_ptr<MemTracker> parent_mem_tracker,
    Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk,
    TableType table_type,
    RetryableRequests* retryable_requests,
    const SplitOpInfo& split_op_info);

  virtual ~RaftConsensus();

  virtual CHECKED_STATUS Start(const ConsensusBootstrapInfo& info) override;

  virtual bool IsRunning() const override;

  // Emulates an election by increasing the term number and asserting leadership
  // in the configuration by sending a NO_OP to other peers.
  // This is NOT safe to use in a distributed configuration with failure detection
  // enabled, as it could result in a split-brain scenario.
  CHECKED_STATUS EmulateElection() override;

  CHECKED_STATUS ElectionLostByProtege(const std::string& election_lost_by_uuid) override;

  CHECKED_STATUS WaitUntilLeaderForTests(const MonoDelta& timeout) override;

  CHECKED_STATUS StepDown(const LeaderStepDownRequestPB* req,
                          LeaderStepDownResponsePB* resp) override;

  CHECKED_STATUS TEST_Replicate(const ConsensusRoundPtr& round) override;
  CHECKED_STATUS ReplicateBatch(ConsensusRounds* rounds) override;

  CHECKED_STATUS Update(
      ConsensusRequestPB* request,
      ConsensusResponsePB* response,
      CoarseTimePoint deadline) override;

  CHECKED_STATUS RequestVote(const VoteRequestPB* request,
                             VoteResponsePB* response) override;

  CHECKED_STATUS ChangeConfig(const ChangeConfigRequestPB& req,
                              const StdStatusCallback& client_cb,
                              boost::optional<tserver::TabletServerErrorPB::Code>* error_code)
                              override;

  RaftPeerPB::Role GetRoleUnlocked() const;

  RaftPeerPB::Role role() const override;

  LeaderState GetLeaderState(bool allow_stale = false) const override;

  std::string peer_uuid() const override;

  std::string tablet_id() const override;

  ConsensusStatePB ConsensusState(
      ConsensusConfigType type,
      LeaderLeaseStatus* leader_lease_status) const override;

  ConsensusStatePB ConsensusStateUnlocked(
      ConsensusConfigType type,
      LeaderLeaseStatus* leader_lease_status) const override;

  RaftConfigPB CommittedConfig() const override;

  void DumpStatusHtml(std::ostream& out) const override;

  void Shutdown() override;

  // Return the active (as opposed to committed) role.
  RaftPeerPB::Role GetActiveRole() const;

  // Returns the replica state for tests. This should never be used outside of
  // tests, in particular calling the LockFor* methods on the returned object
  // can cause consensus to deadlock.
  ReplicaState* GetReplicaStateForTests();

  void TEST_UpdateMajorityReplicated(
      const OpId& majority_replicated, OpId* committed_index, OpId* last_committed_op_id) {
    UpdateMajorityReplicated({ majority_replicated,
                               CoarseTimePoint::min(),
                               HybridTime::kMin.GetPhysicalValueMicros() },
                             committed_index, last_committed_op_id);
  }

  yb::OpId GetLastReceivedOpId() override;

  yb::OpId GetLastCommittedOpId() override;

  yb::OpId GetLastAppliedOpId() override;

  yb::OpId GetAllAppliedOpId();

  yb::OpId GetSplitOpId() override;

  std::array<TabletId, kNumSplitParts> GetSplitChildTabletIds() override;

  Result<MicrosTime> MajorityReplicatedHtLeaseExpiration(
      MicrosTime min_allowed, CoarseTimePoint deadline) const override;

  // The on-disk size of the consensus metadata.
  uint64_t OnDiskSize() const;

  yb::OpId MinRetryableRequestOpId();

  CHECKED_STATUS StartElection(const LeaderElectionData& data) override {
    return DoStartElection(data, PreElected::kFalse);
  }

  size_t LogCacheSize();
  size_t EvictLogCache(size_t bytes_to_evict);

  CHECKED_STATUS FlushLogIndex();

  CHECKED_STATUS CopyLogTo(const std::string& dest_dir);

  RetryableRequestsCounts TEST_CountRetryableRequests();

  void TEST_RejectMode(RejectMode value) {
    reject_mode_.store(value, std::memory_order_release);
  }

  void TEST_DelayUpdate(MonoDelta duration) {
    TEST_delay_update_.store(duration, std::memory_order_release);
  }

  Result<ReadOpsResult> ReadReplicatedMessagesForCDC(const yb::OpId& from,
    int64_t* last_replicated_opid_index) override;

  void UpdateCDCConsumerOpId(const yb::OpId& op_id) override;

  // Start memory tracking of following operation in case it is still present in our caches.
  void TrackOperationMemory(const yb::OpId& op_id);

  uint64_t MajorityNumSSTFiles() const {
    return majority_num_sst_files_.load(std::memory_order_acquire);
  }

  int64_t TEST_LeaderTerm() const;

 protected:
  // Trigger that a non-Operation ConsensusRound has finished replication.
  // If the replication was successful, an status will be OK. Otherwise, it
  // may be Aborted or some other error status.
  // If 'status' is OK, write a Commit message to the local WAL based on the
  // type of message it is.
  // The 'client_cb' will be invoked at the end of this execution.
  virtual void NonTxRoundReplicationFinished(ConsensusRound* round,
                                             const StdStatusCallback& client_cb,
                                             const Status& status);

  // As a leader, append a new ConsensusRound to the queue.
  // Only virtual and protected for mocking purposes.
  virtual CHECKED_STATUS AppendNewRoundToQueueUnlocked(const scoped_refptr<ConsensusRound>& round);
  virtual CHECKED_STATUS AppendNewRoundsToQueueUnlocked(
      const std::vector<scoped_refptr<ConsensusRound>>& rounds);

  // As a follower, start a consensus round not associated with a Operation.
  // Only virtual and protected for mocking purposes.
  virtual CHECKED_STATUS StartConsensusOnlyRoundUnlocked(const ReplicateMsgPtr& msg);

  // Assuming we are the leader, wait until we have a valid leader lease (i.e. the old leader's
  // lease has expired, and we have replicated a new lease that has not expired yet).
  // This says "Imprecise" because there is a slight race condition where this could wait for an
  // additional short time interval (e.g. 100 ms) in case we've just acquired the lease and the
  // waiting thread missed the notification. However, as of 08/14/2017 this is only used in a
  // context where this does not matter, such as catalog manager initialization.
  CHECKED_STATUS WaitForLeaderLeaseImprecise(CoarseTimePoint deadline) override;

  CHECKED_STATUS CheckIsActiveLeaderAndHasLease() const override;

 private:
  friend class ReplicaState;
  friend class RaftConsensusQuorumTest;

  CHECKED_STATUS DoStartElection(const LeaderElectionData& data, PreElected preelected);

  Result<LeaderElectionPtr> CreateElectionUnlocked(
      const LeaderElectionData& data,
      MonoDelta timeout,
      PreElection preelection);

  // Updates the committed_index, triggers the Apply()s for whatever
  // operations were pending and updates last_applied_op_id.
  // This is idempotent.
  void UpdateMajorityReplicated(
      const MajorityReplicatedData& data, OpId* committed_op_id, OpId* last_applied_op_id) override;

  void NotifyTermChange(int64_t term) override;

  void NotifyFailedFollower(const std::string& uuid,
                            int64_t term,
                            const std::string& reason) override;

  void MajorityReplicatedNumSSTFilesChanged(uint64_t majority_replicated_num_sst_files) override;

  // Control whether printing of log messages should be done for a particular
  // function call.
  enum AllowLogging {
    DO_NOT_LOG = 0,
    ALLOW_LOGGING = 1,
  };

  // Helper struct that contains the messages from the leader that we need to
  // append to our log, after they've been deduplicated.
  struct LeaderRequest;

  std::string LogPrefix();

  // Set the leader UUID of the configuration and mark the tablet config dirty for
  // reporting to the master.
  void SetLeaderUuidUnlocked(const std::string& uuid);

  // Replicate (as leader) a pre-validated config change. This includes
  // updating the peers and setting the new_configuration as pending.
  CHECKED_STATUS ReplicateConfigChangeUnlocked(const ReplicateMsgPtr& replicate_ref,
                                               const RaftConfigPB& new_config,
                                               ChangeConfigType type,
                                               StdStatusCallback client_cb);

  // Update the peers and queue to be consistent with a new active configuration.
  // Should only be called by the leader.
  void RefreshConsensusQueueAndPeersUnlocked();

  // Makes the peer become leader.
  // Returns OK once the change config operation that has this peer as leader
  // has been enqueued, the operation will complete asynchronously.
  //
  // The ReplicaState must be locked for configuration change before calling.
  CHECKED_STATUS BecomeLeaderUnlocked();

  // Makes the peer become a replica, i.e. a FOLLOWER or a LEARNER.
  // initial_fd_wait is the initial wait time before the FailureDetector wakes up and triggers a
  // leader election.
  //
  // The ReplicaState must be locked for configuration change before calling.
  CHECKED_STATUS BecomeReplicaUnlocked(
      const std::string& new_leader_uuid,
      MonoDelta initial_fd_wait = MonoDelta());

  struct UpdateReplicaResult {
    OpId wait_for_op_id;

    // Start an election after the writes are committed?
    bool start_election = false;

    int64_t current_term = OpId::kUnknownTerm;
  };

  // Updates the state in a replica by storing the received operations in the log
  // and triggering the required operations. This method won't return until all
  // operations have been stored in the log and all Prepares() have been completed,
  // and a replica cannot accept any more Update() requests until this is done.
  Result<UpdateReplicaResult> UpdateReplica(
      ConsensusRequestPB* request,
      ConsensusResponsePB* response);

  // Deduplicates an RPC request making sure that we get only messages that we
  // haven't appended to our log yet.
  // On return 'deduplicated_req' is instantiated with only the new messages
  // and the correct preceding id.
  CHECKED_STATUS DeduplicateLeaderRequestUnlocked(ConsensusRequestPB* rpc_req,
                                                  LeaderRequest* deduplicated_req);

  // Handles a request from a leader, refusing the request if the term is lower than
  // ours or stepping down if it's higher.
  CHECKED_STATUS HandleLeaderRequestTermUnlocked(const ConsensusRequestPB* request,
                                         ConsensusResponsePB* response);

  // Checks that the preceding op in 'req' is locally committed or pending and sets an
  // appropriate error message in 'response' if not.
  // If there is term mismatch between the preceding op id in 'req' and the local log's
  // pending operations, we proactively abort those pending operations after and including
  // the preceding op in 'req' to avoid a pointless cache miss in the leader's log cache.
  CHECKED_STATUS EnforceLogMatchingPropertyMatchesUnlocked(const LeaderRequest& req,
                                                   ConsensusResponsePB* response);

  // Checks that deduplicated messages in an UpdateConsensus request are in the right order.
  CHECKED_STATUS CheckLeaderRequestOpIdSequence(
      const LeaderRequest& deduped_req,
      ConsensusRequestPB* request);

  // Check a request received from a leader, making sure:
  // - The request is in the right term
  // - The log matching property holds
  // - Messages are de-duplicated so that we only process previously unprocessed requests.
  // - We abort operations if the leader sends operations that have the same index as
  //   operations currently on the pendings set, but different terms.
  // If this returns ok and the response has no errors, 'deduped_req' is set with only
  // the messages to add to our state machine.
  CHECKED_STATUS CheckLeaderRequestUnlocked(
      ConsensusRequestPB* request,
      ConsensusResponsePB* response,
      LeaderRequest* deduped_req);

  // Returns the most recent OpId written to the Log.
  yb::OpId GetLatestOpIdFromLog();

  // Begin a replica operation. If the type of message in 'msg' is not a type
  // that uses operations, delegates to StartConsensusOnlyRoundUnlocked().
  CHECKED_STATUS StartReplicaOperationUnlocked(const ReplicateMsgPtr& msg,
                                               HybridTime propagated_safe_time);

  // Return header string for RequestVote log messages. The ReplicaState lock must be held.
  std::string GetRequestVoteLogPrefix(const VoteRequestPB& request) const;

  // Fills the response with the current status, if an update was successful.
  void FillConsensusResponseOKUnlocked(ConsensusResponsePB* response);

  // Fills the response with an error code and error message.
  void FillConsensusResponseError(ConsensusResponsePB* response,
                                  ConsensusErrorPB::Code error_code,
                                  const Status& status);

  // Fill VoteResponsePB with the following information:
  // - Update responder_term to current local term.
  // - Set vote_granted to true.
  void FillVoteResponseVoteGranted(const VoteRequestPB& request, VoteResponsePB* response);

  // Fill VoteResponsePB with the following information:
  // - Update responder_term to current local term.
  // - Set vote_granted to false.
  // - Set consensus_error.code to the given code.
  void FillVoteResponseVoteDenied(ConsensusErrorPB::Code error_code, VoteResponsePB* response);

  void RequestVoteRespondVoteDenied(
      ConsensusErrorPB::Code error_code, const std::string& message_suffix,
      const VoteRequestPB& request, VoteResponsePB* response);

  // Respond to VoteRequest that the candidate has an old term.
  CHECKED_STATUS RequestVoteRespondInvalidTerm(const VoteRequestPB* request,
                                               VoteResponsePB* response);

  // Respond to VoteRequest that we already granted our vote to the candidate.
  CHECKED_STATUS RequestVoteRespondVoteAlreadyGranted(const VoteRequestPB* request,
                                              VoteResponsePB* response);

  // Respond to VoteRequest that we already granted our vote to someone else.
  CHECKED_STATUS RequestVoteRespondAlreadyVotedForOther(const VoteRequestPB* request,
                                                VoteResponsePB* response);

  // Respond to VoteRequest that the candidate's last-logged OpId is too old.
  CHECKED_STATUS RequestVoteRespondLastOpIdTooOld(const OpIdPB& local_last_opid,
                                                  const VoteRequestPB* request,
                                                  VoteResponsePB* response);

  // Respond to VoteRequest that the vote was not granted because we believe
  // the leader to be alive.
  CHECKED_STATUS RequestVoteRespondLeaderIsAlive(const VoteRequestPB* request,
                                         VoteResponsePB* response);

  // Respond to VoteRequest that the replica is already in the middle of servicing
  // another vote request or an update from a valid leader.
  CHECKED_STATUS RequestVoteRespondIsBusy(const VoteRequestPB* request,
                                  VoteResponsePB* response);

  // Respond to VoteRequest that the vote is granted for candidate.
  CHECKED_STATUS RequestVoteRespondVoteGranted(const VoteRequestPB* request,
                                       VoteResponsePB* response);

  // Callback for leader election driver. ElectionCallback is run on the
  // reactor thread, so it simply defers its work to DoElectionCallback.
  void ElectionCallback(const LeaderElectionData& data, const ElectionResult& result);
  void DoElectionCallback(const LeaderElectionData& data, const ElectionResult& result);
  void NotifyOriginatorAboutLostElection(const std::string& originator_uuid);

  // Helper struct that tracks the RunLeaderElection as part of leadership transferral.
  struct RunLeaderElectionState {
    PeerProxyPtr proxy;
    RunLeaderElectionRequestPB req;
    RunLeaderElectionResponsePB resp;
    rpc::RpcController rpc;
  };

  // Callback for RunLeaderElection async request.
  void RunLeaderElectionResponseRpcCallback(std::shared_ptr<RunLeaderElectionState> election_state);

  // Start tracking the leader for failures. This typically occurs at startup
  // and when the local peer steps down as leader.
  //
  // If 'delta' is set, it is used as the initial failure period. Otherwise,
  // the minimum election timeout is used.
  //
  // If the failure detector is already registered, has no effect.
  void EnableFailureDetector(MonoDelta delta = MonoDelta());

  // Stop tracking the current leader for failures.
  // This typically happens when the local peer becomes leader.
  // If the failure detector is already disabled, has no effect.
  void DisableFailureDetector();

  // "Reset" the failure detector to indicate leader activity.
  // When this is called a failure is guaranteed not to be detected
  // before 'FLAGS_leader_failure_max_missed_heartbeat_periods' *
  // 'FLAGS_raft_heartbeat_interval_ms' has elapsed, unless 'delta' is set, in
  // which case its value is used as the next failure period.
  // If 'allow_logging' is set to ALLOW_LOGGING, then this method
  // will print a log message when called.
  // If the failure detector is not registered, this method has no effect.
  void SnoozeFailureDetector(AllowLogging allow_logging,
                             MonoDelta delta = MonoDelta());

  // Return the minimum election timeout. Due to backoff and random
  // jitter, election timeouts may be longer than this.
  MonoDelta MinimumElectionTimeout() const;

  // Calculates a snooze delta for leader election.
  // The delta increases exponentially with the difference
  // between the current term and the term of the last committed
  // operation.
  // The maximum delta is capped by 'FLAGS_leader_failure_exp_backoff_max_delta_ms'.
  MonoDelta LeaderElectionExpBackoffDeltaUnlocked();

  // Checks if the leader is ready to process a change config request (one requirement for this is
  // for it to have at least one committed op in the current term). Also checks that there are no
  // voters in transition in the active config state. CHECKED_STATUS OK() implies leader is ready.
  // server_uuid is the uuid of the server that we are trying to remove, add, or change its
  // role.
  CHECKED_STATUS IsLeaderReadyForChangeConfigUnlocked(ChangeConfigType type,
                                              const std::string& server_uuid);

  // Increment the term to the next term, resetting the current leader, etc.
  CHECKED_STATUS IncrementTermUnlocked();

  // Handle when the term has advanced beyond the current term.
  CHECKED_STATUS HandleTermAdvanceUnlocked(ConsensusTerm new_term);

  // Notify the tablet peer that the consensus configuration
  // has changed, thus reporting it back to the master. This is performed inline.
  void MarkDirty(std::shared_ptr<StateChangeContext> context);

  // Calls MarkDirty() if 'status' == OK. Then, always calls 'client_cb' with
  // 'status' as its argument.
  void MarkDirtyOnSuccess(std::shared_ptr<StateChangeContext> context,
                          const StdStatusCallback& client_cb,
                          const Status& status);

  // Attempt to remove the follower with the specified 'uuid' from the config,
  // if the 'committed_config' is still the committed config and if the current
  // node is the leader.
  //
  // Since this is inherently an asynchronous operation run on a thread pool,
  // it may fail due to the configuration changing, the local node losing
  // leadership, or the tablet shutting down.
  // Logs a warning on failure.
  void TryRemoveFollowerTask(const std::string& uuid,
                             const RaftConfigPB& committed_config,
                             const std::string& reason);

  // Called when the failure detector expires.
  // Submits ReportFailureDetectedTask() to a thread pool.
  void ReportFailureDetected();

  // Call StartElection(), log a warning if the call fails (usually due to
  // being shut down).
  void ReportFailureDetectedTask();

  // Helper API to check if the pending/committed configuration has a PRE_VOTER. Non-null return
  // string implies there are servers in transit.
  string ServersInTransitionMessage();

  // Prevent starting new election for some time, after we stepped down.
  // protege_uuid - in case of step down we remember our protege.
  // After that we use its UUID to check whether node that lost election is our active protege.
  // There could be case that we already initiated another stepdown, and after that we received
  // delayed packet from old protege.
  // So this field allows us to filter out this situation.
  // Also we could introduce serial number of stepdown and filter using it.
  // That woule be more robust, since it handles also situation when we tried to stepdown
  // to the same node twice, and first retry was delayed, but second procedure is on the way.
  void WithholdElectionAfterStepDown(const std::string& protege_uuid);

  // Steps of UpdateReplica.
  CHECKED_STATUS EarlyCommitUnlocked(const ConsensusRequestPB& request,
                                     const LeaderRequest& deduped_req);
  Result<bool> EnqueuePreparesUnlocked(const ConsensusRequestPB& request,
                                       LeaderRequest* deduped_req,
                                       ConsensusResponsePB* response);
  // Returns last op id received from leader.
  yb::OpId EnqueueWritesUnlocked(const LeaderRequest& deduped_req, WriteEmpty write_empty);
  CHECKED_STATUS MarkOperationsAsCommittedUnlocked(const ConsensusRequestPB& request,
                                                   const LeaderRequest& deduped_req,
                                                   OpId last_from_leader);

  // Wait until the operation with op id equal to wait_for_op_id is flushed in the WAL.
  // If term was changed during wait from the specified one - exit with error.
  CHECKED_STATUS WaitForWrites(int64_t term, const OpId& wait_for_op_id);

  // See comment for ReplicaState::CancelPendingOperation
  void RollbackIdAndDeleteOpId(const ReplicateMsgPtr& replicate_msg, bool should_exists);

  yb::OpId WaitForSafeOpIdToApply(const yb::OpId& op_id) override;

  void AppendEmptyBatchToLeaderLog();

  // Step down in favor of peer.
  // When graceful is true, protege would not be stored and election would not take place in case
  // of protege election failure.
  CHECKED_STATUS StartStepDownUnlocked(const RaftPeerPB& peer, bool graceful);

  // Checked whether we should start step down when protege did not synchronize before timeout.
  void CheckDelayedStepDown(const Status& status);

  // Threadpool token for constructing requests to peers, handling RPC callbacks,
  // etc.
  std::unique_ptr<ThreadPoolToken> raft_pool_token_;

  scoped_refptr<log::Log> log_;
  scoped_refptr<server::Clock> clock_;
  std::unique_ptr<PeerProxyFactory> peer_proxy_factory_;

  std::unique_ptr<PeerManager> peer_manager_;

  // The queue of messages that must be sent to peers.
  std::unique_ptr<PeerMessageQueue> queue_;

  std::unique_ptr<ReplicaState> state_;

  Random rng_;

  std::shared_ptr<rpc::PeriodicTimer> failure_detector_;

  // If any RequestVote() RPC arrives before this hybrid time,
  // the request will be ignored. This prevents abandoned or partitioned
  // nodes from disturbing the healthy leader.
  std::atomic<MonoTime> withhold_votes_until_;

  // UUID of new desired leader during stepdown.
  TabletServerId protege_leader_uuid_;

  // This is the time (in the MonoTime's uint64 representation) for which election should not start
  // on this peer.
  std::atomic<MonoTime> withhold_election_start_until_{MonoTime::Min()};

  // We record the moment at which we discover that an election has been lost by our "protege"
  // during leader stepdown. Then, when the master asks us to step down again in favor of the same
  // server, we'll reply with the amount of time that has passed to avoid leader stepdown loops.s
  MonoTime election_lost_by_protege_at_;

  struct DelayedStepDown {
    int64_t term = OpId::kUnknownTerm;
    TabletServerId protege;
    bool graceful;

    std::string ToString() const;
  };

  DelayedStepDown delayed_step_down_;
  rpc::ScheduledTaskTracker step_down_check_tracker_;

  // The number of times this node has called and lost a leader election since
  // the last time it saw a stable leader (either itself or another node).
  // This is used to calculate back-off of the election timeout.
  std::atomic<int> failed_elections_since_stable_leader_{0};

  const Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk_;

  // Lock ordering note: If both this lock and the ReplicaState lock are to be
  // taken, this lock must be taken first.
  mutable std::timed_mutex update_mutex_;

  std::atomic_flag outstanding_report_failure_task_ = ATOMIC_FLAG_INIT;

  AtomicBool shutdown_;

  scoped_refptr<Counter> follower_memory_pressure_rejections_;
  scoped_refptr<AtomicGauge<int64_t>> term_metric_;
  scoped_refptr<AtomicMillisLag> follower_last_update_time_ms_metric_;
  scoped_refptr<AtomicGauge<int64_t>> is_raft_leader_metric_;
  std::shared_ptr<MemTracker> parent_mem_tracker_;

  TableType table_type_;

  // Mutex / condition used for waiting for acquiring a valid leader lease.
  std::mutex leader_lease_wait_mtx_;
  std::condition_variable leader_lease_wait_cond_;

  scoped_refptr<Histogram> update_raft_config_dns_latency_;

  // Used only when TEST_follower_reject_update_consensus_requests_seconds is greater than 0.
  // Any requests to update the replica will be rejected until this time. For testing only.
  MonoTime withold_replica_updates_until_ = MonoTime::kUninitialized;

  std::atomic<RejectMode> reject_mode_{RejectMode::kNone};

  CoarseTimePoint disable_pre_elections_until_ = CoarseTimePoint::min();

  std::atomic<MonoDelta> TEST_delay_update_{MonoDelta::kZero};

  std::atomic<uint64_t> majority_num_sst_files_{0};

  DISALLOW_COPY_AND_ASSIGN(RaftConsensus);
};

}  // namespace consensus
}  // namespace yb

#endif /* YB_CONSENSUS_RAFT_CONSENSUS_H_ */
