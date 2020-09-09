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

#include <gtest/gtest.h>

#include "yb/client/table_handle.h"
#include "yb/client/yb_table_name.h"

#include "yb/common/ql_value.h"

#include "yb/integration-tests/cluster_itest_util.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/test_workload.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/rpc/messenger.h"

#include "yb/master/master.proxy.h"
#include "yb/master/master_backup.proxy.h"
#include "yb/master/mini_master.h"
#include "yb/master/master-test-util.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_snapshots.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/cast.h"
#include "yb/util/pb_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/test_util.h"

using namespace std::literals;

DECLARE_uint64(log_segment_size_bytes);
DECLARE_int32(log_min_seconds_to_retain);
DECLARE_bool(TEST_tablet_verify_flushed_frontier_after_modifying);
DECLARE_bool(enable_ysql);

namespace yb {

using std::make_shared;
using std::shared_ptr;
using std::unique_ptr;
using std::tuple;
using std::set;
using std::vector;

using google::protobuf::RepeatedPtrField;

using client::YBTableName;
using master::MasterBackupServiceProxy;
using master::MasterServiceProxy;
using master::SysRowEntry;
using master::TableInfo;
using master::TabletInfo;
using rpc::Messenger;
using rpc::MessengerBuilder;
using rpc::RpcController;
using tablet::Tablet;
using tablet::TabletPeer;
using tserver::MiniTabletServer;
using util::to_uchar_ptr;

using master::CreateSnapshotRequestPB;
using master::CreateSnapshotResponsePB;
using master::IdPairPB;
using master::ImportSnapshotMetaRequestPB;
using master::ImportSnapshotMetaResponsePB;
using master::ImportSnapshotMetaResponsePB_TableMetaPB;
using master::IsCreateTableDoneRequestPB;
using master::IsCreateTableDoneResponsePB;
using master::ListTablesRequestPB;
using master::ListTablesResponsePB;
using master::ListSnapshotsRequestPB;
using master::ListSnapshotsResponsePB;
using master::MasterServiceProxy;
using master::RestoreSnapshotRequestPB;
using master::RestoreSnapshotResponsePB;
using master::SnapshotInfoPB;
using master::SysNamespaceEntryPB;
using master::SysTablesEntryPB;
using master::SysSnapshotEntryPB;
using master::TableIdentifierPB;

const YBTableName kTableName(YQL_DATABASE_CQL, "my_keyspace", "snapshot_test_table");

class SnapshotTest : public YBMiniClusterTestBase<MiniCluster> {
 public:
  void SetUp() override {
    YBMiniClusterTestBase::SetUp();

    FLAGS_log_min_seconds_to_retain = 5;
    FLAGS_TEST_tablet_verify_flushed_frontier_after_modifying = true;
    FLAGS_enable_ysql = false;

    MiniClusterOptions opts;
    opts.num_tablet_servers = 3;
    cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(cluster_->Start());

    messenger_ = ASSERT_RESULT(
        MessengerBuilder("test-msgr").set_num_reactors(1).Build());
    rpc::ProxyCache proxy_cache(messenger_.get());
    proxy_.reset(new MasterServiceProxy(
        &proxy_cache, cluster_->mini_master()->bound_rpc_addr()));
    proxy_backup_.reset(new MasterBackupServiceProxy(
        &proxy_cache, cluster_->mini_master()->bound_rpc_addr()));

    // Connect to the cluster.
    client_ = ASSERT_RESULT(cluster_->CreateClient());
  }

  void DoTearDown() override {
    Result<bool> exist = client_->TableExists(kTableName);
    ASSERT_OK(exist);

    if (exist.get()) {
      ASSERT_OK(client_->DeleteTable(kTableName));
    }

    client_.reset();

    messenger_->Shutdown();

    if (cluster_) {
      cluster_->Shutdown();
      cluster_.reset();
    }

    YBMiniClusterTestBase::DoTearDown();
  }

  RpcController* ResetAndGetController() {
    controller_.Reset();
    controller_.set_timeout(10s);
    return &controller_;
  }

