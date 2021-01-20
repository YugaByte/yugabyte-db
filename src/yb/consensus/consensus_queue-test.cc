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
#include <gflags/gflags.h>

#include "yb/common/schema.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/consensus/consensus_queue.h"
#include "yb/consensus/consensus-test-util.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/log_reader.h"
#include "yb/consensus/log-test-base.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/replicate_msgs_holder.h"

#include "yb/fs/fs_manager.h"
#include "yb/server/hybrid_clock.h"
#include "yb/util/metrics.h"
#include "yb/util/scope_exit.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"
#include "yb/util/threadpool.h"

DECLARE_bool(enable_data_block_fsync);
DECLARE_int32(consensus_max_batch_size_bytes);

METRIC_DECLARE_entity(tablet);

namespace yb {
namespace consensus {

static const char* kLeaderUuid = "peer-0";
static const char* kPeerUuid = "peer-1";
static const char* kTestTable = "test-table";
static const char* kTestTablet = "test-tablet";

constexpr int kNumMessages = 100;

class ConsensusQueueTest : public YBTest {
 public:
  ConsensusQueueTest()
      : schema_(GetSimpleTestSchema()),
        metric_entity_(METRIC_ENTITY_tablet.Instantiate(&metric_registry_, "queue-test")),
        registry_(new log::LogAnchorRegistry) {
    FLAGS_enable_data_block_fsync = false; // Keep unit tests fast.
  }

