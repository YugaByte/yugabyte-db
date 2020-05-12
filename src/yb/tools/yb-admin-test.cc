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

#include <regex>

#include <boost/algorithm/string.hpp>

#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/client/table_creator.h"

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/integration-tests/test_workload.h"
#include "yb/integration-tests/ts_itest-base.h"
#include "yb/master/master_defaults.h"

#include "yb/util/net/net_util.h"
#include "yb/util/port_picker.h"
#include "yb/util/stol_utils.h"
#include "yb/util/string_trim.h"
#include "yb/util/string_util.h"
#include "yb/util/subprocess.h"
#include "yb/util/test_util.h"

namespace yb {
namespace tools {

using client::YBClient;
using client::YBClientBuilder;
using client::YBTableName;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBTableCreator;
using client::YBTableType;
using std::shared_ptr;
using std::vector;
using itest::TabletServerMap;
using itest::TServerDetails;
using strings::Substitute;

namespace {

static const char* const kAdminToolName = "yb-admin";

//  Helper to check hosts list by requesting cluster config via yb-admin and parse its output:
//
//  Config:
//  version: 1
//  server_blacklist {
//    hosts {
//      host: "node1"
//      port: 9100
//    }
//    hosts {
//      host: "node2"
//      port: 9100
//    }
//    initial_replica_load: 0
//  }
//
class BlacklistChecker {
 public:
  BlacklistChecker(const string& yb_admin_exe, const string& master_address) :
      args_{yb_admin_exe, "-master_addresses", master_address, "get_universe_config"} {
  }

  CHECKED_STATUS operator()(const vector<HostPort>& servers) const {
    string out;
    RETURN_NOT_OK(Subprocess::Call(args_, &out));
    boost::erase_all(out, "\n");
    size_t match_count = 0;
    std::regex re(R"(.*?hosts \{\s*host:\s*\"([^\"]*)\"\s*port:\s*(\d+)[^\}]*\})");
    for (std::sregex_iterator i = std::sregex_iterator(out.cbegin(), out.cend(), re), end;
        i != end; ++i) {
      HostPort server(i->str(1), VERIFY_RESULT(CheckedStoll(i->str(2))));
      if (std::find(servers.begin(), servers.end(), server) == servers.end()) {
        return STATUS_FORMAT(
            NotFound, "Item $0 not found in list of expected hosts $1",
            server, servers);
      } else {
        ++match_count;
      }
    }
    if (match_count != servers.size()) {
      return STATUS_FORMAT(
          NotFound, "$0 items expected but $1 found",
          servers.size(), match_count);
    }
    return Status::OK();
  }

 private:
  vector<string> args_;
};

} // namespace

class AdminCliTest : public tserver::TabletServerIntegrationTestBase {
 protected:
  // Figure out where the admin tool is.
  string GetAdminToolPath() const;
};

string AdminCliTest::GetAdminToolPath() const {
  return GetToolPath(kAdminToolName);
}

// Test yb-admin config change while running a workload.
// 1. Instantiate external mini cluster with 3 TS.
// 2. Create table with 2 replicas.
// 3. Invoke yb-admin CLI to invoke a config change.
// 4. Wait until the new server bootstraps.
// 5. Profit!
TEST_F(AdminCliTest, TestChangeConfig) {
  FLAGS_num_tablet_servers = 3;
  FLAGS_num_replicas = 2;

  vector<string> ts_flags, master_flags;
  ts_flags.push_back("--enable_leader_failure_detection=false");
  master_flags.push_back("--catalog_manager_wait_for_new_tablets_to_elect_leader=false");
  master_flags.push_back("--replication_factor=2");
  BuildAndStart(ts_flags, master_flags);

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  itest::TabletServerMapUnowned active_tablet_servers;
  auto iter = tablet_replicas_.find(tablet_id_);
  TServerDetails* leader = iter->second;
  TServerDetails* follower = (++iter)->second;
  InsertOrDie(&active_tablet_servers, leader->uuid(), leader);
  InsertOrDie(&active_tablet_servers, follower->uuid(), follower);

  TServerDetails* new_node = nullptr;
  for (TServerDetails* ts : tservers) {
    if (!ContainsKey(active_tablet_servers, ts->uuid())) {
      new_node = ts;
      break;
    }
  }
  ASSERT_TRUE(new_node != nullptr);

  int cur_log_index = 0;
  // Elect the leader (still only a consensus config size of 2).
  ASSERT_OK(StartElection(leader, tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(++cur_log_index, leader, tablet_id_,
                                          MonoDelta::FromSeconds(30)));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(30), active_tablet_servers,
                                  tablet_id_, 1));

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_timeout_allowed(true);
  workload.set_write_timeout_millis(10000);
  workload.set_num_write_threads(1);
  workload.set_write_batch_size(1);
  workload.Setup();
  workload.Start();

