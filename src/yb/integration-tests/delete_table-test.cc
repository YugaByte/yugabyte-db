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

#include <memory>
#include <string>
#include <unordered_map>
#include <thread>

#include <boost/optional.hpp>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>

#include "yb/client/client-test-util.h"
#include "yb/client/table_creator.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/split.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/external_mini_cluster-itest-base.h"
#include "yb/integration-tests/test_workload.h"
#include "yb/master/master_defaults.h"
#include "yb/tablet/tablet.pb.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/util/curl_util.h"
#include "yb/util/subprocess.h"

using yb::client::YBClient;
using yb::client::YBClientBuilder;
using yb::client::YBSchema;
using yb::client::YBSchemaFromSchema;
using yb::client::YBTableCreator;
using yb::client::YBTableName;
using yb::consensus::CONSENSUS_CONFIG_COMMITTED;
using yb::consensus::ConsensusMetadataPB;
using yb::consensus::ConsensusStatePB;
using yb::consensus::RaftPeerPB;
using yb::itest::TServerDetails;
using yb::tablet::TABLET_DATA_COPYING;
using yb::tablet::TABLET_DATA_DELETED;
using yb::tablet::TABLET_DATA_READY;
using yb::tablet::TABLET_DATA_TOMBSTONED;
using yb::tablet::TabletDataState;
using yb::tablet::RaftGroupReplicaSuperBlockPB;
using yb::tserver::ListTabletsResponsePB;
using yb::tserver::TabletServerErrorPB;
using std::numeric_limits;
using std::string;
using std::unordered_map;
using std::vector;
using strings::Substitute;

using namespace std::literals;

namespace yb {

class DeleteTableTest : public ExternalMiniClusterITestBase {
 protected:
  enum IsCMetaExpected {
    CMETA_NOT_EXPECTED = 0,
    CMETA_EXPECTED = 1
  };

  enum IsSuperBlockExpected {
    SUPERBLOCK_NOT_EXPECTED = 0,
    SUPERBLOCK_EXPECTED = 1
  };

  // Get the UUID of the leader of the specified tablet, as seen by the TS with
  // the given 'ts_uuid'.
  string GetLeaderUUID(const string& ts_uuid, const string& tablet_id);

  Status CheckTabletTombstonedOrDeletedOnTS(
      int index,
      const string& tablet_id,
      TabletDataState data_state,
      IsCMetaExpected is_cmeta_expected,
      IsSuperBlockExpected is_superblock_expected);

  Status CheckTabletTombstonedOnTS(int index,
                                   const string& tablet_id,
                                   IsCMetaExpected is_cmeta_expected);

  Status CheckTabletDeletedOnTS(int index,
                                const string& tablet_id,
                                IsSuperBlockExpected is_superblock_expected);

  void WaitForTabletTombstonedOnTS(int index,
                                   const string& tablet_id,
                                   IsCMetaExpected is_cmeta_expected);

  void WaitForTabletDeletedOnTS(int index,
                                const string& tablet_id,
                                IsSuperBlockExpected is_superblock_expected);

  void WaitForAllTSToCrash();
  void WaitUntilTabletRunning(int index, const std::string& tablet_id);

  // Delete the given table. If the operation times out, dumps the master stacks
  // to help debug master-side deadlocks.
  void DeleteTable(const YBTableName& table_name);

  // Repeatedly try to delete the tablet, retrying on failure up to the
  // specified timeout. Deletion can fail when other operations, such as
  // bootstrap, are running.
  void DeleteTabletWithRetries(const TServerDetails* ts, const string& tablet_id,
                               TabletDataState delete_type, const MonoDelta& timeout);
};

string DeleteTableTest::GetLeaderUUID(const string& ts_uuid, const string& tablet_id) {
  ConsensusStatePB cstate;
  auto deadline = MonoTime::Now() + 10s;
  for (;;) {
    CHECK_OK(itest::GetConsensusState(
        ts_map_[ts_uuid].get(),
        tablet_id,
        CONSENSUS_CONFIG_COMMITTED,
        deadline - MonoTime::Now(),
        &cstate));
    if (!cstate.leader_uuid().empty()) {
      break;
    }
    CHECK(MonoTime::Now() <= deadline);
    std::this_thread::sleep_for(100ms);
  }
  CHECK(!cstate.leader_uuid().empty());
  return cstate.leader_uuid();
}

Status DeleteTableTest::CheckTabletTombstonedOrDeletedOnTS(
      int index,
      const string& tablet_id,
      TabletDataState data_state,
      IsCMetaExpected is_cmeta_expected,
      IsSuperBlockExpected is_superblock_expected) {
  CHECK(data_state == TABLET_DATA_TOMBSTONED || data_state == TABLET_DATA_DELETED) << data_state;
  // There should be no WALs and no cmeta.
  if (inspect_->CountWALSegmentsForTabletOnTS(index, tablet_id) > 0) {
    return STATUS(IllegalState, "WAL segments exist for tablet", tablet_id);
  }
  if (is_cmeta_expected == CMETA_EXPECTED &&
      !inspect_->DoesConsensusMetaExistForTabletOnTS(index, tablet_id)) {
    return STATUS(IllegalState, "Expected cmeta for tablet " + tablet_id + " but it doesn't exist");
  }
  if (is_superblock_expected == SUPERBLOCK_EXPECTED) {
    RETURN_NOT_OK(inspect_->CheckTabletDataStateOnTS(index, tablet_id, data_state));
  } else {
    RaftGroupReplicaSuperBlockPB superblock_pb;
    Status s = inspect_->ReadTabletSuperBlockOnTS(index, tablet_id, &superblock_pb);
    if (!s.IsNotFound()) {
      return STATUS(IllegalState, "Found unexpected superblock for tablet " + tablet_id);
    }
  }
  return Status::OK();
}

Status DeleteTableTest::CheckTabletTombstonedOnTS(int index,
                                                  const string& tablet_id,
                                                  IsCMetaExpected is_cmeta_expected) {
  return CheckTabletTombstonedOrDeletedOnTS(index, tablet_id, TABLET_DATA_TOMBSTONED,
                                            is_cmeta_expected, SUPERBLOCK_EXPECTED);
}

Status DeleteTableTest::CheckTabletDeletedOnTS(int index,
                                               const string& tablet_id,
                                               IsSuperBlockExpected is_superblock_expected) {
  return CheckTabletTombstonedOrDeletedOnTS(index, tablet_id, TABLET_DATA_DELETED,
                                            CMETA_NOT_EXPECTED, is_superblock_expected);
}

void DeleteTableTest::WaitForTabletTombstonedOnTS(int index,
                                                  const string& tablet_id,
                                                  IsCMetaExpected is_cmeta_expected) {
  Status s;
  for (int i = 0; i < 6000; i++) {
    s = CheckTabletTombstonedOnTS(index, tablet_id, is_cmeta_expected);
    if (s.ok()) return;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_OK(s);
}

void DeleteTableTest::WaitForTabletDeletedOnTS(int index,
                                               const string& tablet_id,
                                               IsSuperBlockExpected is_superblock_expected) {
  Status s;
  for (int i = 0; i < 6000; i++) {
    s = CheckTabletDeletedOnTS(index, tablet_id, is_superblock_expected);
    if (s.ok()) return;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_OK(s);
}

void DeleteTableTest::WaitForAllTSToCrash() {
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(cluster_->WaitForTSToCrash(i));
  }
}

void DeleteTableTest::WaitUntilTabletRunning(int index, const std::string& tablet_id) {
  ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(index)->uuid()].get(),
                                          tablet_id,
                                          MonoDelta::FromSeconds(30)));
}

