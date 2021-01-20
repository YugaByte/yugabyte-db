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

#include <map>
#include <memory>
#include <set>
#include <string>

#include <gflags/gflags.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>

#include "yb/client/client-test-util.h"
#include "yb/client/client_fwd.h"
#include "yb/client/table.h"
#include "yb/client/table_creator.h"
#include "yb/common/common.pb.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/integration-tests/external_mini_cluster-itest-base.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master_util.h"
#include "yb/master/sys_catalog_initialization.h"
#include "yb/util/metrics.h"
#include "yb/util/path_util.h"

using std::multimap;
using std::set;
using std::string;
using std::vector;
using strings::Substitute;
using yb::client::YBTableType;
using yb::client::YBTableName;

METRIC_DECLARE_entity(server);
METRIC_DECLARE_entity(tablet);
METRIC_DECLARE_gauge_int64(is_raft_leader);
METRIC_DECLARE_histogram(handler_latency_yb_tserver_TabletServerAdminService_CreateTablet);
METRIC_DECLARE_histogram(handler_latency_yb_tserver_TabletServerAdminService_DeleteTablet);

namespace yb {

static const YBTableName kTableName(YQL_DATABASE_CQL, "my_keyspace", "test-table");

class CreateTableITest : public ExternalMiniClusterITestBase {
 public:
  Status CreateTableWithPlacement(
      const master::ReplicationInfoPB& replication_info, const string& table_suffix,
      const YBTableType table_type = YBTableType::YQL_TABLE_TYPE) {
    auto db_type = master::GetDatabaseTypeForTable(
        client::YBTable::ClientToPBTableType(table_type));
    RETURN_NOT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(), db_type));
    std::unique_ptr<client::YBTableCreator> table_creator(client_->NewTableCreator());
    client::YBSchema client_schema(client::YBSchemaFromSchema(yb::GetSimpleTestSchema()));
    if (table_type != YBTableType::REDIS_TABLE_TYPE) {
      table_creator->schema(&client_schema);
    }
    return table_creator->table_name(
        YBTableName(db_type,
                    kTableName.namespace_name(),
                    Substitute("$0:$1", kTableName.table_name(), table_suffix)))
        .replication_info(replication_info)
        .table_type(table_type)
        .wait(true)
        .Create();
  }
};

// TODO(bogdan): disabled until ENG-2687
TEST_F(CreateTableITest, DISABLED_TestCreateRedisTable) {
  const string cloud = "aws";
  const string region = "us-west-1";
  const string zone = "a";

  const int kNumReplicas = 3;
  vector<string> flags = {Substitute("--placement_cloud=$0", cloud),
                          Substitute("--placement_region=$0", region),
                          Substitute("--placement_zone=$0", zone)};
  ASSERT_NO_FATALS(StartCluster(flags, flags, kNumReplicas));

  master::ReplicationInfoPB replication_info;
  replication_info.mutable_live_replicas()->set_num_replicas(kNumReplicas);
  auto* placement_block = replication_info.mutable_live_replicas()->add_placement_blocks();
  auto* cloud_info = placement_block->mutable_cloud_info();
  cloud_info->set_placement_cloud(cloud);
  cloud_info->set_placement_region(region);
  cloud_info->set_placement_zone(zone);
  placement_block->set_min_num_replicas(kNumReplicas);

  // Successful table create.
  ASSERT_OK(
      CreateTableWithPlacement(replication_info, "success_base", YBTableType::REDIS_TABLE_TYPE));
}