  // Wait until the Master knows about the leader tserver.
  TServerDetails* master_observed_leader;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &master_observed_leader));
  ASSERT_EQ(leader->uuid(), master_observed_leader->uuid());

  LOG(INFO) << "Adding tserver with uuid " << new_node->uuid() << " as PRE_VOTER ...";
  string exe_path = GetAdminToolPath();
  ASSERT_OK(Subprocess::Call(ToStringVector(
      exe_path, "-master_addresses", cluster_->master()->bound_rpc_addr(), "change_config",
      tablet_id_, "ADD_SERVER", new_node->uuid(), "PRE_VOTER")));

  InsertOrDie(&active_tablet_servers, new_node->uuid(), new_node);
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(),
                                                leader, tablet_id_,
                                                MonoDelta::FromSeconds(10)));

  workload.StopAndJoin();
  int num_batches = workload.batches_completed();

  LOG(INFO) << "Waiting for replicas to agree...";
  // Wait for all servers to replicate everything up through the last write op.
  // Since we don't batch, there should be at least # rows inserted log entries,
  // plus the initial leader's no-op, plus 1 for
  // the added replica for a total == #rows + 2.
  int min_log_index = num_batches + 2;
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(30),
                                  active_tablet_servers, tablet_id_,
                                  min_log_index));

  int rows_inserted = workload.rows_inserted();
  LOG(INFO) << "Number of rows inserted: " << rows_inserted;

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(
      kTableName, ClusterVerifier::AT_LEAST, rows_inserted));

  // Now remove the server once again.
  LOG(INFO) << "Removing tserver with uuid " << new_node->uuid() << " from the config...";
  ASSERT_OK(Subprocess::Call(ToStringVector(
      exe_path, "-master_addresses", cluster_->master()->bound_rpc_addr(), "change_config",
      tablet_id_, "REMOVE_SERVER", new_node->uuid())));

  ASSERT_EQ(1, active_tablet_servers.erase(new_node->uuid()));
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(),
                                                leader, tablet_id_,
                                                MonoDelta::FromSeconds(10)));
}

TEST_F(AdminCliTest, TestDeleteTable) {
  FLAGS_num_tablet_servers = 1;
  FLAGS_num_replicas = 1;

  vector<string> ts_flags, master_flags;
  master_flags.push_back("--replication_factor=1");
  BuildAndStart(ts_flags, master_flags);
  string master_address = ToString(cluster_->master()->bound_rpc_addr());

  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Default table that gets created;
  string table_name = kTableName.table_name();
  string keyspace = kTableName.namespace_name();

  string exe_path = GetAdminToolPath();
  ASSERT_OK(Subprocess::Call(ToStringVector(
      exe_path, "-master_addresses", master_address, "delete_table", keyspace, table_name)));

  const auto tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(master::kNumSystemTables, tables.size());
}

