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

#include <regex>
#include <unordered_map>
#include <unordered_set>

#include <boost/optional.hpp>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>

#include "yb/client/client-test-util.h"
#include "yb/client/client.h"
#include "yb/client/error.h"
#include "yb/client/session.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_op.h"

#include "yb/common/schema.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/metadata.pb.h"
#include "yb/consensus/quorum_util.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/strcat.h"
#include "yb/gutil/strings/util.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/test_workload.h"
#include "yb/integration-tests/ts_itest-base.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_test_util.h"

#include "yb/server/server_base.pb.h"
#include "yb/server/hybrid_clock.h"

#include "yb/util/opid.pb.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"
#include "yb/util/stopwatch.h"

using namespace std::literals;

DEFINE_int32(num_client_threads, 8,
             "Number of client threads to launch");
DEFINE_int64(client_inserts_per_thread, 50,
             "Number of rows inserted by each client thread");
DEFINE_int64(client_num_batches_per_thread, 5,
             "In how many batches to group the rows, for each client");
DECLARE_int32(consensus_rpc_timeout_ms);
DECLARE_int32(leader_lease_duration_ms);
DECLARE_int32(ht_lease_duration_ms);
DECLARE_int32(rpc_timeout);

METRIC_DECLARE_entity(tablet);
METRIC_DECLARE_counter(not_leader_rejections);
METRIC_DECLARE_gauge_int64(raft_term);
METRIC_DECLARE_counter(log_cache_disk_reads);
METRIC_DECLARE_gauge_int64(log_cache_num_ops);

namespace yb {
namespace tserver {

using std::unordered_map;
using std::unordered_set;
using std::vector;
using std::shared_ptr;

using client::YBSession;
using client::YBTable;
using client::YBTableName;
using consensus::ConsensusRequestPB;
using consensus::ConsensusResponsePB;
using consensus::ConsensusServiceProxy;
using consensus::MajoritySize;
using consensus::MakeOpId;
using consensus::RaftPeerPB;
using consensus::ReplicateMsg;
using consensus::LeaderLeaseCheckMode;
using docdb::KeyValuePairPB;
using docdb::SubDocKey;
using docdb::DocKey;
using docdb::PrimitiveValue;
using docdb::ValueType;
using itest::AddServer;
using itest::GetReplicaStatusAndCheckIfLeader;
using itest::LeaderStepDown;
using itest::TabletServerMapUnowned;
using itest::RemoveServer;
using itest::StartElection;
using itest::WaitUntilNumberOfAliveTServersEqual;
using itest::WaitUntilLeader;
using itest::WriteSimpleTestRow;
using master::GetTabletLocationsRequestPB;
using master::GetTabletLocationsResponsePB;
using master::TabletLocationsPB;
using rpc::RpcController;
using server::SetFlagRequestPB;
using server::SetFlagResponsePB;
using server::HybridClock;
using server::ClockPtr;
using strings::Substitute;

static const int kConsensusRpcTimeoutForTests = 50;

static const int kTestRowKey = 1234;
static const int kTestRowIntVal = 5678;

// Integration test for the raft consensus implementation.
// Uses the whole tablet server stack with ExternalMiniCluster.
class RaftConsensusITest : public TabletServerIntegrationTestBase {
 public:
  RaftConsensusITest()
      : inserters_(FLAGS_num_client_threads),
        clock_(new HybridClock()) {
    CHECK_OK(clock_->Init());
  }

  void SetUp() override {
    TabletServerIntegrationTestBase::SetUp();
    FLAGS_consensus_rpc_timeout_ms = kConsensusRpcTimeoutForTests;
  }

  void ScanReplica(TabletServerServiceProxy* replica_proxy,
                   vector<string>* results) {

    ReadRequestPB req;
    ReadResponsePB resp;
    RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(10));  // Squelch warnings.

    req.set_tablet_id(tablet_id_);
    req.set_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
    auto batch = req.add_ql_batch();
    batch->set_schema_version(0);
    int id = kFirstColumnId;
    auto rsrow = batch->mutable_rsrow_desc();
    for (const auto& col : schema_.columns()) {
      batch->add_selected_exprs()->set_column_id(id);
      batch->mutable_column_refs()->add_ids(id);
      auto coldesc = rsrow->add_rscol_descs();
      coldesc->set_name(col.name());
      col.type()->ToQLTypePB(coldesc->mutable_ql_type());
      ++id;
    }

    // Send the call.
    {
      SCOPED_TRACE(req.DebugString());
      ASSERT_OK(replica_proxy->Read(req, &resp, &rpc));
      SCOPED_TRACE(resp.DebugString());
      if (resp.has_error()) {
        ASSERT_OK(StatusFromPB(resp.error().status()));
      }
    }

    Schema schema(client::MakeColumnSchemasFromColDesc(rsrow->rscol_descs()), 0);
    QLRowBlock result(schema);
    Slice data = ASSERT_RESULT(rpc.GetSidecar(0));
    if (!data.empty()) {
      ASSERT_OK(result.Deserialize(QLClient::YQL_CLIENT_CQL, &data));
    }
    for (const auto& row : result.rows()) {
      results->push_back(row.ToString());
    }

    std::sort(results->begin(), results->end());
  }

  // Scan the given replica in a loop until the number of rows
  // is 'expected_count'. If it takes more than 10 seconds, then
  // fails the test.
  void WaitForRowCount(TabletServerServiceProxy* replica_proxy,
                       int expected_count,
                       vector<string>* results) {
    LOG(INFO) << "Waiting for row count " << expected_count << "...";
    MonoTime start = MonoTime::Now();
    MonoTime deadline = MonoTime::Now();
    deadline.AddDelta(MonoDelta::FromSeconds(10));
    while (true) {
      results->clear();
      ASSERT_NO_FATALS(ScanReplica(replica_proxy, results));
      if (results->size() == expected_count) {
        return;
      }
      SleepFor(MonoDelta::FromMilliseconds(10));
      if (!MonoTime::Now().ComesBefore(deadline)) {
        break;
      }
    }
    MonoTime end = MonoTime::Now();
    LOG(WARNING) << "Didn't reach row count " << expected_count;
    FAIL() << "Did not reach expected row count " << expected_count
           << " after " << end.GetDeltaSince(start).ToString()
           << ": rows: " << *results;
  }

  // Add an Insert operation to the given consensus request.
  // The row to be inserted is generated based on the OpId.
  void AddOp(const OpIdPB& id, ConsensusRequestPB* req);

  string DumpToString(TServerDetails* leader,
                      const vector<string>& leader_results,
                      TServerDetails* replica,
                      const vector<string>& replica_results) {
    string ret = strings::Substitute("Replica results did not match the leaders."
                                     "\nLeader: $0\nReplica: $1. Results size "
                                     "L: $2 R: $3",
                                     leader->ToString(),
                                     replica->ToString(),
                                     leader_results.size(),
                                     replica_results.size());

    StrAppend(&ret, "Leader Results: \n");
    for (const string& result : leader_results) {
      StrAppend(&ret, result, "\n");
    }

    StrAppend(&ret, "Replica Results: \n");
    for (const string& result : replica_results) {
      StrAppend(&ret, result, "\n");
    }

    return ret;
  }

  void InsertTestRowsRemoteThread(uint64_t first_row,
                                  uint64_t count,
                                  uint64_t num_batches,
                                  const vector<CountDownLatch*>& latches) {
    client::TableHandle table;
    ASSERT_OK(table.Open(kTableName, client_.get()));

    shared_ptr<YBSession> session = client_->NewSession();
    session->SetTimeout(60s);

    for (int i = 0; i < num_batches; i++) {
      SCOPED_TRACE(Format("Batch: $0", i));

      uint64_t first_row_in_batch = first_row + (i * count / num_batches);
      uint64_t last_row_in_batch = first_row_in_batch + count / num_batches;

      for (int j = first_row_in_batch; j < last_row_in_batch; j++) {
        auto op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
        auto* const req = op->mutable_request();
        QLAddInt32HashValue(req, j);
        table.AddInt32ColumnValue(req, "int_val", j * 2);
        table.AddStringColumnValue(req, "string_val", StringPrintf("hello %d", j));
        ASSERT_OK(session->Apply(op));
      }

      // We don't handle write idempotency yet. (i.e making sure that when a leader fails
      // writes to it that were eventually committed by the new leader but un-ackd to the
      // client are not retried), so some errors are expected.
      // It's OK as long as the errors are Status::AlreadyPresent();

      int inserted = last_row_in_batch - first_row_in_batch;

      Status s = session->Flush();
      if (PREDICT_FALSE(!s.ok())) {
        client::CollectedErrors errors = session->GetAndClearPendingErrors();
        for (const auto& e : errors) {
          ASSERT_TRUE(e->status().IsAlreadyPresent()) << "Unexpected error: " << e->status();
        }
        inserted -= errors.size();
      }

      for (CountDownLatch* latch : latches) {
        latch->CountDown(inserted);
      }
    }

    inserters_.CountDown();
  }

  // Brings Chaos to a MiniTabletServer by introducing random delays. Does this by
  // pausing the daemon a random amount of time.
  void DelayInjectorThread(ExternalTabletServer* tablet_server, int timeout_msec) {
    while (inserters_.count() > 0) {

      // Adjust the value obtained from the normalized gauss. dist. so that we steal the lock
      // longer than the timeout a small (~5%) percentage of the times.
      // (95% corresponds to 1.64485, in a normalized (0,1) gaussian distribution).
      double sleep_time_usec = 1000 *
          ((random_.Normal(0, 1) * timeout_msec) / 1.64485);

      if (sleep_time_usec < 0) sleep_time_usec = 0;

      // Additionally only cause timeouts at all 50% of the time, otherwise sleep.
      double val = (rand() * 1.0) / RAND_MAX;  // NOLINT(runtime/threadsafe_fn)
      if (val < 0.5) {
        SleepFor(MonoDelta::FromMicroseconds(sleep_time_usec));
        continue;
      }

      ASSERT_OK(tablet_server->Pause());
      LOG_IF(INFO, sleep_time_usec > 0.0)
          << "Delay injector thread for TS " << tablet_server->instance_id().permanent_uuid()
          << " SIGSTOPped the ts, sleeping for " << sleep_time_usec << " usec...";
      SleepFor(MonoDelta::FromMicroseconds(sleep_time_usec));
      ASSERT_OK(tablet_server->Resume());
    }
  }

  // Thread which loops until '*finish' becomes true, trying to insert a row
  // on the given tablet server identified by 'replica_idx'.
  void StubbornlyWriteSameRowThread(int replica_idx, const AtomicBool* finish);

  // Stops the current leader of the configuration, runs leader election and then brings it back.
  // Before stopping the leader this pauses all follower nodes in regular intervals so that
  // we get an increased chance of stuff being pending.
  void StopOrKillLeaderAndElectNewOne() {
    bool kill = rand() % 2 == 0;  // NOLINT(runtime/threadsafe_fn)

    TServerDetails* old_leader;
    CHECK_OK(GetLeaderReplicaWithRetries(tablet_id_, &old_leader));
    ExternalTabletServer* old_leader_ets = cluster_->tablet_server_by_uuid(old_leader->uuid());

    vector<TServerDetails*> followers;
    GetOnlyLiveFollowerReplicas(tablet_id_, &followers);

    for (TServerDetails* ts : followers) {
      ExternalTabletServer* ets = cluster_->tablet_server_by_uuid(ts->uuid());
      CHECK_OK(ets->Pause());
      SleepFor(MonoDelta::FromMilliseconds(100));
    }

    // When all are paused also pause or kill the current leader. Since we've waited a bit
    // the old leader is likely to have operations that must be aborted.
    if (kill) {
      old_leader_ets->Shutdown();
    } else {
      CHECK_OK(old_leader_ets->Pause());
    }

    // Resume the replicas.
    for (TServerDetails* ts : followers) {
      ExternalTabletServer* ets = cluster_->tablet_server_by_uuid(ts->uuid());
      CHECK_OK(ets->Resume());
    }

    // Get the new leader.
    TServerDetails* new_leader;
    CHECK_OK(GetLeaderReplicaWithRetries(tablet_id_, &new_leader));

    // Bring the old leader back.
    if (kill) {
      CHECK_OK(old_leader_ets->Restart());
      // Wait until we have the same number of followers.
      int initial_followers = followers.size();
      do {
        GetOnlyLiveFollowerReplicas(tablet_id_, &followers);
      } while (followers.size() < initial_followers);
    } else {
      CHECK_OK(old_leader_ets->Resume());
    }
  }

  // Writes 'num_writes' operations to the current leader. Each of the operations
  // has a payload of around `size_bytes`. Causes a gtest failure on error.
  void WriteOpsToLeader(int num_writes, size_t size_bytes);

  // Check for and restart any TS that have crashed.
  // Returns the number of servers restarted.
  int RestartAnyCrashedTabletServers();

  // Assert that no tablet servers have crashed.
  // Tablet servers that have been manually Shutdown() are allowed.
  void AssertNoTabletServersCrashed();

  Result<int64_t> GetNumLogCacheOpsReadFromDisk();

  // Ensure that a majority of servers is required for elections and writes.
  // This is done by pausing a majority and asserting that writes and elections fail,
  // then unpausing the majority and asserting that elections and writes succeed.
  // If fails, throws a gtest assertion.
  // Note: This test assumes all tablet servers listed in tablet_servers are voters.
  void AssertMajorityRequiredForElectionsAndWrites(const TabletServerMapUnowned& tablet_servers,
                                                   const string& leader_uuid);

  // Return the replicas of the specified 'tablet_id', as seen by the Master.
  Status GetTabletLocations(const string& tablet_id, const MonoDelta& timeout,
                            master::TabletLocationsPB* tablet_locations);

  enum WaitForLeader {
    NO_WAIT_FOR_LEADER = 0,
    WAIT_FOR_LEADER = 1
  };

  // Wait for the specified number of replicas to be reported by the master for
  // the given tablet. Fails with an assertion if the timeout expires.
  void WaitForReplicasReportedToMaster(int num_replicas, const string& tablet_id,
                                       const MonoDelta& timeout,
                                       WaitForLeader wait_for_leader,
                                       bool* has_leader,
                                       master::TabletLocationsPB* tablet_locations);

  static const bool WITH_NOTIFICATION_LATENCY = true;
  static const bool WITHOUT_NOTIFICATION_LATENCY = false;
  void DoTestChurnyElections(bool with_latency);

 protected:
  // Flags needed for CauseFollowerToFallBehindLogGC() to work well.
  void AddFlagsForLogRolls(vector<string>* extra_tserver_flags);

  // Pause one of the followers and write enough data to the remaining replicas
  // to cause log GC, then resume the paused follower. On success,
  // 'leader_uuid' will be set to the UUID of the leader, 'orig_term' will be
  // set to the term of the leader before un-pausing the follower, and
  // 'fell_behind_uuid' will be set to the UUID of the follower that was paused
  // and caused to fall behind. These can be used for verification purposes.
  //
  // Certain flags should be set. You can add the required flags with
  // AddFlagsForLogRolls() before starting the cluster.
  void CauseFollowerToFallBehindLogGC(string* leader_uuid,
                                      int64_t* orig_term,
                                      string* fell_behind_uuid);

  void TestAddRemoveServer(RaftPeerPB::MemberType member_type);
  void TestRemoveTserverFailsWhenServerInTransition(RaftPeerPB::MemberType member_type);
  void TestRemoveTserverInTransitionSucceeds(RaftPeerPB::MemberType member_type);

  std::vector<scoped_refptr<yb::Thread> > threads_;
  CountDownLatch inserters_;
  ClockPtr clock_;
};

void RaftConsensusITest::AddFlagsForLogRolls(vector<string>* extra_tserver_flags) {
  // We configure a small log segment size so that we roll frequently,
  // configure a small cache size so that we evict data from the cache, and
  // retain as few segments as possible. We also turn off async segment
  // allocation -- this ensures that we roll many segments of logs (with async
  // allocation, it's possible that the preallocation is slow and we wouldn't
  // roll deterministically).
  extra_tserver_flags->push_back("--log_cache_size_limit_mb=1");
  extra_tserver_flags->push_back("--log_segment_size_mb=1");
  extra_tserver_flags->push_back("--log_async_preallocate_segments=false");
  extra_tserver_flags->push_back("--log_min_segments_to_retain=1");
  extra_tserver_flags->push_back("--log_min_seconds_to_retain=0");
  extra_tserver_flags->push_back("--maintenance_manager_polling_interval_ms=100");
  extra_tserver_flags->push_back("--db_write_buffer_size=100000");
}

// Test that we can retrieve the permanent uuid of a server running
// consensus service via RPC.
TEST_F(RaftConsensusITest, TestGetPermanentUuid) {
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  TServerDetails* leader = nullptr;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));
  const string expected_uuid = leader->instance_id.permanent_uuid();

  rpc::MessengerBuilder builder("test builder");
  builder.set_num_reactors(1);
  auto messenger = rpc::CreateAutoShutdownMessengerHolder(ASSERT_RESULT(builder.Build()));
  rpc::ProxyCache proxy_cache(messenger.get());

  // Set a decent timeout for allowing the masters to find eachother.
  const auto kTimeout = 30s;
  std::vector<HostPort> endpoints;
  for (const auto& hp : leader->registration.common().private_rpc_addresses()) {
    endpoints.push_back(HostPortFromPB(hp));
  }
  RaftPeerPB peer;
  ASSERT_OK(consensus::SetPermanentUuidForRemotePeer(&proxy_cache, kTimeout, endpoints, &peer));
  ASSERT_EQ(expected_uuid, peer.permanent_uuid());
}

