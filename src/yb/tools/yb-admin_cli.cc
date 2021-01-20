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
#include "yb/tools/yb-admin_cli.h"

#include <iostream>
#include <memory>
#include <utility>

#include <boost/lexical_cast.hpp>
#include <boost/range.hpp>

#include "yb/rpc/messenger.h"
#include "yb/tools/yb-admin_client.h"
#include "yb/util/flags.h"
#include "yb/util/stol_utils.h"
#include "yb/master/master_defaults.h"
#include "yb/util/string_case.h"

DEFINE_string(master_addresses, "localhost:7100",
              "Comma-separated list of YB Master server addresses");
DEFINE_string(init_master_addrs, "",
              "host:port of any yb-master in a cluster");
DEFINE_int64(timeout_ms, 1000 * 60, "RPC timeout in milliseconds");
DEFINE_bool(exclude_dead, false, "Exclude dead tservers from output");


using std::cerr;
using std::endl;
using std::ostringstream;
using std::make_pair;
using std::next;
using std::pair;
using std::string;

using yb::client::YBTableName;
using strings::Substitute;

using namespace std::placeholders;

namespace yb {
namespace tools {

const Status ClusterAdminCli::kInvalidArguments = STATUS(
                                  InvalidArgument, "Invalid arguments for operation");

namespace {

constexpr auto kBlacklistAdd = "ADD";
constexpr auto kBlacklistRemove = "REMOVE";
constexpr int32 kDefaultRpcPort = 9100;

CHECKED_STATUS GetUniverseConfig(ClusterAdminClientClass* client,
                                 const ClusterAdminCli::CLIArguments&) {
  RETURN_NOT_OK_PREPEND(client->GetUniverseConfig(), "Unable to get universe config");
  return Status::OK();
}

CHECKED_STATUS ChangeBlacklist(ClusterAdminClientClass* client,
                               const ClusterAdminCli::CLIArguments& args, bool blacklist_leader,
                               const std::string& errStr) {
  if (args.size() < 4) {
    return ClusterAdminCli::kInvalidArguments;
  }
  const auto change_type = args[2];
  if (change_type != kBlacklistAdd && change_type != kBlacklistRemove) {
    return ClusterAdminCli::kInvalidArguments;
  }
  std::vector<HostPort> hostports;
  for (const auto& arg : boost::make_iterator_range(args.begin() + 3, args.end())) {
    hostports.push_back(VERIFY_RESULT(HostPort::FromString(arg, kDefaultRpcPort)));
  }

  RETURN_NOT_OK_PREPEND(client->ChangeBlacklist(hostports, change_type == kBlacklistAdd,
        blacklist_leader), errStr);
  return Status::OK();
}

CHECKED_STATUS MasterLeaderStepDown(
    ClusterAdminClientClass* client,
    const ClusterAdminCli::CLIArguments& args) {
  const auto leader_uuid = VERIFY_RESULT(client->GetMasterLeaderUuid());
  return client->MasterLeaderStepDown(leader_uuid,
                      args.size() > 2 ? args[2] : std::string());
}

CHECKED_STATUS LeaderStepDown(
    ClusterAdminClientClass* client,
    const ClusterAdminCli::CLIArguments& args) {
  if (args.size() < 3) {
    return ClusterAdminCli::kInvalidArguments;
  }
  std::string dest_uuid = (args.size() > 3) ? args[3] : "";
  RETURN_NOT_OK_PREPEND(
      client->LeaderStepDownWithNewLeader(args[2], dest_uuid), "Unable to step down leader");
  return Status::OK();
}

bool IsEqCaseInsensitive(const string& check, const string& expected) {
  string upper_check, upper_expected;
  ToUpperCase(check, &upper_check);
  ToUpperCase(expected, &upper_expected);
  return upper_check == upper_expected;
}

Result<pair<int, bool>> GetTimeoutAndAddIndexesFlag(
    CLIArgumentsIterator begin,
    const CLIArgumentsIterator& end) {
  bool add_indexes = false;
  int timeout_secs = 20;
  bool seen_timeout_secs = false;
  for (auto iter = begin; iter != end; iter = next(iter)) {
    if (IsEqCaseInsensitive(*iter, "ADD_INDEXES")) {
      if (add_indexes) {
        return ClusterAdminCli::kInvalidArguments;
      }
      add_indexes = true;
    } else if (!seen_timeout_secs) {
      auto maybe_timeout_secs = CheckedStoi(*iter);
      if (!maybe_timeout_secs.ok()) {
        return ClusterAdminCli::kInvalidArguments;
      }
      timeout_secs = maybe_timeout_secs.get();
      seen_timeout_secs = true;
    } else {
      return ClusterAdminCli::kInvalidArguments;
    }
  }
  return make_pair(timeout_secs, add_indexes);
}

} // namespace

Status ClusterAdminCli::Run(int argc, char** argv) {
  const string prog_name = argv[0];
  FLAGS_logtostderr = 1;
  FLAGS_minloglevel = 2;
  ParseCommandLineFlags(&argc, &argv, true);
  InitGoogleLoggingSafe(prog_name.c_str());

  std::unique_ptr<ClusterAdminClientClass> client;
  const string addrs = FLAGS_master_addresses;
  if (!FLAGS_init_master_addrs.empty()) {
    std::vector<HostPort> init_master_addrs;
    RETURN_NOT_OK(HostPort::ParseStrings(
        FLAGS_init_master_addrs,
        master::kMasterDefaultPort,
        &init_master_addrs));
    client.reset(new ClusterAdminClientClass(
        init_master_addrs[0],
        MonoDelta::FromMilliseconds(FLAGS_timeout_ms)));
  } else {
    client.reset(new ClusterAdminClientClass(
        addrs,
        MonoDelta::FromMilliseconds(FLAGS_timeout_ms)));
  }

  RegisterCommandHandlers(client.get());
  SetUsage(prog_name);

  CLIArguments args;
  for (int i = 0; i < argc; ++i) {
    args.push_back(argv[i]);
  }

  if (args.size() < 2) {
    return ClusterAdminCli::kInvalidArguments;
  }

  // Find operation handler by operation name.
  const string op = args[1];
  auto cmd = command_indexes_.find(op);

  if (cmd == command_indexes_.end()) {
    cerr << "Invalid operation: " << op << endl;
    return ClusterAdminCli::kInvalidArguments;
  }

  // Init client.
  Status s = client->Init();

  if (PREDICT_FALSE(!s.ok())) {
    cerr << s.CloneAndPrepend("Unable to establish connection to leader master at [" + addrs + "]."
                              " Please verify the addresses.\n\n").ToString() << endl;
    return STATUS(RuntimeError, "Error connecting to cluster");
  }

  // Run found command.
  s = commands_[cmd->second].fn_(args);
  if (!s.ok()) {
    cerr << "Error: " << s.ToString() << endl;
    return STATUS(RuntimeError, "Error running command");
  }
  return Status::OK();
}

void ClusterAdminCli::Register(string&& cmd_name, string&& cmd_args, CommandFn&& cmd_fn) {
  command_indexes_[cmd_name] = commands_.size();
  commands_.push_back({std::move(cmd_name), std::move(cmd_args), std::move(cmd_fn)});
}

void ClusterAdminCli::SetUsage(const string& prog_name) {
  ostringstream str;

  str << prog_name << " [-master_addresses server1:port,server2:port,server3:port,...] "
      << " [-timeout_ms <millisec>] [-certs_dir_name <dir_name>] <operation>" << endl
      << "<operation> must be one of:" << endl;

  for (size_t i = 0; i < commands_.size(); ++i) {
    str << ' ' << i + 1 << ". " << commands_[i].name_ << commands_[i].usage_arguments_ << endl;
  }

  str << endl;
  str << "<namespace>:" << endl;
  str << "  [(ycql|ysql).]<namespace_name>" << endl;
  str << "<table>:" << endl;
  str << "  <namespace> <table_name> | tableid.<table_id>" << endl;
  str << "<index>:" << endl;
  str << "  <namespace> <index_name> | tableid.<index_id>" << endl;

  google::SetUsageMessage(str.str());
}

void ClusterAdminCli::RegisterCommandHandlers(ClusterAdminClientClass* client) {
  DCHECK_ONLY_NOTNULL(client);

  Register(
      "change_config",
      " <tablet_id> <ADD_SERVER|REMOVE_SERVER> <peer_uuid> [PRE_VOTER|PRE_OBSERVER]",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 5) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string tablet_id = args[2];
        const string change_type = args[3];
        const string peer_uuid = args[4];
        boost::optional<string> member_type;
        if (args.size() > 5) {
          member_type = args[5];
        }
        RETURN_NOT_OK_PREPEND(client->ChangeConfig(tablet_id, change_type, peer_uuid, member_type),
                              "Unable to change config");
        return Status::OK();
      });