  void SetUp() override {
    YBTest::SetUp();
    fs_manager_.reset(new FsManager(env_.get(), GetTestPath("fs_root"), "tserver_test"));
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
    ASSERT_OK(ThreadPoolBuilder("log").Build(&log_thread_pool_));
    ASSERT_OK(log::Log::Open(log::LogOptions(),
                            kTestTablet,
                            fs_manager_->GetFirstTabletWalDirOrDie(kTestTable, kTestTablet),
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

    ASSERT_OK(ThreadPoolBuilder("raft").Build(&raft_pool_));
    CloseAndReopenQueue();
  }

  void CloseAndReopenQueue() {
    // Blow away the memtrackers before creating the new queue.
    queue_.reset();
    auto token = raft_pool_->NewToken(ThreadPool::ExecutionMode::SERIAL);
    queue_.reset(new PeerMessageQueue(metric_entity_,
                                      log_.get(),
                                      nullptr /* server_tracker */,
                                      nullptr /* parent_tracker */,
                                      FakeRaftPeerPB(kLeaderUuid),
                                      kTestTablet,
                                      clock_,
                                      nullptr /* consensus_context */,
                                      std::move(token)));
    consensus_.reset(new TestRaftConsensusQueueIface());
    queue_->RegisterObserver(consensus_.get());
  }

  void TearDown() override {
    ASSERT_OK(log_->WaitUntilAllFlushed());
    queue_->Close();
  }

  Status AppendReplicateMsg(int term, int index, int payload_size) {
    return queue_->TEST_AppendOperation(CreateDummyReplicate(
        term, index, clock_->Now(), payload_size));
  }

  // Updates the peer's watermark in the queue so that it matches
  // the operation we want, since the queue always assumes that
  // when a peer gets tracked it's always tracked starting at the
  // last operation in the queue
  bool UpdatePeerWatermarkToOp(ConsensusRequestPB* request,
                               ConsensusResponsePB* response,
                               const OpIdPB& last_received,
                               const OpIdPB& last_received_current_leader,
                               int last_committed_idx) {

    queue_->TrackPeer(kPeerUuid);
    response->set_responder_uuid(kPeerUuid);

    // Ask for a request. The queue assumes the peer is up-to-date so this should contain no
    // operations.
    ReplicateMsgsHolder refs;
    bool needs_remote_bootstrap;
    EXPECT_OK(queue_->RequestForPeer(kPeerUuid, request, &refs, &needs_remote_bootstrap));
    EXPECT_FALSE(needs_remote_bootstrap);
    EXPECT_EQ(request->ops_size(), 0);

    // Refuse saying that the log matching property check failed and
    // that our last operation is actually 'last_received'.
    RefuseWithLogPropertyMismatch(response, last_received, last_received_current_leader);
    response->mutable_status()->set_last_committed_idx(last_committed_idx);
    bool result = queue_->ResponseFromPeer(response->responder_uuid(), *response);
    request->Clear();
    response->mutable_status()->Clear();
    return result;
  }

  // Like the above but uses the last received index as the commtited index.
  bool UpdatePeerWatermarkToOp(ConsensusRequestPB* request,
                               ConsensusResponsePB* response,
                               const OpIdPB& last_received,
                               const OpIdPB& last_received_current_leader) {
    return UpdatePeerWatermarkToOp(request, response, last_received,
                                   last_received_current_leader,
                                   last_received.index());
  }

  void RefuseWithLogPropertyMismatch(ConsensusResponsePB* response,
                                     const OpIdPB& last_received,
                                     const OpIdPB& last_received_current_leader) {
    ConsensusStatusPB* status = response->mutable_status();
    status->mutable_last_received()->CopyFrom(last_received);
    status->mutable_last_received_current_leader()->CopyFrom(last_received_current_leader);
    ConsensusErrorPB* error = status->mutable_error();
    error->set_code(ConsensusErrorPB::PRECEDING_ENTRY_DIDNT_MATCH);
    StatusToPB(STATUS(IllegalState, "LMP failed."), error->mutable_status());
  }

  void WaitForLocalPeerToAckIndex(int index) {
    while (true) {
      PeerMessageQueue::TrackedPeer leader = queue_->GetTrackedPeerForTests(kLeaderUuid);
      if (leader.last_received.index >= index) {
        break;
      }
      SleepFor(MonoDelta::FromMilliseconds(10));
    }
  }

  // Sets the last received op on the response, as well as the last committed index.
  void SetLastReceivedAndLastCommitted(ConsensusResponsePB* response,
                                       const OpId& last_received,
                                       const OpId& last_received_current_leader,
                                       int last_committed_idx) {
    last_received.ToPB(response->mutable_status()->mutable_last_received());
    last_received_current_leader.ToPB(
        response->mutable_status()->mutable_last_received_current_leader());
    response->mutable_status()->set_last_committed_idx(last_committed_idx);
  }

  // Like the above but uses the same last_received for current term.
  void SetLastReceivedAndLastCommitted(ConsensusResponsePB* response,
                                       const OpId& last_received,
                                       int last_committed_idx) {
    SetLastReceivedAndLastCommitted(response, last_received, last_received, last_committed_idx);
  }

  // Like the above but just sets the last committed index to have the same index
  // as the last received op.
  void SetLastReceivedAndLastCommitted(ConsensusResponsePB* response,
                                       const OpId& last_received) {
    SetLastReceivedAndLastCommitted(response, last_received, last_received.index);
  }

 protected:
  gscoped_ptr<TestRaftConsensusQueueIface> consensus_;
  const Schema schema_;
  gscoped_ptr<FsManager> fs_manager_;
  MetricRegistry metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  std::unique_ptr<ThreadPool> log_thread_pool_;
  scoped_refptr<log::Log> log_;
  std::unique_ptr<ThreadPool> raft_pool_;
  gscoped_ptr<PeerMessageQueue> queue_;
  scoped_refptr<log::LogAnchorRegistry> registry_;
  scoped_refptr<server::Clock> clock_;
};

// Tests that the queue is able to track a peer when it starts tracking a peer
// after the initial message in the queue. In particular this creates a queue
// with several messages and then starts to track a peer whose watermark
// falls in the middle of the current messages in the queue.
TEST_F(ConsensusQueueTest, TestStartTrackingAfterStart) {
  queue_->Init(OpId::Min());
  queue_->SetLeaderMode(
      OpId::Min(), OpId::Min().term, OpId::Min(), BuildRaftConfigPBForTests(2));
  AppendReplicateMessagesToQueue(queue_.get(), clock_, 1, 100);

  ConsensusRequestPB request;
  ConsensusResponsePB response;
  response.set_responder_uuid(kPeerUuid);

  // Peer already has some messages, last one being index (kNumMessages / 2)
  OpIdPB last_received = MakeOpIdPbForIndex(kNumMessages / 2);
  OpIdPB last_received_current_leader = MinimumOpId();

  ASSERT_TRUE(UpdatePeerWatermarkToOp(
      &request, &response, last_received, last_received_current_leader));

  // Getting a new request should get all operations after 7.50
  ReplicateMsgsHolder refs;
  bool needs_remote_bootstrap;
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(kNumMessages / 2, request.ops_size());

  SetLastReceivedAndLastCommitted(
      &response, OpId::FromPB(request.ops((kNumMessages / 2) - 1).id()));
  ASSERT_FALSE(queue_->ResponseFromPeer(response.responder_uuid(), response))
      << "Queue still had requests pending";

  // if we ask for a new request, it should come back empty
  refs.Reset();
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(0, request.ops_size());
}

// Tests that the peers gets the messages pages, with the size of a page being
// 'consensus_max_batch_size_bytes'
TEST_F(ConsensusQueueTest, TestGetPagedMessages) {
  queue_->Init(OpId::Min());
  queue_->SetLeaderMode(
      OpId::Min(), OpId::Min().term, OpId::Min(), BuildRaftConfigPBForTests(2));

  const int kOpsPerRequest = 9;
  int32_t page_size_estimate = 0;
  {
    // Helper to estimate request size so that we can set the max batch size appropriately.
    ConsensusRequestPB page_size_estimator;
    page_size_estimator.set_caller_term(14);
    OpIdPB* committed_index = page_size_estimator.mutable_committed_op_id();
    OpIdPB* preceding_id = page_size_estimator.mutable_preceding_id();

    // The actual leader lease duration does not matter here, we just want it to be set.
    page_size_estimator.set_leader_lease_duration_ms(kDefaultLeaderLeaseDurationMs);
    page_size_estimator.set_ht_lease_expiration(1000);
    const HybridTime ht = clock_->Now();
    page_size_estimator.set_propagated_hybrid_time(ht.ToUint64());
    committed_index->CopyFrom(MinimumOpId());
    preceding_id->CopyFrom(MinimumOpId());

    // We're going to add 100 messages to the queue so we make each page fetch 9 of those,
    // for a total of 12 pages. The last page should have a single op.
    ReplicateMsgs replicates;
    for (int i = 0; i < kOpsPerRequest; i++) {
      replicates.push_back(CreateDummyReplicate(
          0 /* term */, 0 /* index */, ht, 0 /* payload_size */));
      page_size_estimator.mutable_ops()->AddAllocated(replicates.back().get());
    }

    page_size_estimate = page_size_estimator.ByteSize();
    LOG(INFO) << "page_size_estimate=" << page_size_estimate;
    page_size_estimator.mutable_ops()->ExtractSubrange(0,
                                                       page_size_estimator.ops_size(),
                                                       /* elements */ nullptr);
  }

  // Save the current flag state.
  google::FlagSaver saver;
  FLAGS_consensus_max_batch_size_bytes = page_size_estimate;

  ConsensusRequestPB request;
  ConsensusResponsePB response;
  response.set_responder_uuid(kPeerUuid);

  ASSERT_TRUE(UpdatePeerWatermarkToOp(&request, &response, MinimumOpId(), MinimumOpId()));

  // Append the messages after the queue is tracked. Otherwise the ops might
  // get evicted from the cache immediately and the requests below would
  // result in async log reads instead of cache hits.
  AppendReplicateMessagesToQueue(queue_.get(), clock_, 1, 100);

  OpIdPB last;
  for (int i = 0; i < 11; i++) {
    VLOG(1) << "Making request " << i;
    ReplicateMsgsHolder refs;
    bool needs_remote_bootstrap;
    ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));

    ASSERT_FALSE(needs_remote_bootstrap);
    LOG(INFO) << "Number of ops in request: " << request.ops_size();
    ASSERT_EQ(kOpsPerRequest, request.ops_size());
    last = request.ops(request.ops_size() - 1).id();
    SetLastReceivedAndLastCommitted(&response, OpId::FromPB(last));
    VLOG(1) << "Faking received up through " << last;
    ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response));
  }
  ReplicateMsgsHolder refs;
  bool needs_remote_bootstrap;
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(1, request.ops_size());
  last = request.ops(request.ops_size() - 1).id();
  SetLastReceivedAndLastCommitted(&response, OpId::FromPB(last));
  ASSERT_FALSE(queue_->ResponseFromPeer(response.responder_uuid(), response));
}

