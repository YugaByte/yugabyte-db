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

#include "yb/yql/pgwrapper/pg_mini_test_base.h"

#include "yb/master/sys_catalog_initialization.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"

#include "yb/util/random_util.h"

DECLARE_bool(enable_ysql);
DECLARE_bool(hide_pg_catalog_table_creation_logs);
DECLARE_bool(master_auto_run_initdb);
DECLARE_int32(client_read_write_timeout_ms);
DECLARE_int32(pggate_rpc_timeout_secs);
DECLARE_int32(pgsql_proxy_webserver_port);
DECLARE_int32(ysql_num_shards_per_tserver);

namespace yb {
namespace pgwrapper {

void PgMiniTestBase::DoTearDown() {
  pg_supervisor_->Stop();
  YBMiniClusterTestBase::DoTearDown();
}

void PgMiniTestBase::SetUp() {
  HybridTime::TEST_SetPrettyToString(true);

  constexpr int kNumMasters = 1;

  FLAGS_client_read_write_timeout_ms = 120000;
  FLAGS_enable_ysql = true;
  FLAGS_hide_pg_catalog_table_creation_logs = true;
  FLAGS_master_auto_run_initdb = true;
  FLAGS_pggate_rpc_timeout_secs = 120;
  FLAGS_ysql_num_shards_per_tserver = 1;

  master::SetDefaultInitialSysCatalogSnapshotFlags();
  YBMiniClusterTestBase::SetUp();

  MiniClusterOptions mini_cluster_opt(kNumMasters, NumTabletServers());
  cluster_ = std::make_unique<MiniCluster>(env_.get(), mini_cluster_opt);
  ASSERT_OK(cluster_->Start());

  ASSERT_OK(WaitForInitDb(cluster_.get()));

  auto pg_ts = RandomElement(cluster_->mini_tablet_servers());
  auto port = cluster_->AllocateFreePort();
  PgProcessConf pg_process_conf = ASSERT_RESULT(PgProcessConf::CreateValidateAndRunInitDb(
      yb::ToString(Endpoint(pg_ts->bound_rpc_addr().address(), port)),
      pg_ts->options()->fs_opts.data_paths.front() + "/pg_data",
      pg_ts->server()->GetSharedMemoryFd()));

  pg_process_conf.master_addresses = pg_ts->options()->master_addresses_flag;
  pg_process_conf.force_disable_log_file = true;
  FLAGS_pgsql_proxy_webserver_port = cluster_->AllocateFreePort();

  LOG(INFO) << "Starting PostgreSQL server listening on "
            << pg_process_conf.listen_addresses << ":" << pg_process_conf.pg_port << ", data: "
            << pg_process_conf.data_dir
            << ", pgsql webserver port: " << FLAGS_pgsql_proxy_webserver_port;

  BeforePgProcessStart();
  pg_supervisor_ = std::make_unique<PgSupervisor>(pg_process_conf);
  ASSERT_OK(pg_supervisor_->Start());

  pg_host_port_ = HostPort(pg_process_conf.listen_addresses, pg_process_conf.pg_port);

  DontVerifyClusterBeforeNextTearDown();
}

} // namespace pgwrapper
} // namespace yb
