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
// The following only applies to changes made to this file as part of YugabyteDB development.
//
// Portions Copyright (c) YugabyteDB, Inc.
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

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <thread>
#include <tuple>
#include <unordered_set>

#include <boost/algorithm/string.hpp>
#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/client/schema.h"
#include "yb/client/table_creator.h"

#include "yb/common/colocated_util.h"
#include "yb/common/ddl_mode-test-util.h"
#include "yb/common/json_util.h"
#include "yb/common/transaction.h"

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/escaping.h"

#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/cql_test_util.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/cluster_itest_util.h"
#include "yb/integration-tests/test_workload.h"

#include "yb/master/master_client.pb.h"
#include "yb/master/master_cluster_client.h"
#include "yb/master/master_defaults.h"

#include "yb/rpc/outbound_call.h"

#include "yb/tools/admin-test-base.h"
#include "yb/tools/yb-admin_util.h"

#include "yb/tools/tools_test_utils.h"
#include "yb/util/backoff_waiter.h"
#include "yb/util/date_time.h"
#include "yb/util/format.h"
#include "yb/util/monotime.h"
#include "yb/util/json_document.h"
#include "yb/util/net/net_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status_format.h"
#include "yb/util/string_util.h"
#include "yb/util/subprocess.h"
#include "yb/util/test_util.h"

#include "yb/yql/pgwrapper/libpq_utils.h"

using namespace std::literals;

using yb::common::GetMemberAsArray;
using yb::common::GetMemberAsStr;
using yb::common::PrettyWriteRapidJsonToString;

namespace yb {
namespace tools {

using client::YBClientBuilder;
using client::YBTableName;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBTableCreator;
using client::YBTableType;
using std::shared_ptr;
using std::vector;
using std::string;
using std::unordered_map;
using itest::TabletServerMap;
using itest::TServerDetails;

namespace {

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
      args_{yb_admin_exe, "--master_addresses", master_address, "get_universe_config"} {
  }

  Status operator()(const vector<HostPort>& servers) const {
    string out;
    RETURN_NOT_OK(Subprocess::Call(args_, &out));
    boost::erase_all(out, "\n");
    JsonDocument doc;
    auto root = VERIFY_RESULT(doc.Parse(out));

    auto blacklistEntries = VERIFY_RESULT(root["serverBlacklist"]["hosts"].GetArray());

    for (const auto &entry : blacklistEntries) {
      auto host = VERIFY_RESULT(entry["host"].GetString());
      auto port = VERIFY_RESULT(entry["port"].GetInt32());
      HostPort blacklistServer(host, port);
      if (std::find(servers.begin(), servers.end(), blacklistServer) ==
          servers.end()) {
        return STATUS_FORMAT(NotFound,
                             "Item $0 not found in list of expected hosts $1",
                             blacklistServer, servers);
      }
    }

    if (blacklistEntries.size() != servers.size()) {
      return STATUS_FORMAT(NotFound, "$0 items expected but $1 found",
                           servers.size(), blacklistEntries.size());
    }

    return Status::OK();
  }

 private:
  vector<string> args_;
};

Result<std::string> ReadFileToString(const std::string& file_path) {
  faststring contents;
  RETURN_NOT_OK(yb::ReadFileToString(Env::Default(), file_path, &contents));
  return contents.ToString();
}

} // namespace

const auto kRollbackAutoFlagsCmd = "rollback_auto_flags";
const auto kPromoteAutoFlagsCmd = "promote_auto_flags";
const auto kClusterConfigEntryTypeName =
    master::SysRowEntryType_Name(master::SysRowEntryType::CLUSTER_CONFIG);
const auto kTableEntryTypeName = master::SysRowEntryType_Name(master::SysRowEntryType::TABLE);

YB_STRONGLY_TYPED_BOOL(EmergencyRepairMode);

class AdminCliTest : public AdminTestBase {
 public:
  Result<std::string> GetClusterUuid() {
    master::GetMasterClusterConfigRequestPB config_req;
    master::GetMasterClusterConfigResponsePB config_resp;
    rpc::RpcController rpc;
    rpc.set_timeout(30s);
    RETURN_NOT_OK(
        cluster_->GetLeaderMasterProxy<master::MasterClusterProxy>().GetMasterClusterConfig(
            config_req, &config_resp, &rpc));
    if (config_resp.has_error()) {
      return StatusFromPB(config_resp.error().status());
    }
    return config_resp.cluster_config().cluster_uuid();
  }

  Status RestartMaster(EmergencyRepairMode mode) {
    const auto kEmergencyRepairModeFlag = "emergency_repair_mode";
    auto* leader_master = cluster_->GetLeaderMaster();
    if (mode) {
      leader_master->AddExtraFlag(kEmergencyRepairModeFlag, "true");
    } else {
      leader_master->RemoveExtraFlag(kEmergencyRepairModeFlag);
    }

    leader_master->Shutdown();
    return leader_master->Restart();
  }

  std::string GetTempDir() { return *tmp_dir_; }

  // Dump the cluster config catalog_entry and verify that the dump contains the correct
  // cluster_uuid.
  Status TestDumpSysCatalogEntry(const std::string& cluster_uuid);

  TmpDirProvider tmp_dir_;
};

namespace {

Status RemoteErrorWithCode(const std::string& message, rpc::ErrorStatusPB::RpcErrorCodePB code) {
  return STATUS(RemoteError, message).CloneAndAddErrorCode(rpc::RpcError(code));
}

}  // namespace

TEST_F(AdminCliTest, UnsupportedRpcErrorDetection) {
  ASSERT_TRUE(IsUnsupportedRpcError(RemoteErrorWithCode(
      "Call on service X received from Y with an invalid method name: Z",
      rpc::ErrorStatusPB::ERROR_NO_SUCH_METHOD)));

  ASSERT_FALSE(IsUnsupportedRpcError(RemoteErrorWithCode(
      "Service X not registered on Y", rpc::ErrorStatusPB::ERROR_NO_SUCH_SERVICE)));
  ASSERT_FALSE(IsUnsupportedRpcError(RemoteErrorWithCode(
      "Leader not ready to serve requests", rpc::ErrorStatusPB::ERROR_APPLICATION)));
  ASSERT_FALSE(IsUnsupportedRpcError(STATUS(RemoteError, "Leader not ready to serve requests")));
  ASSERT_FALSE(IsUnsupportedRpcError(STATUS(TimedOut, "no response")));
}

// Verify the "did you mean" help for a misspelled operation, none of which needs a running
// cluster (the operation is checked before yb-admin connects to the master): prefix matches,
// fuzzy (edit-distance) matches, and that an invalid operation no longer dumps the full command
// list (the original complaint in the issue) while running with no operation still prints help.
TEST_F(AdminCliTest, InvalidOperationSuggestsClosestCommands) {
  const auto exe_path = GetAdminToolPath();
  constexpr auto kUnusedMasterAddress = "127.0.0.1:0";
  constexpr auto kOperationListMarker = "Operations (";
  std::string output;
  std::string error;

  // Prefix match: an unambiguous typo suggests the single command it is a prefix of.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(
          exe_path, "--master_addresses", kUnusedMasterAddress, "list_snapshot_schedule"),
      /* output */ nullptr, &error));
  ASSERT_STR_CONTAINS(error, "Did you mean one of these?");
  ASSERT_STR_CONTAINS(error, "list_snapshot_schedules");

  // Prefix match: a prefix of several commands lists every candidate and a hint, and must not dump
  // the operation list on either stream.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", kUnusedMasterAddress, "list_table"), &output,
      &error));
  ASSERT_STR_CONTAINS(error, "Did you mean one of these?");
  ASSERT_STR_CONTAINS(error, "list_tables");
  ASSERT_STR_CONTAINS(error, "list_tablets");
  ASSERT_STR_CONTAINS(error, "help' to list all operations");
  ASSERT_STR_NOT_CONTAINS(output, kOperationListMarker);
  ASSERT_STR_NOT_CONTAINS(error, kOperationListMarker);

  // Fuzzy match: a transposition ("tabels" instead of "tables") is not a prefix but is close.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", kUnusedMasterAddress, "list_tabels"), nullptr,
      &error));
  ASSERT_STR_CONTAINS(error, "Did you mean one of these?");
  ASSERT_STR_CONTAINS(error, "list_tables");

  // Fuzzy match: a missing leading character ("ist_tables") is one edit away from "list_tables".
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", kUnusedMasterAddress, "ist_tables"), nullptr,
      &error));
  ASSERT_STR_CONTAINS(error, "list_tables");

  // Token match: an abbreviation too far from any command for the fuzzy match.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", kUnusedMasterAddress, "list_server"), nullptr,
      &error));
  ASSERT_STR_CONTAINS(error, "Did you mean one of these?");
  ASSERT_STR_CONTAINS(error, "list_tablet_servers");
  ASSERT_STR_CONTAINS(error, "list_all_tablet_servers");

  // An empty operation is a prefix of every command, but should not list all of them.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", kUnusedMasterAddress, ""), nullptr, &error));
  ASSERT_STR_NOT_CONTAINS(error, "Did you mean one of these?");

  // A far-off garbage string is beyond the edit-distance tolerance, so nothing is suggested.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", kUnusedMasterAddress, "zzzzzzzzzzzzzzzzzz"),
      nullptr, &error));
  ASSERT_STR_NOT_CONTAINS(error, "Did you mean one of these?");

  // Running with no operation prints the short overview, without the gflags dump.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", kUnusedMasterAddress), &output, &error));
  ASSERT_STR_CONTAINS(output, "Usage:");
  ASSERT_STR_CONTAINS(output, "Get help:");
  ASSERT_STR_CONTAINS(output, "Common global flags:");
  ASSERT_STR_CONTAINS(output, "Example:");
  ASSERT_STR_NOT_CONTAINS(output, kOperationListMarker);
  ASSERT_STR_NOT_CONTAINS(output, "Flags from");
  ASSERT_STR_NOT_CONTAINS(output, "yb-admin_cli.cc:");
  ASSERT_STR_NOT_CONTAINS(output, "SetUsageMessage");
  ASSERT_STR_NOT_CONTAINS(error, "SetUsageMessage");
  ASSERT_LE(std::count(output.begin(), output.end(), '\n'), 25);
}

// A malformed --init_master_addrs gets a targeted error, not the overview or a crash (#33435).
TEST_F(AdminCliTest, MalformedInitMasterAddrs) {
  const auto exe_path = GetAdminToolPath();
  std::string output;
  std::string error;

  for (const auto& bad_value : {"host:99999", "host:abc", ",", ",,"}) {
    output.clear();
    error.clear();
    ASSERT_NOK(Subprocess::Call(
        ToStringVector(exe_path, "--init_master_addrs", bad_value, "list_tables"), &output,
        &error))
        << "--init_master_addrs=" << bad_value << " unexpectedly succeeded";
    ASSERT_STR_CONTAINS(error, "Invalid --init_master_addrs");
    ASSERT_STR_NOT_CONTAINS(output, "Get help:");
    ASSERT_STR_NOT_CONTAINS(output, "Operations (");
    ASSERT_STR_NOT_CONTAINS(error, "Operations (");
  }
}

TEST_F(AdminCliTest, HelpFlagsPrintOverview) {
  const auto exe_path = GetAdminToolPath();
  std::string output;
  std::string error;

  for (const auto* help_flag : {"--help", "-help", "--h", "-h", "--help=true"}) {
    output.clear();
    error.clear();
    SCOPED_TRACE(help_flag);
    ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, help_flag), &output, &error));
    ASSERT_STR_CONTAINS(output, "administer a YugabyteDB universe");
    ASSERT_STR_CONTAINS(output, "Get help:");
    ASSERT_STR_CONTAINS(output, "Common global flags:");
    ASSERT_STR_NOT_CONTAINS(output, "Flags from");
    ASSERT_STR_NOT_CONTAINS(output, "SetUsageMessage");
    ASSERT_STR_NOT_CONTAINS(error, "SetUsageMessage");
    ASSERT_LE(std::count(output.begin(), output.end(), '\n'), 25) << help_flag;
  }

  // --help=false is not a help request: this is the bare invocation, which exits 1.
  ASSERT_NOK(Subprocess::Call(ToStringVector(exe_path, "--help=false"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Get help:");

  // Here --help is the value of --master_addresses.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", "--help"), &output, &error));

  // --helpshort adds yb-admin's own flags to the overview.
  ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, "--helpshort"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Get help:");
  ASSERT_STR_CONTAINS(output, "own flags:");
  ASSERT_STR_CONTAINS(output, "--timeout_ms");
  ASSERT_STR_CONTAINS(output, "--certs_dir_name");
  ASSERT_STR_NOT_CONTAINS(output, "No modules matched");
  ASSERT_STR_NOT_CONTAINS(output, "Flags from");
  // Hidden flags such as --TEST_metadata_file_format_version are excluded.
  ASSERT_STR_NOT_CONTAINS(output, "--TEST_");

  // --helpfull is still the gflags dump, headed by the overview.
  ASSERT_NOK(Subprocess::Call(ToStringVector(exe_path, "--helpfull"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Flags from");
  ASSERT_STR_CONTAINS(output, "administer a YugabyteDB universe");
  ASSERT_STR_NOT_CONTAINS(output, "SetUsageMessage");
  ASSERT_STR_NOT_CONTAINS(error, "SetUsageMessage");

  // gflags help flags win over a leading "help" but not over --help, as in gflags itself.
  ASSERT_NOK(Subprocess::Call(ToStringVector(exe_path, "--helpfull", "help"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Flags from");
  ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, "--help", "--helpfull"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Get help:");
  ASSERT_STR_NOT_CONTAINS(output, "Flags from");
  // yb's dump flags and --version win over --help too.
  for (const auto* flag : {"--help_auto_flag_json", "--version"}) {
    SCOPED_TRACE(flag);
    ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, "--help", flag), &output, &error));
    ASSERT_STR_NOT_CONTAINS(output, "Get help:");
  }
  ASSERT_OK(Subprocess::Call(
      ToStringVector(exe_path, "--helpfull=false", "help"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Operations (");
}

TEST_F(AdminCliTest, HelpListsAllOperations) {
  const auto exe_path = GetAdminToolPath();
  std::string output;
  std::string error;
  ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, "help"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Operations (");
  ASSERT_STR_CONTAINS(output, "help <operation>' for the usage of one operation");
  ASSERT_STR_NOT_CONTAINS(output, "Flags from");
  ASSERT_STR_NOT_CONTAINS(output, "SetUsageMessage");
  ASSERT_STR_NOT_CONTAINS(error, "SetUsageMessage");

  std::vector<int> numbers;
  std::vector<std::string> names;
  std::regex entry_re(R"(\n\s*(\d+)\. (\S+))");
  for (std::sregex_iterator it(output.begin(), output.end(), entry_re), end; it != end; ++it) {
    numbers.push_back(std::stoi((*it)[1].str()));
    names.push_back((*it)[2].str());
  }
  ASSERT_GE(numbers.size(), 100);
  for (size_t idx = 0; idx < numbers.size(); ++idx) {
    ASSERT_EQ(numbers[idx], static_cast<int>(idx) + 1)
        << "gap or duplicate in operation numbering at position " << idx;
  }
  ASSERT_TRUE(std::is_sorted(names.begin(), names.end())) << "operation list is not alphabetical";
  ASSERT_TRUE(std::find(names.begin(), names.end(), "help") != names.end());
  ASSERT_TRUE(
      std::find(names.begin(), names.end(), "unsafe_release_object_locks_global") == names.end());

  // Long entries are truncated; help <operation> shows the full syntax.
  for (const auto& line : StringSplit(output, '\n')) {
    ASSERT_LE(line.size(), 100) << line;
  }
  ASSERT_TRUE(std::regex_search(
      output, std::regex(R"(alter_universe_replication [^\n]* \.\.\.\n)")))
      << "expected a truncated entry";
  // A cut must not read as the "]..." repetition syntax: a line ending in " ..." is cut, and a
  // line ending in a bare "..." is the operation's complete usage.
  const std::regex line_re(R"(^\s*\d+\. (\S+) (.*)$)");
  for (const auto& line : StringSplit(output, '\n')) {
    std::smatch m;
    if (!line.ends_with("...") || !std::regex_match(line, m, line_re)) {
      continue;
    }
    std::string usage;
    ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, "help", m[1].str()), &usage, &error));
    const auto full = StringSplit(usage, '\n').front();
    const auto prefix = Format("Usage: yb-admin $0 ", m[1].str());
    ASSERT_TRUE(full.starts_with(prefix)) << full;
    const auto args = full.substr(prefix.size());
    if (line.ends_with(" ...")) {
      ASSERT_GT(args.size(), m[2].str().size() - 4) << line;
      ASSERT_TRUE(args.starts_with(m[2].str().substr(0, m[2].str().size() - 4))) << line;
    } else {
      ASSERT_EQ(args, m[2].str()) << "cut without the \" ...\" marker: " << line;
    }
  }
  ASSERT_OK(Subprocess::Call(
      ToStringVector(exe_path, "help", "alter_universe_replication"), &output, &error));
  const auto usage_line = StringSplit(output, '\n').front();
  ASSERT_STR_CONTAINS(usage_line, "Usage: yb-admin alter_universe_replication");
  ASSERT_GT(usage_line.size(), 100);
}

// `help <arg>`: exact operation, then substring filter, then typo suggestions.
TEST_F(AdminCliTest, HelpOperationDispatch) {
  const auto exe_path = GetAdminToolPath();
  std::string output;
  std::string error;

  ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, "help", "snapshot"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Operations matching 'snapshot' (");
  ASSERT_STR_CONTAINS(output, "\n  * abort_snapshot_restore");
  ASSERT_STR_CONTAINS(output, "create_snapshot");
  ASSERT_STR_CONTAINS(output, "restore_snapshot_schedule");
  ASSERT_FALSE(std::regex_search(output, std::regex(R"(\n\s*\d+\. )")));

  // Hidden operations' usage is available by exact name.
  ASSERT_OK(Subprocess::Call(
      ToStringVector(exe_path, "help", "unsafe_release_object_locks_global"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Usage: yb-admin unsafe_release_object_locks_global");

  // Numbers in the list are not operation names.
  ASSERT_NOK(Subprocess::Call(ToStringVector(exe_path, "help", "42"), &output, &error));
  ASSERT_STR_CONTAINS(error, "Invalid operation: 42");

  // An empty topic does not list every operation.
  output.clear();
  error.clear();
  ASSERT_NOK(Subprocess::Call(ToStringVector(exe_path, "help", ""), &output, &error));
  ASSERT_STR_CONTAINS(error, "Invalid operation: ");
  ASSERT_TRUE(output.empty()) << output;

  // No match falls back to the typo suggestions.
  output.clear();
  error.clear();
  ASSERT_NOK(Subprocess::Call(ToStringVector(exe_path, "help", "delete_tabel"), &output, &error));
  ASSERT_STR_CONTAINS(error, "Invalid operation: delete_tabel");
  ASSERT_STR_CONTAINS(error, "delete_table");
  ASSERT_STR_CONTAINS(error, "help' to list all operations");
  ASSERT_STR_NOT_CONTAINS(error, "Error running help");
  ASSERT_TRUE(output.empty()) << output;

  // Far-off garbage gets no suggestions.
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "help", "zzzzzzzzzzzzzzzzzz"), nullptr, &error));
  ASSERT_STR_NOT_CONTAINS(error, "Did you mean one of these?");

  error.clear();
  ASSERT_NOK(Subprocess::Call(ToStringVector(exe_path, "hepl"), nullptr, &error));
  ASSERT_STR_CONTAINS(error, "Did you mean one of these?");
  ASSERT_TRUE(std::regex_search(error, std::regex(R"(\n  help\n)"))) << error;

  output.clear();
  error.clear();
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "help", "extra1", "extra2"), &output, &error));
  ASSERT_STR_CONTAINS(error, "Usage: yb-admin help [<operation>]");
  ASSERT_STR_NOT_CONTAINS(output, "Get help:");
}

// Help is answered before the flag parse and without a client.
TEST_F(AdminCliTest, HelpNeedsNoCluster) {
  const auto exe_path = GetAdminToolPath();
  std::string output;
  std::string error;

  // Must not wait out --timeout_ms (60s by default) on an unreachable master.
  const auto start = MonoTime::Now();
  ASSERT_OK(Subprocess::Call(
      ToStringVector(exe_path, "help", "--master_addresses", "127.0.0.1:1"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Operations (");
  ASSERT_LT((MonoTime::Now() - start).ToSeconds(), 30);

  ASSERT_OK(Subprocess::Call(
      ToStringVector(exe_path, "help", "--init_master_addrs=host:badport"), &output, &error));
  ASSERT_STR_CONTAINS(output, "Operations (");

  // The operation to show help for may follow flags.
  const auto flagfile_path = tmp_dir_ / "flagfile.conf";
  std::ofstream flagfile(flagfile_path);
  flagfile << "--master_addresses=127.0.0.1:1\n";
  flagfile.close();
  ASSERT_OK(Subprocess::Call(
      ToStringVector(exe_path, "--flagfile", flagfile_path, "delete_table", "--help"), &output,
      &error));
  ASSERT_STR_CONTAINS(output, "Usage: yb-admin delete_table <table>");

  // A leading "help" does not shadow the operation it is asking about.
  for (const auto* help_flag : {"--help", "--helpshort"}) {
    SCOPED_TRACE(help_flag);
    output.clear();
    ASSERT_OK(Subprocess::Call(
        ToStringVector(exe_path, "help", "delete_table", help_flag), &output, &error));
    ASSERT_STR_CONTAINS(output, "Usage: yb-admin delete_table <table>");
  }

  const std::vector<std::string> parse_breakers = {
      "--flagfile=/nonexistent/server.conf",  // gflags: "No such file or directory"
      "--timeout_ms=not_a_number",            // gflags: "illegal value ... for int64 flag"
      "--no_such_flag_exists",                // gflags: "unknown command line flag"
  };
  for (const auto& breaker : parse_breakers) {
    SCOPED_TRACE(breaker);

    output.clear();
    error.clear();
    ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, breaker, "help"), &output, &error));
    ASSERT_STR_CONTAINS(output, "Operations (");

    output.clear();
    error.clear();
    ASSERT_OK(Subprocess::Call(ToStringVector(exe_path, breaker, "--help"), &output, &error));
    ASSERT_STR_CONTAINS(output, "Get help:");

    output.clear();
    error.clear();
    ASSERT_OK(Subprocess::Call(
        ToStringVector(exe_path, breaker, "help", "delete_table"), &output, &error));
    ASSERT_STR_CONTAINS(output, "Usage: yb-admin delete_table <table>");
  }

  // "help" as an argument to another operation.
  output.clear();
  error.clear();
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--master_addresses", "127.0.0.1:1", "--timeout_ms", "1000",
                     "flush_table", "help"),
      &output, &error));
  ASSERT_STR_NOT_CONTAINS(output, "Operations (");

  // "help" as the value of a flag.
  output.clear();
  error.clear();
  ASSERT_NOK(Subprocess::Call(
      ToStringVector(exe_path, "--certs_dir_name", "help", "--master_addresses", "127.0.0.1:1",
                     "--timeout_ms", "1000", "list_tables"),
      &output, &error));
  ASSERT_STR_NOT_CONTAINS(output, "Operations (");
}

TEST_F(AdminCliTest, TokenMatchSuggestions) {
  const std::vector<std::string> names = {
      "compact_table", "is_encryption_enabled", "list_all_masters", "list_all_tablet_servers",
      "list_tables", "list_tablet_server_log_locations", "list_tablet_servers",
      "master_leader_stepdown", "write_universe_key_to_file"};

  const std::vector<std::string> expected = {
      "list_tablet_servers", "list_all_tablet_servers", "list_tablet_server_log_locations"};
  ASSERT_EQ(SuggestByNameTokens("list_server", names, 5), expected);

  ASSERT_EQ(SuggestByNameTokens("servers_list", names, 5), expected);
  ASSERT_EQ(SuggestByNameTokens("LIST_Server", names, 5), expected);
  ASSERT_EQ(
      SuggestByNameTokens("list_server", names, 2),
      (std::vector<std::string>{"list_tablet_servers", "list_all_tablet_servers"}));

  ASSERT_EQ(
      SuggestByNameTokens("leader", names, 5),
      (std::vector<std::string>{"master_leader_stepdown"}));

  ASSERT_EQ(
      SuggestByNameTokens("compact_tables", names, 5),
      (std::vector<std::string>{"compact_table"}));

  ASSERT_TRUE(SuggestByNameTokens("list_server_zzz", names, 5).empty());
  // A word that merely starts with a name token is not a match.
  for (const auto& word : {"isolate", "tomorrow", "allow", "login"}) {
    ASSERT_TRUE(SuggestByNameTokens(word, names, 5).empty()) << word;
  }

  // Short tokens must match a name token exactly.
  for (const auto& word : {"t", "l", "ts", "li"}) {
    ASSERT_TRUE(SuggestByNameTokens(word, names, 5).empty()) << word;
  }
  ASSERT_EQ(
      SuggestByNameTokens("is_enabled", names, 5),
      (std::vector<std::string>{"is_encryption_enabled"}));

  ASSERT_TRUE(SuggestByNameTokens("", names, 5).empty());
  ASSERT_TRUE(SuggestByNameTokens("___", names, 5).empty());
}

// `help <operation>`, `<operation> --help`, and the bad-argument error print the same usage.
TEST_F(AdminCliTest, HelpMatchesBadArgumentUsage) {
  const auto exe_path = GetAdminToolPath();
  std::string help_output;
  ASSERT_OK(Subprocess::Call(
      ToStringVector(exe_path, "help", "delete_table"), &help_output, /* stderr_str */ nullptr));
  ASSERT_STR_CONTAINS(help_output, "Usage: yb-admin delete_table <table>");
  ASSERT_STR_CONTAINS(help_output, "Definitions: <table>");
  ASSERT_STR_CONTAINS(help_output, "[(ycql|ysql).]<namespace_name>");

  std::string flag_output;
  ASSERT_OK(Subprocess::Call(
      ToStringVector(exe_path, "delete_table", "--help"), &flag_output, nullptr));
  ASSERT_EQ(help_output, flag_output);

  BuildAndStart();
  const auto status = CallAdmin("delete_table");
  ASSERT_NOK(status);
  boost::trim_right(help_output);
  ASSERT_STR_CONTAINS(status.ToString(), help_output);
}

// Test yb-admin config change while running a workload.
// 1. Instantiate external mini cluster with 3 TS.
// 2. Create table with 2 replicas.
// 3. Invoke yb-admin CLI to invoke a config change.
// 4. Wait until the new server bootstraps.
// 5. Profit!
TEST_F(AdminCliTest, TestChangeConfig) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 3;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 2;

  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--use_create_table_leader_hint=false"s,
  };
  std::vector<std::string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
  };
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

  TestYcqlWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_timeout_allowed(true);
  workload.set_write_timeout_millis(10000);
  workload.set_num_write_threads(1);
  workload.set_write_batch_size(1);
  workload.set_sequential_write(true);
  workload.Setup();
  workload.Start();

  // Wait until the Master knows about the leader tserver.
  TServerDetails* master_observed_leader;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &master_observed_leader));
  ASSERT_EQ(leader->uuid(), master_observed_leader->uuid());

  LOG(INFO) << "Adding tserver with uuid " << new_node->uuid() << " as PRE_VOTER ...";
  string exe_path = GetAdminToolPath();
  ASSERT_OK(CallAdmin("change_config", tablet_id_, "ADD_SERVER", new_node->uuid(), "PRE_VOTER"));

  InsertOrDie(&active_tablet_servers, new_node->uuid(), new_node);
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(),
                                                leader, tablet_id_,
                                                MonoDelta::FromSeconds(10)));

  workload.StopAndJoin();
  auto num_batches = workload.batches_completed();

  LOG(INFO) << "Waiting for replicas to agree...";
  // Wait for all servers to replicate everything up through the last write op.
  // Since we don't batch, there should be at least # rows inserted log entries,
  // plus the initial leader's no-op, plus 1 for
  // the added replica for a total == #rows + 2.
  auto min_log_index = num_batches + 2;
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(30),
                                  active_tablet_servers, tablet_id_,
                                  min_log_index));

  auto rows_inserted = workload.rows_inserted();
  LOG(INFO) << "Number of rows inserted: " << rows_inserted;

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(
      kTableName, ClusterVerifier::AT_LEAST, rows_inserted));

  // Now remove the server once again.
  LOG(INFO) << "Removing tserver with uuid " << new_node->uuid() << " from the config...";
  ASSERT_OK(CallAdmin("change_config", tablet_id_, "REMOVE_SERVER", new_node->uuid()));

  ASSERT_EQ(1, active_tablet_servers.erase(new_node->uuid()));
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(),
                                                leader, tablet_id_,
                                                MonoDelta::FromSeconds(10)));
}