// TODO(bogdan): disabled until ENG-2687
TEST_F(CreateTableITest, DISABLED_TestCreateWithPlacement) {
  const string cloud = "aws";
  const string region = "us-west-1";
  const string zone = "a";

  const int kNumReplicas = 3;
  vector<string> flags = {Substitute("--placement_cloud=$0", cloud),
                          Substitute("--placement_region=$0", region),
                          Substitute("--placement_zone=$0", zone)};
  ASSERT_NO_FATALS(StartCluster(flags, flags, kNumReplicas));

  master::ReplicationInfoPB replication_info;
  replication_info.mutable_live_replicas()->set_num_replicas(kNumReplicas);
  auto* placement_block = replication_info.mutable_live_replicas()->add_placement_blocks();
  auto* cloud_info = placement_block->mutable_cloud_info();
  cloud_info->set_placement_cloud(cloud);
  cloud_info->set_placement_region(region);
  cloud_info->set_placement_zone(zone);
  placement_block->set_min_num_replicas(kNumReplicas);

  // Successful table create.
  ASSERT_OK(CreateTableWithPlacement(replication_info, "success_base"));

  // Cannot create table with 4 replicas when only 3 TS available.
  {
    auto copy_replication_info = replication_info;
    copy_replication_info.mutable_live_replicas()->set_num_replicas(kNumReplicas + 1);
    Status s = CreateTableWithPlacement(copy_replication_info, "fail_num_replicas");
    ASSERT_TRUE(s.IsInvalidArgument());
  }

  // Cannot create table in locations we have no servers.
  {
    auto copy_replication_info = replication_info;
    auto* new_placement =
        copy_replication_info.mutable_live_replicas()->mutable_placement_blocks(0);
    new_placement->mutable_cloud_info()->set_placement_zone("b");
    Status s = CreateTableWithPlacement(copy_replication_info, "fail_zone");
    ASSERT_TRUE(s.IsTimedOut());
  }

  // Set cluster config placement and test table placement interaction. Right now, this should fail
  // instantly, as we do not support cluster and table level at the same time.
  ASSERT_OK(client_->SetReplicationInfo(replication_info));
  {
    Status s = CreateTableWithPlacement(replication_info, "fail_table_placement");
    ASSERT_TRUE(s.IsInvalidArgument());
  }
}

// Regression test for an issue seen when we fail to create a majority of the
// replicas in a tablet. Previously, we'd still consider the tablet "RUNNING"
// on the master and finish the table creation, even though that tablet would
// be stuck forever with its minority never able to elect a leader.
TEST_F(CreateTableITest, TestCreateWhenMajorityOfReplicasFailCreation) {
  const int kNumReplicas = 3;
  const int kNumTablets = 1;
  vector<string> ts_flags;
  vector<string> master_flags;
  master_flags.push_back("--tablet_creation_timeout_ms=1000");
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumReplicas));

  // Shut down 2/3 of the tablet servers.
  cluster_->tablet_server(1)->Shutdown();
  cluster_->tablet_server(2)->Shutdown();

  // Try to create a single-tablet table.
  // This won't succeed because we can't create enough replicas to get
  // a quorum.
  ASSERT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                                kTableName.namespace_type()));
  std::unique_ptr<client::YBTableCreator> table_creator(client_->NewTableCreator());
  client::YBSchema client_schema(client::YBSchemaFromSchema(GetSimpleTestSchema()));
  ASSERT_OK(table_creator->table_name(kTableName)
            .schema(&client_schema)
            .num_tablets(kNumTablets)
            .wait(false)
            .Create());

  // Sleep until we've seen a couple retries on our live server.
  int64_t num_create_attempts = 0;
  while (num_create_attempts < 3) {
    SleepFor(MonoDelta::FromMilliseconds(100));
    num_create_attempts = ASSERT_RESULT(cluster_->tablet_server(0)->GetInt64Metric(
        &METRIC_ENTITY_server,
        "yb.tabletserver",
        &METRIC_handler_latency_yb_tserver_TabletServerAdminService_CreateTablet,
        "total_count"));
    LOG(INFO) << "Waiting for the master to retry creating the tablet 3 times... "
              << num_create_attempts << " RPCs seen so far";

    // The CreateTable operation should still be considered in progress, even though
    // we'll be successful at creating a single replica.
    bool in_progress = false;
    ASSERT_OK(client_->IsCreateTableInProgress(kTableName, &in_progress));
    ASSERT_TRUE(in_progress);
  }

  // Once we restart the servers, we should succeed at creating a healthy
  // replicated tablet.
  ASSERT_OK(cluster_->tablet_server(1)->Restart());
  ASSERT_OK(cluster_->tablet_server(2)->Restart());

  // We should eventually finish the table creation we started earlier.
  bool in_progress = false;
  while (in_progress) {
    LOG(INFO) << "Waiting for the master to successfully create the table...";
    ASSERT_OK(client_->IsCreateTableInProgress(kTableName, &in_progress));
    SleepFor(MonoDelta::FromMilliseconds(100));
  }

  // The server that was up from the beginning should be left with only
  // one tablet, eventually, since the tablets which failed to get created
  // properly should get deleted.
  vector<string> tablets;
  int wait_iter = 0;
  while (tablets.size() != kNumTablets && wait_iter++ < 100) {
    LOG(INFO) << "Waiting for only " << kNumTablets << " tablet(s) to be left on TS 0. "
              << "Currently have: " << tablets;
    SleepFor(MonoDelta::FromMilliseconds(100));
    tablets = inspect_->ListTabletsWithDataOnTS(0);
  }
  ASSERT_EQ(tablets.size(), kNumTablets) << "Tablets on TS0: " << tablets;
}