// TODO allow the scan to define an operation id, fetch the last id
// from the leader and then use that id to make the replica wait
// until it is done. This will avoid the sleeps below.
TEST_F(RaftConsensusITest, TestInsertAndMutateThroughConsensus) {
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  int num_iters = AllowSlowTests() ? 10 : 1;

  for (int i = 0; i < num_iters; i++) {
    ASSERT_NO_FATALS(InsertTestRowsRemoteThread(
        i * FLAGS_client_inserts_per_thread,
        FLAGS_client_inserts_per_thread,
        FLAGS_client_num_batches_per_thread,
        vector<CountDownLatch*>()));
  }
  ASSERT_ALL_REPLICAS_AGREE(FLAGS_client_inserts_per_thread * num_iters);
}

TEST_F(RaftConsensusITest, TestFailedOperation) {
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  // Wait until we have a stable leader.
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10), tablet_servers_,
                                  tablet_id_, 1));

  WriteResponsePB resp;
  RpcController controller;
  controller.set_timeout(MonoDelta::FromSeconds(FLAGS_rpc_timeout));

  TServerDetails* leader = nullptr;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));

  WriteRequestPB req;
  req.set_tablet_id(tablet_id_);
  req.add_ql_write_batch()->set_schema_version(123);

  ASSERT_OK(DCHECK_NOTNULL(leader->tserver_proxy.get())->Write(req, &resp, &controller));
  ASSERT_NE(QLResponsePB::YQL_STATUS_OK, resp.ql_response_batch(0).status())
      << "Response: " << resp.ShortDebugString();

  // Add a proper row so that we can verify that all of the replicas continue
  // to process transactions after a failure. Additionally, this allows us to wait
  // for all of the replicas to finish processing transactions before shutting down,
  // avoiding a potential stall as we currently can't abort transactions (see KUDU-341).

  req.Clear();
  req.set_tablet_id(tablet_id_);
  AddTestRowInsert(0, 0, "original0", &req);

  controller.Reset();
  controller.set_timeout(MonoDelta::FromSeconds(FLAGS_rpc_timeout));

  ASSERT_OK(DCHECK_NOTNULL(leader->tserver_proxy.get())->Write(req, &resp, &controller));
  SCOPED_TRACE(resp.ShortDebugString());
  ASSERT_FALSE(resp.has_error());

  ASSERT_ALL_REPLICAS_AGREE(1);
}

// Inserts rows through consensus and also starts one delay injecting thread
// that steals consensus peer locks for a while. This is meant to test that
// even with timeouts and repeated requests consensus still works.
TEST_F(RaftConsensusITest, MultiThreadedMutateAndInsertThroughConsensus) {
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  if (500 == FLAGS_client_inserts_per_thread) {
    if (AllowSlowTests()) {
      FLAGS_client_inserts_per_thread = FLAGS_client_inserts_per_thread * 10;
      FLAGS_client_num_batches_per_thread = FLAGS_client_num_batches_per_thread * 10;
    }
  }

  int num_threads = FLAGS_num_client_threads;
  for (int i = 0; i < num_threads; i++) {
    scoped_refptr<yb::Thread> new_thread;
    CHECK_OK(yb::Thread::Create("test", strings::Substitute("ts-test$0", i),
                                  &RaftConsensusITest::InsertTestRowsRemoteThread,
                                  this, i * FLAGS_client_inserts_per_thread,
                                  FLAGS_client_inserts_per_thread,
                                  FLAGS_client_num_batches_per_thread,
                                  vector<CountDownLatch*>(),
                                  &new_thread));
    threads_.push_back(new_thread);
  }
  for (int i = 0; i < FLAGS_num_replicas; i++) {
    scoped_refptr<yb::Thread> new_thread;
    CHECK_OK(yb::Thread::Create("test", strings::Substitute("chaos-test$0", i),
                                  &RaftConsensusITest::DelayInjectorThread,
                                  this, cluster_->tablet_server(i),
                                  kConsensusRpcTimeoutForTests,
                                  &new_thread));
    threads_.push_back(new_thread);
  }
  for (scoped_refptr<yb::Thread> thr : threads_) {
    CHECK_OK(ThreadJoiner(thr.get()).Join());
  }

  ASSERT_ALL_REPLICAS_AGREE(FLAGS_client_inserts_per_thread * FLAGS_num_client_threads);
}

TEST_F(RaftConsensusITest, TestReadOnNonLeader) {
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  // Wait for the initial leader election to complete.
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10), tablet_servers_,
                                  tablet_id_, 1));

  // By default reads should be allowed only on the leader.
  ReadRequestPB req;
  ReadResponsePB resp;
  RpcController rpc;
  req.set_tablet_id(tablet_id_);

  // Perform a read on one of the followers.
  vector<TServerDetails*> followers;
  GetOnlyLiveFollowerReplicas(tablet_id_, &followers);

  for (const auto& follower : followers) {
    rpc.Reset();
    ASSERT_OK(follower->tserver_proxy->Read(req, &resp, &rpc));
    ASSERT_TRUE(resp.has_error());
    ASSERT_EQ(TabletServerErrorPB_Code_NOT_THE_LEADER, resp.error().code());
  }
}

TEST_F(RaftConsensusITest, TestInsertOnNonLeader) {
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  // Wait for the initial leader election to complete.
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10), tablet_servers_,
                                  tablet_id_, 1));

  // Manually construct a write RPC to a replica and make sure it responds
  // with the correct error code.
  WriteRequestPB req;
  WriteResponsePB resp;
  RpcController rpc;
  req.set_tablet_id(tablet_id_);
  AddTestRowInsert(kTestRowKey, kTestRowIntVal, "hello world via RPC", &req);

  // Get the leader.
  vector<TServerDetails*> followers;
  GetOnlyLiveFollowerReplicas(tablet_id_, &followers);

  ASSERT_OK(followers[0]->tserver_proxy->Write(req, &resp, &rpc));
  SCOPED_TRACE(resp.DebugString());
  ASSERT_TRUE(resp.has_error());
  Status s = StatusFromPB(resp.error().status());
  EXPECT_TRUE(s.IsIllegalState());
  ASSERT_STR_CONTAINS(s.message().ToBuffer(), "Not the leader");
  // TODO: need to change the error code to be something like REPLICA_NOT_LEADER
  // so that the client can properly handle this case! plumbing this is a little difficult
  // so not addressing at the moment.
  ASSERT_ALL_REPLICAS_AGREE(0);
}

TEST_F(RaftConsensusITest, TestRunLeaderElection) {
  // Reset consensus rpc timeout to the default value or the election might fail often.
  FLAGS_consensus_rpc_timeout_ms = 1000;

  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  int num_iters = AllowSlowTests() ? 10 : 1;

  ASSERT_NO_FATALS(InsertTestRowsRemoteThread(
      0,
      FLAGS_client_inserts_per_thread * num_iters,
      FLAGS_client_num_batches_per_thread,
      vector<CountDownLatch*>()));

  ASSERT_ALL_REPLICAS_AGREE(FLAGS_client_inserts_per_thread * num_iters);

  // Select the last follower to be new leader.
  vector<TServerDetails*> followers;
  GetOnlyLiveFollowerReplicas(tablet_id_, &followers);

  // Now shutdown the current leader.
  TServerDetails* leader = DCHECK_NOTNULL(GetLeaderReplicaOrNull(tablet_id_));
  ExternalTabletServer* leader_ets = cluster_->tablet_server_by_uuid(leader->uuid());
  leader_ets->Shutdown();

  TServerDetails* replica = followers.back();
  CHECK_NE(leader->instance_id.permanent_uuid(), replica->instance_id.permanent_uuid());

  // Make the new replica leader.
  ASSERT_OK(StartElection(replica, tablet_id_, MonoDelta::FromSeconds(10)));

  // Insert a bunch more rows.
  ASSERT_NO_FATALS(InsertTestRowsRemoteThread(
      FLAGS_client_inserts_per_thread * num_iters,
      FLAGS_client_inserts_per_thread * num_iters,
      FLAGS_client_num_batches_per_thread,
      vector<CountDownLatch*>()));

  // Restart the original replica and make sure they all agree.
  ASSERT_OK(leader_ets->Restart());

  ASSERT_ALL_REPLICAS_AGREE(FLAGS_client_inserts_per_thread * num_iters * 2);
}

void RaftConsensusITest::WriteOpsToLeader(int num_writes, size_t size_bytes) {
  TServerDetails* leader = nullptr;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));

  WriteRequestPB req;
  WriteResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(10000));
  int key = 0;

  // generate dummy payload.
  string test_payload(size_bytes, '0');
  for (int i = 0; i < num_writes; i++) {
    rpc.Reset();
    req.Clear();
    req.set_tablet_id(tablet_id_);
    AddTestRowInsert(key, key, test_payload, &req);
    key++;
    ASSERT_OK(leader->tserver_proxy->Write(req, &resp, &rpc));

    ASSERT_FALSE(resp.has_error()) << resp.DebugString();
  }
}

// Test that when a follower is stopped for a long time, the log cache
// properly evicts operations, but still allows the follower to catch
// up when it comes back.
TEST_F(RaftConsensusITest, TestCatchupAfterOpsEvicted) {
  vector<string> extra_flags;
  extra_flags.push_back("--log_cache_size_limit_mb=1");
  extra_flags.push_back("--rpc_throttle_threshold_bytes=-1");
  extra_flags.push_back("--consensus_max_batch_size_bytes=500000");
  ASSERT_NO_FATALS(BuildAndStart(extra_flags));
  TServerDetails* replica = (*tablet_replicas_.begin()).second;
  ASSERT_TRUE(replica != nullptr);
  ExternalTabletServer* replica_ets = cluster_->tablet_server_by_uuid(replica->uuid());

  // Pause a replica.
  ASSERT_OK(replica_ets->Pause());
  LOG(INFO)<< "Paused one of the replicas, starting to write.";

  // Insert 3MB worth of data.
  const int kNumWrites = 25;
  ASSERT_NO_FATALS(WriteOpsToLeader(kNumWrites, 128_KB));

  // Now unpause the replica, the lagging replica should eventually catch back up.
  ASSERT_OK(replica_ets->Resume());

  ASSERT_ALL_REPLICAS_AGREE(kNumWrites);
}

Result<int64_t> RaftConsensusITest::GetNumLogCacheOpsReadFromDisk() {
  TServerDetails* leader = nullptr;
  RETURN_NOT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));
  return cluster_->tablet_server_by_uuid(leader->uuid())->GetInt64Metric(
      &METRIC_ENTITY_tablet,
      nullptr,
      &METRIC_log_cache_disk_reads,
      "value");
}

// Test that when a follower is stopped for a long time, the log cache
// reads few ops from disk due to exponential backoff on number of ops
// to replicate to an unresponsive follower.
TEST_F(RaftConsensusITest, TestCatchupOpsReadFromDisk) {
  vector<string> extra_flags = {
      "--log_cache_size_limit_mb=1"s,
      "--enable_consensus_exponential_backoff=true"s
  };
  ASSERT_NO_FATALS(BuildAndStart(extra_flags));
  TServerDetails* replica = tablet_replicas_.begin()->second;
  ASSERT_TRUE(replica != nullptr);
  ExternalTabletServer* replica_ets = cluster_->tablet_server_by_uuid(replica->uuid());

  // Pause a replica.
  ASSERT_OK(replica_ets->Pause());
  LOG(INFO) << "Paused replica " << replica->uuid() << ", starting to write.";

  // Insert 3MB worth of data.
  const int kNumWrites = 1000;
  ASSERT_NO_FATALS(WriteOpsToLeader(kNumWrites, 3_KB));

  // Allow for unsuccessful replication attempts to lagging follower.
  SleepFor(5s);

  // Confirm that in the steady state the leader is not reading any new ops from disk since it has
  // reached 0 and is sending only NOOP.
  auto ops_before = ASSERT_RESULT(GetNumLogCacheOpsReadFromDisk());
  SleepFor(10s);
  auto ops_after = ASSERT_RESULT(GetNumLogCacheOpsReadFromDisk());
  EXPECT_EQ(ops_before, ops_after);

  // Now unpause the replica, the lagging replica should eventually catch back up.
  ASSERT_OK(replica_ets->Resume());

  // Wait for successful replication to resumed follower.
  ASSERT_OK(WaitForServersToAgree(30s, tablet_servers_, tablet_id_,
                                  kNumWrites));

  auto final_ops_read_from_disk = ASSERT_RESULT(GetNumLogCacheOpsReadFromDisk());
  LOG(INFO)<< "Ops read from disk: " << final_ops_read_from_disk;

  // NOTE: empirically determined threshold.
  const int kOpsReadFromDiskThreshold = kNumWrites * 2;
  ASSERT_LE(final_ops_read_from_disk, kOpsReadFromDiskThreshold);
}

// Test that when a follower is paused so that it misses several writes, is resumed
// briefly such that it only receives some of the entries from the leader but
// becomes aware of the highest committed op id, it is able to bootstrap successfully
// after a restart.
TEST_F(RaftConsensusITest, TestLaggingFollowerRestart) {
  vector<string> extra_flags = {
      "--consensus_inject_latency_ms_in_notifications=10"s,
      "--rpc_throttle_threshold_bytes=-1"s,
      "--consensus_max_batch_size_bytes=1024"s
  };
  ASSERT_NO_FATALS(BuildAndStart(extra_flags));

  // Find leader.
  TServerDetails* leader = nullptr;
  ASSERT_OK(FindTabletLeader(tablet_servers_, tablet_id_, 30s, &leader));
  CHECK_NOTNULL(leader);

  // Find a follower to pause, resume, and restart.
  TServerDetails* replica = nullptr;
  for (const auto& tablet_replica : tablet_replicas_) {
      TServerDetails* ts_details = tablet_replica.second;
      if (ts_details->uuid() != leader->uuid()) {
        replica = ts_details;
        break;
      }
  }
  CHECK_NOTNULL(replica);
  ASSERT_NE(replica->uuid(), leader->uuid());

  ExternalTabletServer* replica_ets = cluster_->tablet_server_by_uuid(replica->uuid());

  // Pause a replica.
  ASSERT_OK(replica_ets->Pause());
  LOG(INFO)<< "Paused replica " << replica->uuid() << ", starting to write.";

  // Insert 3MB worth of data.
  const int kNumWrites = 1000;
  ASSERT_NO_FATALS(WriteOpsToLeader(kNumWrites, 3_KB));

  // Allow for paused replica to lag.
  SleepFor(20s);

  // Now unpause the replica, the lagging replica should eventually catch back up.
  ASSERT_OK(replica_ets->Resume());
  LOG(INFO)<< "Resumed replica " << replica->uuid() << ".";

  // Wait for resumed follower to get some but not all entries.
  int64_t actual_minimum_index = consensus::kInvalidOpIdIndex;
  ASSERT_OK(WaitUntilAllReplicasHaveOp(kNumWrites / 4, tablet_id_,
                                       TServerDetailsVector(tablet_servers_), 10s,
                                       &actual_minimum_index));
  LOG(INFO) << "Replica " << replica->uuid() << " received " << actual_minimum_index;
  ASSERT_LE(actual_minimum_index, kNumWrites / 2);

  replica_ets->Shutdown();
  LOG(INFO)<< "Shutdown replica " << replica->uuid() << ".";

  CHECK_OK(replica_ets->Restart());
  LOG(INFO)<< "Restarted replica " << replica->uuid() << ".";

  // Wait for successful replication to restarted follower.
  ASSERT_OK(WaitForServersToAgree(60s, tablet_servers_, tablet_id_, kNumWrites));
}

