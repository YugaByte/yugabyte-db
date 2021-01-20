// Copyright (c) YugaByte, Inc.
//
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

// Tests for the EE yb-admin command-line tool.

#include "yb/client/client.h"
#include "yb/client/ql-dml-test-base.h"

#include "yb/integration-tests/external_mini_cluster_ent.h"

#include "yb/master/master_backup.proxy.h"

#include "yb/rpc/secure_stream.h"

#include "yb/tools/yb-admin_util.h"

#include "yb/util/env_util.h"
#include "yb/util/path_util.h"
#include "yb/util/subprocess.h"
#include "yb/util/test_util.h"

DECLARE_string(certs_dir);

namespace yb {
namespace tools {

using namespace std::literals;

using std::shared_ptr;
using std::string;

using client::YBTable;
using client::YBTableInfo;
using client::YBTableName;
using client::Transactional;
using master::ListSnapshotsRequestPB;
using master::ListSnapshotsResponsePB;
using master::ListSnapshotRestorationsRequestPB;
using master::ListSnapshotRestorationsResponsePB;
using master::MasterBackupServiceProxy;
using master::SysSnapshotEntryPB;
using rpc::RpcController;

class AdminCliTest : public client::KeyValueTableTest<MiniCluster> {
 protected:
  MasterBackupServiceProxy& BackupServiceProxy() {
    if (!backup_service_proxy_) {
      backup_service_proxy_.reset(new MasterBackupServiceProxy(
          &client_->proxy_cache(), cluster_->leader_mini_master()->bound_rpc_addr()));
    }
    return *backup_service_proxy_.get();
  }

  Status RunAdminToolCommand(const std::initializer_list<string>& args) {
    std::stringstream command;
    command << GetToolPath("yb-admin") << " --master_addresses "
            << cluster_->GetMasterAddresses();
    for (const auto& a : args) {
      command << " " << a;
    }
    const auto command_str = command.str();
    LOG(INFO) << "Run tool: " << command_str;
    return Subprocess::Call(command_str);
  }

  Result<size_t> NumTables(const string& table_name) const;
  void ImportTableAs(const string& snapshot_file, const string& keyspace, const string& table_name);
  void CheckImportedTable(
      const YBTable* src_table, const YBTableName& yb_table_name, bool same_ids = false);
  void CheckAndDeleteImportedTable(
      const string& keyspace, const string& table_name, bool same_ids = false);
  void CheckImportedTableWithIndex(
      const string& keyspace, const string& table_name, const string& index_name,
      bool same_ids = false);

  void DoTestExportImportIndexSnapshot(Transactional transactional);

 private:
  std::unique_ptr<MasterBackupServiceProxy> backup_service_proxy_;
};

TEST_F(AdminCliTest, TestNonTLS) {
  ASSERT_OK(RunAdminToolCommand({"list_all_masters"}));
}

// TODO: Enabled once ENG-4900 is resolved.
TEST_F(AdminCliTest, DISABLED_TestTLS) {
  const auto sub_dir = JoinPathSegments("ent", "test_certs");
  auto root_dir = env_util::GetRootDir(sub_dir) + "/../../";
  ASSERT_OK(RunAdminToolCommand({
    "--certs_dir_name", JoinPathSegments(root_dir, sub_dir), "list_all_masters"}));
}

TEST_F(AdminCliTest, TestCreateSnapshot) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  // There is custom table.
  const auto tables = ASSERT_RESULT(client_->ListTables(table_name, /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());

  ListSnapshotsRequestPB req;
  ListSnapshotsResponsePB resp;
  RpcController rpc;
  ASSERT_OK(BackupServiceProxy().ListSnapshots(req, &resp, &rpc));
  ASSERT_EQ(resp.snapshots_size(), 0);

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand({"create_snapshot", keyspace, table_name}));

  rpc.Reset();
  ASSERT_OK(BackupServiceProxy().ListSnapshots(req, &resp, &rpc));
  ASSERT_EQ(resp.snapshots_size(), 1);

