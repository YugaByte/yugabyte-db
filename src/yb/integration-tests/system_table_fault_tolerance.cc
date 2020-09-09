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

#include "yb/client/client.h"
#include "yb/client/meta_data_cache.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/yql/cql/ql/ql_processor.h"
#include "yb/yql/cql/ql/util/statement_params.h"
#include "yb/util/test_util.h"
#include "yb/util/async_util.h"

namespace yb {
namespace master {

class SystemTableFaultTolerance : public YBTest {
 public:
  SystemTableFaultTolerance() {}

  ~SystemTableFaultTolerance() {}

  void RunAsyncDone(
      Callback<void(const Status&)> cb, const Status& s,
      const ql::ExecutedResult::SharedPtr& result = nullptr) {
    cb.Run(s);
  }

 protected:
  void SetUp() override {
    YBTest::SetUp();
  }

  void TearDown() override {
    if (cluster_) {
      cluster_->Shutdown();
      cluster_.reset();
    }
    YBTest::TearDown();
  }

  std::unique_ptr<ExternalMiniCluster> cluster_;

  void SetupCluster(uint16_t rpc_port, std::vector<std::string> extra_master_flags = {}) {
    ExternalMiniClusterOptions opts;
    opts.master_rpc_ports = { rpc_port }; // external mini-cluster Start() gets the free ports.
    opts.num_masters = 1;
    opts.num_tablet_servers = 1;
    opts.extra_master_flags = extra_master_flags;
    // Master failovers should not be happening concurrently with us trying to load an initial sys
    // catalog snapshot. At least this is not supported as of 05/27/2019.
    opts.enable_ysql = false;
    cluster_.reset(new ExternalMiniCluster(opts));
  }
};

TEST_F(SystemTableFaultTolerance, TestFaultTolerance) {
  LOG(INFO) << "Start failure";
  SetupCluster(0, {"--TEST_catalog_manager_simulate_system_table_create_failure=true"});
  // Startup should fail due to injected failure.
  ASSERT_NOK(cluster_->Start());
  uint64_t rpc_port = cluster_->master()->bound_rpc_hostport().port();
  LOG(INFO) << "Shutdown";
  cluster_->Shutdown();

  // Now startup cluster without any failures.
  SetupCluster(rpc_port);
  LOG(INFO) << "Start normal";
  ASSERT_OK(cluster_->Start());

  // Check the system tables.
  client::YBClientBuilder builder;
  builder.add_master_server_addr(cluster_->GetLeaderMaster()->bound_rpc_hostport().ToString());
  builder.default_rpc_timeout(MonoDelta::FromSeconds(10))
    .default_admin_operation_timeout(MonoDelta::FromSeconds(10));
  auto client = ASSERT_RESULT(builder.Build());

  LOG(INFO) << "Read";
  auto metadata_cache = std::make_shared<client::YBMetaDataCache>(client.get(),
      false /* Update roles' permissions cache */);
  server::ClockPtr clock(new server::HybridClock());
  ASSERT_OK(clock->Init());
  auto processor = std::make_unique<ql::QLProcessor>(
      client.get(), metadata_cache, /* ql_metrics= */ nullptr,
      /* parser_pool= */ nullptr, clock, ql::TransactionPoolProvider());
  Synchronizer s;
  ql::StatementParameters statement_parameters;
  processor->RunAsync("SELECT * from system.peers", statement_parameters,
                      Bind(&SystemTableFaultTolerance::RunAsyncDone, Unretained(this),
                           Bind(&Synchronizer::StatusCB, Unretained(&s))));
  ASSERT_OK(s.Wait());
}

} // namespace master
} // namespace yb
