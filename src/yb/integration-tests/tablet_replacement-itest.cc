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

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <boost/optional.hpp>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "yb/common/wire_protocol.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/external_mini_cluster-itest-base.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/test_workload.h"

using yb::consensus::RaftPeerPB;
using yb::itest::TServerDetails;
using yb::tablet::TABLET_DATA_READY;
using yb::tablet::TABLET_DATA_TOMBSTONED;
using yb::tserver::ListTabletsResponsePB;
using std::shared_ptr;
using std::string;
using std::unordered_map;
using std::vector;
using strings::Substitute;

namespace yb {

class TabletReplacementITest : public ExternalMiniClusterITestBase {
};

// Test that the Master will tombstone a newly-evicted replica.
// Then, test that the Master will NOT tombstone a newly-added replica that is
// not part of the committed config yet (only the pending config).
TEST_F(TabletReplacementITest, TestMasterTombstoneEvictedReplica) {
  MonoDelta timeout = MonoDelta::FromSeconds(30);
  vector<string> ts_flags = { "--enable_leader_failure_detection=false" };
  int num_tservers = 5;
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--replication_factor=5",
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, num_tservers));

  TestWorkload workload(cluster_.get());
  workload.Setup(); // Easy way to create a new tablet.

  const int kLeaderIndex = 0;
  TServerDetails* leader_ts = ts_map_[cluster_->tablet_server(kLeaderIndex)->uuid()].get();
  const int kFollowerIndex = 4;
  TServerDetails* follower_ts = ts_map_[cluster_->tablet_server(kFollowerIndex)->uuid()].get();

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(itest::WaitForNumTabletsOnTS(leader_ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect a leader (TS 0)
  ASSERT_OK(itest::StartElection(leader_ts, tablet_id, timeout));
  ASSERT_OK(itest::WaitForServersToAgree(timeout, ts_map_, tablet_id, 1)); // Wait for NO_OP.
  ASSERT_OK(itest::WaitUntilCommittedOpIdIndexIs(1, leader_ts, tablet_id, timeout));

  // Remove a follower from the config.
  ASSERT_OK(itest::RemoveServer(leader_ts, tablet_id, follower_ts, boost::none, timeout));

  // Wait for the Master to tombstone the replica.
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kFollowerIndex, tablet_id, TABLET_DATA_TOMBSTONED,
                                                 timeout));

  if (!AllowSlowTests()) {
    // The rest of this test has multi-second waits, so we do it in slow test mode.
    LOG(INFO) << "Not verifying that a newly-added replica won't be tombstoned in fast-test mode";
    return;
  }

  // Shut down a majority of followers (3 servers) and then try to add the
  // follower back to the config. This will cause the config change to end up
  // in a pending state.
  auto active_ts_map = CreateTabletServerMapUnowned(ts_map_);
  for (int i = 1; i <= 3; i++) {
    cluster_->tablet_server(i)->Shutdown();
    ASSERT_EQ(1, active_ts_map.erase(cluster_->tablet_server(i)->uuid()));
  }
  // This will time out, but should take effect.
  Status s = itest::AddServer(leader_ts, tablet_id, follower_ts, RaftPeerPB::PRE_VOTER,
                              boost::none, MonoDelta::FromSeconds(5), NULL,
                              false /* retry */);
  ASSERT_TRUE(s.IsTimedOut());
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kFollowerIndex, tablet_id, TABLET_DATA_READY,
                                                 timeout));
  ASSERT_OK(itest::WaitForServersToAgree(timeout, active_ts_map, tablet_id, 3));

  // Sleep for a few more seconds and check again to ensure that the Master
  // didn't end up tombstoning the replica.
  SleepFor(MonoDelta::FromSeconds(3));
  ASSERT_OK(inspect_->CheckTabletDataStateOnTS(kFollowerIndex, tablet_id, TABLET_DATA_READY));
}