TEST_F(AdminCliTest, TestDeleteTable) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 1;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 1;

  vector<string> ts_flags, master_flags;
  BuildAndStart(ts_flags, master_flags);
  string master_address = ToString(cluster_->master()->bound_rpc_addr());

  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Default table that gets created;
  string table_name = kTableName.table_name();
  string keyspace = kTableName.namespace_name();

  string exe_path = GetAdminToolPath();
  ASSERT_OK(CallAdmin("delete_table", keyspace, table_name));

  const auto tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(master::kNumSystemTables, tables.size());
}

TEST_F(AdminCliTest, TestDeleteIndex) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 1;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 1;

  vector<string> ts_flags, master_flags;
  ts_flags.push_back("--index_backfill_upperbound_for_user_enforced_txn_duration_ms=12000");
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
  b.AddColumn("C$_key")->Type(DataType::INT32)->NotNull()->HashPrimaryKey();
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

  Status s = table_creator->table_name(YBTableName(YQL_DATABASE_CQL, keyspace, index_name))
                 .table_type(YBTableType::YQL_TABLE_TYPE)
                 .schema(&index_schema)
                 .indexed_table_id(table_id)
                 .is_local_index(false)
                 .is_unique_index(false)
                 .timeout(MonoDelta::FromSeconds(60))
                 .Create();
  ASSERT_OK(s);

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(2 + master::kNumSystemTables, tables.size());

  // Delete index.
  string exe_path = GetAdminToolPath();
  LOG(INFO) << "Delete index via yb-admin: " << keyspace << "." << index_name;
  ASSERT_OK(CallAdmin("delete_index", keyspace, index_name));

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(1 + master::kNumSystemTables, tables.size());

  // Delete table.
  LOG(INFO) << "Delete table via yb-admin: " << keyspace << "." << table_name;
  ASSERT_OK(CallAdmin("delete_table", keyspace, table_name));

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(master::kNumSystemTables, tables.size());
}

TEST_F(AdminCliTest, BlackList) {
  BuildAndStart();
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());
  const auto exe_path = GetAdminToolPath();
  const auto default_port = 9100;
  vector<HostPort> hosts{{"node1", default_port}, {"node2", default_port}, {"node3", default_port}};
  ASSERT_OK(CallAdmin("change_blacklist", "ADD", unpack(hosts)));
  const BlacklistChecker checker(exe_path, master_address);
  ASSERT_OK(checker(hosts));
  ASSERT_OK(CallAdmin("change_blacklist", "REMOVE", hosts.back()));
  hosts.pop_back();
  ASSERT_OK(checker(hosts));
}

TEST_F(AdminCliTest, InvalidMasterAddresses) {
  int port = AllocateFreePort();
  string unreachable_host = Format("127.0.0.1:$0", port);
  std::string error_string;
  ASSERT_NOK(Subprocess::Call(ToStringVector(
      GetAdminToolPath(), "--master_addresses", unreachable_host,
      "--timeout_ms", "1000", "list_tables"), /* output */ nullptr, &error_string));
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
      exe_path, "--master_addresses", master_address, "list_tablets", table_id_arg);
  const auto args_size = args.size();
  ASSERT_OK(Subprocess::Call(args));
  // Check good optional integer argument.
  args.push_back("1");
  ASSERT_OK(Subprocess::Call(args));
  // Check bad optional integer argument.
  args.resize(args_size);
  args.push_back("bad");
  std::string error;
  ASSERT_NOK(Subprocess::Call(args, /* output */ nullptr, &error));
  // Due to greedy algorithm all bad arguments are treated as table identifier.
  ASSERT_NE(error.find("Namespace 'bad' of type 'ycql' not found"), std::string::npos);
  // Check multiple tables when single one is expected.
  args.resize(args_size);
  args.push_back(table_id_arg);
  ASSERT_NOK(Subprocess::Call(args, /* output */ nullptr, &error));
  ASSERT_NE(error.find("Single table expected, 2 found"), std::string::npos);
  // Check wrong table id.
  args.resize(args_size - 1);
  const auto bad_table_id = table_id + "_bad";
  args.push_back(Format("tableid.$0", bad_table_id));
  ASSERT_NOK(Subprocess::Call(args, /*output*/ nullptr, &error));
  ASSERT_NE(error.find(Format("Table with id '$0' not found", bad_table_id)), std::string::npos);
}

TEST_F(AdminCliTest, TestSnapshotCreation) {
  BuildAndStart();
  const auto extra_table = YBTableName(YQLDatabase::YQL_DATABASE_CQL,
                                       kTableName.namespace_name(),
                                       "extra-table");
  YBSchemaBuilder schema_builder;
  schema_builder.AddColumn("k")->HashPrimaryKey()->Type(DataType::BINARY)->NotNull();
  schema_builder.AddColumn("v")->Type(DataType::BINARY)->NotNull();
  YBSchema schema;
  ASSERT_OK(schema_builder.Build(&schema));
  ASSERT_OK(client_->NewTableCreator()->table_name(extra_table)
      .schema(&schema).table_type(yb::client::YBTableType::YQL_TABLE_TYPE).Create());
  const auto tables = ASSERT_RESULT(client_->ListTables(kTableName.table_name(),
      /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());
  std::string output = ASSERT_RESULT(CallAdmin(
      "create_snapshot", Format("tableid.$0", tables.front().table_id()),
      extra_table.namespace_name(), extra_table.table_name()));
  ASSERT_NE(output.find("Started snapshot creation"), string::npos);

  output = ASSERT_RESULT(CallAdmin("list_snapshots", "SHOW_DETAILS"));
  ASSERT_NE(output.find(extra_table.table_name()), string::npos);
  ASSERT_NE(output.find(kTableName.table_name()), string::npos);

  // Snapshot creation should be blocked for CQL system tables (which are virtual) but not for
  // redis system tables (which are not).
  const auto result = CallAdmin("create_snapshot", "system", "peers");
  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(result.status().IsRuntimeError());
  ASSERT_NE(result.status().ToUserMessage().find(
      "Error running create_snapshot: Invalid argument"), std::string::npos);
  ASSERT_NE(result.status().ToUserMessage().find(
      "Cannot create snapshot of YCQL system table: peers"), std::string::npos);

  ASSERT_OK(CallAdmin("setup_redis_table"));
  ASSERT_OK(CallAdmin("create_snapshot", "system_redis", "redis"));
}

TEST_F(AdminCliTest, GetIsLoadBalancerIdle) {
  const MonoDelta kWaitTime = 20s;
  std::string output;
  std::vector<std::string> master_flags;
  std::vector<std::string> ts_flags;
  master_flags.push_back("--enable_load_balancing=true");
  BuildAndStart(ts_flags, master_flags);

  const std::string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Cluster balancer IsIdle() logic has been changed to the following - unless a task was
  // explicitly triggered by the cluster balancer (AsyncAddServerTask / AsyncRemoveServerTask /
  // AsyncTryStepDown) then the task does not count towards determining whether the cluster balancer
  // is active. If no pending cluster balancer tasks of the aforementioned types exist, the cluster
  // balancer will report idle.

  // Delete table should not activate the cluster balancer.
  ASSERT_OK(client->DeleteTable(kTableName, false /* wait */));
  // This should timeout.
  Status s = WaitFor(
      [&]() -> Result<bool> {
        auto output = VERIFY_RESULT(CallAdmin("get_is_load_balancer_idle"));
        return output.compare("Idle = 0\n") == 0;
      },
      kWaitTime,
      "wait for cluster balancer to stay idle");

  ASSERT_FALSE(s.ok());
}

TEST_F(AdminCliTest, TestLeaderStepdown) {
  BuildAndStart();
  std::string out;
  auto regex_fetch_first = [&out](const std::string& exp) -> Result<std::string> {
    std::smatch match;
    if (!std::regex_search(out.cbegin(), out.cend(), match, std::regex(exp)) || match.size() != 2) {
      return STATUS_FORMAT(NotFound, "No pattern in '$0'", out);
    }
    return match[1];
  };

  out = ASSERT_RESULT(CallAdmin(
      "list_tablets", kTableName.namespace_name(), kTableName.table_name()));
  const auto tablet_id = ASSERT_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+)"));
  out = ASSERT_RESULT(CallAdmin("list_tablet_servers", tablet_id));
  const auto tserver_id = ASSERT_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+FOLLOWER)"));

  // On a freshly-started cluster the chosen follower may not yet be caught up to the leader,
  // in which case leader_stepdown returns "Suggested peer is not caught up yet". Retry until
  // the follower catches up.
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    auto result = CallAdmin("leader_stepdown", tablet_id, tserver_id);
    if (result.ok()) {
      return true;
    }
    if (result.status().ToString().find("not caught up") != std::string::npos) {
      return false;
    }
    return result.status();
  }, 10s, "Leader stepdown call succeeds"));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    out = VERIFY_RESULT(CallAdmin("list_tablet_servers", tablet_id));
    return tserver_id == VERIFY_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+LEADER)"));
  }, 5s, "Leader stepdown"));
}

namespace {
Result<string> RegexFetchFirst(const string out, const string &exp) {
  std::smatch match;
  if (!std::regex_search(out.cbegin(), out.cend(), match, std::regex(exp)) || match.size() != 2) {
    return STATUS_FORMAT(NotFound, "No pattern in '$0'", out);
  }
  return match[1];
}
} // namespace

class AdminCliTestForTableLocks : public AdminCliTest {
 public:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->enable_ysql = true;
    ToggleDDLMode(options->extra_tserver_flags, /* use_legacy = */ false);
    ToggleDDLMode(options->extra_master_flags, /* use_legacy = */ false);
  }

 protected:
  Result<std::pair<std::string, std::string>> ExtractTxnAndSubtxnIdFrom(
      const HostPort& addr, const std::string& page) {
    EasyCurl curl;
    const std::regex txn_regex(
        "\\{txn: ([a-z0-9]{8}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{12}) subtxn_id: "
        "([0-9]+)\\}");
    faststring buf;
    auto url = Format("http://$0/$1", ToString(addr), page);
    RETURN_NOT_OK(curl.FetchURL(url, &buf));
    auto lt_out = buf.ToString();
    VLOG(1) << "Response from url: " << url << " :\n" << lt_out;
    std::smatch match;
    if (!std::regex_search(lt_out.cbegin(), lt_out.cend(), match, txn_regex)) {
      return STATUS(NotFound, "Pattern not found");
    }
    for (std::size_t i = 0; i < match.size(); ++i) {
      VLOG(1) << i << ": " << match[i] << '\n';
    }
    auto txn_id = match[1];
    auto subtxn_id = match[2];
    VLOG(1) << "Transaction ID: " << txn_id;
    VLOG(1) << "Subtransaction ID: " << subtxn_id;
    return std::make_pair(txn_id, subtxn_id);
  }

  Result<std::pair<std::string, std::string>> ExtractTxnAndSubtxnIdFromMaster() {
    return ExtractTxnAndSubtxnIdFrom(
        cluster_->GetLeaderMaster()->bound_http_hostport(), "ObjectLockManager");
  }

  Result<std::pair<std::string, std::string>> ExtractTxnAndSubtxnIdFromTServer(int index) {
    return ExtractTxnAndSubtxnIdFrom(
        cluster_->tablet_server(index)->bound_http_hostport(), "TSLocalLockManager");
  }

  Result<bool> HasLocksTServer(int index) {
    return HasLocks(cluster_->tablet_server(index)->bound_http_hostport(), "TSLocalLockManager");
  }

  Result<bool> HasLocksMaster() {
    return HasLocks(cluster_->GetLeaderMaster()->bound_http_hostport(), "ObjectLockManager");
  }

  Result<bool> HasLocks(const HostPort& addr, const std::string& page) {
    auto res = ExtractTxnAndSubtxnIdFrom(addr, page);
    if (res.ok()) {
      return true;
    } else if (res.status().IsNotFound()) {
      return false;
    } else {
      return res.status();
    }
  }
};

TEST_F(AdminCliTestForTableLocks, ReleaseExclusiveLocksUsingTxnIdAndSubtxnId) {
  BuildAndStart();
  const std::string db_name{"yugabyte"};
  const int kTServerIndex = 0;
  auto conn1 = ASSERT_RESULT(cluster_->ConnectToDB(db_name, kTServerIndex));

  const std::string table_name = "test_table";
  ASSERT_OK(conn1.ExecuteFormat("CREATE TABLE $0 (id INT PRIMARY KEY, value TEXT)", table_name));

  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return !VERIFY_RESULT(HasLocksMaster());
      },
      10s * kTimeMultiplier, "Wait for master to be release locks asynchronously"));

  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksMaster()));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));
  ASSERT_OK(conn1.ExecuteFormat("LOCK TABLE $0 IN ACCESS EXCLUSIVE MODE", table_name));
  ASSERT_TRUE(ASSERT_RESULT(HasLocksMaster()));
  ASSERT_TRUE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));
  std::string txn_id;
  std::string subtxn_id;
  std::tie(txn_id, subtxn_id) = ASSERT_RESULT(ExtractTxnAndSubtxnIdFromMaster());

  ASSERT_OK(CallAdmin("unsafe_release_object_locks_global", txn_id, subtxn_id));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksMaster()));

  // Sanity check that other tranactions can acquire locks.
  auto conn2 = ASSERT_RESULT(cluster_->ConnectToDB(db_name, kTServerIndex));
  ASSERT_OK(conn2.Execute("BEGIN"));
  ASSERT_OK(conn2.ExecuteFormat("LOCK TABLE $0 IN ACCESS EXCLUSIVE MODE", table_name));
  ASSERT_OK(conn2.Execute("COMMIT"));
}

TEST_F(AdminCliTestForTableLocks, ReleaseExclusiveLocksUsingTxnId) {
  BuildAndStart();
  const std::string db_name{"yugabyte"};
  const int kTServerIndex = 0;
  auto conn1 = ASSERT_RESULT(cluster_->ConnectToDB(db_name, kTServerIndex));

  const std::string table_name = "test_table";
  ASSERT_OK(conn1.ExecuteFormat("CREATE TABLE $0 (id INT PRIMARY KEY, value TEXT)", table_name));

  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return !VERIFY_RESULT(HasLocksMaster());
      },
      10s * kTimeMultiplier, "Wait for master to be release locks asynchronously"));

  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksMaster()));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));
  ASSERT_OK(conn1.ExecuteFormat("LOCK TABLE $0 IN ACCESS EXCLUSIVE MODE", table_name));
  ASSERT_TRUE(ASSERT_RESULT(HasLocksMaster()));
  ASSERT_TRUE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));
  std::string txn_id;
  std::tie(txn_id, std::ignore) = ASSERT_RESULT(ExtractTxnAndSubtxnIdFromMaster());

  ASSERT_OK(CallAdmin("unsafe_release_object_locks_global", txn_id));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksMaster()));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));

  // Sanity check that other tranactions can acquire locks.
  auto conn2 = ASSERT_RESULT(cluster_->ConnectToDB(db_name, kTServerIndex));
  ASSERT_OK(conn2.Execute("BEGIN"));
  ASSERT_OK(conn2.ExecuteFormat("LOCK TABLE $0 IN ACCESS EXCLUSIVE MODE", table_name));
  ASSERT_OK(conn2.Execute("COMMIT"));
}

TEST_F(AdminCliTestForTableLocks, ReleaseSharedLocksAtTServer) {
  BuildAndStart();
  const std::string db_name{"yugabyte"};
  const int kTServerIndex = 0;
  auto conn1 = ASSERT_RESULT(cluster_->ConnectToDB(db_name, kTServerIndex));

  const std::string table_name = "test_table";
  ASSERT_OK(conn1.ExecuteFormat("CREATE TABLE $0 (id INT PRIMARY KEY, value TEXT)", table_name));

  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.ExecuteFormat("LOCK TABLE $0 IN ACCESS SHARE MODE", table_name));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksMaster()));
  ASSERT_TRUE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));
  std::string txn_id;
  std::tie(txn_id, std::ignore) = ASSERT_RESULT(ExtractTxnAndSubtxnIdFromTServer(kTServerIndex));

  // Having this test flag allows us to release locks for unknown transactions at the master.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_allow_unknown_txn_release_request", "false"));
  ASSERT_NOK(CallAdmin("unsafe_release_object_locks_global", txn_id));
  ASSERT_TRUE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));

  ASSERT_OK(CallTSCli("unsafe_release_all_locks_for_txn", txn_id));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));

  // Sanity check that other tranactions can acquire locks.
  auto conn2 = ASSERT_RESULT(cluster_->ConnectToDB(db_name, kTServerIndex));
  ASSERT_OK(conn2.Execute("BEGIN"));
  ASSERT_OK(conn2.ExecuteFormat("LOCK TABLE $0 IN ACCESS EXCLUSIVE MODE", table_name));
  ASSERT_OK(conn2.Execute("COMMIT"));
}

TEST_F(AdminCliTestForTableLocks, ReleaseSharedLocksThroughMaster) {
  BuildAndStart();
  const std::string db_name{"yugabyte"};
  const int kTServerIndex = 0;
  auto conn1 = ASSERT_RESULT(cluster_->ConnectToDB(db_name, kTServerIndex));

  const std::string table_name = "test_table";
  ASSERT_OK(conn1.ExecuteFormat("CREATE TABLE $0 (id INT PRIMARY KEY, value TEXT)", table_name));

  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return !VERIFY_RESULT(HasLocksMaster());
      },
      10s * kTimeMultiplier, "Wait for master to be release locks asynchronously"));

  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.ExecuteFormat("LOCK TABLE $0 IN ACCESS SHARE MODE", table_name));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksMaster()));
  ASSERT_TRUE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));
  std::string txn_id = ASSERT_RESULT(ExtractTxnAndSubtxnIdFromTServer(kTServerIndex)).first;

  // Having this test flag allows us to release locks for unknown transactions at the master.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_allow_unknown_txn_release_request", "true"));

  ASSERT_OK(CallAdmin("unsafe_release_object_locks_global", txn_id));
  ASSERT_FALSE(ASSERT_RESULT(HasLocksTServer(kTServerIndex)));

  // Sanity check that other tranactions can acquire locks.
  auto conn2 = ASSERT_RESULT(cluster_->ConnectToDB(db_name, kTServerIndex));
  ASSERT_OK(conn2.Execute("BEGIN"));
  ASSERT_OK(conn2.ExecuteFormat("LOCK TABLE $0 IN ACCESS EXCLUSIVE MODE", table_name));
  ASSERT_OK(conn2.Execute("COMMIT"));
}

// Use list_tablets to list one single tablet for the table.
// Parse out the tablet from the output.
// Use list_tablet_servers to list the tablets leader and follower details.
// Get the leader uuid and host:port from this.
// Also create an unordered_map of all followers host_port to uuid.
// Verify that these are present in the appropriate locations in the output of list_tablets.
// Use list_tablets with the json option to get the json output.
// Parse the json output, extract data for the specific tablet and compare the leader and
// follower details with the values extraced previously.
TEST_F(AdminCliTest, TestFollowersTableList) {
  BuildAndStart();

  string lt_out = ASSERT_RESULT(CallAdmin(
      "list_tablets", kTableName.namespace_name(), kTableName.table_name(),
      "1", "include_followers"));
  const auto tablet_id = ASSERT_RESULT(RegexFetchFirst(lt_out, R"(\s+([a-z0-9]{32})\s+)"));
  ASSERT_FALSE(tablet_id.empty());

  master::TabletLocationsPB locs_pb;
  ASSERT_OK(itest::GetTabletLocations(
        cluster_.get(), tablet_id, MonoDelta::FromSeconds(10), &locs_pb));
  ASSERT_EQ(3, locs_pb.replicas().size());
  string leader_uuid;
  string leader_host_port;
  unordered_map<string, string> follower_hp_to_uuid_map;
  for (const auto& replica : locs_pb.replicas()) {
    if (replica.role() == PeerRole::LEADER) {
      leader_host_port = HostPortPBToString(replica.ts_info().private_rpc_addresses(0));
      leader_uuid = replica.ts_info().permanent_uuid();
    } else {
      follower_hp_to_uuid_map[HostPortPBToString(replica.ts_info().private_rpc_addresses(0))] =
          replica.ts_info().permanent_uuid();
    }
  }
  ASSERT_FALSE(leader_uuid.empty());
  ASSERT_FALSE(leader_host_port.empty());
  ASSERT_EQ(2, follower_hp_to_uuid_map.size());

  const auto follower_list_str = ASSERT_RESULT(RegexFetchFirst(
        lt_out, R"(\s+)" + leader_host_port + R"(\s+)" + leader_uuid + R"(\s+(\S+))"));

  vector<string> followers;
  boost::split(followers, follower_list_str, boost::is_any_of(","));
  ASSERT_EQ(2, followers.size());
  for (const string &follower_host_port : followers) {
      auto got = follower_hp_to_uuid_map.find(follower_host_port);
      ASSERT_TRUE(got != follower_hp_to_uuid_map.end());
  }

  string lt_json_out = ASSERT_RESULT(CallAdmin(
      "list_tablets", kTableName.namespace_name(), kTableName.table_name(),
      "json", "include_followers"));
  boost::erase_all(lt_json_out, "\n");
  JsonDocument doc;
  auto root = ASSERT_RESULT(doc.Parse(lt_json_out));

  for (const auto& entry : ASSERT_RESULT(root["tablets"].GetArray())) {
    auto tid = ASSERT_RESULT(entry["id"].GetString());
    // Testing only for the tablet received in list_tablets <table id> 1 include_followers.
    if (tid != tablet_id) {
      continue;
    }
    auto leader = entry["leader"];
    auto lhp = ASSERT_RESULT(leader["endpoint"].GetString());
    auto luuid = ASSERT_RESULT(leader["uuid"].GetString());
    auto role = ASSERT_RESULT(leader["role"].GetString());
    ASSERT_STR_EQ(lhp, leader_host_port);
    ASSERT_STR_EQ(luuid, leader_uuid);
    ASSERT_STR_EQ(role, PeerRole_Name(PeerRole::LEADER));

    for (const auto& f : ASSERT_RESULT(entry["followers"].GetArray())) {
      auto fhp = ASSERT_RESULT(f["endpoint"].GetString());
      auto fuuid = ASSERT_RESULT(f["uuid"].GetString());
      auto frole = ASSERT_RESULT(f["role"].GetString());
      auto got = follower_hp_to_uuid_map.find(fhp);
      ASSERT_TRUE(got != follower_hp_to_uuid_map.end());
      ASSERT_STR_EQ(got->second, fuuid);
      ASSERT_STR_EQ(frole, PeerRole_Name(PeerRole::FOLLOWER));
    }
  }
}