TEST_F(ConsensusQueueTest, TestPeersDontAckBeyondWatermarks) {
  queue_->Init(OpId::Min());
  queue_->SetLeaderMode(
      OpId::Min(), OpId::Min().term, OpId::Min(), BuildRaftConfigPBForTests(3));
  AppendReplicateMessagesToQueue(queue_.get(), clock_, 1, kNumMessages);

  // Wait for the local peer to append all messages
  WaitForLocalPeerToAckIndex(kNumMessages);

  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), OpId::Min());
  // Since we're tracking a single peer still this should have moved the all
  // replicated watermark to the last op appended to the local log.
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), MakeOpIdForIndex(kNumMessages));
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), OpId::Min());
  ASSERT_EQ(queue_->GetAllAppliedOpId(), OpId::Min());

  // Start to track the peer after the queue has some messages in it
  // at a point that is halfway through the current messages in the queue.
  OpIdPB first_msg = MakeOpIdPbForIndex(kNumMessages / 2);

  ConsensusRequestPB request;
  ConsensusResponsePB response;
  response.set_responder_uuid(kPeerUuid);

  ASSERT_TRUE(UpdatePeerWatermarkToOp(&request, &response, first_msg, MinimumOpId()));

  // Tracking a peer a new peer should have moved the all replicated watermark back.
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), OpId::Min());
  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), OpId::Min());
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), OpId::Min());
  ASSERT_EQ(queue_->GetAllAppliedOpId(), OpId::Min());

  ReplicateMsgsHolder refs;
  bool needs_remote_bootstrap;
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(kNumMessages / 2, request.ops_size());

  AppendReplicateMessagesToQueue(queue_.get(), clock_, 101, kNumMessages);

  SetLastReceivedAndLastCommitted(
      &response, OpId::FromPB(request.ops((kNumMessages / 2) - 1).id()));
  response.set_responder_term(28);

  ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response))
      << "Queue didn't have anymore requests pending";

  auto expected_op_id = MakeOpIdForIndex(kNumMessages);
  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), expected_op_id);
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), expected_op_id);
  consensus_->WaitForMajorityReplicatedIndex(expected_op_id.index);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected_op_id);

  // if we ask for a new request, it should come back with the rest of the messages
  refs.Reset();
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(kNumMessages, request.ops_size());

  OpId expected = OpId::FromPB(request.ops(kNumMessages - 1).id());

  SetLastReceivedAndLastCommitted(&response, expected);
  response.set_responder_term(expected.term);
  ASSERT_FALSE(queue_->ResponseFromPeer(response.responder_uuid(), response))
      << "Queue didn't have anymore requests pending";

  WaitForLocalPeerToAckIndex(expected.index);

  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), expected);
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), expected);
  consensus_->WaitForMajorityReplicatedIndex(expected.index);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected);
}

