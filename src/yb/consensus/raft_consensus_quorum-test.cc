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

#include <gtest/gtest.h>

#include "yb/common/schema.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/consensus/consensus-test-util.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_index.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/peer_manager.h"
#include "yb/consensus/quorum_util.h"
#include "yb/consensus/raft_consensus.h"
#include "yb/consensus/replica_state.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/strcat.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/consensus/log_reader.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_context.h"
#include "yb/server/metadata.h"
#include "yb/server/logical_clock.h"
#include "yb/util/auto_release_pool.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"
#include "yb/util/threadpool.h"

DECLARE_int32(raft_heartbeat_interval_ms);
DECLARE_bool(enable_leader_failure_detection);

METRIC_DECLARE_entity(tablet);

#define REPLICATE_SEQUENCE_OF_MESSAGES(...) \
  ASSERT_NO_FATALS(ReplicateSequenceOfMessages(__VA_ARGS__))

using std::shared_ptr;
using std::unique_ptr;

namespace yb {

namespace consensus {

using log::Log;
using log::LogEntryPB;
using log::LogOptions;
using log::LogReader;
using rpc::RpcContext;
using strings::Substitute;
using strings::SubstituteAndAppend;

const char* kTestTable = "TestTable";
const char* kTestTablet = "TestTablet";

void DoNothing(std::shared_ptr<consensus::StateChangeContext> context) {
}

// Test suite for tests that focus on multiple peer interaction, but
// without integrating with other components, such as transactions.
class RaftConsensusQuorumTest : public YBTest {
 public:
  RaftConsensusQuorumTest()
    : clock_(server::LogicalClock::CreateStartingAt(HybridTime(0))),
      metric_entity_(METRIC_ENTITY_tablet.Instantiate(&metric_registry_, "raft-test")),
      schema_(GetSimpleTestSchema()) {
    options_.tablet_id = kTestTablet;
    FLAGS_enable_leader_failure_detection = false;
  }

  // Builds an initial configuration of 'num' elements.
  // All of the peers start as followers.
  void BuildInitialRaftConfigPB(int num) {
    config_ = BuildRaftConfigPBForTests(num);
    config_.set_opid_index(kInvalidOpIdIndex);
    peers_.reset(new TestPeerMapManager(config_));
  }

  Status BuildFsManagersAndLogs() {
    // Build the fsmanagers and logs
    for (int i = 0; i < config_.peers_size(); i++) {
      shared_ptr<MemTracker> parent_mem_tracker =
          MemTracker::CreateTracker(Substitute("peer-$0", i));
      parent_mem_trackers_.push_back(parent_mem_tracker);
      string test_path = GetTestPath(Substitute("peer-$0-root", i));
      FsManagerOpts opts;
      opts.parent_mem_tracker = parent_mem_tracker;
      opts.wal_paths = { test_path };
      opts.data_paths = { test_path };
      opts.server_type = "tserver_test";
      gscoped_ptr<FsManager> fs_manager(new FsManager(env_.get(), opts));
      RETURN_NOT_OK(fs_manager->CreateInitialFileSystemLayout());
      RETURN_NOT_OK(fs_manager->Open());

      scoped_refptr<Log> log;
      RETURN_NOT_OK(Log::Open(LogOptions(),
                              kTestTablet,
                              fs_manager->GetFirstTabletWalDirOrDie(kTestTable, kTestTablet),
                              fs_manager->uuid(),
                              schema_,
                              0, // schema_version
                              nullptr, // metric_entity
                              log_thread_pool_.get(),
                              log_thread_pool_.get(),
                              std::numeric_limits<int64_t>::max(), // cdc_min_replicated_index
                              &log));
      logs_.push_back(log.get());
      fs_managers_.push_back(fs_manager.release());
    }
    return Status::OK();
  }

  void BuildPeers() {
    vector<LocalTestPeerProxyFactory*> proxy_factories;
    for (int i = 0; i < config_.peers_size(); i++) {
      auto proxy_factory = std::make_unique<LocalTestPeerProxyFactory>(peers_.get());
      proxy_factories.push_back(proxy_factory.get());

      auto operation_factory = new TestOperationFactory();

      string peer_uuid = Substitute("peer-$0", i);

      std::unique_ptr<ConsensusMetadata> cmeta;
      ASSERT_OK(ConsensusMetadata::Create(fs_managers_[i], kTestTablet, peer_uuid, config_,
                                         kMinimumTerm, &cmeta));

      RaftPeerPB local_peer_pb;
      ASSERT_OK(GetRaftConfigMember(config_, peer_uuid, &local_peer_pb));
      auto queue = std::make_unique<PeerMessageQueue>(
          metric_entity_,
          logs_[i],
          MemTracker::FindOrCreateTracker(peer_uuid),
          MemTracker::FindOrCreateTracker(peer_uuid),
          local_peer_pb,
          kTestTablet,
          clock_,
          nullptr /* consensus_context */,
          raft_pool_->NewToken(ThreadPool::ExecutionMode::SERIAL));

      unique_ptr<ThreadPoolToken> pool_token(
          raft_pool_->NewToken(ThreadPool::ExecutionMode::CONCURRENT));

      auto peer_manager = std::make_unique<PeerManager>(
          options_.tablet_id,
          config_.peers(i).permanent_uuid(),
          proxy_factory.get(),
          queue.get(),
          pool_token.get(),
          logs_[i]);

      shared_ptr<RaftConsensus> peer(new RaftConsensus(
          options_,
          std::move(cmeta),
          std::move(proxy_factory),
          std::move(queue),
          std::move(peer_manager),
          std::move(pool_token),
          metric_entity_,
          config_.peers(i).permanent_uuid(),
          clock_,
          operation_factory,
          logs_[i],
          parent_mem_trackers_[i],
          Bind(&DoNothing),
          DEFAULT_TABLE_TYPE,
          nullptr /* retryable_requests */,
          SplitOpInfo()));

      operation_factory->SetConsensus(peer.get());
      operation_factories_.emplace_back(operation_factory);
      peers_->AddPeer(config_.peers(i).permanent_uuid(), peer);
    }
  }