void DeleteTableTest::DeleteTable(const YBTableName& table_name) {
  Status s = client_->DeleteTable(table_name);
  if (s.IsTimedOut()) {
    WARN_NOT_OK(PstackWatcher::DumpPidStacks(cluster_->master()->pid()),
                        "Couldn't dump stacks");
  }
  ASSERT_OK(s);
}

void DeleteTableTest::DeleteTabletWithRetries(const TServerDetails* ts,
                                              const string& tablet_id,
                                              TabletDataState delete_type,
                                              const MonoDelta& timeout) {
  MonoTime start(MonoTime::Now());
  MonoTime deadline = start;
  deadline.AddDelta(timeout);
  Status s;
  while (true) {
    s = itest::DeleteTablet(ts, tablet_id, delete_type, boost::none, timeout);
    if (s.ok()) return;
    if (deadline.ComesBefore(MonoTime::Now())) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_OK(s);
}

TEST_F(DeleteTableTest, TestPendingDeleteStateClearedOnFailure) {
  vector<string> tserver_flags, master_flags;
  master_flags.push_back("--unresponsive_ts_rpc_timeout_ms=5000");
  // Disable tablet delete operations.
  tserver_flags.push_back("--TEST_rpc_delete_tablet_fail=true");
  ASSERT_NO_FATALS(StartCluster(tserver_flags, master_flags, 3));
  // Create a table on the cluster. We're just using TestWorkload
  // as a convenient way to create it.
  auto test_workload = TestWorkload(cluster_.get());
  test_workload.Setup();

  // The table should have replicas on all three tservers.
  ASSERT_OK(inspect_->WaitForReplicaCount(3));

  client_->TEST_set_admin_operation_timeout(MonoDelta::FromSeconds(10));

  // Delete the table.
  DeleteTable(TestWorkloadOptions::kDefaultTableName);

  // Wait for the load balancer to report no pending deletes after the delete table fails.
  ASSERT_OK(WaitFor([&] () { return client_->IsLoadBalanced(3); },
            MonoDelta::FromSeconds(30), "IsLoadBalanced"));
}

// Test deleting an empty table, and ensure that the tablets get removed,
// and the master no longer shows the table as existing.
TEST_F(DeleteTableTest, TestDeleteEmptyTable) {
  ASSERT_NO_FATALS(StartCluster());
  // Create a table on the cluster. We're just using TestWorkload
  // as a convenient way to create it.
  TestWorkload(cluster_.get()).Setup();

  // The table should have replicas on all three tservers.
  ASSERT_OK(inspect_->WaitForReplicaCount(3));

  // Grab the tablet ID (used later).
  vector<string> tablets = inspect_->ListTabletsOnTS(1);
  ASSERT_EQ(1, tablets.size());
  const string& tablet_id = tablets[0];

  // Delete it and wait for the replicas to get deleted.
  ASSERT_NO_FATALS(DeleteTable(TestWorkloadOptions::kDefaultTableName));
  for (int i = 0; i < 3; i++) {
    ASSERT_NO_FATALS(WaitForTabletDeletedOnTS(i, tablet_id, SUPERBLOCK_EXPECTED));
  }

  // Restart the cluster, the superblocks should be deleted on startup.
  cluster_->Shutdown();
  ASSERT_OK(cluster_->Restart());
  ASSERT_OK(inspect_->WaitForNoData());

  // Check that the master no longer exposes the table in any way:

  // 1) Should not list it in ListTables.
  const auto tables = ASSERT_RESULT(client_->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(master::kNumSystemTables, tables.size());

  // 2) Should respond to GetTableSchema with a NotFound error.
  YBSchema schema;
  PartitionSchema partition_schema;
  Status s = client_->GetTableSchema(
      TestWorkloadOptions::kDefaultTableName, &schema, &partition_schema);
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();

  // 3) Should return an error for GetTabletLocations RPCs.
  {
    rpc::RpcController rpc;
    master::GetTabletLocationsRequestPB req;
    master::GetTabletLocationsResponsePB resp;
    rpc.set_timeout(MonoDelta::FromSeconds(10));
    req.add_tablet_ids()->assign(tablet_id);
    ASSERT_OK(cluster_->master_proxy()->GetTabletLocations(req, &resp, &rpc));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_EQ(1, resp.errors_size());
    ASSERT_STR_CONTAINS(resp.errors(0).ShortDebugString(), "code: NOT_FOUND");
  }

  // 4) The master 'dump-entities' page should not list the deleted table or tablets.
  EasyCurl c;
  faststring entities_buf;
  ASSERT_OK(c.FetchURL(Substitute("http://$0/dump-entities",
                                  cluster_->master()->bound_http_hostport().ToString()),
                       &entities_buf));
  ASSERT_TRUE(entities_buf.ToString().find(
      TestWorkloadOptions::kDefaultTableName.table_name()) == std::string::npos);
}

// Test that a DeleteTable RPC is rejected without a matching destination UUID.
TEST_F(DeleteTableTest, TestDeleteTableDestUuidValidation) {
  ASSERT_NO_FATALS(StartCluster());
  // Create a table on the cluster. We're just using TestWorkload
  // as a convenient way to create it.
  TestWorkload(cluster_.get()).Setup();
  ASSERT_OK(inspect_->WaitForReplicaCount(3));

  vector<string> tablets = inspect_->ListTabletsOnTS(1);
  ASSERT_EQ(1, tablets.size());
  const string& tablet_id = tablets[0];

  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();

  tserver::DeleteTabletRequestPB req;
  tserver::DeleteTabletResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(20));

  req.set_dest_uuid("fake-uuid");
  req.set_tablet_id(tablet_id);
  req.set_delete_type(TABLET_DATA_TOMBSTONED);
  ASSERT_OK(ts->tserver_admin_proxy->DeleteTablet(req, &resp, &rpc));
  ASSERT_TRUE(resp.has_error());
  ASSERT_EQ(tserver::TabletServerErrorPB::WRONG_SERVER_UUID, resp.error().code())
      << resp.ShortDebugString();
  ASSERT_STR_CONTAINS(StatusFromPB(resp.error().status()).ToString(),
                      "Wrong destination UUID");
}

// Test the atomic CAS argument to DeleteTablet().
TEST_F(DeleteTableTest, TestAtomicDeleteTablet) {
  MonoDelta timeout = MonoDelta::FromSeconds(30);
  ASSERT_NO_FATALS(StartCluster());
  // Create a table on the cluster. We're just using TestWorkload
  // as a convenient way to create it.
  TestWorkload(cluster_.get()).Setup();

  // The table should have replicas on all three tservers.
  ASSERT_OK(inspect_->WaitForReplicaCount(3));

  // Grab the tablet ID (used later).
  vector<string> tablets = inspect_->ListTabletsOnTS(1);
  ASSERT_EQ(1, tablets.size());
  const string& tablet_id = tablets[0];

  const int kTsIndex = 0;
  TServerDetails* ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();

  // The committed config starts off with an opid_index of -1, so choose something lower.
  boost::optional<int64_t> opid_index(-2);
  tserver::TabletServerErrorPB::Code error_code;
  ASSERT_OK(itest::WaitUntilTabletRunning(ts, tablet_id, timeout));

  Status s;
  for (int i = 0; i < 100; i++) {
    s = itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, opid_index, timeout,
                            &error_code);
    if (error_code == TabletServerErrorPB::CAS_FAILED) break;
    // If we didn't get the expected CAS_FAILED error, it's OK to get 'TABLET_NOT_RUNNING'
    // because the "creating" maintenance state persists just slightly after it starts to
    // expose 'RUNNING' state in ListTablets()
    ASSERT_EQ(TabletServerErrorPB::TABLET_NOT_RUNNING, error_code)
        << "unexpected error: " << s.ToString();
    SleepFor(MonoDelta::FromMilliseconds(100));
  }

  ASSERT_EQ(TabletServerErrorPB::CAS_FAILED, error_code) << "unexpected error: " << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "of -2 but the committed config has opid_index of -1");

  // Now use the "latest", which is -1.
  opid_index = -1;
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, opid_index, timeout,
                                &error_code));
  ASSERT_OK(inspect_->CheckTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_TOMBSTONED));

  // Now that the tablet is already tombstoned, our opid_index should be
  // ignored (because it's impossible to check it).
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, -9999, timeout,
                                &error_code));
  ASSERT_OK(inspect_->CheckTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_TOMBSTONED));

  // Same with TOMBSTONED -> DELETED.
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_DELETED, -9999, timeout,
                                &error_code));
  ASSERT_OK(inspect_->CheckTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_DELETED));
}

