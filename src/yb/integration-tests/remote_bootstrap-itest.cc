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

#include <string>
#include <unordered_map>
#include <memory>
#include <boost/optional.hpp>
#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "yb/client/client-test-util.h"
#include "yb/client/client.h"
#include "yb/client/table_creator.h"

#include "yb/common/wire_protocol-test-util.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/cluster_itest_util.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/external_mini_cluster_fs_inspector.h"
#include "yb/integration-tests/test_workload.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tserver/remote_bootstrap_client.h"
#include "yb/tserver/remote_bootstrap_session.h"
#include "yb/util/metrics.h"
#include "yb/util/pb_util.h"
#include "yb/util/pstack_watcher.h"
#include "yb/util/test_util.h"

using namespace std::literals;

DEFINE_int32(test_delete_leader_num_iters, 3,
             "Number of iterations to run in TestDeleteLeaderDuringRemoteBootstrapStressTest.");
DEFINE_int32(test_delete_leader_min_rows_per_iter, 200,
             "Minimum number of rows to insert per iteration "
              "in TestDeleteLeaderDuringRemoteBootstrapStressTest.");
DEFINE_int32(test_delete_leader_payload_bytes, 16 * 1024,
             "Payload byte size in TestDeleteLeaderDuringRemoteBootstrapStressTest.");
DEFINE_int32(test_delete_leader_num_writer_threads, 1,
             "Number of writer threads in TestDeleteLeaderDuringRemoteBootstrapStressTest.");
DEFINE_int32(remote_bootstrap_itest_timeout_sec, 180,
             "Timeout in seconds to use in remote bootstrap integration test.");

using yb::client::YBClient;
using yb::client::YBClientBuilder;
using yb::client::YBSchema;
using yb::client::YBSchemaFromSchema;
using yb::client::YBTableCreator;
using yb::client::YBTableType;
using yb::client::YBTableName;
using std::shared_ptr;
using yb::consensus::CONSENSUS_CONFIG_COMMITTED;
using yb::consensus::RaftPeerPB;
using yb::itest::TServerDetails;
using yb::tablet::TABLET_DATA_READY;
using yb::tablet::TABLET_DATA_TOMBSTONED;
using yb::tserver::ListTabletsResponsePB;
using yb::tserver::RemoteBootstrapClient;
using std::string;
using std::unordered_map;
using std::vector;
using strings::Substitute;

METRIC_DECLARE_entity(server);
METRIC_DECLARE_histogram(handler_latency_yb_consensus_ConsensusService_UpdateConsensus);
METRIC_DECLARE_counter(glog_info_messages);
METRIC_DECLARE_counter(glog_warning_messages);
METRIC_DECLARE_counter(glog_error_messages);

namespace yb {

using yb::tablet::TabletDataState;

class RemoteBootstrapITest : public YBTest {
 public:
  void TearDown() override {
    client_.reset();
    if (HasFatalFailure()) {
      LOG(INFO) << "Found fatal failure";
      if (cluster_) {
        for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
          if (!cluster_->tablet_server(i)->IsProcessAlive()) {
            LOG(INFO) << "Tablet server " << i << " is not running. Cannot dump its stacks.";
            continue;
          }
          LOG(INFO) << "Attempting to dump stacks of TS " << i
                    << " with UUID " << cluster_->tablet_server(i)->uuid()
                    << " and pid " << cluster_->tablet_server(i)->pid();
          WARN_NOT_OK(PstackWatcher::DumpPidStacks(cluster_->tablet_server(i)->pid()),
                      "Couldn't dump stacks");
        }
      }
    } else if (check_checkpoints_cleared_ && cluster_) {
      CheckCheckpointsCleared();
    }
    if (cluster_) {
      cluster_->Shutdown();
    }
    YBTest::TearDown();
    ts_map_.clear();
  }

 protected:
  void StartCluster(const vector<string>& extra_tserver_flags = vector<string>(),
                    const vector<string>& extra_master_flags = vector<string>(),
                    int num_tablet_servers = 3);

  void RejectRogueLeader(YBTableType table_type);
  void DeleteTabletDuringRemoteBootstrap(YBTableType table_type);
  void RemoteBootstrapFollowerWithHigherTerm(YBTableType table_type);
  void ConcurrentRemoteBootstraps(YBTableType table_type);
  void DeleteLeaderDuringRemoteBootstrapStressTest(YBTableType table_type);
  void DisableRemoteBootstrap_NoTightLoopWhenTabletDeleted(YBTableType table_type);

  void CrashTestSetUp(YBTableType table_type);
  void CrashTestVerify();
  // The following tests verify that a newly added tserver gets bootstrapped even if the leader
  // crashes while bootstrapping it.
  void LeaderCrashesWhileFetchingData(YBTableType table_type);
  void LeaderCrashesBeforeChangeRole(YBTableType table_type);
  void LeaderCrashesAfterChangeRole(YBTableType table_type);

  void ClientCrashesBeforeChangeRole(YBTableType table_type);

  void StartCrashedTabletServer(
      TabletDataState expected_data_state = TabletDataState::TABLET_DATA_TOMBSTONED);
  void CheckCheckpointsCleared();

  void CreateTableAssignLeaderAndWaitForTabletServersReady(const YBTableType table_type,
                                                           const YBTableName& table_name,
                                                           const int num_tablets,
                                                           const int expected_num_tablets_per_ts,
                                                           const int leader_index,
                                                           const MonoDelta& timeout,
                                                           vector<string>* tablet_ids);

  gscoped_ptr<ExternalMiniCluster> cluster_;
  gscoped_ptr<itest::ExternalMiniClusterFsInspector> inspect_;
  std::unique_ptr<YBClient> client_;
  itest::TabletServerMap ts_map_;

  MonoDelta crash_test_timeout_;
  const MonoDelta kWaitForCrashTimeout_ = 60s;
  vector<string> crash_test_tserver_flags_;
  std::unique_ptr<TestWorkload> crash_test_workload_;
  TServerDetails* crash_test_leader_ts_ = nullptr;
  int crash_test_tserver_index_ = -1;
  int crash_test_leader_index_ = -1;
  string crash_test_tablet_id_;
  bool check_checkpoints_cleared_ = true;
};

void RemoteBootstrapITest::StartCluster(const vector<string>& extra_tserver_flags,
                                        const vector<string>& extra_master_flags,
                                        int num_tablet_servers) {
  ExternalMiniClusterOptions opts;
  opts.num_tablet_servers = num_tablet_servers;
  opts.extra_tserver_flags = extra_tserver_flags;
  opts.extra_tserver_flags.emplace_back("--remote_bootstrap_idle_timeout_ms=10000");
  opts.extra_tserver_flags.emplace_back("--never_fsync"); // fsync causes flakiness on EC2.
  if (IsTsan()) {
    opts.extra_tserver_flags.emplace_back("--leader_failure_max_missed_heartbeat_periods=20");
    opts.extra_tserver_flags.emplace_back("--remote_bootstrap_begin_session_timeout_ms=13000");
    opts.extra_tserver_flags.emplace_back("--rpc_connection_timeout_ms=10000");
  } else if (IsSanitizer()) {
    opts.extra_tserver_flags.emplace_back("--leader_failure_max_missed_heartbeat_periods=15");
    opts.extra_tserver_flags.emplace_back("--remote_bootstrap_begin_session_timeout_ms=10000");
    opts.extra_tserver_flags.emplace_back("--rpc_connection_timeout_ms=7500");
  }

  opts.extra_master_flags = extra_master_flags;
  cluster_.reset(new ExternalMiniCluster(opts));
  ASSERT_OK(cluster_->Start());
  inspect_.reset(new itest::ExternalMiniClusterFsInspector(cluster_.get()));
  ASSERT_OK(itest::CreateTabletServerMap(cluster_->master_proxy().get(),
                                         &cluster_->proxy_cache(),
                                         &ts_map_));
  client_ = ASSERT_RESULT(cluster_->CreateClient());
}