class AdminCliTestWithYSQL : public AdminCliTest {
 public:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->enable_ysql = true;
  }
};

// Returns the colocation parent table id for a database created WITH colocation = true. Such a
// database uses GA (tablegroup) colocation, whose parent id is keyed by the implicit tablegroup --
// not the legacy "<namespace>.colocated.parent.uuid" that GetColocatedDbParentTableId() builds and
// which does not exist here. Look it up by name, the way a user finds it via list_tables.
Result<TableId> GetColocationParentTableId(client::YBClient* client, const std::string& database) {
  auto parents = VERIFY_RESULT(client->ListTables(
      kColocationParentTableNameSuffix, /* exclude_ysql = */ false, database));
  SCHECK_EQ(
      parents.size(), 1U, IllegalState,
      Format("Expected exactly one colocation parent table in database $0", database));
  return parents.front().table_id();
}

// Test get_table_hash scoping on a colocated tablet: a child colocated table id hashes just that
// table, the colocation parent table id hashes the whole tablet, and the per-table hashes
// recombine to the whole-tablet hash.
TEST_F(AdminCliTestWithYSQL, TestGetTableXorHashColocated) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE DATABASE codb WITH colocation = true"));
  auto codb = ASSERT_RESULT(cluster_->ConnectToDB("codb"));
  ASSERT_OK(codb.Execute("CREATE TABLE colo_a (k int PRIMARY KEY, v int)"));
  ASSERT_OK(codb.Execute("CREATE TABLE colo_b (k int PRIMARY KEY, v int)"));
  // Distinct data and row counts so the two tables' hashes differ.
  ASSERT_OK(codb.Execute("INSERT INTO colo_a SELECT g, g * 10 FROM generate_series(1, 50) g"));
  ASSERT_OK(codb.Execute("INSERT INTO colo_b SELECT g, g * 7  FROM generate_series(1, 30) g"));

  auto get_table_id = [this](const std::string& name) -> Result<TableId> {
    auto tables = VERIFY_RESULT(client_->ListTables(name));
    SCHECK_EQ(tables.size(), 1U, IllegalState, Format("Expected one table named $0", name));
    return tables.front().table_id();
  };
  const auto table_a = ASSERT_RESULT(get_table_id("colo_a"));
  const auto table_b = ASSERT_RESULT(get_table_id("colo_b"));

  // The colocation parent table id addresses the whole tablet (looked up by name -- see
  // GetColocationParentTableId, since WITH colocation uses GA tablegroup colocation).
  const auto parent_table = ASSERT_RESULT(GetColocationParentTableId(client_.get(), "codb"));

  // Fixed read time so all three calls observe identical data.
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto parse_totals = [](const std::string& output) -> std::pair<uint64_t, uint64_t> {
    const auto row_prefix = "Total row count: ";
    const auto hash_prefix = "Total XOR hash: ";
    auto rp = output.find(row_prefix);
    auto hp = output.find(hash_prefix);
    CHECK(rp != std::string::npos && hp != std::string::npos) << output;
    return {std::stoull(output.substr(rp + strlen(row_prefix))),
            std::stoull(output.substr(hp + strlen(hash_prefix)))};
  };

  // A child colocated table id scopes the hash to that single table.
  auto [rows_a, hash_a] =
      parse_totals(ASSERT_RESULT(CallAdmin("get_table_hash", table_a, ht.ToUint64())));
  auto [rows_b, hash_b] =
      parse_totals(ASSERT_RESULT(CallAdmin("get_table_hash", table_b, ht.ToUint64())));

  ASSERT_EQ(rows_a, 50);
  ASSERT_EQ(rows_b, 30);
  ASSERT_NE(hash_a, 0);
  ASSERT_NE(hash_b, 0);
  ASSERT_NE(hash_a, hash_b);

  // The colocation parent table id hashes the whole colocated tablet, so its totals must equal the
  // combination of the two per-table results.
  auto [rows_all, hash_all] =
      parse_totals(ASSERT_RESULT(CallAdmin("get_table_hash", parent_table, ht.ToUint64())));
  ASSERT_EQ(rows_all, rows_a + rows_b);
  ASSERT_EQ(hash_all, hash_a ^ hash_b);
}

// Test get_table_hash with a partition-key sub-range on a colocated (range-partitioned) table.
//
// Colocated tables are range-only and share a single tablet, so -- unlike a hash-partitioned table
// -- there is no trivial midpoint key (e.g. 0x8000) and no tablet boundary to crib bytes from. To
// obtain real, mid-data range split keys without baking in any client-side key-encoding
// assumptions, we create a throwaway NON-colocated table with the identical primary key, split it
// AT VALUES ((4), (8)), and read back the partition_key_starts the server assigned to its tablets.
// Those byte strings are the server's own encoding of the range keys for id=4 and id=8. Because the
// range-column encoding is independent of colocation (colocation only prepends a per-table prefix,
// which the server adds separately from the table id), the same bytes are valid split points inside
// the colocated table's slice of the shared tablet.
TEST_F(AdminCliTestWithYSQL, TestGetTableXorHashColocatedKeyRange) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE DATABASE codb WITH colocation = true"));
  auto codb = ASSERT_RESULT(cluster_->ConnectToDB("codb"));

  // Colocated table (range-partitioned on id ASC) holding ids 1..10 in the single shared tablet.
  ASSERT_OK(codb.Execute("CREATE TABLE colo_range (id INT, name TEXT, PRIMARY KEY (id ASC))"));
  ASSERT_OK(codb.Execute(
      "INSERT INTO colo_range (id, name) SELECT i, 'test' || i FROM generate_series(1, 10) i"));

  // Throwaway non-colocated table with the SAME primary key, split at ids 4 and 8, used only to
  // read the server-encoded range keys for those ids from the resulting tablet boundaries.
  ASSERT_OK(codb.Execute(
      "CREATE TABLE split_probe (id INT, name TEXT, PRIMARY KEY (id ASC)) "
      "WITH (colocation = false) SPLIT AT VALUES ((4), (8))"));

  auto get_table_id = [this](const std::string& name) -> Result<TableId> {
    auto tables = VERIFY_RESULT(client_->ListTables(name, /* exclude_ysql = */ false, "codb"));
    SCHECK_EQ(tables.size(), 1U, IllegalState, Format("Expected one table named $0", name));
    return tables.front().table_id();
  };
  const auto colocated_tbl_id = ASSERT_RESULT(get_table_id("colo_range"));
  const auto probe_tbl_id = ASSERT_RESULT(get_table_id("split_probe"));

  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> probe_tablets;
  ASSERT_OK(client_->GetTabletsFromTableId(probe_tbl_id, /*max_tablets=*/0, &probe_tablets));
  // SPLIT AT VALUES ((4), (8)) yields three tablets: [-inf, key(4)), [key(4), key(8)),
  // [key(8), +inf). The two split keys are the non-empty partition_key_starts (a partition key
  // carries no table-id prefix); sort them ascending since tablet order is not guaranteed.
  ASSERT_EQ(probe_tablets.size(), 3);
  std::vector<std::string> split_keys;
  for (const auto& tablet : probe_tablets) {
    if (!tablet.partition().partition_key_start().empty()) {
      split_keys.push_back(tablet.partition().partition_key_start());
    }
  }
  ASSERT_EQ(split_keys.size(), 2) << "expected two tablet boundaries from SPLIT AT VALUES";
  std::sort(split_keys.begin(), split_keys.end());
  const auto key4_hex = strings::b2a_hex(split_keys[0]);  // boundary for id=4
  const auto key8_hex = strings::b2a_hex(split_keys[1]);  // boundary for id=8

  // Fixed read time so every call below observes identical data.
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto parse_totals = [](const std::string& output) -> std::pair<uint64_t, uint64_t> {
    const auto row_prefix = "Total row count: ";
    const auto hash_prefix = "Total XOR hash: ";
    auto rp = output.find(row_prefix);
    auto hp = output.find(hash_prefix);
    CHECK(rp != std::string::npos && hp != std::string::npos) << output;
    return {std::stoull(output.substr(rp + strlen(row_prefix))),
            std::stoull(output.substr(hp + strlen(hash_prefix)))};
  };

  auto [full_rows, full_hash] =
      parse_totals(ASSERT_RESULT(CallAdmin("get_table_hash", colocated_tbl_id, ht.ToUint64())));
  ASSERT_EQ(full_rows, 10);
  ASSERT_NE(full_hash, 0);

  // Explicit open bounds (start == -inf, end == +inf) must reproduce the full-table result.
  auto [open_rows, open_hash] = parse_totals(ASSERT_RESULT(CallAdmin(
      "get_table_hash", colocated_tbl_id, ht.ToUint64(), std::string(), std::string())));
  ASSERT_EQ(open_rows, full_rows);
  ASSERT_EQ(open_hash, full_hash);

  // A key range is scoped to a single table by passing that (child) colocated table's id. Split
  // the table into three disjoint, complementary segments (start inclusive, end exclusive); the
  // middle segment specifies BOTH bounds:
  //   lo  = [-inf,  key(4))  -> ids 1..3   (3 rows)
  //   mid = [key(4), key(8)) -> ids 4..7   (4 rows)   <- both bounds set
  //   hi  = [key(8), +inf)   -> ids 8..10  (3 rows)
  auto [lo_rows, lo_hash] = parse_totals(ASSERT_RESULT(CallAdmin(
      "get_table_hash", colocated_tbl_id, ht.ToUint64(), std::string(), key4_hex)));
  auto [mid_rows, mid_hash] = parse_totals(ASSERT_RESULT(CallAdmin(
      "get_table_hash", colocated_tbl_id, ht.ToUint64(), key4_hex, key8_hex)));
  auto [hi_rows, hi_hash] = parse_totals(ASSERT_RESULT(CallAdmin(
      "get_table_hash", colocated_tbl_id, ht.ToUint64(), key8_hex, std::string())));

  // Real narrowing, including a both-bounded middle segment.
  ASSERT_EQ(lo_rows, 3);
  ASSERT_EQ(mid_rows, 4);
  ASSERT_EQ(hi_rows, 3);
  // Recombination: the three disjoint segments partition the rows, so counts sum and hashes XOR
  // back to the full-table totals (the colocated tablet hosts only this one user table).
  ASSERT_EQ(lo_rows + mid_rows + hi_rows, full_rows);
  ASSERT_EQ(lo_hash ^ mid_hash ^ hi_hash, full_hash);

  // A key range against the colocation parent table id (which would hash every colocated table)
  // is ambiguous and must be rejected rather than silently returning a whole-tablet result.
  const auto parent_tbl_id = ASSERT_RESULT(GetColocationParentTableId(client_.get(), "codb"));
  ASSERT_NOK(CallAdmin("get_table_hash", parent_tbl_id, ht.ToUint64(), std::string(), key4_hex));
}

// Test get_table_hash with a partition-key sub-range on a NON-colocated, multi-tablet, range-
// sharded table (PRIMARY KEY (k ASC) with SPLIT AT VALUES). The range here crosses real tablet
// boundaries, so this also exercises the client-side tablet skipping (PartitionRangeOverlaps) on
// range -- not hash -- keys. Split keys come from the table's own tablet boundaries; the CLI takes
// them hex-encoded, so arbitrary key bytes round-trip safely.
TEST_F(AdminCliTestWithYSQL, TestGetTableXorHashKeyRangeRangeSharded) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  // Range-sharded on a text key, split into three tablets at 'd' and 'h'.
  ASSERT_OK(conn.Execute(
      "CREATE TABLE range_split_tbl (k TEXT, v INT, PRIMARY KEY (k ASC)) "
      "SPLIT AT VALUES (('d'), ('h'))"));
  // 10 rows keyed 'a'..'j'; splits at 'd','h' place [a,b,c] [d,e,f,g] [h,i,j] in three tablets.
  ASSERT_OK(conn.Execute(
      "INSERT INTO range_split_tbl (k, v) SELECT chr(96 + i), i FROM generate_series(1, 10) i"));

  auto get_table_id = [this](const std::string& name) -> Result<TableId> {
    auto tables = VERIFY_RESULT(client_->ListTables(name, /* exclude_ysql = */ false, "yugabyte"));
    SCHECK_EQ(tables.size(), 1U, IllegalState, Format("Expected one table named $0", name));
    return tables.front().table_id();
  };
  const auto table_id = ASSERT_RESULT(get_table_id("range_split_tbl"));

  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(client_->GetTabletsFromTableId(table_id, /*max_tablets=*/0, &tablets));
  ASSERT_EQ(tablets.size(), 3);
  // The two non-empty partition_key_starts are the server-encoded boundaries for 'd' and 'h'.
  std::vector<std::string> split_keys;
  for (const auto& tablet : tablets) {
    if (!tablet.partition().partition_key_start().empty()) {
      split_keys.push_back(tablet.partition().partition_key_start());
    }
  }
  ASSERT_EQ(split_keys.size(), 2);
  std::sort(split_keys.begin(), split_keys.end());
  const auto keyd_hex = strings::b2a_hex(split_keys[0]);  // boundary for 'd'
  const auto keyh_hex = strings::b2a_hex(split_keys[1]);  // boundary for 'h'

  // Fixed read time so every call below observes identical data.
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto parse_totals = [](const std::string& output) -> std::pair<uint64_t, uint64_t> {
    const auto row_prefix = "Total row count: ";
    const auto hash_prefix = "Total XOR hash: ";
    auto rp = output.find(row_prefix);
    auto hp = output.find(hash_prefix);
    CHECK(rp != std::string::npos && hp != std::string::npos) << output;
    return {std::stoull(output.substr(rp + strlen(row_prefix))),
            std::stoull(output.substr(hp + strlen(hash_prefix)))};
  };

  auto [full_rows, full_hash] =
      parse_totals(ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())));
  ASSERT_EQ(full_rows, 10);
  ASSERT_NE(full_hash, 0);

  // Three disjoint, complementary segments, each landing in a different tablet:
  //   lo  = [-inf, key('d'))     -> a,b,c    (3 rows)
  //   mid = [key('d'), key('h')) -> d,e,f,g  (4 rows)
  //   hi  = [key('h'), +inf)     -> h,i,j    (3 rows)
  auto [lo_rows, lo_hash] = parse_totals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), std::string(), keyd_hex)));
  auto [mid_rows, mid_hash] = parse_totals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), keyd_hex, keyh_hex)));
  auto [hi_rows, hi_hash] = parse_totals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), keyh_hex, std::string())));

  ASSERT_EQ(lo_rows, 3);
  ASSERT_EQ(mid_rows, 4);
  ASSERT_EQ(hi_rows, 3);
  ASSERT_EQ(lo_rows + mid_rows + hi_rows, full_rows);
  ASSERT_EQ(lo_hash ^ mid_hash ^ hi_hash, full_hash);
}

namespace {

// Pulls "Total row count", "Total XOR hash" and the optional "Next key" out of get_table_hash
// output. The returned next key is empty when the scan covered the whole requested range.
std::tuple<uint64_t, uint64_t, std::string> ParseHashTotals(const std::string& output) {
  const auto row_prefix = "Total row count: ";
  const auto hash_prefix = "Total XOR hash: ";
  const auto next_prefix = "Next key: ";
  auto rp = output.find(row_prefix);
  auto hp = output.find(hash_prefix);
  CHECK(rp != std::string::npos && hp != std::string::npos) << output;
  auto rows = std::stoull(output.substr(rp + strlen(row_prefix)));
  auto hash = std::stoull(output.substr(hp + strlen(hash_prefix)));
  std::string next_key;
  auto np = output.find(next_prefix);
  if (np != std::string::npos) {
    auto start = np + strlen(next_prefix);
    auto end = output.find('\n', start);
    next_key = output.substr(start, end == std::string::npos ? std::string::npos : end - start);
    boost::trim(next_key);
  }
  return {rows, hash, next_key};
}

} // namespace

// Cap a range-table hash at N rows, resume from Next key, and check the XOR of the pieces equals
// the uncapped full-table hash.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashMaxRows) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute(
      "CREATE TABLE range_cap_tbl (k TEXT, v INT, PRIMARY KEY (k ASC)) "
      "SPLIT AT VALUES (('d'), ('h'))"));
  ASSERT_OK(conn.Execute(
      "INSERT INTO range_cap_tbl (k, v) SELECT chr(96 + i), i FROM generate_series(1, 10) i"));

  auto tables = ASSERT_RESULT(
      client_->ListTables("range_cap_tbl", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto [full_rows, full_hash, full_next] =
      ParseHashTotals(ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())));
  ASSERT_EQ(full_rows, 10);
  ASSERT_TRUE(full_next.empty());

  auto [cap_rows, cap_hash, next_key] = ParseHashTotals(
      ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64(), "", "", "4")));
  ASSERT_EQ(cap_rows, 4);
  ASSERT_FALSE(next_key.empty());

  auto [rest_rows, rest_hash, rest_next] = ParseHashTotals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), next_key, std::string())));
  ASSERT_EQ(rest_rows, 6);
  ASSERT_TRUE(rest_next.empty());
  ASSERT_EQ(cap_rows + rest_rows, full_rows);
  ASSERT_EQ(cap_hash ^ rest_hash, full_hash);
}

// Every page of a capped scan has to be read at one pinned time. Each page is a separate yb-admin
// invocation, and with read-ht left at 0 each one picks its own current time, so under concurrent
// writes the pages describe different states of the table and folding them together produces a
// total that never existed. Passing the first page's Read HT to the rest is what makes the fold
// equal an uncapped scan at that time.
//
// Rows are inserted between pages here so that this is actually exercised rather than assumed. They
// land after the pinned time, so every page has to be blind to them.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashMaxRowsPinnedReadTimeIgnoresLaterWrites) {
  BuildAndStart();

  constexpr int kNumRows = 20;
  constexpr int kCap = 3;

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE TABLE paged_writes (k INT, v INT, PRIMARY KEY (k ASC))"));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO paged_writes (k, v) SELECT i, i FROM generate_series(1, $0) i", kNumRows));

  auto tables =
      ASSERT_RESULT(client_->ListTables("paged_writes", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();

  // Taken before any of the interleaved inserts exist, and used by every page below.
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto [full_rows, full_hash, full_next] =
      ParseHashTotals(ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())));
  ASSERT_EQ(full_rows, kNumRows);
  ASSERT_TRUE(full_next.empty());

  const auto cap_arg = std::to_string(kCap);
  constexpr int kMaxWindows = 4 * kNumRows / kCap;
  std::string next_key;
  uint64_t total_rows = 0;
  uint64_t total_hash = 0;
  int windows = 0;
  int extra_key = kNumRows + 1;
  do {
    auto [rows, hash, next] = ParseHashTotals(ASSERT_RESULT(
        CallAdmin("get_table_hash", table_id, ht.ToUint64(), next_key, std::string(), cap_arg)));
    total_rows += rows;
    total_hash ^= hash;
    next_key = next;
    ++windows;
    // Written between two pages and therefore after the pinned time. The key is above every
    // existing one, so it sits past the resume point the next page starts from, which is exactly
    // where a page reading at its own current time would pick it up.
    ASSERT_OK(conn.ExecuteFormat(
        "INSERT INTO paged_writes (k, v) VALUES ($0, $1)", extra_key, extra_key));
    ++extra_key;
  } while (!next_key.empty() && windows < kMaxWindows);

  ASSERT_TRUE(next_key.empty()) << "table not drained after " << windows << " windows";
  ASSERT_GT(windows, 1) << "the cap never paged, so no write ever landed between two pages";
  // The pages fold to the table exactly as it stood at the pinned time.
  ASSERT_EQ(total_rows, full_rows);
  ASSERT_EQ(total_hash, full_hash);

  // The inserts really did land, so the agreement above is the pinned read time holding them out
  // rather than there having been nothing to hold out.
  auto later_ht = ASSERT_RESULT(cluster_->master()->GetServerTime());
  auto [later_rows, later_hash, later_next] = ParseHashTotals(
      ASSERT_RESULT(CallAdmin("get_table_hash", table_id, later_ht.ToUint64())));
  ASSERT_TRUE(later_next.empty());
  ASSERT_EQ(later_rows, kNumRows + windows);
  ASSERT_NE(later_hash, full_hash);
}

// Same row cap on a hash-partitioned table. A hash tablet is a 2-byte hash band that a 2-byte
// partition key cannot address into, so a capped scan that stops partway through a band reports an
// encoded row key as its Next key. Drain the table one capped window at a time and check that the
// windows partition it exactly.
//
// The tablet count and the cap are both load-bearing. A tablet bound for SPLIT INTO n is a bare
// 2-byte hash at i * 65536 / n, while a continuation key is a full encoded row key that always
// begins with kUInt16Hash ('G' == 0x47). Comparing the two without lifting the bound into the
// encoded space -- the bug this test guards -- drops every tablet whose end sorts at or below
// 0x47xx. At 3 tablets the only bounds are 0x5555 and 0xAAAA and the last tablet is unbounded, so
// no tablet is ever skippable and the raw comparison behaves exactly like the correct one: the
// test passed either way. 8 tablets puts bounds at 0x2000 and 0x4000, below 0x47xx, and a cap
// small enough to stop the first window inside the lowest band makes the next window resume from a
// key that a raw comparison reads as already past those two tablets, silently dropping their rows.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashMaxRowsHashPartitioned) {
  BuildAndStart();

  constexpr int kNumTablets = 8;
  constexpr int kNumRows = 100;
  // Well under the ~kNumRows / kNumTablets rows of the lowest band, so the first window stops
  // inside it rather than clearing it.
  constexpr int kCap = 5;

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE hash_cap_tbl (k INT, v INT, PRIMARY KEY (k HASH)) SPLIT INTO $0 TABLETS",
      kNumTablets));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO hash_cap_tbl (k, v) SELECT i, i FROM generate_series(1, $0) i", kNumRows));

  auto tables = ASSERT_RESULT(
      client_->ListTables("hash_cap_tbl", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto [full_rows, full_hash, full_next] =
      ParseHashTotals(ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())));
  ASSERT_EQ(full_rows, kNumRows);
  ASSERT_TRUE(full_next.empty());

  const auto cap_arg = std::to_string(kCap);
  auto [cap_rows, cap_hash, next_key] = ParseHashTotals(
      ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64(), "", "", cap_arg)));
  ASSERT_EQ(cap_rows, kCap);
  ASSERT_FALSE(next_key.empty());

  // A scan stopped mid-band has to hand back an encoded row key, not a partition bound: a 2-byte
  // bound cannot name a row inside a band. A partition bound is 2 bytes, so 4 hex characters;
  // anything longer is not one.
  ASSERT_GT(next_key.size(), 4U)
      << "Next key " << next_key << " is a 2-byte partition bound, but the first window stopped "
      << "inside a tablet and must resume from an encoded row key";
  // And the resume point has to land in one of the low bands (below the 0x4000 bound), because
  // those are the only ones a raw bound comparison would wrongly skip. Without this the windows
  // below could partition the table correctly whether or not the bounds are lifted into the
  // encoded space, which is what made the 3-tablet version of this test unable to fail.
  const auto next_key_hash = next_key.substr(2, 4);
  ASSERT_LT(next_key_hash, "4000")
      << "Next key " << next_key << " resumes in hash band 0x" << next_key_hash
      << ", at or above the 0x4000 tablet bound; no tablet below it can be wrongly skipped, so "
      << "this window cannot detect a raw partition-bound comparison";

  uint64_t total_rows = cap_rows;
  uint64_t total_hash = cap_hash;
  // kNumRows in windows of kCap needs kNumRows / kCap scans, plus one per window that ends exactly
  // on a tablet boundary. The bound only keeps a bug from looping forever.
  constexpr int kMaxWindows = 4 * kNumRows / kCap;
  int windows = 1;
  for (; windows < kMaxWindows && !next_key.empty(); ++windows) {
    auto [rows, hash, next] = ParseHashTotals(ASSERT_RESULT(
        CallAdmin("get_table_hash", table_id, ht.ToUint64(), next_key, std::string(), cap_arg)));
    total_rows += rows;
    total_hash ^= hash;
    next_key = next;
  }
  ASSERT_TRUE(next_key.empty()) << "Table not drained after " << windows << " windows";
  ASSERT_EQ(total_rows, full_rows);
  ASSERT_EQ(total_hash, full_hash);
}