// Test that when a follower is shutdown so that it misses several writes, the
// leader evicts almost all entries from log cache.
TEST_F(RaftConsensusITest, TestLaggingFollowerLogCacheEviction) {
  const auto kHeartBeatInterval = 1s;
  const int32_t kConsensusLaggingFollowerThreshold = 10;

  vector<string> extra_flags = {
      "--consensus_inject_latency_ms_in_notifications=10"s,
      "--rpc_throttle_threshold_bytes=-1"s,
      "--consensus_max_batch_size_bytes=1024"s,
      Format("--consensus_lagging_follower_threshold=$0", kConsensusLaggingFollowerThreshold),
      Format("--raft_heartbeat_interval_ms=$0", ToMilliseconds(kHeartBeatInterval))
  };
  ASSERT_NO_FATALS(BuildAndStart(extra_flags));

  // Find leader.
  TServerDetails* leader = nullptr;
  ASSERT_OK(FindTabletLeader(tablet_servers_, tablet_id_, 30s, &leader));
  CHECK_NOTNULL(leader);

  // Find a follower to pause, resume, and restart.
  TServerDetails* replica = nullptr;
  for (const auto& tablet_replica : tablet_replicas_) {
    TServerDetails* ts_details = tablet_replica.second;
    if (ts_details->uuid() != leader->uuid()) {
      replica = ts_details;
      break;
    }
  }
  CHECK_NOTNULL(replica);
  ASSERT_NE(replica->uuid(), leader->uuid());

  ExternalTabletServer* replica_ets = cluster_->tablet_server_by_uuid(replica->uuid());

  // Shutdown a replica.
  replica_ets->Shutdown();
  LOG(INFO)<< "Shutdown replica " << replica->uuid() << ".";

  // Insert 3MB worth of data.
  const int kNumWrites = 1000;
  ASSERT_NO_FATALS(WriteOpsToLeader(kNumWrites, 3_KB));

  // Allow for shutdown replica to be retransmitted to more than
  // consensus_lagging_follower_threshold times so that it can be marked as lagging.
  // Also use a multiplicative factor to reduce flakiness.
  SleepFor(kHeartBeatInterval * kConsensusLaggingFollowerThreshold * 2);

  // Allow for shutdown replica to lag.
  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);
  ASSERT_EQ(1, active_tablet_servers.erase(replica->uuid()));
  ASSERT_OK(WaitForServersToAgree(60s, active_tablet_servers, tablet_id_, kNumWrites));

  // Find log cache num ops at leader - value from non-leaders will be zero,
  // hence it is unncessary to omit values from followers.
  int64_t log_cache_num_ops = 0;
  for (const auto& tablet_replica : tablet_replicas_) {
    // Skip shutdown replica.
    if (tablet_replica.second->uuid() == replica->uuid()) {
      continue;
    }

    log_cache_num_ops += ASSERT_RESULT(
        cluster_->tablet_server_by_uuid(tablet_replica.second->uuid())->GetInt64Metric(
            &METRIC_ENTITY_tablet,
            nullptr,
            &METRIC_log_cache_num_ops,
            "value"));
  }
  LOG(INFO)<< "Num ops in log cache: " << log_cache_num_ops;

  // NOTE: empirically determined threshold.
  const int kLogCacheNumOpsThreshold = kNumWrites / 10;
  ASSERT_LE(log_cache_num_ops, kLogCacheNumOpsThreshold);

  // Now restart the replica shutdown earlier, the lagging replica should eventually catch back up.
  CHECK_OK(replica_ets->Restart());
  LOG(INFO)<< "Restarted replica " << replica->uuid() << ".";

  // Wait for successful replication to restarted follower.
  ASSERT_OK(WaitForServersToAgree(60s, tablet_servers_, tablet_id_, kNumWrites));
}

TEST_F(RaftConsensusITest, TestAddRemoveNonVoter) {
  MonoDelta kTimeout = MonoDelta::FromSeconds(30);
  FLAGS_num_tablet_servers = 3;
  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    "--TEST_inject_latency_before_change_role_secs=1"s,
    "--follower_unavailable_considered_failed_sec=5"s,
  };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  // Elect server 0 as leader and wait for log index 1 to propagate to all servers.
  TServerDetails* leader_tserver = tservers[0];
  const string& leader_uuid = tservers[0]->uuid();
  ASSERT_OK(StartElection(leader_tserver, tablet_id_, kTimeout));
  int64_t min_index = 1;
  ASSERT_OK(WaitForServersToAgree(kTimeout, tablet_servers_, tablet_id_, min_index, &min_index));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIsAtLeast(&min_index, leader_tserver, tablet_id_, kTimeout));

  // Kill the master, so we can change the config without interference.
  cluster_->master()->Shutdown();

  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);

  // Do majority correctness check for 3 servers.
  ASSERT_NO_FATALS(AssertMajorityRequiredForElectionsAndWrites(active_tablet_servers, leader_uuid));
  OpIdPB opid;
  ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader_tserver, consensus::RECEIVED_OPID, kTimeout,
                                  &opid));
  int64_t cur_log_index = opid.index();

  // Remove tablet server 2, so we can add it as an observer.
  vector<int> remove_list = { 2, 1 };
  LOG(INFO) << "Remove: Going from 3 voter replicas to 2 voter replicas";

  TServerDetails* tserver_to_remove = tservers[2];
  LOG(INFO) << "Removing tserver with uuid " << tserver_to_remove->uuid();
  ASSERT_OK(RemoveServer(leader_tserver, tablet_id_, tserver_to_remove, boost::none, kTimeout));
  ASSERT_EQ(1, active_tablet_servers.erase(tserver_to_remove->uuid()));
  ASSERT_OK(WaitForServersToAgree(kTimeout, active_tablet_servers, tablet_id_, ++cur_log_index));

  // Do majority correctness check for each incremental decrease.
  ASSERT_NO_FATALS(AssertMajorityRequiredForElectionsAndWrites(active_tablet_servers, leader_uuid));
  ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader_tserver, consensus::RECEIVED_OPID, kTimeout,
      &opid));
  cur_log_index = opid.index();

  // Add the tablet server back as an observer.
  LOG(INFO) << "Add: Creating a new read replica";

  TServerDetails* tserver_to_add = tservers[2];
  LOG(INFO) << "Adding tserver with uuid " << tserver_to_add->uuid();

  ASSERT_OK(DeleteTablet(tserver_to_add, tablet_id_, tablet::TABLET_DATA_TOMBSTONED, boost::none,
                         kTimeout));

  ASSERT_OK(AddServer(leader_tserver, tablet_id_, tserver_to_add, RaftPeerPB::PRE_OBSERVER,
                      boost::none, kTimeout));

  consensus::ConsensusStatePB cstate;
  ASSERT_OK(itest::GetConsensusState(leader_tserver, tablet_id_,
                                     consensus::CONSENSUS_CONFIG_COMMITTED, kTimeout, &cstate));

  // Verify that this tserver member type was set correctly.
  for (const auto& peer : cstate.config().peers()) {
    if (peer.permanent_uuid() == tserver_to_add->uuid()) {
      ASSERT_EQ(RaftPeerPB::PRE_OBSERVER, peer.member_type());
    }
  }

  // Wait until the ChangeConfig has finished and this tserver has been made a VOTER.
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(), leader_tserver,
                                                tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitUntilCommittedConfigMemberTypeIs(1, leader_tserver, tablet_id_,
                                                 MonoDelta::FromSeconds(10),
                                                 RaftPeerPB::OBSERVER));

  ASSERT_OK(WaitForServersToAgree(kTimeout, active_tablet_servers, tablet_id_, ++cur_log_index));

  // Pause the read replica and assert that it gets removed from the configuration.
  ASSERT_OK(cluster_->tablet_server_by_uuid(tserver_to_add->uuid())->Pause());
  ASSERT_OK(WaitUntilCommittedConfigMemberTypeIs(0, leader_tserver, tablet_id_,
                                                 MonoDelta::FromSeconds(20),
                                                 RaftPeerPB::OBSERVER));
}

void RaftConsensusITest::CauseFollowerToFallBehindLogGC(string* leader_uuid,
                                                        int64_t* orig_term,
                                                        string* fell_behind_uuid) {
  MonoDelta kTimeout = MonoDelta::FromSeconds(10);
  // Wait for all of the replicas to have acknowledged the elected
  // leader and logged the first NO_OP.
  ASSERT_OK(WaitForServersToAgree(kTimeout, tablet_servers_, tablet_id_, 1));

  // Pause one server. This might be the leader, but pausing it will cause
  // a leader election to happen.
  TServerDetails* replica = (*tablet_replicas_.begin()).second;
  ExternalTabletServer* replica_ets = cluster_->tablet_server_by_uuid(replica->uuid());
  ASSERT_OK(replica_ets->Pause());

  // Find a leader. In case we paused the leader above, this will wait until
  // we have elected a new one.
  TServerDetails* leader = nullptr;
  while (true) {
    Status s = GetLeaderReplicaWithRetries(tablet_id_, &leader);
    if (s.ok() && leader != nullptr && leader != replica) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  *leader_uuid = leader->uuid();
  int leader_index = cluster_->tablet_server_index_by_uuid(*leader_uuid);

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_timeout_allowed(true);
  workload.set_payload_bytes(128 * 1024);  // Write ops of size 128KB.
  workload.set_write_batch_size(1);
  workload.set_num_write_threads(4);
  workload.Setup();
  workload.Start();

  LOG(INFO) << "Waiting until we've written at least 4MB...";
  while (workload.rows_inserted() < 8 * 4) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  workload.StopAndJoin();

  LOG(INFO) << "Waiting for log GC on " << leader->uuid();
  // Some WAL segments must exist, but wal segment 1 must not exist.

  ASSERT_OK(inspect_->WaitForFilePatternInTabletWalDirOnTs(
      leader_index, tablet_id_, { "wal-" }, { "wal-000000001" }));

  LOG(INFO) << "Log GC complete on " << leader->uuid();

  // Then wait another couple of seconds to be sure that it has bothered to try
  // to write to the paused peer.
  // TODO: would be nice to be able to poll the leader with an RPC like
  // GetLeaderStatus() which could tell us whether it has made any requests
  // since the log GC.
  SleepFor(MonoDelta::FromSeconds(2));

  // Make a note of whatever the current term of the cluster is,
  // before we resume the follower.
  {
    OpIdPB op_id;
    ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader, consensus::RECEIVED_OPID, kTimeout,
                                    &op_id));
    *orig_term = op_id.term();
    LOG(INFO) << "Servers converged with original term " << *orig_term;
  }

  // Resume the follower.
  LOG(INFO) << "Resuming " << replica->uuid();
  ASSERT_OK(replica_ets->Resume());

  // Ensure that none of the tablet servers crashed.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    // Make sure it didn't crash.
    ASSERT_TRUE(cluster_->tablet_server(i)->IsProcessAlive())
      << "Tablet server " << i << " crashed";
  }
  *fell_behind_uuid = replica->uuid();
}

void RaftConsensusITest::TestAddRemoveServer(RaftPeerPB::MemberType member_type) {
  ASSERT_TRUE(member_type == RaftPeerPB::PRE_VOTER || member_type == RaftPeerPB::PRE_OBSERVER);

  MonoDelta kTimeout = MonoDelta::FromSeconds(30);
  FLAGS_num_tablet_servers = 3;
  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    "--TEST_inject_latency_before_change_role_secs=1"s,
  };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  // Elect server 0 as leader and wait for log index 1 to propagate to all servers.
  TServerDetails* leader_tserver = tservers[0];
  const string& leader_uuid = tservers[0]->uuid();
  ASSERT_OK(StartElection(leader_tserver, tablet_id_, kTimeout));
  ASSERT_OK(WaitForServersToAgree(kTimeout, tablet_servers_, tablet_id_, 1));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(1, leader_tserver, tablet_id_, kTimeout));

  // Make sure the server rejects removal of itself from the configuration.
  Status s = RemoveServer(leader_tserver, tablet_id_, leader_tserver, boost::none, kTimeout, NULL,
                          false /* retry */);
  ASSERT_TRUE(s.IsInvalidArgument()) << "Should not be able to remove self from config: "
                                     << s.ToString();

  // Insert the row that we will update throughout the test.
  ASSERT_OK(WriteSimpleTestRow(
      leader_tserver, tablet_id_, kTestRowKey, kTestRowIntVal, "initial insert", kTimeout));

  // Kill the master, so we can change the config without interference.
  cluster_->master()->Shutdown();

  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);

  // Do majority correctness check for 3 servers.
  ASSERT_NO_FATALS(AssertMajorityRequiredForElectionsAndWrites(active_tablet_servers, leader_uuid));
  OpIdPB opid;
  ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader_tserver, consensus::RECEIVED_OPID, kTimeout,
                                  &opid));
  int64_t cur_log_index = opid.index();

  // Go from 3 tablet servers down to 1 in the configuration.
  vector<int> remove_list = { 2, 1 };
  for (int to_remove_idx : remove_list) {
    int num_servers = active_tablet_servers.size();
    LOG(INFO) << "Remove: Going from " << num_servers << " to " << num_servers - 1 << " replicas";

    TServerDetails* tserver_to_remove = tservers[to_remove_idx];
    LOG(INFO) << "Removing tserver with uuid " << tserver_to_remove->uuid();
    ASSERT_OK(RemoveServer(leader_tserver, tablet_id_, tserver_to_remove, boost::none, kTimeout));
    ASSERT_EQ(1, active_tablet_servers.erase(tserver_to_remove->uuid()));
    ASSERT_OK(WaitForServersToAgree(kTimeout, active_tablet_servers, tablet_id_, ++cur_log_index));

    // Do majority correctness check for each incremental decrease.
    ASSERT_NO_FATALS(AssertMajorityRequiredForElectionsAndWrites(
        active_tablet_servers, leader_uuid));
    ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader_tserver, consensus::RECEIVED_OPID, kTimeout,
                                    &opid));
    cur_log_index = opid.index();
  }

  int num_observers = 0;
  // Add the tablet servers back, in reverse order, going from 1 to 3 servers in the configuration.
  vector<int> add_list = { 1, 2 };
  for (int to_add_idx : add_list) {
    int num_servers = active_tablet_servers.size();
    if (RaftPeerPB::PRE_VOTER == member_type) {
      LOG(INFO) << "Add: Going from " << num_servers << " to " << num_servers + 1 << " replicas";
    } else {
      LOG(INFO) << "Add: Going from " << num_observers<< " to " << num_observers + 1
                << " observers";
    }

    TServerDetails* tserver_to_add = tservers[to_add_idx];
    LOG(INFO) << "Adding tserver with uuid " << tserver_to_add->uuid();

    ASSERT_OK(DeleteTablet(tserver_to_add, tablet_id_, tablet::TABLET_DATA_TOMBSTONED, boost::none,
                           kTimeout));

    ASSERT_OK(AddServer(leader_tserver, tablet_id_, tserver_to_add, member_type, boost::none,
                        kTimeout));

    consensus::ConsensusStatePB cstate;
    ASSERT_OK(itest::GetConsensusState(leader_tserver, tablet_id_,
                                       consensus::CONSENSUS_CONFIG_COMMITTED, kTimeout, &cstate));

    // Verify that this tserver member type was set correctly.
    for (const auto& peer : cstate.config().peers()) {
      if (peer.permanent_uuid() == tserver_to_add->uuid()) {
        ASSERT_EQ(member_type, peer.member_type());
        LOG(INFO) << "tserver with uuid " << tserver_to_add->uuid() << " was added as a "
                  << RaftPeerPB::MemberType_Name(peer.member_type());
      }
    }

    if (RaftPeerPB::PRE_VOTER == member_type) {
      InsertOrDie(&active_tablet_servers, tserver_to_add->uuid(), tserver_to_add);
    } else {
      num_observers++;
    }

    // Wait until the ChangeConfig has finished and this tserver has been made a VOTER.
    ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(), leader_tserver,
                                                    tablet_id_, MonoDelta::FromSeconds(10)));
    ASSERT_OK(WaitUntilCommittedConfigMemberTypeIs(num_observers, leader_tserver, tablet_id_,
                                                   MonoDelta::FromSeconds(10),
                                                   RaftPeerPB::OBSERVER));

    ASSERT_OK(WaitForServersToAgree(kTimeout, active_tablet_servers, tablet_id_, ++cur_log_index));

    // Do majority correctness check for each incremental increase.
    ASSERT_NO_FATALS(AssertMajorityRequiredForElectionsAndWrites(
        active_tablet_servers, leader_uuid));
    ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader_tserver, consensus::RECEIVED_OPID, kTimeout,
                                    &opid));
    cur_log_index = opid.index();
  }
}

void RaftConsensusITest::TestRemoveTserverFailsWhenServerInTransition(
    RaftPeerPB::MemberType member_type) {

  ASSERT_TRUE(member_type == RaftPeerPB::PRE_VOTER || member_type == RaftPeerPB::PRE_OBSERVER);
  FLAGS_num_tablet_servers = 3;
  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    "--TEST_inject_latency_before_change_role_secs=10"s,
  };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false",
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  // Elect server 0 as leader and wait for log index 1 to propagate to all servers.
  TServerDetails* initial_leader = tservers[0];
  const MonoDelta timeout = MonoDelta::FromSeconds(10);
  ASSERT_OK(StartElection(initial_leader, tablet_id_, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, tablet_servers_, tablet_id_, 1));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(1, initial_leader, tablet_id_, timeout));

  // The server we will remove and then bring back.
  TServerDetails* tserver = tservers[2];

  // Kill the master, so we can change the config without interference.
  cluster_->master()->Shutdown();

  // Now remove server 2 from the configuration.
  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);
  LOG(INFO) << "Removing tserver with uuid " << tserver->uuid();
  ASSERT_OK(RemoveServer(initial_leader, tablet_id_, tserver, boost::none,
                         MonoDelta::FromSeconds(10)));
  ASSERT_EQ(1, active_tablet_servers.erase(tserver->uuid()));
  int64_t cur_log_index = 2;
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10),
                                  active_tablet_servers, tablet_id_, cur_log_index));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(cur_log_index, initial_leader, tablet_id_, timeout));

  ASSERT_OK(DeleteTablet(tserver, tablet_id_, tablet::TABLET_DATA_TOMBSTONED, boost::none,
                         MonoDelta::FromSeconds(30)));

  // Now add server 2 back as a learner to the peers.
  LOG(INFO) << "Adding back Peer " << tserver->uuid();
  ASSERT_OK(AddServer(initial_leader, tablet_id_, tserver, member_type, boost::none,
                      MonoDelta::FromSeconds(10)));

  // Only wait for TS 0 and 1 to agree that the new change config op (ADD_SERVER for server 2)
  // has been replicated.
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(60),
                                  active_tablet_servers, tablet_id_, ++cur_log_index));

  // Now try to remove server 1 from the configuration. This should fail.
  LOG(INFO) << "Removing tserver with uuid " << tservers[1]->uuid();
  auto status = RemoveServer(initial_leader, tablet_id_, tservers[1], boost::none,
                             MonoDelta::FromSeconds(10), NULL, false /* retry */);
  ASSERT_TRUE(status.IsIllegalState());
  ASSERT_STR_CONTAINS(status.ToString(), "Leader is not ready for Config Change");
}

