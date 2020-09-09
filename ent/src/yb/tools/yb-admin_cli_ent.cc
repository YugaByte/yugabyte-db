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

#include "yb/tools/yb-admin_cli.h"

#include <iostream>

#include <boost/algorithm/string.hpp>

#include "yb/tools/yb-admin_client.h"
#include "yb/util/stol_utils.h"
#include "yb/util/string_case.h"
#include "yb/util/tostring.h"

namespace yb {
namespace tools {
namespace enterprise {

using std::cerr;
using std::endl;
using std::string;
using std::vector;

using client::YBTableName;
using strings::Substitute;

void ClusterAdminCli::RegisterCommandHandlers(ClusterAdminClientClass* client) {
  super::RegisterCommandHandlers(client);

  Register(
      "list_snapshots", " [SHOW_DETAILS] [NOT_SHOW_RESTORED] [SHOW_DELETED]",
      [client](const CLIArguments& args) -> Status {
        bool show_details = false;
        bool show_restored = true;
        bool show_deleted = false;

        if (args.size() > 4) {
          return ClusterAdminCli::kInvalidArguments;
        }
        for (int i = 2; i < args.size(); ++i) {
          string uppercase_flag;
          ToUpperCase(args[i], &uppercase_flag);

          if (uppercase_flag == "SHOW_DETAILS") {
            show_details = true;
          } else if (uppercase_flag == "NOT_SHOW_RESTORED") {
            show_restored = false;
          } else if (uppercase_flag == "SHOW_DELETED") {
            show_deleted = true;
          } else {
            return ClusterAdminCli::kInvalidArguments;
          }
        }

        RETURN_NOT_OK_PREPEND(client->ListSnapshots(show_details, show_restored, show_deleted),
                              "Unable to list snapshots");
        return Status::OK();
      });

  Register(
      "create_snapshot",
      " <table>"
      " [<table>]..."
      " [flush_timeout_in_seconds] (default 60, set 0 to skip flushing)",
      [client](const CLIArguments& args) -> Status {
        int timeout_secs = 60;
        const auto tables = VERIFY_RESULT(ResolveTableNames(
            client, args.begin() + 2, args.end(),
            [&timeout_secs](auto i, const auto& end) -> Status {
              if (std::next(i) == end) {
                timeout_secs = VERIFY_RESULT(CheckedStoi(*i));
                return Status::OK();
              }
              return ClusterAdminCli::kInvalidArguments;
            }));
        RETURN_NOT_OK_PREPEND(client->CreateSnapshot(tables, true, timeout_secs),
                              Substitute("Unable to create snapshot of tables: $0",
                                         yb::ToString(tables)));
        return Status::OK();
      });

  Register(
      "create_keyspace_snapshot", " [ycql.]<keyspace_name>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }

        const TypedNamespaceName keyspace = VERIFY_RESULT(ParseNamespaceName(args[2]));
        SCHECK_NE(
            keyspace.db_type, YQL_DATABASE_PGSQL, InvalidArgument,
            Format("Wrong keyspace type: $0", YQLDatabase_Name(keyspace.db_type)));

        RETURN_NOT_OK_PREPEND(client->CreateNamespaceSnapshot(keyspace),
                              Substitute("Unable to create snapshot of keyspace: $0",
                                         keyspace.name));
        return Status::OK();
      });

  Register(
      "create_database_snapshot", " [ysql.]<database_name>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }

        const TypedNamespaceName database =
            VERIFY_RESULT(ParseNamespaceName(args[2], YQL_DATABASE_PGSQL));
        SCHECK_EQ(
            database.db_type, YQL_DATABASE_PGSQL, InvalidArgument,
            Format("Wrong database type: $0", YQLDatabase_Name(database.db_type)));

        RETURN_NOT_OK_PREPEND(client->CreateNamespaceSnapshot(database),
                              Substitute("Unable to create snapshot of database: $0",
                                         database.name));
        return Status::OK();
      });

  Register(
      "restore_snapshot", " <snapshot_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }

        const string snapshot_id = args[2];
        RETURN_NOT_OK_PREPEND(client->RestoreSnapshot(snapshot_id),
                              Substitute("Unable to restore snapshot $0", snapshot_id));
        return Status::OK();
      });

  Register(
      "export_snapshot", " <snapshot_id> <file_name>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 4) {
          return ClusterAdminCli::kInvalidArguments;
        }

