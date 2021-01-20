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

#include "yb/integration-tests/yb_table_test_base.h"

#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_creator.h"
#include "yb/client/yb_op.h"

#include "yb/common/ql_value.h"

#include "yb/util/monotime.h"
#include "yb/yql/redis/redisserver/redis_parser.h"
#include "yb/yql/redis/redisserver/redis_constants.h"
#include "yb/util/curl_util.h"

DECLARE_bool(enable_ysql);

using std::unique_ptr;
using std::shared_ptr;

namespace yb {

using client::YBClient;
using client::YBClientBuilder;
using client::YBColumnSchema;
using client::YBSchemaBuilder;
using client::YBSession;
using client::YBTableCreator;
using client::YBTableType;
using client::YBTableName;
using strings::Substitute;

namespace integration_tests {

const YBTableName YBTableTestBase::kDefaultTableName(
    YQL_DATABASE_CQL, "my_keyspace", "kv-table-test");

int YBTableTestBase::num_masters() {
  return kDefaultNumMasters;
}

int YBTableTestBase::num_tablet_servers() {
  return kDefaultNumTabletServers;
}

int YBTableTestBase::num_tablets() {
  return CalcNumTablets(num_tablet_servers());
}

int YBTableTestBase::session_timeout_ms() {
  return kDefaultSessionTimeoutMs;
}

YBTableName YBTableTestBase::table_name() {
  if (table_names_.empty()) {
    table_names_.push_back(kDefaultTableName);
  }
  return table_names_[0];
}

bool YBTableTestBase::need_redis_table() {
  return true;
}

int YBTableTestBase::client_rpc_timeout_ms() {
  return kDefaultClientRpcTimeoutMs;
}

bool YBTableTestBase::use_external_mini_cluster() {
  return kDefaultUsingExternalMiniCluster;
}

bool YBTableTestBase::use_yb_admin_client() {
  return false;
}

bool YBTableTestBase::enable_ysql() {
  return kDefaultEnableYSQL;
}

YBTableTestBase::YBTableTestBase() {
}

void YBTableTestBase::BeforeCreateTable() {
}

void YBTableTestBase::SetUp() {
  YBTest::SetUp();

  Status mini_cluster_status;
  if (use_external_mini_cluster()) {
    auto opts = ExternalMiniClusterOptions();
    opts.num_masters = num_masters();
    opts.master_rpc_ports = master_rpc_ports();
    opts.num_tablet_servers = num_tablet_servers();
    opts.enable_ysql = enable_ysql();
    CustomizeExternalMiniCluster(&opts);

    external_mini_cluster_.reset(new ExternalMiniCluster(opts));
    mini_cluster_status = external_mini_cluster_->Start();
  } else {
    auto opts = MiniClusterOptions();
    SetAtomicFlag(enable_ysql(), &FLAGS_enable_ysql);
    opts.num_masters = num_masters();
    opts.num_tablet_servers = num_tablet_servers();

    mini_cluster_.reset(new MiniCluster(env_.get(), opts));
    mini_cluster_status = mini_cluster_->Start();
  }
  if (!mini_cluster_status.ok()) {
    // We sometimes crash during cleanup if the cluster creation fails and don't get to report
    // the root cause, so log it here just in case.
    LOG(INFO) << "Failed starting the mini cluster: " << mini_cluster_status.ToString();
  }
  ASSERT_OK(mini_cluster_status);

  CreateClient();
  CreateAdminClient();

  BeforeCreateTable();

  CreateTable();
  OpenTable();
}

void YBTableTestBase::TearDown() {
  DeleteTable();

  // Fetch the tablet server metrics page after we delete the table. [ENG-135].
  FetchTSMetricsPage();

  client_.reset();
  if (use_yb_admin_client()) {
    yb_admin_client_.reset();
  }
  if (use_external_mini_cluster()) {
    external_mini_cluster_->Shutdown();
  } else {
    mini_cluster_->Shutdown();
  }
  YBTest::TearDown();
}

vector<uint16_t> YBTableTestBase::master_rpc_ports() {
  vector<uint16_t> master_rpc_ports;
  for (int i = 0; i < num_masters(); ++i) {
    master_rpc_ports.push_back(0);
  }
  return master_rpc_ports;
}

void YBTableTestBase::CreateClient() {
  client_ = CreateYBClient();
}

std::unique_ptr<YBClient> YBTableTestBase::CreateYBClient() {
  YBClientBuilder builder;
  builder.default_rpc_timeout(MonoDelta::FromMilliseconds(client_rpc_timeout_ms()));
  if (use_external_mini_cluster()) {
    return CHECK_RESULT(external_mini_cluster_->CreateClient(&builder));
  } else {
    return CHECK_RESULT(mini_cluster_->CreateClient(&builder));
  }
}

void YBTableTestBase::CreateAdminClient() {
  if (use_yb_admin_client()) {
    string addrs;
    if (use_external_mini_cluster()) {
      addrs = external_mini_cluster_->GetMasterAddresses();
    }  else {
      addrs = mini_cluster_->GetMasterAddresses();
    }
    yb_admin_client_ = std::make_unique<tools::enterprise::ClusterAdminClient>(
        addrs, MonoDelta::FromMilliseconds(client_rpc_timeout_ms()));

    ASSERT_OK(yb_admin_client_->Init());
  }
}

void YBTableTestBase::OpenTable() {
  ASSERT_OK(table_.Open(table_name(), client_.get()));
  session_ = NewSession();
}

void YBTableTestBase::CreateRedisTable(const YBTableName& table_name) {
  CHECK(table_name.namespace_type() == YQLDatabase::YQL_DATABASE_REDIS);

  ASSERT_OK(client_->CreateNamespaceIfNotExists(table_name.namespace_name(),
                                                table_name.namespace_type()));
  ASSERT_OK(NewTableCreator()->table_name(table_name)
                .table_type(YBTableType::REDIS_TABLE_TYPE)
                .num_tablets(CalcNumTablets(3))
                .Create());
}

void YBTableTestBase::CreateTable() {
  const auto tn = table_name();
  if (!table_exists_) {
    ASSERT_OK(client_->CreateNamespaceIfNotExists(tn.namespace_name(), tn.namespace_type()));

    YBSchemaBuilder b;
    b.AddColumn("k")->Type(BINARY)->NotNull()->HashPrimaryKey();
    b.AddColumn("v")->Type(BINARY)->NotNull();
    ASSERT_OK(b.Build(&schema_));

    ASSERT_OK(NewTableCreator()->table_name(tn).schema(&schema_).Create());
    table_exists_ = true;
  }
}

void YBTableTestBase::DeleteTable() {
  if (table_exists_) {
    ASSERT_OK(client_->DeleteTable(table_name()));
    table_exists_ = false;
  }
}

shared_ptr<YBSession> YBTableTestBase::NewSession() {
  shared_ptr<YBSession> session = client_->NewSession();
  session->SetTimeout(MonoDelta::FromMilliseconds(session_timeout_ms()));
  return session;
}

void YBTableTestBase::PutKeyValue(YBSession* session, string key, string value) {
  auto insert = table_.NewInsertOp();
  QLAddStringHashValue(insert->mutable_request(), key);
  table_.AddStringColumnValue(insert->mutable_request(), "v", value);
  ASSERT_OK(session->ApplyAndFlush(insert));
}

void YBTableTestBase::PutKeyValue(string key, string value) {
  PutKeyValue(session_.get(), key, value);
}

void YBTableTestBase::RestartCluster() {
  DCHECK(!use_external_mini_cluster());
  CHECK_OK(mini_cluster_->RestartSync());
  ASSERT_NO_FATALS(CreateClient());
  ASSERT_NO_FATALS(OpenTable());
}

Result<std::shared_ptr<master::MasterServiceProxy>> YBTableTestBase::GetMasterLeaderProxy() {
  DCHECK(use_external_mini_cluster());
  int idx;
  RETURN_NOT_OK(external_mini_cluster_->GetLeaderMasterIndex(&idx));
  return external_mini_cluster_->master_proxy(idx);
}

Result<std::vector<uint32_t>> YBTableTestBase::GetTserverLoads(const std::vector<int>& ts_idxs) {
  std::vector<uint32_t> tserver_loads;
  for (const auto& ts_idx : ts_idxs) {
    tserver_loads.push_back(
        VERIFY_RESULT(GetLoadOnTserver(external_mini_cluster_->tablet_server(ts_idx))));
  }
  return tserver_loads;
}

Result<uint32_t> YBTableTestBase::GetLoadOnTserver(ExternalTabletServer* server) {
  auto proxy = VERIFY_RESULT(GetMasterLeaderProxy());
  uint32_t count = 0;
  std::vector<string> replicas;
  // Need to get load from each table.
  for (const auto& table_name : table_names_) {
    master::GetTableLocationsRequestPB req;
    if (table_name.namespace_type() == YQL_DATABASE_PGSQL) {
      // Use table_id/namespace_id for SQL tables.
      req.mutable_table()->set_table_id(table_name.table_id());
      req.mutable_table()->mutable_namespace_()->set_id(table_name.namespace_id());
    } else {
      req.mutable_table()->set_table_name(table_name.table_name());
      req.mutable_table()->mutable_namespace_()->set_name(table_name.namespace_name());
    }
    master::GetTableLocationsResponsePB resp;

    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromMilliseconds(client_rpc_timeout_ms()));
    RETURN_NOT_OK(proxy->GetTableLocations(req, &resp, &rpc));

    for (const auto& loc : resp.tablet_locations()) {
      for (const auto& replica : loc.replicas()) {
        if (replica.ts_info().permanent_uuid() == server->instance_id().permanent_uuid()) {
          replicas.push_back(loc.tablet_id());
          count++;
        }
      }
    }
  }