void RemoteBootstrapITest::CheckCheckpointsCleared() {
  auto* env = Env::Default();
  auto deadline = MonoTime::Now() + 10s * kTimeMultiplier;
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto tablet_server = cluster_->tablet_server(i);
    auto data_dir = tablet_server->GetFullDataDir();
    SCOPED_TRACE(Format("Index: $0", i));
    SCOPED_TRACE(Format("UUID: $0", tablet_server->uuid()));
    SCOPED_TRACE(Format("Data dir: $0", data_dir));
    auto meta_dir = FsManager::GetRaftGroupMetadataDir(data_dir);
    auto tablets = ASSERT_RESULT(env->GetChildren(meta_dir, ExcludeDots::kTrue));
    for (const auto& tablet : tablets) {
      SCOPED_TRACE(Format("Tablet: $0", tablet));
      auto metadata_path = JoinPathSegments(meta_dir, tablet);
      tablet::RaftGroupReplicaSuperBlockPB superblock;
      if (!pb_util::ReadPBContainerFromPath(env, metadata_path, &superblock).ok()) {
        // There is a race condition between this and a DeleteTablet request. So skip this tablet
        // if we cant' read the superblock.
        continue;
      }
      auto checkpoints_dir = JoinPathSegments(
          superblock.kv_store().rocksdb_dir(), tserver::RemoteBootstrapSession::kCheckpointsDir);
      ASSERT_OK(Wait([env, checkpoints_dir]() -> bool {
        if (env->FileExists(checkpoints_dir)) {
          auto checkpoints = CHECK_RESULT(env->GetChildren(checkpoints_dir, ExcludeDots::kTrue));
          if (!checkpoints.empty()) {
            LOG(INFO) << "Checkpoints: " << yb::ToString(checkpoints);
            return false;
          }
        }
        return true;
      }, deadline, "Wait checkpoints empty"));
    }
  }
}

void RemoteBootstrapITest::CrashTestSetUp(YBTableType table_type) {
  crash_test_tserver_flags_.push_back("--log_segment_size_mb=1");  // Faster log rolls.
  // Start the cluster with load balancer turned off.
  vector<string> master_flags;
  master_flags.push_back("--enable_load_balancing=false");
  master_flags.push_back("--replication_factor=4");
  ASSERT_NO_FATALS(StartCluster(crash_test_tserver_flags_, master_flags, 5));
  crash_test_tserver_index_ = 0;  // We'll test with the first TS.

  LOG(INFO) << "Started cluster";
  // We'll do a config change to remote bootstrap a replica here later. For
  // now, shut it down.
  LOG(INFO) << "Shutting down TS " << cluster_->tablet_server(crash_test_tserver_index_)->uuid();
  cluster_->tablet_server(crash_test_tserver_index_)->Shutdown();

  // Bounce the Master so it gets new tablet reports and doesn't try to assign
  // a replica to the dead TS.
  cluster_->master()->Shutdown();
  ASSERT_OK(cluster_->master()->Restart());
  ASSERT_OK(cluster_->WaitForTabletServerCount(4, crash_test_timeout_));

  // Start a workload on the cluster, and run it for a little while.
  crash_test_workload_.reset(new TestWorkload(cluster_.get()));
  crash_test_workload_->Setup();
  ASSERT_OK(inspect_->WaitForReplicaCount(4));

  vector<string> tablets = inspect_->ListTabletsOnTS(1);
  ASSERT_EQ(1, tablets.size());
  crash_test_tablet_id_ = tablets[0];

  crash_test_workload_->Start();
  crash_test_workload_->WaitInserted(100);

  // Remote bootstrap doesn't see the active WAL segment, and we need to
  // download a file to trigger the fault in this test. Due to the log index
  // chunks, that means 3 files minimum: One in-flight WAL segment, one index
  // chunk file (these files grow much more slowly than the WAL segments), and
  // one completed WAL segment.
  crash_test_leader_ts_ = nullptr;
  ASSERT_OK(FindTabletLeader(ts_map_, crash_test_tablet_id_, crash_test_timeout_,
                             &crash_test_leader_ts_));
  crash_test_leader_index_ = cluster_->tablet_server_index_by_uuid(crash_test_leader_ts_->uuid());
  ASSERT_NE(-1, crash_test_leader_index_);
  ASSERT_OK(inspect_->WaitForMinFilesInTabletWalDirOnTS(crash_test_leader_index_,
                                                        crash_test_tablet_id_, 3));
  crash_test_workload_->StopAndJoin();
}

void RemoteBootstrapITest::CrashTestVerify() {
  // Wait until the tablet has been tombstoned in TS 0. This will happen after a call to
  // rb_client->Finish() tries to ends the remote bootstrap session with the crashed leader. The
  // returned error will cause the tablet to be tombstoned by the TOMBSTONE_NOT_OK macro.
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(0, crash_test_tablet_id_,
                                                 TABLET_DATA_TOMBSTONED, crash_test_timeout_));

  // After crash_test_leader_ts_ crashes, a new leader will be elected. This new leader will detect
  // that TS 0 needs to be remote bootstrapped. Verify that this process completes successfully.
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(0, crash_test_tablet_id_,
                                                 TABLET_DATA_READY, crash_test_timeout_ * 3));
  auto dead_leader = crash_test_leader_ts_;
  LOG(INFO) << "Dead leader: " << dead_leader->ToString();
  MonoTime start_time = MonoTime::Now();
  Status status;
  TServerDetails* new_leader;
  do {
    ASSERT_OK(FindTabletLeader(ts_map_, crash_test_tablet_id_, crash_test_timeout_, &new_leader));
    status = WaitUntilCommittedConfigNumVotersIs(5, new_leader, crash_test_tablet_id_,
                                                 MonoDelta::FromSeconds(1));
    if (status.ok()) {
      break;
    }
  } while (MonoTime::Now().GetDeltaSince(start_time).ToSeconds() < crash_test_timeout_.ToSeconds());

  CHECK_OK(status);

  start_time = MonoTime::Now();
  do {
    ASSERT_OK(FindTabletLeader(ts_map_, crash_test_tablet_id_, crash_test_timeout_,
                               &crash_test_leader_ts_));

    Status s = RemoveServer(new_leader, crash_test_tablet_id_, dead_leader, boost::none,
                            MonoDelta::FromSeconds(1), NULL, false /* retry */);
    if (s.ok()) {
      break;
    }

    // Ignore the return status if the leader is not ready or if the leader changed.
    if (s.ToString().find("Leader is not ready") == string::npos &&
        s.ToString().find("is not leader of this config") == string::npos) {
      CHECK_OK(s);
    }

    SleepFor(MonoDelta::FromMilliseconds(500));
  } while (MonoTime::Now().GetDeltaSince(start_time).ToSeconds() < crash_test_timeout_.ToSeconds());

  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(4, new_leader, crash_test_tablet_id_,
                                                crash_test_timeout_));

  ClusterVerifier cluster_verifier(cluster_.get());
  // Skip cluster_verifier.CheckCluster() because it calls ListTabletServers which gets its list
  // from TSManager::GetAllDescriptors. This list includes the tserver that is in a crash loop, and
  // the check will always fail.
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(crash_test_workload_->table_name(),
                                                  ClusterVerifier::AT_LEAST,
                                                  crash_test_workload_->rows_inserted()));

  StartCrashedTabletServer();
}

void RemoteBootstrapITest::StartCrashedTabletServer(TabletDataState expected_data_state) {
  // Restore leader so it could cleanup checkpoint.
  LOG(INFO) << "Starting crashed " << crash_test_leader_index_;
  // Actually it is already stopped, calling shutdown to synchronize state.
  cluster_->tablet_server(crash_test_leader_index_)->Shutdown();
  ASSERT_OK(cluster_->tablet_server(crash_test_leader_index_)->Start());
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(
      crash_test_leader_index_, crash_test_tablet_id_, expected_data_state));
}