// In TestRemoveTserverFailsWhenServerInTransition we are testing that a REMOVE_SERVER request
// operation fails whenever the committed config contains a server in PRE_VOTER or PRE_OBSERVER
// mode. In this test we are testing that a REMOVE_SERVER operation succeeds whenever the committed
// config contains a PRE_VOTER or PRE_OBSERVER mode and it's the same server we are trying to
// remove.
void RaftConsensusITest::TestRemoveTserverInTransitionSucceeds(RaftPeerPB::MemberType member_type) {
  ASSERT_TRUE(member_type == RaftPeerPB::PRE_VOTER || member_type == RaftPeerPB::PRE_OBSERVER);
  FLAGS_num_tablet_servers = 3;
  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    "--TEST_skip_change_role"s,
  };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  // Elect server 0 as leader and wait for log index 1 to propagate to all servers.
  TServerDetails* initial_leader = tservers[0];
  const MonoDelta timeout = MonoDelta::FromSeconds(10);
  ASSERT_OK(StartElection(initial_leader, tablet_id_, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, tablet_servers_, tablet_id_, 1));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(1, initial_leader, tablet_id_, timeout));

  // The server we will remove and then bring back.
  TServerDetails* tserver = tservers[2];

  // Kill the master, so we can change the config without interference.
  cluster_->master()->Shutdown();

  // Now remove server 2 from the configuration.
  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);
  LOG(INFO) << "Removing tserver with uuid " << tserver->uuid();
  ASSERT_OK(RemoveServer(initial_leader, tablet_id_, tserver, boost::none,
                         MonoDelta::FromSeconds(10)));
  ASSERT_EQ(1, active_tablet_servers.erase(tserver->uuid()));
  int64_t cur_log_index = 2;
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10),
                                  active_tablet_servers, tablet_id_, cur_log_index));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(cur_log_index, initial_leader, tablet_id_, timeout));

  ASSERT_OK(DeleteTablet(tserver, tablet_id_, tablet::TABLET_DATA_TOMBSTONED, boost::none,
                         MonoDelta::FromSeconds(30)));

  // Now add server 2 back in PRE_VOTER or PRE_OBSERVER mode. This server will never transition to
  // VOTER or OBSERVER because flag TEST_skip_change_role is set.
  LOG(INFO) << "Adding back Peer " << tserver->uuid();
  ASSERT_OK(AddServer(initial_leader, tablet_id_, tserver, member_type, boost::none,
                      MonoDelta::FromSeconds(10)));

  // Only wait for TS 0 and 1 to agree that the new change config op (ADD_SERVER for server 2)
  // has been replicated.
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(60),
                                  active_tablet_servers, tablet_id_, ++cur_log_index));

  // Now try to remove server 2 from the configuration. This should succeed.
  LOG(INFO) << "Removing tserver with uuid " << tservers[2]->uuid();
  ASSERT_OK(RemoveServer(initial_leader, tablet_id_, tservers[2], boost::none,
                         MonoDelta::FromSeconds(10)));
}

// Test that the leader doesn't crash if one of its followers has
// fallen behind so far that the logs necessary to catch it up
// have been GCed.
//
// In a real cluster, this will eventually cause the follower to be
// evicted/replaced. In any case, the leader should not crash.
//
// We also ensure that, when the leader stops writing to the follower,
// the follower won't disturb the other nodes when it attempts to elect
// itself.
//
// This is a regression test for KUDU-775 and KUDU-562.
TEST_F(RaftConsensusITest, TestFollowerFallsBehindLeaderGC) {
  // Disable follower eviction to maintain the original intent of this test.
  vector<string> extra_flags = { "--evict_failed_followers=false" };
  AddFlagsForLogRolls(&extra_flags);  // For CauseFollowerToFallBehindLogGC().
  ASSERT_NO_FATALS(BuildAndStart(extra_flags, std::vector<std::string>()));

  string leader_uuid;
  int64_t orig_term;
  string follower_uuid;
  ASSERT_NO_FATALS(CauseFollowerToFallBehindLogGC(&leader_uuid, &orig_term, &follower_uuid));

  // Wait for remaining majority to agree.
  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);
  ASSERT_EQ(3, active_tablet_servers.size());
  ASSERT_EQ(1, active_tablet_servers.erase(follower_uuid));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(30), active_tablet_servers, tablet_id_,
                                  1));

  if (AllowSlowTests()) {
    // Sleep long enough that the "abandoned" server's leader election interval
    // will trigger several times. Then, verify that the term has not increased.
    // This ensures that the other servers properly ignore the election requests
    // from the abandoned node.
    // TODO: would be nicer to use an RPC to check the current term of the
    // abandoned replica, and wait until it has incremented a couple of times.
    SleepFor(MonoDelta::FromSeconds(5));
    OpIdPB op_id;
    TServerDetails* leader = tablet_servers_[leader_uuid].get();
    ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader, consensus::RECEIVED_OPID,
                                    MonoDelta::FromSeconds(10), &op_id));
    ASSERT_EQ(orig_term, op_id.term())
      << "expected the leader to have not advanced terms but has op " << op_id;
  }
}

int RaftConsensusITest::RestartAnyCrashedTabletServers() {
  int restarted = 0;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    if (!cluster_->tablet_server(i)->IsProcessAlive()) {
      LOG(INFO) << "TS " << i << " appears to have crashed. Restarting.";
      cluster_->tablet_server(i)->Shutdown();
      CHECK_OK(cluster_->tablet_server(i)->Restart());
      restarted++;
    }
  }
  return restarted;
}

void RaftConsensusITest::AssertNoTabletServersCrashed() {
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    if (cluster_->tablet_server(i)->IsShutdown()) continue;

    ASSERT_TRUE(cluster_->tablet_server(i)->IsProcessAlive())
      << "Tablet server " << i << " crashed";
  }
}

// This test starts several tablet servers, and configures them with
// fault injection so that the leaders frequently crash just before
// sending RPCs to followers.
//
// This can result in various scenarios where leaders crash right after
// being elected and never succeed in replicating their first operation.
// For example, KUDU-783 reproduces from this test approximately 5% of the
// time on a slow-test debug build.
TEST_F(RaftConsensusITest, InsertWithCrashyNodes) {
  int kCrashesToCause = 3;
  vector<string> ts_flags, master_flags;
  if (AllowSlowTests()) {
    FLAGS_num_tablet_servers = 7;
    FLAGS_num_replicas = 7;
    kCrashesToCause = 15;
    master_flags.push_back("--replication_factor=7");
  }

  // Crash 5% of the time just before sending an RPC. With 7 servers,
  // this means we crash about 30% of the time before we've fully
  // replicated the NO_OP at the start of the term.
  ts_flags.push_back("--TEST_fault_crash_on_leader_request_fraction=0.05");

  // Inject latency to encourage the replicas to fall out of sync
  // with each other.
  ts_flags.push_back("--log_inject_latency");
  ts_flags.push_back("--log_inject_latency_ms_mean=30");
  ts_flags.push_back("--log_inject_latency_ms_stddev=60");

  // Make leader elections faster so we get through more cycles of
  // leaders.
  ts_flags.push_back("--raft_heartbeat_interval_ms=100");

  // Avoid preallocating segments since bootstrap is a little bit
  // faster if it doesn't have to scan forward through the preallocated
  // log area.
  ts_flags.push_back("--log_preallocate_segments=false");

  CreateCluster("raft_consensus-itest-cluster", ts_flags, master_flags);

  TestWorkload workload(cluster_.get());
  workload.set_timeout_allowed(true);
  workload.set_write_timeout_millis(1000);
  workload.set_num_write_threads(10);
  workload.set_write_batch_size(1);
  workload.Setup(client::YBTableType::YQL_TABLE_TYPE);
  workload.Start();

  int num_crashes = 0;
  while (num_crashes < kCrashesToCause &&
         workload.rows_inserted() < 100) {
    num_crashes += RestartAnyCrashedTabletServers();
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  workload.StopAndJoin();

  // After we stop the writes, we can still get crashes because heartbeats could
  // trigger the fault path. So, disable the faults and restart one more time.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ExternalTabletServer* ts = cluster_->tablet_server(i);
    vector<string>* flags = ts->mutable_flags();
    bool removed_flag = false;
    for (auto it = flags->begin(); it != flags->end(); ++it) {
      if (HasPrefixString(*it, "--TEST_fault_crash")) {
        flags->erase(it);
        removed_flag = true;
        break;
      }
    }
    ASSERT_TRUE(removed_flag) << "could not remove flag from TS " << i
                              << "\nFlags:\n" << *flags;
    ts->Shutdown();
    CHECK_OK(ts->Restart());
  }

  // Ensure that the replicas converge.
  // We don't know exactly how many rows got inserted, since the writer
  // probably saw many errors which left inserts in indeterminate state.
  // But, we should have at least as many as we got confirmation for.
  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(), ClusterVerifier::AT_LEAST,
                            workload.rows_inserted()));
}

// This test sets all of the election timers to be very short, resulting
// in a lot of churn. We expect to make some progress and not diverge or
// crash, despite the frequent re-elections and races.
TEST_F(RaftConsensusITest, TestChurnyElections) {
  DoTestChurnyElections(WITHOUT_NOTIFICATION_LATENCY);
}

// The same test, except inject artificial latency when propagating notifications
// from the queue back to consensus. This can reproduce bugs like KUDU-1078 which
// normally only appear under high load. TODO: Re-enable once we get to the
// bottom of KUDU-1078.
TEST_F(RaftConsensusITest, DISABLED_TestChurnyElections_WithNotificationLatency) {
  DoTestChurnyElections(WITH_NOTIFICATION_LATENCY);
}

void RaftConsensusITest::DoTestChurnyElections(bool with_latency) {
  vector<string> ts_flags, master_flags;

  // On TSAN builds, we need to be a little bit less churny in order to make
  // any progress at all.
  ts_flags.push_back(Format("--raft_heartbeat_interval_ms=$0", NonTsanVsTsan(5, 25)));
  ts_flags.push_back("--never_fsync");
  if (with_latency) {
    ts_flags.push_back("--consensus_inject_latency_ms_in_notifications=50");
  }

  CreateCluster("raft_consensus-itest-cluster", ts_flags, master_flags);

  TestWorkload workload(cluster_.get());
  workload.set_timeout_allowed(true);
  workload.set_write_timeout_millis(100);
  workload.set_num_write_threads(2);
  workload.set_write_batch_size(1);
  workload.Setup();
  workload.Start();

  // Run for either a prescribed number of writes, or 30 seconds,
  // whichever comes first. This prevents test timeouts on slower
  // build machines, TSAN builds, etc.
  Stopwatch sw;
  sw.start();
  const int kNumWrites = AllowSlowTests() ? 10000 : 1000;
  while (workload.rows_inserted() < kNumWrites &&
         (sw.elapsed().wall_seconds() < 30 ||
          // If no rows are inserted, run a little longer.
          (workload.rows_inserted() == 0 && sw.elapsed().wall_seconds() < 90))) {
    SleepFor(MonoDelta::FromMilliseconds(10));
    ASSERT_NO_FATALS(AssertNoTabletServersCrashed());
  }
  workload.StopAndJoin();
  LOG(INFO) << "rows_inserted=" << workload.rows_inserted();
  ASSERT_GT(workload.rows_inserted(), 0) << "No rows inserted";

  // Ensure that the replicas converge.
  // We don't know exactly how many rows got inserted, since the writer
  // probably saw many errors which left inserts in indeterminate state.
  // But, we should have at least as many as we got confirmation for.
  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(
      workload.table_name(), ClusterVerifier::AT_LEAST, workload.rows_inserted()));
  ASSERT_NO_FATALS(AssertNoTabletServersCrashed());
}

TEST_F(RaftConsensusITest, MultiThreadedInsertWithFailovers) {
  int kNumElections = FLAGS_num_replicas;
  vector<string> master_flags;

  if (AllowSlowTests()) {
    FLAGS_num_tablet_servers = 7;
    FLAGS_num_replicas = 7;
    kNumElections = 3 * FLAGS_num_replicas;
    master_flags.push_back("--replication_factor=7");
  }

  // Reset consensus rpc timeout to the default value or the election might fail often.
  FLAGS_consensus_rpc_timeout_ms = 1000;

  // Start a 7 node configuration cluster (since we can't bring leaders back we start with a
  // higher replica count so that we kill more leaders).

  ASSERT_NO_FATALS(BuildAndStart({}, master_flags));

  OverrideFlagForSlowTests(
      "client_inserts_per_thread",
      strings::Substitute("$0", (FLAGS_client_inserts_per_thread * 100)));
  OverrideFlagForSlowTests(
      "client_num_batches_per_thread",
      strings::Substitute("$0", (FLAGS_client_num_batches_per_thread * 100)));

  int num_threads = FLAGS_num_client_threads;
  int64_t total_num_rows = num_threads * FLAGS_client_inserts_per_thread;

  // We create 2 * (kNumReplicas - 1) latches so that we kill the same node at least
  // twice.
  vector<CountDownLatch*> latches;
  for (int i = 1; i < kNumElections; i++) {
    latches.push_back(new CountDownLatch((i * total_num_rows)  / kNumElections));
  }

  for (int i = 0; i < num_threads; i++) {
    scoped_refptr<yb::Thread> new_thread;
    CHECK_OK(yb::Thread::Create("test", strings::Substitute("ts-test$0", i),
                                  &RaftConsensusITest::InsertTestRowsRemoteThread,
                                  this, i * FLAGS_client_inserts_per_thread,
                                  FLAGS_client_inserts_per_thread,
                                  FLAGS_client_num_batches_per_thread,
                                  latches,
                                  &new_thread));
    threads_.push_back(new_thread);
  }

  for (CountDownLatch* latch : latches) {
    latch->Wait();
    StopOrKillLeaderAndElectNewOne();
  }

  for (scoped_refptr<yb::Thread> thr : threads_) {
    CHECK_OK(ThreadJoiner(thr.get()).Join());
  }

  ASSERT_ALL_REPLICAS_AGREE(FLAGS_client_inserts_per_thread * FLAGS_num_client_threads);
  STLDeleteElements(&latches);
}

// Test automatic leader election by killing leaders.
TEST_F(RaftConsensusITest, TestAutomaticLeaderElection) {
  vector<string> master_flags;
  if (AllowSlowTests()) {
    FLAGS_num_tablet_servers = 5;
    FLAGS_num_replicas = 5;
    master_flags.push_back("--replication_factor=5");
  }
  ASSERT_NO_FATALS(BuildAndStart({}, master_flags));

  TServerDetails* leader;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));

  unordered_set<TServerDetails*> killed_leaders;

  const int kNumLeadersToKill = FLAGS_num_replicas / 2;
  const int kFinalNumReplicas = FLAGS_num_replicas / 2 + 1;

  for (int leaders_killed = 0; leaders_killed < kFinalNumReplicas; leaders_killed++) {
    LOG(INFO) << Substitute("Writing data to leader of $0-node config ($1 alive)...",
                            FLAGS_num_replicas, FLAGS_num_replicas - leaders_killed);

    ASSERT_NO_FATALS(InsertTestRowsRemoteThread(
        leaders_killed * FLAGS_client_inserts_per_thread,
        FLAGS_client_inserts_per_thread,
        FLAGS_client_num_batches_per_thread,
        vector<CountDownLatch*>()));

    // At this point, the writes are flushed but the commit index may not be
    // propagated to all replicas. We kill the leader anyway.
    if (leaders_killed < kNumLeadersToKill) {
      LOG(INFO) << "Killing current leader " << leader->instance_id.permanent_uuid() << "...";
      cluster_->tablet_server_by_uuid(leader->uuid())->Shutdown();
      InsertOrDie(&killed_leaders, leader);

      LOG(INFO) << "Waiting for new guy to be elected leader.";
      ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));
    }
  }

  // Restart every node that was killed, and wait for the nodes to converge.
  for (TServerDetails* killed_node : killed_leaders) {
    CHECK_OK(cluster_->tablet_server_by_uuid(killed_node->uuid())->Restart());
  }
  // Verify the data on the remaining replicas.
  ASSERT_ALL_REPLICAS_AGREE(FLAGS_client_inserts_per_thread * kFinalNumReplicas);
}

// Single-replica leader election test.
TEST_F(RaftConsensusITest, TestAutomaticLeaderElectionOneReplica) {
  FLAGS_num_tablet_servers = 1;
  FLAGS_num_replicas = 1;
  ASSERT_NO_FATALS(BuildAndStart({}, {"--replication_factor=1"}));

  TServerDetails* leader;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));
}

void RaftConsensusITest::StubbornlyWriteSameRowThread(int replica_idx, const AtomicBool* finish) {
  vector<TServerDetails*> servers = TServerDetailsVector(tablet_servers_);

  CHECK_LT(replica_idx, servers.size());
  TServerDetails* ts = servers[replica_idx];

  // Manually construct an RPC to our target replica. We expect most of the calls
  // to fail either with an "already present" or an error because we are writing
  // to a follower. That's OK, though - what we care about for this test is
  // just that the operations Apply() in the same order everywhere (even though
  // in this case the result will just be an error).
  WriteRequestPB req;
  WriteResponsePB resp;
  RpcController rpc;
  req.set_tablet_id(tablet_id_);
  AddTestRowInsert(kTestRowKey, kTestRowIntVal, "hello world", &req);

  while (!finish->Load()) {
    resp.Clear();
    rpc.Reset();
    rpc.set_timeout(MonoDelta::FromSeconds(10));
    ignore_result(ts->tserver_proxy->Write(req, &resp, &rpc));
    VLOG(1) << "Response from server " << replica_idx << ": "
            << resp.ShortDebugString();
  }
}