  Register(
      "list_tablet_servers", " <tablet_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string tablet_id = args[2];
        RETURN_NOT_OK_PREPEND(client->ListPerTabletTabletServers(tablet_id),
                              Substitute("Unable to list tablet servers of tablet $0", tablet_id));
        return Status::OK();
      });

  static const auto kIncludeDBType = "include_db_type";
  static const auto kIncludeTableId = "include_table_id";
  static const auto kIncludeTableType = "include_table_type";

  Register(
      "list_tables", Format(" [$0] [$1] [$2]", kIncludeDBType, kIncludeTableId, kIncludeTableType),
      [client](const CLIArguments& args) -> Status {
        bool include_db_type = false;
        bool include_table_id = false;
        bool include_table_type = false;
        const std::array<std::pair<const char*, bool*>, 3> flags{
            std::make_pair(kIncludeDBType, &include_db_type),
            std::make_pair(kIncludeTableId, &include_table_id),
            std::make_pair(kIncludeTableType, &include_table_type)
        };
        for (const auto& arg :  args) {
          for (const auto& flag : flags) {
            if (flag.first == arg) {
              *flag.second = true;
            }
          }
        }
        RETURN_NOT_OK_PREPEND(client->ListTables(include_db_type,
                                                 include_table_id,
                                                 include_table_type),
                              "Unable to list tables");
        return Status::OK();
      });

  // Deprecated list_tables commands with arguments should be used instead.
  Register(
      "list_tables_with_db_types", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(
            client->ListTables(true /* include_db_type */,
                               false /* include_table_id*/,
                               false /* include_table_type*/),
            "Unable to list tables");
        return Status::OK();
      });

  Register(
      "list_tablets",
      " <table> [max_tablets] (default 10, set 0 for max)",
      [client](const CLIArguments& args) -> Status {
        int max = -1;
        const auto table_name  = VERIFY_RESULT(ResolveSingleTableName(
            client, args.begin() + 2, args.end(),
            [&max](auto i, const auto& end) -> Status {
              if (std::next(i) == end) {
                max = VERIFY_RESULT(CheckedStoi(*i));
                return Status::OK();
              }
              return ClusterAdminCli::kInvalidArguments;
            }));
        RETURN_NOT_OK_PREPEND(
            client->ListTablets(table_name, max),
            Substitute("Unable to list tablets of table $0", table_name.ToString()));
        return Status::OK();
      });

  static const auto kTableName = "<(<keyspace> <table_name>)|tableid.<table_id>>";
  static const auto kPlacementInfo = "placement_info";
  static const auto kReplicationFactor = "replication_factor";
  static const auto kPlacementUuid = "placement_uuid";

  Register(
      "modify_table_placement_info",
        Format(" [$0] [$1] [$2] [$3]", kTableName, kPlacementInfo, kReplicationFactor,
          kPlacementUuid),
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 5 || args.size() > 7) {
          return ClusterAdminCli::kInvalidArguments;
        }
        std::string placement_info;
        int rf = -1;
        std::string placement_uuid;
        const auto table_name  = VERIFY_RESULT(ResolveSingleTableName(
            client, args.begin() + 2, args.end(),
            [&placement_info, &rf, &placement_uuid](
        auto i, const auto& end) -> Status {
          // Get placement info.
          placement_info = *i;
          i = std::next(i);
          // Get replication factor.
          rf = VERIFY_RESULT(CheckedStoi(*i));
          i = std::next(i);
          // Get optional placement uuid.
          if (i != end) {
            placement_uuid = *i;
          }
          return Status::OK();
        }
        ));
        RETURN_NOT_OK_PREPEND(
            client->ModifyTablePlacementInfo(table_name, placement_info, rf, placement_uuid),
            Substitute("Unable to modify placement info for table $0", table_name.ToString()));
        return Status::OK();
      });

  Register(
      "modify_placement_info", " <placement_info> <replication_factor> [placement_uuid]",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 4 && args.size() != 5) {
          return ClusterAdminCli::kInvalidArguments;
        }
        int rf = boost::lexical_cast<int>(args[3]);
        string placement_uuid = args.size() == 5 ? args[4] : "";
        RETURN_NOT_OK_PREPEND(client->ModifyPlacementInfo(args[2], rf, placement_uuid),
                              Substitute("Unable to modify placement info."));
        return Status::OK();
      });

  Register(
      "clear_placement_info", "",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 2) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->ClearPlacementInfo(),
                              Substitute("Unable to clear placement info."));
        return Status::OK();
      });

  Register(
      "add_read_replica_placement_info", " <placement_info> <replication_factor> [placement_uuid]",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 4 && args.size() != 5) {
          return ClusterAdminCli::kInvalidArguments;
        }
        int rf = boost::lexical_cast<int>(args[3]);
        string placement_uuid = args.size() == 5 ? args[4] : "";
        RETURN_NOT_OK_PREPEND(client->AddReadReplicaPlacementInfo(args[2], rf, placement_uuid),
                              Substitute("Unable to add read replica placement info."));
        return Status::OK();
      });

  Register(
      "modify_read_replica_placement_info",
      " <placement_info> <replication_factor> [placement_uuid]",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 4 && args.size() != 5) {
          return ClusterAdminCli::kInvalidArguments;
        }
        int rf = boost::lexical_cast<int>(args[3]);
        string placement_uuid = args.size() == 5 ? args[4] : "";
        RETURN_NOT_OK_PREPEND(client->ModifyReadReplicaPlacementInfo(placement_uuid, args[2], rf),
                              Substitute("Unable to modify read replica placement info."));
        return Status::OK();
      });

  Register(
      "delete_read_replica_placement_info", "",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 2) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->DeleteReadReplicaPlacementInfo(),
                              Substitute("Unable to delete read replica placement info."));
        return Status::OK();
      });

  Register(
      "delete_namespace", " <namespace>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        auto namespace_name = VERIFY_RESULT(ParseNamespaceName(args[2]));
        RETURN_NOT_OK_PREPEND(client->DeleteNamespace(namespace_name),
                              Substitute("Unable to delete namespace $0", namespace_name.name));
        return Status::OK();
      });

  Register(
      "delete_namespace_by_id", " <namespace_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->DeleteNamespaceById(args[2]),
                              Substitute("Unable to delete namespace $0", args[2]));
        return Status::OK();
      });

  Register(
      "delete_table", " <table>",
      [client](const CLIArguments& args) -> Status {
        const auto table_name = VERIFY_RESULT(
            ResolveSingleTableName(client, args.begin() + 2, args.end()));
        RETURN_NOT_OK_PREPEND(client->DeleteTable(table_name),
                              Substitute("Unable to delete table $0", table_name.ToString()));
        return Status::OK();
      });

  Register(
      "delete_table_by_id", " <table_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->DeleteTableById(args[2]),
                              Substitute("Unable to delete table $0", args[2]));
        return Status::OK();
      });

  Register(
      "delete_index", " <index>",
      [client](const CLIArguments& args) -> Status {
        const auto table_name = VERIFY_RESULT(
            ResolveSingleTableName(client, args.begin() + 2, args.end()));
        RETURN_NOT_OK_PREPEND(client->DeleteIndex(table_name),
                              Substitute("Unable to delete index $0", table_name.ToString()));
        return Status::OK();
      });

  Register(
      "delete_index_by_id", " <index_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->DeleteIndexById(args[2]),
                              Substitute("Unable to delete index $0", args[2]));
        return Status::OK();
      });

  Register(
      "flush_table",
      " <table> [timeout_in_seconds] (default 20)"
      " [ADD_INDEXES] (default false)",
      [client](const CLIArguments& args) -> Status {
        bool add_indexes = false;
        int timeout_secs = 20;
        const auto table_name = VERIFY_RESULT(ResolveSingleTableName(
            client, args.begin() + 2, args.end(),
            [&add_indexes, &timeout_secs](auto i, const auto& end) -> Status {
              std::tie(timeout_secs, add_indexes) = VERIFY_RESULT(
                  GetTimeoutAndAddIndexesFlag(i, end));
              return Status::OK();
            }));
        RETURN_NOT_OK_PREPEND(client->FlushTables({table_name},
                                                  add_indexes,
                                                  timeout_secs,
                                                  false /* is_compaction */),
                              Substitute("Unable to flush table $0", table_name.ToString()));
        return Status::OK();
      });

  Register(
      "flush_table_by_id", " <table_id> [timeout_in_seconds] (default 20)"
      " [ADD_INDEXES] (default false)",
      [client](const CLIArguments& args) -> Status {
        bool add_indexes = false;
        int timeout_secs = 20;
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        std::tie(timeout_secs, add_indexes) = VERIFY_RESULT(GetTimeoutAndAddIndexesFlag(
          args.begin() + 3, args.end()));
        RETURN_NOT_OK_PREPEND(client->FlushTablesById({args[2]},
                                                      add_indexes,
                                                      timeout_secs,
                                                      false /* is_compaction */),
                              Substitute("Unable to flush table $0", args[2]));
        return Status::OK();
      });

  Register(
      "compact_table",
      " <table> [timeout_in_seconds] (default 20)"
      " [ADD_INDEXES] (default false)",
      [client](const CLIArguments& args) -> Status {
        bool add_indexes = false;
        int timeout_secs = 20;
        const auto table_name = VERIFY_RESULT(ResolveSingleTableName(
            client, args.begin() + 2, args.end(),
            [&add_indexes, &timeout_secs](auto i, const auto& end) -> Status {
              std::tie(timeout_secs, add_indexes) = VERIFY_RESULT(
                  GetTimeoutAndAddIndexesFlag(i, end));
              return Status::OK();
            }));
        // We use the same FlushTables RPC to trigger compaction.
        RETURN_NOT_OK_PREPEND(client->FlushTables({table_name},
                                                  add_indexes,
                                                  timeout_secs,
                                                  true /* is_compaction */),
                              Substitute("Unable to compact table $0", table_name.ToString()));
        return Status::OK();
      });

  Register(
      "compact_table_by_id", " <table_id> [timeout_in_seconds] (default 20)"
      " [ADD_INDEXES] (default false)",
      [client](const CLIArguments& args) -> Status {
        bool add_indexes = false;
        int timeout_secs = 20;
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        std::tie(timeout_secs, add_indexes) = VERIFY_RESULT(
            GetTimeoutAndAddIndexesFlag(args.begin() + 3, args.end()));
        // We use the same FlushTables RPC to trigger compaction.
        RETURN_NOT_OK_PREPEND(client->FlushTablesById({args[2]},
                                                      add_indexes,
                                                      timeout_secs,
                                                      true /* is_compaction */),
                              Substitute("Unable to compact table $0", args[2]));
        return Status::OK();
      });

  Register(
      "list_all_tablet_servers", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->ListAllTabletServers(FLAGS_exclude_dead),
                              "Unable to list tablet servers");
        return Status::OK();
      });

  Register(
      "list_all_masters", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->ListAllMasters(),
                              "Unable to list masters");
        return Status::OK();
      });

  Register(
      "change_master_config", " <ADD_SERVER|REMOVE_SERVER> <ip_addr> <port> <0|1>",
      [client](const CLIArguments& args) -> Status {
        int16 new_port = 0;
        string new_host;

        if (args.size() != 5 && args.size() != 6) {
          return ClusterAdminCli::kInvalidArguments;
        }

        const string change_type = args[2];
        if (change_type != "ADD_SERVER" && change_type != "REMOVE_SERVER") {
          return ClusterAdminCli::kInvalidArguments;
        }

        new_host = args[3];
        new_port = VERIFY_RESULT(CheckedStoi(args[4]));

        // For REMOVE_SERVER, default to using host:port to identify server
        // to make it easier to remove down hosts
        bool use_hostport = (change_type == "REMOVE_SERVER");
        if (args.size() == 6) {
          use_hostport = (atoi(args[5].c_str()) != 0);
        }
        RETURN_NOT_OK_PREPEND(client->ChangeMasterConfig(change_type, new_host, new_port,
                                                         use_hostport),
                              "Unable to change master config");
        return Status::OK();
      });

  Register(
      "dump_masters_state", " [CONSOLE]",
      [client](const CLIArguments& args) -> Status {
        bool to_console = false;
        if (args.size() > 2) {
          if (IsEqCaseInsensitive(args[2], "CONSOLE")) {
            to_console = true;
          } else {
            return ClusterAdminCli::kInvalidArguments;
          }
        }
        RETURN_NOT_OK_PREPEND(client->DumpMasterState(to_console),
                              "Unable to dump master state");
        return Status::OK();
      });

  Register(
      "list_tablet_server_log_locations", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->ListTabletServersLogLocations(),
                              "Unable to list tablet server log locations");
        return Status::OK();
      });

  Register(
      "list_tablets_for_tablet_server", " <ts_uuid>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string& ts_uuid = args[2];
        RETURN_NOT_OK_PREPEND(client->ListTabletsForTabletServer(ts_uuid),
                              "Unable to list tablet server tablets");
        return Status::OK();
      });

  Register(
      "set_load_balancer_enabled", " <0|1>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }

        const bool is_enabled = atoi(args[2].c_str()) != 0;
        RETURN_NOT_OK_PREPEND(client->SetLoadBalancerEnabled(is_enabled),
                              "Unable to change load balancer state");
        return Status::OK();
      });

  Register(
      "get_load_balancer_state", "",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 2) {
          return ClusterAdminCli::kInvalidArguments;
        }

        RETURN_NOT_OK_PREPEND(client->GetLoadBalancerState(),
                              "Unable to get the load balancer state");
        return Status::OK();
      });

  Register(
      "get_load_move_completion", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->GetLoadMoveCompletion(),
                              "Unable to get load completion");
        return Status::OK();
      });

  Register(
      "get_leader_blacklist_completion", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->GetLeaderBlacklistCompletion(),
                              "Unable to get leader blacklist completion");
        return Status::OK();
      });

  Register(
      "get_is_load_balancer_idle", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->GetIsLoadBalancerIdle(),
                              "Unable to get is load balancer idle");
        return Status::OK();
      });

  Register(
      "list_leader_counts", " <table>",
      [client](const CLIArguments& args) -> Status {
        const auto table_name = VERIFY_RESULT(
            ResolveSingleTableName(client, args.begin() + 2, args.end()));
        RETURN_NOT_OK_PREPEND(client->ListLeaderCounts(table_name),
                              "Unable to get leader counts");
        return Status::OK();
      });

  Register(
      "setup_redis_table", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->SetupRedisTable(),
                              "Unable to setup Redis keyspace and table");
        return Status::OK();
      });

  Register(
      "drop_redis_table", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->DropRedisTable(),
                              "Unable to drop Redis table");
        return Status::OK();
      });

  Register(
      "get_universe_config", "",
      std::bind(&GetUniverseConfig, client, _1));

  Register(
      "change_blacklist", Format(" <$0|$1> <ip_addr>:<port> [<ip_addr>:<port>]...",
          kBlacklistAdd, kBlacklistRemove),
      std::bind(&ChangeBlacklist, client, _1, false, "Unable to change blacklist"));

  Register(
      "change_leader_blacklist", Format(" <$0|$1> <ip_addr>:<port> [<ip_addr>:<port>]...",
          kBlacklistAdd, kBlacklistRemove),
      std::bind(&ChangeBlacklist, client, _1, true, "Unable to change leader blacklist"));

  Register(
      "master_leader_stepdown", " [dest_uuid]",
      std::bind(&MasterLeaderStepDown, client, _1));

  Register(
      "leader_stepdown", " <tablet_id> [dest_ts_uuid]",
      std::bind(&LeaderStepDown, client, _1));

  Register(
      "split_tablet", " <tablet_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string tablet_id = args[2];
        RETURN_NOT_OK_PREPEND(client->SplitTablet(tablet_id),
                              Format("Unable to start split of tablet $0", tablet_id));
        return Status::OK();
      });

  Register(
      "ysql_catalog_version", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->GetYsqlCatalogVersion(),
                              "Unable to get catalog version");
        return Status::OK();
      });
}

