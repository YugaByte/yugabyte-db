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
// Tests for the yb-admin command-line tool with multiple masters.

#include <regex>

#include <boost/algorithm/string.hpp>

#include <gtest/gtest.h>

#include "yb/integration-tests/external_mini_cluster-itest-base.h"

#include "yb/util/string_util.h"
#include "yb/util/subprocess.h"
#include "yb/util/test_util.h"

namespace yb {
namespace tools {

namespace {

static const char* const kAdminToolName = "yb-admin";

} // namespace

class YBAdminMultiMasterTest : public ExternalMiniClusterITestBase {
};

TEST_F(YBAdminMultiMasterTest, InitialMasterAddresses) {
  auto admin_path = GetToolPath(kAdminToolName);
  ASSERT_NO_FATALS(StartCluster({}, {}, 1/*num tservers*/, 3/*num masters*/));

  // Verify that yb-admin query results with --init_master_addrs match
  // query results with the full master addresses
  int non_leader_idx = -1;
  ASSERT_OK(cluster_->GetFirstNonLeaderMasterIndex(&non_leader_idx));
  auto non_leader = cluster_->master(non_leader_idx);
  HostPort non_leader_hp = non_leader->bound_rpc_hostport();
  std::string output1;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-init_master_addrs", non_leader_hp.ToString(),
      "list_all_masters"), &output1));
  LOG(INFO) << "init_master_addrs: list_all_masters: " << output1;
  std::string output2;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-master_addresses", cluster_->GetMasterAddresses(),
      "list_all_masters"), &output2));
  LOG(INFO) << "full master_addresses: list_all_masters: " << output2;
  ASSERT_EQ(output1, output2);

  output1.clear();
  output2.clear();

  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-init_master_addrs", non_leader_hp.ToString(),
      "get_universe_config"), &output1));
  // Remove the time output from list_all_tablet_servers since it doesn't match
  LOG(INFO) << "init_master_addrs: get_universe_config: " << output1;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-master_addresses", cluster_->GetMasterAddresses(),
      "get_universe_config"), &output2));
  LOG(INFO) << "full master_addresses: get_universe_config: " << output2;
  ASSERT_EQ(output1, output2);
}

TEST_F(YBAdminMultiMasterTest, RemoveDownMaster) {
  const int kNumInitMasters = 3;
  const auto admin_path = GetToolPath(kAdminToolName);
  ASSERT_NO_FATALS(StartCluster({}, {}, 1/*num tservers*/, kNumInitMasters));
  int idx = -1;
  const auto master_addrs = cluster_->GetMasterAddresses();
  ASSERT_OK(cluster_->GetFirstNonLeaderMasterIndex(&idx));
  const auto addr = cluster_->master(idx)->bound_rpc_addr();
  ASSERT_OK(cluster_->master(idx)->Pause());

  std::string output2;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-master_addresses", cluster_->GetMasterAddresses(),
      "list_all_masters"), &output2));
  LOG(INFO) << "list_all_masters \n" << output2;
  const auto lines2 = StringSplit(output2, '\n');
  ASSERT_EQ(lines2.size(), kNumInitMasters + 1);

  std::string output3;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-master_addresses", cluster_->GetMasterAddresses(),
      "change_master_config", "REMOVE_SERVER", addr.host(), addr.port()), &output3));
  LOG(INFO) << "change_master_config: REMOVE_SERVER\n" << output3;

  std::string output4;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-master_addresses", cluster_->GetMasterAddresses(),
      "list_all_masters"), &output4));
  LOG(INFO) << "list_all_masters \n" << output4;
  const auto lines4 = StringSplit(output4, '\n');
  ASSERT_EQ(lines4.size(), kNumInitMasters);
}

TEST_F(YBAdminMultiMasterTest, AddShellMaster) {
  const auto admin_path = GetToolPath(kAdminToolName);
  const int kNumInitMasters = 2;
  ASSERT_NO_FATALS(StartCluster({}, {}, 1/*num tservers*/, kNumInitMasters));
  const auto master_addrs = cluster_->GetMasterAddresses();

  std::string output2;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-master_addresses", cluster_->GetMasterAddresses(),
      "list_all_masters"), &output2));
  LOG(INFO) << "list_all_masters \n" << output2;
  const auto lines2 = StringSplit(output2, '\n');
  ASSERT_EQ(lines2.size(), kNumInitMasters + 1);

  ExternalMaster* shell_master = nullptr;
  cluster_->StartShellMaster(&shell_master);
  ASSERT_NE(shell_master, nullptr);
  scoped_refptr<ExternalMaster> shell_master_ref(shell_master);
  const auto shell_addr = shell_master->bound_rpc_addr();

  std::string output3;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-master_addresses", cluster_->GetMasterAddresses(),
      "change_master_config", "ADD_SERVER", shell_addr.host(), shell_addr.port()), &output3));
  LOG(INFO) << "change_master_config: ADD_SERVER\n" << output3;

  std::string output4;
  ASSERT_OK(Subprocess::Call(ToStringVector(
      admin_path, "-master_addresses", cluster_->GetMasterAddresses(),
      "list_all_masters"), &output4));
  LOG(INFO) << "list_all_masters \n" << output4;
  const auto lines4 = StringSplit(output4, '\n');
  ASSERT_EQ(lines4.size(), kNumInitMasters + 2);
}

TEST_F(YBAdminMultiMasterTest, TestMasterLeaderStepdown) {
  const int kNumInitMasters = 3;
  ASSERT_NO_FATALS(StartCluster({}, {}, 1/*num tservers*/, kNumInitMasters));
  std::string out;
  auto call_admin = [
      &out,
      admin_path = GetToolPath(kAdminToolName),
      master_address = ToString(cluster_->GetMasterAddresses())] (
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

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    RETURN_NOT_OK(call_admin({"list_all_masters"}));
    return new_leader_id ==
        VERIFY_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+\S+\s+LEADER)"));
  }, 5s, "Master leader stepdown"));

  ASSERT_OK(call_admin({"master_leader_stepdown"}));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    RETURN_NOT_OK(call_admin({"list_all_masters"}));
    return new_leader_id !=
      VERIFY_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+\S+\s+LEADER)"));
  }, 5s, "Master leader stepdown"));
}

}  // namespace tools
}  // namespace yb