  Status StartPeers() {
    ConsensusBootstrapInfo boot_info;

    TestPeerMap all_peers = peers_->GetPeerMapCopy();
    for (const TestPeerMap::value_type& entry : all_peers) {
      RETURN_NOT_OK(entry.second->Start(boot_info));
    }
    return Status::OK();
  }

  Status BuildConfig(int num) {
    RETURN_NOT_OK(ThreadPoolBuilder("raft").Build(&raft_pool_));
    RETURN_NOT_OK(ThreadPoolBuilder("log").Build(&log_thread_pool_));
    BuildInitialRaftConfigPB(num);
    RETURN_NOT_OK(BuildFsManagersAndLogs());
    BuildPeers();
    return Status::OK();
  }

  Status BuildAndStartConfig(int num) {
    RETURN_NOT_OK(BuildConfig(num));
    RETURN_NOT_OK(StartPeers());

    // Automatically elect the last node in the list.
    const int kLeaderIdx = num - 1;
    shared_ptr<RaftConsensus> leader;
    RETURN_NOT_OK(peers_->GetPeerByIdx(kLeaderIdx, &leader));
    RETURN_NOT_OK(leader->EmulateElection());
    RETURN_NOT_OK(leader->WaitUntilLeaderForTests(MonoDelta::FromSeconds(10)));
    return Status::OK();
  }

  LocalTestPeerProxy* GetLeaderProxyToPeer(int peer_idx, int leader_idx) {
    shared_ptr<RaftConsensus> follower;
    CHECK_OK(peers_->GetPeerByIdx(peer_idx, &follower));
    shared_ptr<RaftConsensus> leader;
    CHECK_OK(peers_->GetPeerByIdx(leader_idx, &leader));
    for (LocalTestPeerProxy* proxy : down_cast<LocalTestPeerProxyFactory*>(
        leader->peer_proxy_factory_.get())->GetProxies()) {
      if (proxy->GetTarget() == follower->peer_uuid()) {
        return proxy;
      }
    }
    CHECK(false) << "Proxy not found";
    return nullptr;
  }

  Status AppendDummyMessage(int peer_idx,
                            scoped_refptr<ConsensusRound>* round) {
    auto msg = std::make_shared<ReplicateMsg>();
    msg->set_op_type(NO_OP);
    msg->mutable_noop_request();
    msg->set_hybrid_time(clock_->Now().ToUint64());

    shared_ptr<RaftConsensus> peer;
    CHECK_OK(peers_->GetPeerByIdx(peer_idx, &peer));

    // Use a latch in place of a Transaction callback.
    gscoped_ptr<Synchronizer> sync(new Synchronizer());
    *round = peer->NewRound(std::move(msg),
        [sync = sync.get()](const Status& status, int64_t, OpIds*) {
      sync->StatusCB(status);
    });
    (**round).BindToTerm(peer->LeaderTerm());
    InsertOrDie(&syncs_, round->get(), sync.release());
    RETURN_NOT_OK_PREPEND(peer->TEST_Replicate(round->get()),
                          Substitute("Unable to replicate to peer $0", peer_idx));
    return Status::OK();
  }

  Status WaitForReplicate(ConsensusRound* round) {
    return FindOrDie(syncs_, round)->Wait();
  }

  Status TimedWaitForReplicate(ConsensusRound* round, const MonoDelta& delta) {
    return FindOrDie(syncs_, round)->WaitFor(delta);
  }

  void WaitForReplicateIfNotAlreadyPresent(const OpIdPB& to_wait_for, int peer_idx) {
    shared_ptr<RaftConsensus> peer;
    ASSERT_OK(peers_->GetPeerByIdx(peer_idx, &peer));
    ReplicaState* state = peer->GetReplicaStateForTests();
    while (true) {
      {
        auto lock = state->LockForRead();
        if (state->GetLastReceivedOpIdUnlocked().index >= to_wait_for.index()) {
          return;
        }
      }
      SleepFor(MonoDelta::FromMilliseconds(1));
    }
  }