TEST_F(DeleteTableTest, TestDeleteTableWithConcurrentWrites) {
  ASSERT_NO_FATALS(StartCluster());
  int n_iters = AllowSlowTests() ? 20 : 1;
  for (int i = 0; i < n_iters; i++) {
    TestWorkload workload(cluster_.get());
    workload.set_table_name(YBTableName(YQL_DATABASE_CQL, "my_keyspace",
        Substitute("table-$0", i)));

    // We'll delete the table underneath the writers, so we expcted
    // a NotFound error during the writes.
    workload.set_not_found_allowed(true);
    workload.Setup();

    // Start the workload, and wait to see some rows actually inserted
    workload.Start();
    while (workload.rows_inserted() < 100) {
      SleepFor(MonoDelta::FromMilliseconds(10));
    }

    vector<string> tablets = inspect_->ListTabletsOnTS(1);
    ASSERT_EQ(1, tablets.size());
    const string& tablet_id = tablets[0];

    // Delete it and wait for the replicas to get deleted.
    ASSERT_NO_FATALS(DeleteTable(workload.table_name()));
    for (int i = 0; i < 3; i++) {
      ASSERT_NO_FATALS(WaitForTabletDeletedOnTS(i, tablet_id, SUPERBLOCK_EXPECTED));
    }

    // Sleep just a little longer to make sure client threads send
    // requests to the missing tablets.
    SleepFor(MonoDelta::FromMilliseconds(50));

    workload.StopAndJoin();
    cluster_->AssertNoCrashes();

    // Restart the cluster, the superblocks should be deleted on startup.
    cluster_->Shutdown();
    ASSERT_OK(cluster_->Restart());
    ASSERT_OK(inspect_->WaitForNoData());
  }
}

TEST_F(DeleteTableTest, DeleteTableWithConcurrentWritesNoRestarts) {
  ASSERT_NO_FATALS(StartCluster());
  constexpr auto kNumIters = 10;
  for (int iter = 0; iter < kNumIters; iter++) {
    TestWorkload workload(cluster_.get());
    workload.set_table_name(YBTableName(YQL_DATABASE_CQL, "my_keyspace", Format("table-$0", iter)));

    // We'll delete the table underneath the writers, so we expect a NotFound error during the
    // writes.
    workload.set_not_found_allowed(true);
    workload.Setup();
    workload.Start();

    AssertLoggedWaitFor(
        [&workload] { return workload.rows_inserted() > 100; }, 60s,
        "Waiting until we have inserted some data...", 10ms);

    auto tablets = inspect_->ListTabletsWithDataOnTS(1);
    ASSERT_EQ(1, tablets.size());
    const auto& tablet_id = tablets[0];

    ASSERT_NO_FATALS(DeleteTable(workload.table_name()));
    for (int ts_idx = 0; ts_idx < cluster_->num_tablet_servers(); ts_idx++) {
      ASSERT_NO_FATALS(WaitForTabletDeletedOnTS(ts_idx, tablet_id, SUPERBLOCK_EXPECTED));
    }

    workload.StopAndJoin();
    cluster_->AssertNoCrashes();
  }
}