TEST_F(ConsensusQueueTest, TestQueueAdvancesCommittedIndex) {
  queue_->Init(OpId::Min());
  queue_->SetLeaderMode(
      OpId::Min(), OpId::Min().term, OpId::Min(), BuildRaftConfigPBForTests(5));
  // Track 4 additional peers (in addition to the local peer)
  queue_->TrackPeer("peer-1");
  queue_->TrackPeer("peer-2");
  queue_->TrackPeer("peer-3");
  queue_->TrackPeer("peer-4");

  // Append 10 messages to the queue with a majority of 2 for a total of 3 peers.
  // This should add messages 0.1 -> 0.7, 1.8 -> 1.10 to the queue.
  AppendReplicateMessagesToQueue(queue_.get(), clock_, 1, 10);
  WaitForLocalPeerToAckIndex(10);

  // Since only the local log might have ACKed at this point,
  // the committed_index should be MinimumOpId().
  queue_->raft_pool_observers_token_->Wait();
  ASSERT_EQ(queue_->TEST_GetCommittedIndex(), OpId::Min());
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), OpId::Min());
  ASSERT_EQ(queue_->GetAllAppliedOpId(), OpId::Min());

  // NOTE: We don't need to get operations from the queue. The queue
  // only cares about what the peer reported as received, not what was sent.
  ConsensusResponsePB response;
  response.set_responder_term(1);

  OpId last_sent = MakeOpIdForIndex(5);

  // Ack the first five operations for peer-1
  response.set_responder_uuid("peer-1");
  SetLastReceivedAndLastCommitted(&response, last_sent, MinimumOpId().index());

  ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response));

  // Committed index should be the same
  queue_->raft_pool_observers_token_->Wait();
  ASSERT_EQ(queue_->TEST_GetCommittedIndex(), OpId::Min());
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), OpId::Min());
  ASSERT_EQ(queue_->GetAllAppliedOpId(), OpId::Min());

  // Ack the first five operations for peer-2
  response.set_responder_uuid("peer-2");
  ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response));

  // A majority has now replicated up to 0.5.
  queue_->raft_pool_observers_token_->Wait();
  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), MakeOpIdForIndex(5));

  string up_to_date_peer = queue_->GetUpToDatePeer();
  ASSERT_TRUE((up_to_date_peer == "peer-2") || (up_to_date_peer == "peer-1"));

  // Ack all operations for peer-3
  response.set_responder_uuid("peer-3");
  last_sent = MakeOpIdForIndex(10);
  SetLastReceivedAndLastCommitted(&response, last_sent, OpId::Min().index);

  // The committed index moved so 'more_pending' should be true so that the peer is
  // notified.
  ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response));

  up_to_date_peer.clear();
  up_to_date_peer = queue_->GetUpToDatePeer();
  ASSERT_EQ(up_to_date_peer, "peer-3");

  // Majority replicated watermark should be the same
  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), MakeOpIdForIndex(5));

  // Ack the remaining operations for peer-4
  response.set_responder_uuid("peer-4");
  ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response));

  up_to_date_peer.clear();
  up_to_date_peer = queue_->GetUpToDatePeer();
  ASSERT_TRUE((up_to_date_peer == "peer-3") || (up_to_date_peer == "peer-4"));

  // Now that a majority of peers have replicated an operation in the queue's
  // term the committed index should advance.
  queue_->raft_pool_observers_token_->Wait();
  const auto expected_op_id = MakeOpIdForIndex(10);
  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), expected_op_id);
  ASSERT_EQ(queue_->TEST_GetCommittedIndex(), expected_op_id);
  consensus_->WaitForMajorityReplicatedIndex(expected_op_id.index);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected_op_id);
}