  LOG(INFO) << "Test TestCreateSnapshot finished.";
}

Result<ListSnapshotsResponsePB> WaitForAllSnapshots(MasterBackupServiceProxy* proxy) {
  ListSnapshotsRequestPB req;
  ListSnapshotsResponsePB resp;
  RETURN_NOT_OK(
      WaitFor([proxy, &req, &resp]() -> Result<bool> {
                RpcController rpc;
                RETURN_NOT_OK(proxy->ListSnapshots(req, &resp, &rpc));
                for (auto const& snapshot : resp.snapshots()) {
                  if (snapshot.entry().state() != SysSnapshotEntryPB::COMPLETE) {
                    return false;
                  }
                }
                return true;
              },
              30s, "Waiting for all snapshots to complete"));
  return resp;
}

Result<string> GetCompletedSnapshot(MasterBackupServiceProxy* proxy,
                                    int num_snapshots = 1,
                                    int idx = 0) {
  auto resp = VERIFY_RESULT(WaitForAllSnapshots(proxy));

  if (resp.snapshots_size() != num_snapshots) {
    return STATUS_FORMAT(Corruption, "Wrong snapshot count $0", resp.snapshots_size());
  }

  return SnapshotIdToString(resp.snapshots(idx).id());
}

Result<size_t> AdminCliTest::NumTables(const string& table_name) const {
  auto tables = VERIFY_RESULT(
      client_->ListTables(/* filter */ table_name, /* exclude_ysql */ true));
  return tables.size();
}

void AdminCliTest::CheckImportedTable(const YBTable* src_table,
                                      const YBTableName& yb_table_name,
                                      bool same_ids) {
  shared_ptr<YBTable> table;
  ASSERT_OK(client_->OpenTable(yb_table_name, &table));

  ASSERT_EQ(same_ids, table->id() == src_table->id());
  ASSERT_EQ(table->table_type(), src_table->table_type());
  ASSERT_EQ(table->GetPartitionsCopy(), src_table->GetPartitionsCopy());
  ASSERT_TRUE(table->partition_schema().Equals(src_table->partition_schema()));
  ASSERT_TRUE(table->schema().Equals(src_table->schema()));
  ASSERT_EQ(table->schema().table_properties().is_transactional(),
            src_table->schema().table_properties().is_transactional());
}

void AdminCliTest::CheckAndDeleteImportedTable(const string& keyspace,
                                               const string& table_name,
                                               bool same_ids) {
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));

  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  CheckImportedTable(table_.get(), yb_table_name, same_ids);
  ASSERT_EQ(1, ASSERT_RESULT(NumTables(table_name)));
  ASSERT_OK(client_->DeleteTable(yb_table_name, /* wait */ true));
  ASSERT_EQ(0, ASSERT_RESULT(NumTables(table_name)));
}

void AdminCliTest::ImportTableAs(const string& snapshot_file,
                                 const string& keyspace,
                                 const string& table_name) {
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file, keyspace, table_name}));
  CheckAndDeleteImportedTable(keyspace, table_name);
}

TEST_F(AdminCliTest, TestImportSnapshot) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand({"create_snapshot", keyspace, table_name}));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot(&BackupServiceProxy()));

  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));
  const auto snapshot_file = JoinPathSegments(tmp_dir, "exported_snapshot.dat");
  ASSERT_OK(RunAdminToolCommand({"export_snapshot", snapshot_id, snapshot_file}));

  // Import snapshot into the existing table.
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file}));
  CheckAndDeleteImportedTable(keyspace, table_name, /* same_ids */ true);

  // Import snapshot into original table from the snapshot.
  // (The table was deleted by the call above.)
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file}));
  CheckAndDeleteImportedTable(keyspace, table_name);

  // Import snapshot into non existing namespace.
  ImportTableAs(snapshot_file, keyspace + "_new", table_name);
  // Import snapshot into already existing namespace.
  ImportTableAs(snapshot_file, keyspace, table_name + "_new");
  // Import snapshot into already existing namespace and table.
  ImportTableAs(snapshot_file, keyspace, table_name);

  LOG(INFO) << "Test TestImportSnapshot finished.";
}