TEST_F(AdminCliTest, TestDeleteIndex) {
  FLAGS_num_tablet_servers = 1;
  FLAGS_num_replicas = 1;

  vector<string> ts_flags, master_flags;
  master_flags.push_back("--replication_factor=1");
  BuildAndStart(ts_flags, master_flags);
  string master_address = ToString(cluster_->master()->bound_rpc_addr());

  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Default table that gets created;
  string table_name = kTableName.table_name();
  string keyspace = kTableName.namespace_name();
  string index_name = table_name + "-index";

  auto tables = ASSERT_RESULT(client->ListTables(/* filter */ table_name));
  ASSERT_EQ(1, tables.size());
  const auto table_id = tables.front().table_id();

  YBSchema index_schema;
  YBSchemaBuilder b;
  b.AddColumn("C$_key")->Type(INT32)->NotNull()->HashPrimaryKey();
  ASSERT_OK(b.Build(&index_schema));

  // Create index.
  shared_ptr<YBTableCreator> table_creator(client->NewTableCreator());

  IndexInfoPB *index_info = table_creator->mutable_index_info();
  index_info->set_indexed_table_id(table_id);
  index_info->set_is_local(false);
  index_info->set_is_unique(false);
  index_info->set_hash_column_count(1);
  index_info->set_range_column_count(0);
  index_info->set_use_mangled_column_name(true);
  index_info->add_indexed_hash_column_ids(10);

  auto *col = index_info->add_columns();
  col->set_column_name("C$_key");
  col->set_indexed_column_id(10);

  Status s = table_creator->
      table_name(YBTableName(YQL_DATABASE_CQL, keyspace, index_name))
      .table_type(YBTableType::YQL_TABLE_TYPE)
      .schema(&index_schema)
      .indexed_table_id(table_id)
      .is_local_index(false)
      .is_unique_index(false)
      .Create();
  ASSERT_OK(s);

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(2 + master::kNumSystemTables, tables.size());

  // Delete index.
  string exe_path = GetAdminToolPath();
  LOG(INFO) << "Delete index via yb-admin: " << keyspace << "." << index_name;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      exe_path, "-master_addresses", master_address, "delete_index", keyspace, index_name)));

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(1 + master::kNumSystemTables, tables.size());

  // Delete table.
  LOG(INFO) << "Delete table via yb-admin: " << keyspace << "." << table_name;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      exe_path, "-master_addresses", master_address, "delete_table", keyspace, table_name)));

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(master::kNumSystemTables, tables.size());
}

TEST_F(AdminCliTest, BlackList) {
  BuildAndStart();
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());
  const auto exe_path = GetAdminToolPath();
  const auto default_port = 9100;
  vector<HostPort> hosts{{"node1", default_port}, {"node2", default_port}, {"node3", default_port}};
  ASSERT_OK(Subprocess::Call(ToStringVector(
      exe_path, "-master_addresses", master_address, "change_blacklist", "ADD", unpack(hosts))));
  const BlacklistChecker checker(exe_path, master_address);
  ASSERT_OK(checker(hosts));
  ASSERT_OK(Subprocess::Call(ToStringVector(
      exe_path, "-master_addresses", master_address, "change_blacklist", "REMOVE", hosts.back())));
  hosts.pop_back();
  ASSERT_OK(checker(hosts));
}

TEST_F(AdminCliTest, InvalidMasterAddresses) {
  int port = AllocateFreePort();
  string unreachable_host = Substitute("127.0.0.1:$0", port);
  std::string error_string;
  ASSERT_NOK(Subprocess::Call(ToStringVector(
      GetAdminToolPath(), "-master_addresses", unreachable_host,
      "-timeout_ms", "1000", "list_tables"), &error_string, true /*read_stderr*/));
  ASSERT_STR_CONTAINS(error_string, "verify the addresses");
}

TEST_F(AdminCliTest, CheckTableIdUsage) {
  BuildAndStart();
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());
  const auto tables = ASSERT_RESULT(client->ListTables(kTableName.table_name(),
                                                       /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());
  const auto exe_path = GetAdminToolPath();
  const auto table_id = tables.front().table_id();
  const auto table_id_arg = Format("tableid.$0", table_id);
  auto args = ToStringVector(
      exe_path, "-master_addresses", master_address, "list_tablets", table_id_arg);
  const auto args_size = args.size();
  ASSERT_OK(Subprocess::Call(args));
  // Check good optional integer argument.
  args.push_back("1");
  ASSERT_OK(Subprocess::Call(args));
  // Check bad optional integer argument.
  args.resize(args_size);
  args.push_back("bad");
  std::string output;
  ASSERT_NOK(Subprocess::Call(args, &output, /* read_stderr */ true));
  // Due to greedy algorithm all bad arguments are treated as table identifier.
  ASSERT_NE(output.find("Namespace 'bad' of type 'ycql' not found"), std::string::npos);
  // Check multiple tables when single one is expected.
  args.resize(args_size);
  args.push_back(table_id_arg);
  ASSERT_NOK(Subprocess::Call(args, &output, /* read_stderr */ true));
  ASSERT_NE(output.find("Single table expected, 2 found"), std::string::npos);
  // Check wrong table id.
  args.resize(args_size - 1);
  const auto bad_table_id = table_id + "_bad";
  args.push_back(Format("tableid.$0", bad_table_id));
  ASSERT_NOK(Subprocess::Call(args, &output, /* read_stderr */ true));
  ASSERT_NE(
      output.find(Format("Table with id '$0' not found", bad_table_id)), std::string::npos);
}