// A cap that runs out exactly as a tablet ends resumes from the next tablet's start, which is the
// one continuation key not taken from a row. It still has to be reported encoded, like every other
// one: callers compare a continuation key against range bounds, which are encoded, and a bare
// 2-byte hash sorts above almost every encoded key. Reported raw, a bound reads as already passed,
// so a range is declared covered with whole tablets inside it never hashed -- a clean result over
// rows nobody read.
//
// get_table_hash alone cannot show this, because it re-encodes a 2-byte key on the way back in and
// so drains the table either way. Only the byte space of the key it hands out distinguishes them.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashNextKeyIsEncodedOnTabletBoundary) {
  BuildAndStart();

  constexpr int kNumTablets = 8;
  constexpr int kNumRows = 100;

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE boundary_cap_tbl (k INT, v INT, PRIMARY KEY (k HASH)) SPLIT INTO $0 TABLETS",
      kNumTablets));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO boundary_cap_tbl (k, v) SELECT i, i FROM generate_series(1, $0) i", kNumRows));

  auto tables = ASSERT_RESULT(
      client_->ListTables("boundary_cap_tbl", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto [full_rows, full_hash, full_next] =
      ParseHashTotals(ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())));
  ASSERT_EQ(full_rows, kNumRows);
  ASSERT_TRUE(full_next.empty());

  // The leading tablet is the one reporting an empty start. Capping at exactly the number of rows
  // below its end is what lands the scan on the boundary instead of inside a tablet: the tablet is
  // hashed whole, so it returns no continuation key of its own, and the next tablet is where the
  // spent budget is noticed.
  auto json_out = ASSERT_RESULT(
      CallAdmin("list_tablets", "ysql.yugabyte", "boundary_cap_tbl", "0", "JSON"));
  boost::erase_all(json_out, "\n");
  JsonDocument doc;
  auto root = ASSERT_RESULT(doc.Parse(json_out));
  std::string first_end_hex;
  for (const auto& entry : ASSERT_RESULT(root["tablets"].GetArray())) {
    const auto start = ASSERT_RESULT(entry["partition_key_start_hex"].GetString());
    if (start.empty()) {
      first_end_hex = ASSERT_RESULT(entry["partition_key_end_hex"].GetString());
      break;
    }
  }
  ASSERT_FALSE(first_end_hex.empty()) << "no leading tablet in list_tablets output";

  auto [first_rows, first_hash, first_next] = ParseHashTotals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), std::string(), first_end_hex)));
  ASSERT_TRUE(first_next.empty());
  ASSERT_GT(first_rows, 0U) << "leading tablet holds no rows, so no cap can end on its boundary";

  auto [cap_rows, cap_hash, next_key] = ParseHashTotals(ASSERT_RESULT(CallAdmin(
      "get_table_hash", table_id, ht.ToUint64(), "", "", std::to_string(first_rows))));
  ASSERT_EQ(cap_rows, first_rows);
  ASSERT_EQ(cap_hash, first_hash);
  ASSERT_FALSE(next_key.empty()) << "cap ran out with tablets still unhashed, so a key is owed";

  // An encoded hash key is kUInt16Hash ('G' == 0x47) followed by the 2-byte hash, so 6 hex
  // characters. Neither check alone is enough: a raw key may itself begin 0x47, and a longer key
  // may be an encoded row key rather than an encoded bound.
  ASSERT_EQ(next_key.substr(0, 2), "47")
      << "Next key " << next_key << " does not begin with kUInt16Hash, so it is not encoded";
  ASSERT_GT(next_key.size(), 4U)
      << "Next key " << next_key << " is a bare 2-byte partition bound, not an encoded key";

  // The rest of the table still hashes from it, so nothing was skipped or counted twice.
  auto [rest_rows, rest_hash, rest_next] = ParseHashTotals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), next_key, std::string())));
  ASSERT_TRUE(rest_next.empty());
  ASSERT_EQ(cap_rows + rest_rows, full_rows);
  ASSERT_EQ(cap_hash ^ rest_hash, full_hash);
}

// The same values in a different arrangement have to hash differently, or a cross-cluster
// comparison reports a target that has visibly lost data as kMatch. Each pair of tables below is
// created from identical DDL, so the two hold the same column ids and differ only in where the
// values sit. Every pair hashed identically before per-column and per-row identity went into the
// hash.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashDistinguishesValuePlacement) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));

  // One tablet, so the hash is over a single scan and nothing depends on how rows land.
  auto table_hash = [&](const std::string& name, const std::string& values) -> Result<uint64_t> {
    RETURN_NOT_OK(conn.ExecuteFormat(
        "CREATE TABLE $0 (k INT PRIMARY KEY, x INT, y INT) SPLIT INTO 1 TABLETS", name));
    RETURN_NOT_OK(conn.ExecuteFormat("INSERT INTO $0 (k, x, y) VALUES $1", name, values));
    auto tables = VERIFY_RESULT(client_->ListTables(name, /* exclude_ysql = */ false, "yugabyte"));
    SCHECK_EQ(tables.size(), 1U, IllegalState, Format("expected exactly one table named $0", name));
    auto output = VERIFY_RESULT(CallAdmin("get_table_hash", tables.front().table_id()));
    return std::get<1>(ParseHashTotals(output));
  };

  // Identical data in two identically shaped tables still has to hash the same, or nothing about
  // the hash is comparable across clusters in the first place.
  const auto same_a = ASSERT_RESULT(table_hash("placement_same_a", "(1, 5, 7), (2, 8, 9)"));
  const auto same_b = ASSERT_RESULT(table_hash("placement_same_b", "(1, 5, 7), (2, 8, 9)"));
  ASSERT_EQ(same_a, same_b);

  // Values swapped between two same-typed columns of one row.
  const auto swapped_columns =
      ASSERT_RESULT(table_hash("placement_swapped_cols", "(1, 7, 5), (2, 8, 9)"));
  ASSERT_NE(same_a, swapped_columns);

  // Two columns of one row holding the same value: the pair used to cancel out, leaving the row
  // hashing as if it carried its key alone.
  const auto equal_fives = ASSERT_RESULT(table_hash("placement_equal_fives", "(1, 5, 5)"));
  const auto equal_sevens = ASSERT_RESULT(table_hash("placement_equal_sevens", "(1, 7, 7)"));
  ASSERT_NE(equal_fives, equal_sevens);
  const auto key_only_null = ASSERT_RESULT(table_hash("placement_key_only", "(1, NULL, NULL)"));
  ASSERT_NE(equal_fives, key_only_null);

  // The same value moved between two rows of the same column, which is the shape a misapplied
  // update takes.
  const auto rows_ab = ASSERT_RESULT(table_hash("placement_rows_ab", "(1, 5, 0), (2, 7, 0)"));
  const auto rows_ba = ASSERT_RESULT(table_hash("placement_rows_ba", "(1, 7, 0), (2, 5, 0)"));
  ASSERT_NE(rows_ab, rows_ba);

  // A value shared by two rows, versus the same rows with that column nulled out on both.
  const auto shared_value = ASSERT_RESULT(table_hash("placement_shared", "(1, 5, 0), (2, 5, 0)"));
  const auto shared_nulled =
      ASSERT_RESULT(table_hash("placement_nulled", "(1, NULL, 0), (2, NULL, 0)"));
  ASSERT_NE(shared_value, shared_nulled);
}

// list_tablets JSON exposes partition_key_*_hex so a verify driver can hash range tablets one at a
// time.
TEST_F(AdminCliTestWithYSQL, TestListTabletsJsonPartitionKeyHex) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute(
      "CREATE TABLE range_hex_tbl (k TEXT, v INT, PRIMARY KEY (k ASC)) "
      "SPLIT AT VALUES (('d'), ('h'))"));

  string json_out = ASSERT_RESULT(
      CallAdmin("list_tablets", "ysql.yugabyte", "range_hex_tbl", "0", "JSON"));
  boost::erase_all(json_out, "\n");
  JsonDocument doc;
  auto root = ASSERT_RESULT(doc.Parse(json_out));
  auto tablets = ASSERT_RESULT(root["tablets"].GetArray());
  ASSERT_EQ(tablets.size(), 3);

  size_t empty_starts = 0;
  size_t empty_ends = 0;
  std::unordered_set<std::string> starts;
  std::unordered_set<std::string> ends;
  for (const auto& entry : tablets) {
    auto start = ASSERT_RESULT(entry["partition_key_start_hex"].GetString());
    auto end = ASSERT_RESULT(entry["partition_key_end_hex"].GetString());
    if (start.empty()) {
      empty_starts++;
    } else {
      starts.insert(start);
    }
    if (end.empty()) {
      empty_ends++;
    } else {
      ends.insert(end);
    }
  }
  // Three tablets: [-inf, k1), [k1, k2), [k2, +inf). Internal bounds must match.
  ASSERT_EQ(empty_starts, 1);
  ASSERT_EQ(empty_ends, 1);
  ASSERT_EQ(starts.size(), 2);
  ASSERT_EQ(ends.size(), 2);
  ASSERT_EQ(starts, ends);
}

// The workflow a verify driver runs: read the tablet bounds out of list_tablets, hash each tablet
// as a key range, and expect the pieces to reconstruct the table. It works only if the two commands
// agree on what a bound is, so the hex list_tablets prints has to be exactly what get_table_hash
// accepts. The encoding differs between hash and range partitioning, so both are covered here.
TEST_F(AdminCliTestWithYSQL, TestListTabletsHexBoundsHashWholeTable) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute(
      "CREATE TABLE rt_range_tbl (k TEXT, v INT, PRIMARY KEY (k ASC)) "
      "SPLIT AT VALUES (('d'), ('h'))"));
  ASSERT_OK(conn.Execute(
      "INSERT INTO rt_range_tbl (k, v) SELECT chr(96 + i), i FROM generate_series(1, 10) i"));
  ASSERT_OK(conn.Execute(
      "CREATE TABLE rt_hash_tbl (k INT, v INT, PRIMARY KEY (k HASH)) SPLIT INTO 8 TABLETS"));
  ASSERT_OK(conn.Execute(
      "INSERT INTO rt_hash_tbl (k, v) SELECT i, i FROM generate_series(1, 100) i"));

  // Hashes one table a tablet at a time, using only what list_tablets reports, and returns the
  // combined row count, combined hash and the number of tablets it visited.
  auto hash_by_tablet = [this](const TableId& table_id, const std::string& table_name,
                               uint64_t read_ht) -> Result<std::tuple<uint64_t, uint64_t, size_t>> {
    auto json_out =
        VERIFY_RESULT(CallAdmin("list_tablets", "ysql.yugabyte", table_name, "0", "JSON"));
    boost::erase_all(json_out, "\n");
    JsonDocument doc;
    auto root = VERIFY_RESULT(doc.Parse(json_out));
    uint64_t rows = 0;
    uint64_t hash = 0;
    size_t tablets = 0;
    for (const auto& entry : VERIFY_RESULT(root["tablets"].GetArray())) {
      // An outer tablet reports an empty bound, which get_table_hash reads as unbounded -- so the
      // ends of the table need no special handling by the driver, and get none here.
      auto start = VERIFY_RESULT(entry["partition_key_start_hex"].GetString());
      auto end = VERIFY_RESULT(entry["partition_key_end_hex"].GetString());
      auto [tablet_rows, tablet_hash, next] = ParseHashTotals(
          VERIFY_RESULT(CallAdmin("get_table_hash", table_id, read_ht, start, end)));
      SCHECK(
          next.empty(), IllegalState,
          Format("uncapped scan of one tablet of $0 returned a continuation key", table_name));
      rows += tablet_rows;
      hash ^= tablet_hash;
      ++tablets;
    }
    return std::make_tuple(rows, hash, tablets);
  };

  auto table_id = [this](const std::string& name) -> Result<TableId> {
    auto tables = VERIFY_RESULT(client_->ListTables(name, /* exclude_ysql = */ false, "yugabyte"));
    SCHECK_EQ(tables.size(), 1U, IllegalState, Format("expected one table named $0", name));
    return tables.front().table_id();
  };

  // One read time for every scan, or the pieces would be under no obligation to recombine.
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  // Range partitioning: the bounds are encoded range keys of arbitrary length.
  {
    const auto id = ASSERT_RESULT(table_id("rt_range_tbl"));
    auto [full_rows, full_hash, full_next] =
        ParseHashTotals(ASSERT_RESULT(CallAdmin("get_table_hash", id, ht.ToUint64())));
    ASSERT_EQ(full_rows, 10);
    ASSERT_TRUE(full_next.empty());

    auto [rows, hash, tablets] = ASSERT_RESULT(hash_by_tablet(id, "rt_range_tbl", ht.ToUint64()));
    ASSERT_EQ(tablets, 3);
    ASSERT_EQ(rows, full_rows);
    ASSERT_EQ(hash, full_hash);
  }

  // Hash partitioning: the bounds are bare 2-byte hashes, which the server has to encode into
  // doc-key form before comparing them against anything.
  {
    const auto id = ASSERT_RESULT(table_id("rt_hash_tbl"));
    auto [full_rows, full_hash, full_next] =
        ParseHashTotals(ASSERT_RESULT(CallAdmin("get_table_hash", id, ht.ToUint64())));
    ASSERT_EQ(full_rows, 100);
    ASSERT_TRUE(full_next.empty());

    auto [rows, hash, tablets] = ASSERT_RESULT(hash_by_tablet(id, "rt_hash_tbl", ht.ToUint64()));
    ASSERT_EQ(tablets, 8);
    ASSERT_EQ(rows, full_rows);
    ASSERT_EQ(hash, full_hash);
  }
}

// The row cap at its boundaries. A driver scans until the Next key comes back empty, so a cap that
// reports a continuation key with nothing behind it costs a wasted scan, and one that reports none
// with rows still behind it drops them silently. The interesting case is a cap spent exactly at a
// tablet edge, where the tablet itself has nothing left to resume from and the continuation has to
// come from the next tablet's start instead.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashMaxRowsBoundaries) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  // Ten rows 'a'..'j' over three tablets, holding 3, 4 and 3 of them.
  ASSERT_OK(conn.Execute(
      "CREATE TABLE cap_edge_tbl (k TEXT, v INT, PRIMARY KEY (k ASC)) "
      "SPLIT AT VALUES (('d'), ('h'))"));
  ASSERT_OK(conn.Execute(
      "INSERT INTO cap_edge_tbl (k, v) SELECT chr(96 + i), i FROM generate_series(1, 10) i"));

  auto tables = ASSERT_RESULT(
      client_->ListTables("cap_edge_tbl", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto [full_rows, full_hash, full_next] =
      ParseHashTotals(ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())));
  ASSERT_EQ(full_rows, 10);
  ASSERT_TRUE(full_next.empty());

  // A cap spent exactly on the first tablet's last row. The tablet's own scan ended because it ran
  // out of rows, not because of the cap, so it has no row to continue from; the continuation has to
  // name where the next tablet starts.
  {
    auto [rows, hash, next] = ParseHashTotals(
        ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64(), "", "", "3")));
    ASSERT_EQ(rows, 3);
    ASSERT_FALSE(next.empty())
        << "a cap spent at a tablet edge left seven rows unhashed and reported the scan complete";

    auto [rest_rows, rest_hash, rest_next] = ParseHashTotals(ASSERT_RESULT(
        CallAdmin("get_table_hash", table_id, ht.ToUint64(), next, std::string())));
    ASSERT_EQ(rest_rows, 7);
    ASSERT_TRUE(rest_next.empty());
    ASSERT_EQ(hash ^ rest_hash, full_hash);
  }

  // A cap equal to the row count is spent on the table's last row, and there is nothing to resume.
  {
    auto [rows, hash, next] = ParseHashTotals(
        ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64(), "", "", "10")));
    ASSERT_EQ(rows, 10);
    ASSERT_EQ(hash, full_hash);
    ASSERT_TRUE(next.empty()) << "a cap spent on the last row must report the scan complete";
  }

  // A cap the scan never reaches is an uncapped scan.
  {
    auto [rows, hash, next] = ParseHashTotals(
        ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64(), "", "", "11")));
    ASSERT_EQ(rows, 10);
    ASSERT_EQ(hash, full_hash);
    ASSERT_TRUE(next.empty());
  }

  // A cap of one, which makes every tablet edge a window edge as well.
  {
    uint64_t total_rows = 0;
    uint64_t total_hash = 0;
    std::string next;
    int windows = 0;
    constexpr int kMaxWindows = 40;
    do {
      auto [rows, hash, window_next] = ParseHashTotals(ASSERT_RESULT(
          CallAdmin("get_table_hash", table_id, ht.ToUint64(), next, std::string(), "1")));
      ASSERT_LE(rows, 1);
      total_rows += rows;
      total_hash ^= hash;
      next = window_next;
      ++windows;
    } while (!next.empty() && windows < kMaxWindows);

    ASSERT_TRUE(next.empty()) << "table not drained after " << windows << " windows";
    ASSERT_EQ(total_rows, full_rows);
    ASSERT_EQ(total_hash, full_hash);
    ASSERT_EQ(windows, 10) << "ten rows one at a time should take ten windows, not " << windows;
  }
}

// Paging through a COLOCATED child table, which is the one continuation key path the other cap
// tests never reach. A colocated row's DocKey carries a table id prefix that a non-colocated row
// does not, and the key handed back is the tuple id, meaning that prefix stripped off, so passing
// it back as a start bound has to put it on again. The strip and the re-add have to be exact
// inverses: leave the prefix on and the next scan looks for a key prefixed twice and finds nothing;
// take off too much and it resumes early and hashes rows it already counted.
//
// The second table sharing the tablet is what gives the prefix something to get wrong. With one
// table a bound that lost or doubled its prefix would still select the same rows; with two, a
// mishandled prefix reaches into another table's key space and the row count says so.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashMaxRowsColocatedChild) {
  BuildAndStart();

  constexpr int kNumRows = 20;
  constexpr int kCap = 3;

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE DATABASE pagedb WITH colocation = true"));
  auto db = ASSERT_RESULT(cluster_->ConnectToDB("pagedb"));

  ASSERT_OK(db.Execute("CREATE TABLE paged (id INT, name TEXT, PRIMARY KEY (id ASC))"));
  ASSERT_OK(db.ExecuteFormat(
      "INSERT INTO paged (id, name) SELECT i, 'v' || i FROM generate_series(1, $0) i", kNumRows));
  // Shares the colocated tablet with `paged`.
  ASSERT_OK(db.Execute("CREATE TABLE neighbour (id INT, name TEXT, PRIMARY KEY (id ASC))"));
  ASSERT_OK(db.ExecuteFormat(
      "INSERT INTO neighbour (id, name) SELECT i, 'n' || i FROM generate_series(1, $0) i",
      kNumRows));

  auto tables = ASSERT_RESULT(client_->ListTables("paged", /* exclude_ysql = */ false, "pagedb"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  auto [full_rows, full_hash, full_next] =
      ParseHashTotals(ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())));
  ASSERT_EQ(full_rows, kNumRows);
  ASSERT_TRUE(full_next.empty());

  const auto cap_arg = std::to_string(kCap);
  constexpr int kMaxWindows = 4 * kNumRows / kCap;
  std::string next_key;
  uint64_t total_rows = 0;
  uint64_t total_hash = 0;
  int windows = 0;
  do {
    auto [rows, hash, next] = ParseHashTotals(ASSERT_RESULT(
        CallAdmin("get_table_hash", table_id, ht.ToUint64(), next_key, std::string(), cap_arg)));
    total_rows += rows;
    total_hash ^= hash;
    next_key = next;
    ++windows;
  } while (!next_key.empty() && windows < kMaxWindows);

  ASSERT_TRUE(next_key.empty()) << "table not drained after " << windows << " windows";
  ASSERT_GT(windows, 1) << "the cap never paged, so no continuation key was ever fed back";
  // Exactly the rows of `paged`, so the paging neither stopped short nor crossed into `neighbour`.
  ASSERT_EQ(total_rows, full_rows);
  ASSERT_EQ(total_hash, full_hash);
}

// A row cap needs a concrete table id. A colocation parent covers several tables with independent
// key spaces, so one continuation key cannot say where to resume, and the server rejects the
// combination rather than returning a key that means nothing.
//
// This also covers the scheme version line, which every get_table_hash prints and which a caller
// comparing two hashes by hand is supposed to read first.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashMaxRowsRejectsColocationParent) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE DATABASE capdb WITH colocation = true"));
  auto capdb = ASSERT_RESULT(cluster_->ConnectToDB("capdb"));
  ASSERT_OK(capdb.Execute("CREATE TABLE cap_colo_a (k int PRIMARY KEY, v int)"));
  ASSERT_OK(capdb.Execute("CREATE TABLE cap_colo_b (k int PRIMARY KEY, v int)"));
  ASSERT_OK(capdb.Execute("INSERT INTO cap_colo_a SELECT g, g FROM generate_series(1, 20) g"));
  ASSERT_OK(capdb.Execute("INSERT INTO cap_colo_b SELECT g, g FROM generate_series(1, 10) g"));

  const auto parent_table = ASSERT_RESULT(GetColocationParentTableId(client_.get(), "capdb"));

  // Uncapped, the parent id is a valid request: it hashes the whole tablet.
  auto output = ASSERT_RESULT(CallAdmin("get_table_hash", parent_table));
  auto [rows, hash, next] = ParseHashTotals(output);
  ASSERT_EQ(rows, 30);
  ASSERT_TRUE(next.empty());
  ASSERT_STR_CONTAINS(output, "Hash scheme version: ");

  auto result = CallAdmin("get_table_hash", parent_table, "", "", "", "5");
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.status().ToString(), "max_rows requires a concrete table id");
}

// Folding together hashes taken under two different schemes is what the version field exists to
// prevent: mid-upgrade the leaders of one table sit on binaries that hash differently, and xoring
// their answers yields a number that matches nothing on either scheme.
// TEST_dump_tablet_data_hash_scheme_version lets one tserver stand in for such a binary.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashRefusesToCombineSchemes) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute(
      "CREATE TABLE scheme_tbl (k INT, v INT, PRIMARY KEY (k HASH)) SPLIT INTO 6 TABLETS"));
  ASSERT_OK(conn.Execute(
      "INSERT INTO scheme_tbl (k, v) SELECT i, i FROM generate_series(1, 60) i"));

  auto tables = ASSERT_RESULT(
      client_->ListTables("scheme_tbl", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();

  auto leader_uuids = [this]() -> Result<std::unordered_set<std::string>> {
    auto json_out = VERIFY_RESULT(
        CallAdmin("list_tablets", "ysql.yugabyte", "scheme_tbl", "json", "include_followers"));
    boost::erase_all(json_out, "\n");
    JsonDocument doc;
    auto root = VERIFY_RESULT(doc.Parse(json_out));
    std::unordered_set<std::string> uuids;
    for (const auto& entry : VERIFY_RESULT(root["tablets"].GetArray())) {
      auto leader = entry["leader"];
      uuids.insert(VERIFY_RESULT(leader["uuid"].GetString()));
    }
    return uuids;
  };

  // The override is only observable if one scan reaches both an overridden leader and a normal one,
  // so the table's leaders have to sit on more than one tserver. They are placed that way, but
  // placement settles asynchronously, and a tablet without a leader yet has no uuid to read.
  std::string overridden_uuid;
  ASSERT_OK(WaitFor(
      [&]() -> bool {
        auto uuids = leader_uuids();
        if (!uuids.ok() || uuids->size() < 2) {
          return false;
        }
        overridden_uuid = *uuids->begin();
        return true;
      },
      60s * kTimeMultiplier, "tablet leaders spread over at least two tservers"));

  ExternalTabletServer* overridden = nullptr;
  for (size_t i = 0; i < cluster_->num_tablet_servers(); ++i) {
    if (cluster_->tablet_server(i)->uuid() == overridden_uuid) {
      overridden = cluster_->tablet_server(i);
      break;
    }
  }
  ASSERT_NE(overridden, nullptr) << "no tserver has uuid " << overridden_uuid;

  // Every tserver hashes the same way to begin with.
  ASSERT_OK(CallAdmin("get_table_hash", table_id));

  // One now reports a version none of the others do.
  ASSERT_OK(cluster_->SetFlag(overridden, "TEST_dump_tablet_data_hash_scheme_version", "2"));
  auto mismatch = CallAdmin("get_table_hash", table_id);
  ASSERT_NOK(mismatch);
  ASSERT_STR_CONTAINS(mismatch.status().ToString(), "hashed under different schemes");

  // One now reports nothing at all, as a tserver predating the field does. That has to read as a
  // different scheme rather than as agreement -- a check that only compared the versions it was
  // given would let this through and report a blended total.
  ASSERT_OK(cluster_->SetFlag(overridden, "TEST_dump_tablet_data_hash_scheme_version", "0"));
  auto absent = CallAdmin("get_table_hash", table_id);
  ASSERT_NOK(absent);
  ASSERT_STR_CONTAINS(absent.status().ToString(), "hashed under different schemes");

  // And once it agrees again the scan combines the tablets and reports the scheme it used.
  ASSERT_OK(cluster_->SetFlag(overridden, "TEST_dump_tablet_data_hash_scheme_version", "-1"));
  auto output = ASSERT_RESULT(CallAdmin("get_table_hash", table_id));
  auto [rows, hash, next] = ParseHashTotals(output);
  ASSERT_EQ(rows, 60);
  ASSERT_NE(hash, 0);
  ASSERT_TRUE(next.empty());
  ASSERT_STR_CONTAINS(output, "Hash scheme version: 1");
}

// A bound longer than a partition key is taken as a continuation key from an earlier capped scan.
// Only a real one qualifies: a byte string that is merely long is bad input, and scanning it as a
// key would match nothing and report an empty range as a successful hash of zero rows.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashRejectsMalformedHashBound) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute(
      "CREATE TABLE bad_bound_tbl (k INT, v INT, PRIMARY KEY (k HASH)) SPLIT INTO 4 TABLETS"));
  ASSERT_OK(conn.Execute(
      "INSERT INTO bad_bound_tbl (k, v) SELECT i, i FROM generate_series(1, 40) i"));

  auto tables = ASSERT_RESULT(
      client_->ListTables("bad_bound_tbl", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  // Two bytes is a partition key, which is what an ordinary key range is made of.
  ASSERT_OK(CallAdmin("get_table_hash", table_id, ht.ToUint64(), "8000", std::string()));

  // One byte is too short to be either kind of bound.
  auto too_short = CallAdmin("get_table_hash", table_id, ht.ToUint64(), "80", std::string());
  ASSERT_NOK(too_short);
  ASSERT_STR_CONTAINS(too_short.status().ToString(), "requires a 2-byte partition key bound");

  // Longer, but not an encoded row key: 0x00 is not the marker a hash row key opens with.
  auto not_a_row_key =
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), "00112233445566", std::string());
  ASSERT_NOK(not_a_row_key);
  ASSERT_STR_CONTAINS(
      not_a_row_key.status().ToString(), "does not begin like an encoded row key");

  // A continuation key from a real capped scan still resumes, so the check above rejects malformed
  // bounds without rejecting the bounds this command hands out.
  auto [capped_rows, capped_hash, next] = ParseHashTotals(
      ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64(), "", "", "3")));
  ASSERT_EQ(capped_rows, 3);
  ASSERT_FALSE(next.empty());
  auto [rest_rows, rest_hash, rest_next] = ParseHashTotals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), next, std::string())));
  ASSERT_EQ(capped_rows + rest_rows, 40);
  ASSERT_TRUE(rest_next.empty());
  ASSERT_EQ(capped_hash ^ rest_hash, std::get<1>(ParseHashTotals(
      ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())))));
}