  // Waits for an operation to be (database) committed in the replica at index
  // 'peer_idx'. If the operation was already committed this returns immediately.
  void WaitForCommitIfNotAlreadyPresent(const OpIdPB& to_wait_for,
                                        int peer_idx,
                                        int leader_idx) {
    MonoDelta timeout(MonoDelta::FromSeconds(10));
    MonoTime start(MonoTime::Now());

    shared_ptr<RaftConsensus> peer;
    ASSERT_OK(peers_->GetPeerByIdx(peer_idx, &peer));
    ReplicaState* state = peer->GetReplicaStateForTests();

    int backoff_exp = 0;
    const int kMaxBackoffExp = 8;
    OpIdPB committed_op_id;
    while (true) {
      {
        auto lock = state->LockForRead();
        state->GetCommittedOpIdUnlocked().ToPB(&committed_op_id);
        if (OpIdCompare(committed_op_id, to_wait_for) >= 0) {
          return;
        }
      }
      MonoDelta elapsed = MonoTime::Now().GetDeltaSince(start);
      if (elapsed.MoreThan(timeout)) {
        break;
      }
      SleepFor(MonoDelta::FromMilliseconds(1 << backoff_exp));
      backoff_exp = std::min(backoff_exp + 1, kMaxBackoffExp);
    }

    LOG(ERROR) << "Max timeout reached (" << timeout.ToString() << ") while waiting for commit of "
               << "op " << to_wait_for << " on replica. Last committed op on replica: "
               << committed_op_id << ". Dumping state and quitting.";
    vector<string> lines;
    shared_ptr<RaftConsensus> leader;
    ASSERT_OK(peers_->GetPeerByIdx(leader_idx, &leader));
    for (const string& line : lines) {
      LOG(ERROR) << line;
    }

    // Gather the replica and leader operations for printing
    log::LogEntries replica_ops = GatherLogEntries(peer_idx, logs_[peer_idx]);
    log::LogEntries leader_ops = GatherLogEntries(leader_idx, logs_[leader_idx]);
    SCOPED_TRACE(PrintOnError(replica_ops, Substitute("local peer ($0)", peer->peer_uuid())));
    SCOPED_TRACE(PrintOnError(leader_ops, Substitute("leader (peer-$0)", leader_idx)));
    FAIL() << "Replica did not commit.";
  }

  // Used in ReplicateSequenceOfMessages() to specify whether
  // we should wait for all replicas to have replicated the
  // sequence or just a majority.
  enum ReplicateWaitMode {
    WAIT_FOR_ALL_REPLICAS,
    WAIT_FOR_MAJORITY
  };

  // Used in ReplicateSequenceOfMessages() to specify whether
  // we should also commit the messages in the sequence
  enum CommitMode {
    DONT_COMMIT,
    COMMIT_ONE_BY_ONE
  };

  // Replicates a sequence of messages to the peer passed as leader.
  // Optionally waits for the messages to be replicated to followers.
  // 'last_op_id' is set to the id of the last replicated operation.
  // The operations are only committed if 'commit_one_by_one' is true.
  void ReplicateSequenceOfMessages(int seq_size,
                                   int leader_idx,
                                   ReplicateWaitMode wait_mode,
                                   CommitMode commit_mode,
                                   OpIdPB* last_op_id,
                                   vector<scoped_refptr<ConsensusRound> >* rounds) {
    for (int i = 0; i < seq_size; i++) {
      scoped_refptr<ConsensusRound> round;
      ASSERT_OK(AppendDummyMessage(leader_idx, &round));
      ASSERT_OK(WaitForReplicate(round.get()));
      last_op_id->CopyFrom(round->id());
      rounds->push_back(round);
    }

    if (wait_mode == WAIT_FOR_ALL_REPLICAS) {
      shared_ptr<RaftConsensus> leader;
      ASSERT_OK(peers_->GetPeerByIdx(leader_idx, &leader));

      TestPeerMap all_peers = peers_->GetPeerMapCopy();
      int i = 0;
      for (const TestPeerMap::value_type& entry : all_peers) {
        if (entry.second->peer_uuid() != leader->peer_uuid()) {
          WaitForReplicateIfNotAlreadyPresent(*last_op_id, i);
        }
        i++;
      }
    }
  }

  log::LogEntries GatherLogEntries(int idx, const scoped_refptr<Log>& log) {
    EXPECT_OK(log->WaitUntilAllFlushed());
    EXPECT_OK(log->Close());
    std::unique_ptr<LogReader> log_reader;
    EXPECT_OK(log::LogReader::Open(fs_managers_[idx]->env(),
                                   scoped_refptr<log::LogIndex>(),
                                   kTestTablet,
                                   fs_managers_[idx]->GetFirstTabletWalDirOrDie(kTestTable,
                                                                                kTestTablet),
                                   fs_managers_[idx]->uuid(),
                                   metric_entity_.get(),
                                   &log_reader));
    log::LogEntries ret;
    log::SegmentSequence segments;
    EXPECT_OK(log_reader->GetSegmentsSnapshot(&segments));

    for (const log::SegmentSequence::value_type& entry : segments) {
      auto result = entry->ReadEntries();
      EXPECT_OK(result.status);
      for (auto& e : result.entries) {
        ret.push_back(std::move(e));
      }
    }

    return ret;
  }

  // Verifies that the replica's log match the leader's. This deletes the
  // peers (so we're sure that no further writes occur) and closes the logs
  // so it must be the very last thing to run, in a test.
  void VerifyLogs(int leader_idx, int first_replica_idx, int last_replica_idx) {
    // Wait for in-flight transactions to be done. We're destroying the
    // peers next and leader transactions won't be able to commit anymore.
    for (const auto& factory : operation_factories_) {
      factory->WaitDone();
    }

    // Shut down all the peers.
    TestPeerMap all_peers = peers_->GetPeerMapCopy();
    for (const TestPeerMap::value_type& entry : all_peers) {
      entry.second->Shutdown();
    }

    log::LogEntries leader_entries = GatherLogEntries(leader_idx, logs_[leader_idx]);
    shared_ptr<RaftConsensus> leader;
    ASSERT_OK(peers_->GetPeerByIdx(leader_idx, &leader));

    for (int replica_idx = first_replica_idx; replica_idx < last_replica_idx; replica_idx++) {
      log::LogEntries replica_entries = GatherLogEntries(replica_idx, logs_[replica_idx]);

      shared_ptr<RaftConsensus> replica;
      ASSERT_OK(peers_->GetPeerByIdx(replica_idx, &replica));
      VerifyReplica(leader_entries,
                    replica_entries,
                    leader->peer_uuid(),
                    replica->peer_uuid());
    }
  }

