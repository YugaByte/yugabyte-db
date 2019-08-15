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

#include "yb/tools/yb-admin_client.h"
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
      "list_snapshots", "",
      [client](const CLIArguments&) -> Status {
        RETURN_NOT_OK_PREPEND(client->ListSnapshots(),
                              "Unable to list snapshots");
        return Status::OK();
      });

  Register(
      "create_snapshot",
      " <keyspace> <table_name> [<keyspace> <table_name>]... [flush_timeout_in_seconds]"
      " (default 60, set 0 to skip flushing)",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 4) {
          UsageAndExit(args[0]);
        }

        const int num_tables = (args.size() - 2)/2;
        vector<YBTableName> tables(num_tables);

        for (int i = 0; i < num_tables; ++i) {
          tables[i].set_namespace_name(args[2 + i*2]);
          tables[i].set_table_name(args[3 + i*2]);
        }

        int timeout_secs = 60;
        if (args.size() % 2 == 1) {
          timeout_secs = std::stoi(args[args.size() - 1].c_str());
        }

        RETURN_NOT_OK_PREPEND(client->CreateSnapshot(tables, timeout_secs),
                              Substitute("Unable to create snapshot of tables: $0",
                                         yb::ToString(tables)));
        return Status::OK();
      });

  Register(
      "restore_snapshot", " <snapshot_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          UsageAndExit(args[0]);
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
          UsageAndExit(args[0]);
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
      "import_snapshot", " <file_name> [<keyspace> <table_name> [<keyspace> <table_name>]...]",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3 || args.size() % 2 != 1) {
          UsageAndExit(args[0]);
        }

        const string file_name = args[2];
        const int num_tables = (args.size() - 3)/2;
        vector<YBTableName> tables(num_tables);

        for (int i = 0; i < num_tables; ++i) {
          tables[i].set_namespace_name(args[3 + i*2]);
          tables[i].set_table_name(args[4 + i*2]);
        }

        string msg = num_tables > 0 ?
            Substitute("Unable to import tables $0 from snapshot meta file $1",
                       yb::ToString(tables), file_name) :
            Substitute("Unable to import snapshot meta file $0", file_name);

        RETURN_NOT_OK_PREPEND(client->ImportSnapshotMetaFile(file_name, tables), msg);
        return Status::OK();
      });

  Register(
      "delete_snapshot", " <snapshot_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 3) {
          UsageAndExit(args[0]);
        }

        const string snapshot_id = args[2];
        RETURN_NOT_OK_PREPEND(client->DeleteSnapshot(snapshot_id),
                              Substitute("Unable to delete snapshot $0", snapshot_id));
        return Status::OK();
      });

  Register(
      "list_replica_type_counts", " <keyspace> <table_name>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() != 4) {
          UsageAndExit(args[0]);
        }
        const YBTableName table_name(args[2], args[3]);
        RETURN_NOT_OK_PREPEND(client->ListReplicaTypeCounts(table_name),
                              "Unable to list live and read-only replica counts");
        return Status::OK();
      });

  Register(
      "set_preferred_zones", " <cloud.region.zone> [<cloud.region.zone>]...",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          UsageAndExit(args[0]);
        }
        RETURN_NOT_OK_PREPEND(client->SetPreferredZones(
          std::vector<string>(args.begin() + 2, args.end())), "Unable to set preferred zones");
        return Status::OK();
      });

  Register(
      "rotate_universe_key", " key_path",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 2) {
          UsageAndExit(args[0]);
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
      "create_cdc_stream", " <table_id>",
      [client](const CLIArguments& args) -> Status {
        if (args.size() < 3) {
          UsageAndExit(args[0]);
        }
        const string table_id = args[2];
        RETURN_NOT_OK_PREPEND(client->CreateCDCStream(table_id),
                              Substitute("Unable to create CDC stream for table $0", table_id));
        return Status::OK();
      });
}

}  // namespace enterprise
}  // namespace tools
}  // namespace yb