TEST_F(AdminCliTest, TestExportImportSnapshot) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand({"create_snapshot", keyspace, table_name}));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot(&BackupServiceProxy()));

  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));
  const auto snapshot_file = JoinPathSegments(tmp_dir, "exported_snapshot.dat");
  ASSERT_OK(RunAdminToolCommand({"export_snapshot", snapshot_id, snapshot_file}));
  // Import below will not create a new table - reusing the old one.
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file, keyspace, table_name}));

  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  CheckImportedTable(table_.get(), yb_table_name, /* same_ids */ true);
  ASSERT_EQ(1, ASSERT_RESULT(NumTables(table_name)));

  LOG(INFO) << "Test TestExportImportSnapshot finished.";
}

void AdminCliTest::CheckImportedTableWithIndex(const string& keyspace,
                                               const string& table_name,
                                               const string& index_name,
                                               bool same_ids) {
  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  const YBTableName yb_index_name(YQL_DATABASE_CQL, keyspace, index_name);

  CheckImportedTable(table_.get(), yb_table_name, same_ids);
  ASSERT_EQ(2, ASSERT_RESULT(NumTables(table_name)));
  CheckImportedTable(index_.get(), yb_index_name, same_ids);
  ASSERT_EQ(1, ASSERT_RESULT(NumTables(index_name)));

  YBTableInfo table_info = ASSERT_RESULT(client_->GetYBTableInfo(yb_table_name));
  YBTableInfo index_info = ASSERT_RESULT(client_->GetYBTableInfo(yb_index_name));
  // Check index ---> table relation.
  ASSERT_EQ(index_info.index_info->indexed_table_id(), table_info.table_id);
  // Check table ---> index relation.
  ASSERT_EQ(table_info.index_map.size(), 1);
  ASSERT_EQ(table_info.index_map.count(index_info.table_id), 1);
  ASSERT_EQ(table_info.index_map.begin()->first, index_info.table_id);
  ASSERT_EQ(table_info.index_map.begin()->second.table_id(), index_info.table_id);
  ASSERT_EQ(table_info.index_map.begin()->second.indexed_table_id(), table_info.table_id);

  ASSERT_OK(client_->DeleteTable(yb_table_name, /* wait */ true));
  ASSERT_EQ(0, ASSERT_RESULT(NumTables(table_name)));
}