// In this test we append a sequence of operations to a log
// and then start tracking a peer whose first required operation
// is before the first operation in the queue.
TEST_F(ConsensusQueueTest, TestQueueLoadsOperationsForPeer) {
  OpIdPB opid = MakeOpId(1, 1);

  for (int i = 1; i <= 100; i++) {
    ASSERT_OK(log::AppendNoOpToLogSync(clock_, log_.get(), &opid));
    // Roll the log every 10 ops
    if (i % 10 == 0) {
      ASSERT_OK(log_->AllocateSegmentAndRollOver());
    }
  }
  ASSERT_OK(log_->WaitUntilAllFlushed());

  OpIdPB queues_last_op = opid;
  queues_last_op.set_index(queues_last_op.index() - 1);

  // Now reset the queue so that we can pass a new committed index,
  // the last operation in the log.
  CloseAndReopenQueue();

  OpId committed_index(1, 100);
  const auto last_applied_op = committed_index;
  queue_->Init(committed_index);
  queue_->SetLeaderMode(
      committed_index, committed_index.term, last_applied_op, BuildRaftConfigPBForTests(3));

  ConsensusRequestPB request;
  ConsensusResponsePB response;
  response.set_responder_uuid(kPeerUuid);

  // The peer will actually be behind the first operation in the queue
  // in this case about 50 operations before.
  OpIdPB peers_last_op;
  peers_last_op.set_term(1);
  peers_last_op.set_index(50);

  // Now we start tracking the peer, this negotiation round should let
  // the queue know how far along the peer is.
  // The queue should reply that there are more messages for the peer.
  ASSERT_TRUE(UpdatePeerWatermarkToOp(
      &request, &response, peers_last_op, MinimumOpId()));

  // When we get another request for the peer the queue should load
  // the missing operations.
  ReplicateMsgsHolder refs;
  bool needs_remote_bootstrap;
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(request.ops_size(), 50);
}

// This tests that the queue is able to handle operation overwriting, i.e. when a
// newly tracked peer reports the last received operations as some operation that
// doesn't exist in the leader's log. In particular it tests the case where a
// new leader starts at term 2 with only a part of the operations of the previous
// leader having been committed.
TEST_F(ConsensusQueueTest, TestQueueHandlesOperationOverwriting) {
  OpIdPB opid = MakeOpId(1, 1);
  // Append 10 messages in term 1 to the log.
  for (int i = 1; i <= 10; i++) {
    ASSERT_OK(log::AppendNoOpToLogSync(clock_, log_.get(), &opid));
    // Roll the log every 3 ops
    if (i % 3 == 0) {
      ASSERT_OK(log_->AllocateSegmentAndRollOver());
    }
  }

  opid = MakeOpId(2, 11);
  // Now append 10 more messages in term 2.
  for (int i = 11; i <= 20; i++) {
    ASSERT_OK(log::AppendNoOpToLogSync(clock_, log_.get(), &opid));
    // Roll the log every 3 ops
    if (i % 3 == 0) {
      ASSERT_OK(log_->AllocateSegmentAndRollOver());
    }
  }


  // Now reset the queue so that we can pass a new committed index,
  // op, 2.15.
  CloseAndReopenQueue();

  const OpId committed_op_id(2, 15);
  queue_->Init(OpId(2, 20));
  queue_->SetLeaderMode(
      committed_op_id, committed_op_id.term, committed_op_id,
      BuildRaftConfigPBForTests(3));

  // Now get a request for a simulated old leader, which contains more operations
  // in term 1 than the new leader has.
  // The queue should realize that the old leader's last received doesn't exist
  // and send it operations starting at the old leader's committed index.
  ConsensusRequestPB request;
  ConsensusResponsePB response;
  response.set_responder_uuid(kPeerUuid);

  queue_->TrackPeer(kPeerUuid);

  // Ask for a request. The queue assumes the peer is up-to-date so
  // this should contain no operations.
  ReplicateMsgsHolder refs;
  bool needs_remote_bootstrap;
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(request.ops_size(), 0);
  ASSERT_OPID_EQ(request.preceding_id(), MakeOpId(2, 20));
  ASSERT_EQ(OpId::FromPB(request.committed_op_id()), committed_op_id);

  // The old leader was still in term 1 but it increased its term with our request.
  response.set_responder_term(2);

  // We emulate that the old leader had 25 total operations in Term 1 (15 more than we knew about)
  // which were never committed, and that its last known committed index was 5.
  ConsensusStatusPB* status = response.mutable_status();
  status->mutable_last_received()->CopyFrom(MakeOpId(1, 25));
  status->mutable_last_received_current_leader()->CopyFrom(MinimumOpId());
  status->set_last_committed_idx(5);
  ConsensusErrorPB* error = status->mutable_error();
  error->set_code(ConsensusErrorPB::PRECEDING_ENTRY_DIDNT_MATCH);
  StatusToPB(STATUS(IllegalState, "LMP failed."), error->mutable_status());

  // The queue should reply that there are more operations pending.
  ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response));
  request.Clear();

  // We're waiting for a two nodes. The all committed watermark should be
  // 0.0 since we haven't had a successful exchange with the 'remote' peer.
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), OpId::Min());
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), committed_op_id);
  ASSERT_EQ(queue_->GetAllAppliedOpId(), OpId::Min());

  // Test even when a correct peer responds (meaning we actually get to execute
  // watermark advancement) we sill have the same all-replicated watermark.
  auto replicate = CreateDummyReplicate(2, 21, clock_->Now(), 0);
  ASSERT_OK(queue_->TEST_AppendOperation(replicate));
  WaitForLocalPeerToAckIndex(21);

  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), OpId::Min());
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), committed_op_id);
  ASSERT_EQ(queue_->GetAllAppliedOpId(), OpId::Min());

  // Generate another request for the remote peer, which should include
  // all of the ops since the peer's last-known committed index.
  refs.Reset();
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_OPID_EQ(MakeOpId(1, 5), request.preceding_id());
  ASSERT_EQ(16, request.ops_size());

  // Now when we respond the watermarks should advance.
  response.mutable_status()->clear_error();
  SetLastReceivedAndLastCommitted(&response, OpId(2, 21), 5);
  queue_->ResponseFromPeer(response.responder_uuid(), response);

  // Now the watermark should have advanced.
  const auto expected_op_id = OpId(2, 21);
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), expected_op_id);
  consensus_->WaitForMajorityReplicatedIndex(expected_op_id.index);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected_op_id);
}