// If a rogue (a.k.a. zombie) leader tries to remote bootstrap a tombstoned
// tablet, make sure its term isn't older than the latest term we observed.
// If it is older, make sure we reject the request, to avoid allowing old
// leaders to create a parallel universe. This is possible because config
// change could cause nodes to move around. The term check is reasonable
// because only one node can be elected leader for a given term.
//
// A leader can "go rogue" due to a VM pause, CTRL-z, partition, etc.
void RemoteBootstrapITest::RejectRogueLeader(YBTableType table_type) {
  // This test pauses for at least 10 seconds. Only run in slow-test mode.
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in fast-test mode.";
    return;
  }

  std::vector<std::string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
  };
  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  const MonoDelta timeout = MonoDelta::FromSeconds(30);
  const int kTsIndex = 0; // We'll test with the first TS.
  TServerDetails* ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();

  TestWorkload workload(cluster_.get());
  workload.Setup(table_type);

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(WaitForNumTabletsOnTS(ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect a leader for term 1, then run some data through the cluster.
  int zombie_leader_index = 1;
  string zombie_leader_uuid = cluster_->tablet_server(zombie_leader_index)->uuid();
  ASSERT_OK(itest::StartElection(ts_map_[zombie_leader_uuid].get(), tablet_id, timeout));
  workload.Start();
  workload.WaitInserted(100);
  workload.StopAndJoin();

  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, workload.batches_completed()));

  // Come out of the blue and try to remotely bootstrap a running server while
  // specifying an old term. That running server should reject the request.
  // We are essentially masquerading as a rogue leader here.
  Status s = itest::StartRemoteBootstrap(ts, tablet_id, zombie_leader_uuid,
                                         HostPort(cluster_->tablet_server(1)->bound_rpc_addr()),
                                         0, // Say I'm from term 0.
                                         timeout);
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(), "term 0 lower than last logged term 1");

  // Now pause the actual leader so we can bring him back as a zombie later.
  ASSERT_OK(cluster_->tablet_server(zombie_leader_index)->Pause());

  // Trigger TS 2 to become leader of term 2.
  int new_leader_index = 2;
  string new_leader_uuid = cluster_->tablet_server(new_leader_index)->uuid();
  ASSERT_OK(itest::StartElection(ts_map_[new_leader_uuid].get(), tablet_id, timeout));
  ASSERT_OK(itest::WaitUntilLeader(ts_map_[new_leader_uuid].get(), tablet_id, timeout));

  auto active_ts_map = CreateTabletServerMapUnowned(ts_map_);
  ASSERT_EQ(1, active_ts_map.erase(zombie_leader_uuid));

  // Wait for the NO_OP entry from the term 2 election to propagate to the
  // remaining nodes' logs so that we are guaranteed to reject the rogue
  // leader's remote bootstrap request when we bring it back online.
  int log_index = workload.batches_completed() + 2; // 2 terms == 2 additional NO_OP entries.
  ASSERT_OK(WaitForServersToAgree(timeout, active_ts_map, tablet_id, log_index));
  // TODO: Write more rows to the new leader once KUDU-1034 is fixed.

  // Now kill the new leader and tombstone the replica on TS 0.
  cluster_->tablet_server(new_leader_index)->Shutdown();
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none, timeout));

  // Zombies!!! Resume the rogue zombie leader.
  // He should attempt to remote bootstrap TS 0 but fail.
  ASSERT_OK(cluster_->tablet_server(zombie_leader_index)->Resume());

  // Loop for a few seconds to ensure that the tablet doesn't transition to READY.
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(MonoDelta::FromSeconds(5));
  while (MonoTime::Now().ComesBefore(deadline)) {
    ASSERT_OK(itest::ListTablets(ts, timeout, &tablets));
    ASSERT_EQ(1, tablets.size());
    ASSERT_EQ(TABLET_DATA_TOMBSTONED, tablets[0].tablet_status().tablet_data_state());
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  // Force the rogue leader to step down.
  // Then, send a remote bootstrap start request from a "fake" leader that
  // sends an up-to-date term in the RB request but the actual term stored
  // in the bootstrap source's consensus metadata would still be old.
  LOG(INFO) << "Forcing rogue leader T " << tablet_id << " P " << zombie_leader_uuid
            << " to step down...";
  ASSERT_OK(itest::LeaderStepDown(ts_map_[zombie_leader_uuid].get(), tablet_id, nullptr, timeout));
  ExternalTabletServer* zombie_ets = cluster_->tablet_server(zombie_leader_index);
  // It's not necessarily part of the API but this could return faliure due to
  // rejecting the remote. We intend to make that part async though, so ignoring
  // this return value in this test.
  ignore_result(itest::StartRemoteBootstrap(ts, tablet_id, zombie_leader_uuid,
                                            HostPort(zombie_ets->bound_rpc_addr()),
                                            2, // Say I'm from term 2.
                                            timeout));

  // Wait another few seconds to be sure the remote bootstrap is rejected.
  deadline = MonoTime::Now();
  deadline.AddDelta(MonoDelta::FromSeconds(5));
  while (MonoTime::Now().ComesBefore(deadline)) {
    ASSERT_OK(itest::ListTablets(ts, timeout, &tablets));
    ASSERT_EQ(1, tablets.size());
    ASSERT_EQ(TABLET_DATA_TOMBSTONED, tablets[0].tablet_status().tablet_data_state());
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(), ClusterVerifier::EXACTLY,
      workload.rows_inserted()));
}

// Start remote bootstrap session and delete the tablet in the middle.
// It should actually be possible to complete bootstrap in such a case, because
// when a remote bootstrap session is started on the "source" server, all of
// the relevant files are either read or opened, meaning that an in-progress
// remote bootstrap can complete even after a tablet is officially "deleted" on
// the source server. This is also a regression test for KUDU-1009.

// For yugbayteDB we have modified this test. We no longer expect the remote bootstrap
// to finish successfully after the tablet has been deleted in the leader, but we do
// expect the leader to not crash.
void RemoteBootstrapITest::DeleteTabletDuringRemoteBootstrap(YBTableType table_type) {
  MonoDelta timeout = MonoDelta::FromSeconds(10);
  const int kTsIndex = 0; // We'll test with the first TS.
  ASSERT_NO_FATALS(StartCluster());

  // Populate a tablet with some data.
  TestWorkload workload(cluster_.get());
  workload.Setup(table_type);
  workload.Start();
  workload.WaitInserted(1000);

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  TServerDetails* ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();
  ASSERT_OK(WaitForNumTabletsOnTS(ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Ensure all the servers agree before we proceed.
  workload.StopAndJoin();
  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, workload.batches_completed()));

  // Set up an FsManager to use with the RemoteBootstrapClient.
  FsManagerOpts opts;
  string testbase = GetTestPath("fake-ts");
  ASSERT_OK(env_->CreateDir(testbase));
  opts.wal_paths.push_back(JoinPathSegments(testbase, "wals"));
  opts.data_paths.push_back(JoinPathSegments(testbase, "data-0"));
  opts.server_type = "tserver_test";
  gscoped_ptr<FsManager> fs_manager(new FsManager(env_.get(), opts));
  ASSERT_OK(fs_manager->CreateInitialFileSystemLayout());
  ASSERT_OK(fs_manager->Open());

  // Start up a RemoteBootstrapClient and open a remote bootstrap session.
  RemoteBootstrapClient rb_client(tablet_id, fs_manager.get());
  scoped_refptr<tablet::RaftGroupMetadata> meta;
  ASSERT_OK(rb_client.Start(cluster_->tablet_server(kTsIndex)->uuid(),
                            &cluster_->proxy_cache(),
                            cluster_->tablet_server(kTsIndex)->bound_rpc_hostport(),
                            &meta));

  // Tombstone the tablet on the remote!
  ASSERT_OK(itest::DeleteTablet(ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none, timeout));

  // Now finish bootstrapping!
  tablet::TabletStatusListener listener(meta);

  // This will fail because the leader won't have any rocksdb files since they were deleted when
  // we called DeleteTablet.
  ASSERT_NOK(rb_client.FetchAll(&listener));
  ASSERT_OK(rb_client.EndRemoteSession());
  ASSERT_OK(rb_client.Remove());

  SleepFor(MonoDelta::FromMilliseconds(500));  // Give a little time for a crash (KUDU-1009).
  ASSERT_TRUE(cluster_->tablet_server(kTsIndex)->IsProcessAlive());

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(), ClusterVerifier::EXACTLY,
      workload.rows_inserted()));
}