// Ensure that the Master will tombstone a replica if it reports in with an old
// config. This tests a slightly different code path in the catalog manager
// than TestMasterTombstoneEvictedReplica does.
TEST_F(TabletReplacementITest, TestMasterTombstoneOldReplicaOnReport) {
  MonoDelta timeout = MonoDelta::FromSeconds(30);
  vector<string> ts_flags = { "--enable_leader_failure_detection=false" };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  TestWorkload workload(cluster_.get());
  workload.Setup(); // Easy way to create a new tablet.

  const int kLeaderIndex = 0;
  TServerDetails* leader_ts = ts_map_[cluster_->tablet_server(kLeaderIndex)->uuid()].get();
  const int kFollowerIndex = 2;
  TServerDetails* follower_ts = ts_map_[cluster_->tablet_server(kFollowerIndex)->uuid()].get();

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(itest::WaitForNumTabletsOnTS(leader_ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect a leader (TS 0)
  ASSERT_OK(itest::StartElection(leader_ts, tablet_id, timeout));
  ASSERT_OK(itest::WaitForServersToAgree(timeout, ts_map_, tablet_id, 1)); // Wait for NO_OP.
  ASSERT_OK(itest::WaitUntilCommittedOpIdIndexIs(1, leader_ts, tablet_id, timeout));

  // Shut down the follower to be removed, then remove it from the config.
  // We will wait for the Master to be notified of the config change, then shut
  // down the rest of the cluster and bring the follower back up. The follower
  // will heartbeat to the Master and then be tombstoned.
  cluster_->tablet_server(kFollowerIndex)->Shutdown();

  // Remove the follower from the config and wait for the Master to notice the
  // config change.
  ASSERT_OK(itest::RemoveServer(leader_ts, tablet_id, follower_ts, boost::none, timeout));
  ASSERT_OK(itest::WaitForNumVotersInConfigOnMaster(cluster_->master_proxy(), tablet_id, 2,
                                                    timeout));

  // Shut down the remaining tablet servers and restart the dead one.
  cluster_->tablet_server(0)->Shutdown();
  cluster_->tablet_server(1)->Shutdown();
  ASSERT_OK(cluster_->tablet_server(kFollowerIndex)->Restart());

  // Wait for the Master to tombstone the revived follower.
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kFollowerIndex, tablet_id, TABLET_DATA_TOMBSTONED,
                                                 timeout));
}

