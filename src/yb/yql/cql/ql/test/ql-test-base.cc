//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/test/ql-test-base.h"

#include "yb/client/client.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"

DECLARE_bool(use_cassandra_authentication);

namespace yb {
namespace ql {

using std::string;
using std::vector;
using std::shared_ptr;
using client::YBClient;
using client::YBSession;
using client::YBClientBuilder;

//--------------------------------------------------------------------------------------------------
const string QLTestBase::kDefaultKeyspaceName("my_keyspace");

QLTestBase::QLTestBase() {
}

QLTestBase::~QLTestBase() {
}

//--------------------------------------------------------------------------------------------------

void QLTestBase::CreateSimulatedCluster(int num_tablet_servers) {
  // Start mini-cluster with given number of tservers (default: 1), config client options
  MiniClusterOptions opts;
  opts.num_tablet_servers = num_tablet_servers;
  cluster_.reset(new MiniCluster(env_.get(), opts));
  ASSERT_OK(cluster_->Start());
  YBClientBuilder builder;
  builder.add_master_server_addr(cluster_->mini_master()->bound_rpc_addr_str());
  builder.default_rpc_timeout(MonoDelta::FromSeconds(30));
  builder.default_admin_operation_timeout(MonoDelta::FromSeconds(60));
  builder.set_tserver_uuid(cluster_->mini_tablet_server(0)->server()->permanent_uuid());
  client_ = ASSERT_RESULT(builder.Build());
  metadata_cache_ = std::make_shared<client::YBMetaDataCache>(client_.get(),
      false /* Update roles' permissions cache */);
  ASSERT_OK(client_->CreateNamespaceIfNotExists(kDefaultKeyspaceName));
}

//--------------------------------------------------------------------------------------------------
static void CallUseKeyspace(const TestQLProcessor::UniPtr& processor, const string& keyspace_name) {
  // Workaround: it's implemented as a separate function just because ASSERT_OK can
  // call 'return void;' what can be incompatible with another return type.
  ASSERT_OK(processor->UseKeyspace(keyspace_name));
}

TestQLProcessor* QLTestBase::GetQLProcessor(const RoleName& role_name) {
  if (!role_name.empty()) {
    CHECK(FLAGS_use_cassandra_authentication);
  }
  if (!client_) {
    CreateSimulatedCluster();
  }

  ql_processors_.emplace_back(new TestQLProcessor(client_.get(), metadata_cache_, role_name));
  CallUseKeyspace(ql_processors_.back(), kDefaultKeyspaceName);
  return ql_processors_.back().get();
}

}  // namespace ql
}  // namespace yb