        const string snapshot_id = args[2];
        const string file_name = args[3];
        RETURN_NOT_OK_PREPEND(client->CreateSnapshotMetaFile(snapshot_id, file_name),
                              Substitute("Unable to export snapshot $0 to file $1",
                                         snapshot_id,
                                         file_name));
        return Status::OK();
      });

  Register(
      "import_snapshot", " <file_name> [[ycql.]<keyspace_name> <table_name> [<table_name>]...]",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }

        const string file_name = args[2];
        TypedNamespaceName keyspace;
        int num_tables = 0;
        vector<YBTableName> tables;

        if (args.size() >= 4) {
          keyspace = VERIFY_RESULT(ParseNamespaceName(args[3]));
          num_tables = args.size() - 4;

          if (num_tables > 0) {
            LOG_IF(DFATAL, keyspace.name.empty()) << "Uninitialized keyspace: " << keyspace.name;
            tables.reserve(num_tables);

            for (int i = 0; i < num_tables; ++i) {
              tables.push_back(YBTableName(keyspace.db_type, keyspace.name, args[4 + i]));
            }
          }
        }

        const string msg = num_tables > 0 ?
            Substitute("Unable to import tables $0 from snapshot meta file $1",
                       yb::ToString(tables), file_name) :
            Substitute("Unable to import snapshot meta file $0", file_name);

        RETURN_NOT_OK_PREPEND(client->ImportSnapshotMetaFile(file_name, keyspace, tables), msg);
        return Status::OK();
      });

  Register(
      "delete_snapshot", " <snapshot_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }

        const string snapshot_id = args[2];
        RETURN_NOT_OK_PREPEND(client->DeleteSnapshot(snapshot_id),
                              Substitute("Unable to delete snapshot $0", snapshot_id));
        return Status::OK();
      });

  Register(
      "list_replica_type_counts",
      " <table>",
      [client](const CLIArguments& args) -> Status {
        const auto table_name = VERIFY_RESULT(
            ResolveSingleTableName(client, args.begin() + 2, args.end()));
        RETURN_NOT_OK_PREPEND(client->ListReplicaTypeCounts(table_name),
                              "Unable to list live and read-only replica counts");
        return Status::OK();
      });

  Register(
      "set_preferred_zones", " <cloud.region.zone> [<cloud.region.zone>]...",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->SetPreferredZones(
          std::vector<string>(args.begin() + 2, args.end())), "Unable to set preferred zones");
        return Status::OK();
      });

  Register(
      "rotate_universe_key", " key_path",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 2) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(
            client->RotateUniverseKey(args[2]), "Unable to rotate universe key.");
        return Status::OK();
      });

  Register(
      "disable_encryption", "",
      [client](const CLIArguments& args) -> Status {
        RETURN_NOT_OK_PREPEND(client->DisableEncryption(), "Unable to disable encryption.");
        return Status::OK();
      });

  Register(
      "is_encryption_enabled", "",
      [client](const CLIArguments& args) -> Status {
        RETURN_NOT_OK_PREPEND(client->IsEncryptionEnabled(), "Unable to get encryption status.");
        return Status::OK();
      });

  Register(
      "add_universe_key_to_all_masters", " key_id key_path",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 4) {
          return ClusterAdminCli::kInvalidArguments;
        }
        string key_id = args[2];
        faststring contents;
        RETURN_NOT_OK(ReadFileToString(Env::Default(), args[3], &contents));
        string universe_key = contents.ToString();

        RETURN_NOT_OK_PREPEND(client->AddUniverseKeyToAllMasters(key_id, universe_key),
                              "Unable to add universe key to all masters.");
        return Status::OK();
      });

  Register(
      "all_masters_have_universe_key_in_memory", " key_id",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->AllMastersHaveUniverseKeyInMemory(args[2]),
                              "Unable to check whether master has universe key in memory.");
        return Status::OK();
      });

  Register(
      "rotate_universe_key_in_memory", " key_id",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        string key_id = args[2];

        RETURN_NOT_OK_PREPEND(client->RotateUniverseKeyInMemory(key_id),
                              "Unable rotate universe key in memory.");
        return Status::OK();
      });

  Register(
      "disable_encryption_in_memory", "",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 2) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->DisableEncryptionInMemory(), "Unable to disable encryption.");
        return Status::OK();
      });

  Register(
      "write_universe_key_to_file", " <key_id> <file_name>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 4) {
          return ClusterAdminCli::kInvalidArguments;
        }
        RETURN_NOT_OK_PREPEND(client->WriteUniverseKeyToFile(args[2], args[3]),
                              "Unable to write key to file");
        return Status::OK();
      });

  Register(
      "create_cdc_stream", " <table_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string table_id = args[2];
        RETURN_NOT_OK_PREPEND(client->CreateCDCStream(table_id),
                              Substitute("Unable to create CDC stream for table $0", table_id));
        return Status::OK();
      });

  Register(
      "delete_cdc_stream", " <stream_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string stream_id = args[2];
        RETURN_NOT_OK_PREPEND(client->DeleteCDCStream(stream_id),
            Substitute("Unable to delete CDC stream id $0", stream_id));
        return Status::OK();
      });

  Register(
      "list_cdc_streams", " [table_id]",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 2 && args.size() != 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string table_id = (args.size() == 3 ? args[2] : "");
        RETURN_NOT_OK_PREPEND(client->ListCDCStreams(table_id),
            Substitute("Unable to list CDC streams for table $0", table_id));
        return Status::OK();
      });

  Register(
      "setup_universe_replication",
      " <producer_universe_uuid> <producer_master_addresses> <comma_separated_list_of_table_ids>"
          " [comma_separated_list_of_producer_bootstrap_ids]"  ,
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 5) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string producer_uuid = args[2];

        vector<string> producer_addresses;
        boost::split(producer_addresses, args[3], boost::is_any_of(","));

        vector<string> table_uuids;
        boost::split(table_uuids, args[4], boost::is_any_of(","));

        vector<string> producer_bootstrap_ids;
        if (args.size() == 6) {
          boost::split(producer_bootstrap_ids, args[5], boost::is_any_of(","));
        }

        RETURN_NOT_OK_PREPEND(client->SetupUniverseReplication(producer_uuid,
                                                               producer_addresses,
                                                               table_uuids,
                                                               producer_bootstrap_ids),
                              Substitute("Unable to setup replication from universe $0",
                                         producer_uuid));
        return Status::OK();
      });

  Register(
      "delete_universe_replication", " <producer_universe_uuid>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string producer_id = args[2];
        RETURN_NOT_OK_PREPEND(client->DeleteUniverseReplication(producer_id),
                              Substitute("Unable to delete replication for universe $0",
                              producer_id));
        return Status::OK();
      });

  Register(
      "alter_universe_replication",
      " <producer_universe_uuid>"
      " {set_master_addresses <producer_master_addresses,...> |"
      "  add_table <table_id>[, <table_id>...] | remove_table <table_id>[, <table_id>...] }",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 5) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string producer_uuid = args[2];
        vector<string> master_addresses;
        vector<string> add_tables;
        vector<string> remove_tables;

        vector<string> newElem, *lst;
        if (args[3] == "set_master_addresses") lst = &master_addresses;
        else if (args[3] == "add_table") lst = &add_tables;
        else if (args[3] == "remove_table") lst = &remove_tables;
        else
          return ClusterAdminCli::kInvalidArguments;

        boost::split(newElem, args[4], boost::is_any_of(","));
        lst->insert(lst->end(), newElem.begin(), newElem.end());

        RETURN_NOT_OK_PREPEND(client->AlterUniverseReplication(producer_uuid,
                                                               master_addresses,
                                                               add_tables,
                                                               remove_tables),
            Substitute("Unable to alter replication for universe $0", producer_uuid));

        return Status::OK();
      });

  Register(
      "set_universe_replication_enabled", " <producer_universe_uuid> <0|1>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 4) {
          return ClusterAdminCli::kInvalidArguments;
        }
        const string producer_id = args[2];
        const bool is_enabled = atoi(args[3].c_str()) != 0;
        RETURN_NOT_OK_PREPEND(client->SetUniverseReplicationEnabled(producer_id, is_enabled),
            Substitute("Unable to $0 replication for universe $1",
                is_enabled ? "enable" : "disable",
                producer_id));
        return Status::OK();
      });


  Register(
      "bootstrap_cdc_producer", " <comma_separated_list_of_table_ids>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          return ClusterAdminCli::kInvalidArguments;
        }

        vector<string> table_ids;
        boost::split(table_ids, args[2], boost::is_any_of(","));

        RETURN_NOT_OK_PREPEND(client->BootstrapProducer(table_ids),
                              "Unable to bootstrap CDC producer");
        return Status::OK();
      });
}

}  // namespace enterprise
}  // namespace tools
}  // namespace yb