  LOG(INFO) << Format("For ts $0, tablets are $1 with count $2",
                      server->instance_id().permanent_uuid(), VectorToString(replicas), count);
  return count;
}

std::vector<std::pair<std::string, std::string>> YBTableTestBase::GetScanResults(
    const client::TableRange& range) {
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto& row : range) {
    result.emplace_back(row.column(0).binary_value(), row.column(1).binary_value());
  }
  std::sort(result.begin(), result.end());
  return result;
}

void YBTableTestBase::FetchTSMetricsPage() {
  EasyCurl c;
  faststring buf;

  string addr;
  // TODO: unify external and in-process mini cluster interfaces.
  if (use_external_mini_cluster()) {
    if (external_mini_cluster_->num_tablet_servers() > 0) {
      addr = external_mini_cluster_->tablet_server(0)->bound_http_hostport().ToString();
    }
  } else {
    if (mini_cluster_->num_tablet_servers() > 0) {
      addr = ToString(mini_cluster_->mini_tablet_server(0)->bound_http_addr());
    }
  }

  if (!addr.empty()) {
    LOG(INFO) << "Fetching metrics from " << addr;
    ASSERT_OK(c.FetchURL(Substitute("http://$0/metrics", addr), &buf));
  }
}

void YBTableTestBase::WaitForLoadBalanceCompletion(yb::MonoDelta timeout) {
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    bool is_idle = VERIFY_RESULT(client_->IsLoadBalancerIdle());
    return !is_idle;
  }, timeout, "IsLoadBalancerActive"));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return client_->IsLoadBalancerIdle();
  }, timeout, "IsLoadBalancerIdle"));
}

std::unique_ptr<client::YBTableCreator> YBTableTestBase::NewTableCreator() {
  unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  if (num_tablets() > 0) {
    table_creator->num_tablets(num_tablets());
  }
  table_creator->table_type(YBTableType::YQL_TABLE_TYPE);
  return table_creator;
}

}  // namespace integration_tests
}  // namespace yb