TEST_F(RemoteBootstrapITest, IncompleteWALDownloadDoesntCauseCrash) {
  std::vector<std::string> master_flags = {
      "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
      "--enable_load_balancing=false"s,
      "--use_create_table_leader_hint=false"s,
  };

  int constexpr kBootstrapIdleTimeoutMs = 5000;

  std::vector<std::string> ts_flags = {
      "--log_min_segments_to_retain=1000"s,
      "--log_min_seconds_to_retain=900"s,
      "--log_segment_size_bytes=65536"s,
      "--enable_leader_failure_detection=false"s,
      // Disable pre-elections since we wait for term to become 2,
      // that does not happen with pre-elections.
      "--use_preelection=false"s,
      "--memstore_size_mb=1"s,
      "--TEST_download_partial_wal_segments=true"s,
      "--remote_bootstrap_idle_timeout_ms="s + std::to_string(kBootstrapIdleTimeoutMs)
  };

  const int kNumTabletServers = 3;
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumTabletServers));

  // Elect a leader for term 1, then run some data through the cluster.
  const int kLeaderIndex = 1;

  constexpr int kNumTablets = RegularBuildVsSanitizers(10, 2);
  MonoDelta timeout = MonoDelta::FromSeconds(RegularBuildVsSanitizers(15, 45));

  vector<string> tablet_ids;

  CreateTableAssignLeaderAndWaitForTabletServersReady(YBTableType::YQL_TABLE_TYPE,
      TestWorkloadOptions::kDefaultTableName, kNumTablets, kNumTablets, kLeaderIndex, timeout,
      &tablet_ids);

  const int kFollowerIndex = 0;
  TServerDetails* follower_ts = ts_map_[cluster_->tablet_server(kFollowerIndex)->uuid()].get();

  TestWorkload workload(cluster_.get());
  workload.set_write_timeout_millis(10000);
  workload.set_timeout_allowed(true);
  workload.set_write_batch_size(10);
  workload.set_num_write_threads(10);
  workload.Setup(YBTableType::YQL_TABLE_TYPE);
  workload.Start();
  workload.WaitInserted(RegularBuildVsSanitizers(500, 5000));
  workload.StopAndJoin();

  // Ensure all the servers agree before we proceed.
  for (const auto& tablet_id : tablet_ids) {
    ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, 1));
  }
  LOG(INFO) << "All servers agree";
  // Now tombstone the follower.
  ASSERT_OK(itest::DeleteTablet(follower_ts, tablet_ids[0], TABLET_DATA_TOMBSTONED, boost::none,
                                timeout));
  LOG(INFO) << "Successfully sent delete request " << tablet_ids[0] << " on TS " << kFollowerIndex;
  // Wait until the tablet has been tombstoned on the follower.
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kFollowerIndex, tablet_ids[0],
                                                 tablet::TABLET_DATA_TOMBSTONED, timeout));
  LOG(INFO) << "Tablet state on TS " << kFollowerIndex << " is TABLET_DATA_TOMBSTONED";
  SleepFor(MonoDelta::FromMilliseconds(5000));  // Give a little time for a crash.
  ASSERT_TRUE(cluster_->tablet_server(kFollowerIndex)->IsProcessAlive());
  LOG(INFO) << "TS " << kFollowerIndex << " didn't crash";
  // Now remove the crash flag, so the next replay will complete.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(kFollowerIndex),
                              "TEST_download_partial_wal_segments", "false"));
  LOG(INFO) << "Successfully turned off flag TEST_download_partial_wal_segments";
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kFollowerIndex, tablet_ids[0],
                                                 tablet::TABLET_DATA_READY, timeout * 4));
  LOG(INFO) << "Tablet " << tablet_ids[0]
            << " is in state TABLET_DATA_READY in TS " << kFollowerIndex;
  TServerDetails* leader_ts = ts_map_[cluster_->tablet_server(kLeaderIndex)->uuid()].get();
  OpIdPB op_id;
  ASSERT_OK(GetLastOpIdForReplica(tablet_ids[0], leader_ts,
                                  consensus::COMMITTED_OPID, timeout,
                                  &op_id));

  auto expected_index = op_id.index();

  ASSERT_OK(WaitForServersToAgree(timeout,
                                  ts_map_,
                                  tablet_ids[0],
                                  expected_index,
                                  &expected_index));

  LOG(INFO) << "Op id index in TS " << kFollowerIndex << " is " << op_id.index()
            << " for tablet " << tablet_ids[0];

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(), ClusterVerifier::EXACTLY,
                                                  workload.rows_inserted()));

  LOG(INFO) << "Cluster verifier succeeded";

  // Sleep to make sure all the remote bootstrap sessions that were initiated but
  // not completed are expired.
  SleepFor(MonoDelta::FromMilliseconds(kBootstrapIdleTimeoutMs * 2));
}

// This test ensures that a leader can remote-bootstrap a tombstoned replica
// that has a higher term recorded in the replica's consensus metadata if the
// replica's last-logged opid has the same term (or less) as the leader serving
// as the remote bootstrap source. When a tablet is tombstoned, its last-logged
// opid is stored in a field its on-disk superblock.
void RemoteBootstrapITest::RemoteBootstrapFollowerWithHigherTerm(YBTableType table_type) {
  std::vector<std::string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    // Disable pre-elections since we wait for term to become 2,
    // that does not happen with pre-elections
    "--use_preelection=false"s
  };

  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--replication_factor=2"s,
    "--use_create_table_leader_hint=false"s,
  };

  const int kNumTabletServers = 2;
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumTabletServers));

  const MonoDelta timeout = MonoDelta::FromSeconds(30);
  const int kFollowerIndex = 0;
  TServerDetails* follower_ts = ts_map_[cluster_->tablet_server(kFollowerIndex)->uuid()].get();

  TestWorkload workload(cluster_.get());
  workload.Setup(table_type);

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(WaitForNumTabletsOnTS(follower_ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect a leader for term 1, then run some data through the cluster.
  const int kLeaderIndex = 1;
  TServerDetails* leader_ts = ts_map_[cluster_->tablet_server(kLeaderIndex)->uuid()].get();
  ASSERT_OK(itest::StartElection(leader_ts, tablet_id, timeout));
  workload.Start();
  workload.WaitInserted(100);
  workload.StopAndJoin();

  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, workload.batches_completed()));

  // Pause the leader and increment the term on the follower by starting an
  // election on the follower. The election will fail asynchronously but we
  // just wait until we see that its term has incremented.
  ASSERT_OK(cluster_->tablet_server(kLeaderIndex)->Pause());
  ASSERT_OK(itest::StartElection(
      follower_ts, tablet_id, timeout, consensus::TEST_SuppressVoteRequest::kTrue));
  int64_t term = 0;
  for (int i = 0; i < 1000; i++) {
    consensus::ConsensusStatePB cstate;
    ASSERT_OK(itest::GetConsensusState(follower_ts, tablet_id, CONSENSUS_CONFIG_COMMITTED,
                                       timeout, &cstate));
    term = cstate.current_term();
    if (term == 2) break;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  ASSERT_EQ(2, term);

  // Now tombstone the follower.
  ASSERT_OK(itest::DeleteTablet(follower_ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none,
                                timeout));

  // Wait until the tablet has been tombstoned on the follower.
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kFollowerIndex, tablet_id,
                                                 tablet::TABLET_DATA_TOMBSTONED, timeout));

  // Now wake the leader. It should detect that the follower needs to be
  // remotely bootstrapped and proceed to bring it back up to date.
  ASSERT_OK(cluster_->tablet_server(kLeaderIndex)->Resume());

  // Wait for remote bootstrap to complete successfully.
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kFollowerIndex, tablet_id,
                                                 tablet::TABLET_DATA_READY, timeout));

  // Wait for the follower to come back up.
  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, workload.batches_completed()));

  ClusterVerifier cluster_verifier(cluster_.get());
  // During this test we disable leader failure detection.
  // So we use CONSISTENT_PREFIX for verification because it could end up w/o leader at all. We also
  // don't call CheckCluster for this reason.
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(
      workload.table_name(), ClusterVerifier::EXACTLY, workload.rows_inserted(),
      YBConsistencyLevel::CONSISTENT_PREFIX));
}