Result<std::vector<client::YBTableName>> ResolveTableNames(ClusterAdminClientClass* client,
                                                           CLIArgumentsIterator i,
                                                           const CLIArgumentsIterator& end,
                                                           TailArgumentsProcessor tail_processor) {
  auto resolver = VERIFY_RESULT(client->BuildTableNameResolver());
  auto tail = i;
  // Greedy algorithm of taking as much tables as possible.
  for (; i != end; ++i) {
    const auto result = resolver.Feed(*i);
    if (!result.ok()) {
      // If tail arguments were not processed suppose it is bad table
      // and return its parsing error instead.
      if (tail_processor && tail_processor(tail, end).ok()) {
        break;
      }
      return result.status();
    }
    if (*result) {
      tail = std::next(i);
    }
  }
  // Handle case when no table name is followed keyspace.
  if (tail != end) {
    if (tail_processor) {
      RETURN_NOT_OK(tail_processor(tail, end));
    } else {
      return STATUS(InvalidArgument, "Table name is missed");
    }
  }
  auto tables = std::move(resolver.values());
  if (tables.empty()) {
    return STATUS(InvalidArgument, "Empty list of tables");
  }
  return tables;
}

Result<client::YBTableName> ResolveSingleTableName(ClusterAdminClientClass* client,
                                                   CLIArgumentsIterator i,
                                                   const CLIArgumentsIterator& end,
                                                   TailArgumentsProcessor tail_processor) {
  auto tables = VERIFY_RESULT(ResolveTableNames(client, i, end, tail_processor));
  if (tables.size() != 1) {
    return STATUS_FORMAT(InvalidArgument, "Single table expected, $0 found", tables.size());
  }
  return std::move(tables.front());
}

}  // namespace tools
}  // namespace yb

int main(int argc, char** argv) {
  yb::Status s = yb::tools::enterprise::ClusterAdminCli().Run(argc, argv);
  if (s.ok()) {
    return 0;
  }

  if (s.IsInvalidArgument()) {
    google::ShowUsageWithFlagsRestrict(argv[0], __FILE__);
  }

  return 1;
}