// Test for a bug where we wouldn't move any watermark back, when overwriting
// operations, which would cause a check failure on the write immediately
// following the overwriting write.
TEST_F(ConsensusQueueTest, TestQueueMovesWatermarksBackward) {
  RestartSafeCoarseMonoClock restart_safe_coarse_mono_clock;
  queue_->Init(OpId::Min());
  queue_->SetNonLeaderMode();
  // Append a bunch of messages.
  AppendReplicateMessagesToQueue(queue_.get(), clock_, 1, 10);
  ASSERT_OK(log_->WaitUntilAllFlushed());
  ASSERT_EQ(queue_->TEST_GetLastAppended(), OpId(1, 10));
  // Now rewrite some of the operations and wait for the log to append.
  ASSERT_OK(queue_->AppendOperations(
      { CreateDummyReplicate(2, 5, clock_->Now(), 0) }, yb::OpId() /* committed_op_id */,
        restart_safe_coarse_mono_clock.Now()));

  // Wait for the operation to be in the log.
  log_->WaitForSafeOpIdToApply(yb::OpId(2, 5));

  // Without the fix the following append would trigger a check failure
  // in log cache.
  ASSERT_OK(queue_->AppendOperations(
      { CreateDummyReplicate(2, 6, clock_->Now(), 0) }, yb::OpId() /* committed_op_id */,
      restart_safe_coarse_mono_clock.Now()));

  // Wait for the operation to be in the log.
  log_->WaitForSafeOpIdToApply(yb::OpId(2, 6));

  // Now the last appended should have moved backward.
  ASSERT_OK(WaitFor(
        [this]() { return queue_->TEST_GetLastAppended() == OpId(2, 6); },
        1s, "AllReplicatedOpIdForTests", 10ms));
}