// Omitting read_ht sends the command down the read-time resolution path, which draws the time from
// the target's xCluster safe time. This cluster has no inbound replication at all, so no safe time
// exists for the namespace and there is no time to resolve.
//
// The failure has to be kError, not kTryAgain or kDiverged: nothing about the data was learned, and
// nothing will be learned by retrying, so a driver has to be told to supply a time rather than to
// come back later. Above all it must not reach the hashes -- a hash at an unresolved time would let
// each universe pin its own instant, and every write in between would surface as false divergence.
// The absence of both per-side totals in the JSON is what shows nothing was hashed.
TEST_F(AdminCliTestWithYSQL, TestVerifyXClusterSliceWithoutSafeTimeIsInfra) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE TABLE verify_no_safe_time_tbl (k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn.Execute(
      "INSERT INTO verify_no_safe_time_tbl SELECT g, g FROM generate_series(1, 5) g"));
  auto tables = ASSERT_RESULT(client_->ListTables(
      "verify_no_safe_time_tbl", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();

  // read_ht omitted entirely. The command still exits 0: the tool ran, and the verdict is in the
  // JSON rather than in the process status.
  string json_out = ASSERT_RESULT(CallAdmin(
      "verify_xcluster_slice", table_id, table_id, GetMasterAddresses()));
  boost::erase_all(json_out, "\n");
  JsonDocument doc;
  auto root = ASSERT_RESULT(doc.Parse(json_out));
  ASSERT_EQ(ASSERT_RESULT(root["result"].GetString()), "kError");

  // Assert on the message, not just the verdict. kError on its own tells an operator nothing about
  // what to fix.
  const auto detail = ASSERT_RESULT(root["detail"].GetString());
  ASSERT_STR_CONTAINS(detail, "unable to resolve the target's xCluster safe time");
  ASSERT_STR_CONTAINS(detail, "no usable xCluster safe time for namespace");

  // Neither side may appear: a side is emitted only when it hashed, so their absence is the
  // observable form of "the hashes were never called".
  ASSERT_FALSE(root["source"].IsValid());
  ASSERT_FALSE(root["target"].IsValid());

  // An explicit read time cannot bypass the same safety check.
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());
  string pinned_out = ASSERT_RESULT(CallAdmin(
      "verify_xcluster_slice", table_id, table_id, GetMasterAddresses(), ht.ToUint64()));
  boost::erase_all(pinned_out, "\n");
  JsonDocument pinned_doc;
  auto pinned_root = ASSERT_RESULT(pinned_doc.Parse(pinned_out));
  ASSERT_EQ(ASSERT_RESULT(pinned_root["result"].GetString()), "kError");
  ASSERT_FALSE(pinned_root["source"].IsValid());
  ASSERT_FALSE(pinned_root["target"].IsValid());
}

// Invalid positional values must fail before verification can silently reinterpret them.
TEST_F(AdminCliTestWithYSQL, TestVerifyXClusterRejectsInvalidArguments) {
  BuildAndStart();

  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE TABLE verify_flags_tbl (k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn.Execute("INSERT INTO verify_flags_tbl SELECT g, g FROM generate_series(1, 5) g"));
  auto tables = ASSERT_RESULT(
      client_->ListTables("verify_flags_tbl", /* exclude_ysql = */ false, "yugabyte"));
  ASSERT_EQ(tables.size(), 1U);
  const auto table_id = tables.front().table_id();

  auto slice_at = [&](const std::string& read_ht) {
    return CallAdmin(
        "verify_xcluster_slice", table_id, table_id, GetMasterAddresses(), read_ht);
  };

  // A literal negative read time is not tested because it cannot be reached: flag parsing claims
  // any leading-dash token, so "-1" here is rejected as an unknown flag named 1 before the argument
  // is ever parsed as a time. The guard against it stays in the code as a guard on the conversion
  // to unsigned, not as a reachable input.
  //
  // 0 is the sentinel for "resolve a time for me", which a caller asks for by omitting the argument
  // entirely. Passing it literally is someone who believes they pinned a time and did not.
  auto zero_ht = slice_at("0");
  ASSERT_NOK(zero_ht);
  ASSERT_STR_CONTAINS(zero_ht.status().ToString(), "read_ht must be a positive hybrid time");

  // A thread count below one is not a smaller sweep but no sweep at all, and the sweep clamps to
  // sequential rather than refusing, so an accepted 0 would report a clean result over work it
  // never did.
  //
  // The argument is checked before the nonexistent group is looked up, so a "no such replication
  // group" error would itself be the failure this asserts against.
  auto no_concurrency = CallAdmin(
      "verify_xcluster_group", "no_such_replication_group", "", "0");
  ASSERT_NOK(no_concurrency);
  ASSERT_STR_CONTAINS(
      no_concurrency.status().ToString(), "max_concurrent_ranges must be at least 1");

  // A valid read time passes argument validation and reaches verification. This cluster has no
  // xCluster safe time, so verification reports kError rather than rejecting the argument.
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());
  auto ok_out = ASSERT_RESULT(slice_at(std::to_string(ht.ToUint64())));
  boost::erase_all(ok_out, "\n");
  JsonDocument ok_doc;
  auto ok_root = ASSERT_RESULT(ok_doc.Parse(ok_out));
  ASSERT_EQ(ASSERT_RESULT(ok_root["result"].GetString()), "kError");
  ASSERT_STR_CONTAINS(
      ASSERT_RESULT(ok_root["detail"].GetString()),
      "unable to resolve the target's xCluster safe time");

  // --xcluster_source_certs_dir_name has to reach the source connection and only it. This cluster
  // serves plaintext, so naming certificates for the source makes that one connection attempt TLS
  // and it fails, while the target connection, made without certificates, still succeeds. Were the
  // flag ignored, or applied to both, the failure would not be this one.
  auto source_certs = CallAdmin(
      "--xcluster_source_certs_dir_name", GetCertsDir(),
      "--yb_client_admin_rpc_timeout_sec", "5",
      "verify_xcluster_slice", table_id, table_id, GetMasterAddresses());
  ASSERT_NOK(source_certs);
  ASSERT_STR_CONTAINS(source_certs.status().ToString(), "Unable to connect to source masters");
}


// Test that partition ranges are displayed in correct format for both hash and range partitioning
// (similar to the :9000/tablets endpoint format)
TEST_F(AdminCliTestWithYSQL, TestPartitionRangeFormat) {
  BuildAndStart();

  // Test 1: Hash partitioned table (default CQL table)
  string hash_table_out = ASSERT_RESULT(
      CallAdmin("list_tablets", kTableName.namespace_name(), kTableName.table_name()));

  LOG(INFO) << "Hash partitioned table output: " << hash_table_out;

  // Verify hash partition format: hash_split: [0x0000, 0x3332]
  ASSERT_NE(hash_table_out.find("hash_split:"), string::npos)
      << "Expected hash_split format for hash table, got: " << hash_table_out;

  ASSERT_NE(hash_table_out.find("0x"), string::npos)
      << "Expected hex format ranges for hash table, got: " << hash_table_out;

  // Verify that octal escape sequences are no longer present
  ASSERT_EQ(hash_table_out.find("\\"), string::npos)
      << "Expected no octal escape sequences in hash table, got: " << hash_table_out;

  // Verify that all hex values follow the 0x0000 pattern (4 digits after 0x)
  std::regex hex_pattern(R"(0x[0-9A-Fa-f]{4})");
  size_t hex_count = 0;
  std::string::const_iterator search_start(hash_table_out.cbegin());
  std::smatch match;
  while (std::regex_search(search_start, hash_table_out.cend(), match, hex_pattern)) {
    hex_count++;
    search_start = match.suffix().first;
  }

  // Should have at least 2 hex values (start and end for at least one tablet)
  ASSERT_GE(hex_count, 2) << "Expected at least 2 hex values in 0x0000 format for hash table, got: "
                          << hex_count;

  // Test 2: Range partitioned table (YSQL table with range partitioning)
  // Create a range partitioned table using YSQL
  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));

  // Create a range partitioned table with explicit splits (using ASC to force range partitioning)
  ASSERT_OK(conn.Execute("DROP TABLE IF EXISTS range_test_table"));
  ASSERT_OK(
      conn.Execute("CREATE TABLE range_test_table (k int, v text, PRIMARY KEY (k ASC)) SPLIT AT "
                   "VALUES ((10), (20), (30))"));

  string range_table_out =
      ASSERT_RESULT(CallAdmin("list_tablets", "ysql.yugabyte", "range_test_table"));

  LOG(INFO) << "Range partitioned table output: " << range_table_out;

  // Verify range partition format: range: [<start>, DocKey([], [20])) or range: [DocKey([], [20]),
  // DocKey([], [40]))
  ASSERT_NE(range_table_out.find("range:"), string::npos)
      << "Expected range format for range table, got: " << range_table_out;

  ASSERT_TRUE(
      range_table_out.find("<start>") != string::npos ||
      range_table_out.find("DocKey") != string::npos)
      << "Expected <start> or DocKey format for range table, got: " << range_table_out;

  ASSERT_TRUE(
      range_table_out.find("<end>") != string::npos ||
      range_table_out.find("DocKey") != string::npos)
      << "Expected <end> or DocKey format for range table, got: " << range_table_out;

  // Verify that range table doesn't have hex format (should have DocKey format instead)
  ASSERT_EQ(range_table_out.find("0x"), string::npos)
      << "Range table should not have hex format, got: " << range_table_out;

  // Clean up
  ASSERT_OK(conn.Execute("DROP TABLE range_test_table"));

  // Test 3: YSQL index formatting
  ASSERT_OK(conn.Execute("DROP TABLE IF EXISTS prf_ysql_idx_tbl"));
  ASSERT_OK(
      conn.Execute("CREATE TABLE prf_ysql_idx_tbl (k int, v text, PRIMARY KEY (k ASC)) SPLIT AT "
                   "VALUES ((10), (20))"));
  ASSERT_OK(conn.Execute("CREATE INDEX prf_ysql_idx ON prf_ysql_idx_tbl (v)"));

  const string ysql_idx_out =
      ASSERT_RESULT(CallAdmin("list_tablets", "ysql.yugabyte", "prf_ysql_idx"));
  LOG(INFO) << "YSQL index partition output: " << ysql_idx_out;
  const bool ysql_idx_has_hash = ysql_idx_out.find("hash_split:") != string::npos;
  const bool ysql_idx_has_range = ysql_idx_out.find("range:") != string::npos;
  ASSERT_TRUE(ysql_idx_has_hash || ysql_idx_has_range)
      << "Expected either hash_split or range format for YSQL index, got: " << ysql_idx_out;
  if (ysql_idx_has_hash) {
    ASSERT_NE(ysql_idx_out.find("0x"), string::npos)
        << "Expected hex ranges for hash partitioned YSQL index, got: " << ysql_idx_out;
    ASSERT_EQ(ysql_idx_out.find("\\"), string::npos)
        << "Expected no octal escapes for hash partitioned YSQL index, got: " << ysql_idx_out;
  } else {
    ASSERT_TRUE(
        ysql_idx_out.find("<start>") != string::npos || ysql_idx_out.find("DocKey") != string::npos)
        << "Expected <start>/<end> or DocKey for range partitioned YSQL index, got: "
        << ysql_idx_out;
    ASSERT_EQ(ysql_idx_out.find("0x"), string::npos)
        << "Range partitioned YSQL index should not have hex ranges, got: " << ysql_idx_out;
  }

  // Cleanup YSQL objects
  ASSERT_OK(conn.Execute("DROP TABLE prf_ysql_idx_tbl"));

  // Test 3b: YSQL non-range table index formatting (default hash partitioned table)
  ASSERT_OK(conn.Execute("DROP TABLE IF EXISTS prf_ysql_idx_tbl2"));
  ASSERT_OK(conn.Execute("CREATE TABLE prf_ysql_idx_tbl2 (k int PRIMARY KEY, v int)"));
  ASSERT_OK(conn.Execute("CREATE INDEX prf_ysql_idx2 ON prf_ysql_idx_tbl2 (v)"));

  const string ysql_idx2_out =
      ASSERT_RESULT(CallAdmin("list_tablets", "ysql.yugabyte", "prf_ysql_idx2"));
  LOG(INFO) << "YSQL non-range index partition output: " << ysql_idx2_out;
  ASSERT_NE(ysql_idx2_out.find("hash_split:"), string::npos)
      << "Expected hash_split for default-partitioned YSQL index, got: " << ysql_idx2_out;
  ASSERT_NE(ysql_idx2_out.find("0x"), string::npos)
      << "Expected hex ranges for default-partitioned YSQL index, got: " << ysql_idx2_out;
  ASSERT_EQ(ysql_idx2_out.find("\\"), string::npos)
      << "Expected no octal escapes for default-partitioned YSQL index, got: " << ysql_idx2_out;

  ASSERT_OK(conn.Execute("DROP TABLE prf_ysql_idx_tbl2"));

  // Test 4: YCQL table and YCQL secondary index formatting
  auto session = ASSERT_RESULT(CqlConnect());
  const std::string ks = "ks_prf";
  ASSERT_OK(session.ExecuteQueryFormat("CREATE KEYSPACE IF NOT EXISTS $0", ks));
  ASSERT_OK(session.ExecuteQueryFormat("USE $0", ks));

  // Create YCQL table with 5 tablets using YB client API.
  // Create YCQL table with transactions enabled via CQL (tablet count may be defaulted).
  ASSERT_OK(
      session.ExecuteQuery("CREATE TABLE IF NOT EXISTS t1 (k INT PRIMARY KEY, v TEXT) WITH "
                           "transactions = { 'enabled' : true }"));
  ASSERT_OK(session.ExecuteQuery("CREATE INDEX IF NOT EXISTS t1_idx ON t1 (v)"));

  const string ycql_tbl_out = ASSERT_RESULT(CallAdmin("list_tablets", ks, "t1"));
  LOG(INFO) << "YCQL table partition output: " << ycql_tbl_out;
  ASSERT_NE(ycql_tbl_out.find("hash_split:"), string::npos)
      << "Expected hash_split for YCQL table, got: " << ycql_tbl_out;
  ASSERT_NE(ycql_tbl_out.find("0x"), string::npos)
      << "Expected hex ranges for YCQL table, got: " << ycql_tbl_out;
  ASSERT_EQ(ycql_tbl_out.find("\\"), string::npos)
      << "Expected no octal escapes for YCQL table, got: " << ycql_tbl_out;

  const string ycql_idx_out = ASSERT_RESULT(CallAdmin("list_tablets", ks, "t1_idx"));
  LOG(INFO) << "YCQL index partition output: " << ycql_idx_out;
  ASSERT_NE(ycql_idx_out.find("hash_split:"), string::npos)
      << "Expected hash_split for YCQL index, got: " << ycql_idx_out;
  ASSERT_NE(ycql_idx_out.find("0x"), string::npos)
      << "Expected hex ranges for YCQL index, got: " << ycql_idx_out;
  ASSERT_EQ(ycql_idx_out.find("\\"), string::npos)
      << "Expected no octal escapes for YCQL index, got: " << ycql_idx_out;
}

TEST_F(AdminCliTest, TestGetClusterLoadBalancerState) {
  std::string output;
  std::vector<std::string> master_flags;
  std::vector<std::string> ts_flags;
  master_flags.push_back("--enable_load_balancing=true");
  BuildAndStart(ts_flags, master_flags);

  const std::string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder()
                                  .add_master_server_addr(master_address)
                                  .Build());
  output = ASSERT_RESULT(CallAdmin("get_load_balancer_state"));

  ASSERT_NE(output.find("ENABLED"), std::string::npos);

  output = ASSERT_RESULT(CallAdmin("set_load_balancer_enabled", "0"));

  ASSERT_EQ(output.find("Unable to change cluster balancer state"), std::string::npos);

  output = ASSERT_RESULT(CallAdmin("get_load_balancer_state"));

  ASSERT_NE(output.find("DISABLED"), std::string::npos);

  output = ASSERT_RESULT(CallAdmin("set_load_balancer_enabled", "1"));

  ASSERT_EQ(output.find("Unable to change cluster balancer state"), std::string::npos);

  output = ASSERT_RESULT(CallAdmin("get_load_balancer_state"));

  ASSERT_NE(output.find("ENABLED"), std::string::npos);
}

TEST_F(AdminCliTest, TestModifyPlacementPolicy) {
  BuildAndStart();

  // Modify the cluster placement policy to consist of 2 zones.
  ASSERT_OK(CallAdmin("modify_placement_info", "c.r.z0:1:2,c.r.z1:2,c.r.z0:2:3", 5, ""));

  auto output = ASSERT_RESULT(CallAdmin("get_universe_config"));

  std::string expected_placement_blocks =
      "[{\"cloudInfo\":{\"placementCloud\":\"c\",\"placementRegion\":\"r\","
      "\"placementZone\":\"z1\"},\"minNumReplicas\":2},{\"cloudInfo\":{\"placementCloud\":\"c\","
      "\"placementRegion\":\"r\",\"placementZone\":\"z0\"},\"minNumReplicas\":3,"
      "\"maxNumReplicas\":5}]";

  ASSERT_NE(output.find(expected_placement_blocks), string::npos);
  ASSERT_NOK(CallAdmin("modify_placement_info", "c.r.z0::2", 2, ""));
}

TEST_F(AdminCliTest, TestModifyTablePlacementPolicy) {
  // Start a cluster with 3 tservers, each corresponding to a different zone.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 3;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 2;
  std::vector<std::string> master_flags;
  master_flags.push_back("--enable_load_balancing=true");
  master_flags.push_back("--catalog_manager_wait_for_new_tablets_to_elect_leader=false");
  std::vector<std::string> ts_flags;
  ts_flags.push_back("--placement_cloud=c");
  ts_flags.push_back("--placement_region=r");
  ts_flags.push_back("--placement_zone=z${index}");
  BuildAndStart(ts_flags, master_flags);

  const std::string& master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Modify the cluster placement policy to consist of 2 zones.
  ASSERT_OK(CallAdmin("modify_placement_info", "c.r.z0,c.r.z1", 2, ""));

  // Create a new table.
  const auto extra_table = YBTableName(YQLDatabase::YQL_DATABASE_CQL,
                                       kTableName.namespace_name(),
                                       "extra-table");
  // Start a workload.
  TestYcqlWorkload workload(cluster_.get());
  workload.set_table_name(extra_table);
  workload.set_timeout_allowed(true);
  workload.set_sequential_write(true);
  workload.Setup();
  workload.Start();

  // Verify that the table has no custom placement policy set for it.
  std::shared_ptr<client::YBTable> table;
  ASSERT_OK(client->OpenTable(extra_table, &table));
  ASSERT_FALSE(table->replication_info());

  // Use yb-admin_cli to set a custom placement policy different from that of
  // the cluster placement policy for the new table.
  ASSERT_OK(CallAdmin(
      "modify_table_placement_info", kTableName.namespace_name(), "extra-table",
      "c.r.z0,c.r.z1,c.r.z2", 3, ""));

  // Verify that changing the placement _uuid for a table fails if the
  // placement_uuid does not match the cluster live placement_uuid.
  const string& random_placement_uuid = "19dfa091-2b53-434f-b8dc-97280a5f8831";
  ASSERT_NOK(CallAdmin(
      "modify_table_placement_info", kTableName.namespace_name(), "extra-table",
      "c.r.z0,c.r.z1,c.r.z2", 3, random_placement_uuid));

  ASSERT_OK(client->OpenTable(extra_table, &table));
  ASSERT_TRUE(table->replication_info()->live_replicas().placement_uuid().empty());

  // Fetch the placement policy for the table and verify that it matches
  // the custom info set previously.
  ASSERT_OK(client->OpenTable(extra_table, &table));
  vector<bool> found_zones;
  found_zones.assign(3, false);
  ASSERT_EQ(table->replication_info()->live_replicas().placement_blocks_size(), 3);
  for (int ii = 0; ii < 3; ++ii) {
    auto pb = table->replication_info()->live_replicas().placement_blocks(ii).cloud_info();
    ASSERT_EQ(pb.placement_cloud(), "c");
    ASSERT_EQ(pb.placement_region(), "r");
    if (pb.placement_zone() == "z0") {
      found_zones[0] = true;
    } else if (pb.placement_zone() == "z1") {
      found_zones[1] = true;
    } else {
      ASSERT_EQ(pb.placement_zone(), "z2");
      found_zones[2] = true;
    }
  }
  for (const bool found : found_zones) {
    ASSERT_TRUE(found);
  }

  // Perform the same test, but use the table-id instead of table name to set the
  // custom placement policy.
  std::string table_id = "tableid." + table->id();
  ASSERT_OK(CallAdmin("modify_table_placement_info", table_id, "c.r.z1", 1, ""));

  // Verify that changing the placement _uuid for a table fails if the
  // placement_uuid does not match the cluster live placement_uuid.
  ASSERT_NOK(
      CallAdmin("modify_table_placement_info", table_id, "c.r.z1", 1, random_placement_uuid));

  ASSERT_OK(client->OpenTable(extra_table, &table));
  ASSERT_TRUE(table->replication_info()->live_replicas().placement_uuid().empty());

  // Fetch the placement policy for the table and verify that it matches
  // the custom info set previously.
  ASSERT_OK(client->OpenTable(extra_table, &table));
  ASSERT_EQ(table->replication_info()->live_replicas().placement_blocks_size(), 1);
  auto pb = table->replication_info()->live_replicas().placement_blocks(0).cloud_info();
  ASSERT_EQ(pb.placement_cloud(), "c");
  ASSERT_EQ(pb.placement_region(), "r");
  ASSERT_EQ(pb.placement_zone(), "z1");

  // Stop the workload.
  workload.StopAndJoin();
  auto rows_inserted = workload.rows_inserted();
  LOG(INFO) << "Number of rows inserted: " << rows_inserted;

  sleep(5);

  // Verify that there was no data loss.
  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(
    extra_table, ClusterVerifier::EXACTLY, rows_inserted));
}


TEST_F(AdminCliTest, TestCreateTransactionStatusTablesWithPlacements) {
  // Start a cluster with 3 tservers, each corresponding to a different zone.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 3;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 3;
  std::vector<std::string> master_flags;
  master_flags.push_back("--enable_load_balancing=true");
  master_flags.push_back("--catalog_manager_wait_for_new_tablets_to_elect_leader=false");
  std::vector<std::string> ts_flags;
  ts_flags.push_back("--placement_cloud=c");
  ts_flags.push_back("--placement_region=r");
  ts_flags.push_back("--placement_zone=z${index}");
  BuildAndStart(ts_flags, master_flags);

  // Create a new table.
  const auto extra_table = YBTableName(YQLDatabase::YQL_DATABASE_CQL,
                                       kTableName.namespace_name(),
                                       "extra-table");
  // Start a workload.
  TestYcqlWorkload workload(cluster_.get());
  workload.set_table_name(extra_table);
  workload.set_timeout_allowed(true);
  workload.set_sequential_write(true);
  workload.Setup();
  workload.Start();

  const std::string& master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Create transaction tables for each zone.
  for (int i = 0; i < 3; ++i) {
    string table_name = Format("transactions_z$0", i);
    string placement = Format("c.r.z$0", i);
    ASSERT_OK(CallAdmin("create_transaction_table", table_name));
    ASSERT_OK(CallAdmin("modify_table_placement_info", "system", table_name, placement, 1));
  }

  // Verify that the tables are all in transaction status tables in the right zone.
  std::shared_ptr<client::YBTable> table;
  for (int i = 0; i < 3; ++i) {
    const auto table_name =
        YBTableName(YQLDatabase::YQL_DATABASE_CQL, "system", Format("transactions_z$0", i));
    ASSERT_OK(client->OpenTable(table_name, &table));
    ASSERT_EQ(table->table_type(), YBTableType::TRANSACTION_STATUS_TABLE_TYPE);
    ASSERT_EQ(table->replication_info()->live_replicas().placement_blocks_size(), 1);
    auto pb = table->replication_info()->live_replicas().placement_blocks(0).cloud_info();
    ASSERT_EQ(pb.placement_zone(), Format("z$0", i));
  }

  // Add two new tservers, to zone3 and an unused zone.
  std::vector<std::string> existing_zone_ts_flags;
  existing_zone_ts_flags.push_back("--placement_cloud=c");
  existing_zone_ts_flags.push_back("--placement_region=r");
  existing_zone_ts_flags.push_back("--placement_zone=z3");
  ASSERT_OK(cluster_->AddTabletServer(true, existing_zone_ts_flags));

  std::vector<std::string> new_zone_ts_flags;
  new_zone_ts_flags.push_back("--placement_cloud=c");
  new_zone_ts_flags.push_back("--placement_region=r");
  new_zone_ts_flags.push_back("--placement_zone=z4");
  ASSERT_OK(cluster_->AddTabletServer(true, new_zone_ts_flags));

  ASSERT_OK(cluster_->WaitForTabletServerCount(5, 5s));

  // Blacklist the original zone3 tserver.
  ASSERT_OK(cluster_->AddTServerToBlacklist(cluster_->master(), cluster_->tablet_server(2)));

  // Stop the workload.
  workload.StopAndJoin();
  auto rows_inserted = workload.rows_inserted();
  LOG(INFO) << "Number of rows inserted: " << rows_inserted;

  sleep(5);

  // Verify that there was no data loss.
  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCountWithRetries(
    extra_table, ClusterVerifier::EXACTLY, rows_inserted, 20s));
}

