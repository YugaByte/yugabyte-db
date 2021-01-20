// Copyright (c) YugaByte, Inc.
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

#include "yb/integration-tests/yb_table_test_base.h"

#include "yb/client/schema.h"
#include "yb/client/table.h"
#include "yb/client/table_creator.h"
#include "yb/client/yb_table_name.h"
#include "yb/gutil/strings/join.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/test_workload.h"
#include "yb/master/master.h"
#include "yb/master/master.proxy.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/tools/yb-admin_client.h"

using namespace std::literals;

namespace yb {
namespace integration_tests {

const auto kDefaultTimeout = 30000ms;

class LoadBalancerPlacementPolicyTest : public YBTableTestBase {
 protected:
  void SetUp() override {
    YBTableTestBase::SetUp();

    yb_admin_client_ = std::make_unique<tools::enterprise::ClusterAdminClient>(
        external_mini_cluster()->GetMasterAddresses(), kDefaultTimeout);

    ASSERT_OK(yb_admin_client_->Init());
  }

  bool use_external_mini_cluster() override { return true; }

  int num_tablets() override {
    return 4;
  }

  bool enable_ysql() override {
    // Do not create the transaction status table.
    return false;
  }

  void GetLoadOnTservers(const string tablename,
                         int num_tservers,
                         vector<int> *const out_load_per_tserver) {
    out_load_per_tserver->clear();
    for (int ii = 0; ii < num_tservers; ++ii) {
      const int count = ASSERT_RESULT(GetLoadOnTserver(
          external_mini_cluster()->tablet_server(ii), tablename));
      out_load_per_tserver->emplace_back(count);
    }
  }

  Result<uint32_t> GetLoadOnTserver(ExternalTabletServer* server, const string tablename) {
    auto proxy = VERIFY_RESULT(GetMasterLeaderProxy());
    master::GetTableLocationsRequestPB req;
    req.mutable_table()->set_table_name(tablename);
    req.mutable_table()->mutable_namespace_()->set_name(table_name().namespace_name());
    master::GetTableLocationsResponsePB resp;

    rpc::RpcController rpc;
    rpc.set_timeout(kDefaultTimeout);
    RETURN_NOT_OK(proxy->GetTableLocations(req, &resp, &rpc));

    uint32_t count = 0;
    std::vector<string> replicas;
    for (const auto& loc : resp.tablet_locations()) {
      for (const auto& replica : loc.replicas()) {
        if (replica.ts_info().permanent_uuid() == server->instance_id().permanent_uuid()) {
          replicas.push_back(loc.tablet_id());
          count++;
        }
      }
    }
    LOG(INFO) << Format("For ts $0, table name $1 tablet count $2",
                        server->instance_id().permanent_uuid(), tablename, count);
    return count;
  }

  Result<std::shared_ptr<master::MasterServiceProxy>> GetMasterLeaderProxy() {
    int idx;
    RETURN_NOT_OK(external_mini_cluster()->GetLeaderMasterIndex(&idx));
    return external_mini_cluster()->master_proxy(idx);
  }

  void CustomizeExternalMiniCluster(ExternalMiniClusterOptions* opts) override {
    opts->extra_tserver_flags.push_back("--placement_cloud=c");
    opts->extra_tserver_flags.push_back("--placement_region=r");
    opts->extra_tserver_flags.push_back("--placement_zone=z${index}");
    opts->extra_master_flags.push_back("--load_balancer_skip_leader_as_remove_victim=false");
    opts->extra_master_flags.push_back("--tserver_unresponsive_timeout_ms=5000");
  }

  void WaitForLoadBalancer() {
    ASSERT_OK(WaitFor([&]() -> Result<bool> {
      bool is_idle = VERIFY_RESULT(client_->IsLoadBalancerIdle());
      return !is_idle;
    },  kDefaultTimeout * 2, "IsLoadBalancerActive"));

    ASSERT_OK(WaitFor([&]() -> Result<bool> {
      return client_->IsLoadBalancerIdle();
    },  kDefaultTimeout * 4, "IsLoadBalancerIdle"));
  }