  std::vector<OpIdPB> ExtractReplicateIds(const log::LogEntries& entries) {
    std::vector<OpIdPB> result;
    result.reserve(entries.size() / 2);
    for (const auto& entry : entries) {
      if (entry->has_replicate()) {
        result.push_back(entry->replicate().id());
      }
    }
    return result;
  }

  void VerifyReplicateOrderMatches(const log::LogEntries& leader_entries,
                                   const log::LogEntries& replica_entries) {
    auto leader_ids = ExtractReplicateIds(leader_entries);
    auto replica_ids = ExtractReplicateIds(replica_entries);
    ASSERT_EQ(leader_ids.size(), replica_ids.size());
    for (int i = 0; i < leader_ids.size(); i++) {
      ASSERT_EQ(leader_ids[i].ShortDebugString(),
                replica_ids[i].ShortDebugString());
    }
  }

  void VerifyNoCommitsBeforeReplicates(const log::LogEntries& entries) {
    unordered_set<OpIdPB,
                  OpIdHashFunctor,
                  OpIdEqualsFunctor> replication_ops;

    for (const auto& entry : entries) {
      if (entry->has_replicate()) {
        ASSERT_TRUE(InsertIfNotPresent(&replication_ops, entry->replicate().id()))
          << "REPLICATE op id showed up twice: " << entry->ShortDebugString();
      }
    }
  }

  void VerifyReplica(const log::LogEntries& leader_entries,
                     const log::LogEntries& replica_entries,
                     const string& leader_name,
                     const string& replica_name) {
    SCOPED_TRACE(PrintOnError(leader_entries, Substitute("Leader: $0", leader_name)));
    SCOPED_TRACE(PrintOnError(replica_entries, Substitute("Replica: $0", replica_name)));

    // Check that the REPLICATE messages come in the same order on both nodes.
    VerifyReplicateOrderMatches(leader_entries, replica_entries);

    // Check that no COMMIT precedes its related REPLICATE on both the replica
    // and leader.
    VerifyNoCommitsBeforeReplicates(replica_entries);
    VerifyNoCommitsBeforeReplicates(leader_entries);
  }

  string PrintOnError(const log::LogEntries& replica_entries,
                      const string& replica_id) {
    string ret = "";
    SubstituteAndAppend(&ret, "$1 log entries for replica $0:\n",
                        replica_id, replica_entries.size());
    for (const auto& replica_entry : replica_entries) {
      StrAppend(&ret, "Replica log entry: ", replica_entry->ShortDebugString(), "\n");
    }
    return ret;
  }

  // Read the ConsensusMetadata for the given peer from disk.
  std::unique_ptr<ConsensusMetadata> ReadConsensusMetadataFromDisk(int peer_index) {
    string peer_uuid = Substitute("peer-$0", peer_index);
    std::unique_ptr<ConsensusMetadata> cmeta;
    CHECK_OK(ConsensusMetadata::Load(fs_managers_[peer_index], kTestTablet, peer_uuid, &cmeta));
    return cmeta;
  }

  // Assert that the durable term == term and that the peer that got the vote == voted_for.
  void AssertDurableTermAndVote(int peer_index, int64_t term, const std::string& voted_for) {
    auto cmeta = ReadConsensusMetadataFromDisk(peer_index);
    ASSERT_EQ(term, cmeta->current_term());
    ASSERT_EQ(voted_for, cmeta->voted_for());
  }

  // Assert that the durable term == term and that the peer has not yet voted.
  void AssertDurableTermWithoutVote(int peer_index, int64_t term) {
    auto cmeta = ReadConsensusMetadataFromDisk(peer_index);
    ASSERT_EQ(term, cmeta->current_term());
    ASSERT_FALSE(cmeta->has_voted_for());
  }

  ~RaftConsensusQuorumTest() {
    peers_->Clear();
    operation_factories_.clear();
    // We need to clear the logs before deleting the fs_managers_ or we'll
    // get a SIGSEGV when closing the logs.
    logs_.clear();
    STLDeleteElements(&fs_managers_);
    STLDeleteValues(&syncs_);
  }