TEST_F(AdminCliTest, TestClearPlacementPolicy) {
  // Start a cluster with 3 tservers.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 3;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 2;
  std::vector<std::string> master_flags;
  master_flags.push_back("--enable_load_balancing=true");
  std::vector<std::string> ts_flags;
  ts_flags.push_back("--placement_cloud=c");
  ts_flags.push_back("--placement_region=r");
  ts_flags.push_back("--placement_zone=z");
  BuildAndStart(ts_flags, master_flags);

  // Create the placement config.
  ASSERT_OK(CallAdmin("modify_placement_info", "c.r.z", 3, ""));

  // Ensure that the universe config has placement information.
  auto output = ASSERT_RESULT(CallAdmin("get_universe_config"));
  ASSERT_TRUE(output.find("replicationInfo") != std::string::npos);

  // Clear the placement config.
  ASSERT_OK(CallAdmin("clear_placement_info"));

  // Ensure that the placement config is absent.
  output = ASSERT_RESULT(CallAdmin("get_universe_config"));
  ASSERT_TRUE(output.find("replicationInfo") == std::string::npos);
}

TEST_F(AdminCliTest, DdlLog) {
  const std::string kNamespaceName = "test_namespace";
  const std::string kTableName = "test_table";
  BuildAndStart({}, {});

  auto session = ASSERT_RESULT(CqlConnect());
  ASSERT_OK(session.ExecuteQueryFormat(
      "CREATE KEYSPACE IF NOT EXISTS $0", kNamespaceName));

  ASSERT_OK(session.ExecuteQueryFormat("USE $0", kNamespaceName));

  ASSERT_OK(session.ExecuteQueryFormat(
      "CREATE TABLE $0 (key INT PRIMARY KEY, text_column TEXT) "
      "WITH transactions = { 'enabled' : true }", kTableName));

  ASSERT_OK(session.ExecuteQueryFormat(
      "CREATE INDEX test_idx ON $0 (text_column)", kTableName));

  ASSERT_OK(session.ExecuteQueryFormat(
      "ALTER TABLE $0 ADD int_column INT", kTableName));

  ASSERT_OK(session.ExecuteQuery("DROP INDEX test_idx"));

  ASSERT_OK(session.ExecuteQueryFormat("ALTER TABLE $0 DROP text_column", kTableName));

  auto document = ASSERT_RESULT(CallJsonAdmin("ddl_log"));

  auto log = ASSERT_RESULT(GetMemberAsArray(document, "log"));
  ASSERT_EQ(log.Size(), 3);
  std::vector<std::string> actions;
  actions.reserve(log.Size());
  for (const auto& entry : log) {
    LOG(INFO) << "Entry: " << PrettyWriteRapidJsonToString(entry);
    TableType type;
    bool parse_result = TableType_Parse(
        std::string(ASSERT_RESULT(GetMemberAsStr(entry, "table_type"))), &type);
    ASSERT_TRUE(parse_result);
    ASSERT_EQ(type, TableType::YQL_TABLE_TYPE);
    auto namespace_name = ASSERT_RESULT(GetMemberAsStr(entry, "namespace"));
    ASSERT_EQ(namespace_name, kNamespaceName);
    auto table_name = ASSERT_RESULT(GetMemberAsStr(entry, "table"));
    ASSERT_EQ(table_name, kTableName);
    actions.emplace_back(ASSERT_RESULT(GetMemberAsStr(entry, "action")));
  }
  ASSERT_EQ(actions[0], "Drop column text_column");
  ASSERT_EQ(actions[1], "Drop index test_idx");
  ASSERT_EQ(actions[2], "Add column int_column[int32 NULLABLE VALUE]");
}

TEST_F(AdminCliTest, FlushSysCatalogAll) {
  BuildAndStart();
  string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());
  ASSERT_OK(CallAdmin("flush_sys_catalog"));
}

TEST_F(AdminCliTest, FlushSysCatalogLeaderOnly) {
  BuildAndStart();
  string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());
  ASSERT_OK(CallAdmin("flush_sys_catalog", "leader_only"));
}

TEST_F(AdminCliTest, CompactSysCatalogAll) {
  BuildAndStart();
  string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());
  ASSERT_OK(CallAdmin("compact_sys_catalog"));
}

TEST_F(AdminCliTest, CompactSysCatalogLeaderOnly) {
  BuildAndStart();
  string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());
  ASSERT_OK(CallAdmin("compact_sys_catalog", "leader_only"));
}

// A simple smoke test to ensure it working and
// nothing is broken by future changes.
TEST_F(AdminCliTest, SetWalRetentionSecsTest) {
  constexpr auto WAL_RET_TIME_SEC = 100ul;
  constexpr auto UNEXPECTED_ARG = 911ul;

  BuildAndStart();
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());

  // Default table that gets created;
  const auto& table_name = kTableName.table_name();
  const auto& keyspace = kTableName.namespace_name();

  // No WAL ret time found
  {
    const auto output = ASSERT_RESULT(CallAdmin("get_wal_retention_secs", keyspace, table_name));
    ASSERT_NE(output.find("not set"), std::string::npos);
  }

  // Successfuly set WAL time and verified by the getter
  {
    ASSERT_OK(CallAdmin("set_wal_retention_secs", keyspace, table_name, WAL_RET_TIME_SEC));
    const auto output = ASSERT_RESULT(CallAdmin("get_wal_retention_secs", keyspace, table_name));
    ASSERT_TRUE(
        output.find(std::to_string(WAL_RET_TIME_SEC)) != std::string::npos &&
        output.find(table_name) != std::string::npos);
  }

  // Too many args in input
  {
    const auto output_setter =
        CallAdmin("set_wal_retention_secs", keyspace, table_name, WAL_RET_TIME_SEC, UNEXPECTED_ARG);
    ASSERT_FALSE(output_setter.ok());
    ASSERT_TRUE(output_setter.status().IsRuntimeError());
    ASSERT_TRUE(
        output_setter.status().ToUserMessage().find("Invalid argument") != std::string::npos);

    const auto output_getter =
        CallAdmin("get_wal_retention_secs", keyspace, table_name, UNEXPECTED_ARG);
    ASSERT_FALSE(output_getter.ok());
    ASSERT_TRUE(output_getter.status().IsRuntimeError());
    ASSERT_TRUE(
        output_getter.status().ToUserMessage().find("Invalid argument") != std::string::npos);
  }

  // Outbounded arg in input
  {
    const auto output_setter =
        CallAdmin("set_wal_retention_secs", keyspace, table_name, -WAL_RET_TIME_SEC);
    ASSERT_FALSE(output_setter.ok());
    ASSERT_TRUE(output_setter.status().IsRuntimeError());

    const auto output_getter =
        CallAdmin("get_wal_retention_secs", keyspace, table_name + "NoTable");
    ASSERT_FALSE(output_getter.ok());
    ASSERT_TRUE(output_getter.status().IsRuntimeError());
  }
}

TEST_F(AdminCliTest, AddTransactionStatusTablet) {
  const std::string kNamespaceName = "test_namespace";
  const std::string kTableName = "test_table";
  const auto kWaitNewTabletReadyTimeout = MonoDelta::FromMilliseconds(10000);

  BuildAndStart({}, {});

  string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());

  // Force creation of system.transactions.
  auto session = ASSERT_RESULT(CqlConnect());
  ASSERT_OK(session.ExecuteQueryFormat(
      "CREATE KEYSPACE IF NOT EXISTS $0", kNamespaceName));
  ASSERT_OK(session.ExecuteQueryFormat("USE $0", kNamespaceName));
  ASSERT_OK(session.ExecuteQueryFormat(
      "CREATE TABLE $0 (key INT PRIMARY KEY) "
      "WITH transactions = { 'enabled' : true }", kTableName));

  auto global_txn_table = YBTableName(
      YQL_DATABASE_CQL, master::kSystemNamespaceName, kGlobalTransactionsTableName);
  auto global_txn_table_id = ASSERT_RESULT(client::GetTableId(client_.get(), global_txn_table));

  auto tablets_before = ASSERT_RESULT(CallAdmin(
      "list_tablets", master::kSystemNamespaceName, kGlobalTransactionsTableName));
  auto num_tablets_before = std::count(tablets_before.begin(), tablets_before.end(), '\n');
  LOG(INFO) << "Tablets before adding transaction tablet: " << AsString(tablets_before);

  ASSERT_OK(CallAdmin("add_transaction_tablet", global_txn_table_id));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    auto tablets = VERIFY_RESULT(CallAdmin(
        "list_tablets", master::kSystemNamespaceName, kGlobalTransactionsTableName));
    auto num_tablets = std::count(tablets.begin(), tablets.end(), '\n');
    LOG(INFO) << "Tablets: " << AsString(tablets);
    return num_tablets == num_tablets_before + 1;
  }, kWaitNewTabletReadyTimeout, "Timeout waiting for new status tablet to be ready"));
}

class AddTransactionTabletTest : public AdminCliTest {
 public:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->transaction_table_num_tablets = 1;
  }

  void WaitForTransactionTabletCount(
      std::string_view txn_table, int64_t expected_count, MonoDelta timeout = 20s) {
    int64_t num_tablets;
    ASSERT_OK(WaitFor([&] -> Result<bool> {
      auto tablets = VERIFY_RESULT(CallAdmin(
          "list_tablets", master::kSystemNamespaceName, std::string(txn_table)));
      // -1 to exclude table header.
      num_tablets = std::count(tablets.begin(), tablets.end(), '\n') - 1;
      LOG(INFO) << "Tablets: " << AsString(tablets);
      return num_tablets >= expected_count;
    }, timeout, "Timeout waiting for status tablet count"));
    ASSERT_EQ(num_tablets, expected_count);
  }
};

TEST_F_EX(AdminCliTest, AddStuckTransactionStatusTablet, AddTransactionTabletTest) {
  constexpr auto kNamespaceName = "test_namespace";
  constexpr auto kTableName = "test_table";
  constexpr auto kLocalTransactionTableName = "transactions_local";

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 1;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 1;

  BuildAndStart(/*ts_flags=*/{
    "--ycql_use_local_transaction_tables=true",
    "--TEST_transaction_manager_disable_local_filter=true",
  }, /*master_flags=*/{
    "--autoscale_transaction_tables=false",
    "--tablet_creation_timeout_ms=10000",
  });

  string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());

  // Force creation of system.transactions.
  auto session = ASSERT_RESULT(CqlConnect());
  ASSERT_OK(session.ExecuteQueryFormat(
      "CREATE KEYSPACE IF NOT EXISTS $0", kNamespaceName));
  ASSERT_OK(session.ExecuteQueryFormat("USE $0", kNamespaceName));
  ASSERT_OK(session.ExecuteQueryFormat(
      "CREATE TABLE $0 (key INT PRIMARY KEY) "
      "WITH transactions = { 'enabled' : true }", kTableName));

  auto global_txn_table = YBTableName(
      YQL_DATABASE_CQL, master::kSystemNamespaceName, kGlobalTransactionsTableName);
  auto global_txn_table_id = ASSERT_RESULT(client::GetTableId(client_.get(), global_txn_table));
  ASSERT_NO_FATALS(WaitForTransactionTabletCount(kGlobalTransactionsTableName, /*count=*/1));

  // We create a transaction tablet that gets stuck in CREATING (for tablet_creation_timeout_ms) by
  // adding the tablet after shutting down all tservers, and then restarting everything once create
  // tablet RPCs start failing (see #33820).
  // If this behavior is changed in the future, this test should be changed to create such a tablet
  // by some other means.
  cluster_->tablet_server(0)->Shutdown();
  {
    ASSERT_OK(CallAdmin("add_transaction_tablet", global_txn_table_id));
    auto log_waiter = cluster_->GetMasterLogWaiter(
        Format("Processing pending assignments for table: $0", global_txn_table_id));
    ASSERT_OK(log_waiter.WaitFor(5s));
  }
  cluster_->master()->Shutdown(SafeShutdown::kFalse);
  ASSERT_OK(cluster_->Restart());

  auto do_inserts = [&](size_t start, size_t end) -> Status {
    for (size_t i = 0; i < 10; ++i) {
      RETURN_NOT_OK(session.ExecuteQueryFormat(
          "START TRANSACTION;"
          "INSERT INTO $0.$1(key) VALUES ($2);"
          "COMMIT",
          kNamespaceName, kTableName, i));
    }
    return Status::OK();
  };

  ASSERT_NO_FATALS(WaitForTransactionTabletCount(kGlobalTransactionsTableName, /*count=*/1));
  session = ASSERT_RESULT(CqlConnect());
  ASSERT_OK(do_inserts(0, 10));
  ASSERT_NO_FATALS(WaitForTransactionTabletCount(kGlobalTransactionsTableName, /*count=*/2));
  ASSERT_OK(do_inserts(10, 20));

  ASSERT_OK(CallAdmin("create_transaction_table", kLocalTransactionTableName));
  ASSERT_OK(CallAdmin(
      "modify_table_placement_info", kNamespaceName, kTableName,
      "cloud1.datacenter1.rack1", "1"));
  ASSERT_OK(CallAdmin(
      "modify_table_placement_info", master::kSystemNamespaceName, kLocalTransactionTableName,
      "cloud1.datacenter1.rack1", "1"));
  auto local_txn_table = YBTableName(
      YQL_DATABASE_CQL, master::kSystemNamespaceName, kLocalTransactionTableName);
  auto local_txn_table_id = ASSERT_RESULT(client::GetTableId(client_.get(), local_txn_table));
  ASSERT_NO_FATALS(WaitForTransactionTabletCount(kLocalTransactionTableName, /*count=*/1));

  cluster_->tablet_server(0)->Shutdown();
  {
    auto log_waiter = cluster_->GetMasterLogWaiter(
        Format("Processing pending assignments for table: $0", local_txn_table_id));
    ASSERT_OK(CallAdmin("add_transaction_tablet", local_txn_table_id));
    ASSERT_OK(log_waiter.WaitFor(5s));
  }
  cluster_->master()->Shutdown(SafeShutdown::kFalse);
  ASSERT_OK(cluster_->Restart());

  ASSERT_NO_FATALS(WaitForTransactionTabletCount(kLocalTransactionTableName, /*count=*/1));
  session = ASSERT_RESULT(CqlConnect());
  ASSERT_OK(do_inserts(20, 30));
  ASSERT_NO_FATALS(WaitForTransactionTabletCount(kLocalTransactionTableName, /*count=*/2));
  ASSERT_OK(do_inserts(30, 40));
}

class AdminCliListTabletsTest : public AdminCliTest {
 public:
  template <class... Args>
  Status ListTabletsWithFailure(const std::string& expected_error_substr, Args&&... args) {
    auto result = ListTablets(std::forward<Args>(args)...);
    SCHECK(!result.ok(), IllegalState, "list_tablets command expected to fail");
    auto error = result.status().ToUserMessage();
    SCHECK(
        error.find(expected_error_substr) != std::string::npos,
        IllegalState,
        Format("$0 substring was not found in $1", expected_error_substr, error));
    return Status::OK();
  }

  template <class... Args>
  Result<std::size_t> GetListTabletsCount(Args&&... args) {
    JsonDocument doc;
    auto root = VERIFY_RESULT(doc.Parse(VERIFY_RESULT(ListTablets(std::forward<Args>(args)...,
                                                                  "json"))));
    return root["tablets"].size();
  }

 private:
  template <class... Args>
  Result<std::string> ListTablets(Args&&... args) {
    return CallAdminVec(ToStringVector(
        GetAdminToolPath(), "--master_addresses", GetMasterAddresses(), "list_tablets",
        std::forward<Args>(args)...));
  }
};

TEST_F_EX(AdminCliTest, CheckTableNameAndNamespaceUsage, AdminCliListTabletsTest) {
  ASSERT_NO_FATALS(BuildAndStart());

  // Check table name is missing.
  ASSERT_OK(ListTabletsWithFailure("Empty list of tables", kTableName.namespace_name()));

  // Check namespace does not exist.
  const auto kBadNamespaceName = kTableName.namespace_name() + "bad";
  ASSERT_OK(ListTabletsWithFailure(
      Format("Namespace '$0' of type 'ycql' not found", kBadNamespaceName), kBadNamespaceName,
      kTableName.table_name()));

  // Check wrong keyspace of a table when called with table name.
  const auto client =
      ASSERT_RESULT(YBClientBuilder().add_master_server_addr(GetMasterAddresses()).Build());
  ASSERT_OK(client->CreateNamespace(kBadNamespaceName, YQL_DATABASE_CQL));
  ASSERT_OK(ListTabletsWithFailure(
      Format(
          "Table with name '$0' not found in namespace 'ycql.$1'",
          kTableName.table_name(),
          kBadNamespaceName),
      kBadNamespaceName, kTableName.table_name()));

  // Check wrong keyspace of a table when called with table id.
  const auto tables =
      ASSERT_RESULT(client->ListTables(kTableName.table_name(), /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());
  const auto& table_id = tables.front().table_id();
  ASSERT_OK(ListTabletsWithFailure(
      Format(
          "Table with id '$0' belongs to different namespace 'ycql.$1'",
          table_id,
          kBadNamespaceName),
      kBadNamespaceName,
      Format("tableid.$0", table_id)));
}

TEST_F_EX(AdminCliTest, ListTabletDefaultTenTablets, AdminCliListTabletsTest) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 1;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 1;

  ASSERT_NO_FATALS(BuildAndStart({}, {}));

  YBSchema schema;
  YBSchemaBuilder schema_builder;
  schema_builder.AddColumn("k")->HashPrimaryKey()->Type(DataType::BINARY)->NotNull();
  schema_builder.AddColumn("v")->Type(DataType::BINARY)->NotNull();
  ASSERT_OK(schema_builder.Build(&schema));

  const auto client =
      ASSERT_RESULT(YBClientBuilder().add_master_server_addr(GetMasterAddresses()).Build());
  // Default table that gets created.
  const auto& keyspace = kTableName.namespace_name();
  // Create table with 20 tablets.
  constexpr auto* kTestTableName = "t20";
  ASSERT_OK(client->NewTableCreator()
      ->table_name(YBTableName(YQL_DATABASE_CQL, keyspace, kTestTableName))
      .table_type(YBTableType::YQL_TABLE_TYPE)
      .schema(&schema)
      .num_tablets(20)
      .Create());

  // Test 10 tablets should be listed by default
  auto count = ASSERT_RESULT(GetListTabletsCount(keyspace, kTestTableName));
  ASSERT_EQ(count, 10);

  // Test all tablets should be listed when value is 0
  count = ASSERT_RESULT(GetListTabletsCount(keyspace, kTestTableName, 0));
  ASSERT_EQ(count, 20);
}

TEST_F_EX(AdminCliTest, TestSplitTabletDefault, AdminCliListTabletsTest) {
  BuildAndStart();
  const auto& keyspace = kTableName.namespace_name();
  const auto& table_name = kTableName.table_name();

  // Insert some rows.
  TestYcqlWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_timeout_allowed(true);
  workload.Setup();
  workload.Start();
  workload.WaitInserted(100);
  workload.StopAndJoin();
  LOG(INFO) << "Number of rows inserted: " << workload.rows_inserted();

  // Flush to SST. The middle key determination Tablet::GetEncodedMiddleSplitKey() works off SSTs.
  ASSERT_OK(CallAdmin("flush_table", keyspace, table_name));

  // Split tablet.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_enable_multi_way_tablet_split", "false"));
  ASSERT_OK(CallAdmin("split_tablet", tablet_id_));

  // Verify the default split creates 2 child tablets.
  constexpr int kSplitFactor = 2;
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return kSplitFactor == VERIFY_RESULT(GetListTabletsCount(keyspace, table_name));
      }, 30s, "Wait for tablet split to complete"));
}

TEST_F_EX(AdminCliTest, TestSplitTabletMultiWay, AdminCliListTabletsTest) {
  // 256B data blocks so Cross has enough cut points for a 5-way split.
  BuildAndStart({"--db_block_size_bytes=256"});
  const auto& keyspace = kTableName.namespace_name();
  const auto& table_name = kTableName.table_name();

  // Insert some rows.
  TestYcqlWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_timeout_allowed(true);
  workload.Setup();
  workload.Start();
  workload.WaitInserted(100);
  workload.StopAndJoin();
  LOG(INFO) << "Number of rows inserted: " << workload.rows_inserted();

  // Flush to SST. Size-based split (FLAGS_use_cross_split_key_detection_algorithm) needs SST data.
  ASSERT_OK(CallAdmin("flush_table", keyspace, table_name));

  // Verify multi-way split is disallowed when gFlag is disabled.
  constexpr int kSplitFactor = 5;
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_enable_multi_way_tablet_split", "false"));
  ASSERT_NOK_STR_CONTAINS(
      CallAdmin("split_tablet", tablet_id_, kSplitFactor),
      "Split factor must be 2");

  // Verify multi-way split is allowed when gFlag is enabled.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_enable_multi_way_tablet_split", "true"));
  ASSERT_OK(CallAdmin("split_tablet", tablet_id_, kSplitFactor));

  // Verify the split creates 5 child tablets.
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return kSplitFactor == VERIFY_RESULT(GetListTabletsCount(keyspace, table_name));
      }, 30s, "Wait for tablet split to complete"));
}

TEST_F(AdminCliTest, GetAutoFlagsConfig) {
  BuildAndStart();
  auto message = ASSERT_RESULT(CallAdmin("get_auto_flags_config"));
  ASSERT_STR_CONTAINS(message, R"#(AutoFlags config:
config_version: 1
promoted_flags {)#");
}

TEST_F(AdminCliTest, PromoteAutoFlags) {
  BuildAndStart();
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());

  ASSERT_NOK_STR_CONTAINS(
      CallAdmin(kPromoteAutoFlagsCmd, "invalid"), "Invalid value provided for max_flags_class");
  ASSERT_NOK_STR_CONTAINS(
      CallAdmin(kPromoteAutoFlagsCmd, "kExternal", "invalid"), "Invalid arguments for operation");
  ASSERT_NOK_STR_CONTAINS(
      CallAdmin(kPromoteAutoFlagsCmd, "kExternal", "force", "invalid"),
      "Invalid arguments for operation");

  ASSERT_OK(CallAdmin(kPromoteAutoFlagsCmd, "kLocalVolatile"));
  ASSERT_OK(CallAdmin(kPromoteAutoFlagsCmd, "kLocalPersisted"));
  ASSERT_OK(CallAdmin(kPromoteAutoFlagsCmd, "kExternal"));

  auto result = ASSERT_RESULT(CallAdmin(kPromoteAutoFlagsCmd, "kLocalVolatile"));
  ASSERT_STR_CONTAINS(
      result,
      "PromoteAutoFlags completed successfully\n"
      "No new AutoFlags eligible to promote\n"
      "Current config version: 1");

  result = ASSERT_RESULT(CallAdmin(kPromoteAutoFlagsCmd, "kLocalVolatile", "force"));
  ASSERT_STR_CONTAINS(
      result,
      "PromoteAutoFlags completed successfully\n"
      "New AutoFlags were promoted\n"
      "New config version: 2");
}

TEST_F(AdminCliTest, RollbackAutoFlags) {
  BuildAndStart(
      /* ts_flags = */ {}, /* master_flags = */ {"--limit_auto_flag_promote_for_new_universe=0"});
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());

  auto status = CallAdmin(kRollbackAutoFlagsCmd);
  ASSERT_NOK(status);
  ASSERT_NE(status.ToString().find("Invalid arguments for operation"), std::string::npos);

  status = CallAdmin(kRollbackAutoFlagsCmd, "invalid");
  ASSERT_NOK(status);
  ASSERT_NE(status.ToString().find("invalid is not a valid number"), std::string::npos);

  status = CallAdmin(kRollbackAutoFlagsCmd, std::numeric_limits<int64_t>::max());
  ASSERT_NOK(status);
  ASSERT_STR_CONTAINS(status.ToString(), "rollback_version exceeds bounds");

  auto result = ASSERT_RESULT(CallAdmin(kRollbackAutoFlagsCmd, 0));
  ASSERT_STR_CONTAINS(
      result,
      "RollbackAutoFlags completed successfully\n"
      "No AutoFlags have been promoted since version 0\n"
      "Current config version: 0");

  result = ASSERT_RESULT(CallAdmin(kPromoteAutoFlagsCmd, "kLocalVolatile"));
  ASSERT_STR_CONTAINS(
      result,
      "New AutoFlags were promoted\n"
      "New config version:");

  result = ASSERT_RESULT(CallAdmin(kRollbackAutoFlagsCmd, 0));
  ASSERT_STR_CONTAINS(
      result,
      "RollbackAutoFlags completed successfully\n"
      "AutoFlags that were promoted after config version 0 were successfully rolled back\n"
      "New config version: 2");

  result = ASSERT_RESULT(CallAdmin(kRollbackAutoFlagsCmd, 0));
  ASSERT_STR_CONTAINS(
      result,
      "RollbackAutoFlags completed successfully\n"
      "No AutoFlags have been promoted since version 0\n"
      "Current config version: 2");

  result = ASSERT_RESULT(CallAdmin(kPromoteAutoFlagsCmd));
  ASSERT_STR_CONTAINS(
      result,
      "PromoteAutoFlags completed successfully\n"
      "New AutoFlags were promoted\n"
      "New config version: 3");

  // Rollback external AutoFlags should fail.
  status = CallAdmin(kRollbackAutoFlagsCmd, 0);
  ASSERT_NOK(status);
  ASSERT_STR_CONTAINS(status.ToString(), "is not eligible for rollback");
}

