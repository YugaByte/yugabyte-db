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

using namespace std::literals;

namespace yb {
namespace integration_tests {

const MonoDelta kDefaultTimeout = 30000ms;

class LoadBalancerRespectAffinityTest : public YBTableTestBase {
 protected:
  void SetUp() override {
    YBTableTestBase::SetUp();

    yb_admin_client_ = std::make_unique<tools::enterprise::ClusterAdminClient>(
        external_mini_cluster()->GetMasterAddresses(), kDefaultTimeout);

    ASSERT_OK(yb_admin_client_->Init());
  }

  bool use_external_mini_cluster() override { return true; }

  int num_masters() override {
    return 3;
  }

  int num_tablet_servers() override {
    return 3;
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

  void CustomizeExternalMiniCluster(ExternalMiniClusterOptions* opts) override {
    opts->extra_tserver_flags.push_back("--placement_cloud=c");
    opts->extra_tserver_flags.push_back("--placement_region=r");
    opts->extra_tserver_flags.push_back("--placement_zone=z${index}");
    opts->extra_tserver_flags.push_back("--transaction_tables_use_preferred_zones=false");
  }

  std::unique_ptr<tools::enterprise::ClusterAdminClient> yb_admin_client_;
};

TEST_F(LoadBalancerRespectAffinityTest,
       YB_DISABLE_TEST_IN_TSAN(TransactionUsePreferredZones)) {
  ASSERT_OK(yb_admin_client_->ModifyPlacementInfo("c.r.z0,c.r.z1,c.r.z2", 3, ""));
  ASSERT_OK(yb_admin_client_->SetPreferredZones({"c.r.z1"}));

  // First test whether load is correctly balanced when transaction tablet leaders are not
  // using preferred zones.
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalanced(num_tablet_servers());
  }, kDefaultTimeout * 2, "IsLoadBalanced"));

  ASSERT_OK(WaitFor([&]() {
    return AreLeadersOnPreferredOnly();
  }, kDefaultTimeout, "AreLeadersOnPreferredOnly"));

  // Now test that once setting this gflag, after leader load re-balances all leaders are
  // in the preferred zone.
  for (ExternalDaemon* daemon : external_mini_cluster()->master_daemons()) {
    ASSERT_OK(external_mini_cluster()->
      SetFlag(daemon, "transaction_tables_use_preferred_zones", "1"));
  }

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalanced(num_tablet_servers());
  },  kDefaultTimeout * 2, "IsLoadBalanced"));

  ASSERT_OK(WaitFor([&]() {
    return AreLeadersOnPreferredOnly();
  }, kDefaultTimeout, "AreLeadersOnPreferredOnly"));

  // Now test that toggling the gflag back to false rebalances the transaction tablet leaders
  // to not just be on preferred zones.
  for (ExternalDaemon* daemon : external_mini_cluster()->master_daemons()) {
    ASSERT_OK(external_mini_cluster()->
      SetFlag(daemon, "transaction_tables_use_preferred_zones", "0"));
  }

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalanced(num_tablet_servers());
  },  kDefaultTimeout * 2, "IsLoadBalanced"));

  ASSERT_OK(WaitFor([&]() {
    return AreLeadersOnPreferredOnly();
  }, kDefaultTimeout, "AreLeadersOnPreferredOnly"));
}

} // namespace integration_tests
} // namespace yb