  void CheckAllSnapshots(
      const std::set<std::tuple<SnapshotId, SysSnapshotEntryPB::State>>& snapshot_info,
      SnapshotId cur_id = "") {
    ListSnapshotsRequestPB list_req;
    ListSnapshotsResponsePB list_resp;

    LOG(INFO) << "Requested available snapshots.";
    const Status s = proxy_backup_->ListSnapshots(
        list_req, &list_resp, ResetAndGetController());

    ASSERT_TRUE(s.ok());
    SCOPED_TRACE(list_resp.DebugString());
    ASSERT_FALSE(list_resp.has_error());

    LOG(INFO) << "Number of snapshots: " << list_resp.snapshots_size();
    ASSERT_EQ(list_resp.snapshots_size(), snapshot_info.size());

    if (cur_id.empty()) {
      ASSERT_FALSE(list_resp.has_current_snapshot_id());
    } else {
      ASSERT_TRUE(list_resp.has_current_snapshot_id());
      ASSERT_EQ(list_resp.current_snapshot_id(), cur_id);
      LOG(INFO) << "Current snapshot: " << list_resp.current_snapshot_id();
    }

    for (int i = 0; i < list_resp.snapshots_size(); ++i) {
      LOG(INFO) << "Snapshot " << i << ": " << list_resp.snapshots(i).DebugString();

      auto search_key = std::make_tuple(
          list_resp.snapshots(i).id(), list_resp.snapshots(i).entry().state());
      ASSERT_TRUE(snapshot_info.find(search_key) != snapshot_info.end())
          << strings::Substitute("Couldn't find snapshot id $0 in state $1",
              list_resp.snapshots(i).id(), list_resp.snapshots(i).entry().state());
    }
  }

  template <typename THandler>
  void WaitTillComplete(const string& handler_name, THandler handler) {
    ASSERT_OK(WaitFor(handler, 30s, handler_name, 100ms, 1.5));
  }

  void WaitForSnapshotOpDone(const string& op_name, const string& snapshot_id) {
    WaitTillComplete(
        op_name,
        [this, &snapshot_id]() -> bool {
          ListSnapshotsRequestPB list_req;
          ListSnapshotsResponsePB list_resp;
          list_req.set_snapshot_id(snapshot_id);

          EXPECT_OK(proxy_backup_->ListSnapshots(list_req, &list_resp, ResetAndGetController()));
          EXPECT_FALSE(list_resp.has_error());
          EXPECT_EQ(list_resp.snapshots_size(), 1);
          return list_resp.snapshots(0).entry().state() == SysSnapshotEntryPB::COMPLETE;
        });
  }

  void WaitForCreateTableDone(const YBTableName& table_name) {
    WaitTillComplete(
        "IsCreateTableDone",
        [this, &table_name]() -> bool {
          IsCreateTableDoneRequestPB req;
          IsCreateTableDoneResponsePB resp;
          table_name.SetIntoTableIdentifierPB(req.mutable_table());

          EXPECT_OK(proxy_->IsCreateTableDone(req, &resp, ResetAndGetController()));
          EXPECT_FALSE(resp.has_error());
          EXPECT_TRUE(resp.has_done());
          return resp.done();
        });
  }

  SnapshotId CreateSnapshot() {
    CreateSnapshotRequestPB req;
    CreateSnapshotResponsePB resp;
    TableIdentifierPB* const table = req.mutable_tables()->Add();
    table->set_table_name(kTableName.table_name());
    table->mutable_namespace_()->set_name(kTableName.namespace_name());

    // Check the request.
    EXPECT_OK(proxy_backup_->CreateSnapshot(req, &resp, ResetAndGetController()));

    // Check the response.
    SCOPED_TRACE(resp.DebugString());
    EXPECT_FALSE(resp.has_error());
    EXPECT_TRUE(resp.has_snapshot_id());
    LOG(INFO) << "Started snapshot creation: ID=" << resp.snapshot_id();
    const string snapshot_id = resp.snapshot_id();

    CheckAllSnapshots(
        {
            std::make_tuple(snapshot_id, SysSnapshotEntryPB::CREATING)
        }, snapshot_id);

    // Check the snapshot creation is complete.
    WaitForSnapshotOpDone("IsCreateSnapshotDone", snapshot_id);

    CheckAllSnapshots(
        {
            std::make_tuple(snapshot_id, SysSnapshotEntryPB::COMPLETE)
        });

    return snapshot_id;
  }