// Test that a tablet replica is automatically tombstoned on startup if a local
// crash occurs in the middle of remote bootstrap.
TEST_F(DeleteTableTest, TestAutoTombstoneAfterCrashDuringRemoteBootstrap) {
  vector<string> tserver_flags, master_flags;
  master_flags.push_back("--replication_factor=2");
  ASSERT_NO_FATALS(StartCluster(tserver_flags, master_flags));
  const MonoDelta timeout = MonoDelta::FromSeconds(10);
  const int kTsIndex = 0;  // We'll test with the first TS.

  // We'll do a config change to remote bootstrap a replica here later. For
  // now, shut it down.
  LOG(INFO) << "Shutting down TS " << cluster_->tablet_server(kTsIndex)->uuid();
  cluster_->tablet_server(kTsIndex)->Shutdown();

  // Bounce the Master so it gets new tablet reports and doesn't try to assign
  // a replica to the dead TS.
  cluster_->master()->Shutdown();
  ASSERT_OK(cluster_->master()->Restart());
  ASSERT_OK(cluster_->WaitForTabletServerCount(2, timeout));

  // Start a workload on the cluster, and run it for a little while.
  TestWorkload workload(cluster_.get());
  workload.Setup();
  ASSERT_OK(inspect_->WaitForReplicaCount(2));

  workload.Start();
  while (workload.rows_inserted() < 100) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  workload.StopAndJoin();

  // Enable a fault crash when remote bootstrap occurs on TS 0.
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart());
  const string& kFaultFlag = "TEST_fault_crash_after_rb_files_fetched";
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(kTsIndex), kFaultFlag, "1.0"));

  // Figure out the tablet id to remote bootstrap.
  vector<string> tablets = inspect_->ListTabletsOnTS(1);
  ASSERT_EQ(1, tablets.size());
  const string& tablet_id = tablets[0];

  // Add our TS 0 to the config and wait for it to crash.
  string leader_uuid = GetLeaderUUID(cluster_->tablet_server(1)->uuid(), tablet_id);
  TServerDetails* leader = DCHECK_NOTNULL(ts_map_[leader_uuid].get());
  TServerDetails* ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();
  ASSERT_OK(itest::AddServer(leader, tablet_id, ts, RaftPeerPB::PRE_VOTER, boost::none, timeout));
  ASSERT_OK(cluster_->WaitForTSToCrash(kTsIndex));

  // The superblock should be in TABLET_DATA_COPYING state on disk.
  ASSERT_OK(inspect_->CheckTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_COPYING));

  // Kill the other tablet servers so the leader doesn't try to remote
  // bootstrap it again during our verification here.
  cluster_->tablet_server(1)->Shutdown();
  cluster_->tablet_server(2)->Shutdown();

  // Now we restart the TS. It will clean up the failed remote bootstrap and
  // convert it to TABLET_DATA_TOMBSTONED. It crashed, so we have to call
  // Shutdown() then Restart() to bring it back up.
  cluster_->tablet_server(kTsIndex)->Shutdown();
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart());
  ASSERT_NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id, CMETA_NOT_EXPECTED));
}

// Test that a tablet replica automatically tombstones itself if the remote
// bootstrap source server fails in the middle of the remote bootstrap process.
// Also test that we can remotely bootstrap a tombstoned tablet.
TEST_F(DeleteTableTest, TestAutoTombstoneAfterRemoteBootstrapRemoteFails) {
  vector<string> tserver_flags, master_flags;

  tserver_flags.push_back("--log_segment_size_mb=1");  // Faster log rolls.

  master_flags.push_back("--enable_load_balancing=false");
  master_flags.push_back("--replication_factor=2");

  // Start the cluster with load balancer turned off.
  ASSERT_NO_FATALS(StartCluster(tserver_flags, master_flags));
  const MonoDelta timeout = MonoDelta::FromSeconds(40);
  const int kTsIndex = 0;  // We'll test with the first TS.

  // We'll do a config change to remote bootstrap a replica here later. For
  // now, shut it down.
  LOG(INFO) << "Shutting down TS " << cluster_->tablet_server(kTsIndex)->uuid();
  cluster_->tablet_server(kTsIndex)->Shutdown();

  // Bounce the Master so it gets new tablet reports and doesn't try to assign
  // a replica to the dead TS.
  cluster_->master()->Shutdown();
  ASSERT_OK(cluster_->master()->Restart());
  ASSERT_OK(cluster_->WaitForTabletServerCount(2, timeout));

  // Start a workload on the cluster, and run it for a little while.
  TestWorkload workload(cluster_.get());
  workload.Setup();
  ASSERT_OK(inspect_->WaitForReplicaCount(2));

  vector<string> tablets = inspect_->ListTabletsOnTS(1);
  ASSERT_EQ(1, tablets.size());
  const string& tablet_id = tablets[0];

  workload.Start();
  while (workload.rows_inserted() < 100) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  // Remote bootstrap doesn't see the active WAL segment, and we need to
  // download a file to trigger the fault in this test. Due to the log index
  // chunks, that means 3 files minimum: One in-flight WAL segment, one index
  // chunk file (these files grow much more slowly than the WAL segments), and
  // one completed WAL segment.
  string leader_uuid = GetLeaderUUID(cluster_->tablet_server(1)->uuid(), tablet_id);
  int leader_index = cluster_->tablet_server_index_by_uuid(leader_uuid);
  ASSERT_NE(-1, leader_index);
  ASSERT_OK(inspect_->WaitForMinFilesInTabletWalDirOnTS(leader_index, tablet_id, 3));
  workload.StopAndJoin();

  // Cause the leader to crash when a follower tries to remotely bootstrap from it.
  const string& fault_flag = "TEST_fault_crash_on_handle_rb_fetch_data";
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(leader_index), fault_flag, "1.0"));

  // Add our TS 0 to the config and wait for the leader to crash.
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart());
  TServerDetails* leader = ts_map_[leader_uuid].get();
  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();
  ASSERT_OK(itest::AddServer(leader, tablet_id, ts, RaftPeerPB::PRE_VOTER, boost::none, timeout));
  ASSERT_OK(cluster_->WaitForTSToCrash(leader_index));

  // The tablet server will detect that the leader failed, and automatically
  // tombstone its replica. Shut down the other non-leader replica to avoid
  // interference while we wait for this to happen.
  cluster_->tablet_server(1)->Shutdown();
  cluster_->tablet_server(2)->Shutdown();
  ASSERT_NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id, CMETA_NOT_EXPECTED));

  // Now bring the other replicas back, and wait for the leader to remote
  // bootstrap the tombstoned replica. This will have replaced a tablet with no
  // consensus metadata.
  ASSERT_OK(cluster_->tablet_server(1)->Restart());
  ASSERT_OK(cluster_->tablet_server(2)->Restart());
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_READY));

  // Because the object deleter (whose destructor will unset the variable transition_in_progress_)
  // is created before rb_client in TsTabletManager::StartRemoteBootstrap, rb_client will be
  // destroyed before deleter. Before rb_client is destroyed, the remote bootstrap session has to be
  // destroyed too. With the new PRE_VOTER member_type, a remote bootstrap session won't finish
  // until we have successfully started a ChangeConfig. This will delay the destruction of
  // rb_client. Thus we need to wait until we know that tablet_server(0) has been promoted to a
  // VOTER role before we continue. Otherwise, we might send the DeleteTablet request before
  // transition_in_progress_ has been cleared and we'll get error
  // "State transition of tablet XXX already in progress: remote bootstrapping tablet".
  leader_uuid = GetLeaderUUID(cluster_->tablet_server(1)->uuid(), tablet_id);
  auto leader_it = ts_map_.find(leader_uuid);
  ASSERT_NE(leader_it, ts_map_.end())
      << "Leader UUID: " << leader_uuid << ", ts map: " << yb::ToString(ts_map_);
  leader = leader_it->second.get();
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(3, leader, tablet_id, timeout));

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(), ClusterVerifier::AT_LEAST,
                            workload.rows_inserted()));

  // For now there is no way to know if the server has finished its remote bootstrap (by verifying
  // that its role has changed in its consensus object). As a workaround, sleep for 10 seconds
  // before pausing the other two servers which are needed to propagate the consensus to the new
  // server.
  SleepFor(MonoDelta::FromSeconds(10));
  // Now pause the other replicas and tombstone our replica again.
  ASSERT_OK(cluster_->tablet_server(1)->Pause());
  ASSERT_OK(cluster_->tablet_server(2)->Pause());

  // If we send the request before the lock in StartRemoteBootstrap is released (not really a lock,
  // but effectively it serves as one), we need to retry.
  ASSERT_NO_FATALS(DeleteTabletWithRetries(ts, tablet_id, TABLET_DATA_TOMBSTONED, timeout));
  ASSERT_NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id, CMETA_NOT_EXPECTED));

  // Bring them back again, let them yet again bootstrap our tombstoned replica.
  // This time, the leader will have replaced a tablet with consensus metadata.
  ASSERT_OK(cluster_->tablet_server(1)->Resume());
  ASSERT_OK(cluster_->tablet_server(2)->Resume());
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_READY));

  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(), ClusterVerifier::AT_LEAST,
                            workload.rows_inserted()));
}

