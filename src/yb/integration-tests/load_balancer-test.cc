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

class LoadBalancerTest : public YBTableTestBase {
 protected:
  bool use_yb_admin_client() override { return true; }

  bool use_external_mini_cluster() override { return true; }

  int num_tablets() override {
    return 4;
  }

  bool enable_ysql() override {
    // Do not create the transaction status table.
    return false;
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

  void CustomizeExternalMiniCluster(ExternalMiniClusterOptions* opts) override {
    opts->extra_tserver_flags.push_back("--placement_cloud=c");
    opts->extra_tserver_flags.push_back("--placement_region=r");
    opts->extra_tserver_flags.push_back("--placement_zone=z${index}");
    opts->extra_master_flags.push_back("--load_balancer_skip_leader_as_remove_victim=false");
  }

};

TEST_F(LoadBalancerTest, PreferredZoneAddNode) {
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1,c.r.z2", 3, ""));
  ASSERT_OK(yb_admin_client_->SetPreferredZones({"c.r.z1"}));

  ASSERT_OK(WaitFor([&]() {
    return AreLeadersOnPreferredOnly();
  }, kDefaultTimeout, "AreLeadersOnPreferredOnly"));

  std::vector<std::string> extra_opts;
  extra_opts.push_back("--placement_cloud=c");
  extra_opts.push_back("--placement_region=r");
  extra_opts.push_back("--placement_zone=z1");
  ASSERT_OK(external_mini_cluster()->AddTabletServer(true, extra_opts));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalanced(num_tablet_servers() + 1);
  },  kDefaultTimeout * 2, "IsLoadBalanced"));

  auto firstLoad = ASSERT_RESULT(GetLoadOnTserver(external_mini_cluster()->tablet_server(1)));
  auto secondLoad = ASSERT_RESULT(GetLoadOnTserver(external_mini_cluster()->tablet_server(3)));
  // Now assert that both tablet servers in zone z1 have the same count.
  ASSERT_EQ(firstLoad, secondLoad);
}

// Test load balancer idle / active:
// 1. Add tserver.
// 2. Check that load balancer becomes active and completes balancing load.
// 3. Delete table should not activate the load balancer. Not triggered through LB.
TEST_F(LoadBalancerTest, IsLoadBalancerIdle) {
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1,c.r.z2", 3, ""));

  std::vector<std::string> extra_opts;
  extra_opts.push_back("--placement_cloud=c");
  extra_opts.push_back("--placement_region=r");
  extra_opts.push_back("--placement_zone=z1");
  ASSERT_OK(external_mini_cluster()->AddTabletServer(true, extra_opts));
  ASSERT_OK(external_mini_cluster()->WaitForTabletServerCount(num_tablet_servers() + 1,
      kDefaultTimeout));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    bool is_idle = VERIFY_RESULT(client_->IsLoadBalancerIdle());
    return !is_idle;
  },  kDefaultTimeout * 2, "IsLoadBalancerActive"));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalancerIdle();
  },  kDefaultTimeout * 2, "IsLoadBalancerIdle"));

  YBTableTestBase::DeleteTable();
  // Assert that this times out.
  ASSERT_NOK(WaitFor([&]() -> Result<bool> {
    bool is_idle = VERIFY_RESULT(client_->IsLoadBalancerIdle());
    return !is_idle;
  },  MonoDelta::FromMilliseconds(10000), "IsLoadBalancerActive"));
}

// This regression test is to check that we don't hit the CHECK in cluster_balance.cc
//  state_->pending_stepdown_leader_tasks_[tablet->table()->id()].count(tablet->tablet_id()) == 0
// This CHECK was previously hit when load_balancer_max_concurrent_moves was set to a value > 1
// and multiple stepdown tasks were sent to the same tablet on subsequent LB runs.
TEST_F(LoadBalancerTest, PendingLeaderStepdownRegressTest) {
  const int test_bg_task_wait_ms = 1000;
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1,c.r.z2", 3, ""));
  ASSERT_OK(yb_admin_client_->SetPreferredZones({"c.r.z1"}));

  // Move all leaders to one tablet.
  ASSERT_OK(WaitFor([&]() {
    return AreLeadersOnPreferredOnly();
  }, kDefaultTimeout, "AreLeadersOnPreferredOnly"));

  // Allow for multiple leader moves per table.
  for (int i = 0; i < num_masters(); ++i) {
    ASSERT_OK(external_mini_cluster_->SetFlag(external_mini_cluster_->master(i),
                                              "load_balancer_max_concurrent_moves", "10"));
    ASSERT_OK(external_mini_cluster_->SetFlag(external_mini_cluster_->master(i),
                                              "load_balancer_max_concurrent_moves_per_table", "5"));
    ASSERT_OK(external_mini_cluster_->SetFlag(external_mini_cluster_->master(i),
                                              "catalog_manager_bg_task_wait_ms",
                                              std::to_string(test_bg_task_wait_ms)));
  }
  // Add stepdown delay of 2 * catalog_manager_bg_task_wait_ms.
  // This ensures that we will have pending stepdown tasks during a subsequent LB run.
  for (int i = 0; i < num_tablet_servers(); ++i) {
    ASSERT_OK(external_mini_cluster_->SetFlag(external_mini_cluster_->tablet_server(i),
                                              "TEST_leader_stepdown_delay_ms",
                                              std::to_string(2 * test_bg_task_wait_ms)));
  }

  // Trigger leader balancing.
  ASSERT_OK(yb_admin_client_->SetPreferredZones({"c.r.z0", "c.r.z1", "c.r.z2"}));

  // Wait for load balancing to complete.
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    bool is_idle = VERIFY_RESULT(client_->IsLoadBalancerIdle());
    return !is_idle;
  },  kDefaultTimeout * 2, "IsLoadBalancerActive"));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalancerIdle();
  },  kDefaultTimeout * 2, "IsLoadBalancerIdle"));
}

} // namespace integration_tests
} // namespace yb