  void VerifySnapshotFiles(const std::string& snapshot_id) {
    std::unordered_map<TabletId, OpId> last_tablet_op;

    size_t max_tablets = 0;
    for (int i = 0; i < cluster_->num_tablet_servers(); ++i) {
      MiniTabletServer* const ts = cluster_->mini_tablet_server(i);
      auto ts_tablet_peers = ts->server()->tablet_manager()->GetTabletPeers();
      max_tablets = std::max(max_tablets, ts_tablet_peers.size());
      for (const auto& tablet_peer : ts_tablet_peers) {
        last_tablet_op[tablet_peer->tablet_id()].MakeAtLeast(
            tablet_peer->consensus()->GetLastReceivedOpId());
      }
    }

    for (int i = 0; i < cluster_->num_tablet_servers(); ++i) {
      MiniTabletServer* ts = cluster_->mini_tablet_server(i);
      auto predicate = [max_tablets, ts]() {
        return ts->server()->tablet_manager()->GetTabletPeers().size() >= max_tablets;
      };
      ASSERT_OK(WaitFor(predicate, 15s, "Wait for peers to be up"));
    }

    // Check snapshot files existence.
    for (int i = 0; i < cluster_->num_tablet_servers(); ++i) {
      MiniTabletServer* const ts = cluster_->mini_tablet_server(i);
      auto ts_tablet_peers = ts->server()->tablet_manager()->GetTabletPeers();
      SCOPED_TRACE(Format("TServer: $0", i));

      // Iterate through all available tablets (on this TabletServer), because there is
      // only one table here (testtb). And snapshot was created for this table.
      for (const auto& tablet_peer : ts_tablet_peers) {
        SCOPED_TRACE(Format("Tablet: $0", tablet_peer->tablet_id()));
        auto last_op_id = last_tablet_op[tablet_peer->tablet_id()];
        ASSERT_OK(WaitFor([tablet_peer, last_op_id]() {
            EXPECT_OK(tablet_peer->WaitUntilConsensusRunning(15s));
            return tablet_peer->consensus()->GetLastCommittedOpId() >= last_op_id;
          },
          15s,
          "Wait for op id commit"
        ));
        FsManager* const fs = tablet_peer->tablet_metadata()->fs_manager();
        const auto rocksdb_dir = tablet_peer->tablet_metadata()->rocksdb_dir();
        const auto top_snapshots_dir = tablet_peer->tablet_metadata()->snapshots_dir();
        const auto snapshot_dir = JoinPathSegments(top_snapshots_dir, snapshot_id);

        LOG(INFO) << "Checking tablet snapshot folder: " << snapshot_dir;
        ASSERT_TRUE(fs->Exists(rocksdb_dir));
        ASSERT_TRUE(fs->Exists(top_snapshots_dir));
        ASSERT_TRUE(fs->Exists(snapshot_dir));
        // Check existence of snapshot files:
        auto list = ASSERT_RESULT(fs->ListDir(snapshot_dir));
        ASSERT_TRUE(std::find(list.begin(), list.end(), "CURRENT") != list.end());
        bool has_manifest = false;
        for (const auto& file : list) {
          SCOPED_TRACE("File: " + file);
          if (file.find("MANIFEST-") == 0) {
            has_manifest = true;
          }
          if (file.find(".sst") != std::string::npos) {
            auto snapshot_path = JoinPathSegments(snapshot_dir, file);
            auto rocksdb_path = JoinPathSegments(rocksdb_dir, file);
            auto snapshot_inode = ASSERT_RESULT(fs->env()->GetFileINode(snapshot_path));
            auto rocksdb_inode = ASSERT_RESULT(fs->env()->GetFileINode(rocksdb_path));
            ASSERT_EQ(snapshot_inode, rocksdb_inode);
            LOG(INFO) << "Snapshot: " << snapshot_path << " vs " << rocksdb_path
                      << ", inode: " << snapshot_inode << " vs " << rocksdb_inode;
          }
        }
        ASSERT_TRUE(has_manifest);
      }
    }
  }