// Regression test for KUDU-597, an issue where we could mis-order operations on
// a machine if the following sequence occurred:
//  1) Replica is a FOLLOWER
//  2) A client request hits the machine
//  3) It receives some operations from the current leader
//  4) It gets elected LEADER
// In this scenario, it would incorrectly sequence the client request's PREPARE phase
// before the operations received in step (3), even though the correct behavior would be
// to either reject them or sequence them after those operations, because the operation
// index is higher.
//
// The test works by setting up three replicas and manually hammering them with write
// requests targeting a single row. If the bug exists, then OperationOrderVerifier
// will trigger an assertion because the prepare order and the op indexes will become
// misaligned.
TEST_F(RaftConsensusITest, VerifyTransactionOrder) {
  FLAGS_num_tablet_servers = 3;
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  AtomicBool finish(false);
  for (int i = 0; i < FLAGS_num_tablet_servers; i++) {
    scoped_refptr<yb::Thread> new_thread;
    CHECK_OK(yb::Thread::Create("test", strings::Substitute("ts-test$0", i),
                                  &RaftConsensusITest::StubbornlyWriteSameRowThread,
                                  this, i, &finish, &new_thread));
    threads_.push_back(new_thread);
  }

  const int num_loops = AllowSlowTests() ? 10 : 1;
  for (int i = 0; i < num_loops; i++) {
    StopOrKillLeaderAndElectNewOne();
    SleepFor(MonoDelta::FromSeconds(1));
    ASSERT_OK(CheckTabletServersAreAlive(FLAGS_num_tablet_servers));
  }

  finish.Store(true);
  for (scoped_refptr<yb::Thread> thr : threads_) {
    CHECK_OK(ThreadJoiner(thr.get()).Join());
  }
}

void RaftConsensusITest::AddOp(const OpIdPB& id, ConsensusRequestPB* req) {
  ReplicateMsg* msg = req->add_ops();
  msg->mutable_id()->CopyFrom(id);
  msg->set_hybrid_time(clock_->Now().ToUint64());
  msg->set_op_type(consensus::WRITE_OP);
  WriteRequestPB* write_req = msg->mutable_write_request();
  write_req->set_tablet_id(tablet_id_);
  int32_t key = static_cast<int32_t>(id.index() * 10000 + id.term());
  string str_val = Substitute("term: $0 index: $1", id.term(), id.index());
  AddKVToPB(key, key + 10, str_val, write_req->mutable_write_batch());
}

// Regression test for KUDU-644:
// Triggers some complicated scenarios on the replica involving aborting and
// replacing transactions.
TEST_F(RaftConsensusITest, TestReplicaBehaviorViaRPC) {
  FLAGS_num_tablet_servers = 3;
  auto ts_flags = {
    "--enable_leader_failure_detection=false"s,
    "--max_wait_for_safe_time_ms=100"s,
    "--propagate_safe_time=false"s,
    // Disable bounded stale reads because the
    // update consensus requests generated by this
    // test don't update the hybrid timesetamp, so
    // follower reads will always be stale.
    "--max_stale_read_bound_time_ms=0"s
  };
  auto master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  // Kill all the servers but one.
  TServerDetails *replica_ts;
  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(3, tservers.size());

  // Elect server 2 as leader and wait for log index 1 to propagate to all servers.
  ASSERT_OK(StartElection(tservers[2], tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10), tablet_servers_, tablet_id_, 1));

  replica_ts = tservers[0];
  cluster_->tablet_server_by_uuid(tservers[1]->uuid())->Shutdown();
  cluster_->tablet_server_by_uuid(tservers[2]->uuid())->Shutdown();

  LOG(INFO) << "================================== Cluster setup complete.";

  // Check that the 'term' metric is correctly exposed.
  {
    int64_t term_from_metric = ASSERT_RESULT(
        cluster_->tablet_server_by_uuid(replica_ts->uuid())->GetInt64Metric(
            &METRIC_ENTITY_tablet,
            nullptr,
            &METRIC_raft_term,
            "value"));
    ASSERT_EQ(term_from_metric, 1);
  }

  ConsensusServiceProxy* c_proxy = CHECK_NOTNULL(replica_ts->consensus_proxy.get());

  ConsensusRequestPB req;
  ConsensusResponsePB resp;
  RpcController rpc;

  LOG(INFO) << "Send a simple request with no ops.";
  req.set_tablet_id(tablet_id_);
  req.set_dest_uuid(replica_ts->uuid());
  req.set_caller_uuid("fake_caller");
  req.set_caller_term(2);
  req.mutable_committed_op_id()->CopyFrom(MakeOpId(1, 1));
  req.mutable_preceding_id()->CopyFrom(MakeOpId(1, 1));

  ASSERT_OK(c_proxy->UpdateConsensus(req, &resp, &rpc));
  ASSERT_FALSE(resp.has_error()) << resp.DebugString();

  LOG(INFO) << "Send some operations, but don't advance the commit index. They should not commit.";
  AddOp(MakeOpId(2, 2), &req);
  AddOp(MakeOpId(2, 3), &req);
  AddOp(MakeOpId(2, 4), &req);
  rpc.Reset();
  ASSERT_OK(c_proxy->UpdateConsensus(req, &resp, &rpc));
  ASSERT_FALSE(resp.has_error()) << resp.DebugString();

  LOG(INFO) << "We shouldn't read anything yet, because the ops should be pending.";
  {
    vector<string> results;
    ASSERT_NO_FATALS(ScanReplica(replica_ts->tserver_proxy.get(), &results));
    ASSERT_EQ(0, results.size()) << results;
  }

  LOG(INFO) << "Send op 2.6, but set preceding OpId to 2.4. "
            << "This is an invalid request, and the replica should reject it.";
  req.mutable_preceding_id()->CopyFrom(MakeOpId(2, 4));
  req.clear_ops();
  AddOp(MakeOpId(2, 6), &req);
  rpc.Reset();
  ASSERT_OK(c_proxy->UpdateConsensus(req, &resp, &rpc));
  ASSERT_TRUE(resp.has_error()) << resp.DebugString();
  ASSERT_EQ(resp.error().status().message(),
            "New operation's index does not follow the previous op's index. "
            "Current: 2.6. Previous: 2.4");

  resp.Clear();
  req.clear_ops();
  LOG(INFO) << "Send ops 3.5 and 2.6, then commit up to index 6, the replica "
            << "should fail because of the out-of-order terms.";
  req.mutable_preceding_id()->CopyFrom(MakeOpId(2, 4));
  AddOp(MakeOpId(3, 5), &req);
  AddOp(MakeOpId(2, 6), &req);
  rpc.Reset();
  ASSERT_OK(c_proxy->UpdateConsensus(req, &resp, &rpc));
  ASSERT_TRUE(resp.has_error()) << resp.DebugString();
  ASSERT_EQ(resp.error().status().message(),
            "New operation's term is not >= than the previous op's term. "
            "Current: 2.6. Previous: 3.5");

  LOG(INFO) << "Regression test for KUDU-639";
  // If we send a valid request, but the
  // current commit index is higher than the data we're sending, we shouldn't
  // commit anything higher than the last op sent by the leader.
  //
  // To test, we re-send operation 2.3, with the correct preceding ID 2.2,
  // but we set the committed index to 2.4. This should only commit
  // 2.2 and 2.3.
  resp.Clear();
  req.clear_ops();
  req.mutable_preceding_id()->CopyFrom(MakeOpId(2, 2));
  AddOp(MakeOpId(2, 3), &req);
  req.mutable_committed_op_id()->CopyFrom(MakeOpId(2, 4));
  rpc.Reset();
  ASSERT_OK(c_proxy->UpdateConsensus(req, &resp, &rpc));
  ASSERT_FALSE(resp.has_error()) << resp.DebugString();
  LOG(INFO) << "Verify only 2.2 and 2.3 are committed.";
  {
    vector<string> results;
    ASSERT_NO_FATALS(WaitForRowCount(replica_ts->tserver_proxy.get(), 2, &results));
    ASSERT_STR_CONTAINS(results[0], "term: 2 index: 2");
    ASSERT_STR_CONTAINS(results[1], "term: 2 index: 3");
  }

  resp.Clear();
  req.clear_ops();
  LOG(INFO) << "Now send some more ops, and commit the earlier ones.";
  req.mutable_committed_op_id()->CopyFrom(MakeOpId(2, 4));
  req.mutable_preceding_id()->CopyFrom(MakeOpId(2, 4));
  AddOp(MakeOpId(2, 5), &req);
  AddOp(MakeOpId(2, 6), &req);
  rpc.Reset();
  ASSERT_OK(c_proxy->UpdateConsensus(req, &resp, &rpc));
  ASSERT_FALSE(resp.has_error()) << resp.DebugString();

  LOG(INFO) << "Verify they are committed.";
  {
    vector<string> results;
    ASSERT_NO_FATALS(WaitForRowCount(replica_ts->tserver_proxy.get(), 3, &results));
    ASSERT_STR_CONTAINS(results[0], "term: 2 index: 2");
    ASSERT_STR_CONTAINS(results[1], "term: 2 index: 3");
    ASSERT_STR_CONTAINS(results[2], "term: 2 index: 4");
  }

  resp.Clear();
  req.clear_ops();
  int leader_term = 2;
  const int kNumTerms = AllowSlowTests() ? 10000 : 100;
  while (leader_term < kNumTerms) {
    leader_term++;
    // Now pretend to be a new leader (term 3) and replace the earlier ops
    // without committing the new replacements.
    req.set_caller_term(leader_term);
    req.set_caller_uuid("new_leader");
    req.mutable_preceding_id()->CopyFrom(MakeOpId(2, 4));
    req.clear_ops();
    AddOp(MakeOpId(leader_term, 5), &req);
    AddOp(MakeOpId(leader_term, 6), &req);
    rpc.Reset();
    ASSERT_OK(c_proxy->UpdateConsensus(req, &resp, &rpc));
    ASSERT_FALSE(resp.has_error()) << "Req: " << req.ShortDebugString()
        << " Resp: " << resp.DebugString();
  }

  LOG(INFO) << "Send an empty request from the newest term which should commit "
            << "the earlier ops.";
  {
    req.mutable_preceding_id()->CopyFrom(MakeOpId(leader_term, 6));
    req.mutable_committed_op_id()->CopyFrom(MakeOpId(leader_term, 6));
    req.clear_ops();
    rpc.Reset();
    ASSERT_OK(c_proxy->UpdateConsensus(req, &resp, &rpc));
    ASSERT_FALSE(resp.has_error()) << resp.DebugString();
  }

  LOG(INFO) << "Verify the new rows are committed.";
  {
    vector<string> results;
    ASSERT_NO_FATALS(WaitForRowCount(replica_ts->tserver_proxy.get(), 5, &results));
    SCOPED_TRACE(results);
    ASSERT_STR_CONTAINS(results[3], Substitute("term: $0 index: 5", leader_term));
    ASSERT_STR_CONTAINS(results[4], Substitute("term: $0 index: 6", leader_term));
  }
}

TEST_F(RaftConsensusITest, TestLeaderStepDown) {
  FLAGS_num_tablet_servers = 3;

  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
  };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);

  // Start with no leader.
  Status s = GetReplicaStatusAndCheckIfLeader(tservers[0], tablet_id_, MonoDelta::FromSeconds(10));
  ASSERT_TRUE(s.IsIllegalState()) << "TS #0 should not be leader yet: " << s.ToString();

  // Become leader.
  ASSERT_OK(StartElection(tservers[0], tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitUntilLeader(tservers[0], tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WriteSimpleTestRow(
      tservers[0], tablet_id_, kTestRowKey, kTestRowIntVal, "foo", MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10), tablet_servers_, tablet_id_, 2));

  LOG(INFO) << "Stepping down leader " << tservers[0];
  // Step down and verify that leadership gracefully transitions to a new leader
  // despite failure detection being disabled.
  bool allow_graceful_leader_transfer = true;
  ASSERT_OK(LeaderStepDown(
      tservers[0], tablet_id_, nullptr, MonoDelta::FromSeconds(10),
      !allow_graceful_leader_transfer));
  TServerDetails* new_leader_ts = nullptr;
  ASSERT_OK(
      FindTabletLeader(tablet_servers_, tablet_id_, MonoDelta::FromSeconds(10), &new_leader_ts));
  ASSERT_TRUE(new_leader_ts != nullptr);
  LOG(INFO) << "Identified new leader " << new_leader_ts;

  // Verify that further leader stepdowns and writes against the original leader do not succeed.
  TabletServerErrorPB error;
  s = LeaderStepDown(
      tservers[0], tablet_id_, nullptr, MonoDelta::FromSeconds(10), !allow_graceful_leader_transfer,
      &error);
  ASSERT_TRUE(s.IsIllegalState()) << "TS #0 should not be leader anymore: " << s.ToString();
  ASSERT_EQ(TabletServerErrorPB::NOT_THE_LEADER, error.code()) << error.ShortDebugString();

  s = WriteSimpleTestRow(
      tservers[0], tablet_id_, kTestRowKey, kTestRowIntVal, "foo", MonoDelta::FromSeconds(10));
  ASSERT_TRUE(s.IsIllegalState()) << "TS #0 should not accept writes as follower: "
                                  << s.ToString();

  // Stepping down a leader with graceful transition disabled should
  // result in no leader being elected
  LOG(INFO) << "Stepping down leader " << new_leader_ts->uuid();
  allow_graceful_leader_transfer = false;
  ASSERT_OK(LeaderStepDown(
      new_leader_ts, tablet_id_, nullptr, MonoDelta::FromSeconds(10),
      !allow_graceful_leader_transfer));
  TServerDetails* final_leader_ts = nullptr;
  ASSERT_NOK(
      FindTabletLeader(tablet_servers_, tablet_id_, MonoDelta::FromSeconds(10), &final_leader_ts));
}

// Test for #350: sets the consensus RPC timeout to be long, and freezes both followers before
// asking the leader to step down. Prior to fixing #350, the step-down process would block
// until the pending requests timed out.
TEST_F(RaftConsensusITest, TestStepDownWithSlowFollower) {
  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false",
    // Bump up the RPC timeout, so that we can verify that the stepdown responds
    // quickly even when an outbound request is hung.
    "--consensus_rpc_timeout_ms=15000",
    // Make heartbeats more often so we can detect dead tservers faster.
    "--raft_heartbeat_interval_ms=10",
    // Set it high enough so that the election rpcs don't time out.
    "--leader_failure_max_missed_heartbeat_periods=100"
  };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false",
    "--tserver_unresponsive_timeout_ms=5000"s,
    "--use_create_table_leader_hint=false"s,
  };
  BuildAndStart(ts_flags, master_flags);

  vector<TServerDetails *> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_OK(StartElection(tservers[0], tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitUntilLeader(tservers[0], tablet_id_, MonoDelta::FromSeconds(10)));

  // Stop both followers.
  for (int i = 1; i < 3; i++) {
    ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[i]->uuid())->Pause());
  }

  // Wait until the paused tservers have stopped heartbeating.
  ASSERT_OK(WaitUntilNumberOfAliveTServersEqual(1, cluster_->master_proxy().get(),
                                                MonoDelta::FromSeconds(20)));

  // Step down should respond quickly despite the hung requests.
  ASSERT_OK(LeaderStepDown(tservers[0], tablet_id_, nullptr, MonoDelta::FromSeconds(3)));
}

void RaftConsensusITest::AssertMajorityRequiredForElectionsAndWrites(
    const TabletServerMapUnowned& tablet_servers, const string& leader_uuid) {

  TServerDetails* initial_leader = FindOrDie(tablet_servers, leader_uuid);

  // Calculate number of servers to leave unpaused (minority).
  // This math is a little unintuitive but works for cluster sizes including 2 and 1.
  // Note: We assume all of these TSes are voters.
  int config_size = tablet_servers.size();
  int minority_to_retain = MajoritySize(config_size) - 1;

  // Only perform this part of the test if we have some servers to pause, else
  // the failure assertions will throw.
  if (config_size > 1) {
    // Pause enough replicas to prevent a majority.
    int num_to_pause = config_size - minority_to_retain;
    LOG(INFO) << "Pausing " << num_to_pause << " tablet servers in config of size " << config_size;
    vector<string> paused_uuids;
    for (const auto& entry : tablet_servers) {
      if (paused_uuids.size() == num_to_pause) {
        continue;
      }
      const string& replica_uuid = entry.first;
      if (replica_uuid == leader_uuid) {
        // Always leave this one alone.
        continue;
      }
      ExternalTabletServer* replica_ts = cluster_->tablet_server_by_uuid(replica_uuid);
      ASSERT_OK(replica_ts->Pause());
      paused_uuids.push_back(replica_uuid);
    }

    // Ensure writes timeout while only a minority is alive.
    Status s = WriteSimpleTestRow(initial_leader, tablet_id_,
                                  kTestRowKey, kTestRowIntVal, "foo",
                                  MonoDelta::FromMilliseconds(100));
    ASSERT_TRUE(s.IsTimedOut()) << s.ToString();

    // Step down.
    ASSERT_OK(LeaderStepDown(initial_leader, tablet_id_, nullptr, MonoDelta::FromSeconds(10)));

    // Assert that elections time out without a live majority.
    // We specify a very short timeout here to keep the tests fast.
    ASSERT_OK(StartElection(initial_leader, tablet_id_, MonoDelta::FromSeconds(10)));
    s = WaitUntilLeader(initial_leader, tablet_id_, MonoDelta::FromMilliseconds(100));
    ASSERT_TRUE(s.IsTimedOut()) << s.ToString();
    LOG(INFO) << "Expected timeout encountered on election with weakened config: " << s.ToString();

    // Resume the paused servers.
    LOG(INFO) << "Resuming " << num_to_pause << " tablet servers in config of size " << config_size;
    for (const string& replica_uuid : paused_uuids) {
      ExternalTabletServer* replica_ts = cluster_->tablet_server_by_uuid(replica_uuid);
      ASSERT_OK(replica_ts->Resume());
    }
  }

  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(20), tablet_servers, tablet_id_, 1));

  // Now an election should succeed.
  ASSERT_OK(StartElection(initial_leader, tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitUntilLeader(initial_leader, tablet_id_, MonoDelta::FromSeconds(10)));
  LOG(INFO) << "Successful election with full config of size " << config_size;

  // And a write should also succeed.
  ASSERT_OK(WriteSimpleTestRow(initial_leader, tablet_id_,
                               kTestRowKey, kTestRowIntVal, Substitute("qsz=$0", config_size),
                               MonoDelta::FromSeconds(10)));
}