// Regression test for KUDU-1317. Ensure that, when a table is created,
// the tablets are well spread out across the machines in the cluster and
// that recovery from failures will be well parallelized.
TEST_F(CreateTableITest, TestSpreadReplicasEvenly) {
  const int kNumServers = 10;
  const int kNumTablets = 20;
  vector<string> ts_flags;
  vector<string> master_flags;
  ts_flags.push_back("--never_fsync");  // run faster on slow disks
  master_flags.push_back("--enable_load_balancing=false");  // disable load balancing moves
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumServers));

  ASSERT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                                kTableName.namespace_type()));
  std::unique_ptr<client::YBTableCreator> table_creator(client_->NewTableCreator());
  client::YBSchema client_schema(client::YBSchemaFromSchema(GetSimpleTestSchema()));
  ASSERT_OK(table_creator->table_name(kTableName)
            .schema(&client_schema)
            .num_tablets(kNumTablets)
            .Create());

  // Computing the standard deviation of the number of replicas per server.
  const double kMeanPerServer = kNumTablets * 3.0 / kNumServers;
  double sum_squared_deviation = 0;
  vector<int> tablet_counts;
  for (int ts_idx = 0; ts_idx < kNumServers; ts_idx++) {
    int num_replicas = inspect_->ListTabletsOnTS(ts_idx).size();
    LOG(INFO) << "TS " << ts_idx << " has " << num_replicas << " tablets";
    double deviation = static_cast<double>(num_replicas) - kMeanPerServer;
    sum_squared_deviation += deviation * deviation;
  }
  double stddev = 0.0;
  // The denominator in the following formula is kNumServers - 1 instead of kNumServers because
  // of Bessel's correction for unbiased estimation of variance.
  if (kNumServers > 1) {
    stddev = sqrt(sum_squared_deviation / (kNumServers - 1));
  }
  LOG(INFO) << "stddev = " << stddev;
  LOG(INFO) << "mean = " << kMeanPerServer;
  // We want to ensure that stddev is small compared to mean.
  const double threshold_ratio = 0.2;
  // We are verifying that stddev is less than 20% of the mean + 1.0.
  // "+ 1.0" is needed because stddev is inflated by discreet counting.

  // In 100 runs, the maximum threshold needed was 10%. 20% is a safe value to prevent
  // failures from random chance.
  ASSERT_LE(stddev, kMeanPerServer * threshold_ratio + 1.0);

  // Construct a map from tablet ID to the set of servers that each tablet is hosted on.
  multimap<string, int> tablet_to_servers;
  for (int ts_idx = 0; ts_idx < kNumServers; ts_idx++) {
    vector<string> tablets = inspect_->ListTabletsOnTS(ts_idx);
    for (const string& tablet_id : tablets) {
      tablet_to_servers.insert(std::make_pair(tablet_id, ts_idx));
    }
  }

  // For each server, count how many other servers it shares tablets with.
  // This is highly correlated to how well parallelized recovery will be
  // in the case the server crashes.
  int sum_num_peers = 0;
  for (int ts_idx = 0; ts_idx < kNumServers; ts_idx++) {
    vector<string> tablets = inspect_->ListTabletsOnTS(ts_idx);
    set<int> peer_servers;
    for (const string& tablet_id : tablets) {
      auto peer_indexes = tablet_to_servers.equal_range(tablet_id);
      for (auto it = peer_indexes.first; it != peer_indexes.second; ++it) {
        peer_servers.insert(it->second);
      }
    }

    peer_servers.erase(ts_idx);
    LOG(INFO) << "Server " << ts_idx << " has " << peer_servers.size() << " peers";
    sum_num_peers += peer_servers.size();
  }

  // On average, servers should have at least half the other servers as peers.
  double avg_num_peers = static_cast<double>(sum_num_peers) / kNumServers;
  LOG(INFO) << "avg_num_peers = " << avg_num_peers;
  ASSERT_GE(avg_num_peers, kNumServers / 2);
}