void RemoteBootstrapITest::CreateTableAssignLeaderAndWaitForTabletServersReady(
    const YBTableType table_type, const YBTableName& table_name, const int num_tablets,
    const int expected_num_tablets_per_ts, const int leader_index, const MonoDelta& timeout,
    vector<string>* tablet_ids) {

  ASSERT_OK(client_->CreateNamespaceIfNotExists(table_name.namespace_name()));

  // Create a table with several tablets. These will all be simultaneously
  // remotely bootstrapped to a single target node from the same leader host.
  YBSchema client_schema(YBSchemaFromSchema(GetSimpleTestSchema()));
  std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_OK(table_creator->table_name(table_name)
                .num_tablets(num_tablets)
                .schema(&client_schema)
                .table_type(table_type)
                .Create());

  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();

  // Figure out the tablet ids of the created tablets.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(WaitForNumTabletsOnTS(ts, expected_num_tablets_per_ts, timeout, &tablets));

  for (const ListTabletsResponsePB::StatusAndSchemaPB& t : tablets) {
    tablet_ids->push_back(t.tablet_status().tablet_id());
  }

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    for (const string& tablet_id : *tablet_ids) {
      ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                              tablet_id, timeout));
    }
  }

  // Elect leaders on each tablet for term 1. All leaders will be on TS leader_index.
  const string kLeaderUuid = cluster_->tablet_server(leader_index)->uuid();
  for (const string& tablet_id : *tablet_ids) {
    ASSERT_OK(itest::StartElection(ts_map_[kLeaderUuid].get(), tablet_id, timeout));
  }

  for (const string& tablet_id : *tablet_ids) {
    TServerDetails* leader_ts = nullptr;
    ASSERT_OK(FindTabletLeader(ts_map_, tablet_id, timeout, &leader_ts));
    ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(3, leader_ts, tablet_id, timeout));
  }
}

// Test that multiple concurrent remote bootstraps do not cause problems.
// This is a regression test for KUDU-951, in which concurrent sessions on
// multiple tablets between the same remote bootstrap client host and remote
// bootstrap source host could corrupt each other.
void RemoteBootstrapITest::ConcurrentRemoteBootstraps(YBTableType table_type) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in fast-test mode.";
    return;
  }

  std::vector<std::string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    "--log_cache_size_limit_mb=1"s,
    "--log_segment_size_mb=1"s,
    "--log_async_preallocate_segments=false"s,
    "--log_min_segments_to_retain=100"s,
    "--maintenance_manager_polling_interval_ms=10"s,
  };
  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--enable_load_balancing=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  const int kNumTablets = 10;
  const int kLeaderIndex = 1;
  const MonoDelta timeout = MonoDelta::FromSeconds(FLAGS_remote_bootstrap_itest_timeout_sec);
  vector<string> tablet_ids;

  CreateTableAssignLeaderAndWaitForTabletServersReady(table_type,
      TestWorkloadOptions::kDefaultTableName, kNumTablets, kNumTablets, kLeaderIndex, timeout,
      &tablet_ids);

  TestWorkload workload(cluster_.get());
  workload.set_write_timeout_millis(10000);
  workload.set_timeout_allowed(true);
  workload.set_write_batch_size(10);
  workload.set_num_write_threads(10);
  workload.Setup(table_type);
  workload.Start();
  workload.WaitInserted(20000);
  workload.StopAndJoin();

  for (const string& tablet_id : tablet_ids) {
    ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, 1));
  }

  // Now pause the leader so we can tombstone the tablets.
  ASSERT_OK(cluster_->tablet_server(kLeaderIndex)->Pause());

  const int kTsIndex = 0; // We'll test with the first TS.
  TServerDetails* target_ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();

  for (const string& tablet_id : tablet_ids) {
    LOG(INFO) << "Tombstoning tablet " << tablet_id << " on TS " << target_ts->uuid();
    ASSERT_OK(itest::DeleteTablet(target_ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none,
                                  MonoDelta::FromSeconds(10)));
  }

  // Unpause the leader TS and wait for it to remotely bootstrap the tombstoned
  // tablets, in parallel.
  ASSERT_OK(cluster_->tablet_server(kLeaderIndex)->Resume());
  for (const string& tablet_id : tablet_ids) {
    ASSERT_OK(itest::WaitUntilTabletRunning(target_ts, tablet_id, timeout));
  }

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(), ClusterVerifier::AT_LEAST,
                            workload.rows_inserted()));
}

TEST_F(RemoteBootstrapITest, TestLimitNumberOfConcurrentRemoteBootstraps) {
  constexpr int kMaxConcurrentTabletRemoteBootstrapSessions = 5;
  constexpr int kMaxConcurrentTabletRemoteBootstrapSessionsPerTable = 2;

  vector<string> ts_flags;
  int follower_considered_failed_sec;
  follower_considered_failed_sec = 10;
  ts_flags.push_back("--follower_unavailable_considered_failed_sec="+
                     std::to_string(follower_considered_failed_sec));
  ts_flags.push_back("--heartbeat_interval_ms=100");
  ts_flags.push_back("--enable_leader_failure_detection=false");
  ts_flags.push_back("--TEST_crash_if_remote_bootstrap_sessions_greater_than=" +
      std::to_string(kMaxConcurrentTabletRemoteBootstrapSessions + 1));
  ts_flags.push_back("--TEST_crash_if_remote_bootstrap_sessions_per_table_greater_than=" +
      std::to_string(kMaxConcurrentTabletRemoteBootstrapSessionsPerTable + 1));
  ts_flags.push_back("--TEST_simulate_long_remote_bootstrap_sec=5");

  std::vector<std::string> master_flags = {
    "--TEST_load_balancer_handle_under_replicated_tablets_only=true"s,
    "--load_balancer_max_concurrent_tablet_remote_bootstraps=" +
        std::to_string(kMaxConcurrentTabletRemoteBootstrapSessions),
    "--load_balancer_max_concurrent_tablet_remote_bootstraps_per_table=" +
        std::to_string(kMaxConcurrentTabletRemoteBootstrapSessionsPerTable),
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    // This value has to be less than follower_considered_failed_sec.
    "--tserver_unresponsive_timeout_ms=8000"s,
    "--use_create_table_leader_hint=false"s,
  };

  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  const MonoDelta timeout = MonoDelta::FromSeconds(FLAGS_remote_bootstrap_itest_timeout_sec);
  const int kLeaderIndex = 1;
  const int kNumberTables = 3;
  const int kNumTablets = 8;
  vector<vector<string>> tablet_ids;

  for (int i = 0; i < kNumberTables; i++) {
    tablet_ids.push_back(vector<string>());
    std::string table_name_str = "table_test_" + std::to_string(i);
    std::string keyspace_name_str = "keyspace_test_" + std::to_string(i);
    YBTableName table_name(YQL_DATABASE_CQL, keyspace_name_str, table_name_str);
    CreateTableAssignLeaderAndWaitForTabletServersReady(YBTableType::YQL_TABLE_TYPE,
        table_name, kNumTablets, (i + 1) * kNumTablets, kLeaderIndex, timeout, &(tablet_ids[i]));
  }

  const int kTsIndex = 0; // We'll test with the first TS.

  // Now pause the first tserver so that it gets removed from the configuration for all of the
  // tablets.
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Pause());
  LOG(INFO) << "Paused tserver " << cluster_->tablet_server(kTsIndex)->uuid();

  // Sleep for longer than FLAGS_follower_unavailable_considered_failed_sec to guarantee that the
  // other peers in the config for each tablet removes this tserver from the raft config.
  int total_time_slept = 0;
  while (total_time_slept < follower_considered_failed_sec * 2) {
    SleepFor(MonoDelta::FromSeconds(1));
    total_time_slept++;
    LOG(INFO) << "Total time slept " << total_time_slept;
  }


  // Resume the tserver. The cluster balancer will ensure that all the tablets are added back to
  // this tserver, and it will cause the leader to start remote bootstrap sessions for all of the
  // tablets. FLAGS_TEST_crash_if_remote_bootstrap_sessions_greater_than will make sure that we
  // never have more than the expected number of concurrent remote bootstrap sessions.
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Resume());

  LOG(INFO) << "Tserver " << cluster_->tablet_server(kTsIndex)->uuid() << " resumed";

  // Wait until the config for all the tablets have three voters. This means that the tserver that
  // we just resumed was remote bootstrapped correctly.
  for (int i = 0; i < kNumberTables; i++) {
    for (const string& tablet_id : tablet_ids[i]) {
        TServerDetails* leader_ts = nullptr;
        ASSERT_OK(FindTabletLeader(ts_map_, tablet_id, timeout, &leader_ts));
        LOG(INFO) << "Waiting until config has 3 voters for tablet " << tablet_id;
        ASSERT_OK(itest::WaitUntilCommittedConfigNumVotersIs(3, leader_ts, tablet_id, timeout));
      }
  }
}

