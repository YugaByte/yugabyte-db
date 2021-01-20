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

#include "yb/client/session.h"
#include "yb/client/transaction.h"
#include "yb/client/txn-test-base.h"

#include "yb/common/transaction_error.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/master_backup.proxy.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/sys_catalog_constants.h"

#include "yb/tablet/tablet_snapshots.h"

#include "yb/tserver/mini_tablet_server.h"

using namespace std::literals;
using yb::master::SysSnapshotEntryPB;

DECLARE_bool(enable_history_cutoff_propagation);
DECLARE_bool(flush_rocksdb_on_shutdown);
DECLARE_int32(TEST_inject_status_resolver_complete_delay_ms);
DECLARE_int32(history_cutoff_propagation_interval_ms);
DECLARE_int32(raft_heartbeat_interval_ms);
DECLARE_int32(timestamp_history_retention_interval_sec);
DECLARE_int32(unresponsive_ts_rpc_timeout_ms);
DECLARE_uint64(max_clock_skew_usec);
DECLARE_uint64(snapshot_coordinator_cleanup_delay_ms);
DECLARE_uint64(snapshot_coordinator_poll_interval_ms);

namespace yb {
namespace client {

using Snapshots = google::protobuf::RepeatedPtrField<master::SnapshotInfoPB>;
using ImportedSnapshotData = google::protobuf::RepeatedPtrField<
    master::ImportSnapshotMetaResponsePB::TableMetaPB>;

constexpr auto kWaitTimeout = 15s;

class BackupTxnTest : public TransactionTestBase<MiniCluster> {
 protected:
  void SetUp() override {
    FLAGS_enable_history_cutoff_propagation = true;
    SetIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);
    mini_cluster_opt_.num_masters = 3;
    TransactionTestBase::SetUp();
  }

  void DoBeforeTearDown() override {
    if (!testing::Test::HasFailure()) {
      FLAGS_flush_rocksdb_on_shutdown = false;
      ASSERT_OK(cluster_->RestartSync());
    }

    TransactionTestBase::DoBeforeTearDown();
  }

  master::MasterBackupServiceProxy MakeBackupServiceProxy() {
    return master::MasterBackupServiceProxy(
        &client_->proxy_cache(), cluster_->leader_mini_master()->bound_rpc_addr());
  }

  Result<TxnSnapshotId> StartSnapshot() {
    rpc::RpcController controller;
    controller.set_timeout(60s);
    master::CreateSnapshotRequestPB req;
    req.set_transaction_aware(true);
    auto id = req.add_tables();
    id->set_table_id(table_.table()->id());
    master::CreateSnapshotResponsePB resp;
    RETURN_NOT_OK(MakeBackupServiceProxy().CreateSnapshot(req, &resp, &controller));
    return FullyDecodeTxnSnapshotId(resp.snapshot_id());
  }

  Result<SysSnapshotEntryPB::State> SnapshotState(const TxnSnapshotId& snapshot_id) {
    auto snapshots = VERIFY_RESULT(ListSnapshots(snapshot_id));
    if (snapshots.size() != 1) {
      return STATUS_FORMAT(RuntimeError, "Wrong number of snapshots, one expected but $0 found",
                           snapshots.size());
    }
    LOG(INFO) << "Snapshot state: " << snapshots[0].ShortDebugString();
    return snapshots[0].entry().state();
  }

  Result<bool> IsSnapshotDone(const TxnSnapshotId& snapshot_id) {
    return VERIFY_RESULT(SnapshotState(snapshot_id)) == SysSnapshotEntryPB::COMPLETE;
  }

  Result<TxnSnapshotRestorationId> StartRestoration(const TxnSnapshotId& snapshot_id) {
    master::RestoreSnapshotRequestPB req;
    master::RestoreSnapshotResponsePB resp;

    rpc::RpcController controller;
    controller.set_timeout(60s);
    req.set_snapshot_id(snapshot_id.data(), snapshot_id.size());
    RETURN_NOT_OK(MakeBackupServiceProxy().RestoreSnapshot(req, &resp, &controller));
    return FullyDecodeTxnSnapshotRestorationId(resp.restoration_id());
  }