// Return the replicas of the specified 'tablet_id', as seen by the Master.
Status RaftConsensusITest::GetTabletLocations(const string& tablet_id, const MonoDelta& timeout,
                                              master::TabletLocationsPB* tablet_locations) {
  RpcController rpc;
  rpc.set_timeout(timeout);
  GetTabletLocationsRequestPB req;
  *req.add_tablet_ids() = tablet_id;
  GetTabletLocationsResponsePB resp;
  RETURN_NOT_OK(cluster_->master_proxy()->GetTabletLocations(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  if (resp.errors_size() > 0) {
    CHECK_EQ(1, resp.errors_size()) << resp.ShortDebugString();
    CHECK_EQ(tablet_id, resp.errors(0).tablet_id()) << resp.ShortDebugString();
    return StatusFromPB(resp.errors(0).status());
  }
  CHECK_EQ(1, resp.tablet_locations_size()) << resp.ShortDebugString();
  *tablet_locations = resp.tablet_locations(0);
  return Status::OK();
}

void RaftConsensusITest::WaitForReplicasReportedToMaster(
    int num_replicas, const string& tablet_id,
    const MonoDelta& timeout,
    WaitForLeader wait_for_leader,
    bool* has_leader,
    master::TabletLocationsPB* tablet_locations) {
  MonoTime deadline(MonoTime::Now());
  deadline.AddDelta(timeout);
  while (true) {
    ASSERT_OK(GetTabletLocations(tablet_id, timeout, tablet_locations));
    *has_leader = false;
    if (tablet_locations->replicas_size() == num_replicas) {
      for (const master::TabletLocationsPB_ReplicaPB& replica :
                    tablet_locations->replicas()) {
        if (replica.role() == RaftPeerPB::LEADER) {
          *has_leader = true;
        }
      }
      if (wait_for_leader == NO_WAIT_FOR_LEADER ||
          (wait_for_leader == WAIT_FOR_LEADER && *has_leader)) {
        break;
      }
    }
    if (deadline.ComesBefore(MonoTime::Now())) break;
    SleepFor(MonoDelta::FromMilliseconds(20));
  }
  ASSERT_EQ(num_replicas, tablet_locations->replicas_size()) << tablet_locations->DebugString();
  if (wait_for_leader == WAIT_FOR_LEADER) {
    ASSERT_TRUE(*has_leader) << tablet_locations->DebugString();
  }
}

// Basic tests of adding and removing servers from a configuration.
TEST_F(RaftConsensusITest, TestAddRemoveVoter) {
  TestAddRemoveServer(RaftPeerPB::PRE_VOTER);
}

TEST_F(RaftConsensusITest, TestAddRemoveObserver) {
  TestAddRemoveServer(RaftPeerPB::PRE_OBSERVER);
}

// Regression test for KUDU-1169: a crash when a Config Change operation is replaced
// by a later leader.
TEST_F(RaftConsensusITest, TestReplaceChangeConfigOperation) {
  FLAGS_num_tablet_servers = 3;
  vector<string> ts_flags = { "--enable_leader_failure_detection=false"s };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  TServerDetails* leader_tserver = tservers[0];
  LOG(INFO) << "Elect server 0 (" << leader_tserver->uuid()
            << ") as leader and wait for log index 1 to propagate to all servers.";

  auto original_followers = CreateTabletServerMapUnowned(tablet_servers_);
  ASSERT_EQ(1, original_followers.erase(leader_tserver->uuid()));

  const MonoDelta timeout = MonoDelta::FromSeconds(10);
  ASSERT_OK(StartElection(leader_tserver, tablet_id_, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, tablet_servers_, tablet_id_, 1));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(1, leader_tserver, tablet_id_, timeout));

  LOG(INFO) << "Shut down servers 1 and 2, so that server 0 can't replicate anything.";
  cluster_->tablet_server_by_uuid(tservers[1]->uuid())->Shutdown();
  cluster_->tablet_server_by_uuid(tservers[2]->uuid())->Shutdown();

  LOG(INFO) << "Now try to replicate a ChangeConfig operation. This should get stuck and time out";
  LOG(INFO) << "because the server can't replicate any operations.";
  TabletServerErrorPB::Code error_code;
  Status s = RemoveServer(leader_tserver, tablet_id_, tservers[1],
                          -1, MonoDelta::FromSeconds(1),
                          &error_code, false /* retry */);
  ASSERT_TRUE(s.IsTimedOut());

  LOG(INFO) << "Pause the leader, and restart the other servers.";
  ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[0]->uuid())->Pause());
  ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[1]->uuid())->Restart());
  ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[2]->uuid())->Restart());

  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10), original_followers, tablet_id_, 1));

  LOG(INFO) << "Elect one of the other servers: " << tservers[1]->uuid();
  ASSERT_OK(StartElection(tservers[1], tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10), original_followers, tablet_id_, 2));

  LOG(INFO) << "Resume the original leader. Its change-config operation will now be aborted "
               "since it was never replicated to the majority, and the new leader will have "
               "replaced the operation.";
  ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[0]->uuid())->Resume());

  LOG(INFO) << "Insert some data and verify that it propagates to all servers.";
  ASSERT_NO_FATALS(InsertTestRowsRemoteThread(0, 10, 1, vector<CountDownLatch*>()));
  ASSERT_ALL_REPLICAS_AGREE(10);
}

// Test the atomic CAS arguments to ChangeConfig() add server and remove server.
TEST_F(RaftConsensusITest, TestAtomicAddRemoveServer) {
  FLAGS_num_tablet_servers = 3;
  vector<string> ts_flags = { "--enable_leader_failure_detection=false" };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  // Elect server 0 as leader and wait for log index 1 to propagate to all servers.
  TServerDetails* leader_tserver = tservers[0];
  const MonoDelta timeout = MonoDelta::FromSeconds(10);
  ASSERT_OK(StartElection(leader_tserver, tablet_id_, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, tablet_servers_, tablet_id_, 1));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(1, leader_tserver, tablet_id_, timeout));
  int64_t cur_log_index = 1;

  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);

  TServerDetails* follower_ts = tservers[2];

  // Initial committed config should have opid_index == -1.
  // Server should reject request to change config from opid other than this.
  int64_t invalid_committed_opid_index = 7;
  TabletServerErrorPB::Code error_code;
  Status s = RemoveServer(leader_tserver, tablet_id_, follower_ts,
                          invalid_committed_opid_index, MonoDelta::FromSeconds(10),
                          &error_code, false /* retry */);
  ASSERT_EQ(TabletServerErrorPB::CAS_FAILED, error_code);
  ASSERT_STR_CONTAINS(s.ToString(), "of 7 but the committed config has opid_index of -1");

  // Specifying the correct committed opid index should work.
  int64_t committed_opid_index = -1;
  ASSERT_OK(RemoveServer(leader_tserver, tablet_id_, follower_ts,
                         committed_opid_index, MonoDelta::FromSeconds(10)));

  ASSERT_EQ(1, active_tablet_servers.erase(follower_ts->uuid()));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10),
                                  active_tablet_servers, tablet_id_, ++cur_log_index));

  // Now, add the server back. Again, specifying something other than the
  // latest committed_opid_index should fail.
  invalid_committed_opid_index = -1;  // The old one is no longer valid.
  s = AddServer(leader_tserver, tablet_id_, follower_ts, RaftPeerPB::VOTER,
                invalid_committed_opid_index, MonoDelta::FromSeconds(10),
                &error_code, false /* retry */);
  ASSERT_EQ(TabletServerErrorPB::CAS_FAILED, error_code);
  ASSERT_STR_CONTAINS(s.ToString(), "of -1 but the committed config has opid_index of 2");

  // Specifying the correct committed opid index should work.
  // The previous config change op is the latest entry in the log.
  committed_opid_index = cur_log_index;
  ASSERT_OK(AddServer(leader_tserver, tablet_id_, follower_ts, RaftPeerPB::PRE_VOTER,
                      committed_opid_index, MonoDelta::FromSeconds(10)));

  InsertOrDie(&active_tablet_servers, follower_ts->uuid(), follower_ts);
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10),
                                  active_tablet_servers, tablet_id_, ++cur_log_index));
}

// Ensure that we can elect a server that is in the "pending" configuration.  This is required by
// the Raft protocol. See Diego Ongaro's PhD thesis, section 4.1, where it states that "it is the
// caller's configuration that is used in reaching consensus, both for voting and for log
// replication".
//
// This test also tests the case where a node comes back from the dead to a leader that was not in
// its configuration when it died. That should also work, i.e.  the revived node should accept
// writes from the new leader.
TEST_F(RaftConsensusITest, TestElectPendingVoter) {
  // Test plan:
  //  1. Disable failure detection to avoid non-deterministic behavior.
  //  2. Start with a configuration size of 5, all servers synced.
  //  3. Remove one server from the configuration, wait until committed.
  //  4. Pause the 3 remaining non-leaders (SIGSTOP).
  //  5. Run a config change to add back the previously-removed server.
  //     Ensure that, while the op cannot be committed yet due to lack of a
  //     majority in the new config (only 2 out of 5 servers are alive), the op
  //     has been replicated to both the local leader and the new member.
  //  6. Force the existing leader to step down.
  //  7. Resume one of the paused nodes so that a majority (of the 5-node
  //     configuration, but not the original 4-node configuration) will be available.
  //  8. Start a leader election on the new (pending) node. It should win.
  //  9. Unpause the two remaining stopped nodes.
  // 10. Wait for all nodes to sync to the new leader's log.
  FLAGS_num_tablet_servers = 5;
  FLAGS_num_replicas = 5;
  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    "--TEST_inject_latency_before_change_role_secs=10"s,
  };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--replication_factor=5"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  // Elect server 0 as leader and wait for log index 1 to propagate to all servers.
  TServerDetails* initial_leader = tservers[0];
  const MonoDelta timeout = MonoDelta::FromSeconds(10);
  ASSERT_OK(StartElection(initial_leader, tablet_id_, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, tablet_servers_, tablet_id_, 1));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(1, initial_leader, tablet_id_, timeout));

  // The server we will remove and then bring back.
  TServerDetails* final_leader = tservers[4];

  // Kill the master, so we can change the config without interference.
  cluster_->master()->Shutdown();

  // Now remove server 4 from the configuration.
  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);
  LOG(INFO) << "Removing tserver with uuid " << final_leader->uuid();
  ASSERT_OK(RemoveServer(initial_leader, tablet_id_, final_leader, boost::none,
                         MonoDelta::FromSeconds(10)));
  ASSERT_EQ(1, active_tablet_servers.erase(final_leader->uuid()));
  int64_t cur_log_index = 2;
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10),
                                  active_tablet_servers, tablet_id_, cur_log_index));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(cur_log_index, initial_leader, tablet_id_, timeout));

  ASSERT_OK(DeleteTablet(final_leader, tablet_id_, tablet::TABLET_DATA_TOMBSTONED, boost::none,
                         MonoDelta::FromSeconds(30)));

  // Now add server 4 back as a learner to the peers.
  LOG(INFO) << "Adding back Peer " << final_leader->uuid() << " and expecting timeout...";
  ASSERT_OK(AddServer(initial_leader, tablet_id_, final_leader, RaftPeerPB::PRE_VOTER, boost::none,
                       MonoDelta::FromSeconds(10)));

  // Pause tablet servers 1 through 3, so they won't see the operation to change server 4 from
  // learner to voter which will happen automatically once remote bootstrap for server 4 is
  // completed.
  LOG(INFO) << "Pausing 3 replicas...";
  for (int i = 1; i <= 3; i++) {
    ExternalTabletServer* replica_ts = cluster_->tablet_server_by_uuid(tservers[i]->uuid());
    ASSERT_OK(replica_ts->Pause());
  }

  // Reset to the unpaused servers.
  active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);
  for (int i = 1; i <= 3; i++) {
    ASSERT_EQ(1, active_tablet_servers.erase(tservers[i]->uuid()));
  }

  // Adding a server will cause two calls to ChangeConfig. One to add the server as a learner,
  // and another one to change its role to voter.
  ++cur_log_index;

  // Only wait for TS 0 and 4 to agree that the new change config op (CHANGE_ROLE for server 4,
  // which will be automatically sent by the leader once remote bootstrap has completed) has been
  // replicated.
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(60),
                                  active_tablet_servers, tablet_id_, ++cur_log_index));

  // Now that TS 4 is electable (and pending), have TS 0 step down.
  LOG(INFO) << "Forcing Peer " << initial_leader->uuid() << " to step down...";
  Status status = LeaderStepDown(initial_leader, tablet_id_, nullptr, MonoDelta::FromSeconds(10));
  // We allow illegal state for now as the leader step down does not succeed in this case when it
  // has a pending config. Peer 4 will get elected though as it has new term and others
  // in quorum to vote.
  if (!status.IsIllegalState()) {
    ASSERT_OK(status);
  } else {
    LOG(INFO) << "Resuming Peer " << tservers[2]->uuid() << " as leader did not step down...";
    ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[2]->uuid())->Resume());
    InsertOrDie(&active_tablet_servers, tservers[2]->uuid(), tservers[2]);
  }
  // Resume TS 1 so we have a majority of 3 to elect a new leader.
  LOG(INFO) << "Resuming Peer " << tservers[1]->uuid() << " ...";
  ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[1]->uuid())->Resume());
  InsertOrDie(&active_tablet_servers, tservers[1]->uuid(), tservers[1]);

  // Now try to get TS 4 elected. It should succeed and push a NO_OP.
  LOG(INFO) << "Trying to elect Peer " << tservers[4]->uuid() << " ...";
  ASSERT_OK(StartElection(final_leader, tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10),
                                  active_tablet_servers, tablet_id_, ++cur_log_index));

  // Resume the remaining paused nodes.
  LOG(INFO) << "Resuming remaining nodes...";
  ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[2]->uuid())->Resume());
  ASSERT_OK(cluster_->tablet_server_by_uuid(tservers[3]->uuid())->Resume());
  active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);

  // Wait until the leader is sure that the old leader's lease is over.
  ASSERT_OK(WaitUntilLeader(final_leader, tablet_id_,
      MonoDelta::FromMilliseconds(GetAtomicFlag(&FLAGS_leader_lease_duration_ms))));

  // Do one last operation on the new leader: an insert.
  ASSERT_OK(WriteSimpleTestRow(final_leader, tablet_id_,
                               kTestRowKey, kTestRowIntVal, "Ob-La-Di, Ob-La-Da",
                               MonoDelta::FromSeconds(10)));

  // Wait for all servers to replicate everything up through the last write op.
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10),
                                  active_tablet_servers, tablet_id_, ++cur_log_index));
}

// Writes test rows in ascending order to a single tablet server.
// Essentially a poor-man's version of TestWorkload that only operates on a
// single tablet. Does not batch, does not tolerate timeouts, and does not
// interact with the Master. 'rows_inserted' is used to determine row id and is
// incremented prior to each successful insert. Since a write failure results in
// a crash, as long as there is no crash then 'rows_inserted' will have a
// correct count at the end of the run.
// Crashes on any failure, so 'write_timeout' should be high.
void DoWriteTestRows(const TServerDetails* leader_tserver,
                     const string& tablet_id,
                     const MonoDelta& write_timeout,
                     std::atomic<int32_t>* rows_inserted,
                     std::atomic<int32_t>* row_key,
                     const std::atomic<bool>* finish) {
  while (!finish->load()) {
    int cur_row_key = ++*row_key;
    Status write_status = WriteSimpleTestRow(
        leader_tserver, tablet_id, cur_row_key, cur_row_key,
        Substitute("key=$0", cur_row_key), write_timeout);
    if (!write_status.IsLeaderHasNoLease() &&
        !write_status.IsLeaderNotReadyToServe()) {
      // Temporary failures to write because of not having a valid leader lease are OK. We don't
      // increment the number of rows inserted in that case.
      CHECK_OK(write_status);
      ++*rows_inserted;
    }
  }
}