TEST_F(AdminCliTest, PromoteDemoteSingleAutoFlag) {
  BuildAndStart();
  const auto kPromoteSingleFlaCmd = "promote_single_auto_flag";
  const auto kDemoteSingleFlagCmd = "demote_single_auto_flag";

  // Promote a single AutoFlag
  auto result = CallAdmin(kPromoteSingleFlaCmd, "NAProcess", "NAFlag");
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.ToString(), "Process NAProcess not found");

  result = CallAdmin(kPromoteSingleFlaCmd, "yb-master", "NAFlag");
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.ToString(), "AutoFlag NAFlag not found in process yb-master");

  result = CallAdmin(kPromoteSingleFlaCmd, "yb-master", "TEST_auto_flags_initialized");
  ASSERT_OK(result);
  ASSERT_STR_CONTAINS(
      result.ToString(),
      "Failed to promote AutoFlag TEST_auto_flags_initialized from process yb-master. Check the "
      "logs for more information\n"
      "Current config version: 1");

  // Demote a single AutoFlag
  result = CallAdmin(kDemoteSingleFlagCmd, "NAProcess", "NAFlag", "force");
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.ToString(), "Process NAProcess not found");

  result = CallAdmin(kDemoteSingleFlagCmd, "yb-master", "NAFlag", "force");
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.ToString(), "AutoFlag NAFlag not found in process yb-master");

  result = CallAdmin(kDemoteSingleFlagCmd, "yb-master", "TEST_auto_flags_initialized", "force");
  ASSERT_OK(result);
  ASSERT_STR_CONTAINS(
      result.ToString(),
      "AutoFlag TEST_auto_flags_initialized from process yb-master was successfully demoted\n"
      "New config version: 2");

  result = CallAdmin(kDemoteSingleFlagCmd, "yb-master", "TEST_auto_flags_initialized", "force");
  ASSERT_OK(result);
  ASSERT_STR_CONTAINS(
      result.ToString(),
      "Unable to demote AutoFlag TEST_auto_flags_initialized from process yb-master because the "
      "flag is not in promoted state\n"
      "Current config version: 2");

  result = CallAdmin(kPromoteSingleFlaCmd, "yb-master", "TEST_auto_flags_initialized");
  ASSERT_OK(result);
  ASSERT_STR_CONTAINS(
      result.ToString(),
      "AutoFlag TEST_auto_flags_initialized from process yb-master was successfully promoted\n"
      "New config version: 3");
}

TEST_F(AdminCliTest, TestListNamespaces) {
  BuildAndStart();
  ASSERT_OK(client_->CreateNamespaceIfNotExists("a_user_namespace"));
  auto status = CallAdmin("list_namespaces");
  ASSERT_OK(status);

  std::string output = status.ToString();

  ASSERT_STR_CONTAINS(output, "User Namespaces");
  auto user_namespaces_pos = output.find("User Namespaces");
  ASSERT_STR_CONTAINS(output, "System Namespaces");
  auto system_namespaces_pos = output.find("System Namespaces");

  std::regex system_namespace_regex("system .* ycql [a-zA-Z_]+ [a-zA-Z_]+");
  std::smatch system_namespace_match;
  std::regex_search(output, system_namespace_match, system_namespace_regex);
  ASSERT_FALSE(system_namespace_match.empty());
  // We expect the "system" keyspace to be under "System Keyspaces".
  ASSERT_GT(system_namespace_match.position(0), system_namespaces_pos);

  std::smatch user_namespace_match;
  std::regex user_namespace_regex("a_user_namespace");
  std::regex_search(output, user_namespace_match, user_namespace_regex);
  ASSERT_FALSE(user_namespace_match.empty());
  /* Because we compare their character positions, we expect "User Namespaces:" to be outputted
  before "System Namespaces:" to simplify testing of whether "a_user_namespace" is under
  "User Namespaces:" and not "System Namespaces:". */
  ASSERT_LT(user_namespaces_pos, system_namespaces_pos);
  ASSERT_GT(user_namespace_match.position(0), user_namespaces_pos);
  ASSERT_LT(user_namespace_match.position(0), system_namespaces_pos);

  // We test just YSQL namespaces here because YCQL namespaces get directly deleted without being
  // in the DELETED state and thus there is nothing for include_nonrunning to see in that case.
  ASSERT_OK(client_->CreateNamespace("new_user_namespace", YQL_DATABASE_PGSQL));

  ASSERT_OK(CallAdmin("delete_namespace", "ysql.new_user_namespace"));

  status = CallAdmin("list_namespaces");
  ASSERT_OK(status);
  ASSERT_STR_NOT_CONTAINS(status.ToString(), "new_user_namespace");

  status = CallAdmin("list_namespaces", "include_nonrunning");
  ASSERT_OK(status);
  ASSERT_TRUE(std::regex_search(status.ToString(), std::regex("new_user_namespace.*DELETED")));
}

TEST_F(AdminCliTest, PrintArgumentExpressions) {
  const auto namespace_expression =
      "<namespace>\n  [(ycql|ysql).]<namespace_name> (default: ycql.)";
  const auto table_expression = "<table>\n  <namespace> <table_name> | tableid.<table_id>";
  const auto index_expression = "<index>\n  <namespace> <index_name> | tableid.<index_id>";

  BuildAndStart();
  auto status = CallAdmin("delete_table");
  ASSERT_NOK(status);
  ASSERT_NE(status.ToString().find(table_expression), std::string::npos);
  ASSERT_NE(status.ToString().find(namespace_expression), std::string::npos);
  ASSERT_LT(status.ToString().find(table_expression), status.ToString().find(namespace_expression));

  status = CallAdmin("delete_namespace");
  ASSERT_NOK(status);
  ASSERT_NE(status.ToString().find(namespace_expression), std::string::npos);

  status = CallAdmin("delete_index");
  ASSERT_NOK(status);
  ASSERT_NE(status.ToString().find(index_expression), std::string::npos);
  ASSERT_NE(status.ToString().find(namespace_expression), std::string::npos);

  status = CallAdmin("add_universe_key_to_all_masters");
  ASSERT_NOK(status);
  ASSERT_EQ(status.ToString().find(namespace_expression), std::string::npos);
  ASSERT_EQ(status.ToString().find(table_expression), std::string::npos);
  ASSERT_EQ(status.ToString().find(index_expression), std::string::npos);

  // Placeholders inside [] are defined too.
  status = CallAdmin("import_snapshot");
  ASSERT_NOK(status);
  ASSERT_NE(status.ToString().find(namespace_expression), std::string::npos);

  status = CallAdmin("list_change_data_streams", "extra_arg_1", "extra_arg_2");
  ASSERT_NOK(status);
  ASSERT_NE(status.ToString().find(namespace_expression), std::string::npos);
}

TEST_F(AdminCliTest, TestCompactionStatusBeforeCompaction) {
  BuildAndStart();
  const string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());

  ASSERT_OK(WaitFor(
      [this]() -> Result<bool> {
        const string output = VERIFY_RESULT(
            CallAdmin("compaction_status", kTableName.namespace_name(), kTableName.table_name()));

        std::smatch match;
        const std::regex regex(
            "No full compaction taking place\n"
            "A full compaction has never been completed\n"
            "An admin compaction has never been requested");
        std::regex_search(output, match, regex);
        return !match.empty();
      },
      30s, "Wait for initial metrics heartbeats to report full compaction statuses"));
}

TEST_F(AdminCliTest, TestCompactionStatusAfterCompactionFinishes) {
  BuildAndStart();
  const string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());

  const auto time_before_compaction = DateTime::TimestampNow();
  ASSERT_OK(CallAdmin("compact_table", kTableName.namespace_name(), kTableName.table_name()));

  string output;
  std::smatch match;
  ASSERT_OK(WaitFor(
      [&]() {
        const auto result =
            CallAdmin("compaction_status", kTableName.namespace_name(), kTableName.table_name());
        if (!result.ok()) {
          return false;
        }
        output = *result;
        const std::regex regex("No full compaction taking place");
        std::regex_search(output, match, regex);
        return !match.empty();
      },
      30s /* timeout */, "Wait for compaction status to report no compaction"));

  const std::regex regex(
      "Last full compaction completion time: (.+)\nLast admin compaction request time: (.+)");
  std::regex_search(output, match, regex);
  ASSERT_FALSE(match.empty());
  const auto last_full_compaction_time =
      ASSERT_RESULT(DateTime::TimestampFromString(match[1].str()));
  ASSERT_GT(last_full_compaction_time, time_before_compaction);
  const auto last_request_time = ASSERT_RESULT(DateTime::TimestampFromString(match[2].str()));
  ASSERT_GT(last_request_time, time_before_compaction);
}

TEST_F(AdminCliTest, TestCompactTableOutputFormat) {
  BuildAndStart();
  const string output = ASSERT_RESULT(CallAdmin(
      "compact_table", kTableName.namespace_name(), kTableName.table_name(), "30" /* timeout */));
  const string expected_table_name =
      Format("$0.$1", kTableName.namespace_name(), kTableName.table_name());
  const std::regex regex(Format(R"(Compacted \[$0.*\] tables\.)", expected_table_name));
  ASSERT_TRUE(std::regex_search(output, regex)) << "Unexpected output format: " << output;
}

TEST_F(AdminCliTest, TestCompactTableWithMissingTable) {
  BuildAndStart();
  const auto result = CallAdmin("compact_table");
  ASSERT_FALSE(result.ok());
  ASSERT_STR_CONTAINS(result.status().ToString(), "Empty list of tables");
  ASSERT_STR_CONTAINS(result.status().ToString(), "Usage:");
}

TEST_F(AdminCliTest, TestCompactTableWithInvalidTable) {
  BuildAndStart();
  const auto result = CallAdmin(
      "compact_table", "ycql.nonexistent_namespace", "nonexistent_table");
  ASSERT_FALSE(result.ok());
  ASSERT_STR_CONTAINS(result.status().ToString(), "not found");
}

TEST_F(AdminCliTest, TestCompactTableById) {
  BuildAndStart();
  const string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());

  auto tables = ASSERT_RESULT(client->ListTables(/* filter */ kTableName.table_name(),
                                                  /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());
  const auto table_id = tables.front().table_id();

  const string output = ASSERT_RESULT(
      CallAdmin("compact_table_by_id", table_id, /* timeout */ "30"));
  const string expected_table_name =
      Format("$0.$1", kTableName.namespace_name(), kTableName.table_name());
  const std::regex regex(Format(R"(Compacted \[$0.*\] tables\.)", expected_table_name));
  ASSERT_TRUE(std::regex_search(output, regex)) << "Unexpected output format: " << output;
}

TEST_F(AdminCliTest, TestCompactTableByIdWithMissingTableId) {
  BuildAndStart();
  const auto result = CallAdmin("compact_table_by_id");
  ASSERT_FALSE(result.ok());
  ASSERT_STR_CONTAINS(result.status().ToString(), "Usage:");
}

TEST_F(AdminCliTest, TestCompactTableByIdWithInvalidTableId) {
  BuildAndStart();
  const auto result = CallAdmin(
      "compact_table_by_id", "nonexistent_table_id", /* timeout */ "30");
  ASSERT_FALSE(result.ok());
  ASSERT_STR_CONTAINS(result.status().ToString(), "not found");
}

// Covers https://github.com/yugabyte/yugabyte-db/issues/19957
TEST_F(AdminCliTest, TestAdminCommandTimeout) {
  // The test checks that an admin command timeout for compactions and flushes is honored by
  // FlushManager on yb-master side (no dependency on `master_ts_rpc_timeout_ms`).

  BuildAndStart();
  const string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());

  // No admin timeout higher than `master_ts_rpc_timeout_ms` was honored for flush and compaction
  // operation (passed via FlushManager) before the fix, which means `master_ts_rpc_timeout_ms`
  // was the max timeout available for the mentioned operations. It is definitely not enough at
  // least for large compactions.
  // In case of timeout by admin command timeout value, the admin tool gets the error:
  // (1) `Timed out waiting for FlushTables`.
  // In case of timeout by `master_ts_rpc_timeout_ms` value, the admin tool gets the error like:
  // (2) `Flush operation for id: 9be5b8cbd0d14cfea3371da1a6cbccab failed`.
  // With the fix, only the error (1) is thrown for timied out commands.
  constexpr size_t kMasterRpcTimeout = 4000;
  constexpr size_t kCompactionPause  = 2 * kMasterRpcTimeout;
  constexpr size_t kAdminCmdTimeout  =
      kMasterRpcTimeout + ((kCompactionPause - kMasterRpcTimeout) / 2);

  // Update flags.
  ASSERT_OK(cluster_->SetFlagOnMasters(
    "refresh_waiter_timeout_ms", std::to_string(kMasterRpcTimeout)));
  ASSERT_OK(cluster_->SetFlagOnMasters(
      "master_ts_rpc_timeout_ms", std::to_string(kMasterRpcTimeout)));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "TEST_pause_tablet_compact_flush_ms", std::to_string(kCompactionPause)));
  SleepFor(MonoDelta::FromSeconds(1));

  const auto before_ts = DateTime::TimestampNow();
  auto result = CallAdmin(
      "compact_table", kTableName.namespace_name(), kTableName.table_name(),
      std::to_string(kAdminCmdTimeout / MonoTime::kMillisecondsPerSecond));
  auto duration = MonoDelta::FromMicroseconds(DateTime::TimestampNow().value() - before_ts.value());
  LOG(INFO) << "Result: " << result << ", dutaion: " << duration;

  // It is expected to always get the timeout error since kAdminCmdTimeout < kCompactionPause,
  // additionaly it covers the cases kAdminCmdTimeout > kMasterRpcTimeout for the expected error.
  bool timed_out = !result.ok() &&
                    result.status().message().Contains("Timed out waiting for FlushTables");
  ASSERT_TRUE(timed_out);

  // The expectation is to have the call's duration approximately kAdminCmdTimeout amount.
  ASSERT_GT(duration, MonoDelta::FromMilliseconds(kAdminCmdTimeout));
  ASSERT_LT(duration, MonoDelta::FromMilliseconds(kAdminCmdTimeout * 1.25));

  // Wait for some time to let the compaction be unpaused and completed.
  SleepFor(MonoDelta::FromMilliseconds(1.1 * (kCompactionPause - kAdminCmdTimeout)));
}

// Covers https://github.com/yugabyte/yugabyte-db/issues/26722
TEST_F(AdminCliTest, TestAdminRpcTimeout) {
  // The test checks that an admin command timeout is honored when it is greater than admin's
  // default RPC timeout, which is set by FLAGS_yb_client_admin_rpc_timeout_sec.

  BuildAndStart();
  const string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());

  // With the fix for https://github.com/yugabyte/yugabyte-db/issues/19957, no admin command
  // timeout higher than 60 seconds was honored for any admin RPC operation (refer to
  // YBClientBuilder::Data::default_rpc_timeout_). Which means any admin command was limited
  // by the mentioned 60 seconds even if a user was specified the higher timeout.
  // In case of timeout by admin command timeout value, the admin tool gets the error:
  // (1) `Timed out waiting for FlushTables`.
  // In case of timeout by default_rpc_timeout_ value, the admin tool gets the error like:
  // (2) `Flush operation for id: 9be5b8cbd0d14cfea3371da1a6cbccab failed`.
  // After the fix, only the error (1) is thrown for timed out commands.
  constexpr size_t kAdminRpcTimeout  = 4000;
  constexpr size_t kAdminCmdTimeout  = 2 * kAdminRpcTimeout;
  constexpr size_t kCompactionPause  = 1.5 * kAdminCmdTimeout;
  constexpr size_t kMasterRpcTimeout = kAdminRpcTimeout / 2;

  // Update flags.
  ASSERT_OK(cluster_->SetFlagOnMasters(
    "refresh_waiter_timeout_ms", std::to_string(kMasterRpcTimeout)));
  ASSERT_OK(cluster_->SetFlagOnMasters(
      "master_ts_rpc_timeout_ms", std::to_string(kMasterRpcTimeout)));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "TEST_pause_tablet_compact_flush_ms", std::to_string(kCompactionPause)));
  SleepFor(MonoDelta::FromSeconds(1));

  const auto before_ts = DateTime::TimestampNow();
  auto result = CallAdmin(
      "--yb_client_admin_rpc_timeout_sec",
      std::to_string(kAdminRpcTimeout / MonoTime::kMillisecondsPerSecond),
      "compact_table", kTableName.namespace_name(), kTableName.table_name(),
      std::to_string(kAdminCmdTimeout / MonoTime::kMillisecondsPerSecond));
  auto duration = MonoDelta::FromMicroseconds(DateTime::TimestampNow().value() - before_ts.value());
  LOG(INFO) << "Result: " << result;

  // It is expected to always get the timeout error since kAdminCmdTimeout < kCompactionPause.
  bool timed_out = !result.ok() &&
                    result.status().message().Contains("Timed out waiting for FlushTables");
  ASSERT_TRUE(timed_out);

  // The expectation is to have the call's duration approximately kAdminCmdTimeout amount.
  ASSERT_GT(duration, MonoDelta::FromMilliseconds(kAdminCmdTimeout));
  ASSERT_LT(duration, MonoDelta::FromMilliseconds(kAdminCmdTimeout * 1.25));

  // Wait for some time to let the compaction be unpaused and completed.
  SleepFor(MonoDelta::FromMilliseconds(1.1 * (kCompactionPause - kAdminCmdTimeout)));
}

Status AdminCliTest::TestDumpSysCatalogEntry(const std::string& cluster_uuid) {
  const auto dump_file_path = tmp_dir_ / Format("$0-", kClusterConfigEntryTypeName);

  // We should be able to dump the data while not in emergency_repair_mode.
  auto output =
      VERIFY_RESULT(CallAdmin("dump_sys_catalog_entries", kClusterConfigEntryTypeName, *tmp_dir_));
  SCHECK_STR_CONTAINS(output, dump_file_path);

  auto file_contents = VERIFY_RESULT(ReadFileToString(dump_file_path));
  SCHECK_STR_CONTAINS(file_contents, cluster_uuid);
  return Status::OK();
}

TEST_F(AdminCliTest, TestDumpSysCatalogEntryInNonEmergencyMode) {
  BuildAndStart();
  const auto cluster_uuid = ASSERT_RESULT(GetClusterUuid());
  ASSERT_OK(TestDumpSysCatalogEntry(cluster_uuid));
}

TEST_F(AdminCliTest, TestDumpSysCatalogEntryInEmergencyMode) {
  BuildAndStart();
  const auto cluster_uuid = ASSERT_RESULT(GetClusterUuid());
  ASSERT_OK(RestartMaster(EmergencyRepairMode::kTrue));
  ASSERT_OK(TestDumpSysCatalogEntry(cluster_uuid));
  ASSERT_OK(RestartMaster(EmergencyRepairMode::kFalse));
}

// Make sure we cannot write to sys catalog when not in emergency repair mode.
TEST_F(AdminCliTest, BlockWriteToSysCatalogEntryInNonEmergencyMode) {
  BuildAndStart();

  auto env = Env::Default();
  const auto dump_file_path = tmp_dir_ / Format("$0-", kClusterConfigEntryTypeName);

  master::SysClusterConfigEntryPB dummy_cluster_config;
  dummy_cluster_config.set_cluster_uuid(Uuid::Generate().ToString());

  ASSERT_OK(WriteStringToFileSync(env, dummy_cluster_config.DebugString(), dump_file_path));

  const auto kExpectedError = "Updating sys_catalog is only allowed in emergency repair mode";

  ASSERT_NOK_STR_CONTAINS(
      CallAdmin("write_sys_catalog_entry", "delete", kClusterConfigEntryTypeName, "", "", "force"),
      kExpectedError);

  ASSERT_NOK_STR_CONTAINS(
      CallAdmin(
          "write_sys_catalog_entry", "insert", kClusterConfigEntryTypeName, "dummy", dump_file_path,
          "force"),
      kExpectedError);

  ASSERT_NOK_STR_CONTAINS(
      CallAdmin(
          "write_sys_catalog_entry", "update", kClusterConfigEntryTypeName, "", dump_file_path,
          "force"),
      kExpectedError);
}

// Test insert and delete of CatalogEntity.
TEST_F(AdminCliTest, TestInsertDeleteSysCatalogEntry) {
  BuildAndStart();
  ASSERT_OK(RestartMaster(EmergencyRepairMode::kTrue));

  auto env = Env::Default();
  const auto second_cluster_config_id = "second_cc";

  master::SysClusterConfigEntryPB second_cluster_config;
  second_cluster_config.set_cluster_uuid(Uuid::Generate().ToString());
  const auto file_path =
      tmp_dir_ / Format("$0-$1", kClusterConfigEntryTypeName, second_cluster_config_id);

  ASSERT_OK(WriteStringToFileSync(env, second_cluster_config.DebugString(), file_path));

  ASSERT_OK(CallAdmin(
      "write_sys_catalog_entry", "insert", kClusterConfigEntryTypeName, second_cluster_config_id,
      file_path, "force"));

  auto validate_dump_cluster_config = [&]() -> Status {
    RETURN_NOT_OK(env->DeleteFile(file_path));
    auto output = VERIFY_RESULT(CallAdmin(
        "dump_sys_catalog_entries", kClusterConfigEntryTypeName, *tmp_dir_,
        second_cluster_config_id));
    SCHECK_STR_CONTAINS(output, file_path);
    auto file_contents = VERIFY_RESULT(ReadFileToString(file_path));
    SCHECK_STR_CONTAINS(file_contents, second_cluster_config.cluster_uuid());
    return Status::OK();
  };

  // Dump the new entry to make sure it was updated.
  ASSERT_OK(validate_dump_cluster_config());

  // Restart the master and dump the new entry to make sure it was persisted.
  auto* leader_master = cluster_->GetLeaderMaster();
  leader_master->Shutdown();
  ASSERT_OK(leader_master->Restart());

  ASSERT_OK(validate_dump_cluster_config());

  // Delete the second entry.
  ASSERT_OK(CallAdmin(
      "write_sys_catalog_entry", "delete", kClusterConfigEntryTypeName, second_cluster_config_id,
      "", "force"));

  auto output = ASSERT_RESULT(CallAdmin(
      "dump_sys_catalog_entries", kClusterConfigEntryTypeName, *tmp_dir_,
      second_cluster_config_id));
  ASSERT_STR_CONTAINS(output, "Found 0 entries of type CLUSTER_CONFIG");

  // Restart the master to make sure it starts correctly. If the delete did not succeed the master
  // would crash and fail to start.
  ASSERT_OK(RestartMaster(EmergencyRepairMode::kFalse));
  ASSERT_OK(cluster_->WaitForTabletServerCount(cluster_->num_tablet_servers(), 30s));
}

// Test insert and delete of TableEntry.
TEST_F(AdminCliTest, TestInsertDeleteTableEntry) {
  const auto new_table_name = "new_table";
  const auto new_table_id = Uuid::Generate().ToString();
  auto env = Env::Default();

  BuildAndStart();
  const auto tables =
      ASSERT_RESULT(client_->ListTables(kTableName.table_name(), /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());
  const auto& table_id = tables.front().table_id();

  ASSERT_OK(RestartMaster(EmergencyRepairMode::kTrue));

  const auto folder_path = *tmp_dir_;
  const auto existing_table_file_path = tmp_dir_ / Format("$0-$1", kTableEntryTypeName, table_id);
  const auto new_table_file_path = tmp_dir_ / Format("$0-$1", kTableEntryTypeName, new_table_id);
  auto output = ASSERT_RESULT(
      CallAdmin("dump_sys_catalog_entries", kTableEntryTypeName, folder_path, table_id));
  ASSERT_STR_CONTAINS(output, existing_table_file_path);
  auto file_contents = ASSERT_RESULT(ReadFileToString(existing_table_file_path));
  boost::replace_all(file_contents, table_id, new_table_id);
  boost::replace_all(file_contents, kTableName.table_name(), new_table_name);
  ASSERT_OK(WriteStringToFileSync(env, file_contents, new_table_file_path));

  ASSERT_OK(CallAdmin(
      "write_sys_catalog_entry", "insert", kTableEntryTypeName, new_table_id, new_table_file_path,
      "force"));

  auto validate_dump_cluster_config = [&]() -> Status {
    RETURN_NOT_OK(env->DeleteFile(new_table_file_path));
    auto output = VERIFY_RESULT(
        CallAdmin("dump_sys_catalog_entries", kTableEntryTypeName, folder_path, new_table_id));
    SCHECK_STR_CONTAINS(output, new_table_file_path);
    auto file_contents = VERIFY_RESULT(ReadFileToString(new_table_file_path));
    SCHECK_STR_CONTAINS(file_contents, new_table_name);
    return Status::OK();
  };

  // Dump the new entry to make sure it was inserted.
  ASSERT_OK(validate_dump_cluster_config());

  // Restart the master and dump the new entry to make sure it was persisted.
  auto* leader_master = cluster_->GetLeaderMaster();
  leader_master->Shutdown();
  ASSERT_OK(leader_master->Restart());

  ASSERT_OK(validate_dump_cluster_config());

  // Delete the new table.
  ASSERT_OK(CallAdmin(
      "write_sys_catalog_entry", "delete", kTableEntryTypeName, new_table_id, "", "force"));

  output = ASSERT_RESULT(
      CallAdmin("dump_sys_catalog_entries", kTableEntryTypeName, *tmp_dir_, new_table_id));
  ASSERT_STR_CONTAINS(output, "Found 0 entries of type TABLE");
}

// Update the ClusterConfig entry in the sys catalog and verify that the change is persisted across
// master restarts.
TEST_F(AdminCliTest, TestUpdateSysCatalogEntry) {
  BuildAndStart();
  const auto old_cluster_uuid = ASSERT_RESULT(GetClusterUuid());
  ASSERT_OK(RestartMaster(EmergencyRepairMode::kTrue));

  auto env = Env::Default();
  const auto file_path = tmp_dir_ / Format("$0-", kClusterConfigEntryTypeName);

  auto new_cluster_uuid = Uuid::Generate().ToString();
  LOG(INFO) << "Replacing cluster_uuid " << old_cluster_uuid << " with " << new_cluster_uuid;

  master::SysClusterConfigEntryPB new_cluster_config;
  new_cluster_config.set_cluster_uuid(new_cluster_uuid);
  ASSERT_OK(WriteStringToFileSync(env, new_cluster_config.DebugString(), file_path));

  auto output = ASSERT_RESULT(CallAdmin(
      "write_sys_catalog_entry", "update", kClusterConfigEntryTypeName, "", file_path, "force"));
  ASSERT_STR_CONTAINS(output, new_cluster_uuid);

  ASSERT_OK(RestartMaster(EmergencyRepairMode::kFalse));

  const auto cluster_uuid = ASSERT_RESULT(GetClusterUuid());
  ASSERT_EQ(cluster_uuid, new_cluster_uuid);

  ASSERT_OK(env->DeleteFile(file_path));
  output =
      ASSERT_RESULT(CallAdmin("dump_sys_catalog_entries", kClusterConfigEntryTypeName, *tmp_dir_));
  ASSERT_STR_CONTAINS(output, file_path);
  auto file_contents = ASSERT_RESULT(ReadFileToString(file_path));
  ASSERT_STR_NOT_CONTAINS(file_contents, old_cluster_uuid);
  ASSERT_STR_CONTAINS(file_contents, new_cluster_uuid);
}

TEST_F(AdminCliTest, TestRemoveTabletServer) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 1;
  // remove_tablet_server requires the TServer to have definitely lost its xCluster-guarded
  // information lease.  Keep the lease (plus clock-skew slack) shorter than
  // tserver_unresponsive_timeout_ms so that holds as soon as the TServer is marked unresponsive.
  BuildAndStart(
      {}, {"--enable_load_balancing=false", "--tserver_unresponsive_timeout_ms=5000",
           "--xcluster_guarded_lease_duration_ms=3000"});
  ASSERT_OK(cluster_->AddTabletServer(true));
  auto added_tserver = cluster_->tablet_server(cluster_->num_tablet_servers() - 1);
  ASSERT_OK(cluster_->AddTServerToBlacklist(cluster_->master(), added_tserver));
  added_tserver->Shutdown();
  auto cluster_client =
      master::MasterClusterClient(cluster_->GetLeaderMasterProxy<master::MasterClusterProxy>());
  ASSERT_OK(WaitFor(
      [&cluster_client, added_tserver]() -> Result<bool> {
        auto tserver = VERIFY_RESULT(cluster_client.GetTabletServer(added_tserver->uuid()));
        return tserver && !tserver->alive();
      },
      30s, "tserver not present or still alive"));
  ASSERT_RESULT(CallAdmin("remove_tablet_server", added_tserver->uuid()));
  auto find_tserver_result = ASSERT_RESULT(cluster_client.GetTabletServer(added_tserver->uuid()));
  EXPECT_EQ(find_tserver_result, std::nullopt);
}

