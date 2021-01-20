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
#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/client/table_creator.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/consensus/metadata.pb.h"
#include "yb/consensus/quorum_util.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/cluster_itest_util.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/master/master.pb.h"
#include "yb/master/master.proxy.h"
#include "yb/master/mini_master.h"
#include "yb/rpc/messenger.h"
#include "yb/server/server_base.proxy.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/tserver_admin.proxy.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/util/test_util.h"

DECLARE_bool(enable_leader_failure_detection);
DECLARE_bool(catalog_manager_wait_for_new_tablets_to_elect_leader);
DEFINE_int32(num_election_test_loops, 3,
             "Number of random EmulateElection() loops to execute in "
             "TestReportNewLeaderOnLeaderChange");
DECLARE_bool(enable_ysql);
DECLARE_bool(use_create_table_leader_hint);

namespace yb {
namespace tserver {

using client::YBClient;
using client::YBSchema;
using client::YBTable;
using client::YBTableCreator;
using client::YBTableName;
using consensus::GetConsensusRole;
using consensus::RaftPeerPB;
using itest::SimpleIntKeyYBSchema;
using master::MasterServiceProxy;
using master::ReportedTabletPB;
using master::TabletReportPB;
using rpc::Messenger;
using rpc::MessengerBuilder;
using strings::Substitute;
using tablet::TabletPeer;
using tserver::MiniTabletServer;
using tserver::TSTabletManager;

static const YBTableName kTableName(YQL_DATABASE_CQL, "my_keyspace", "test-table");
static const int kNumReplicas = 3;

class TsTabletManagerITest : public YBTest {
 public:
  TsTabletManagerITest()
      : schema_(SimpleIntKeyYBSchema()) {
  }
  void SetUp() override;
  void TearDown() override;

 protected:
  const YBSchema schema_;

  gscoped_ptr<MiniCluster> cluster_;
  std::unique_ptr<Messenger> client_messenger_;
  std::unique_ptr<YBClient> client_;
};

void TsTabletManagerITest::SetUp() {
  // We don't need the transaction status table to be created.
  SetAtomicFlag(false, &FLAGS_enable_ysql);
  YBTest::SetUp();

  MessengerBuilder bld("client");
  client_messenger_ = ASSERT_RESULT(bld.Build());
  client_messenger_->TEST_SetOutboundIpBase(ASSERT_RESULT(HostToAddress("127.0.0.1")));

  MiniClusterOptions opts;
  opts.num_tablet_servers = kNumReplicas;
  cluster_.reset(new MiniCluster(env_.get(), opts));
  ASSERT_OK(cluster_->Start());
  client_ = ASSERT_RESULT(cluster_->CreateClient(client_messenger_.get()));
}

void TsTabletManagerITest::TearDown() {
  client_.reset();
  client_messenger_->Shutdown();
  cluster_->Shutdown();
  YBTest::TearDown();
}

// Test that when the leader changes, the tablet manager gets notified and
// includes that information in the next tablet report.
TEST_F(TsTabletManagerITest, TestReportNewLeaderOnLeaderChange) {
  // We need to control elections precisely for this test since we're using
  // EmulateElection() with a distributed consensus configuration.
  FLAGS_enable_leader_failure_detection = false;
  FLAGS_catalog_manager_wait_for_new_tablets_to_elect_leader = false;
  FLAGS_use_create_table_leader_hint = false;

  // Run a few more iters in slow-test mode.
  OverrideFlagForSlowTests("num_election_test_loops", "10");

  ASSERT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                                kTableName.namespace_type()));
  // Create the table.
  std::shared_ptr<YBTable> table;
  std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_OK(table_creator->table_name(kTableName)
            .schema(&schema_)
            .hash_schema(YBHashSchema::kMultiColumnHash)
            .num_tablets(1)
            .Create());
  ASSERT_OK(client_->OpenTable(kTableName, &table));

  rpc::ProxyCache proxy_cache(client_messenger_.get());

  // Build a TServerDetails map so we can check for convergence.
  MasterServiceProxy master_proxy(&proxy_cache, cluster_->mini_master()->bound_rpc_addr());

  itest::TabletServerMap ts_map;
  ASSERT_OK(CreateTabletServerMap(&master_proxy, &proxy_cache, &ts_map));

  // Collect the tablet peers so we get direct access to consensus.
  vector<std::shared_ptr<TabletPeer> > tablet_peers;
  for (int replica = 0; replica < kNumReplicas; replica++) {
    MiniTabletServer* ts = cluster_->mini_tablet_server(replica);
    ts->FailHeartbeats(); // Stop heartbeating we don't race against the Master.
    vector<std::shared_ptr<TabletPeer> > cur_ts_tablet_peers;
    // The replicas may not have been created yet, so loop until we see them.
    while (true) {
      ts->server()->tablet_manager()->GetTabletPeers(&cur_ts_tablet_peers);
      if (!cur_ts_tablet_peers.empty()) break;
      SleepFor(MonoDelta::FromMilliseconds(10));
    }
    ASSERT_EQ(1, cur_ts_tablet_peers.size());
    ASSERT_OK(cur_ts_tablet_peers[0]->WaitUntilConsensusRunning(MonoDelta::FromSeconds(10)));
    tablet_peers.push_back(cur_ts_tablet_peers[0]);
  }

  // Loop and cause elections and term changes from different servers.
  // TSTabletManager should acknowledge the role changes via tablet reports.
  unsigned int seed = SeedRandom();
  for (int i = 0; i < FLAGS_num_election_test_loops; i++) {
    SCOPED_TRACE(Substitute("Iter: $0", i));
    int new_leader_idx = rand_r(&seed) % 2;
    LOG(INFO) << "Electing peer " << new_leader_idx << "...";
    consensus::Consensus* con = CHECK_NOTNULL(tablet_peers[new_leader_idx]->consensus());
    ASSERT_OK(con->EmulateElection());
    LOG(INFO) << "Waiting for servers to agree...";
    ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(5),
                                    ts_map, tablet_peers[0]->tablet_id(), i + 1));

    // Now check that the tablet report reports the correct role for both servers.
    for (int replica = 0; replica < kNumReplicas; replica++) {
      // The MarkDirty() callback is on an async thread so it might take the
      // follower a few milliseconds to execute it. Wait for that to happen.
      TSTabletManager* tablet_manager =
          cluster_->mini_tablet_server(replica)->server()->tablet_manager();
      for (int retry = 0; retry <= 12; retry++) {
        if (tablet_manager->GetNumDirtyTabletsForTests() > 0) break;
        SleepFor(MonoDelta::FromMilliseconds(1 << retry));
      }

      // Ensure that our tablet reports are consistent.
      TabletReportPB report;
      tablet_manager->GenerateTabletReport(&report);
      ASSERT_EQ(1, report.updated_tablets_size()) << "Wrong report size:\n" << report.DebugString();
      ReportedTabletPB reported_tablet = report.updated_tablets(0);
      ASSERT_TRUE(reported_tablet.has_committed_consensus_state());

      string uuid = tablet_peers[replica]->permanent_uuid();
      RaftPeerPB::Role role = GetConsensusRole(uuid, reported_tablet.committed_consensus_state());
      if (replica == new_leader_idx) {
        ASSERT_EQ(RaftPeerPB::LEADER, role)
            << "Tablet report: " << report.ShortDebugString();
      } else {
        ASSERT_EQ(RaftPeerPB::FOLLOWER, role)
            << "Tablet report: " << report.ShortDebugString();
      }
    }
  }
}

}  // namespace tserver
}  // namespace yb