// Test that config change works while running a workload.
TEST_F(RaftConsensusITest, TestConfigChangeUnderLoad) {
  FLAGS_num_tablet_servers = 3;
  vector<string> ts_flags = { "--enable_leader_failure_detection=false" };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  // Elect server 0 as leader and wait for log index 1 to propagate to all servers.
  TServerDetails* leader_tserver = tservers[0];
  MonoDelta timeout = MonoDelta::FromSeconds(10);
  ASSERT_OK(StartElection(leader_tserver, tablet_id_, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, tablet_servers_, tablet_id_, 1));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(1, leader_tserver, tablet_id_, timeout));

  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);

  // Start a write workload.
  LOG(INFO) << "Starting write workload...";
  std::atomic<int32_t> rows_inserted(0);
  std::atomic<int32_t> row_key(0);
  {
    std::atomic<bool> finish(false);
    vector<scoped_refptr<Thread> > threads;
    auto se = ScopeExit([&threads, &finish] {
      LOG(INFO) << "Joining writer threads...";
      finish = true;
      for (const scoped_refptr<Thread> &thread : threads) {
        ASSERT_OK(ThreadJoiner(thread.get()).Join());
      }
    });

    int num_threads = FLAGS_num_client_threads;
    for (int i = 0; i < num_threads; i++) {
      scoped_refptr<Thread> thread;
      ASSERT_OK(Thread::Create(CURRENT_TEST_NAME(), Substitute("row-writer-$0", i),
          &DoWriteTestRows,
          leader_tserver,
          tablet_id_,
          MonoDelta::FromSeconds(10),
          &rows_inserted,
          &row_key,
          &finish,
          &thread));
      threads.push_back(thread);
    }

    LOG(INFO) << "Removing servers...";
    // Go from 3 tablet servers down to 1 in the configuration.
    vector<int> remove_list = {2, 1};
    for (int to_remove_idx : remove_list) {
      int num_servers = active_tablet_servers.size();
      LOG(INFO) << "Remove: Going from " << num_servers << " to " << num_servers - 1 << " replicas";

      TServerDetails *tserver_to_remove = tservers[to_remove_idx];
      LOG(INFO) << "Removing tserver with uuid " << tserver_to_remove->uuid();
      ASSERT_OK(RemoveServer(leader_tserver, tablet_id_, tserver_to_remove, boost::none,
          MonoDelta::FromSeconds(10)));
      ASSERT_EQ(1, active_tablet_servers.erase(tserver_to_remove->uuid()));
      ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(),
          leader_tserver, tablet_id_,
          MonoDelta::FromSeconds(10)));
    }

    LOG(INFO) << "Adding servers...";
    // Add the tablet servers back, in reverse order, going from 1 to 3 servers in the
    // configuration.
    vector<int> add_list = {1, 2};
    for (int to_add_idx : add_list) {
      int num_servers = active_tablet_servers.size();
      LOG(INFO) << "Add: Going from " << num_servers << " to " << num_servers + 1 << " replicas";

      TServerDetails *tserver_to_add = tservers[to_add_idx];
      LOG(INFO) << "Adding tserver with uuid " << tserver_to_add->uuid();
      ASSERT_OK(AddServer(leader_tserver, tablet_id_, tserver_to_add, RaftPeerPB::PRE_VOTER,
          boost::none, MonoDelta::FromSeconds(10)));
      InsertOrDie(&active_tablet_servers, tserver_to_add->uuid(), tserver_to_add);
      ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(),
          leader_tserver, tablet_id_,
          MonoDelta::FromSeconds(10)));
    }
  }

  LOG(INFO) << "Waiting for replicas to agree... (rows_inserted=" << rows_inserted
            << ", unique row keys used: " << row_key << ")";
  // Wait for all servers to replicate everything up through the last write op.
  // Since we don't batch, there should be at least # rows inserted log entries,
  // plus the initial leader's no-op, plus 2 for the removed servers, plus 2 for
  // the added servers for a total of 5.
  int min_log_index = rows_inserted + 5;
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(10),
                                  active_tablet_servers, tablet_id_,
                                  min_log_index));

  LOG(INFO) << "Number of rows inserted: " << rows_inserted.load();
  ASSERT_ALL_REPLICAS_AGREE(rows_inserted.load());
}

TEST_F(RaftConsensusITest, TestMasterNotifiedOnConfigChange) {
  MonoDelta timeout = MonoDelta::FromSeconds(30);
  FLAGS_num_tablet_servers = 3;
  FLAGS_num_replicas = 2;
  vector<string> ts_flags;
  vector<string> master_flags;
  master_flags.push_back("--replication_factor=2");
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  LOG(INFO) << "Finding tablet leader and waiting for things to start...";
  string tablet_id = tablet_replicas_.begin()->first;

  // Determine the list of tablet servers currently in the config.
  TabletServerMapUnowned active_tablet_servers;
  for (itest::TabletReplicaMap::const_iterator iter = tablet_replicas_.find(tablet_id);
       iter != tablet_replicas_.end(); ++iter) {
    InsertOrDie(&active_tablet_servers, iter->second->uuid(), iter->second);
  }

  // Determine the server to add to the config.
  string uuid_to_add;
  for (const TabletServerMap::value_type& entry : tablet_servers_) {
    if (!ContainsKey(active_tablet_servers, entry.second->uuid())) {
      uuid_to_add = entry.second->uuid();
    }
  }
  ASSERT_FALSE(uuid_to_add.empty());

  // Get a baseline config reported to the master.
  LOG(INFO) << "Waiting for Master to see the current replicas...";
  master::TabletLocationsPB tablet_locations;
  bool has_leader;
  ASSERT_NO_FATALS(WaitForReplicasReportedToMaster(2, tablet_id, timeout, WAIT_FOR_LEADER,
                                            &has_leader, &tablet_locations));
  LOG(INFO) << "Tablet locations:\n" << tablet_locations.DebugString();

  // Wait for initial NO_OP to be committed by the leader.
  TServerDetails* leader_ts;
  ASSERT_OK(FindTabletLeader(tablet_servers_, tablet_id, timeout, &leader_ts));
  int64_t expected_index = 0;
  ASSERT_OK(WaitForServersToAgree(timeout, active_tablet_servers, tablet_id, 1, &expected_index));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(expected_index, leader_ts, tablet_id, timeout));
  expected_index += 2; // Adding a new peer generates two ChangeConfig requests.

  // Change the config.
  TServerDetails* tserver_to_add = tablet_servers_[uuid_to_add].get();
  LOG(INFO) << "Adding tserver with uuid " << tserver_to_add->uuid();
  ASSERT_OK(AddServer(leader_ts, tablet_id_, tserver_to_add, RaftPeerPB::PRE_VOTER, boost::none,
                      timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, tablet_servers_, tablet_id_, expected_index));
  ++expected_index;

  // Wait for the master to be notified of the config change.
  // It should continue to have the same leader, even without waiting.
  LOG(INFO) << "Waiting for Master to see config change...";
  ASSERT_NO_FATALS(WaitForReplicasReportedToMaster(3, tablet_id, timeout, NO_WAIT_FOR_LEADER,
                                            &has_leader, &tablet_locations));
  ASSERT_TRUE(has_leader) << tablet_locations.DebugString();
  LOG(INFO) << "Tablet locations:\n" << tablet_locations.DebugString();

  // Change the config again.
  LOG(INFO) << "Removing tserver with uuid " << tserver_to_add->uuid();
  ASSERT_OK(RemoveServer(leader_ts, tablet_id_, tserver_to_add, boost::none, timeout));
  active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);
  ASSERT_EQ(1, active_tablet_servers.erase(tserver_to_add->uuid()));
  ASSERT_OK(WaitForServersToAgree(timeout, active_tablet_servers, tablet_id_, expected_index));

  // Wait for the master to be notified of the removal.
  LOG(INFO) << "Waiting for Master to see config change...";
  ASSERT_NO_FATALS(WaitForReplicasReportedToMaster(2, tablet_id, timeout, NO_WAIT_FOR_LEADER,
                                            &has_leader, &tablet_locations));
  ASSERT_TRUE(has_leader) << tablet_locations.DebugString();
  LOG(INFO) << "Tablet locations:\n" << tablet_locations.DebugString();
}

// Test that we can create (vivify) a new tablet via remote bootstrap.
TEST_F(RaftConsensusITest, TestAutoCreateReplica) {
  FLAGS_num_tablet_servers = 3;
  FLAGS_num_replicas = 2;
  std::vector<std::string> ts_flags = {
    "--enable_leader_failure_detection=false",
    "--log_cache_size_limit_mb=1",
    "--log_segment_size_mb=1",
    "--log_async_preallocate_segments=false",
    "--maintenance_manager_polling_interval_ms=300",
    Format("--db_write_buffer_size=$0", 256_KB),
    "--remote_bootstrap_begin_session_timeout_ms=15000"
  };
  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--replication_factor=2"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  // 50K is enough to cause flushes & log rolls.
  int num_rows_to_write = 50000;
  if (AllowSlowTests()) {
    num_rows_to_write = 150000;
  }
  num_rows_to_write = NonTsanVsTsan(num_rows_to_write, num_rows_to_write / 4);

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  itest::TabletServerMapUnowned active_tablet_servers;
  auto iter = tablet_replicas_.find(tablet_id_);
  TServerDetails* leader = iter->second;
  TServerDetails* follower = (++iter)->second;
  InsertOrDie(&active_tablet_servers, leader->uuid(), leader);
  InsertOrDie(&active_tablet_servers, follower->uuid(), follower);

  TServerDetails* new_node = nullptr;
  for (TServerDetails* ts : tservers) {
    if (!ContainsKey(active_tablet_servers, ts->uuid())) {
      new_node = ts;
      break;
    }
  }
  ASSERT_TRUE(new_node != nullptr);

  // Elect the leader (still only a consensus config size of 2).
  ASSERT_OK(StartElection(leader, tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(30), active_tablet_servers,
                                  tablet_id_, 1));

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_num_write_threads(10);
  workload.set_write_batch_size(100);
  workload.Setup();

  LOG(INFO) << "Starting write workload...";
  workload.Start();

  while (true) {
    int rows_inserted = workload.rows_inserted();
    if (rows_inserted >= num_rows_to_write) {
      break;
    }
    LOG(INFO) << "Only inserted " << rows_inserted << " rows so far, sleeping for 100ms";
    SleepFor(MonoDelta::FromMilliseconds(100));
  }

  LOG(INFO) << "Adding tserver with uuid " << new_node->uuid() << " as VOTER...";
  ASSERT_OK(AddServer(leader, tablet_id_, new_node, RaftPeerPB::PRE_VOTER, boost::none,
                      MonoDelta::FromSeconds(10)));
  InsertOrDie(&active_tablet_servers, new_node->uuid(), new_node);
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(
      active_tablet_servers.size(), leader, tablet_id_,
      MonoDelta::FromSeconds(NonTsanVsTsan(20, 60))));

  workload.StopAndJoin();
  int num_batches = workload.batches_completed();

  LOG(INFO) << "Waiting for replicas to agree...";
  // Wait for all servers to replicate everything up through the last write op.
  // Since we don't batch, there should be at least # rows inserted log entries,
  // plus the initial leader's no-op, plus 1 for
  // the added replica for a total == #rows + 2.
  int min_log_index = num_batches + 2;
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(120),
                                  active_tablet_servers, tablet_id_,
                                  min_log_index));

  int rows_inserted = workload.rows_inserted();
  LOG(INFO) << "Number of rows inserted: " << rows_inserted;
  ASSERT_ALL_REPLICAS_AGREE(rows_inserted);
}

TEST_F(RaftConsensusITest, TestMemoryRemainsConstantDespiteTwoDeadFollowers) {
  const int64_t kMinRejections = 100;
  const MonoDelta kMaxWaitTime = MonoDelta::FromSeconds(60);

  // Start the cluster with a low per-tablet transaction memory limit, so that
  // the test can complete faster.
  std::vector<std:: string> flags = {
      "--tablet_operation_memory_limit_mb=2"s
  };

  ASSERT_NO_FATALS(BuildAndStart(flags));

  // Kill both followers.
  TServerDetails* details;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &details));
  int num_shutdown = 0;
  int leader_ts_idx = -1;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ExternalTabletServer* ts = cluster_->tablet_server(i);
    if (ts->instance_id().permanent_uuid() != details->uuid()) {
      ts->Shutdown();
      num_shutdown++;
    } else {
      leader_ts_idx = i;
    }
  }
  ASSERT_EQ(2, num_shutdown);
  ASSERT_NE(-1, leader_ts_idx);

  // Because the majority of the cluster is dead and because of this workload's
  // timeout behavior, more and more wedged transactions will accumulate in the
  // leader. To prevent memory usage from skyrocketing, the leader will
  // eventually reject new transactions. That's what we're testing for here.
  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_timeout_allowed(true);
  workload.set_write_timeout_millis(50);
  workload.Setup();
  workload.Start();

  // Run until the former leader has rejected several transactions.
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(kMaxWaitTime);
  while (true) {
    int64_t num_rejections = ASSERT_RESULT(cluster_->tablet_server(leader_ts_idx)->GetInt64Metric(
        &METRIC_ENTITY_tablet,
        nullptr,
        &METRIC_not_leader_rejections,
        "value"));
    if (num_rejections >= kMinRejections) {
      break;
    } else if (deadline.ComesBefore(MonoTime::Now())) {
      FAIL() << "Ran for " << kMaxWaitTime.ToString() << ", deadline expired";
    }
    SleepFor(MonoDelta::FromMilliseconds(200));
  }
}

static void EnableLogLatency(server::GenericServiceProxy* proxy) {
  typedef unordered_map<string, string> FlagMap;
  FlagMap flags;
  InsertOrDie(&flags, "log_inject_latency", "true");
  InsertOrDie(&flags, "log_inject_latency_ms_mean", "1000");
  for (const FlagMap::value_type& e : flags) {
    SetFlagRequestPB req;
    SetFlagResponsePB resp;
    RpcController rpc;
    req.set_flag(e.first);
    req.set_value(e.second);
    ASSERT_OK(proxy->SetFlag(req, &resp, &rpc));
  }
}

// Check if hinted replica becomes leader.
TEST_F(RaftConsensusITest, HintedLeader) {
  vector<string> master_flags = {
      "--use_create_table_leader_hint=true"s,
      "--TEST_create_table_leader_hint_min_lexicographic=true"s,
  };

  ASSERT_NO_FATALS(BuildAndStart({}, master_flags));

  std::string init_leader_hint = "";
  for (const TabletServerMap::value_type& entry : tablet_servers_) {
    if (init_leader_hint == "" || init_leader_hint > entry.first) {
      init_leader_hint = entry.first;
    }
  }

  LOG(INFO) << "hinted replica = " << init_leader_hint;

  // Check that hinted replica became leader.
  TServerDetails* leader;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));
  ASSERT_EQ(init_leader_hint, leader->uuid());

  // Insert 3MB worth of data.
  const int kNumWrites = 1000;
  ASSERT_NO_FATALS(WriteOpsToLeader(kNumWrites, 3_KB));
  ASSERT_OK(WaitForServersToAgree(30s, tablet_servers_, tablet_id_,
                                  kNumWrites));

  // Check that no leadership change occurred.
  OpIdPB op_id;
  ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader, consensus::RECEIVED_OPID, 10s,
                                  &op_id));
  ASSERT_EQ(op_id.term(), 1);
}

// Run a regular workload with a leader that's writing to its WAL slowly.
TEST_F(RaftConsensusITest, TestSlowLeader) {
  if (!AllowSlowTests()) return;
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  TServerDetails* leader;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));
  ASSERT_NO_FATALS(EnableLogLatency(leader->generic_proxy.get()));

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.Setup();
  workload.Start();
  SleepFor(MonoDelta::FromSeconds(60));
}

// Run a regular workload with one follower that's writing to its WAL slowly.
TEST_F(RaftConsensusITest, TestSlowFollower) {
  if (!AllowSlowTests()) return;
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  TServerDetails* leader;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader));
  int num_reconfigured = 0;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ExternalTabletServer* ts = cluster_->tablet_server(i);
    if (ts->instance_id().permanent_uuid() != leader->uuid()) {
      TServerDetails* follower;
      follower = GetReplicaWithUuidOrNull(tablet_id_, ts->instance_id().permanent_uuid());
      ASSERT_TRUE(follower);
      ASSERT_NO_FATALS(EnableLogLatency(follower->generic_proxy.get()));
      num_reconfigured++;
      break;
    }
  }
  ASSERT_EQ(1, num_reconfigured);

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.Setup();
  workload.Start();
  SleepFor(MonoDelta::FromSeconds(60));
}

// Run a special workload that constantly updates a single row on a cluster
// where every replica is writing to its WAL slowly.
TEST_F(RaftConsensusITest, TestHammerOneRow) {
  if (!AllowSlowTests()) return;
  ASSERT_NO_FATALS(BuildAndStart(vector<string>()));

  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ExternalTabletServer* ts = cluster_->tablet_server(i);
    TServerDetails* follower;
    follower = GetReplicaWithUuidOrNull(tablet_id_, ts->instance_id().permanent_uuid());
    ASSERT_TRUE(follower);
    ASSERT_NO_FATALS(EnableLogLatency(follower->generic_proxy.get()));
  }

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_pathological_one_row_enabled(true);
  workload.set_num_write_threads(20);
  workload.Setup();
  workload.Start();
  SleepFor(MonoDelta::FromSeconds(60));
}