// Test for correct remote bootstrap merge of consensus metadata.
TEST_F(DeleteTableTest, TestMergeConsensusMetadata) {
  // Enable manual leader selection.
  std::vector<std::string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    // Disable pre-elections since we wait for term to become 2,
    // that does not happen with pre-elections
    "--use_preelection=false"s
  };

  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));
  const MonoDelta timeout = MonoDelta::FromSeconds(10);
  const int kTsIndex = 0;

  TestWorkload workload(cluster_.get());
  workload.Setup();
  ASSERT_OK(inspect_->WaitForReplicaCount(3));

  // Figure out the tablet id to remote bootstrap.
  vector<string> tablets = inspect_->ListTabletsOnTS(1);
  ASSERT_EQ(1, tablets.size());
  const string& tablet_id = tablets[0];

  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_NO_FATALS(WaitUntilTabletRunning(i, tablet_id));
  }

  // Elect a leader and run some data through the cluster.
  int leader_index = 1;
  string leader_uuid = cluster_->tablet_server(leader_index)->uuid();
  ASSERT_OK(itest::StartElection(ts_map_[leader_uuid].get(), tablet_id, timeout));
  workload.Start();
  while (workload.rows_inserted() < 100) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  workload.StopAndJoin();
  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, workload.batches_completed()));

  // Verify that TS 0 voted for the chosen leader.
  ConsensusMetadataPB cmeta_pb;
  ASSERT_OK(inspect_->ReadConsensusMetadataOnTS(kTsIndex, tablet_id, &cmeta_pb));
  ASSERT_EQ(1, cmeta_pb.current_term());
  ASSERT_EQ(leader_uuid, cmeta_pb.voted_for());

  // Shut down all but TS 0 and try to elect TS 0. The election will fail but
  // the TS will record a vote for itself as well as a new term (term 2).
  cluster_->tablet_server(1)->Shutdown();
  cluster_->tablet_server(2)->Shutdown();
  ASSERT_NO_FATALS(WaitUntilTabletRunning(kTsIndex, tablet_id));
  TServerDetails* ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();
  ASSERT_OK(itest::StartElection(ts, tablet_id, timeout));
  for (int i = 0; i < 6000; i++) {
    Status s = inspect_->ReadConsensusMetadataOnTS(kTsIndex, tablet_id, &cmeta_pb);
    if (s.ok() &&
        cmeta_pb.current_term() == 2 &&
        cmeta_pb.voted_for() == ts->uuid()) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_EQ(2, cmeta_pb.current_term());
  ASSERT_EQ(ts->uuid(), cmeta_pb.voted_for());

  // Tombstone our special little guy, then shut him down.
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none, timeout));
  ASSERT_NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id, CMETA_EXPECTED));
  cluster_->tablet_server(kTsIndex)->Shutdown();

  // Restart the other dudes and re-elect the same leader.
  ASSERT_OK(cluster_->tablet_server(1)->Restart());
  ASSERT_OK(cluster_->tablet_server(2)->Restart());
  TServerDetails* leader = ts_map_[leader_uuid].get();
  ASSERT_NO_FATALS(WaitUntilTabletRunning(1, tablet_id));
  ASSERT_NO_FATALS(WaitUntilTabletRunning(2, tablet_id));
  ASSERT_OK(itest::StartElection(leader, tablet_id, timeout));
  ASSERT_OK(itest::WaitUntilLeader(leader, tablet_id, timeout));

  // Bring our special little guy back up.
  // Wait until he gets remote bootstrapped.
  LOG(INFO) << "Bringing TS " << cluster_->tablet_server(kTsIndex)->uuid()
            << " back up...";
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart());
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_READY));

  // Assert that the election history is retained (voted for self).
  ASSERT_OK(inspect_->ReadConsensusMetadataOnTS(kTsIndex, tablet_id, &cmeta_pb));
  ASSERT_EQ(2, cmeta_pb.current_term());
  ASSERT_EQ(ts->uuid(), cmeta_pb.voted_for());

  // Now do the same thing as above, where we tombstone TS 0 then trigger a new
  // term (term 3) on the other machines. TS 0 will get remotely bootstrapped
  // again, but this time the vote record on TS 0 for term 2 should not be
  // retained after remote bootstrap occurs.
  cluster_->tablet_server(1)->Shutdown();
  cluster_->tablet_server(2)->Shutdown();

  // Delete with retries because the tablet might still be bootstrapping.
  ASSERT_NO_FATALS(DeleteTabletWithRetries(ts, tablet_id, TABLET_DATA_TOMBSTONED, timeout));
  ASSERT_NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id, CMETA_EXPECTED));

  ASSERT_OK(cluster_->tablet_server(1)->Restart());
  ASSERT_OK(cluster_->tablet_server(2)->Restart());
  ASSERT_NO_FATALS(WaitUntilTabletRunning(1, tablet_id));
  ASSERT_NO_FATALS(WaitUntilTabletRunning(2, tablet_id));
  ASSERT_OK(itest::StartElection(leader, tablet_id, timeout));
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_READY));

  // The election history should have been wiped out.
  ASSERT_OK(inspect_->ReadConsensusMetadataOnTS(kTsIndex, tablet_id, &cmeta_pb));
  ASSERT_EQ(3, cmeta_pb.current_term());
  ASSERT_TRUE(!cmeta_pb.has_voted_for()) << cmeta_pb.ShortDebugString();
}

