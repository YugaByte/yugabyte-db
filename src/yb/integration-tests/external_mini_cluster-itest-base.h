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

#ifndef YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_ITEST_BASE_H_
#define YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_ITEST_BASE_H_

#include <string>
#include <unordered_map>
#include <vector>
#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/gutil/stl_util.h"
#include "yb/integration-tests/cluster_itest_util.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/external_mini_cluster_fs_inspector.h"
#include "yb/util/pstack_watcher.h"
#include "yb/util/test_util.h"

namespace yb {

// Simple base utility class to provide an external mini cluster with common
// setup routines useful for integration tests.
class ExternalMiniClusterITestBase : public YBTest {
 public:
  virtual void SetUpCluster(ExternalMiniClusterOptions* opts) {
    // Fsync causes flakiness on EC2.
    CHECK_NOTNULL(opts)->extra_tserver_flags.push_back("--never_fsync");
  }

  virtual void TearDown() override {
    client_.reset();
    if (cluster_) {
      if (HasFatalFailure()) {
        LOG(INFO) << "Found fatal failure";
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
      cluster_->Shutdown();
    }
    YBTest::TearDown();
    ts_map_.clear();
  }

 protected:
  void StartCluster(const std::vector<std::string>& extra_ts_flags = std::vector<std::string>(),
                    const std::vector<std::string>& extra_master_flags = std::vector<std::string>(),
                    int num_tablet_servers = 3,
                    int num_masters = 1,
                    bool enable_ysql = false);

  gscoped_ptr<ExternalMiniCluster> cluster_;
  gscoped_ptr<itest::ExternalMiniClusterFsInspector> inspect_;
  std::unique_ptr<client::YBClient> client_;
  itest::TabletServerMap ts_map_;
};

void ExternalMiniClusterITestBase::StartCluster(const std::vector<std::string>& extra_ts_flags,
                                                const std::vector<std::string>& extra_master_flags,
                                                int num_tablet_servers,
                                                int num_masters,
                                                bool enable_ysql) {
  ExternalMiniClusterOptions opts;
  opts.num_masters = num_masters;
  opts.num_tablet_servers = num_tablet_servers;
  opts.extra_master_flags = extra_master_flags;
  opts.extra_tserver_flags = extra_ts_flags;
  opts.enable_ysql = enable_ysql;
  SetUpCluster(&opts);

  cluster_.reset(new ExternalMiniCluster(opts));
  ASSERT_OK(cluster_->Start());
  inspect_.reset(new itest::ExternalMiniClusterFsInspector(cluster_.get()));
  int master_leader = 0;
  ASSERT_OK(cluster_->GetLeaderMasterIndex(&master_leader));

  ASSERT_OK(itest::CreateTabletServerMap(cluster_->master_proxy(master_leader).get(),
                                         &cluster_->proxy_cache(),
                                         &ts_map_));
  client_ = ASSERT_RESULT(cluster_->CreateClient());
}

}  // namespace yb

#endif // YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_ITEST_BASE_H_