// Test that followers that fall behind the leader's log GC threshold are
// evicted from the config.
TEST_F(RaftConsensusITest, TestEvictAbandonedFollowers) {
  vector<string> ts_flags;
  AddFlagsForLogRolls(&ts_flags);  // For CauseFollowerToFallBehindLogGC().
  vector<string> master_flags;
  LOG(INFO) << __func__ << ": starting the cluster";
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  MonoDelta timeout = MonoDelta::FromSeconds(30);
  auto active_tablet_servers = CreateTabletServerMapUnowned(tablet_servers_);
  ASSERT_EQ(3, active_tablet_servers.size());

  string leader_uuid;
  int64_t orig_term;
  string follower_uuid;
  LOG(INFO) << __func__ << ": causing followers to fall behind";
  ASSERT_NO_FATALS(CauseFollowerToFallBehindLogGC(&leader_uuid, &orig_term, &follower_uuid));

  LOG(INFO) << __func__ << ": waiting for 2 voters in the committed config";
  // Wait for the abandoned follower to be evicted.
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(2,
                                                tablet_servers_[leader_uuid].get(),
                                                tablet_id_,
                                                timeout));
  ASSERT_EQ(1, active_tablet_servers.erase(follower_uuid));
  LOG(INFO) << __func__ << ": waiting for servers to agree";
  ASSERT_OK(WaitForServersToAgree(timeout, active_tablet_servers, tablet_id_, 2));
}

// Test that followers that fall behind the leader's log GC threshold are
// evicted from the config.
TEST_F(RaftConsensusITest, TestMasterReplacesEvictedFollowers) {
  vector<string> extra_flags;
  AddFlagsForLogRolls(&extra_flags);  // For CauseFollowerToFallBehindLogGC().
  ASSERT_NO_FATALS(BuildAndStart(extra_flags, {"--enable_load_balancing=true"}));

  MonoDelta timeout = MonoDelta::FromSeconds(30);

  string leader_uuid;
  int64_t orig_term;
  string follower_uuid;
  ASSERT_NO_FATALS(CauseFollowerToFallBehindLogGC(&leader_uuid, &orig_term, &follower_uuid));

  // The follower will be evicted. Now wait for the master to cause it to be remotely bootstrapped.
  ASSERT_OK(WaitForServersToAgree(timeout, tablet_servers_, tablet_id_, 2));

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(kTableName, ClusterVerifier::AT_LEAST, 1));
}

// Test that a ChangeConfig() request is rejected unless the leader has replicated one of its own
// log entries during the current term.  This is required for correctness of Raft config change. For
// details, see https://groups.google.com/forum/#!topic/raft-dev/t4xj6dJTP6E
TEST_F(RaftConsensusITest, TestChangeConfigRejectedUnlessNoopReplicated) {
  vector<string> ts_flags = { "--enable_leader_failure_detection=false" };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  MonoDelta timeout = MonoDelta::FromSeconds(30);

  int kLeaderIndex = 0;
  TServerDetails* leader_ts = tablet_servers_[cluster_->tablet_server(kLeaderIndex)->uuid()].get();

  // Prevent followers from accepting UpdateConsensus requests from the leader, even though they
  // will vote. This will allow us to get the distributed system into a state where there is a valid
  // leader (based on winning an election) but that leader will be unable to commit any entries from
  // its own term, making it illegal to accept ChangeConfig() requests.
  for (int i = 1; i <= 2; i++) {
    ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(i),
              "TEST_follower_reject_update_consensus_requests", "true"));
  }

  // Elect the leader.
  ASSERT_OK(StartElection(leader_ts, tablet_id_, timeout));

  // We don't need to wait until the leader obtains a lease here. In fact, that will never happen,
  // because the followers are rejecting UpdateConsensus, and the leader needs to majority-replicate
  // a lease expiration that is in the future in order to establish a leader lease.
  ASSERT_OK(WaitUntilLeader(leader_ts, tablet_id_, timeout, LeaderLeaseCheckMode::DONT_NEED_LEASE));
  // Now attempt to do a config change. It should be rejected because there have not been any ops
  // (notably the initial NO_OP) from the leader's term that have been committed yet.
  Status s = itest::RemoveServer(leader_ts,
                                 tablet_id_,
                                 tablet_servers_[cluster_->tablet_server(1)->uuid()].get(),
                                 boost::none,
                                 timeout,
                                 nullptr /* error_code */,
                                 false /* retry */);
  ASSERT_TRUE(s.IsLeaderNotReadyToServe()) << s;
  ASSERT_STR_CONTAINS(s.message().ToBuffer(),
                      "Leader not yet replicated NoOp to be ready to serve requests");
}

TEST_F(RaftConsensusITest, TestChangeConfigBasedOnJepsen) {
  FLAGS_num_tablet_servers = 4;
  vector<string> ts_flags = { "--enable_leader_failure_detection=false",
                              "--TEST_log_change_config_every_n=3000" };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));
  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  int leader_idx = -1;
  int new_node_idx = -1;
  for (int i = 0; i < 4; i++) {
    auto& uuid = cluster_->tablet_server(i)->uuid();
    if (nullptr == GetReplicaWithUuidOrNull(tablet_id_, uuid)) {
      new_node_idx = i;
      continue;
    } else if (leader_idx == -1) {
      leader_idx = i;
      continue;
    }
  }

  CHECK_NE(new_node_idx, -1) << "Could not find the node not having tablet_id_";

  TServerDetails* leader_ts = tablet_servers_[cluster_->tablet_server(leader_idx)->uuid()].get();
  MonoDelta timeout = MonoDelta::FromSeconds(30);
  ASSERT_OK(StartElection(leader_ts, tablet_id_, timeout));

  ASSERT_OK(WaitUntilLeader(leader_ts, tablet_id_, timeout, LeaderLeaseCheckMode::NEED_LEASE));

  vector<TServerDetails*> tservers_list(4);
  int ts_idx = 0;
  tservers_list[ts_idx++] = tablet_servers_[cluster_->tablet_server(leader_idx)->uuid()].get();
  for (int i = 0; i < 4; i++) {
    if (i == leader_idx || i == new_node_idx) {
      continue;
    }
    ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(i),
                                "TEST_follower_reject_update_consensus_requests", "true"));
    tservers_list[ts_idx++] = tablet_servers_[cluster_->tablet_server(i)->uuid()].get();
  }
  tservers_list[ts_idx++] = tablet_servers_[cluster_->tablet_server(new_node_idx)->uuid()].get();

  // Now attempt to do a config change. It should be rejected because there have not been any ops
  // (notably the initial NO_OP) from the leader's term that have been committed yet.
  Status s = itest::AddServer(leader_ts,
                              tablet_id_,
                              tablet_servers_[cluster_->tablet_server(new_node_idx)->uuid()].get(),
                              RaftPeerPB::PRE_VOTER,
                              boost::none,
                              timeout,
                              nullptr /* error_code */,
                              false /* retry */);
  LOG(INFO) << "Got status " << yb::ToString(s);
  const double kSleepDelaySec = 50;
  SleepFor(MonoDelta::FromSeconds(kSleepDelaySec));
  LOG(INFO) << "Done Sleeping";

  vector<OpIdPB> committed_op_ids, received_op_ids;
  GetLastOpIdForEachReplica(tablet_id_, tservers_list, consensus::OpIdType::COMMITTED_OPID,
      timeout, &committed_op_ids);
  GetLastOpIdForEachReplica(tablet_id_, tservers_list, consensus::OpIdType::RECEIVED_OPID,
      timeout, &received_op_ids);

  for(int i = 0; i < 4; i++) {
    LOG(INFO) << "i = " << i << " Peer " << tservers_list[i]->uuid()
              << " Committed op id " << yb::ToString(committed_op_ids[i])
              << " Last received op id " << yb::ToString(received_op_ids[i]);
  }

  const OpIdPB kLeaderCommittedOpId = committed_op_ids[0];
  int num_voters_who_received_committed_op_id = 0;
  for(int i = 0; i < 3; i++) {
     if (yb::consensus::OpIdCompare(kLeaderCommittedOpId, received_op_ids[i]) <= 0) {
        num_voters_who_received_committed_op_id++;
      }
  }
  CHECK_GE(num_voters_who_received_committed_op_id, 2)
      << "At least 2 voters should have received the op id";
}

// Test that if for some reason none of the transactions can be prepared, that it will come back as
// an error in UpdateConsensus().
TEST_F(RaftConsensusITest, TestUpdateConsensusErrorNonePrepared) {
  const int kNumOps = 10;

  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false",
  };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(BuildAndStart(ts_flags, master_flags));

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(3, tservers.size());

  // Shutdown the other servers so they don't get chatty.
  cluster_->tablet_server_by_uuid(tservers[1]->uuid())->Shutdown();
  cluster_->tablet_server_by_uuid(tservers[2]->uuid())->Shutdown();

  // Configure the first server to fail all on prepare.
  TServerDetails *replica_ts = tservers[0];
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server_by_uuid(replica_ts->uuid()),
                "TEST_follower_fail_all_prepare", "true"));

  // Pretend to be the leader and send a request that should return an error.
  ConsensusRequestPB req;
  ConsensusResponsePB resp;
  RpcController rpc;
  req.set_dest_uuid(replica_ts->uuid());
  req.set_tablet_id(tablet_id_);
  req.set_caller_uuid(tservers[2]->instance_id.permanent_uuid());
  req.set_caller_term(0);
  req.mutable_committed_op_id()->CopyFrom(MakeOpId(0, 0));
  req.mutable_preceding_id()->CopyFrom(MakeOpId(0, 0));
  for (int i = 0; i < kNumOps; i++) {
    AddOp(MakeOpId(0, 1 + i), &req);
  }

  ASSERT_OK(replica_ts->consensus_proxy->UpdateConsensus(req, &resp, &rpc));
  LOG(INFO) << resp.ShortDebugString();
  ASSERT_TRUE(resp.status().has_error());
  ASSERT_EQ(consensus::ConsensusErrorPB::CANNOT_PREPARE, resp.status().error().code());
  ASSERT_STR_CONTAINS(resp.ShortDebugString(), "Could not prepare a single operation");
}

TEST_F(RaftConsensusITest, TestRemoveTserverFailsWhenVoterInTransition) {
  TestRemoveTserverFailsWhenServerInTransition(RaftPeerPB::PRE_VOTER);
}

TEST_F(RaftConsensusITest, TestRemoveTserverFailsWhenObserverInTransition) {
  TestRemoveTserverFailsWhenServerInTransition(RaftPeerPB::PRE_OBSERVER);
}

TEST_F(RaftConsensusITest, TestRemovePreObserverServerSucceeds) {
  TestRemoveTserverInTransitionSucceeds(RaftPeerPB::PRE_VOTER);
}

TEST_F(RaftConsensusITest, TestRemovePreVoterServerSucceeds) {
  TestRemoveTserverInTransitionSucceeds(RaftPeerPB::PRE_OBSERVER);
}

// A test scenario to verify that a disruptive server doesn't start needless
// elections in case if it takes a long time to replicate Raft transactions
// from leader to follower replicas (e.g., due to slowness in WAL IO ops).
TEST_F(RaftConsensusITest, DisruptiveServerAndSlowWAL) {
  const MonoDelta kTimeout = MonoDelta::FromSeconds(10);
  // Shorten the heartbeat interval for faster test run time.
  const auto kHeartbeatIntervalMs = 200;
  const auto kMaxMissedHeartbeatPeriods = 3;
  const vector<string> ts_flags {
    Substitute("--raft_heartbeat_interval_ms=$0", kHeartbeatIntervalMs),
    Substitute("--leader_failure_max_missed_heartbeat_periods=$0",
               kMaxMissedHeartbeatPeriods),
  };
  NO_FATALS(BuildAndStart(ts_flags));

  // Sanity check: this scenario assumes there are 3 tablet servers. The test
  // table is created with RF=FLAGS_num_replicas.
  ASSERT_EQ(3, FLAGS_num_replicas);
  ASSERT_EQ(3, tablet_servers_.size());

  // A convenience array to access each tablet server as TServerDetails.
  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(cluster_->num_tablet_servers(), tservers.size());

  // The leadership might fluctuate before shutting down the third tablet
  // server, so ASSERT_EVENTUALLY() below is for those rare cases.
  //
  // However, after one of the tablet servers is shutdown, the leadership should
  // not fluctuate because:
  //   1) only two voters out of three are alive
  //   2) current leader should not be disturbed by any rogue votes -- that's
  //      the whole premise of this test scenario
  //
  // So, for this scenario the leadership can fluctuate only if significantly
  // delaying or dropping Raft heartbeats sent from leader to follower replicas.
  // However, minicluster components send heartbeats via the loopback interface,
  // so no real network layer that might significantly delay heartbeats
  // is involved. Also, the consensus RPC queues should not overflow
  // in this scenario because the number of consensus RPCs is relatively low.
  TServerDetails* leader_tserver = nullptr;
  TServerDetails* other_tserver = nullptr;
  TServerDetails* shutdown_tserver = nullptr;
  ASSERT_EVENTUALLY([&] {
    // This is a clean-up in case of retry.
    if (shutdown_tserver) {
      auto* ts = cluster_->tablet_server_by_uuid(shutdown_tserver->uuid());
      if (ts->IsShutdown()) {
        ASSERT_OK(ts->Restart());
      }
    }
    for (auto idx = 0; idx < cluster_->num_tablet_servers(); ++idx) {
      auto* ts = cluster_->tablet_server(idx);
      ASSERT_OK(cluster_->SetFlag(ts, "log_inject_latency", "false"));
    }
    leader_tserver = nullptr;
    other_tserver = nullptr;
    shutdown_tserver = nullptr;

    ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &leader_tserver));
    ASSERT_OK(WriteSimpleTestRow(leader_tserver, tablet_id_,
                                 0 /* key */, 0 /* int_val */, "" /* string_val */, kTimeout));
    OpIdPB op_id;
    ASSERT_OK(GetLastOpIdForReplica(tablet_id_, leader_tserver,
                                    consensus::COMMITTED_OPID, kTimeout,
                                    &op_id));
    ASSERT_OK(WaitForServersToAgree(kTimeout, tablet_servers_, tablet_id_,
                                    op_id.index()));
    // Shutdown one tablet server that doesn't host the leader replica of the
    // target tablet and inject WAL latency to others.
    for (const auto& server : tservers) {
      auto* ts = cluster_->tablet_server_by_uuid(server->uuid());
      const bool is_leader = server->uuid() == leader_tserver->uuid();
      if (!is_leader && !shutdown_tserver) {
        shutdown_tserver = server;
        continue;
      }
      if (!is_leader && !other_tserver) {
        other_tserver = server;
      }
      // For this scenario it's important to reserve some inactivity intervals
      // for the follower between processing Raft messages from the leader.
      // If a vote request arrives while replica is busy with processing
      // Raft message from leader, it is rejected with 'busy' status before
      // evaluating the vote withholding interval.
      const auto mult = is_leader ? 2 : 1;
      const auto latency_ms = mult * kHeartbeatIntervalMs * kMaxMissedHeartbeatPeriods;
      ASSERT_OK(cluster_->SetFlag(ts, "log_inject_latency_ms_mean",
                                  std::to_string(latency_ms)));
      ASSERT_OK(cluster_->SetFlag(ts, "log_inject_latency_ms_stddev", "0"));
      ASSERT_OK(cluster_->SetFlag(ts, "log_inject_latency", "true"));
    }

    // Shutdown the third tablet server.
    cluster_->tablet_server_by_uuid(shutdown_tserver->uuid())->Shutdown();

    // Sanity check: make sure the leadership hasn't changed since the leader
    // has been determined.
    TServerDetails* current_leader;
    ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &current_leader));
    ASSERT_EQ(cluster_->tablet_server_index_by_uuid(leader_tserver->uuid()),
              cluster_->tablet_server_index_by_uuid(current_leader->uuid()));
  });

  // Get the Raft term from the established leader.
  consensus::ConsensusStatePB cstate;
  ASSERT_OK(GetConsensusState(leader_tserver, tablet_id_,
                              consensus::CONSENSUS_CONFIG_COMMITTED, kTimeout, &cstate));

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_timeout_allowed(true);
  workload.set_num_write_threads(1);
  workload.set_write_batch_size(1);
  // Make a 'space' for the artificial vote requests (see below) to arrive
  // while there is no activity on RaftConsensus::Update().
  workload.set_write_interval_millis(kHeartbeatIntervalMs);
  workload.Setup();
  workload.Start();

  // Issue rogue vote requests, imitating a disruptive tablet replica.
  const auto& shutdown_server_uuid = shutdown_tserver->uuid();
  const auto next_term = cstate.current_term() + 1;
  const auto targets = { leader_tserver, other_tserver };
  for (auto i = 0; i < 100; ++i) {
    SleepFor(MonoDelta::FromMilliseconds(kHeartbeatIntervalMs / 4));
    for (const auto* ts : targets) {
      auto s = RequestVote(ts, tablet_id_, shutdown_server_uuid,
                           next_term, MakeOpId(next_term + i, 0),
                           /*ignore_live_leader=*/ false,
                           /*is_pre_election=*/ false,
                           kTimeout);
      // Neither leader nor follower replica should grant 'yes' vote
      // since the healthy leader is there and doing well, sending Raft
      // transactions to be replicated.
      ASSERT_TRUE(s.IsInvalidArgument() || s.IsServiceUnavailable())
          << s.ToString();
      std::regex pattern(
          "("
              "because replica is either leader or "
              "believes a valid leader to be alive"
          "|"
              "because replica is already servicing an update "
              "from a current leader or another vote"
          ")");
      ASSERT_TRUE(std::regex_search(s.ToString(), pattern));
    }
  }
}

    }  // namespace tserver
}  // namespace yb