// Regression test for KUDU-987, a bug where followers with transactions in
// REPLICATING state, which means they have not yet been committed to a
// majority, cannot shut down during a DeleteTablet() call.
TEST_F(DeleteTableTest, TestDeleteFollowerWithReplicatingOperation) {
  if (!AllowSlowTests()) {
    // We will typically wait at least 5 seconds for timeouts to occur.
    LOG(INFO) << "Skipping test in fast-test mode.";
    return;
  }

  const MonoDelta timeout = MonoDelta::FromSeconds(10);

  const int kNumTabletServers = 5;
  vector<string> ts_flags, master_flags;
  ts_flags.push_back("--enable_leader_failure_detection=false");
  ts_flags.push_back("--maintenance_manager_polling_interval_ms=100");
  master_flags.push_back("--catalog_manager_wait_for_new_tablets_to_elect_leader=false");
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumTabletServers));

  const int kTsIndex = 0;  // We'll test with the first TS.
  TServerDetails* ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();

  // Create the table.
  TestWorkload workload(cluster_.get());
  workload.Setup();

  // Figure out the tablet ids of the created tablets.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(WaitForNumTabletsOnTS(ts, 1, timeout, &tablets));
  const string& tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect TS 1 as leader.
  const int kLeaderIndex = 1;
  const string kLeaderUuid = cluster_->tablet_server(kLeaderIndex)->uuid();
  TServerDetails* leader = ts_map_[kLeaderUuid].get();
  ASSERT_OK(itest::StartElection(leader, tablet_id, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, 1));

  // Kill a majority, but leave the leader and a single follower.
  LOG(INFO) << "Killing majority";
  for (int i = 2; i < kNumTabletServers; i++) {
    cluster_->tablet_server(i)->Shutdown();
  }

  // Now write a single row to the leader.
  // We give 5 seconds for the timeout to pretty much guarantee that a flush
  // will occur due to the low flush threshold we set.
  LOG(INFO) << "Writing a row";
  Status s = WriteSimpleTestRow(leader, tablet_id, 1, 1, "hola, world", MonoDelta::FromSeconds(5));
  ASSERT_TRUE(s.IsTimedOut());
  ASSERT_STR_CONTAINS(s.ToString(), "timed out");

  LOG(INFO) << "Killing the leader...";
  cluster_->tablet_server(kLeaderIndex)->Shutdown();

  // Now tombstone the follower tablet. This should succeed even though there
  // are uncommitted operations on the replica.
  LOG(INFO) << "Tombstoning tablet " << tablet_id << " on TS " << ts->uuid();
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none, timeout));
}

// Verify that memtable is not flushed when tablet is deleted.
TEST_F(DeleteTableTest, TestMemtableNoFlushOnTabletDelete) {
  const MonoDelta timeout = MonoDelta::FromSeconds(10);

  const int kNumTabletServers = 1;
  vector<string> ts_flags, master_flags;
  master_flags.push_back("--replication_factor=1");
  master_flags.push_back("--yb_num_shards_per_tserver=1");
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumTabletServers));

  const int kTsIndex = 0;  // We'll test with the first TS.
  TServerDetails* ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();

  // Create the table.
  TestWorkload workload(cluster_.get());
  workload.Setup();

  // Figure out the tablet ids of the created tablets.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(WaitForNumTabletsOnTS(ts, 1, timeout, &tablets));
  const string& tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect TS 0 as leader.
  const int kLeaderIndex = 0;
  const string kLeaderUuid = cluster_->tablet_server(kLeaderIndex)->uuid();
  TServerDetails* leader = ts_map_[kLeaderUuid].get();
  ASSERT_OK(itest::StartElection(leader, tablet_id, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, 1));

  // Now write a single row to the leader.
  LOG(INFO) << "Writing a row";
  ASSERT_OK(WriteSimpleTestRow(leader, tablet_id, 1, 1, "hola, world", MonoDelta::FromSeconds(5)));

  // Set test flag to detect that memtable should not be flushed on table delete.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(kLeaderIndex),
        "TEST_rocksdb_crash_on_flush", "true"));

  // Now delete the tablet.
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_DELETED, boost::none, timeout));

  // Sleep to allow background memtable flush to be scheduled (in case).
  SleepFor(MonoDelta::FromMilliseconds(5 * 1000));

  // Unset test flag to allow other memtable flushes (if any) in teardown
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(kLeaderIndex),
        "TEST_rocksdb_crash_on_flush", "false"));
}

// Test that orphaned blocks are cleared from the superblock when a tablet is tombstoned.
TEST_F(DeleteTableTest, TestOrphanedBlocksClearedOnDelete) {
  const MonoDelta timeout = MonoDelta::FromSeconds(30);
  vector<string> ts_flags, master_flags;
  ts_flags.push_back("--enable_leader_failure_detection=false");
  // Flush quickly since we wait for a flush to occur.
  ts_flags.push_back("--maintenance_manager_polling_interval_ms=100");
  master_flags.push_back("--catalog_manager_wait_for_new_tablets_to_elect_leader=false");
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  const int kFollowerIndex = 0;
  TServerDetails* follower_ts = ts_map_[cluster_->tablet_server(kFollowerIndex)->uuid()].get();

  // Create the table.
  TestWorkload workload(cluster_.get());
  workload.Setup();

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(WaitForNumTabletsOnTS(follower_ts, 1, timeout, &tablets));
  const string& tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect TS 1 as leader.
  const int kLeaderIndex = 1;
  const string kLeaderUuid = cluster_->tablet_server(kLeaderIndex)->uuid();
  TServerDetails* leader_ts = ts_map_[kLeaderUuid].get();
  ASSERT_OK(itest::StartElection(leader_ts, tablet_id, timeout));
  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, 1));

  // Run a write workload and wait some time for the workload to add data.
  workload.Start();
  SleepFor(MonoDelta::FromMilliseconds(2000));
  ASSERT_GT(workload.rows_inserted(), 20);
  // Shut down the leader so it doesn't try to bootstrap our follower later.
  workload.StopAndJoin();
  cluster_->tablet_server(kLeaderIndex)->Shutdown();

  // Tombstone the follower and check that follower superblock is still accessible.
  ASSERT_OK(itest::DeleteTablet(follower_ts, tablet_id, TABLET_DATA_TOMBSTONED,
                                boost::none, timeout));
  ASSERT_NO_FATALS(WaitForTabletTombstonedOnTS(kFollowerIndex, tablet_id, CMETA_EXPECTED));
  RaftGroupReplicaSuperBlockPB superblock_pb;
  ASSERT_OK(inspect_->ReadTabletSuperBlockOnTS(kFollowerIndex, tablet_id, &superblock_pb));
}