// Tests that we're advancing the watermarks properly and only when the peer
// has a prefix of our log. This also tests for a specific bug that we had. Here's
// the scenario:
// Peer would report:
//   - last received 75.49
//   - last committed 72.31
//
// Queue has messages:
// 72.31-72.45
// 73.46-73.51
// 76.52-76.53
//
// The queue has more messages than the peer, but the peer has messages
// that the queue doesn't and which will be overwritten.
//
// In the first round of negotiation the peer would report LMP mismatch.
// In the second round the queue would try to send it messages starting at 75.49
// but since that message didn't exist in the queue's log it would instead send
// messages starting at 72.31. However, because the batches were big it was only
// able to send a few messages (e.g. up to 72.40).
//
// Since in this last exchange everything went ok (the peer still doesn't know
// that messages will be overwritten later), the queue would mark the exchange
// as successful and the peer's last received would be taken into account when
// calculating watermarks, which was incorrect.
TEST_F(ConsensusQueueTest, TestOnlyAdvancesWatermarkWhenPeerHasAPrefixOfOurLog) {
  FLAGS_consensus_max_batch_size_bytes = 1024 * 10;

  queue_->Init(OpId(72, 30));
  queue_->SetLeaderMode(OpId(72, 31), 76, OpId(72, 31), BuildRaftConfigPBForTests(3));

  ConsensusRequestPB request;
  ConsensusResponsePB response;

  // We expect the majority replicated watermark to star at the committed index.
  OpId expected_majority_replicated(72, 31);
  // We expect the all replicated watermark to be reset when we track a new peer.
  OpId expected_all_replicated = OpId::Min();
  auto expected_last_applied = expected_majority_replicated;

  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), expected_majority_replicated);
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), expected_all_replicated);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected_last_applied);

  ASSERT_TRUE(UpdatePeerWatermarkToOp(&request, &response, MakeOpId(75, 49), MinimumOpId(), 31));

  for (int i = 31; i <= 53; i++) {
    if (i <= 45) {
      AppendReplicateMsg(72, i, 1024);
      continue;
    }
    if (i <= 51) {
      AppendReplicateMsg(73, i, 1024);
      continue;
    }
    AppendReplicateMsg(76, i, 1024);
  }

  WaitForLocalPeerToAckIndex(53);

  // When we get operations for this peer we should get them starting immediately after
  // the committed index, for a total of 9 operations.
  ReplicateMsgsHolder refs;
  bool needs_remote_bootstrap;
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(request.ops_size(), 9);
  ASSERT_OPID_EQ(request.ops(0).id(), MakeOpId(72, 32));
  const OpIdPB* last_op = &request.ops(request.ops_size() - 1).id();

  // When the peer acks that it received an operation that is not in our current
  // term, it gets ignored in terms of watermark advancement.
  SetLastReceivedAndLastCommitted(&response, OpId(75, 49), OpId::FromPB(*last_op), 31);
  ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response));

  // We've sent (and received and ack) up to 72.40 from the remote peer
  expected_majority_replicated = OpId(72, 40);
  expected_all_replicated = expected_majority_replicated;
  expected_last_applied = expected_majority_replicated;

  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), expected_majority_replicated);
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), expected_all_replicated);
  consensus_->WaitForMajorityReplicatedIndex(expected_last_applied.index);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected_last_applied);

  // Another request for this peer should get another page of messages. Still not
  // on the queue's term (and thus without advancing watermarks).
  refs.Reset();
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(request.ops_size(), 9);
  ASSERT_OPID_EQ(request.ops(0).id(), MakeOpId(72, 41));
  last_op = &request.ops(request.ops_size() - 1).id();

  SetLastReceivedAndLastCommitted(&response, OpId(75, 49), OpId::FromPB(*last_op), 31);
  queue_->ResponseFromPeer(response.responder_uuid(), response);

  // We've now sent (and received an ack) up to 73.39
  expected_majority_replicated = OpId(73, 49);
  expected_all_replicated = expected_majority_replicated;
  expected_last_applied = expected_majority_replicated;

  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), expected_majority_replicated);
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), expected_all_replicated);
  consensus_->WaitForMajorityReplicatedIndex(expected_last_applied.index);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected_last_applied);

  // The last page of request should overwrite the peer's operations and the
  // response should finally advance the watermarks.
  refs.Reset();
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);
  ASSERT_EQ(request.ops_size(), 4);
  ASSERT_OPID_EQ(request.ops(0).id(), MakeOpId(73, 50));

  // We're done, both watermarks should be at the end.
  expected_majority_replicated = OpId(76, 53);
  expected_all_replicated = expected_majority_replicated;
  expected_last_applied = expected_majority_replicated;

  SetLastReceivedAndLastCommitted(&response, expected_majority_replicated,
                                  expected_majority_replicated, 31);
  queue_->ResponseFromPeer(response.responder_uuid(), response);

  ASSERT_EQ(queue_->TEST_GetMajorityReplicatedOpId(), expected_majority_replicated);
  ASSERT_EQ(queue_->TEST_GetAllReplicatedIndex(), expected_all_replicated);
  consensus_->WaitForMajorityReplicatedIndex(expected_last_applied.index);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected_last_applied);

  request.mutable_ops()->ExtractSubrange(0, request.ops().size(), nullptr);
}