TEST_F(CreateTableITest, TestNoAllocBlacklist) {
  const int kNumServers = 4;
  const int kNumTablets = 24;
  vector<string> ts_flags;
  vector<string> master_flags;
  ts_flags.push_back("--never_fsync");  // run faster on slow disks
  master_flags.push_back("--enable_load_balancing=false");  // disable load balancing moves
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumServers));
  // add TServer to blacklist
  ASSERT_OK(cluster_->AddTServerToBlacklist(cluster_->master(), cluster_->tablet_server(1)));
  // create table
  ASSERT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                                kTableName.namespace_type()));
  std::unique_ptr<client::YBTableCreator> table_creator(client_->NewTableCreator());
  client::YBSchema client_schema(client::YBSchemaFromSchema(GetSimpleTestSchema()));
  ASSERT_OK(table_creator->table_name(kTableName)
                .schema(&client_schema)
                .num_tablets(kNumTablets)
                .Create());
  // check that no tablets have been allocated to blacklisted TServer
  ASSERT_EQ(inspect_->ListTabletsOnTS(1).size(), 0);
}

TEST_F(CreateTableITest, TableColocationRemoteBootstrapTest) {
  const int kNumReplicas = 3;
  string parent_table_id;
  string tablet_id;
  vector<string> ts_flags;
  vector<string> master_flags;

  ts_flags.push_back("--follower_unavailable_considered_failed_sec=3");
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumReplicas));
  ASSERT_OK(
      client_->CreateNamespace("colocation_test", boost::none /* db */, "" /* creator */,
                               "" /* ns_id */, "" /* src_ns_id */,
                               boost::none /* next_pg_oid */, boost::none /* txn */, true));

  {
    string ns_id;
    auto namespaces = ASSERT_RESULT(client_->ListNamespaces(boost::none));
    for (const auto& ns : namespaces) {
      if (ns.name() == "colocation_test") {
        ns_id = ns.id();
        break;
      }
    }
    ASSERT_FALSE(ns_id.empty());
    parent_table_id = ns_id + master::kColocatedParentTableIdSuffix;
  }

  {
    google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
    ASSERT_OK(WaitFor(
        [&]() -> bool {
          EXPECT_OK(client_->GetTabletsFromTableId(parent_table_id, 0, &tablets));
          return tablets.size() == 1;
        },
        MonoDelta::FromSeconds(30), "Create colocated tablet"));
    tablet_id = tablets[0].tablet_id();
  }

  string rocksdb_dir = JoinPathSegments(
      cluster_->data_root(), "ts-1", "yb-data", "tserver", "data", "rocksdb",
      "table-" + parent_table_id, "tablet-" + tablet_id);
  string wal_dir = JoinPathSegments(
      cluster_->data_root(), "ts-1", "yb-data", "tserver", "wals", "table-" + parent_table_id,
      "tablet-" + tablet_id);
  std::function<Result<bool>()> dirs_exist = [&] {
    return Env::Default()->FileExists(rocksdb_dir) && Env::Default()->FileExists(wal_dir);
  };

  ASSERT_OK(WaitFor(dirs_exist, MonoDelta::FromSeconds(30), "Create data and wal directories"));

  // Stop a tablet server and create a new tablet server. This will trigger a remote bootstrap on
  // the new tablet server.
  cluster_->tablet_server(2)->Shutdown();
  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(4, MonoDelta::FromSeconds(20)));

  // Remote bootstrap should create the correct tablet directory for the new tablet server.
  rocksdb_dir = JoinPathSegments(
      cluster_->data_root(), "ts-4", "yb-data", "tserver", "data", "rocksdb",
      "table-" + parent_table_id, "tablet-" + tablet_id);
  wal_dir = JoinPathSegments(
      cluster_->data_root(), "ts-4", "yb-data", "tserver", "wals", "table-" + parent_table_id,
      "tablet-" + tablet_id);
  ASSERT_OK(WaitFor(dirs_exist, MonoDelta::FromSeconds(100), "Create data and wal directories"));
}