vector<const string*> Grep(const string& needle, const vector<string>& haystack) {
  vector<const string*> results;
  for (const string& s : haystack) {
    if (s.find(needle) != string::npos) {
      results.push_back(&s);
    }
  }
  return results;
}

vector<string> ListOpenFiles(pid_t pid) {
  string cmd = strings::Substitute("export PATH=$$PATH:/usr/bin:/usr/sbin; lsof -n -p $0", pid);
  vector<string> argv = { "bash", "-c", cmd };
  string out;
  CHECK_OK(Subprocess::Call(argv, &out));
  vector<string> lines = strings::Split(out, "\n");
  return lines;
}

int PrintOpenTabletFiles(pid_t pid, const string& tablet_id) {
  vector<string> lines = ListOpenFiles(pid);
  vector<const string*> wal_lines = Grep(tablet_id, lines);
  LOG(INFO) << "There are " << wal_lines.size() << " open WAL files for pid " << pid << ":";
  for (const string* l : wal_lines) {
    LOG(INFO) << *l;
  }
  return wal_lines.size();
}

// Regression test for tablet deletion FD leak. See KUDU-1288.
TEST_F(DeleteTableTest, TestFDsNotLeakedOnTabletTombstone) {
  const MonoDelta timeout = MonoDelta::FromSeconds(30);

  vector<string> ts_flags, master_flags;
  master_flags.push_back("--replication_factor=1");
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, 1));

  // Create the table.
  TestWorkload workload(cluster_.get());
  workload.Setup();
  workload.Start();
  while (workload.rows_inserted() < 1000) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  workload.StopAndJoin();

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(WaitForNumTabletsOnTS(ts_map_.begin()->second.get(), 1, timeout, &tablets));
  const string& tablet_id = tablets[0].tablet_status().tablet_id();

  // Tombstone the tablet and then ensure that lsof does not list any
  // tablet-related paths.
  ExternalTabletServer* ets = cluster_->tablet_server(0);
  ASSERT_OK(itest::DeleteTablet(ts_map_[ets->uuid()].get(),
                                tablet_id, TABLET_DATA_TOMBSTONED, boost::none, timeout));
  ASSERT_EQ(0, PrintOpenTabletFiles(ets->pid(), tablet_id));

  // Restart the TS after deletion and then do the same lsof check again.
  ets->Shutdown();
  ASSERT_OK(ets->Restart());
  ASSERT_EQ(0, PrintOpenTabletFiles(ets->pid(), tablet_id));
}

// Parameterized test case for TABLET_DATA_DELETED deletions.
class DeleteTableDeletedParamTest : public DeleteTableTest,
                                    public ::testing::WithParamInterface<const char*> {
};

// Test that if a server crashes mid-delete that the delete will be rolled
// forward on startup. Parameterized by different fault flags that cause a
// crash at various points.
TEST_P(DeleteTableDeletedParamTest, TestRollForwardDelete) {
  ASSERT_NO_FATALS(StartCluster());
  const string fault_flag = GetParam();
  LOG(INFO) << "Running with fault flag: " << fault_flag;

  // Dynamically set the fault flag so they crash when DeleteTablet() is called
  // by the Master.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(i), fault_flag, "1.0"));
  }

  // Create a table on the cluster. We're just using TestWorkload
  // as a convenient way to create it.
  TestWorkload(cluster_.get()).Setup();

  // The table should have replicas on all three tservers.
  ASSERT_OK(inspect_->WaitForReplicaCount(3));

  // Delete it and wait for the tablet servers to crash.
  // The DeleteTable() call can be blocking, so it should be called in a separate thread.
  std::thread delete_table_thread([&]() {
        ASSERT_NO_FATALS(DeleteTable(TestWorkloadOptions::kDefaultTableName));
      });

  SleepFor(MonoDelta::FromMilliseconds(50));
  ASSERT_NO_FATALS(WaitForAllTSToCrash());

  // There should still be data left on disk.
  Status s = inspect_->CheckNoData();
  ASSERT_TRUE(s.IsIllegalState()) << s.ToString();

  // Now restart the tablet servers. They should roll forward their deletes.
  // We don't have to reset the fault flag here because it was set dynamically.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    cluster_->tablet_server(i)->Shutdown();
    ASSERT_OK(cluster_->tablet_server(i)->Restart());
  }

  delete_table_thread.join();
  ASSERT_OK(inspect_->WaitForNoData());
}

// Faults appropriate for the TABLET_DATA_DELETED case.
const char* deleted_faults[] = {"TEST_fault_crash_after_blocks_deleted",
                                "TEST_fault_crash_after_wal_deleted",
                                "TEST_fault_crash_after_cmeta_deleted"};

INSTANTIATE_TEST_CASE_P(FaultFlags, DeleteTableDeletedParamTest,
                        ::testing::ValuesIn(deleted_faults));

// Parameterized test case for TABLET_DATA_TOMBSTONED deletions.
class DeleteTableTombstonedParamTest : public DeleteTableTest,
                                       public ::testing::WithParamInterface<const char*> {
};