  Result<bool> IsRestorationDone(const TxnSnapshotRestorationId& restoration_id) {
    master::ListSnapshotRestorationsRequestPB req;
    master::ListSnapshotRestorationsResponsePB resp;

    rpc::RpcController controller;
    controller.set_timeout(60s);
    req.set_restoration_id(restoration_id.data(), restoration_id.size());
    RETURN_NOT_OK(MakeBackupServiceProxy().ListSnapshotRestorations(req, &resp, &controller));
    LOG(INFO) << "Restoration: " << resp.ShortDebugString();
    if (resp.has_status()) {
      return StatusFromPB(resp.status());
    }
    if (resp.restorations().size() != 1) {
      return STATUS_FORMAT(RuntimeError, "Wrong number of restorations, one expected but $0 found",
                           resp.restorations().size());
    }
    return resp.restorations(0).entry().state() == SysSnapshotEntryPB::RESTORED;
  }

  CHECKED_STATUS RestoreSnapshot(const TxnSnapshotId& snapshot_id) {
    auto restoration_id = VERIFY_RESULT(StartRestoration(snapshot_id));

    return WaitFor([this, &restoration_id] {
      return IsRestorationDone(restoration_id);
    }, kWaitTimeout * kTimeMultiplier, "Restoration done");
  }