// Regression test for https://github.com/yugabyte/yugabyte-db/issues/32681:
// list_tablet_server_log_locations must not abort on DEAD tservers, and must
// sort alive tservers ahead of dead ones.
TEST_F(AdminCliTest, TestListTabletServerLogLocationsWithDeadTServer) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_tablet_servers) = 2;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_num_replicas) = 1;
  BuildAndStart({}, {"--enable_load_balancing=false", "--tserver_unresponsive_timeout_ms=5000"});

  // Kill tserver 0 (the one that sorts first by host/port) and keep tserver 1
  // alive, so alive-first ordering must actively reorder the two entries rather
  // than coincidentally matching the host/port tie-break.
  auto* dead_ts = cluster_->tablet_server(0);
  auto* alive_ts = cluster_->tablet_server(1);
  const auto dead_uuid = dead_ts->uuid();
  const auto alive_uuid = alive_ts->uuid();

  // Helper: extract the LogLocation column for a given tserver UUID.
  auto log_location_for = [](const std::string& output, const std::string& uuid) -> std::string {
    std::smatch match;
    if (!std::regex_search(output, match, std::regex(uuid + R"(\s+\S+\s+(\S+))"))) {
      return "";
    }
    return match[1].str();
  };

  // While both are alive, both report a real log directory (no N/A).
  auto output_alive = ASSERT_RESULT(CallAdmin("list_tablet_server_log_locations"));
  ASSERT_STR_CONTAINS(output_alive, dead_uuid);
  ASSERT_STR_CONTAINS(output_alive, alive_uuid);
  ASSERT_STR_NOT_CONTAINS(output_alive, "N/A");

  dead_ts->Shutdown();
  auto cluster_client =
      master::MasterClusterClient(cluster_->GetLeaderMasterProxy<master::MasterClusterProxy>());
  ASSERT_OK(WaitFor(
      [&cluster_client, &dead_uuid]() -> Result<bool> {
        auto tserver = VERIFY_RESULT(cluster_client.GetTabletServer(dead_uuid));
        return tserver && !tserver->alive();
      },
      30s, "tserver not marked dead"));

  // Must succeed (not connect-timeout abort) and list both tservers.
  auto output = ASSERT_RESULT(CallAdmin("list_tablet_server_log_locations"));
  ASSERT_STR_CONTAINS(output, dead_uuid);
  ASSERT_STR_CONTAINS(output, alive_uuid);

  // Alive tserver reports a real log location; dead tserver reports N/A.
  ASSERT_NE(log_location_for(output, alive_uuid), "N/A");
  ASSERT_NE(log_location_for(output, alive_uuid), "");
  ASSERT_EQ(log_location_for(output, dead_uuid), "N/A");

  // Alive tserver sorts ahead of the dead one.
  const auto alive_pos = output.find(alive_uuid);
  const auto dead_pos = output.find(dead_uuid);
  ASSERT_NE(alive_pos, std::string::npos);
  ASSERT_NE(dead_pos, std::string::npos);
  ASSERT_LT(alive_pos, dead_pos);
}

TEST_F(AdminCliTest, TestDisallowImplicitStreamCreation) {
  std::string test_namespace = "pg_namespace_cdc";
  BuildAndStart();
  ASSERT_OK(client_->CreateNamespace(test_namespace, YQL_DATABASE_PGSQL));

  ASSERT_NOK(CallAdmin("create_change_data_stream", "ysql." + test_namespace, "IMPLICIT"));
}

TEST_F(AdminCliTest, TestAllowImplicitStreamCreationWhenFlagEnabled) {
  std::string test_namespace = "pg_namespace_cdc";
  BuildAndStart(
      {"--cdc_enable_implicit_checkpointing=true"}, {});
  ASSERT_OK(client_->CreateNamespace(test_namespace, YQL_DATABASE_PGSQL));

  auto output =
      ASSERT_RESULT(CallAdmin("create_change_data_stream", "ysql." + test_namespace, "IMPLICIT"));

  ASSERT_STR_CONTAINS(
      output, "Creation of streams with IMPLICIT checkpointing is deprecated");
}

// Test yb-admin get_table_hash command for YCQL table.
TEST_F(AdminCliTest, TestGetTableXorHash) {
  BuildAndStart();
  const auto table_name =
      YBTableName(YQLDatabase::YQL_DATABASE_CQL, kTableName.namespace_name(), "my_table");

  client::TableHandle table;
  const auto num_tablets = 4;
  ASSERT_OK(table.Create(table_name, num_tablets, client::YBSchema(schema_), client_.get()));

  TestYcqlWorkload workload(cluster_.get());
  workload.set_table_name(table_name);
  workload.Setup();
  workload.Start();
  workload.WaitInserted(100);
  auto ht1 = ASSERT_RESULT(cluster_->master()->GetServerTime());
  // Insert 100 more rows.
  workload.WaitInserted(workload.rows_inserted() + 100);
  workload.StopAndJoin();

  auto rows_inserted = boost::size(client::TableRange(table));
  ASSERT_GE(rows_inserted, 200);

  auto tables = ASSERT_RESULT(client_->ListTables(table_name.table_name()));
  ASSERT_EQ(1, tables.size());
  const auto table_id = tables.front().table_id();

  auto extract_from_output = [&](const std::string& output) -> std::pair<uint64_t, uint64_t> {
    LOG(INFO) << "Command output: " << output;
    // Count the number of line with `Tablet ID:`
    // Read HT: { days: 20466 time: 15:34:35.245376 }
    // Tablet ID: 9bd7b975e43e4f41aa41d8bd9c2f8fb0
    // Row count: 16100
    // XOR hash: 5512406178816

    // Total row count: 16100
    // Total XOR hash: 5512406178816
    size_t tablet_id_lines = 0;
    const std::string table_id_str = "Tablet ID:";
    size_t pos = 0;
    while ((pos = output.find(table_id_str, pos)) != std::string::npos) {
      ++tablet_id_lines;
      pos += table_id_str.length();
    }
    CHECK_EQ(tablet_id_lines, num_tablets);

    // Extract the row count from the output.
    // Total row count: 100
    // Total XOR hash: 1234567890
    const auto total_row_count_prefix = "Total row count: ";
    const auto total_xor_hash_prefix = "Total XOR hash: ";
    auto row_count_pos = output.find(total_row_count_prefix);
    auto xor_hash_pos = output.find(total_xor_hash_prefix);
    auto row_count = std::stoull(output.substr(row_count_pos + strlen(total_row_count_prefix)));
    auto xor_hash = std::stoull(output.substr(xor_hash_pos + strlen(total_xor_hash_prefix)));
    LOG(INFO) << "Row count: " << row_count << ", XOR hash: " << xor_hash;
    return std::make_pair(row_count, xor_hash);
  };
  auto output = ASSERT_RESULT(CallAdmin("get_table_hash", table_id));
  auto [row_count, xor_hash] = extract_from_output(output);
  ASSERT_EQ(row_count, rows_inserted);
  ASSERT_NE(xor_hash, 0);

  output = ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht1.ToUint64()));
  auto [row_count2, xor_hash2] = extract_from_output(output);
  ASSERT_LT(row_count2, rows_inserted);
  ASSERT_GT(row_count2, 100);
  ASSERT_NE(xor_hash2, 0);
  ASSERT_NE(xor_hash, xor_hash2);
}

// DB-21953: get_table_hash at a read_ht below the history-retention cutoff must fail with
// SnapshotTooOld, not return a wrong hash over a compacted view. Before the fix it returned OK.
TEST_F(AdminCliTest, TestGetTableHashRejectsTooOldReadTime) {
  // No history retention, so one compaction pushes the cutoff past any earlier read time.
  std::vector<std::string> ts_flags = {
    "--timestamp_history_retention_interval_sec=0"s,
  };
  std::vector<std::string> master_flags;
  BuildAndStart(ts_flags, master_flags);

  const auto table_name =
      YBTableName(YQLDatabase::YQL_DATABASE_CQL, kTableName.namespace_name(), "too_old_table");
  client::TableHandle table;
  ASSERT_OK(table.Create(
      table_name, /* num_tablets */ 1, client::YBSchema(schema_), client_.get()));

  TestYcqlWorkload workload(cluster_.get());
  workload.set_table_name(table_name);
  workload.Setup();
  workload.Start();
  workload.WaitInserted(200);
  workload.StopAndJoin();

  auto tables = ASSERT_RESULT(client_->ListTables(table_name.table_name()));
  ASSERT_EQ(1, tables.size());
  const auto table_id = tables.front().table_id();

  // Grab a read time that's valid now; it'll fall below the cutoff once we compact.
  const auto too_old_ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  // Reading at it now works (still within the window).
  ASSERT_OK(CallAdmin("get_table_hash", table_id, too_old_ht.ToUint64()));

  // More writes + compaction GC the history below the cutoff and push the cutoff past too_old_ht.
  workload.Start();
  workload.WaitInserted(workload.rows_inserted() + 200);
  workload.StopAndJoin();

  // Cutoff tracks safe time, so sleep to make sure too_old_ht is strictly behind it.
  SleepFor(2s * kTimeMultiplier);
  ASSERT_OK(CompactTablets(cluster_.get(), 300s * kTimeMultiplier));

  // Now the same read must be rejected, not return a stale hash.
  auto result = CallAdmin("get_table_hash", table_id, too_old_ht.ToUint64());
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.status().ToString(), "Snapshot too old");

  // A default (safe time) read still works -- only the too-old path is rejected.
  ASSERT_OK(CallAdmin("get_table_hash", table_id));
}

// get_table_hash tests. It only talks to leaders, whose safe time cannot be held back without
// costing them their majority, so these tests use a read time ahead of the cluster's clock instead.
class AdminCliGetTableHashReadTimeTest : public AdminCliTest {
 protected:
  // Brings up the cluster with a populated multi-tablet YCQL table in hash_table_id_.
  void StartClusterWithTable() {
    BuildAndStart();

    const auto table_name = YBTableName(
        YQLDatabase::YQL_DATABASE_CQL, kTableName.namespace_name(), "read_time_table");
    client::TableHandle table;
    ASSERT_OK(table.Create(
        table_name, /* num_tablets */ 3, client::YBSchema(schema_), client_.get()));

    TestYcqlWorkload workload(cluster_.get());
    workload.set_table_name(table_name);
    workload.Setup();
    workload.Start();
    workload.WaitInserted(100);
    workload.StopAndJoin();

    auto tables = ASSERT_RESULT(client_->ListTables(table_name.table_name()));
    ASSERT_EQ(1, tables.size());
    hash_table_id_ = tables.front().table_id();
  }

  // A read time no replica has reached, but inside dump_tablet_data_max_read_time_ahead_ms so it
  // counts as lag, not as a caller error. Recompute per command: yb-admin startup eats a second or
  // two.
  Result<uint64_t> ReadTimeAheadOfClock(int64_t seconds) {
    return VERIFY_RESULT(cluster_->master()->GetServerTime()).AddSeconds(seconds).ToUint64();
  }

  // Reads get_table_hash's whole-table {row count, XOR hash} back out of its stdout.
  static Result<std::pair<uint64_t, uint64_t>> Totals(const std::string& output) {
    const auto value = [&output](const char* prefix) -> Result<uint64_t> {
      auto pos = output.find(prefix);
      SCHECK_NE(
          pos, std::string::npos, NotFound,
          Format("'$0' missing from output: $1", prefix, output));
      return std::stoull(output.substr(pos + strlen(prefix)));
    };
    auto row_count = VERIFY_RESULT(value("Total row count: "));
    auto xor_hash = VERIFY_RESULT(value("Total XOR hash: "));
    return std::make_pair(row_count, xor_hash);
  }

  std::string hash_table_id_;
};

// A read time nothing has reached must be reported as such, not answered with whatever happens to
// be applied. With a zero wait, straight away.
TEST_F(AdminCliGetTableHashReadTimeTest, TestGetTableHashReadTimeNotReachedFailsFast) {
  ASSERT_NO_FATALS(StartClusterWithTable());

  const auto past_ht = ASSERT_RESULT(cluster_->master()->GetServerTime());
  // Safe time trails the present. Let it move past past_ht before asking with a zero wait.
  SleepFor(2s * kTimeMultiplier);

  // A read time the tablets have reached does not involve the wait at all.
  ASSERT_OK(CallAdmin(
      "--read_time_wait_ms", 0, "get_table_hash", hash_table_id_, past_ht.ToUint64()));

  const auto start = MonoTime::Now();
  auto result = CallAdmin(
      "--read_time_wait_ms", 0, "get_table_hash", hash_table_id_,
      ASSERT_RESULT(ReadTimeAheadOfClock(10)));
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.status().ToString(), "is not yet safe on this replica");
  ASSERT_STR_CONTAINS(result.status().ToString(), "behind by");
  // Zero means zero: it did not sit out the 10s gap.
  ASSERT_LT(MonoTime::Now() - start, MonoDelta::FromSeconds(30));

  // A key range narrows which rows get hashed. It must not narrow away the read-time check.
  result = CallAdmin(
      "--read_time_wait_ms", 0, "get_table_hash", hash_table_id_,
      ASSERT_RESULT(ReadTimeAheadOfClock(10)),
      /* start_key_hex */ "8000", /* end_key_hex */ std::string());
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.status().ToString(), "is not yet safe on this replica");
}

// Given time to wait, get_table_hash waits instead of failing, and answers with the same data as a
// read at that time once it is in the past.
TEST_F(AdminCliGetTableHashReadTimeTest, TestGetTableHashWaitsForReadTime) {
  ASSERT_NO_FATALS(StartClusterWithTable());

  const auto past_ht = ASSERT_RESULT(cluster_->master()->GetServerTime());
  SleepFor(2s * kTimeMultiplier);
  const auto past_totals = ASSERT_RESULT(Totals(
      ASSERT_RESULT(CallAdmin("get_table_hash", hash_table_id_, past_ht.ToUint64()))));
  ASSERT_GT(past_totals.first, 0ULL);
  ASSERT_NE(past_totals.second, 0ULL);

  // Nothing writes between the two read times, so the totals have to match.
  const auto future_totals = ASSERT_RESULT(Totals(ASSERT_RESULT(CallAdmin(
      "--read_time_wait_ms", 30000, "get_table_hash", hash_table_id_,
      ASSERT_RESULT(ReadTimeAheadOfClock(8))))));
  ASSERT_EQ(future_totals, past_totals);
}

// DB-21953: get_table_hash registers its read time with the retention policy, which also pins the
// history cutoff for the whole call. That pin is what stops a compaction running alongside the scan
// from garbage-collecting row versions the scan still needs.
//
// To actually exercise the pin we need a scan that opens more than one iterator at the same read
// time: a single open iterator is already safe, since it holds onto its RocksDB files until it
// finishes. A colocated tablet gives us exactly that -- one whole-tablet scan walks each colocated
// table with its own iterator, all under a single read time T. So we get the scan going on the
// first table, kick off a compaction (with history retention off it would push the cutoff past T
// and drop the old versions), and then check that the rest of the scan still reads the data as it
// was at T.
//
// TEST_fetch_next_delay_ms slows the scan down so the compaction reliably lands in the middle of
// it. With the fix the cutoff stays pinned and the totals match the pre-update baseline; without it
// the second table reads the compacted view and the hash comes back wrong.
TEST_F(AdminCliTestWithYSQL, TestGetTableHashPinsCutoffDuringConcurrentCompaction) {
  std::vector<std::string> ts_flags = {
    "--timestamp_history_retention_interval_sec=0"s,
  };
  std::vector<std::string> master_flags;
  BuildAndStart(ts_flags, master_flags);

  // Two colocated tables sharing one tablet, so one whole-tablet scan opens two iterators in turn.
  auto conn = ASSERT_RESULT(cluster_->ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE DATABASE codb WITH colocation = true"));
  auto codb = ASSERT_RESULT(cluster_->ConnectToDB("codb"));
  ASSERT_OK(codb.Execute("CREATE TABLE colo_a (k int PRIMARY KEY, v int)"));
  ASSERT_OK(codb.Execute("CREATE TABLE colo_b (k int PRIMARY KEY, v int)"));
  constexpr int kRowsPerTable = 50;
  ASSERT_OK(codb.ExecuteFormat(
      "INSERT INTO colo_a SELECT g, g * 10 FROM generate_series(1, $0) g", kRowsPerTable));
  ASSERT_OK(codb.ExecuteFormat(
      "INSERT INTO colo_b SELECT g, g * 7  FROM generate_series(1, $0) g", kRowsPerTable));

  // The colocation parent id addresses the whole tablet (both tables).
  const auto parent_table = ASSERT_RESULT(GetColocationParentTableId(client_.get(), "codb"));

  auto parse_totals = [](const std::string& output) -> std::pair<uint64_t, uint64_t> {
    const auto row_prefix = "Total row count: ";
    const auto hash_prefix = "Total XOR hash: ";
    auto rp = output.find(row_prefix);
    auto hp = output.find(hash_prefix);
    CHECK(rp != std::string::npos && hp != std::string::npos) << output;
    return {std::stoull(output.substr(rp + strlen(row_prefix))),
            std::stoull(output.substr(hp + strlen(hash_prefix)))};
  };

  // Read time T captured while every row still holds its original value.
  const auto read_ht = ASSERT_RESULT(cluster_->master()->GetServerTime());
  auto [baseline_rows, baseline_hash] =
      parse_totals(ASSERT_RESULT(CallAdmin("get_table_hash", parent_table, read_ht.ToUint64())));
  ASSERT_EQ(baseline_rows, 2 * kRowsPerTable);

  // Overwrite every row in both tables *after* T. The old values now sit below T; a compaction
  // whose cutoff passes T would GC them, so a fresh iterator reading at T would miss those rows.
  ASSERT_OK(codb.Execute("UPDATE colo_a SET v = v + 100000"));
  ASSERT_OK(codb.Execute("UPDATE colo_b SET v = v + 100000"));

  // Slow every per-table scan (both tables have column "v") so the whole-tablet scan stays in
  // flight long enough for a compaction to land between its two per-table iterators.
  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_fetch_next_delay_column", "v"));
  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_fetch_next_delay_ms", "200"));

  // Whole-tablet scan in the background under a single read time T.
  Result<std::string> scan_output = STATUS(Uninitialized, "");
  std::thread scan_thread([&] {
    scan_output = CallAdmin("get_table_hash", parent_table, read_ht.ToUint64());
  });
  auto joiner = ScopeExit([&scan_thread] {
    if (scan_thread.joinable()) {
      scan_thread.join();
    }
  });

  // Wait long enough for the scan to register read time T and open the first table's iterator (the
  // FetchNext delay keeps it on the first table for ~kRowsPerTable * 200ms), then compact. The
  // compaction completes well before the scan moves on to the second table's fresh iterator.
  SleepFor(5s * kTimeMultiplier);
  ASSERT_OK(CompactTablets(cluster_.get(), 300s * kTimeMultiplier));

  scan_thread.join();

  // The pinned scan must still see the original (pre-update) data across both tables.
  ASSERT_OK(scan_output);
  auto [scan_rows, scan_hash] = parse_totals(*scan_output);
  ASSERT_EQ(scan_rows, baseline_rows);
  ASSERT_EQ(scan_hash, baseline_hash);
}

// Test get_table_hash with a partition-key sub-range on a (non-colocated) hash-partitioned table.
// Exercises the infimum/supremum encoding (empty start_key = -inf, empty end_key = +inf) and that
// a complementary split recombines to the full-table totals.
TEST_F(AdminCliTest, TestGetTableXorHashKeyRange) {
  BuildAndStart();
  const auto table_name =
      YBTableName(YQLDatabase::YQL_DATABASE_CQL, kTableName.namespace_name(), "range_table");

  client::TableHandle table;
  const auto num_tablets = 4;
  ASSERT_OK(table.Create(table_name, num_tablets, client::YBSchema(schema_), client_.get()));

  TestYcqlWorkload workload(cluster_.get());
  workload.set_table_name(table_name);
  workload.Setup();
  workload.Start();
  workload.WaitInserted(300);
  workload.StopAndJoin();

  auto tables = ASSERT_RESULT(client_->ListTables(table_name.table_name()));
  ASSERT_EQ(1, tables.size());
  const auto table_id = tables.front().table_id();

  // Fixed read time so every call below observes identical data.
  auto ht = ASSERT_RESULT(cluster_->master()->GetServerTime());

  // Parse just the totals; ranged calls process a variable number of tablets, so we do not assert
  // on the per-tablet line count here.
  auto parse_totals = [](const std::string& output) -> std::pair<uint64_t, uint64_t> {
    const auto total_row_count_prefix = "Total row count: ";
    const auto total_xor_hash_prefix = "Total XOR hash: ";
    auto row_count_pos = output.find(total_row_count_prefix);
    auto xor_hash_pos = output.find(total_xor_hash_prefix);
    CHECK(row_count_pos != std::string::npos && xor_hash_pos != std::string::npos) << output;
    auto row_count = std::stoull(output.substr(row_count_pos + strlen(total_row_count_prefix)));
    auto xor_hash = std::stoull(output.substr(xor_hash_pos + strlen(total_xor_hash_prefix)));
    return std::make_pair(row_count, xor_hash);
  };

  auto [full_rows, full_hash] =
      parse_totals(ASSERT_RESULT(CallAdmin("get_table_hash", table_id, ht.ToUint64())));
  ASSERT_GT(full_rows, 0);

  // Explicit empty bounds (start == -inf, end == +inf) must reproduce the full-table result.
  auto [open_rows, open_hash] = parse_totals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), std::string(), std::string())));
  ASSERT_EQ(open_rows, full_rows);
  ASSERT_EQ(open_hash, full_hash);

  // Split the 2-byte hash partition-key space at 0x8000 into two complementary, disjoint ranges:
  //   lower = [-inf, 0x8000)   (empty start_key = infimum)
  //   upper = [0x8000, +inf)   (empty end_key   = supremum)
  // Every row falls in exactly one side (start inclusive, end exclusive), so the row counts must
  // sum and the XOR hashes must recombine to the full-table totals.
  const std::string kMidKey = "8000";  // hex-encoded raw partition key.
  auto [lo_rows, lo_hash] = parse_totals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), std::string(), kMidKey)));
  auto [hi_rows, hi_hash] = parse_totals(ASSERT_RESULT(
      CallAdmin("get_table_hash", table_id, ht.ToUint64(), kMidKey, std::string())));

  ASSERT_GT(lo_rows, 0);
  ASSERT_GT(hi_rows, 0);
  ASSERT_EQ(lo_rows + hi_rows, full_rows);
  ASSERT_EQ(lo_hash ^ hi_hash, full_hash);
}

}  // namespace tools
}  // namespace yb
