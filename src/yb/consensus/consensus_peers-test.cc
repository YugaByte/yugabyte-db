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

#include <chrono>

#include <gtest/gtest.h>

#include "yb/common/schema.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/consensus-test-util.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/fs/fs_manager.h"
#include "yb/rpc/messenger.h"
#include "yb/server/hybrid_clock.h"
#include "yb/util/metrics.h"
#include "yb/util/opid.h"
#include "yb/util/scope_exit.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"
#include "yb/util/threadpool.h"

using namespace std::chrono_literals;

METRIC_DECLARE_entity(tablet);

namespace yb {
namespace consensus {

using log::Log;
using log::LogOptions;
using log::LogAnchorRegistry;
using rpc::Messenger;
using rpc::MessengerBuilder;
using std::shared_ptr;
using std::unique_ptr;

const char* kTableId = "test-peers-table";
const char* kTabletId = "test-peers-tablet";
const char* kLeaderUuid = "peer-0";
const char* kFollowerUuid = "peer-1";

class ConsensusPeersTest : public YBTest {
 public:
  ConsensusPeersTest()
      : metric_entity_(METRIC_ENTITY_tablet.Instantiate(&metric_registry_, "peer-test")),
        schema_(GetSimpleTestSchema()) {
  }

  void SetUp() override {
    YBTest::SetUp();
    MessengerBuilder bld("test");
    messenger_ = ASSERT_RESULT(bld.Build());
    ASSERT_OK(ThreadPoolBuilder("test-raft-pool").Build(&raft_pool_));
    raft_pool_token_ = raft_pool_->NewToken(ThreadPool::ExecutionMode::CONCURRENT);
    ASSERT_OK(ThreadPoolBuilder("log").Build(&log_thread_pool_));
    fs_manager_.reset(new FsManager(env_.get(), GetTestPath("fs_root"), "tserver_test"));

    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
    ASSERT_OK(Log::Open(options_,
                       kTabletId,
                       fs_manager_->GetFirstTabletWalDirOrDie(kTableId, kTabletId),
                       fs_manager_->uuid(),
                       schema_,
                       0, // schema_version
                       NULL,
                       log_thread_pool_.get(),
                       log_thread_pool_.get(),
                       std::numeric_limits<int64_t>::max(), // cdc_min_replicated_index
                       &log_));
    clock_.reset(new server::HybridClock());
    ASSERT_OK(clock_->Init());

    consensus_.reset(new TestRaftConsensusQueueIface());
    message_queue_.reset(new PeerMessageQueue(
        metric_entity_,
        log_.get(),
        nullptr /* server_tracker */,
        nullptr /* parent_tracker */,
        FakeRaftPeerPB(kLeaderUuid),
        kTabletId,
        clock_,
        nullptr /* consensus_context */,
        raft_pool_->NewToken(ThreadPool::ExecutionMode::SERIAL)));
    message_queue_->RegisterObserver(consensus_.get());

    message_queue_->Init(MinimumOpId());
    message_queue_->SetLeaderMode(MinimumOpId(),
                                  MinimumOpId().term(),
                                  OpId::Min(),
                                  BuildRaftConfigPBForTests(3));
  }

  void TearDown() override {
    ASSERT_OK(log_->WaitUntilAllFlushed());
    log_thread_pool_->Shutdown();
    raft_pool_->Shutdown();
    messenger_->Shutdown();
  }

  DelayablePeerProxy<NoOpTestPeerProxy>* NewRemotePeer(
      const string& peer_name,
      std::shared_ptr<Peer>* peer) {
    RaftPeerPB peer_pb;
    peer_pb.set_permanent_uuid(peer_name);
    auto proxy_ptr = new DelayablePeerProxy<NoOpTestPeerProxy>(
        raft_pool_.get(), new NoOpTestPeerProxy(raft_pool_.get(), peer_pb));
    *peer = CHECK_RESULT(Peer::NewRemotePeer(
        peer_pb, kTabletId, kLeaderUuid, PeerProxyPtr(proxy_ptr), message_queue_.get(),
        raft_pool_token_.get(), nullptr /* consensus */, messenger_.get()));
    return proxy_ptr;
  }