  Result<Snapshots> ListSnapshots(
      const TxnSnapshotId& snapshot_id = TxnSnapshotId::Nil()) {
    master::ListSnapshotsRequestPB req;
    master::ListSnapshotsResponsePB resp;

    req.set_list_deleted_snapshots(true);
    if (!snapshot_id.IsNil()) {
      req.set_snapshot_id(snapshot_id.data(), snapshot_id.size());
    }

    rpc::RpcController controller;
    controller.set_timeout(60s);
    RETURN_NOT_OK(MakeBackupServiceProxy().ListSnapshots(req, &resp, &controller));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status());
    }
    LOG(INFO) << "Snapshots: " << resp.ShortDebugString();
    return std::move(resp.snapshots());
  }

  CHECKED_STATUS VerifySnapshot(
      const TxnSnapshotId& snapshot_id, SysSnapshotEntryPB::State state) {
    auto snapshots = VERIFY_RESULT(ListSnapshots());
    SCHECK_EQ(snapshots.size(), 1, IllegalState, "Wrong number of snapshots");
    const auto& snapshot = snapshots[0];
    auto listed_snapshot_id = VERIFY_RESULT(FullyDecodeTxnSnapshotId(snapshot.id()));
    if (listed_snapshot_id != snapshot_id) {
      return STATUS_FORMAT(
          IllegalState, "Wrong snapshot id returned $0, expected $1", listed_snapshot_id,
          snapshot_id);
    }
    if (snapshot.entry().state() != state) {
      return STATUS_FORMAT(
          IllegalState, "Wrong snapshot state: $0 vs $1",
          SysSnapshotEntryPB::State_Name(snapshot.entry().state()),
          SysSnapshotEntryPB::State_Name(state));
    }
    size_t num_namespaces = 0, num_tables = 0, num_tablets = 0;
    for (const auto& entry : snapshot.entry().entries()) {
      switch (entry.type()) {
        case master::SysRowEntry::TABLET:
          ++num_tablets;
          break;
        case master::SysRowEntry::TABLE:
          ++num_tables;
          break;
        case master::SysRowEntry::NAMESPACE:
          ++num_namespaces;
          break;
        default:
          return STATUS_FORMAT(
              IllegalState, "Unexpected entry type: $0",
              master::SysRowEntry::Type_Name(entry.type()));
      }
    }
    SCHECK_EQ(num_namespaces, 1, IllegalState, "Wrong number of namespaces");
    SCHECK_EQ(num_tables, 1, IllegalState, "Wrong number of tables");
    SCHECK_EQ(num_tablets, table_.table()->GetPartitionCount(), IllegalState,
              "Wrong number of tablets");

    return Status::OK();
  }

  Result<TxnSnapshotId> CreateSnapshot() {
    TxnSnapshotId snapshot_id = VERIFY_RESULT(StartSnapshot());
    RETURN_NOT_OK(WaitSnapshotDone(snapshot_id));
    return snapshot_id;
  }

  CHECKED_STATUS WaitSnapshotInState(
      const TxnSnapshotId& snapshot_id, SysSnapshotEntryPB::State state,
      MonoDelta duration = kWaitTimeout) {
    auto state_name = SysSnapshotEntryPB::State_Name(state);
    SysSnapshotEntryPB::State last_state = SysSnapshotEntryPB::UNKNOWN;
    auto status = WaitFor([this, &snapshot_id, state, &last_state]() -> Result<bool> {
      last_state = VERIFY_RESULT(SnapshotState(snapshot_id));
      return last_state == state;
    }, duration * kTimeMultiplier, "Snapshot in state " + state_name);

    if (!status.ok() && status.IsTimedOut()) {
      return STATUS_FORMAT(
        IllegalState, "Wrong snapshot state: $0, while $1 expected",
        SysSnapshotEntryPB::State_Name(last_state), state_name);
    }
    return status;
  }

  CHECKED_STATUS WaitSnapshotDone(
      const TxnSnapshotId& snapshot_id, MonoDelta duration = kWaitTimeout) {
    return WaitSnapshotInState(snapshot_id, SysSnapshotEntryPB::COMPLETE, duration);
  }

  CHECKED_STATUS DeleteSnapshot(const TxnSnapshotId& snapshot_id) {
    master::DeleteSnapshotRequestPB req;
    master::DeleteSnapshotResponsePB resp;

    rpc::RpcController controller;
    controller.set_timeout(60s);
    req.set_snapshot_id(snapshot_id.data(), snapshot_id.size());
    RETURN_NOT_OK(MakeBackupServiceProxy().DeleteSnapshot(req, &resp, &controller));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status());
    }
    return Status::OK();
  }

  CHECKED_STATUS WaitAllSnapshotsDeleted() {
    RETURN_NOT_OK(WaitFor([this]() -> Result<bool> {
      auto snapshots = VERIFY_RESULT(ListSnapshots());
      SCHECK_EQ(snapshots.size(), 1, IllegalState, "Wrong number of snapshots");
      if (snapshots[0].entry().state(), SysSnapshotEntryPB::DELETED) {
        return true;
      }
      SCHECK_EQ(snapshots[0].entry().state(), SysSnapshotEntryPB::DELETING, IllegalState,
                "Wrong snapshot state");
      return false;
    }, kWaitTimeout * kTimeMultiplier, "Complete delete snapshot"));

    return WaitFor([this]() -> Result<bool> {
      auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
      for (const auto& peer : peers) {
        auto db = peer->tablet()->doc_db().regular;
        if (!db) {
          continue;
        }
        auto dir = tablet::TabletSnapshots::SnapshotsDirName(db->GetName());
        auto children = VERIFY_RESULT(Env::Default()->GetChildren(dir, ExcludeDots::kTrue));
        if (!children.empty()) {
          LOG(INFO) << peer->LogPrefix() << "Children: " << AsString(children);
          return false;
        }
      }
      return true;
    }, kWaitTimeout * kTimeMultiplier, "Delete on tablets");
  }

  CHECKED_STATUS WaitAllSnapshotsCleaned() {
    return WaitFor([this]() -> Result<bool> {
      return VERIFY_RESULT(ListSnapshots()).empty();
    }, kWaitTimeout * kTimeMultiplier, "Snapshot cleanup");
  }

  Result<ImportedSnapshotData> StartImportSnapshot(const master::SnapshotInfoPB& snapshot) {
    master::ImportSnapshotMetaRequestPB req;
    master::ImportSnapshotMetaResponsePB resp;
    rpc::RpcController controller;
    controller.set_timeout(60s);

    *req.mutable_snapshot() = snapshot;

    RETURN_NOT_OK(MakeBackupServiceProxy().ImportSnapshotMeta(req, &resp, &controller));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status());
    }

    LOG(INFO) << "Imported snapshot metadata: " << resp.DebugString();

    return resp.tables_meta();
  }

  Result<bool> IsSnapshotImportDone(const ImportedSnapshotData& data) {
    for (const auto& table : data) {
      RETURN_NOT_OK(client_->OpenTable(table.table_ids().new_id()));
    }

    return true;
  }

  void TestDeleteTable(bool restart_masters);
};