// Test that remote bootstrap is triggered when a "tablet not found" error occurs.
TEST_F(ConsensusQueueTest, TestTriggerRemoteBootstrapIfTabletNotFound) {
  queue_->Init(OpId::Min());
  queue_->SetLeaderMode(
      OpId::Min(), OpId::Min().term, OpId::Min(), BuildRaftConfigPBForTests(3));
  AppendReplicateMessagesToQueue(queue_.get(), clock_, 1, 100);

  ConsensusRequestPB request;
  ConsensusResponsePB response;
  response.set_responder_uuid(kPeerUuid);
  queue_->TrackPeer(kPeerUuid);

  // Create request for new peer.
  ReplicateMsgsHolder refs;
  bool needs_remote_bootstrap;
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_FALSE(needs_remote_bootstrap);

  // Peer responds with tablet not found.
  response.mutable_error()->set_code(tserver::TabletServerErrorPB::TABLET_NOT_FOUND);
  StatusToPB(STATUS(NotFound, "No such tablet"), response.mutable_error()->mutable_status());

  // If the peer needs remote bootstrap, more_pending should be set to true.
  ASSERT_TRUE(queue_->ResponseFromPeer(kPeerUuid, response));

  // On the next request, we should find out that the queue wants us to remotely bootstrap.
  request.Clear();
  refs.Reset();
  ASSERT_OK(queue_->RequestForPeer(kPeerUuid, &request, &refs, &needs_remote_bootstrap));
  ASSERT_TRUE(needs_remote_bootstrap);

  StartRemoteBootstrapRequestPB rb_req;
  ASSERT_OK(queue_->GetRemoteBootstrapRequestForPeer(kPeerUuid, &rb_req));

  ASSERT_TRUE(rb_req.IsInitialized()) << rb_req.ShortDebugString();
  ASSERT_EQ(kTestTablet, rb_req.tablet_id());
  ASSERT_EQ(kLeaderUuid, rb_req.bootstrap_peer_uuid());
  ASSERT_EQ(FakeRaftPeerPB(kLeaderUuid).last_known_private_addr()[0].ShortDebugString(),
            rb_req.source_private_addr()[0].ShortDebugString());
}

// Tests that ReadReplicatedMessagesForCDC() only reads messages until the last known
// committed index.
TEST_F(ConsensusQueueTest, TestReadReplicatedMessagesForCDC) {
  auto start_op_id = MakeOpIdForIndex(3); // Starting after the normal first index.
  queue_->Init(start_op_id);
  queue_->SetLeaderMode(
      start_op_id, start_op_id.term, start_op_id, BuildRaftConfigPBForTests(2));
  queue_->TrackPeer(kPeerUuid);

  AppendReplicateMessagesToQueue(queue_.get(), clock_, start_op_id.index, kNumMessages);

  // Wait for the local peer to append all messages.
  WaitForLocalPeerToAckIndex(kNumMessages);

  // Since only the local log might have ACKed at this point,
  // the committed_index should be MinimumOpId().
  queue_->raft_pool_observers_token_->Wait();
  ASSERT_EQ(queue_->TEST_GetCommittedIndex(), start_op_id);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), start_op_id);

  ConsensusResponsePB response;
  response.set_responder_uuid(kPeerUuid);

  int last_committed_index = kNumMessages - 20;
  // Ack last_committed_index messages.
  SetLastReceivedAndLastCommitted(&response, MakeOpIdForIndex(last_committed_index));
  ASSERT_TRUE(queue_->ResponseFromPeer(response.responder_uuid(), response));
  queue_->raft_pool_observers_token_->Wait();
  const auto expected_op_id = MakeOpIdForIndex(last_committed_index);
  ASSERT_EQ(queue_->TEST_GetCommittedIndex(), expected_op_id);
  consensus_->WaitForMajorityReplicatedIndex(expected_op_id.index);
  ASSERT_EQ(queue_->TEST_GetLastAppliedOpId(), expected_op_id);

  // Read from the start_op_id
  auto read_result = ASSERT_RESULT(queue_->ReadReplicatedMessagesForCDC(MakeOpIdForIndex(3)));
  ASSERT_EQ(last_committed_index - start_op_id.index, read_result.messages.size());

  // Start reading from 0.0 and ensure that we get the first known OpID.
  read_result = ASSERT_RESULT(queue_->ReadReplicatedMessagesForCDC(MakeOpIdForIndex(0)));
  ASSERT_EQ(last_committed_index - start_op_id.index, read_result.messages.size());

  // Read from some index > 0
  int start = 10;
  read_result = ASSERT_RESULT(queue_->ReadReplicatedMessagesForCDC(MakeOpIdForIndex(start)));
  ASSERT_EQ(last_committed_index - start, read_result.messages.size());
}

}  // namespace consensus
}  // namespace yb
