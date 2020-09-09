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
#include "yb/client/table_creator.h"
#include "yb/client/yb_table_name.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/gutil/strings/join.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/master/master.h"
#include "yb/master/master-test-util.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/master.proxy.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/tools/yb-admin_client.h"
#include "yb/util/monotime.h"

using namespace std::literals;

namespace yb {
namespace integration_tests {

const auto kDefaultTimeout = 30000ms;
constexpr int kNumTables = 3;
constexpr int kMovesPerTable = 1;

// We need multiple tables in order to test load_balancer_max_concurrent_moves_per_table.
class LoadBalancerMultiTableTest : public YBTableTestBase {
 protected:
  void SetUp() override {
    YBTableTestBase::SetUp();

    yb_admin_client_ = std::make_unique<tools::enterprise::ClusterAdminClient>(
        external_mini_cluster()->GetMasterAddresses(), kDefaultTimeout);

    ASSERT_OK(yb_admin_client_->Init());
  }

  bool use_external_mini_cluster() override { return true; }

  bool enable_ysql() override {
    // Do not create the transaction status table.
    return false;
  }

  int num_tablets() override {
    return 8;
  }

  client::YBTableName table_name() override {
    return table_names_[0];
  }

  void CustomizeExternalMiniCluster(ExternalMiniClusterOptions* opts) override {
    opts->extra_tserver_flags.push_back("--placement_cloud=c");
    opts->extra_tserver_flags.push_back("--placement_region=r");
    opts->extra_tserver_flags.push_back("--placement_zone=z${index}");
    opts->extra_master_flags.push_back("--load_balancer_skip_leader_as_remove_victim=false");
    opts->extra_master_flags.push_back("--load_balancer_max_concurrent_moves=10");
    opts->extra_master_flags.push_back("--load_balancer_max_concurrent_moves_per_table="
                                       + std::to_string(kMovesPerTable));
  }

  void CreateTables() {
    for (int i = 1; i <= kNumTables; ++i) {
      table_names_.emplace_back(YQL_DATABASE_CQL,
                               "my_keyspace-" + std::to_string(i),
                               "kv-table-test-" + std::to_string(i));
    }

    for (const auto& tn : table_names_) {
      ASSERT_OK(client_->CreateNamespaceIfNotExists(tn.namespace_name(), tn.namespace_type()));

      client::YBSchemaBuilder b;
      b.AddColumn("k")->Type(BINARY)->NotNull()->HashPrimaryKey();
      b.AddColumn("v")->Type(BINARY)->NotNull();
      ASSERT_OK(b.Build(&schema_));

      ASSERT_OK(NewTableCreator()->table_name(tn).schema(&schema_).Create());
    }
  }

  void DeleteTables() {
    for (const auto& tn : table_names_) {
      ASSERT_OK(client_->DeleteTable(tn));
    }
    table_names_.clear();
  }

  void CreateTable() override {
    if (!table_exists_) {
      CreateTables();
      table_exists_ = true;
    }
  }

  void DeleteTable() override {
    if (table_exists_) {
      DeleteTables();
      table_exists_ = false;
    }
  }

  Result<bool> AreLeadersOnPreferredOnly() {
    master::AreLeadersOnPreferredOnlyRequestPB req;
    master::AreLeadersOnPreferredOnlyResponsePB resp;
    rpc::RpcController rpc;
    rpc.set_timeout(kDefaultTimeout);
    auto proxy = VERIFY_RESULT(GetMasterLeaderProxy());
    RETURN_NOT_OK(proxy->AreLeadersOnPreferredOnly(req, &resp, &rpc));
    return !resp.has_error();
  }

  Result<std::shared_ptr<master::MasterServiceProxy>> GetMasterLeaderProxy() {
    int idx;
    RETURN_NOT_OK(external_mini_cluster()->GetLeaderMasterIndex(&idx));
    return external_mini_cluster()->master_proxy(idx);
  }

  std::unique_ptr<tools::enterprise::ClusterAdminClient> yb_admin_client_;
  vector<client::YBTableName> table_names_;
};

TEST_F(LoadBalancerMultiTableTest, MultipleLeaderTabletMovesPerTable) {
  const int test_bg_task_wait_ms = 5000;

  // Start with 3 tables each with 8 tablets on 3 servers.
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1,c.r.z2", 3, ""));

  // Disable leader balancing.
  for (int i = 0; i < num_masters(); ++i) {
    ASSERT_OK(external_mini_cluster_->SetFlag(external_mini_cluster_->master(i),
                                              "load_balancer_max_concurrent_moves", "0"));
  }

  // Add new tserver.
  std::vector<std::string> extra_opts;
  extra_opts.push_back("--placement_cloud=c");
  extra_opts.push_back("--placement_region=r");
  extra_opts.push_back("--placement_zone=z1");
  ASSERT_OK(external_mini_cluster()->AddTabletServer(true, extra_opts));
  ASSERT_OK(external_mini_cluster()->WaitForTabletServerCount(num_tablet_servers() + 1,
      kDefaultTimeout));

  // Wait for load balancing to complete.
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    bool is_idle = VERIFY_RESULT(client_->IsLoadBalancerIdle());
    return !is_idle;
  },  kDefaultTimeout * 2, "IsLoadBalancerActive"));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalancerIdle();
  },  kDefaultTimeout * 2, "IsLoadBalancerIdle"));

  // Get current leader counts.
  unordered_map<string, unordered_map<string, int>> initial_leader_counts;
  for (const auto& tn : table_names_) {
    initial_leader_counts[tn.table_name()] = ASSERT_RESULT(yb_admin_client_->GetLeaderCounts(tn));
  }

  // Enable leader balancing and also increase LB run delay.
  LOG(INFO) << "Re-enabling leader balancing.";
  for (int i = 0; i < num_masters(); ++i) {
    ASSERT_OK(external_mini_cluster_->SetFlag(external_mini_cluster_->master(i),
                                              "load_balancer_max_concurrent_moves", "10"));
    ASSERT_OK(external_mini_cluster_->SetFlag(external_mini_cluster_->master(i),
                                              "catalog_manager_bg_task_wait_ms",
                                              std::to_string(test_bg_task_wait_ms)));
  }

  // Wait for one run of the load balancer to complete
  SleepFor(MonoDelta::FromMilliseconds(test_bg_task_wait_ms));

  // Check new leader counts.
  int num_leader_moves = 0;
  for (const auto& tn : table_names_) {
    const auto new_leader_counts = ASSERT_RESULT(yb_admin_client_->GetLeaderCounts(tn));
    // Only count increases in leaders
    for (const auto& lc : new_leader_counts) {
      num_leader_moves += max(0, lc.second - initial_leader_counts[tn.table_name()][lc.first]);
    }
  }

  // Ensure that we moved one run's worth of leaders.
  LOG(INFO) << "Moved " << num_leader_moves << " leaders in total.";
  ASSERT_EQ(num_leader_moves, kMovesPerTable * kNumTables);
}

} // namespace integration_tests
} // namespace yb