// Regression test for tablet tombstoning. Tests:
// 1. basic creation & tombstoning of a tablet.
// 2. roll-forward (crash recovery) of a partially-completed tombstoning of a tablet.
// 3. permanent deletion of a TOMBSTONED tablet
//    (transition from TABLET_DATA_TOMBSTONED to TABLET_DATA_DELETED).
TEST_P(DeleteTableTombstonedParamTest, TestTabletTombstone) {
  vector<string> flags;
  flags.push_back("--log_segment_size_mb=1");  // Faster log rolls.
  ASSERT_NO_FATALS(StartCluster(flags));
  const string fault_flag = GetParam();
  LOG(INFO) << "Running with fault flag: " << fault_flag;

  MonoDelta timeout = MonoDelta::FromSeconds(30);

  // Create a table with 2 tablets. We delete the first tablet without
  // injecting any faults, then we delete the second tablet while exercising
  // several fault injection points.
  ASSERT_OK(client_->CreateNamespaceIfNotExists(
      TestWorkloadOptions::kDefaultTableName.namespace_name(),
      TestWorkloadOptions::kDefaultTableName.namespace_type()));
  const int kNumTablets = 2;
  Schema schema(GetSimpleTestSchema());
  client::YBSchema client_schema(client::YBSchemaFromSchema(schema));
  std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_OK(table_creator->table_name(TestWorkloadOptions::kDefaultTableName)
                          .num_tablets(kNumTablets)
                          .schema(&client_schema)
                          .Create());

  // Start a workload on the cluster, and run it until we find WALs on disk.
  TestWorkload workload(cluster_.get());
  workload.Setup();

  // The table should have 2 tablets (1 split) on all 3 tservers (for a total of 6).
  ASSERT_OK(inspect_->WaitForReplicaCount(6));

  // Set up the proxies so we can easily send DeleteTablet() RPCs.
  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();

  // Ensure the tablet server is reporting 2 tablets.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(itest::WaitForNumTabletsOnTS(ts, 2, timeout, &tablets));

  // Run the workload against whoever the leader is until WALs appear on TS 0
  // for the tablets we created.
  const int kTsIndex = 0;  // Index of the tablet server we'll use for the test.
  workload.Start();
  while (workload.rows_inserted() < 100) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_OK(inspect_->WaitForMinFilesInTabletWalDirOnTS(kTsIndex,
            tablets[0].tablet_status().tablet_id(), 3));
  ASSERT_OK(inspect_->WaitForMinFilesInTabletWalDirOnTS(kTsIndex,
            tablets[1].tablet_status().tablet_id(), 3));
  workload.StopAndJoin();

  // Shut down the master and the other tablet servers so they don't interfere
  // by attempting to create tablets or remote bootstrap while we delete tablets.
  cluster_->master()->Shutdown();
  cluster_->tablet_server(1)->Shutdown();
  cluster_->tablet_server(2)->Shutdown();

  // Tombstone the first tablet.
  string tablet_id = tablets[0].tablet_status().tablet_id();
  LOG(INFO) << "Tombstoning first tablet " << tablet_id << "...";
  ASSERT_TRUE(inspect_->DoesConsensusMetaExistForTabletOnTS(kTsIndex, tablet_id)) << tablet_id;
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none, timeout));
  LOG(INFO) << "Waiting for first tablet to be tombstoned...";
  ASSERT_NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id, CMETA_EXPECTED));

  ASSERT_OK(itest::WaitForNumTabletsOnTS(ts, 2, timeout, &tablets));
  for (const ListTabletsResponsePB::StatusAndSchemaPB& t : tablets) {
    if (t.tablet_status().tablet_id() == tablet_id) {
      ASSERT_EQ(tablet::SHUTDOWN, t.tablet_status().state());
      ASSERT_EQ(TABLET_DATA_TOMBSTONED, t.tablet_status().tablet_data_state())
          << t.tablet_status().tablet_id() << " not tombstoned";
    }
  }

  // Now tombstone the 2nd tablet, causing a fault.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(kTsIndex), fault_flag, "1.0"));
  tablet_id = tablets[1].tablet_status().tablet_id();
  LOG(INFO) << "Tombstoning second tablet " << tablet_id << "...";
  ignore_result(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none, timeout));
  ASSERT_OK(cluster_->WaitForTSToCrash(kTsIndex));

  // Restart the tablet server and wait for the WALs to be deleted and for the
  // superblock to show that it is tombstoned.
  cluster_->tablet_server(kTsIndex)->Shutdown();
  // Don't start the CQL proxy, since it'll try to connect to the master.
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart(false));
  LOG(INFO) << "Waiting for second tablet to be tombstoned...";
  ASSERT_NO_FATALS(WaitForTabletTombstonedOnTS(kTsIndex, tablet_id, CMETA_EXPECTED));

  // The tombstoned tablets will still show up in ListTablets(),
  // just with their data state set as TOMBSTONED. They should also be listed
  // as NOT_STARTED because we restarted the server.
  ASSERT_OK(itest::WaitForNumTabletsOnTS(ts, 2, timeout, &tablets));
  for (const ListTabletsResponsePB::StatusAndSchemaPB& t : tablets) {
    ASSERT_EQ(tablet::NOT_STARTED, t.tablet_status().state());
    ASSERT_EQ(TABLET_DATA_TOMBSTONED, t.tablet_status().tablet_data_state())
        << t.tablet_status().tablet_id() << " not tombstoned";
  }

  // Check that, upon restart of the tablet server with a tombstoned tablet,
  // we don't unnecessary "roll forward" and rewrite the tablet metadata file
  // when it is already fully deleted.
  int64_t orig_mtime = inspect_->GetTabletSuperBlockMTimeOrDie(kTsIndex, tablet_id);
  cluster_->tablet_server(kTsIndex)->Shutdown();
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart());
  int64_t new_mtime = inspect_->GetTabletSuperBlockMTimeOrDie(kTsIndex, tablet_id);
  ASSERT_EQ(orig_mtime, new_mtime)
                << "Tablet superblock should not have been re-flushed unnecessarily";

  // Finally, delete all tablets on the TS, and wait for all data to be gone.
  LOG(INFO) << "Deleting all tablets...";
  for (const ListTabletsResponsePB::StatusAndSchemaPB& tablet : tablets) {
    string tablet_id = tablet.tablet_status().tablet_id();
    // We need retries here, since some of the tablets may still be
    // bootstrapping after being restarted above.
    ASSERT_NO_FATALS(DeleteTabletWithRetries(ts, tablet_id, TABLET_DATA_DELETED, timeout));
    ASSERT_NO_FATALS(WaitForTabletDeletedOnTS(kTsIndex, tablet_id, SUPERBLOCK_EXPECTED));
  }

  // Restart the TS, the superblock should be deleted on startup.
  cluster_->tablet_server(kTsIndex)->Shutdown();
  // Don't start the CQL proxy, since it'll try to connect to the master.
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart(false));
  ASSERT_OK(inspect_->WaitForNoDataOnTS(kTsIndex));
}

// Faults appropriate for the TABLET_DATA_TOMBSTONED case.
// Tombstoning a tablet does not delete the consensus metadata.
const char* tombstoned_faults[] = {"TEST_fault_crash_after_blocks_deleted",
                                   "TEST_fault_crash_after_wal_deleted"};

INSTANTIATE_TEST_CASE_P(FaultFlags, DeleteTableTombstonedParamTest,
                        ::testing::ValuesIn(tombstoned_faults));

}  // namespace yb