// Test that repeatedly runs a load, tombstones a follower, then tombstones the
// leader while the follower is remotely bootstrapping. Regression test for
// KUDU-1047.
void RemoteBootstrapITest::DeleteLeaderDuringRemoteBootstrapStressTest(YBTableType table_type) {
  // This test takes a while due to failure detection.
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test in fast-test mode.";
    return;
  }

  const MonoDelta timeout = MonoDelta::FromSeconds(FLAGS_remote_bootstrap_itest_timeout_sec);
  vector<string> master_flags;
  master_flags.push_back("--replication_factor=5");
  ASSERT_NO_FATALS(StartCluster(vector<string>(), master_flags, 5));

  TestWorkload workload(cluster_.get());
  workload.set_payload_bytes(FLAGS_test_delete_leader_payload_bytes);
  workload.set_num_write_threads(FLAGS_test_delete_leader_num_writer_threads);
  workload.set_write_batch_size(1);
  workload.set_write_timeout_millis(10000);
  workload.set_timeout_allowed(true);
  workload.set_not_found_allowed(true);
  workload.Setup(table_type);

  // Figure out the tablet id.
  const int kTsIndex = 0;
  TServerDetails* ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ASSERT_OK(WaitForNumTabletsOnTS(ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  int leader_index = -1;
  int follower_index = -1;
  TServerDetails* leader_ts = nullptr;
  TServerDetails* follower_ts = nullptr;

  for (int i = 0; i < FLAGS_test_delete_leader_num_iters; i++) {
    LOG(INFO) << "Iteration " << (i + 1);
    int rows_previously_inserted = workload.rows_inserted();

    // Find out who's leader.
    ASSERT_OK(FindTabletLeader(ts_map_, tablet_id, timeout, &leader_ts));
    leader_index = cluster_->tablet_server_index_by_uuid(leader_ts->uuid());

    // Select an arbitrary follower.
    follower_index = (leader_index + 1) % cluster_->num_tablet_servers();
    follower_ts = ts_map_[cluster_->tablet_server(follower_index)->uuid()].get();

    // Spin up the workload.
    workload.Start();
    while (workload.rows_inserted() < rows_previously_inserted +
                                      FLAGS_test_delete_leader_min_rows_per_iter) {
      SleepFor(MonoDelta::FromMilliseconds(10));
    }

    // Tombstone the follower.
    LOG(INFO) << "Tombstoning follower tablet " << tablet_id << " on TS " << follower_ts->uuid();
    ASSERT_OK(itest::DeleteTablet(follower_ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none,
                                  timeout));

    // Wait for remote bootstrap to start.
    // ENG-81: There is a frequent race condition here: if the bootstrap happens too quickly, we can
    // see TABLET_DATA_READY right away without seeing TABLET_DATA_COPYING first (at last that's a
    // working hypothesis of an explanation). In an attempt to remedy this, we have increased the
    // number of rows inserted per iteration from 20 to 200.
    ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(follower_index, tablet_id,
                                                   tablet::TABLET_DATA_COPYING, timeout));

    // Tombstone the leader.
    LOG(INFO) << "Tombstoning leader tablet " << tablet_id << " on TS " << leader_ts->uuid();
    ASSERT_OK(itest::DeleteTablet(leader_ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none,
                                  timeout));

    // Quiesce and rebuild to full strength. This involves electing a new
    // leader from the remaining three, which requires a unanimous vote, and
    // that leader then remotely bootstrapping the old leader.
    workload.StopAndJoin();
    ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, 1));
  }

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(), ClusterVerifier::AT_LEAST,
                            workload.rows_inserted()));
}

namespace {
int64_t CountUpdateConsensusCalls(ExternalTabletServer* ets, const string& tablet_id) {
  return CHECK_RESULT(ets->GetInt64Metric(
      &METRIC_ENTITY_server,
      "yb.tabletserver",
      &METRIC_handler_latency_yb_consensus_ConsensusService_UpdateConsensus,
      "total_count"));
}
int64_t CountLogMessages(ExternalTabletServer* ets) {
  int64_t total = 0;

  total += CHECK_RESULT(ets->GetInt64Metric(
      &METRIC_ENTITY_server,
      "yb.tabletserver",
      &METRIC_glog_info_messages,
      "value"));

  total += CHECK_RESULT(ets->GetInt64Metric(
      &METRIC_ENTITY_server,
      "yb.tabletserver",
      &METRIC_glog_warning_messages,
      "value"));

  total += CHECK_RESULT(ets->GetInt64Metric(
      &METRIC_ENTITY_server,
      "yb.tabletserver",
      &METRIC_glog_error_messages,
      "value"));

  return total;
}
} // anonymous namespace

// Test that if remote bootstrap is disabled by a flag, we don't get into
// tight loops after a tablet is deleted. This is a regression test for situation
// similar to the bug described in KUDU-821: we were previously handling a missing
// tablet within consensus in such a way that we'd immediately send another RPC.
void RemoteBootstrapITest::DisableRemoteBootstrap_NoTightLoopWhenTabletDeleted(
    YBTableType table_type) {

  MonoDelta timeout = MonoDelta::FromSeconds(10);
  std::vector<string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
    "--TEST_enable_remote_bootstrap=false"s,
    "--rpc_slow_query_threshold_ms=10000000"s,
  };
  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  TestWorkload workload(cluster_.get());
  // TODO(KUDU-1054): the client should handle retrying on different replicas
  // if the tablet isn't found, rather than giving us this error.
  workload.set_not_found_allowed(true);
  workload.set_write_batch_size(1);
  workload.Setup(table_type);

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  ExternalTabletServer* replica_ets = cluster_->tablet_server(1);
  TServerDetails* replica_ts = ts_map_[replica_ets->uuid()].get();
  ASSERT_OK(WaitForNumTabletsOnTS(replica_ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  // Wait until all replicas are up and running.
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  // Elect a leader (TS 0)
  ExternalTabletServer* leader_ts = cluster_->tablet_server(0);
  ASSERT_OK(itest::StartElection(ts_map_[leader_ts->uuid()].get(), tablet_id, timeout));

  // Start writing, wait for some rows to be inserted.
  workload.Start();
  workload.WaitInserted(100);

  // Tombstone the tablet on one of the servers (TS 1)
  ASSERT_OK(itest::DeleteTablet(replica_ts, tablet_id, TABLET_DATA_TOMBSTONED, boost::none,
                                timeout));

  // Ensure that, if we sleep for a second while still doing writes to the leader:
  // a) we don't spew logs on the leader side
  // b) we don't get hit with a lot of UpdateConsensus calls on the replica.
  int64_t num_update_rpcs_initial = CountUpdateConsensusCalls(replica_ets, tablet_id);
  int64_t num_logs_initial = CountLogMessages(leader_ts);

  SleepFor(MonoDelta::FromSeconds(1));
  int64_t num_update_rpcs_after_sleep = CountUpdateConsensusCalls(replica_ets, tablet_id);
  int64_t num_logs_after_sleep = CountLogMessages(leader_ts);

  // Calculate rate per second of RPCs and log messages
  int64_t update_rpcs_per_second = num_update_rpcs_after_sleep - num_update_rpcs_initial;
  EXPECT_LT(update_rpcs_per_second, 20);
  int64_t num_logs_per_second = num_logs_after_sleep - num_logs_initial;
  EXPECT_LT(num_logs_per_second, 20);
}

void RemoteBootstrapITest::LeaderCrashesWhileFetchingData(YBTableType table_type) {
  crash_test_timeout_ = MonoDelta::FromSeconds(40);
  CrashTestSetUp(table_type);

  // Cause the leader to crash when a follower tries to fetch data from it.
  const string& fault_flag = "TEST_fault_crash_on_handle_rb_fetch_data";
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(crash_test_leader_index_), fault_flag,
                              "1.0"));

  // Add our TS 0 to the config and wait for the leader to crash.
  ASSERT_OK(cluster_->tablet_server(crash_test_tserver_index_)->Restart());
  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();

  ASSERT_OK(itest::AddServer(crash_test_leader_ts_, crash_test_tablet_id_, ts,
                             RaftPeerPB::PRE_VOTER, boost::none, crash_test_timeout_,
                             NULL /* error code */,
                             true /* retry */));

  ASSERT_OK(cluster_->WaitForTSToCrash(crash_test_leader_index_, kWaitForCrashTimeout_));

  CrashTestVerify();
}

