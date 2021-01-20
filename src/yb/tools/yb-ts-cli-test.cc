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
// Tests for the yb-admin command-line tool.

#include <boost/assign/list_of.hpp>
#include <gtest/gtest.h>

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/split.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/cluster_itest_util.h"
#include "yb/integration-tests/external_mini_cluster-itest-base.h"
#include "yb/integration-tests/test_workload.h"
#include "yb/util/path_util.h"
#include "yb/util/subprocess.h"

using boost::assign::list_of;
using yb::itest::TabletServerMap;
using yb::itest::TServerDetails;
using strings::Split;
using strings::Substitute;

constexpr int kTabletTimeout = 30;

namespace yb {
namespace tools {

static const char* const kTsCliToolName = "yb-ts-cli";

class YBTsCliTest : public ExternalMiniClusterITestBase {
 protected:
  // Figure out where the admin tool is.
  string GetTsCliToolPath() const;
};

string YBTsCliTest::GetTsCliToolPath() const {
  return GetToolPath(kTsCliToolName);
}

// Test deleting a tablet.
TEST_F(YBTsCliTest, TestDeleteTablet) {
  MonoDelta timeout = MonoDelta::FromSeconds(kTabletTimeout);
  std::vector<std::string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
  };
  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags));

  TestWorkload workload(cluster_.get());
  workload.Setup(); // Easy way to create a new tablet.

  vector<tserver::ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  for (const auto& entry : ts_map_) {
    TServerDetails* ts = entry.second.get();
    ASSERT_OK(itest::WaitForNumTabletsOnTS(ts, 1, timeout, &tablets));
  }
  string tablet_id = tablets[0].tablet_status().tablet_id();

  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  string exe_path = GetTsCliToolPath();
  vector<string> argv;
  argv.push_back(exe_path);
  argv.push_back("--server_address");
  argv.push_back(yb::ToString(cluster_->tablet_server(0)->bound_rpc_addr()));
  argv.push_back("delete_tablet");
  argv.push_back(tablet_id);
  argv.push_back("Deleting for yb-ts-cli-test");
  ASSERT_OK(Subprocess::Call(argv));

  ASSERT_OK(inspect_->WaitForTabletDataStateOnTS(0, tablet_id, tablet::TABLET_DATA_TOMBSTONED));
  TServerDetails* ts = ts_map_[cluster_->tablet_server(0)->uuid()].get();
  ASSERT_OK(itest::WaitUntilTabletInState(ts, tablet_id, tablet::SHUTDOWN, timeout));
}

// Test readiness check before and after a tablet server is done bootstrapping.
TEST_F(YBTsCliTest, TestTabletServerReadiness) {
  MonoDelta timeout = MonoDelta::FromSeconds(kTabletTimeout);
  ASSERT_NO_FATALS(StartCluster({ "--TEST_tablet_bootstrap_delay_ms=2000"s }));

  TestWorkload workload(cluster_.get());
  workload.Setup(); // Easy way to create a new tablet.

  vector<tserver::ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  for (const auto& entry : ts_map_) {
    TServerDetails* ts = entry.second.get();
    ASSERT_OK(itest::WaitForNumTabletsOnTS(ts, 1, timeout, &tablets));
  }
  string tablet_id = tablets[0].tablet_status().tablet_id();

  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    ASSERT_OK(itest::WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                            tablet_id, timeout));
  }

  ASSERT_NO_FATALS(cluster_->tablet_server(0)->Shutdown());
  ASSERT_OK(cluster_->tablet_server(0)->Restart());

  string exe_path = GetTsCliToolPath();
  vector<string> argv;
  argv.push_back(exe_path);
  argv.push_back("--server_address");
  argv.push_back(yb::ToString(cluster_->tablet_server(0)->bound_rpc_addr()));
  argv.push_back("is_server_ready");
  ASSERT_NOK(Subprocess::Call(argv));

  ASSERT_OK(WaitFor([&]() {
    return Subprocess::Call(argv).ok();
  }, MonoDelta::FromSeconds(10), "Wait for tablet bootstrap to finish"));
}

} // namespace tools
} // namespace yb
