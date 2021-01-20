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
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/master-test-util.h"
#include "yb/master/master_fwd.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/master.proxy.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/tools/yb-admin_client.h"
#include "yb/tserver/tablet_server_options.h"
#include "yb/util/monotime.h"

DECLARE_bool(enable_load_balancing);
DECLARE_int32(catalog_manager_bg_task_wait_ms);
DECLARE_int32(TEST_slowdown_master_async_rpc_tasks_by_ms);
DECLARE_int32(TEST_load_balancer_wait_after_count_pending_tasks_ms);
DECLARE_int32(load_balancer_max_concurrent_moves);

using namespace std::literals;

namespace yb {
namespace integration_tests {

const auto kDefaultTimeout = 30000ms;

class LoadBalancerMiniClusterTest : public YBTableTestBase {
 protected:
  void SetUp() override {
    YBTableTestBase::SetUp();
  }

  bool use_yb_admin_client() override { return true; }

  bool use_external_mini_cluster() override { return false; }

  int num_tablets() override {
    return 4;
  }

  bool enable_ysql() override {
    // Do not create the transaction status table.
    return false;
  }
};

// See issue #6278. This test tests the segfault that used to occur during a rare race condition,
// where we would have an uninitialized TSDescriptor that we try to access.
// To trigger the race condition, we need a pending add task that gets completed after
// CountPendingTasksUnlocked, and for the tserver that the add is going to needs to be marked as
// not live before hitting AnalyzeTabletsUnlocked.
TEST_F(LoadBalancerMiniClusterTest, UninitializedTSDescriptorOnPendingAddTest) {
  const int test_bg_task_wait_ms = 5000;
  const int test_short_delay_ms = 100;
  // See MiniTabletServer::MiniTabletServer for default placement info.
  ASSERT_OK(yb_admin_client_->
      ModifyPlacementInfo("cloud1.rack1.zone,cloud1.rack2.zone,cloud2.rack3.zone", 3, ""));

  // Disable load balancing.
  SetAtomicFlag(false, &FLAGS_enable_load_balancing);

  // Increase the time between LB runs so we can better time things.
  SetAtomicFlag(test_bg_task_wait_ms, &FLAGS_catalog_manager_bg_task_wait_ms);
  // Set this to delay the add task so that we can have a pending add.
  SetAtomicFlag(test_bg_task_wait_ms + test_short_delay_ms,
                &FLAGS_TEST_slowdown_master_async_rpc_tasks_by_ms);
  // Insert a pause after finding pending tasks so that the race condition can be hit.
  SetAtomicFlag(4000, &FLAGS_TEST_load_balancer_wait_after_count_pending_tasks_ms);
    // Don't allow for leader moves, only want add tasks.
  SetAtomicFlag(0, &FLAGS_load_balancer_max_concurrent_moves);

  // Add new tserver in to force load balancer moves.
  tserver::TabletServerOptions extra_opts =
      ASSERT_RESULT(tserver::TabletServerOptions::CreateTabletServerOptions());
  // Important to set to cloud2 (see TEST_SetupConnectivity, we group servers in groups of two).
  extra_opts.SetPlacement("cloud2", "rack3", "zone");
  ASSERT_OK(mini_cluster()->AddTabletServer(extra_opts));
  ASSERT_OK(mini_cluster()->WaitForTabletServerCount(num_tablet_servers() + 1));
  const auto ts3_uuid = mini_cluster_->mini_tablet_server(3)->server()->permanent_uuid();

  // Re-enable the load balancer.
  SetAtomicFlag(true, &FLAGS_enable_load_balancing);

  // LB RUN #1 : Start an add server task.

  // LB RUN #2 : Count the pending add task, and then pause for
  //             FLAGS_TEST_load_balancer_wait_after_count_pending_tasks_ms.

  // Wait for the add task to be processed (at least one replica reporting on the new tserver).
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    scoped_refptr<master::TableInfo> tbl_info =
      mini_cluster()->leader_mini_master()->master()->catalog_manager()->
          GetTableInfoFromNamespaceNameAndTableName(table_name().namespace_type(),
                                                    table_name().namespace_name(),
                                                    table_name().table_name());
    vector<scoped_refptr<master::TabletInfo>> tablets;
    tbl_info->GetAllTablets(&tablets);
    bool foundReplica = false;
    for (const auto& tablet : tablets) {
      auto replica_map = tablet->GetReplicaLocations();
      if (replica_map->find(ts3_uuid) != replica_map->end()) {
        foundReplica = true;
        break;
      }
    }
    return foundReplica;
  }, MonoDelta::FromMilliseconds(test_bg_task_wait_ms * 2), "WaitForAddTaskToBeProcessed"));

  // Modify GetAllReportedDescriptors so that it does not report the new tserver
  // (this could happen normally from a late heartbeat).
  master::TSDescriptorVector ts_descs;
  master::TSDescriptorPtr ts3_desc;
  mini_cluster_->leader_mini_master()->master()->ts_manager()->GetAllReportedDescriptors(&ts_descs);
  for (const auto& ts_desc : ts_descs) {
    if (ts_desc->permanent_uuid() == ts3_uuid) {
      ts_desc->SetRemoved();
      ts3_desc = ts_desc;
      break;
    }
  }
  CHECK_NOTNULL(ts3_desc);

  // LB run #2 will now continue, with GetAllReportedDescriptors not reporting this tserver, but
  // with GetReplicaLocations reporting it.

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    bool is_idle = VERIFY_RESULT(client_->IsLoadBalancerIdle());
    return !is_idle;
  },  kDefaultTimeout * 2, "IsLoadBalancerActive"));

  // If the seg fault occurs, it will occur in the first 2 runs.
  SleepFor(MonoDelta::FromMilliseconds(3 * test_bg_task_wait_ms));

  // If it has yet to happen, bring back the tserver so that load balancing can complete.
  ts3_desc->SetRemoved(false);
  SetAtomicFlag(1000, &FLAGS_catalog_manager_bg_task_wait_ms);
  SetAtomicFlag(0, &FLAGS_TEST_slowdown_master_async_rpc_tasks_by_ms);
  SetAtomicFlag(0, &FLAGS_TEST_load_balancer_wait_after_count_pending_tasks_ms);

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalancerIdle();
  },  kDefaultTimeout * 2, "IsLoadBalancerIdle"));
}

} // namespace integration_tests
} // namespace yb