void RemoteBootstrapITest::LeaderCrashesBeforeChangeRole(YBTableType table_type) {
  // Make the tablet server sleep in LogAndTombstone after it has called DeleteTabletData so we can
  // verify that the tablet has been tombstoned (by calling WaitForTabletDataStateOnTs).
  crash_test_tserver_flags_.push_back("--TEST_sleep_after_tombstoning_tablet_secs=5");
  crash_test_timeout_ = MonoDelta::FromSeconds(40);
  CrashTestSetUp(table_type);

  // Cause the leader to crash when the follower ends the remote bootstrap session and just before
  // the leader is about to change the role of the follower.
  const string& fault_flag = "TEST_fault_crash_leader_before_changing_role";
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(crash_test_leader_index_), fault_flag,
                              "1.0"));

  // Add our TS 0 to the config and wait for the leader to crash.
  ASSERT_OK(cluster_->tablet_server(crash_test_tserver_index_)->Restart());
  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();
  ASSERT_OK(itest::AddServer(crash_test_leader_ts_, crash_test_tablet_id_, ts,
                             RaftPeerPB::PRE_VOTER, boost::none, crash_test_timeout_));
  ASSERT_OK(cluster_->WaitForTSToCrash(crash_test_leader_index_, kWaitForCrashTimeout_));
  CrashTestVerify();
}

void RemoteBootstrapITest::LeaderCrashesAfterChangeRole(YBTableType table_type) {
  // Make the tablet server sleep in LogAndTombstone after it has called DeleteTabletData so we can
  // verify that the tablet has been tombstoned (by calling WaitForTabletDataStateOnTs).
  crash_test_tserver_flags_.push_back("--TEST_sleep_after_tombstoning_tablet_secs=5");
  crash_test_timeout_ = MonoDelta::FromSeconds(40);
  CrashTestSetUp(table_type);

  // Cause the leader to crash after it has successfully sent a ChangeConfig CHANGE_ROLE request and
  // before it responds to the EndRemoteBootstrapSession request.
  const string& fault_flag = "TEST_fault_crash_leader_after_changing_role";
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(crash_test_leader_index_), fault_flag,
                              "1.0"));

  // Add our TS 0 to the config and wait for the leader to crash.
  ASSERT_OK(cluster_->tablet_server(crash_test_tserver_index_)->Restart());
  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();
  ASSERT_OK(itest::AddServer(crash_test_leader_ts_, crash_test_tablet_id_, ts,
                             RaftPeerPB::PRE_VOTER, boost::none, crash_test_timeout_));
  ASSERT_OK(cluster_->WaitForTSToCrash(crash_test_leader_index_, kWaitForCrashTimeout_));

  CrashTestVerify();
}

void RemoteBootstrapITest::ClientCrashesBeforeChangeRole(YBTableType table_type) {
  crash_test_timeout_ = MonoDelta::FromSeconds(40);
  crash_test_tserver_flags_.push_back("--TEST_return_error_on_change_config=0.60");
  CrashTestSetUp(table_type);

  // Add our TS 0 to the config and wait for it to crash.
  ASSERT_OK(cluster_->tablet_server(crash_test_tserver_index_)->Restart());
  // Cause the newly added tserver to crash after the transfer of files for remote bootstrap has
  // completed but before ending the session with the leader to avoid triggering a ChangeConfig
  // in the leader.
  const string& fault_flag = "TEST_fault_crash_bootstrap_client_before_changing_role";
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(crash_test_tserver_index_), fault_flag,
                              "1.0"));

  TServerDetails* ts = ts_map_[cluster_->tablet_server(crash_test_tserver_index_)->uuid()].get();
  ASSERT_OK(itest::AddServer(crash_test_leader_ts_, crash_test_tablet_id_, ts,
                             RaftPeerPB::PRE_VOTER, boost::none, crash_test_timeout_));

  ASSERT_OK(cluster_->WaitForTSToCrash(crash_test_tserver_index_, kWaitForCrashTimeout_));

  LOG(INFO) << "Restarting TS " << cluster_->tablet_server(crash_test_tserver_index_)->uuid();
  cluster_->tablet_server(crash_test_tserver_index_)->Shutdown();
  ASSERT_OK(cluster_->tablet_server(crash_test_tserver_index_)->Restart());

  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(crash_test_tserver_index_, crash_test_tablet_id_,
                                                 TABLET_DATA_READY, crash_test_timeout_));

  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(5, crash_test_leader_ts_, crash_test_tablet_id_,
                                                crash_test_timeout_));

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(crash_test_workload_->table_name(),
                                                  ClusterVerifier::AT_LEAST,
                                                  crash_test_workload_->rows_inserted()));


  // We know that the remote bootstrap checkpoint on the leader hasn't been cleared because it
  // hasn't restarted. So we don't want to check that it's empty.
  check_checkpoints_cleared_ = false;
}

TEST_F(RemoteBootstrapITest, TestVeryLongRemoteBootstrap) {
  vector<string> ts_flags, master_flags;

  // Make everything happen 100x faster:
  //  - follower_unavailable_considered_failed_sec from 300 to 3 secs
  //  - raft_heartbeat_interval_ms from 500 to 5 ms
  //  - consensus_rpc_timeout_ms from 3000 to 30 ms

  ts_flags.push_back("--follower_unavailable_considered_failed_sec=3");
  ts_flags.push_back("--raft_heartbeat_interval_ms=5");
  ts_flags.push_back("--consensus_rpc_timeout_ms=30");

  // Increase the number of missed heartbeats used to detect leader failure since in slow testing
  // instances it is very easy to miss the default (6) heartbeats since they are being sent very
  // fast (5ms).
  ts_flags.push_back("--leader_failure_max_missed_heartbeat_periods=40.0");

  // Make the remote bootstrap take longer than follower_unavailable_considered_failed_sec seconds
  // so the peer gets removed from the config while it is being remote bootstrapped.
  ts_flags.push_back("--TEST_simulate_long_remote_bootstrap_sec=5");

  master_flags.push_back("--enable_load_balancing=false");

  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, 4));

  // We'll do a config change to remote bootstrap a replica here later. For now, shut it down.
  auto constexpr kTsIndex = 0;
  LOG(INFO) << "Shutting down TS " << cluster_->tablet_server(kTsIndex)->uuid();
  cluster_->tablet_server(kTsIndex)->Shutdown();
  auto new_ts = ts_map_[cluster_->tablet_server(kTsIndex)->uuid()].get();

  // Bounce the Master so it gets new tablet reports and doesn't try to assign a replica to the
  // dead TS.
  const auto timeout = MonoDelta::FromSeconds(40);
  cluster_->master()->Shutdown();
  LOG(INFO) << "Restarting master " << cluster_->master()->uuid();
  ASSERT_OK(cluster_->master()->Restart());
  ASSERT_OK(cluster_->WaitForTabletServerCount(3, timeout));

  // Populate a tablet with some data.
  LOG(INFO)  << "Starting workload";
  TestWorkload workload(cluster_.get());
  workload.Setup(YBTableType::YQL_TABLE_TYPE);
  workload.Start();
  workload.WaitInserted(10);
  LOG(INFO) << "Stopping workload";
  workload.StopAndJoin();

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  TServerDetails* ts = ts_map_[cluster_->tablet_server(1)->uuid()].get();
  ASSERT_OK(WaitForNumTabletsOnTS(ts, 1, timeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  TServerDetails* leader_ts;
  // Find out who's leader.
  ASSERT_OK(FindTabletLeader(ts_map_, tablet_id, timeout, &leader_ts));

  // Add back TS0.
  ASSERT_OK(cluster_->tablet_server(kTsIndex)->Restart());
  LOG(INFO) << "Adding tserver with uuid " << new_ts->uuid();
  ASSERT_OK(itest::AddServer(leader_ts, tablet_id, new_ts, RaftPeerPB::PRE_VOTER, boost::none,
                             timeout));
  // After adding  new_ts, the leader will detect that TS0 needs to be remote bootstrapped. Verify
  // that this process completes successfully.
  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(kTsIndex, tablet_id, TABLET_DATA_READY));
  LOG(INFO) << "Tablet " << tablet_id << " in state TABLET_DATA_READY in tablet server "
              << new_ts->uuid();

  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(4, leader_ts, tablet_id, timeout));
  LOG(INFO) << "Number of voters for tablet " << tablet_id << " is 4";

  // Ensure all the servers agree before we proceed.
  ASSERT_OK(WaitForServersToAgree(timeout, ts_map_, tablet_id, workload.batches_completed()));

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(),
                                                  ClusterVerifier::AT_LEAST,
                                                  workload.rows_inserted()));
}