TEST_F(AdminCliTest, TestSnapshotCreation) {
  BuildAndStart();
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());
  const auto extra_table = YBTableName(YQLDatabase::YQL_DATABASE_CQL,
                                       kTableName.namespace_name(),
                                       "extra-table");
  YBSchemaBuilder schemaBuilder;
  schemaBuilder.AddColumn("k")->HashPrimaryKey()->Type(yb::BINARY)->NotNull();
  schemaBuilder.AddColumn("v")->Type(yb::BINARY)->NotNull();
  YBSchema schema;
  ASSERT_OK(schemaBuilder.Build(&schema));
  ASSERT_OK(client->NewTableCreator()->table_name(extra_table)
      .schema(&schema).table_type(yb::client::YBTableType::YQL_TABLE_TYPE).Create());
  const auto tables = ASSERT_RESULT(client->ListTables(kTableName.table_name(),
      /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());
  std::string output;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      GetAdminToolPath(), "-master_addresses", master_address, "create_snapshot",
      Format("tableid.$0", tables.front().table_id()),
      extra_table.namespace_name(), extra_table.table_name()),
                            &output));
  ASSERT_NE(output.find("Started snapshot creation"), string::npos);

  ASSERT_OK(Subprocess::Call(
      ToStringVector(
          GetAdminToolPath(), "-master_addresses", master_address,
          "list_snapshots", "SHOW_DETAILS"),
      &output));
  ASSERT_NE(output.find(extra_table.table_name()), string::npos);
  ASSERT_NE(output.find(kTableName.table_name()), string::npos);
}

TEST_F(AdminCliTest, TestLeaderStepdown) {
  BuildAndStart();
  std::string out;
  auto call_admin = [
      &out,
      admin_path = GetAdminToolPath(),
      aster_address = ToString(cluster_->master()->bound_rpc_addr())] (
      const std::initializer_list<std::string>& args) mutable {
    auto cmds = ToStringVector(admin_path, "-master_addresses", master_address);
    std::copy(args.begin(), args.end(), std::back_inserter(cmds));
    return Subprocess::Call(cmds, &out);
  };
  auto regex_fetch_first = [&out](const std::string& exp) -> Result<std::string> {
    std::smatch match;
    if (!std::regex_search(out.cbegin(), out.cend(), match, std::regex(exp)) || match.size() != 2) {
      return STATUS_FORMAT(NotFound, "No pattern in '$0'", out);
    }
    return match[1];
  };

  ASSERT_OK(call_admin({"list_tablets", kTableName.namespace_name(), kTableName.table_name()}));
  const auto tablet_id = ASSERT_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+)"));
  ASSERT_OK(call_admin({"list_tablet_servers", tablet_id}));
  const auto tserver_id = ASSERT_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+FOLLOWER)"));
  ASSERT_OK(call_admin({"leader_stepdown", tablet_id, tserver_id}));
  ASSERT_OK(call_admin({"list_tablet_servers", tablet_id}));
  ASSERT_EQ(tserver_id, ASSERT_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+LEADER)")));
}

TEST_F(AdminCliTest, TestMasterLeaderStepdown) {
  BuildAndStart();
  std::string out;
  auto call_admin = [
      &out,
      admin_path = GetAdminToolPath(),
      master_address = ToString(cluster_->master()->bound_rpc_addr())] (
      const std::initializer_list<std::string>& args) mutable {
    auto cmds = ToStringVector(admin_path, "-master_addresses", master_address);
    std::copy(args.begin(), args.end(), std::back_inserter(cmds));
    return Subprocess::Call(cmds, &out);
  };
  auto regex_fetch_first = [&out](const std::string& exp) -> Result<std::string> {
    std::smatch match;
    if (!std::regex_search(out.cbegin(), out.cend(), match, std::regex(exp)) || match.size() != 2) {
      return STATUS_FORMAT(NotFound, "No pattern in '$0'", out);
    }
    return match[1];
  };

  ASSERT_OK(call_admin({"list_all_masters"}));
  const auto new_leader_id = ASSERT_RESULT(
      regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+\S+\s+FOLLOWER)"));
  ASSERT_OK(call_admin({"master_leader_stepdown", new_leader_id}));
  ASSERT_OK(call_admin({"list_all_masters"}));
  ASSERT_EQ(new_leader_id, ASSERT_RESULT(
      regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+\S+\s+LEADER)")));
  }

}  // namespace tools
}  // namespace yb