// Test that unreachable followers are evicted and replaced.
TEST_F(TabletReplacementITest, TestEvictAndReplaceDeadFollower) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in fast-test mode.";
    return;
  }

  MonoDelta timeout = MonoDelta::FromSeconds(30);
  vector<string> ts_flags = { "--enable_leader_failure_detection=false",
                              "--follower_unavailable_considered_failed_sec=5" };
  vector<string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  TestWorkload workload(cluster_.get());
  workload.Setup(); // Easy way to create a new tablet.

  const int kLeaderIndex = 0;
  TServerDetails* leader_ts = ts_map_[cluster_->tablet_server(kLeaderIndex)->uuid()].get();
  const int kFollowerIndex = 2;

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(itest::WaitForNumTabletsOnTS(leader_ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect a leader (TS 0)
  ASSERT_OK(itest::StartElection(leader_ts, tablet_id, timeout));
  ASSERT_OK(itest::WaitForServersToAgree(timeout, ts_map_, tablet_id, 1)); // Wait for NO_OP.

  // Shut down the follower to be removed. It should be evicted.
  cluster_->tablet_server(kFollowerIndex)->Shutdown();

  // With a RemoveServer and AddServer, the opid_index of the committed config will be 3.
  ASSERT_OK(itest::WaitUntilCommittedOpIdIndexIs(3,
                                                 leader_ts,
                                                 tablet_id,
                                                 timeout,
                                                 itest::CommittedEntryType::CONFIG));
  ASSERT_OK(cluster_->tablet_server(kFollowerIndex)->Restart());
}

// Regression test for KUDU-1233. This test creates a situation in which tablet
// bootstrap will attempt to replay committed (and applied) config change
// operations. This is achieved by delaying application of a write at the
// tablet level that precedes the config change operations in the WAL, then
// initiating a remote bootstrap to a follower. The follower will not have the
// COMMIT for the write operation, so will ignore COMMIT messages for the
// applied config change operations. At startup time, the newly
// remotely-bootstrapped tablet should detect that these config change
// operations have already been applied and skip them.
TEST_F(TabletReplacementITest, TestRemoteBoostrapWithPendingConfigChangeCommits) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in fast-test mode.";
    return;
  }

  MonoDelta timeout = MonoDelta::FromSeconds(30);
  vector<string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
  };
  // We will manage doing the AddServer() manually, in order to make this test
  // more deterministic.
  vector<string> master_flags = {
    "--master_tombstone_evicted_tablet_replicas=false"s,
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  TestWorkload workload(cluster_.get());
  workload.Setup(); // Convenient way to create a table.

  const int kLeaderIndex = 0;
  TServerDetails* leader_ts = ts_map_[cluster_->tablet_server(kLeaderIndex)->uuid()].get();
  const int kFollowerIndex = 2;
  TServerDetails* ts_to_remove = ts_map_[cluster_->tablet_server(kFollowerIndex)->uuid()].get();

  // Wait for tablet creation and then identify the tablet id.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(itest::WaitForNumTabletsOnTS(leader_ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect a leader (TS 0)
  ASSERT_OK(itest::StartElection(leader_ts, tablet_id, timeout));
  ASSERT_OK(itest::WaitForServersToAgree(timeout, ts_map_, tablet_id, 1)); // Wait for NO_OP.

  // Write a single row.
  ASSERT_OK(WriteSimpleTestRow(leader_ts, tablet_id, 0, 0, "", timeout));

  // Delay tablet applies in order to delay COMMIT messages to trigger KUDU-1233.
  // Then insert another row.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server_by_uuid(leader_ts->uuid()),
                              "TEST_tablet_inject_latency_on_apply_write_txn_ms", "5000"));

  // Kick off an async insert, which will be delayed for 5 seconds. This is
  // normally enough time to evict a replica, tombstone it, add it back, and
  // remotely bootstrap it when the log is only a few entries.
  tserver::WriteRequestPB req;
  tserver::WriteResponsePB resp;
  CountDownLatch latch(1);
  rpc::RpcController rpc;
  rpc.set_timeout(timeout);
  req.set_tablet_id(tablet_id);
  AddTestRowInsert(1, 1, "", &req);
  leader_ts->tserver_proxy->WriteAsync(req, &resp, &rpc, [&latch]() { latch.CountDown(); });

  // Wait for the replicate to show up (this doesn't wait for COMMIT messages).
  ASSERT_OK(itest::WaitForServersToAgree(timeout, ts_map_, tablet_id, 3));
  ASSERT_OK(itest::WaitUntilCommittedOpIdIndexIs(3, leader_ts, tablet_id, timeout));

  // Manually evict the server from the cluster, tombstone the replica, then
  // add the replica back to the cluster. Without the fix for KUDU-1233, this
  // will cause the replica to fail to start up.
  ASSERT_OK(itest::RemoveServer(leader_ts, tablet_id, ts_to_remove, boost::none, timeout));
  ASSERT_OK(itest::DeleteTablet(ts_to_remove, tablet_id, TABLET_DATA_TOMBSTONED,
                                boost::none, timeout));
  ASSERT_OK(itest::AddServer(leader_ts, tablet_id, ts_to_remove, RaftPeerPB::PRE_VOTER,
                             boost::none, timeout));
  ASSERT_OK(itest::WaitUntilTabletRunning(ts_to_remove, tablet_id, timeout));

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(),
                            ClusterVerifier::EXACTLY, 2));

  latch.Wait(); // Avoid use-after-free on the response from the delayed RPC callback.
}

} // namespace yb