TEST_F(RemoteBootstrapITest, TestFailedTabletIsRemoteBootstrapped) {
  std::vector<std::string> ts_flags = {
      "--follower_unavailable_considered_failed_sec=30",
      "--raft_heartbeat_interval_ms=50",
      "--consensus_rpc_timeout_ms=300",
      "--TEST_delay_removing_peer_with_failed_tablet_secs=10",
      "--memstore_size_mb=1",
      // Increase the number of missed heartbeats used to detect leader failure since in slow
      // testing instances it is very easy to miss the default (6) heartbeats since they are being
      // sent very fast (50ms).
      "--leader_failure_max_missed_heartbeat_periods=40.0"
  };

  std::vector<std::string> master_flags = {"--enable_load_balancing=true"};

  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, 3));

  const auto kTimeout = 40s;
  ASSERT_OK(cluster_->WaitForTabletServerCount(3, kTimeout));

  // Populate a tablet with some data.
  LOG(INFO) << "Starting workload";
  TestWorkload workload(cluster_.get());
  workload.Setup(YBTableType::YQL_TABLE_TYPE);
  workload.set_payload_bytes(1024);
  workload.Start();
  workload.WaitInserted(5000);
  LOG(INFO) << "Stopping workload";
  workload.StopAndJoin();

  // Figure out the tablet id of the created tablet.
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();
  ASSERT_OK(WaitForNumTabletsOnTS(ts, 1, kTimeout, &tablets));
  string tablet_id = tablets[0].tablet_status().tablet_id();

  TServerDetails* leader_ts;
  // Find out who's leader.
  ASSERT_OK(FindTabletLeader(ts_map_, tablet_id, kTimeout, &leader_ts));

  TServerDetails* non_leader_ts = nullptr;
  int non_leader_idx = -1;
  // Find the first non-leader TS.
  for (int i = 0; i < 3; i++) {
    if (cluster_->tablet_server(i)->uuid() != leader_ts->uuid()) {
      non_leader_ts = ts_map_[cluster_->tablet_server(i)->uuid()].get();
      non_leader_idx = i;
      break;
    }
  }

  ASSERT_NE(non_leader_ts, nullptr);
  ASSERT_NE(non_leader_idx, -1);

  ASSERT_OK(WaitUntilTabletInState(non_leader_ts, tablet_id, tablet::RUNNING, kTimeout));

  auto* env = Env::Default();
  const string& data_dir = cluster_->tablet_server_by_uuid(non_leader_ts->uuid())->GetFullDataDir();
  auto meta_dir = FsManager::GetRaftGroupMetadataDir(data_dir);
  auto metadata_path = JoinPathSegments(meta_dir, tablet_id);
  tablet::RaftGroupReplicaSuperBlockPB superblock;
  ASSERT_OK(pb_util::ReadPBContainerFromPath(env, metadata_path, &superblock));
  string tablet_data_dir = superblock.kv_store().rocksdb_dir();
  const auto& rocksdb_files = ASSERT_RESULT(env->GetChildren(tablet_data_dir, ExcludeDots::kTrue));
  ASSERT_GT(rocksdb_files.size(), 0);
  for (const auto& file : rocksdb_files) {
    if (file.size() > 4 && file.substr(file.size() - 4) == ".sst") {
      ASSERT_OK(env->DeleteFile(JoinPathSegments(tablet_data_dir, file)));
      LOG(INFO) << "Deleted file " << JoinPathSegments(tablet_data_dir, file);
    }
  }

  // Restart the tserver so that the tablet gets marked as FAILED when it's bootstrapped.
  // Flag TEST_delay_removing_peer_with_failed_tablet_secs will keep the tablet in the FAILED state
  // for the specified amount of time so that we can verify that it was indeed marked as FAILED.
  cluster_->tablet_server_by_uuid(non_leader_ts->uuid())->Shutdown();
  ASSERT_OK(cluster_->tablet_server_by_uuid(non_leader_ts->uuid())->Restart());

  ASSERT_OK(WaitUntilTabletInState(non_leader_ts, tablet_id, tablet::FAILED, kTimeout, 500ms));
  LOG(INFO) << "Tablet " << tablet_id << " in state FAILED in tablet server "
            << non_leader_ts->uuid();

  // After setting the tablet state to FAILED, the leader will detect that this TS needs to be
  // removed from the config so that it can be remote bootstrapped again. Check that this process
  // completes successfully.
  ASSERT_OK(WaitUntilTabletInState(non_leader_ts, tablet_id, tablet::RUNNING, kTimeout));
  LOG(INFO) << "Tablet " << tablet_id << " in state RUNNING in tablet server "
            << non_leader_ts->uuid();

  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(3, leader_ts, tablet_id, kTimeout));
  LOG(INFO) << "Number of voters for tablet " << tablet_id << " is 3";

  // Ensure all the servers agree before we proceed.
  ASSERT_OK(WaitForServersToAgree(kTimeout, ts_map_, tablet_id, workload.batches_completed()));

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(workload.table_name(),
                                                  ClusterVerifier::AT_LEAST,
                                                  workload.rows_inserted()));
}

TEST_F(RemoteBootstrapITest, TestRejectRogueLeaderKeyValueType) {
  RejectRogueLeader(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestDeleteTabletDuringRemoteBootstrapKeyValueType) {
  DeleteTabletDuringRemoteBootstrap(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestRemoteBootstrapFollowerWithHigherTermKeyValueType) {
  RemoteBootstrapFollowerWithHigherTerm(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestConcurrentRemoteBootstrapsKeyValueType) {
  ConcurrentRemoteBootstraps(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestDeleteLeaderDuringRemoteBootstrapStressTestKeyValueType) {
  DeleteLeaderDuringRemoteBootstrapStressTest(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestDisableRemoteBootstrap_NoTightLoopWhenTabletDeletedKeyValueType) {
  DisableRemoteBootstrap_NoTightLoopWhenTabletDeleted(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestLeaderCrashesWhileFetchingDataKeyValueTableType) {
  RemoteBootstrapITest::LeaderCrashesWhileFetchingData(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestLeaderCrashesBeforeChangeRoleKeyValueTableType) {
  RemoteBootstrapITest::LeaderCrashesBeforeChangeRole(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestLeaderCrashesAfterChangeRoleKeyValueTableType) {
  RemoteBootstrapITest::LeaderCrashesAfterChangeRole(YBTableType::YQL_TABLE_TYPE);
}

TEST_F(RemoteBootstrapITest, TestClientCrashesBeforeChangeRoleKeyValueTableType) {
  RemoteBootstrapITest::ClientCrashesBeforeChangeRole(YBTableType::YQL_TABLE_TYPE);
}

}  // namespace yb