TEST_F(CreateTableITest, TablegroupRemoteBootstrapTest) {
  const int kNumReplicas = 3;
  string parent_table_id;
  string tablet_id;
  vector<string> ts_flags;
  vector<string> master_flags;
  string namespace_name = "tablegroup_test_namespace_name";
  TablegroupId tablegroup_id = "tablegroup_test_id00000000000000";
  string namespace_id;

  ts_flags.push_back("--follower_unavailable_considered_failed_sec=3");
  ts_flags.push_back("--ysql_beta_feature_tablegroup=true");
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumReplicas));

  ASSERT_OK(client_->CreateNamespace(namespace_name, YQL_DATABASE_PGSQL, "" /* creator */,
                                     "" /* ns_id */, "" /* src_ns_id */,
                                     boost::none /* next_pg_oid */, boost::none /* txn */, false));

  {
    auto namespaces = ASSERT_RESULT(client_->ListNamespaces(boost::none));
    for (const auto& ns : namespaces) {
      if (ns.name() == namespace_name) {
        namespace_id = ns.id();
        break;
      }
    }
    ASSERT_FALSE(namespace_id.empty());
  }

  // Since this is just for testing purposes, we do not bother generating a valid PgsqlTablegroupId
  ASSERT_OK(
      client_->CreateTablegroup(namespace_name, namespace_id, tablegroup_id));

  // Now want to ensure that the newly created tablegroup shows up in the list.
  auto exists = ASSERT_RESULT(client_->TablegroupExists(namespace_name, tablegroup_id));
  ASSERT_TRUE(exists);
  parent_table_id = tablegroup_id + master::kTablegroupParentTableIdSuffix;

  {
    google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
    ASSERT_OK(WaitFor(
        [&]() -> bool {
          EXPECT_OK(client_->GetTabletsFromTableId(parent_table_id, 0, &tablets));
          return tablets.size() == 1;
        },
        MonoDelta::FromSeconds(30), "Create tablegroup tablet"));
    tablet_id = tablets[0].tablet_id();
  }

  string rocksdb_dir = JoinPathSegments(
      cluster_->data_root(), "ts-1", "yb-data", "tserver", "data", "rocksdb",
      "table-" + parent_table_id, "tablet-" + tablet_id);
  string wal_dir = JoinPathSegments(
      cluster_->data_root(), "ts-1", "yb-data", "tserver", "wals", "table-" + parent_table_id,
      "tablet-" + tablet_id);
  std::function<Result<bool>()> dirs_exist = [&] {
    return Env::Default()->FileExists(rocksdb_dir) && Env::Default()->FileExists(wal_dir);
  };

  ASSERT_OK(WaitFor(dirs_exist, MonoDelta::FromSeconds(30), "Create data and wal directories"));

  // Stop a tablet server and create a new tablet server. This will trigger a remote bootstrap on
  // the new tablet server.
  cluster_->tablet_server(2)->Shutdown();
  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(4, MonoDelta::FromSeconds(20)));

  // Remote bootstrap should create the correct tablet directory for the new tablet server.
  rocksdb_dir = JoinPathSegments(
      cluster_->data_root(), "ts-4", "yb-data", "tserver", "data", "rocksdb",
      "table-" + parent_table_id, "tablet-" + tablet_id);
  wal_dir = JoinPathSegments(
      cluster_->data_root(), "ts-4", "yb-data", "tserver", "wals", "table-" + parent_table_id,
      "tablet-" + tablet_id);
  ASSERT_OK(WaitFor(dirs_exist, MonoDelta::FromSeconds(100), "Create data and wal directories"));
}