  void AddNewTserverToZone(
    const string& zone,
    const int expected_num_tservers,
    const string& placement_uuid = "") {

    std::vector<std::string> extra_opts;
    extra_opts.push_back("--placement_cloud=c");
    extra_opts.push_back("--placement_region=r");
    extra_opts.push_back("--placement_zone=" + zone);

    if (!placement_uuid.empty()) {
      extra_opts.push_back("--placement_uuid=" + placement_uuid);
    }

    ASSERT_OK(external_mini_cluster()->AddTabletServer(true, extra_opts));
    ASSERT_OK(external_mini_cluster()->WaitForTabletServerCount(expected_num_tservers,
      kDefaultTimeout));
  }

  std::unique_ptr<tools::enterprise::ClusterAdminClient> yb_admin_client_;
};

TEST_F(LoadBalancerPlacementPolicyTest, CreateTableWithPlacementPolicyTest) {
  // Set cluster placement policy.
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1,c.r.z2", 3, ""));

  const string& create_custom_policy_table = "creation-placement-test";
  const yb::client::YBTableName placement_table(
    YQL_DATABASE_CQL, table_name().namespace_name(), create_custom_policy_table);

  yb::client::YBSchemaBuilder b;
  yb::client::YBSchema schema;
  b.AddColumn("k")->Type(BINARY)->NotNull()->HashPrimaryKey();
  ASSERT_OK(b.Build(&schema));

  // Set placement policy for the new table that is different from the cluster placement policy.
  master::ReplicationInfoPB replication_info;
  replication_info.mutable_live_replicas()->set_num_replicas(2);
  auto* placement_block = replication_info.mutable_live_replicas()->add_placement_blocks();
  auto* cloud_info = placement_block->mutable_cloud_info();
  cloud_info->set_placement_cloud("c");
  cloud_info->set_placement_region("r");
  cloud_info->set_placement_zone("z1");
  placement_block->set_min_num_replicas(1);

  placement_block = replication_info.mutable_live_replicas()->add_placement_blocks();
  cloud_info = placement_block->mutable_cloud_info();
  cloud_info->set_placement_cloud("c");
  cloud_info->set_placement_region("r");
  cloud_info->set_placement_zone("z2");
  placement_block->set_min_num_replicas(1);

  ASSERT_OK(NewTableCreator()->table_name(placement_table).schema(&schema).replication_info(
    replication_info).Create());

  vector<int> counts_per_ts;
  int64 num_tservers = num_tablet_servers();
  GetLoadOnTservers(create_custom_policy_table, num_tservers, &counts_per_ts);
  // Verify that the tserver in zone0 does not have any tablets assigned to it.
  ASSERT_EQ(counts_per_ts[0], 0);
  // Verify that the tservers in z1 and z2 have tablets assigned to them.
  ASSERT_EQ(counts_per_ts[1], 4);
  ASSERT_EQ(counts_per_ts[2], 4);

  // Verify that modifying the placement info for a table with custom placement
  // policy works as expected.
  ASSERT_OK(yb_admin_client_->ModifyTablePlacementInfo(
    placement_table, "c.r.z0,c.r.z1,c.r.z2", 3, ""));
  WaitForLoadBalancer();

  // The replication factor increased to 3, and the placement info now has all 3 zones.
  // Thus, all tservers should have 4 tablets.
  GetLoadOnTservers(create_custom_policy_table, num_tservers, &counts_per_ts);
  for (int ii = 0; ii < 3; ++ii) {
    ASSERT_EQ(counts_per_ts[ii], 4);
  }
}

TEST_F(LoadBalancerPlacementPolicyTest, PlacementPolicyTest) {
  // Set cluster placement policy.
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1,c.r.z2", 3, ""));

  // Add a new tserver to zone 1.
  int num_tservers = num_tablet_servers() + 1;
  AddNewTserverToZone("z1", num_tservers);

  WaitForLoadBalancer();

  // Create another table for which we will set custom placement info.
  const string& custom_policy_table = "placement-test";
  const yb::client::YBTableName placement_table(
    YQL_DATABASE_CQL, table_name().namespace_name(), custom_policy_table);
  ASSERT_OK(client_->CreateNamespaceIfNotExists(
    placement_table.namespace_name(),
    placement_table.namespace_type()));

  yb::client::YBSchemaBuilder b;
  yb::client::YBSchema schema;
  b.AddColumn("k")->Type(BINARY)->NotNull()->HashPrimaryKey();
  b.AddColumn("v")->Type(BINARY)->NotNull();
  ASSERT_OK(b.Build(&schema));

  ASSERT_OK(NewTableCreator()->table_name(placement_table).schema(&schema).Create());

  WaitForLoadBalancer();

  // Modify the placement info for the table.
  ASSERT_OK(yb_admin_client_->ModifyTablePlacementInfo(placement_table, "c.r.z1,c.r.z2", 3, ""));

  WaitForLoadBalancer();

  // Test 1: Verify placement of tablets for the table with modified placement info.
  vector<int> counts_per_ts;
  GetLoadOnTservers(custom_policy_table, num_tservers, &counts_per_ts);
  // ts0 in c.r.z0 should have no tablets in it.
  ASSERT_EQ(counts_per_ts[0], 0);
  // The other tablet servers should have tablets spread equally.
  ASSERT_EQ(counts_per_ts[1], counts_per_ts[2]);
  ASSERT_EQ(counts_per_ts[2], counts_per_ts[3]);

  // The table with cluster placement policy should have tablets spread across all tservers.
  GetLoadOnTservers(table_name().table_name(), num_tservers, &counts_per_ts);
  for (int ii = 0; ii < num_tservers; ++ii) {
    ASSERT_GT(counts_per_ts[ii], 0);
  }

  // Test 2: Verify that custom placement info is honored when tservers are added.
  // Add two new tservers in both z0 and z2.
  ++num_tservers;
  AddNewTserverToZone("z0", num_tservers);

  ++num_tservers;
  AddNewTserverToZone("z2", num_tservers);

  WaitForLoadBalancer();

  GetLoadOnTservers(custom_policy_table, num_tservers, &counts_per_ts);
  for (int ii = 0; ii < num_tservers; ++ii) {
    if (ii == 0 || ii == 4) {
      // The table with custom policy should have no tablets in z0, i.e. ts0 and ts4.
      ASSERT_EQ(counts_per_ts[ii], 0);
      continue;
    }
    // The other tablet servers should have tablets in them.
    ASSERT_GT(counts_per_ts[ii], 0);
  }

  // Test 3: Verify that custom placement info is honored when tservers are removed.
  ASSERT_OK(external_mini_cluster()->AddTServerToBlacklist(
    external_mini_cluster()->master(),
    external_mini_cluster()->tablet_server(4)));
  ASSERT_OK(external_mini_cluster()->AddTServerToBlacklist(
    external_mini_cluster()->master(),
    external_mini_cluster()->tablet_server(5)));
  WaitForLoadBalancer();

  num_tservers -= 2;
  GetLoadOnTservers(custom_policy_table, num_tservers, &counts_per_ts);
  // ts0 in c.r.z0 should have no tablets in it.
  ASSERT_EQ(counts_per_ts[0], 0);
  // The other tablet servers should have tablets spread equally.
  ASSERT_EQ(counts_per_ts[1], counts_per_ts[2]);
  ASSERT_EQ(counts_per_ts[2], counts_per_ts[3]);

  // The table with cluster placement policy should continue to have tablets spread across all
  // tservers.
  GetLoadOnTservers(table_name().table_name(), num_tservers, &counts_per_ts);
  for (int ii = 0; ii < num_tservers; ++ii) {
    ASSERT_GT(counts_per_ts[ii], 0);
  }
}

TEST_F(LoadBalancerPlacementPolicyTest, AlterPlacementDataConsistencyTest) {
  // Set cluster placement policy.
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1", 2, ""));

  // Start workload on a table.
  const string& table = "placement-data-consistency-test";
  const yb::client::YBTableName placement_table(
    YQL_DATABASE_CQL, table_name().namespace_name(), table);

  TestWorkload workload(external_mini_cluster());
  workload.set_table_name(placement_table);
  workload.Setup();
  workload.Start();

  // Change its placement policy such that it now has additional replicas spanning additional
  // tservers.
  ASSERT_OK(yb_admin_client_->ModifyTablePlacementInfo(
      placement_table, "c.r.z0,c.r.z1,c.r.z2", 3, ""));
  WaitForLoadBalancer();

  // Verify that the placement policy is honored.
  vector<int> counts_per_ts;
  GetLoadOnTservers(table, num_tablet_servers(), &counts_per_ts);
  for (int ii = 0; ii < 3; ++ii) {
    ASSERT_EQ(counts_per_ts[ii], 1);
  }

  // Change placement policy such that it now spans lesser replicas spanning fewer tservers.
  ASSERT_OK(yb_admin_client_->ModifyTablePlacementInfo(placement_table, "c.r.z0", 1, ""));
  WaitForLoadBalancer();

  // Verify that placement policy is honored.
  GetLoadOnTservers(table, num_tablet_servers(), &counts_per_ts);
  // The table is RF1 and confined to zone 0. Ts0 should have 1 tablet.
  // The other two tablet servers should not have any tablets.
  ASSERT_EQ(counts_per_ts[0], 1);
  ASSERT_EQ(counts_per_ts[1], 0);
  ASSERT_EQ(counts_per_ts[2], 0);

  // Verify that the data inserted is still sane.
  workload.StopAndJoin();
  int rows_inserted = workload.rows_inserted();
  LOG(INFO) << "Number of rows inserted: " << rows_inserted;

  // Verify that number of rows is as expected.
  ClusterVerifier cluster_verifier(external_mini_cluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(
    placement_table, ClusterVerifier::EXACTLY, rows_inserted));
}

TEST_F(LoadBalancerPlacementPolicyTest, ModifyPlacementUUIDTest) {
  // Set cluster placement policy.
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1,c.r.z2", 3, ""));

  // Add 2 tservers with custom placement uuid.
  int num_tservers = num_tablet_servers() + 1;
  const string& random_placement_uuid = "19dfa091-2b53-434f-b8dc-97280a5f8831";
  AddNewTserverToZone("z1", num_tservers, random_placement_uuid);
  AddNewTserverToZone("z2", ++num_tservers, random_placement_uuid);

  vector<int> counts_per_ts;
  GetLoadOnTservers(table_name().table_name(), num_tservers, &counts_per_ts);

  // The first 3 tservers should have equal number of tablets allocated to them, but the new
  // tservers should not.
  for (int ii = 0; ii < 3; ++ii) {
    ASSERT_EQ(counts_per_ts[ii], 4);
  }
  ASSERT_EQ(counts_per_ts[3], 0);
  ASSERT_EQ(counts_per_ts[4], 0);

  // Now there are 2 tservers with custom placement_uuid and 3 tservers with default placement_uuid.
  // Modify the cluster config to have new placement_uuid matching the new tservers.
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo(
    "c.r.z0,c.r.z1,c.r.z2", 2, random_placement_uuid));

  // Change the table placement policy and verify that the change reflected.
  ASSERT_OK(yb_admin_client_->ModifyTablePlacementInfo(
    table_name(), "c.r.z1,c.r.z2", 2, random_placement_uuid));
  WaitForLoadBalancer();

  // There must now be tablets on the new tservers.
  GetLoadOnTservers(table_name().table_name(), num_tservers, &counts_per_ts);
  ASSERT_EQ(counts_per_ts[3], 4);
  ASSERT_EQ(counts_per_ts[4], 4);

  // Modify the placement policy with different zones and replication factor but with same
  // placement uuid.
  ASSERT_OK(yb_admin_client_->ModifyTablePlacementInfo(
    table_name(), "c.r.z2", 1, random_placement_uuid));
  WaitForLoadBalancer();

  GetLoadOnTservers(table_name().table_name(), num_tservers, &counts_per_ts);
  // TS3 belongs to zone1 and will have 0 tablets whereas since TS4 is in zone2 it should have 4
  // tablets allotted to it.
  ASSERT_EQ(counts_per_ts[3], 0);
  ASSERT_EQ(counts_per_ts[4], 4);

}

} // namespace integration_tests
} // namespace yb