 protected:
  ConsensusOptions options_;
  RaftConfigPB config_;
  OpIdPB initial_id_;
  vector<shared_ptr<MemTracker> > parent_mem_trackers_;
  vector<FsManager*> fs_managers_;
  vector<scoped_refptr<Log> > logs_;
  unique_ptr<ThreadPool> raft_pool_;
  unique_ptr<ThreadPool> log_thread_pool_;
  gscoped_ptr<TestPeerMapManager> peers_;
  std::vector<std::unique_ptr<TestOperationFactory>> operation_factories_;
  scoped_refptr<server::Clock> clock_;
  MetricRegistry metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  const Schema schema_;
  unordered_map<ConsensusRound*, Synchronizer*> syncs_;
};

TEST_F(RaftConsensusQuorumTest, TestConsensusContinuesIfAMinorityFallsBehind) {
  // Constants with the indexes of peers with certain roles,
  // since peers don't change roles in this test.
  const int kFollower0Idx = 0;
  const int kFollower1Idx = 1;
  const int kLeaderIdx = 2;

  ASSERT_OK(BuildAndStartConfig(3));

  OpIdPB last_replicate;
  vector<scoped_refptr<ConsensusRound> > rounds;
  {
    // lock one of the replicas down by obtaining the state lock
    // and never letting it go.
    shared_ptr<RaftConsensus> follower0;
    ASSERT_OK(peers_->GetPeerByIdx(kFollower0Idx, &follower0));

    ReplicaState* follower0_rs = follower0->GetReplicaStateForTests();
    auto lock = follower0_rs->LockForRead();

    // If the locked replica would stop consensus we would hang here
    // as we wait for operations to be replicated to a majority.
    ASSERT_NO_FATALS(ReplicateSequenceOfMessages(
                              10,
                              kLeaderIdx,
                              WAIT_FOR_MAJORITY,
                              COMMIT_ONE_BY_ONE,
                              &last_replicate,
                              &rounds));

    // Follower 1 should be fine (Were we to wait for follower0's replicate
    // this would hang here). We know he must have replicated but make sure
    // by calling Wait().
    WaitForReplicateIfNotAlreadyPresent(last_replicate, kFollower1Idx);
    WaitForCommitIfNotAlreadyPresent(last_replicate, kFollower1Idx, kLeaderIdx);
  }

  // After we let the lock go the remaining follower should get up-to-date
  WaitForReplicateIfNotAlreadyPresent(last_replicate, kFollower0Idx);
  WaitForCommitIfNotAlreadyPresent(last_replicate, kFollower0Idx, kLeaderIdx);
  VerifyLogs(2, 0, 1);
}

TEST_F(RaftConsensusQuorumTest, TestConsensusStopsIfAMajorityFallsBehind) {
  // Constants with the indexes of peers with certain roles,
  // since peers don't change roles in this test.
  const int kFollower0Idx = 0;
  const int kFollower1Idx = 1;
  const int kLeaderIdx = 2;

  ASSERT_OK(BuildAndStartConfig(3));

  OpIdPB last_op_id;

  scoped_refptr<ConsensusRound> round;
  {
    // lock two of the replicas down by obtaining the state locks
    // and never letting them go.
    shared_ptr<RaftConsensus> follower0;
    ASSERT_OK(peers_->GetPeerByIdx(kFollower0Idx, &follower0));
    ReplicaState* follower0_rs = follower0->GetReplicaStateForTests();
    auto lock0 = follower0_rs->LockForRead();

    shared_ptr<RaftConsensus> follower1;
    ASSERT_OK(peers_->GetPeerByIdx(kFollower1Idx, &follower1));
    ReplicaState* follower1_rs = follower1->GetReplicaStateForTests();
    auto lock1 = follower1_rs->LockForRead();

    // Append a single message to the queue
    ASSERT_OK(AppendDummyMessage(kLeaderIdx, &round));
    last_op_id.CopyFrom(round->id());
    // This should timeout.
    Status status = TimedWaitForReplicate(round.get(), MonoDelta::FromMilliseconds(500));
    ASSERT_TRUE(status.IsTimedOut());
  }

  // After we release the locks the operation should replicate to all replicas
  // and we commit.
  ASSERT_OK(WaitForReplicate(round.get()));

  // Assert that everything was ok
  WaitForReplicateIfNotAlreadyPresent(last_op_id, kFollower0Idx);
  WaitForReplicateIfNotAlreadyPresent(last_op_id, kFollower1Idx);
  WaitForCommitIfNotAlreadyPresent(last_op_id, kFollower0Idx, kLeaderIdx);
  WaitForCommitIfNotAlreadyPresent(last_op_id, kFollower1Idx, kLeaderIdx);
  VerifyLogs(2, 0, 1);
}

// If some communication error happens the leader will resend the request to the
// peers. This tests that the peers handle repeated requests.
TEST_F(RaftConsensusQuorumTest, TestReplicasHandleCommunicationErrors) {
  // Constants with the indexes of peers with certain roles,
  // since peers don't change roles in this test.
  const int kFollower0Idx = 0;
  const int kFollower1Idx = 1;
  const int kLeaderIdx = 2;

  ASSERT_OK(BuildAndStartConfig(3));

  OpIdPB last_op_id;

  // Append a dummy message, with faults injected on the first attempt
  // to send the message.
  scoped_refptr<ConsensusRound> round;
  GetLeaderProxyToPeer(kFollower0Idx, kLeaderIdx)->InjectCommFaultLeaderSide();
  GetLeaderProxyToPeer(kFollower1Idx, kLeaderIdx)->InjectCommFaultLeaderSide();
  ASSERT_OK(AppendDummyMessage(kLeaderIdx, &round));

  // We should successfully replicate it due to retries.
  ASSERT_OK(WaitForReplicate(round.get()));

  GetLeaderProxyToPeer(kFollower0Idx, kLeaderIdx)->InjectCommFaultLeaderSide();
  GetLeaderProxyToPeer(kFollower1Idx, kLeaderIdx)->InjectCommFaultLeaderSide();

  // The commit should eventually reach both followers as well.
  last_op_id = round->id();
  WaitForCommitIfNotAlreadyPresent(last_op_id, kFollower0Idx, kLeaderIdx);
  WaitForCommitIfNotAlreadyPresent(last_op_id, kFollower1Idx, kLeaderIdx);

  // Append a sequence of messages, and keep injecting errors into the
  // replica proxies.
  vector<scoped_refptr<ConsensusRound> > rounds;
  for (int i = 0; i < 100; i++) {
    scoped_refptr<ConsensusRound> round;
    ASSERT_OK(AppendDummyMessage(kLeaderIdx, &round));
    ConsensusRound* round_ptr = round.get();
    last_op_id.CopyFrom(round->id());
    rounds.push_back(round);

    // inject comm faults
    if (i % 2 == 0) {
      GetLeaderProxyToPeer(kFollower0Idx, kLeaderIdx)->InjectCommFaultLeaderSide();
    } else {
      GetLeaderProxyToPeer(kFollower1Idx, kLeaderIdx)->InjectCommFaultLeaderSide();
    }

    ASSERT_OK(WaitForReplicate(round_ptr));
  }

  // Assert last operation was correctly replicated and committed.
  WaitForReplicateIfNotAlreadyPresent(last_op_id, kFollower0Idx);
  WaitForReplicateIfNotAlreadyPresent(last_op_id, kFollower1Idx);

  // See comment at the end of TestFollowersReplicateAndCommitMessage
  // for an explanation on this waiting sequence.
  WaitForCommitIfNotAlreadyPresent(last_op_id, kFollower0Idx, kLeaderIdx);
  WaitForCommitIfNotAlreadyPresent(last_op_id, kFollower1Idx, kLeaderIdx);
  VerifyLogs(2, 0, 1);
}

// In this test we test the ability of the leader to send heartbeats
// to replicas by simply pushing nothing after the configuration round
// and still expecting for the replicas Update() hooks to be called.
TEST_F(RaftConsensusQuorumTest, TestLeaderHeartbeats) {
  // Constants with the indexes of peers with certain roles,
  // since peers don't change roles in this test.
  const int kFollower0Idx = 0;
  const int kFollower1Idx = 1;
  const int kLeaderIdx = 2;

  ASSERT_OK(BuildConfig(3));

  shared_ptr<RaftConsensus> follower0;
  ASSERT_OK(peers_->GetPeerByIdx(kFollower0Idx, &follower0));
  shared_ptr<RaftConsensus> follower1;
  ASSERT_OK(peers_->GetPeerByIdx(kFollower1Idx, &follower1));

  shared_ptr<CounterHooks> counter_hook_rpl0(
      new CounterHooks(follower0->GetFaultHooks()));
  shared_ptr<CounterHooks> counter_hook_rpl1(
      new CounterHooks(follower1->GetFaultHooks()));

  // Replace the default fault hooks on the replicas with counter hooks
  // before we start the configuration.
  follower0->SetFaultHooks(counter_hook_rpl0);
  follower1->SetFaultHooks(counter_hook_rpl1);

  ASSERT_OK(StartPeers());

  shared_ptr<RaftConsensus> leader;
  ASSERT_OK(peers_->GetPeerByIdx(kLeaderIdx, &leader));
  ASSERT_OK(leader->EmulateElection());

  // Wait for the config round to get committed and count the number
  // of update calls, calls after that will be heartbeats.
  OpIdPB config_round;
  config_round.set_term(1);
  config_round.set_index(1);
  WaitForCommitIfNotAlreadyPresent(config_round, kFollower0Idx, kLeaderIdx);
  WaitForCommitIfNotAlreadyPresent(config_round, kFollower1Idx, kLeaderIdx);

  int repl0_init_count = counter_hook_rpl0->num_pre_update_calls();
  int repl1_init_count = counter_hook_rpl1->num_pre_update_calls();

  // Now wait for about 4 times the heartbeat period the counters
  // should have increased between 3 to 8 times.
  //
  // Why the variance? Heartbeat timing is jittered such that the period
  // between heartbeats can be anywhere from half the interval to the full interval.
  SleepFor(MonoDelta::FromMilliseconds(FLAGS_raft_heartbeat_interval_ms * 4));

  int repl0_final_count = counter_hook_rpl0->num_pre_update_calls();
  int repl1_final_count = counter_hook_rpl1->num_pre_update_calls();

  ASSERT_GE(repl0_final_count - repl0_init_count, 3);
  ASSERT_LE(repl0_final_count - repl0_init_count, 8);
  ASSERT_GE(repl1_final_count - repl1_init_count, 3);
  ASSERT_LE(repl1_final_count - repl1_init_count, 8);

  VerifyLogs(2, 0, 1);
}

// After creating the initial configuration, this test writes a small sequence
// of messages to the initial leader. It then shuts down the current
// leader, makes another peer become leader and writes a sequence of
// messages to it. The new leader and the follower should agree on the
// sequence of messages.
TEST_F(RaftConsensusQuorumTest, TestLeaderElectionWithQuiescedQuorum) {
  const int kInitialNumPeers = 5;
  ASSERT_OK(BuildAndStartConfig(kInitialNumPeers));

  OpIdPB last_op_id;
  vector<scoped_refptr<ConsensusRound> > rounds;

  // Loop twice, successively shutting down the previous leader.
  for (int current_config_size = kInitialNumPeers;
       current_config_size >= kInitialNumPeers - 1;
       current_config_size--) {
    REPLICATE_SEQUENCE_OF_MESSAGES(10,
                                   current_config_size - 1, // The index of the leader.
                                   WAIT_FOR_ALL_REPLICAS,
                                   COMMIT_ONE_BY_ONE,
                                   &last_op_id,
                                   &rounds);

    // Make sure the last operation is committed everywhere
    for (int i = 0; i < current_config_size - 1; i++) {
      WaitForCommitIfNotAlreadyPresent(last_op_id, i, current_config_size - 1);
    }

    // Now shutdown the current leader.
    LOG(INFO) << "Shutting down current leader with index " << (current_config_size - 1);
    shared_ptr<RaftConsensus> current_leader;
    ASSERT_OK(peers_->GetPeerByIdx(current_config_size - 1, &current_leader));
    current_leader->Shutdown();
    peers_->RemovePeer(current_leader->peer_uuid());

    // ... and make the peer before it become leader.
    shared_ptr<RaftConsensus> new_leader;
    ASSERT_OK(peers_->GetPeerByIdx(current_config_size - 2, &new_leader));

    // This will force an election in which we expect to make the last
    // non-shutdown peer in the list become leader.
    LOG(INFO) << "Running election for future leader with index " << (current_config_size - 1);
    ASSERT_OK(new_leader->StartElection({ElectionMode::ELECT_EVEN_IF_LEADER_IS_ALIVE}));
    ASSERT_OK(new_leader->WaitUntilLeaderForTests(MonoDelta::FromSeconds(15)));
    LOG(INFO) << "Election won";

    // ... replicating a set of messages to the new leader should now be possible.
    REPLICATE_SEQUENCE_OF_MESSAGES(10,
                                   current_config_size - 2, // The index of the new leader.
                                   WAIT_FOR_MAJORITY,
                                   COMMIT_ONE_BY_ONE,
                                   &last_op_id,
                                   &rounds);

    // Make sure the last operation is committed everywhere
    for (int i = 0; i < current_config_size - 2; i++) {
      WaitForCommitIfNotAlreadyPresent(last_op_id, i, current_config_size - 2);
    }
  }
  // We can only verify the logs of the peers that were not killed, due to the
  // old leaders being out-of-date now.
  VerifyLogs(2, 0, 1);
}

TEST_F(RaftConsensusQuorumTest, TestReplicasEnforceTheLogMatchingProperty) {
  ASSERT_OK(BuildAndStartConfig(3));

  OpIdPB last_op_id;
  vector<scoped_refptr<ConsensusRound> > rounds;
  REPLICATE_SEQUENCE_OF_MESSAGES(10,
                                 2, // The index of the initial leader.
                                 WAIT_FOR_ALL_REPLICAS,
                                 COMMIT_ONE_BY_ONE,
                                 &last_op_id,
                                 &rounds);

  // Make sure the last operation is committed everywhere
  WaitForCommitIfNotAlreadyPresent(last_op_id, 0, 2);
  WaitForCommitIfNotAlreadyPresent(last_op_id, 1, 2);

  // Now replicas should only accept operations with
  // 'last_id' as the preceding id.
  ConsensusRequestPB req;
  ConsensusResponsePB resp;

  shared_ptr<RaftConsensus> leader;
  ASSERT_OK(peers_->GetPeerByIdx(2, &leader));

  shared_ptr<RaftConsensus> follower;
  ASSERT_OK(peers_->GetPeerByIdx(0, &follower));

  req.set_caller_uuid(leader->peer_uuid());
  req.set_caller_term(last_op_id.term());
  req.mutable_preceding_id()->CopyFrom(last_op_id);
  req.mutable_committed_op_id()->CopyFrom(last_op_id);

  ReplicateMsg* replicate = req.add_ops();
  replicate->set_hybrid_time(clock_->Now().ToUint64());
  OpIdPB* id = replicate->mutable_id();
  id->set_term(last_op_id.term());
  id->set_index(last_op_id.index() + 1);
  // Make a copy of the OpId to be TSAN friendly.
  auto req_copy = req;
  auto id_copy = req_copy.mutable_ops(0)->mutable_id();
  replicate->set_op_type(NO_OP);

  // Appending this message to peer0 should work and update
  // its 'last_received' to 'id'.
  ASSERT_OK(follower->Update(&req, &resp, CoarseBigDeadline()));
  ASSERT_TRUE(OpIdEquals(resp.status().last_received(), *id));

  // Now skip one message in the same term. The replica should
  // complain with the right error message.
  req_copy.mutable_preceding_id()->set_index(id_copy->index() + 1);
  id_copy->set_index(id_copy->index() + 2);
  // Appending this message to peer0 should return a Status::OK
  // but should contain an error referring to the log matching property.
  ASSERT_OK(follower->Update(&req_copy, &resp, CoarseBigDeadline()));
  ASSERT_TRUE(resp.has_status());
  ASSERT_TRUE(resp.status().has_error());
  ASSERT_EQ(resp.status().error().code(), ConsensusErrorPB::PRECEDING_ENTRY_DIDNT_MATCH);
  ASSERT_STR_CONTAINS(resp.status().error().status().message(),
                      "Log matching property violated");
}

// Test that RequestVote performs according to "spec".
TEST_F(RaftConsensusQuorumTest, TestRequestVote) {
  ASSERT_OK(BuildAndStartConfig(3));

  OpIdPB last_op_id;
  vector<scoped_refptr<ConsensusRound> > rounds;
  REPLICATE_SEQUENCE_OF_MESSAGES(10,
                                 2, // The index of the initial leader.
                                 WAIT_FOR_ALL_REPLICAS,
                                 COMMIT_ONE_BY_ONE,
                                 &last_op_id,
                                 &rounds);

  // Make sure the last operation is committed everywhere
  WaitForCommitIfNotAlreadyPresent(last_op_id, 0, 2);
  WaitForCommitIfNotAlreadyPresent(last_op_id, 1, 2);

  // Ensure last-logged OpId is > (0,0).
  ASSERT_TRUE(OpIdLessThan(MinimumOpId(), last_op_id));

  const int kPeerIndex = 1;
  shared_ptr<RaftConsensus> peer;
  ASSERT_OK(peers_->GetPeerByIdx(kPeerIndex, &peer));

  VoteRequestPB request;
  request.set_tablet_id(kTestTablet);
  request.mutable_candidate_status()->mutable_last_received()->CopyFrom(last_op_id);

  // Test that the replica won't vote since it has recently heard from
  // a valid leader.
  VoteResponsePB response;
  request.set_candidate_uuid("peer-0");
  request.set_candidate_term(last_op_id.term() + 1);
  ASSERT_OK(peer->RequestVote(&request, &response));
  ASSERT_FALSE(response.vote_granted());
  ASSERT_EQ(ConsensusErrorPB::LEADER_IS_ALIVE, response.consensus_error().code());

  // Test that replicas only vote yes for a single peer per term.

  // Indicate that replicas should vote even if they think another leader is alive.
  // This will allow the rest of the requests in the test to go through.
  request.set_ignore_live_leader(true);
  ASSERT_OK(peer->RequestVote(&request, &response));
  ASSERT_TRUE(response.vote_granted());
  ASSERT_EQ(last_op_id.term() + 1, response.responder_term());
  ASSERT_NO_FATALS(AssertDurableTermAndVote(kPeerIndex, last_op_id.term() + 1, "peer-0"));

  // Ensure we get same response for same term and same UUID.
  response.Clear();
  ASSERT_OK(peer->RequestVote(&request, &response));
  ASSERT_TRUE(response.vote_granted());

  // Ensure we get a "no" for a different candidate UUID for that term.
  response.Clear();
  request.set_candidate_uuid("peer-2");
  ASSERT_OK(peer->RequestVote(&request, &response));
  ASSERT_FALSE(response.vote_granted());
  ASSERT_TRUE(response.has_consensus_error());
  ASSERT_EQ(ConsensusErrorPB::ALREADY_VOTED, response.consensus_error().code());
  ASSERT_EQ(last_op_id.term() + 1, response.responder_term());
  ASSERT_NO_FATALS(AssertDurableTermAndVote(kPeerIndex, last_op_id.term() + 1, "peer-0"));

  //
  // Test that replicas refuse votes for an old term.
  //

  // Increase the term of our candidate, which will cause the voter replica to
  // increase its own term to match.
  request.set_candidate_uuid("peer-0");
  request.set_candidate_term(last_op_id.term() + 2);
  response.Clear();
  ASSERT_OK(peer->RequestVote(&request, &response));
  ASSERT_TRUE(response.vote_granted());
  ASSERT_EQ(last_op_id.term() + 2, response.responder_term());
  ASSERT_NO_FATALS(AssertDurableTermAndVote(kPeerIndex, last_op_id.term() + 2, "peer-0"));

  // Now try the old term.
  // Note: Use the peer who "won" the election on the previous term (peer-0),
  // although in practice the impl does not store historical vote data.
  request.set_candidate_term(last_op_id.term() + 1);
  response.Clear();
  ASSERT_OK(peer->RequestVote(&request, &response));
  ASSERT_FALSE(response.vote_granted());
  ASSERT_TRUE(response.has_consensus_error());
  ASSERT_EQ(ConsensusErrorPB::INVALID_TERM, response.consensus_error().code());
  ASSERT_EQ(last_op_id.term() + 2, response.responder_term());
  ASSERT_NO_FATALS(AssertDurableTermAndVote(kPeerIndex, last_op_id.term() + 2, "peer-0"));

  //
  // Ensure replicas vote no for an old op index.
  //

  request.set_candidate_uuid("peer-0");
  request.set_candidate_term(last_op_id.term() + 3);
  request.mutable_candidate_status()->mutable_last_received()->CopyFrom(MinimumOpId());
  response.Clear();
  ASSERT_OK(peer->RequestVote(&request, &response));
  ASSERT_FALSE(response.vote_granted());
  ASSERT_TRUE(response.has_consensus_error());
  ASSERT_EQ(ConsensusErrorPB::LAST_OPID_TOO_OLD, response.consensus_error().code());
  ASSERT_EQ(last_op_id.term() + 3, response.responder_term());
  ASSERT_NO_FATALS(AssertDurableTermWithoutVote(kPeerIndex, last_op_id.term() + 3));

  // Send a "heartbeat" to the peer. It should be rejected.
  ConsensusRequestPB req;
  req.set_caller_term(last_op_id.term());
  req.set_caller_uuid("peer-0");
  req.mutable_committed_op_id()->CopyFrom(last_op_id);
  ConsensusResponsePB res;
  Status s = peer->Update(&req, &res, CoarseBigDeadline());
  ASSERT_EQ(last_op_id.term() + 3, res.responder_term());
  ASSERT_TRUE(res.status().has_error());
  ASSERT_EQ(ConsensusErrorPB::INVALID_TERM, res.status().error().code());
  LOG(INFO) << "Follower rejected old heartbeat, as expected: " << res.ShortDebugString();
}

}  // namespace consensus
}  // namespace yb