TEST_F(CreateTableITest, TestIsRaftLeaderMetric) {
  const int kNumReplicas = 3;
  const int kNumTablets = 1;
  const int kExpectedRaftLeaders = 1;
  vector<string> ts_flags;
  vector<string> master_flags;
  master_flags.push_back("--tablet_creation_timeout_ms=1000");
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, kNumReplicas));
  ASSERT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                                kTableName.namespace_type()));
  std::unique_ptr<client::YBTableCreator> table_creator(client_->NewTableCreator());
  client::YBSchema client_schema(client::YBSchemaFromSchema(GetSimpleTestSchema()));

  // create a table
  ASSERT_OK(table_creator->table_name(kTableName)
                .schema(&client_schema)
                .num_tablets(kNumTablets)
                .Create());

  // Count the total Number of Raft Leaders in the cluster. Go through each tablet of every
  // tablet-server and sum up the leaders.
  int64_t kNumRaftLeaders = 0;
  for (int i = 0 ; i < kNumReplicas; i++) {
    auto tablet_ids = ASSERT_RESULT(cluster_->GetTabletIds(cluster_->tablet_server(i)));
    for(int ti = 0; ti < inspect_->ListTabletsOnTS(i).size(); ti++) {
      const char *tabletId = tablet_ids[ti].c_str();
      kNumRaftLeaders += ASSERT_RESULT(cluster_->tablet_server(i)->GetInt64Metric(
          &METRIC_ENTITY_tablet, tabletId, &METRIC_is_raft_leader, "value"));
    }
  }
  ASSERT_EQ(kNumRaftLeaders, kExpectedRaftLeaders);
}

TEST_F(CreateTableITest, TestTransactionStatusTableCreation) {
  // Set up an RF 1.
  // Tell the Master leader to wait for 3 TS to join before creating the
  // transaction status table.
  vector<string> master_flags = {
        "--txn_table_wait_min_ts_count=3"
  };
  // We also need to enable ysql.
  ASSERT_NO_FATALS(StartCluster({}, master_flags, 1, 1, true));

  // Check that the transaction table hasn't been created yet.
  YQLDatabase db = YQL_DATABASE_CQL;
  YBTableName transaction_status_table(db, master::kSystemNamespaceId,
                                  master::kSystemNamespaceName, kTransactionsTableName);
  bool exists = ASSERT_RESULT(client_->TableExists(transaction_status_table));
  ASSERT_FALSE(exists) << "Transaction table exists even though the "
                          "requirement for the minimum number of TS not met";

  // Add two tservers.
  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->AddTabletServer());

  auto tbl_exists = [&]() -> Result<bool> {
    return client_->TableExists(transaction_status_table);
  };

  ASSERT_OK(WaitFor(tbl_exists, 30s * kTimeMultiplier,
                    "Transaction table doesn't exist even though the "
                    "requirement for the minimum number of TS met"));
}

}  // namespace yb