  void CheckLastLogEntry(int term, int index) {
    ASSERT_EQ(log_->GetLatestEntryOpId(), yb::OpId(term, index));
  }

  void CheckLastRemoteEntry(DelayablePeerProxy<NoOpTestPeerProxy>* proxy, int term, int index) {
    ASSERT_EQ(yb::OpId::FromPB(proxy->proxy()->last_received()), yb::OpId(term, index));
  }

 protected:
  unique_ptr<ThreadPool> raft_pool_;
  gscoped_ptr<TestRaftConsensusQueueIface> consensus_;
  MetricRegistry metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  gscoped_ptr<FsManager> fs_manager_;
  unique_ptr<ThreadPool> log_thread_pool_;
  scoped_refptr<Log> log_;
  gscoped_ptr<PeerMessageQueue> message_queue_;
  const Schema schema_;
  LogOptions options_;
  unique_ptr<ThreadPoolToken> raft_pool_token_;
  scoped_refptr<server::Clock> clock_;
  std::unique_ptr<Messenger> messenger_;
};

// Tests that a remote peer is correctly built and tracked
// by the message queue.
// After the operations are considered done the proxy (which
// simulates the other endpoint) should reflect the replicated
// messages.
TEST_F(ConsensusPeersTest, TestRemotePeer) {
  // We use a majority size of 2 since we make one fake remote peer
  // in addition to our real local log.

  std::shared_ptr<Peer> remote_peer;
  auto se = ScopeExit([&remote_peer] {
    // This guarantees that the Peer object doesn't get destroyed if there is a pending request.
    remote_peer->Close();
  });

  DelayablePeerProxy<NoOpTestPeerProxy>* proxy = NewRemotePeer(kFollowerUuid, &remote_peer);

  // Append a bunch of messages to the queue.
  AppendReplicateMessagesToQueue(message_queue_.get(), clock_, 1, 20);

  // The above append ends up appending messages in term 2, so we update the peer's term to match.
  remote_peer->SetTermForTest(2);

  // signal the peer there are requests pending.
  ASSERT_OK(remote_peer->SignalRequest(RequestTriggerMode::kNonEmptyOnly));

  // Now wait on the status of the last operation. This will complete once the peer has logged all
  // requests.
  consensus_->WaitForMajorityReplicatedIndex(20);
  // Verify that the replicated watermark corresponds to the last replicated message.
  CheckLastRemoteEntry(proxy, 2, 20);
}

TEST_F(ConsensusPeersTest, TestLocalAppendAndRemotePeerDelay) {
  // Create a set of remote peers.
  std::shared_ptr<Peer> remote_peer1;
  NewRemotePeer("peer-1", &remote_peer1);

  std::shared_ptr<Peer> remote_peer2;
  DelayablePeerProxy<NoOpTestPeerProxy>* remote_peer2_proxy =
      NewRemotePeer("peer-2", &remote_peer2);

  auto se = ScopeExit([&remote_peer1, &remote_peer2] {
    // This guarantees that the Peer objects don't get destroyed if there is a pending request.
    remote_peer1->Close();
    remote_peer2->Close();
  });

  // Delay the response from the second remote peer.
  const auto kAppendDelayTime = 1s;
  log_->TEST_SetSleepDuration(kAppendDelayTime);
  remote_peer2_proxy->DelayResponse();
  auto se2 = ScopeExit([this, &remote_peer2_proxy] {
    log_->TEST_SetSleepDuration(0s);
    remote_peer2_proxy->Respond(TestPeerProxy::kUpdate);
  });

  // Append one message to the queue.
  const auto start_time = MonoTime::Now();
  OpIdPB first = MakeOpId(0, 1);
  AppendReplicateMessagesToQueue(message_queue_.get(), clock_, first.index(), 1);

  ASSERT_OK(remote_peer1->SignalRequest(RequestTriggerMode::kNonEmptyOnly));
  ASSERT_OK(remote_peer2->SignalRequest(RequestTriggerMode::kNonEmptyOnly));

  // Replication should time out, because of the delayed response.
  consensus_->WaitForMajorityReplicatedIndex(first.index());
  const auto elapsed_time = MonoTime::Now() - start_time;
  LOG(INFO) << "Replication elapsed time: " << elapsed_time;
  // Replication should take at least as much time as it takes the local peer to append, because
  // there is only one remote peer that is responding.
  ASSERT_GE(elapsed_time, kAppendDelayTime);
}

TEST_F(ConsensusPeersTest, TestRemotePeers) {
  // Create a set of remote peers.
  std::shared_ptr<Peer> remote_peer1;
  DelayablePeerProxy<NoOpTestPeerProxy>* remote_peer1_proxy =
      NewRemotePeer("peer-1", &remote_peer1);

  std::shared_ptr<Peer> remote_peer2;
  DelayablePeerProxy<NoOpTestPeerProxy>* remote_peer2_proxy =
      NewRemotePeer("peer-2", &remote_peer2);

  auto se = ScopeExit([&remote_peer1, &remote_peer2] {
    // This guarantees that the Peer objects don't get destroyed if there is a pending request.
    remote_peer1->Close();
    remote_peer2->Close();
  });

  // Delay the response from the second remote peer.
  remote_peer2_proxy->DelayResponse();

  // Append one message to the queue.
  OpIdPB first = MakeOpId(0, 1);

  AppendReplicateMessagesToQueue(message_queue_.get(), clock_, first.index(), 1);

  ASSERT_OK(remote_peer1->SignalRequest(RequestTriggerMode::kNonEmptyOnly));
  ASSERT_OK(remote_peer2->SignalRequest(RequestTriggerMode::kNonEmptyOnly));

  // Now wait for the message to be replicated, this should succeed since
  // majority = 2 and only one peer was delayed. The majority is made up
  // of remote-peer1 and the local log.
  consensus_->WaitForMajorityReplicatedIndex(first.index());

  SCOPED_TRACE(Format(
      "Written to log locally: $0, Received by peer1: {$1}, by peer2: {$2}",
      log_->GetLatestEntryOpId(),
      remote_peer1_proxy->proxy()->last_received(),
      remote_peer2_proxy->proxy()->last_received()));

  CheckLastLogEntry(first.term(), first.index());
  CheckLastRemoteEntry(remote_peer1_proxy, first.term(), first.index());

  remote_peer2_proxy->Respond(TestPeerProxy::kUpdate);
  // Wait until all peers have replicated the message, otherwise
  // when we add the next one remote_peer2 might find the next message
  // in the queue and will replicate it, which is not what we want.
  while (!OpIdEquals(message_queue_->GetAllReplicatedIndexForTests(), first)) {
    std::this_thread::sleep_for(1ms);
  }

  // Now append another message to the queue.
  AppendReplicateMessagesToQueue(message_queue_.get(), clock_, 2, 1);

  // We should not see it replicated, even after 10ms,
  // since only the local peer replicates the message.
  SleepFor(MonoDelta::FromMilliseconds(10));
  ASSERT_FALSE(consensus_->IsMajorityReplicated(2));

  // Signal one of the two remote peers.
  ASSERT_OK(remote_peer1->SignalRequest(RequestTriggerMode::kNonEmptyOnly));
  // We should now be able to wait for it to replicate, since two peers (a majority)
  // have replicated the message.
  consensus_->WaitForMajorityReplicatedIndex(2);
}

// Regression test for KUDU-699: even if a peer isn't making progress,
// and thus always has data pending, we should be able to close the peer.
TEST_F(ConsensusPeersTest, TestCloseWhenRemotePeerDoesntMakeProgress) {
  auto mock_proxy = new MockedPeerProxy(raft_pool_.get());
  auto peer = ASSERT_RESULT(Peer::NewRemotePeer(
      FakeRaftPeerPB(kFollowerUuid), kTabletId, kLeaderUuid, PeerProxyPtr(mock_proxy),
      message_queue_.get(), raft_pool_token_.get(), nullptr /* consensus */,
      messenger_.get()));

  // Make the peer respond without making any progress -- it always returns
  // that it has only replicated op 0.0. When we see the response, we always
  // decide that more data is pending, and we want to send another request.
  ConsensusResponsePB peer_resp;
  peer_resp.set_responder_uuid(kFollowerUuid);
  peer_resp.set_responder_term(0);
  peer_resp.mutable_status()->mutable_last_received()->CopyFrom(
      MakeOpId(0, 0));
  peer_resp.mutable_status()->mutable_last_received_current_leader()->CopyFrom(
      MakeOpId(0, 0));
  peer_resp.mutable_status()->set_last_committed_idx(0);

  mock_proxy->set_update_response(peer_resp);

  // Add an op to the queue and start sending requests to the peer.
  AppendReplicateMessagesToQueue(message_queue_.get(), clock_, 1, 1);
  ASSERT_OK(peer->SignalRequest(RequestTriggerMode::kAlwaysSend));

  // We should be able to close the peer even though it has more data pending.
  peer->Close();
}

TEST_F(ConsensusPeersTest, TestDontSendOneRpcPerWriteWhenPeerIsDown) {
  auto mock_proxy = new MockedPeerProxy(raft_pool_.get());
  auto peer = ASSERT_RESULT(Peer::NewRemotePeer(
      FakeRaftPeerPB(kFollowerUuid), kTabletId, kLeaderUuid, PeerProxyPtr(mock_proxy),
      message_queue_.get(), raft_pool_token_.get(), nullptr /* consensus */,
      messenger_.get()));

  auto se = ScopeExit([&peer] {
    // This guarantees that the Peer object doesn't get destroyed if there is a pending request.
    peer->Close();
  });

  // Initial response has to be successful -- otherwise we'll consider the peer "new" and only send
  // heartbeat RPCs.
  ConsensusResponsePB initial_resp;
  initial_resp.set_responder_uuid(kFollowerUuid);
  initial_resp.set_responder_term(0);
  initial_resp.mutable_status()->mutable_last_received()->CopyFrom(
      MakeOpId(1, 1));
  initial_resp.mutable_status()->mutable_last_received_current_leader()->CopyFrom(
      MakeOpId(1, 1));
  initial_resp.mutable_status()->set_last_committed_idx(0);
  mock_proxy->set_update_response(initial_resp);

  AppendReplicateMessagesToQueue(message_queue_.get(), clock_, 1, 1);
  LOG(INFO) << "Initial SignalRequest";
  ASSERT_OK(peer->SignalRequest(RequestTriggerMode::kAlwaysSend));
  LOG(INFO) << "Initial SignalRequest done";

  // Now wait for the message to be replicated, this should succeed since the local (leader) peer
  // always acks and the follower also acked this time.
  consensus_->WaitForMajorityReplicatedIndex(1);
  LOG(INFO) << "Message replicated, setting error response";

  // Set up the peer to respond with an error.
  ConsensusResponsePB error_resp;
  error_resp.mutable_error()->set_code(tserver::TabletServerErrorPB::UNKNOWN_ERROR);
  StatusToPB(STATUS(NotFound, "fake error"), error_resp.mutable_error()->mutable_status());
  mock_proxy->set_update_response(error_resp);

  // Up to this point we might have sent a lot of updates: we get the response from the fake peer
  // that it accepted our entry, we consider it replicated, and we are trying to tell the fake peer
  // about that, but it replies with the same canned response without bumping up the committed
  // index. We can keep spinning in this loop for a few hundred times until we replace the response
  // with an error. At the end of the test we should just consider the UpdateAsync calls made after
  // this point.
  int initial_update_count = mock_proxy->update_count();

  // Add a bunch of messages to the queue.
  for (int i = 2; i <= 100; i++) {
    AppendReplicateMessagesToQueue(message_queue_.get(), clock_, i, /* count */ 1);
    ASSERT_OK(peer->SignalRequest(RequestTriggerMode::kNonEmptyOnly));
    // Sleep for a longer time during the first iteration so we have a higher chance of handling
    // the response and incrementing failed_attempts_.
    std::this_thread::sleep_for(i == 2 ? 100ms : 2ms);
  }

  LOG(INFO) << EXPR_VALUE_FOR_LOG(mock_proxy->update_count());
  LOG(INFO) << EXPR_VALUE_FOR_LOG(initial_update_count);
  // Check that we didn't attempt to send one UpdateConsensus call per
  // Write. 100 writes might have taken a second or two, though, so it's
  // OK to have called UpdateConsensus() a few times due to regularly
  // scheduled heartbeats.
  ASSERT_LT(mock_proxy->update_count() - initial_update_count, 5);
}

}  // namespace consensus
}  // namespace yb