TEST_F(BackupTxnTest, Simple) {
  SetAtomicFlag(
      std::chrono::duration_cast<std::chrono::microseconds>(1s).count() * kTimeMultiplier,
      &FLAGS_max_clock_skew_usec);
  ASSERT_NO_FATALS(WriteData());

  TxnSnapshotId snapshot_id = ASSERT_RESULT(StartSnapshot());

  bool has_pending = false;
  ASSERT_OK(WaitFor([this, &snapshot_id, &has_pending]() -> Result<bool> {
    if (!VERIFY_RESULT(IsSnapshotDone(snapshot_id))) {
      has_pending = true;
      return false;
    }
    return true;
  }, 10s, "Snapshot done"));

  ASSERT_TRUE(has_pending);

  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::COMPLETE));

  ASSERT_NO_FATALS(WriteData(WriteOpType::UPDATE));
  ASSERT_NO_FATALS(VerifyData(1, WriteOpType::UPDATE));

  ASSERT_OK(RestoreSnapshot(snapshot_id));

  ASSERT_NO_FATALS(VerifyData(/* num_transactions=*/ 1, WriteOpType::INSERT));
}

TEST_F(BackupTxnTest, Persistence) {
  LOG(INFO) << "Write data";

  ASSERT_NO_FATALS(WriteData());

  LOG(INFO) << "Create snapshot";

  auto snapshot_id = ASSERT_RESULT(CreateSnapshot());

  LOG(INFO) << "First restart";

  ASSERT_OK(cluster_->leader_mini_master()->Restart());
  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::COMPLETE));

  LOG(INFO) << "Create namespace";

  // Create namespace and flush, to avoid replaying logs in the master tablet containing the
  // CREATE_ON_MASTER operation for the snapshot.
  ASSERT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name() + "_Test",
                                                kTableName.namespace_type()));

  LOG(INFO) << "Flush";

  auto catalog_manager = cluster_->leader_mini_master()->master()->catalog_manager();
  tablet::TabletPeerPtr tablet_peer;
  ASSERT_OK(catalog_manager->GetTabletPeer(master::kSysCatalogTabletId, &tablet_peer));
  ASSERT_OK(tablet_peer->tablet()->Flush(tablet::FlushMode::kSync));

  LOG(INFO) << "Second restart";

  ASSERT_OK(cluster_->leader_mini_master()->Restart());

  LOG(INFO) << "Verify";

  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::COMPLETE));
}

TEST_F(BackupTxnTest, Delete) {
  ASSERT_NO_FATALS(WriteData());
  auto snapshot_id = ASSERT_RESULT(CreateSnapshot());
  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::COMPLETE));
  ASSERT_OK(DeleteSnapshot(snapshot_id));
  ASSERT_OK(WaitAllSnapshotsDeleted());

  SetAtomicFlag(1000, &FLAGS_snapshot_coordinator_cleanup_delay_ms);

  ASSERT_OK(WaitAllSnapshotsCleaned());
}

TEST_F(BackupTxnTest, CleanupAfterRestart) {
  SetAtomicFlag(300000, &FLAGS_snapshot_coordinator_cleanup_delay_ms);

  ASSERT_NO_FATALS(WriteData());
  auto snapshot_id = ASSERT_RESULT(CreateSnapshot());
  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::COMPLETE));
  ASSERT_OK(DeleteSnapshot(snapshot_id));
  ASSERT_OK(WaitAllSnapshotsDeleted());

  ASSERT_FALSE(ASSERT_RESULT(ListSnapshots()).empty());

  SetAtomicFlag(1000, &FLAGS_snapshot_coordinator_cleanup_delay_ms);
  ASSERT_OK(cluster_->leader_mini_master()->Restart());

  ASSERT_OK(WaitAllSnapshotsCleaned());
}

TEST_F(BackupTxnTest, ImportMeta) {
  ASSERT_NO_FATALS(WriteData());
  auto snapshot_id = ASSERT_RESULT(CreateSnapshot());
  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::COMPLETE));

  ASSERT_OK(client_->DeleteTable(kTableName));
  ASSERT_OK(client_->DeleteNamespace(kTableName.namespace_name()));

  auto snapshots = ASSERT_RESULT(ListSnapshots());
  ASSERT_EQ(snapshots.size(), 1);

  auto import_data = ASSERT_RESULT(StartImportSnapshot(snapshots[0]));

  ASSERT_OK(WaitFor([this, import_data] {
    return IsSnapshotImportDone(import_data);
  }, kWaitTimeout * kTimeMultiplier, "Complete import snapshot"));

  ASSERT_OK(table_.Open(kTableName, client_.get()));

  ASSERT_NO_FATALS(WriteData());
}