  TestWorkload SetupWorkload() {
    TestWorkload workload(cluster_.get());
    workload.set_table_name(kTableName);
    workload.set_sequential_write(true);
    workload.set_insert_failures_allowed(false);
    workload.set_num_write_threads(1);
    workload.set_write_batch_size(10);
    workload.Setup();
    return workload;
  }

 protected:
  std::unique_ptr<Messenger> messenger_;
  unique_ptr<MasterServiceProxy> proxy_;
  unique_ptr<MasterBackupServiceProxy> proxy_backup_;
  RpcController controller_;
  std::unique_ptr<client::YBClient> client_;
};

TEST_F(SnapshotTest, CreateSnapshot) {
  SetupWorkload(); // Used to create table

  // Check tablet folders before the snapshot creation.
  for (int i = 0; i < cluster_->num_tablet_servers(); ++i) {
    MiniTabletServer* const ts = cluster_->mini_tablet_server(i);
    vector<std::shared_ptr<TabletPeer> > ts_tablet_peers;
    ts->server()->tablet_manager()->GetTabletPeers(&ts_tablet_peers);

    // Iterate through all available tablets (on this TabletServer).
    // There is only one table here (testtb).
    for (std::shared_ptr<TabletPeer>& tablet_peer : ts_tablet_peers) {
      FsManager* const fs = tablet_peer->tablet_metadata()->fs_manager();
      const string rocksdb_dir = tablet_peer->tablet_metadata()->rocksdb_dir();
      const string top_snapshots_dir = tablet_peer->tablet_metadata()->snapshots_dir();

      ASSERT_TRUE(fs->Exists(rocksdb_dir));
      ASSERT_TRUE(fs->Exists(top_snapshots_dir));
    }
  }

  CheckAllSnapshots({});

  // Check CreateSnapshot().
  const string snapshot_id = CreateSnapshot();

  ASSERT_NO_FATALS(VerifySnapshotFiles(snapshot_id));

  ASSERT_OK(cluster_->RestartSync());
}

TEST_F(SnapshotTest, RestoreSnapshot) {
  TestWorkload workload = SetupWorkload();
  workload.Start();

  workload.WaitInserted(100);

  CheckAllSnapshots({});

  int64_t min_inserted = workload.rows_inserted();
  // Check CreateSnapshot().
  const auto snapshot_id = CreateSnapshot();
  int64_t max_inserted = workload.rows_inserted();

  workload.WaitInserted(max_inserted + 100);

  workload.StopAndJoin();

  // Check RestoreSnapshot().
  {
    RestoreSnapshotRequestPB req;
    RestoreSnapshotResponsePB resp;
    req.set_snapshot_id(snapshot_id);

    // Check the request.
    ASSERT_OK(proxy_backup_->RestoreSnapshot(req, &resp, ResetAndGetController()));

    // Check the response.
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
    LOG(INFO) << "Started snapshot restoring: ID=" << snapshot_id;
  }

  CheckAllSnapshots(
      {
          std::make_tuple(snapshot_id, SysSnapshotEntryPB::RESTORING)
      }, snapshot_id);

  // Check the snapshot restoring is complete.
  WaitForSnapshotOpDone("IsRestoreSnapshotDone", snapshot_id);

  CheckAllSnapshots(
      {
          std::make_tuple(snapshot_id, SysSnapshotEntryPB::COMPLETE)
      });

  client::TableHandle table;
  ASSERT_OK(table.Open(kTableName, client_.get()));
  int64_t inserted_before_min = 0;
  for (const auto& row : client::TableRange(table)) {
    auto key = row.column(0).int32_value();
    ASSERT_LE(key, max_inserted);
    ASSERT_GE(key, 1);
    if (key <= min_inserted) {
      ++inserted_before_min;
    }
  }
  ASSERT_EQ(inserted_before_min, min_inserted);
}

TEST_F(SnapshotTest, SnapshotRemoteBootstrap) {
  auto* const ts0 = cluster_->mini_tablet_server(0);

  // Shutdown one node, so remote bootstrap will be required after its start.
  ts0->Shutdown();
  auto se = ScopeExit([ts0] {
    // Restart the node in the end, because we need to perform table deletion, etc.
    LOG(INFO) << "Restarting the stopped tserver";
    ASSERT_OK(ts0->RestartStoppedServer());
    ASSERT_OK(ts0->WaitStarted());
  });

  SnapshotId snapshot_id;
  {
    LOG(INFO) << "Setting up workload";
    TestWorkload workload = SetupWorkload();
    workload.Start();
    auto se = ScopeExit([&workload] {
      LOG(INFO) << "Stopping workload";
      workload.StopAndJoin();
    });
    LOG(INFO) << "Waiting for data to be inserted";
    workload.WaitInserted(1000);

    LOG(INFO) << "Creating snapshot";
    snapshot_id = CreateSnapshot();

    LOG(INFO) << "Wait to make sure that we would need remote bootstrap";
    std::this_thread::sleep_for(std::chrono::seconds(FLAGS_log_min_seconds_to_retain) * 1.1);

    // Workload will stop here at the latest.
  }

  // Flushing tablets for all tablet servers except for the one that we stopped.
  for (int i = 1; i < cluster_->num_tablet_servers(); ++i) {
    ASSERT_OK(cluster_->mini_tablet_server(i)->FlushTablets());
  }

  ASSERT_OK(cluster_->CleanTabletLogs());

  ASSERT_OK(ts0->Start());
  ASSERT_NO_FATALS(VerifySnapshotFiles(snapshot_id));
}

TEST_F(SnapshotTest, ImportSnapshotMeta) {
  TestWorkload workload = SetupWorkload();
  workload.Start();
  workload.WaitInserted(100);

  CheckAllSnapshots({});

  Result<bool> result_exist = client_->TableExists(kTableName);
  ASSERT_OK(result_exist);
  ASSERT_TRUE(result_exist.get());

  // Check CreateSnapshot().
  const auto snapshot_id = CreateSnapshot();

  workload.StopAndJoin();

  // Check the snapshot creating is complete.
  WaitForSnapshotOpDone("IsCreateSnapshotDone", snapshot_id);

  CheckAllSnapshots(
      {
          std::make_tuple(snapshot_id, SysSnapshotEntryPB::COMPLETE)
      });

  ListSnapshotsRequestPB list_req;
  ListSnapshotsResponsePB list_resp;
  list_req.set_snapshot_id(snapshot_id);
  ASSERT_OK(proxy_backup_->ListSnapshots(list_req, &list_resp, ResetAndGetController()));
  LOG(INFO) << "Requested available snapshots.";
  SCOPED_TRACE(list_resp.DebugString());
  ASSERT_FALSE(list_resp.has_error());

  ASSERT_EQ(list_resp.snapshots_size(), 1);
  const SnapshotInfoPB& snapshot = list_resp.snapshots(0);

  // Get snapshot items names.
  const SysSnapshotEntryPB& snapshot_pb = snapshot.entry();
  const int old_table_num_tablets = snapshot_pb.tablet_snapshots_size();
  string old_table_name, old_namespace_name;

  for (const SysRowEntry& entry : snapshot_pb.entries()) {
    switch (entry.type()) {
      case SysRowEntry::NAMESPACE: { // Get NAMESPACE name.
        SysNamespaceEntryPB meta;
        const string& data = entry.data();
        ASSERT_OK(pb_util::ParseFromArray(&meta, to_uchar_ptr(data.data()), data.size()));
        ASSERT_TRUE(old_namespace_name.empty()); // One namespace allowed.
        old_namespace_name = meta.name();
        break;
      }
      case SysRowEntry::TABLE: { // Recreate TABLE.
        SysTablesEntryPB meta;
        const string& data = entry.data();
        ASSERT_OK(pb_util::ParseFromArray(&meta, to_uchar_ptr(data.data()), data.size()));
        ASSERT_TRUE(old_table_name.empty()); // One table allowed.
        old_table_name = meta.name();
        break;
      }
      case SysRowEntry::TABLET: // No need to get tablet info. Ignore.
        break;
      default:
        ASSERT_OK(STATUS_SUBSTITUTE(
            IllegalState, "Unexpected snapshot entry type $0", entry.type()));
    }
  }

  LOG(INFO) << "Deleting table & namespace: " << kTableName.ToString();
  ASSERT_OK(client_->DeleteTable(kTableName));
  ASSERT_OK(client_->DeleteNamespace(kTableName.namespace_name()));

  result_exist = client_->TableExists(kTableName);
  ASSERT_OK(result_exist);
  ASSERT_FALSE(result_exist.get());

  result_exist = client_->NamespaceExists(kTableName.namespace_name());
  ASSERT_OK(result_exist);
  ASSERT_FALSE(result_exist.get());

  // Check ImportSnapshotMeta().
  {
    ImportSnapshotMetaRequestPB req;
    ImportSnapshotMetaResponsePB resp;
    *req.mutable_snapshot() = snapshot;

    // Check the request.
    ASSERT_OK(proxy_backup_->ImportSnapshotMeta(req, &resp, ResetAndGetController()));

    // Check the response.
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error());
    LOG(INFO) << "Imported snapshot: ID=" << snapshot_id << ". ID map:";

    const RepeatedPtrField<ImportSnapshotMetaResponsePB_TableMetaPB>& tables_meta =
        resp.tables_meta();

    for (int i = 0; i < tables_meta.size(); ++i) {
      const ImportSnapshotMetaResponsePB_TableMetaPB& table_meta = tables_meta.Get(i);

      const IdPairPB& ns_pair = table_meta.namespace_ids();
      LOG(INFO) << "Keyspace: " << ns_pair.old_id() << " -> " << ns_pair.new_id();
      ASSERT_NE(ns_pair.old_id(), ns_pair.new_id());

      const string new_namespace_name = cluster_->mini_master()->master()->catalog_manager()->
          GetNamespaceName(ns_pair.new_id());
      ASSERT_EQ(old_namespace_name, new_namespace_name);

      const IdPairPB& table_pair = table_meta.table_ids();
      LOG(INFO) << "Table: " << table_pair.old_id() << " -> " << table_pair.new_id();
      ASSERT_NE(table_pair.old_id(), table_pair.new_id());
      scoped_refptr<TableInfo> info = cluster_->mini_master()->master()->catalog_manager()->
          GetTableInfo(table_pair.new_id());
      ASSERT_EQ(old_table_name, info->name());
      vector<scoped_refptr<TabletInfo>> tablets;
      info->GetAllTablets(&tablets);
      ASSERT_EQ(old_table_num_tablets, tablets.size());

      const RepeatedPtrField<IdPairPB>& tablets_map = table_meta.tablets_ids();
      for (int j = 0; j < tablets_map.size(); ++j) {
        const IdPairPB& pair = tablets_map.Get(j);
        LOG(INFO) << "Tablet " << j << ": " << pair.old_id() << " -> " << pair.new_id();
        ASSERT_NE(pair.old_id(), pair.new_id());
      }
    }
  }

  // Check imported table creating is complete.
  WaitForCreateTableDone(kTableName);

  result_exist = client_->TableExists(kTableName);
  ASSERT_OK(result_exist);
  ASSERT_TRUE(result_exist.get());

  result_exist = client_->NamespaceExists(kTableName.namespace_name());
  ASSERT_OK(result_exist);
  ASSERT_TRUE(result_exist.get());

  LOG(INFO) << "Test ImportSnapshotMeta finished.";
}

} // namespace yb