void AdminCliTest::DoTestExportImportIndexSnapshot(Transactional transactional) {
  CreateTable(transactional);
  CreateIndex(transactional);

  // Default tables that were created.
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();
  const string& index_name = index_.name().table_name();
  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  const YBTableName yb_index_name(YQL_DATABASE_CQL, keyspace, index_name);

  // Check there are 2 tables.
  ASSERT_EQ(2, ASSERT_RESULT(NumTables(table_name)));

  // Create snapshot of default table and the attached index that gets created.
  ASSERT_OK(RunAdminToolCommand({"create_snapshot", keyspace, table_name}));
  auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot(&BackupServiceProxy()));

  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));
  const auto snapshot_file = JoinPathSegments(tmp_dir, "exported_snapshot.dat");
  ASSERT_OK(RunAdminToolCommand({"export_snapshot", snapshot_id, snapshot_file}));

  // Import table and index into the existing table and index.
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex(keyspace, table_name, index_name, /* same_ids */ true);

  // Import table and index with original names - not providing any names.
  // (The table was deleted by the call above.)
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex(keyspace, table_name, index_name);

  // Import table and index with original names - using the old names.
  ASSERT_OK(RunAdminToolCommand({
      "import_snapshot", snapshot_file, keyspace, table_name, index_name}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex(keyspace, table_name, index_name);

  // Import table and index with original names - providing only old table name.
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file, keyspace, table_name}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex(keyspace, table_name, index_name);

  // Renaming table and index, but keeping the same keyspace.
  ASSERT_OK(RunAdminToolCommand({
      "import_snapshot", snapshot_file, keyspace, "new_" + table_name, "new_" + index_name}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex(keyspace, "new_" + table_name, "new_" + index_name);

  // Keeping the same table and index names, but renaming the keyspace.
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file, "new_" + keyspace}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex("new_" + keyspace, table_name, index_name);

  // Repeat previous keyspace renaming case, but pass explicitly the same table name
  // (and skip index name).
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file, "new_" + keyspace, table_name}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex("new_" + keyspace, table_name, index_name);

  // Import table and index into a new keyspace with old table and index names.
  ASSERT_OK(RunAdminToolCommand({
      "import_snapshot", snapshot_file, "new_" + keyspace, table_name, index_name}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex("new_" + keyspace, table_name, index_name);

  // Rename only index and keyspace, but keep the main table name.
  ASSERT_OK(RunAdminToolCommand({
      "import_snapshot", snapshot_file, "new_" + keyspace, table_name, "new_" + index_name}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex("new_" + keyspace, table_name, "new_" + index_name);

  // Import table and index with renaming into a new keyspace.
  ASSERT_OK(RunAdminToolCommand({
      "import_snapshot", snapshot_file, "new_" + keyspace,
      "new_" + table_name, "new_" + index_name}));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots(&BackupServiceProxy()));
  CheckImportedTableWithIndex("new_" + keyspace, "new_" + table_name, "new_" + index_name);

  // Renaming table only, no new name for the index - expecting error.
  ASSERT_NOK(RunAdminToolCommand({
      "import_snapshot", snapshot_file, keyspace, "new_" + table_name}));
  ASSERT_NOK(RunAdminToolCommand({
      "import_snapshot", snapshot_file, "new_" + keyspace, "new_" + table_name}));
}

TEST_F(AdminCliTest, TestExportImportIndexSnapshot) {
  // Test non-transactional table.
  DoTestExportImportIndexSnapshot(Transactional::kFalse);
  LOG(INFO) << "Test TestExportImportIndexSnapshot finished.";
}

TEST_F(AdminCliTest, TestExportImportIndexSnapshot_ForTransactional) {
  // Test the recreated transactional table.
  DoTestExportImportIndexSnapshot(Transactional::kTrue);
  LOG(INFO) << "Test TestExportImportIndexSnapshot_ForTransactional finished.";
}

Result<SysSnapshotEntryPB::State> WaitForRestoration(MasterBackupServiceProxy* proxy) {
  ListSnapshotRestorationsRequestPB req;
  ListSnapshotRestorationsResponsePB resp;
  RETURN_NOT_OK(
      WaitFor([proxy, &req, &resp]() -> Result<bool> {
        RpcController rpc;
        RETURN_NOT_OK(proxy->ListSnapshotRestorations(req, &resp, &rpc));
        for (auto const& restoration : resp.restorations()) {
          if (restoration.entry().state() == SysSnapshotEntryPB::RESTORING) {
            return false;
          }
        }
        return true;
      },
      30s, "Waiting for all restorations to complete"));

  SCHECK_EQ(resp.restorations_size(), 1, IllegalState, "Expected only one restoration");
  return resp.restorations(0).entry().state();
}

TEST_F(AdminCliTest, TestFailedRestoration) {
  CreateTable(Transactional::kTrue);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand({"create_snapshot", keyspace, table_name}));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot(&BackupServiceProxy()));
  LOG(INFO) << "Created snapshot: " << snapshot_id;

  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));
  const auto snapshot_file = JoinPathSegments(tmp_dir, "exported_snapshot.dat");
  ASSERT_OK(RunAdminToolCommand({"export_snapshot", snapshot_id, snapshot_file}));
  // Import below will not create a new table - reusing the old one.
  ASSERT_OK(RunAdminToolCommand({"import_snapshot", snapshot_file}));

  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  CheckImportedTable(table_.get(), yb_table_name, /* same_ids */ true);
  ASSERT_EQ(1, ASSERT_RESULT(NumTables(table_name)));

  auto new_snapshot_id = ASSERT_RESULT(GetCompletedSnapshot(&BackupServiceProxy(), 2));
  if (new_snapshot_id == snapshot_id) {
    new_snapshot_id = ASSERT_RESULT(GetCompletedSnapshot(&BackupServiceProxy(), 2, 1));
  }
  LOG(INFO) << "Imported snapshot: " << new_snapshot_id;

  ASSERT_OK(RunAdminToolCommand({"restore_snapshot", new_snapshot_id}));

  const SysSnapshotEntryPB::State state = ASSERT_RESULT(WaitForRestoration(&BackupServiceProxy()));
  LOG(INFO) << "Restoration: " << SysSnapshotEntryPB::State_Name(state);
  ASSERT_EQ(state, SysSnapshotEntryPB::FAILED);

  LOG(INFO) << "Test TestFailedRestoration finished.";
}

}  // namespace tools
}  // namespace yb