TEST_F(BackupTxnTest, Retry) {
  FLAGS_unresponsive_ts_rpc_timeout_ms = 1000;
  FLAGS_snapshot_coordinator_poll_interval_ms = 1000;

  ASSERT_NO_FATALS(WriteData());

  ShutdownAllTServers(cluster_.get());

  TxnSnapshotId snapshot_id = ASSERT_RESULT(StartSnapshot());

  std::this_thread::sleep_for(FLAGS_unresponsive_ts_rpc_timeout_ms * 1ms + 1s);

  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::CREATING));

  ASSERT_OK(StartAllTServers(cluster_.get()));

  ASSERT_OK(WaitSnapshotDone(snapshot_id, 15s));

  ASSERT_NO_FATALS(VerifyData());

  ASSERT_NO_FATALS(WriteData(WriteOpType::UPDATE));
  ASSERT_NO_FATALS(VerifyData(WriteOpType::UPDATE));

  ASSERT_OK(RestoreSnapshot(snapshot_id));

  ASSERT_NO_FATALS(VerifyData());
}

TEST_F(BackupTxnTest, Failure) {
  FLAGS_timestamp_history_retention_interval_sec = 0;
  FLAGS_history_cutoff_propagation_interval_ms = 1;

  ASSERT_NO_FATALS(WriteData());

  ShutdownAllTServers(cluster_.get());

  TxnSnapshotId snapshot_id = ASSERT_RESULT(StartSnapshot());

  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::CREATING));

  ShutdownAllMasters(cluster_.get());

  ASSERT_OK(StartAllTServers(cluster_.get()));

  // Wait 2 rounds to be sure that very recent history cutoff committed.
  std::this_thread::sleep_for(FLAGS_raft_heartbeat_interval_ms * 2ms * kTimeMultiplier);

  ASSERT_OK(StartAllMasters(cluster_.get()));

  ASSERT_OK(WaitSnapshotInState(snapshot_id, SysSnapshotEntryPB::FAILED, 30s));
}

TEST_F(BackupTxnTest, Restart) {
  FLAGS_timestamp_history_retention_interval_sec =
      std::chrono::duration_cast<std::chrono::seconds>(kWaitTimeout).count() *
      kTimeMultiplier;
  FLAGS_history_cutoff_propagation_interval_ms = 1;
  FLAGS_flush_rocksdb_on_shutdown = false;

  ASSERT_NO_FATALS(WriteData());
  auto snapshot_id = ASSERT_RESULT(CreateSnapshot());

  ShutdownAllMasters(cluster_.get());

  // Wait 2 rounds to be sure that very recent history cutoff committed.
  std::this_thread::sleep_for((FLAGS_timestamp_history_retention_interval_sec + 1) * 1s);

  ASSERT_OK(StartAllMasters(cluster_.get()));

  ASSERT_OK(WaitSnapshotInState(snapshot_id, SysSnapshotEntryPB::COMPLETE, 1s));
}

TEST_F(BackupTxnTest, CompleteAndBounceMaster) {
  ASSERT_NO_FATALS(WriteData());
  auto snapshot_id = ASSERT_RESULT(CreateSnapshot());

  std::this_thread::sleep_for(1s);

  ASSERT_OK(client_->DeleteTable(kTableName));

  auto leader = cluster_->leader_mini_master();
  leader->Shutdown();

  ASSERT_OK(WaitSnapshotInState(snapshot_id, SysSnapshotEntryPB::COMPLETE, 1s));

  ASSERT_OK(leader->Start());
}

TEST_F(BackupTxnTest, FlushSysCatalogAndDelete) {
  ASSERT_NO_FATALS(WriteData());
  auto snapshot_id = ASSERT_RESULT(CreateSnapshot());

  for (int i = 0; i != cluster_->num_masters(); ++i) {
    auto sys_catalog = cluster_->mini_master(i)->master()->catalog_manager()->sys_catalog();
    ASSERT_OK(sys_catalog->tablet_peer()->tablet()->Flush(tablet::FlushMode::kSync));
  }

  ShutdownAllTServers(cluster_.get());
  ASSERT_OK(DeleteSnapshot(snapshot_id));

  FLAGS_flush_rocksdb_on_shutdown = false;
  ShutdownAllMasters(cluster_.get());

  LOG(INFO) << "Start masters";

  ASSERT_OK(StartAllMasters(cluster_.get()));
  ASSERT_OK(StartAllTServers(cluster_.get()));

  ASSERT_OK(WaitSnapshotInState(snapshot_id, SysSnapshotEntryPB::DELETED, 30s));
}

// Workload writes same value across all keys in a txn, using sevaral txns in concurrently.
// Checks that after restore all keys/tablets report same value.
TEST_F(BackupTxnTest, Consistency) {
  constexpr int kThreads = 5;
  constexpr int kKeys = 10;

  FLAGS_TEST_inject_status_resolver_complete_delay_ms = 100;

  TestThreadHolder thread_holder;
  std::atomic<int> value(0);

  for (int i = 0; i != kThreads; ++i) {
    thread_holder.AddThreadFunctor([this, &stop = thread_holder.stop_flag(), &value] {
      auto session = CreateSession();
      while (!stop.load(std::memory_order_acquire)) {
        auto txn = CreateTransaction();
        session->SetTransaction(txn);
        auto v = value.fetch_add(1, std::memory_order_acq_rel);
        for (int j = 0; j != kKeys; ++j) {
          ASSERT_OK(WriteRow(session, j, v, WriteOpType::INSERT, Flush::kFalse));
        }
        auto status = session->FlushFuture().get();
        if (status.ok()) {
          status = txn->CommitFuture().get();
        }
        if (!status.ok()) {
          TransactionError txn_error(status);
          ASSERT_TRUE(txn_error == TransactionErrorCode::kConflict ||
                      txn_error == TransactionErrorCode::kAborted) << status;
        } else {
          LOG(INFO) << "Committed: " << txn->id() << ", written: " << v;
        }
      }
    });
  }

  while (value.load(std::memory_order_acquire) < 100) {
    std::this_thread::sleep_for(5ms);
  }

  auto snapshot_id = ASSERT_RESULT(CreateSnapshot());

  thread_holder.Stop();

  ASSERT_OK(RestoreSnapshot(snapshot_id));

  auto session = CreateSession();
  int restored_value = -1;
  for (int j = 0; j != kKeys; ++j) {
    auto current_value = ASSERT_RESULT(SelectRow(session, j));
    LOG(INFO) << "Key: " << j << ", value: " << current_value;
    if (restored_value == -1) {
      restored_value = current_value;
    } else {
      ASSERT_EQ(restored_value, current_value);
    }
  }

  LOG(INFO) << "Value: " << restored_value;
}

void BackupTxnTest::TestDeleteTable(bool restart_masters) {
  FLAGS_unresponsive_ts_rpc_timeout_ms = 1000;
  FLAGS_snapshot_coordinator_poll_interval_ms = 2500 * kTimeMultiplier;

  ASSERT_NO_FATALS(WriteData());

  ShutdownAllTServers(cluster_.get());

  TxnSnapshotId snapshot_id = ASSERT_RESULT(StartSnapshot());

  std::this_thread::sleep_for(FLAGS_unresponsive_ts_rpc_timeout_ms * 1ms + 1s);
  ASSERT_OK(VerifySnapshot(snapshot_id, SysSnapshotEntryPB::CREATING));

  ASSERT_OK(client_->DeleteTable(kTableName, false));

  if (restart_masters) {
    ShutdownAllMasters(cluster_.get());
  }

  ASSERT_OK(StartAllTServers(cluster_.get()));

  if (restart_masters) {
    ASSERT_OK(StartAllMasters(cluster_.get()));
  }

  ASSERT_OK(WaitSnapshotInState(snapshot_id, SysSnapshotEntryPB::FAILED, 5s * kTimeMultiplier));
}

TEST_F(BackupTxnTest, DeleteTable) {
  TestDeleteTable(/* restart_masters= */ false);
}

TEST_F(BackupTxnTest, DeleteTableWithMastersRestart) {
  TestDeleteTable(/* restart_masters= */ true);
}

} // namespace client
} // namespace yb
