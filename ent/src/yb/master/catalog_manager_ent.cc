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

#include <memory>
#include <set>
#include <unordered_set>
#include <google/protobuf/util/message_differencer.h>

#include "yb/master/catalog_manager.h"
#include "yb/master/catalog_manager-internal.h"
#include "yb/master/catalog_entity_info.h"
#include "yb/master/cdc_rpc_tasks.h"
#include "yb/master/cluster_balance.h"

#include "yb/cdc/cdc_service.h"
#include "yb/client/schema.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_handle.h"
#include "yb/client/table_alterer.h"
#include "yb/client/yb_op.h"
#include "yb/common/common.pb.h"
#include "yb/common/entity_ids.h"
#include "yb/common/ql_name.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/gutil/bind.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/master_defaults.h"
#include "yb/master/master_util.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/sys_catalog-internal.h"
#include "yb/master/async_snapshot_tasks.h"
#include "yb/master/async_rpc_tasks.h"
#include "yb/master/encryption_manager.h"

#include "yb/rpc/messenger.h"

#include "yb/tablet/operations/snapshot_operation.h"

#include "yb/tserver/backup.proxy.h"
#include "yb/tserver/service_util.h"

#include "yb/util/cast.h"
#include "yb/util/flag_tags.h"
#include "yb/util/scope_exit.h"
#include "yb/util/service_util.h"
#include "yb/util/status.h"
#include "yb/util/tostring.h"
#include "yb/util/string_util.h"
#include "yb/util/random_util.h"
#include "yb/cdc/cdc_consumer.pb.h"

using namespace std::literals;
using namespace std::placeholders;

using std::string;
using std::unique_ptr;
using std::vector;

using google::protobuf::RepeatedPtrField;
using google::protobuf::util::MessageDifferencer;
using strings::Substitute;

DEFINE_uint64(cdc_state_table_num_tablets, 0,
    "Number of tablets to use when creating the CDC state table."
    "0 to use the same default num tablets as for regular tables.");

DEFINE_int32(cdc_wal_retention_time_secs, 4 * 3600,
             "WAL retention time in seconds to be used for tables for which a CDC stream was "
             "created.");
DECLARE_int32(master_rpc_timeout_ms);

DEFINE_bool(enable_transaction_snapshots, true,
            "The flag enables usage of transaction aware snapshots.");
TAG_FLAG(enable_transaction_snapshots, hidden);
TAG_FLAG(enable_transaction_snapshots, advanced);
TAG_FLAG(enable_transaction_snapshots, runtime);

namespace yb {

using rpc::RpcContext;
using pb_util::ParseFromSlice;

namespace master {
namespace enterprise {

////////////////////////////////////////////////////////////
// Snapshot Loader
////////////////////////////////////////////////////////////

class SnapshotLoader : public Visitor<PersistentSnapshotInfo> {
 public:
  explicit SnapshotLoader(CatalogManager* catalog_manager) : catalog_manager_(catalog_manager) {}

  CHECKED_STATUS Visit(const SnapshotId& snapshot_id, const SysSnapshotEntryPB& metadata) override {
    if (TryFullyDecodeTxnSnapshotId(snapshot_id)) {
      // Transaction aware snapshots should be already loaded.
      return Status::OK();
    }
    return VisitNonTransactionAwareSnapshot(snapshot_id, metadata);
  }

  CHECKED_STATUS VisitNonTransactionAwareSnapshot(
      const SnapshotId& snapshot_id, const SysSnapshotEntryPB& metadata) {

    // Setup the snapshot info.
    auto snapshot_info = make_scoped_refptr<SnapshotInfo>(snapshot_id);
    auto l = snapshot_info->LockForWrite();
    l->mutable_data()->pb.CopyFrom(metadata);

    // Add the snapshot to the IDs map (if the snapshot is not deleted).
    auto emplace_result = catalog_manager_->non_txn_snapshot_ids_map_.emplace(
        snapshot_id, std::move(snapshot_info));
    CHECK(emplace_result.second) << "Snapshot already exists: " << snapshot_id;

    LOG(INFO) << "Loaded metadata for snapshot (id=" << snapshot_id << "): "
              << emplace_result.first->second->ToString() << ": " << metadata.ShortDebugString();
    l->Commit();
    return Status::OK();
  }

 private:
  CatalogManager *catalog_manager_;

  DISALLOW_COPY_AND_ASSIGN(SnapshotLoader);
};


////////////////////////////////////////////////////////////
// CDC Stream Loader
////////////////////////////////////////////////////////////

class CDCStreamLoader : public Visitor<PersistentCDCStreamInfo> {
 public:
  explicit CDCStreamLoader(CatalogManager* catalog_manager) : catalog_manager_(catalog_manager) {}

  Status Visit(const CDCStreamId& stream_id, const SysCDCStreamEntryPB& metadata)
      REQUIRES(catalog_manager_->lock_) {
    DCHECK(!ContainsKey(catalog_manager_->cdc_stream_map_, stream_id))
        << "CDC stream already exists: " << stream_id;

    scoped_refptr<TableInfo> table =
        FindPtrOrNull(*catalog_manager_->table_ids_map_, metadata.table_id());

    if (!table) {
      LOG(ERROR) << "Invalid table ID " << metadata.table_id() << " for stream " << stream_id;
      // TODO (#2059): Potentially signals a race condition that table got deleted while stream was
      // being created.
      // Log error and continue without loading the stream.
      return Status::OK();
    }

    // Setup the CDC stream info.
    auto stream = make_scoped_refptr<CDCStreamInfo>(stream_id);
    auto l = stream->LockForWrite();
    l->mutable_data()->pb.CopyFrom(metadata);

    // If the table has been deleted, then mark this stream as DELETING so it can be deleted by the
    // catalog manager background thread.
    if (table->LockForRead()->data().is_deleting() && !l->data().is_deleting()) {
      l->mutable_data()->pb.set_state(SysCDCStreamEntryPB::DELETING);
    }

    // Add the CDC stream to the CDC stream map.
    catalog_manager_->cdc_stream_map_[stream->id()] = stream;

    l->Commit();

    LOG(INFO) << "Loaded metadata for CDC stream " << stream->ToString() << ": "
              << metadata.ShortDebugString();

    return Status::OK();
  }

 private:
  CatalogManager *catalog_manager_;

  DISALLOW_COPY_AND_ASSIGN(CDCStreamLoader);
};

////////////////////////////////////////////////////////////
// Universe Replication Loader
////////////////////////////////////////////////////////////

class UniverseReplicationLoader : public Visitor<PersistentUniverseReplicationInfo> {
 public:
  explicit UniverseReplicationLoader(CatalogManager* catalog_manager)
      : catalog_manager_(catalog_manager) {}

  Status Visit(const std::string& producer_id, const SysUniverseReplicationEntryPB& metadata) {
    DCHECK(!ContainsKey(catalog_manager_->universe_replication_map_, producer_id))
        << "Producer universe already exists: " << producer_id;

    // Setup the universe replication info.
    UniverseReplicationInfo* const ri = new UniverseReplicationInfo(producer_id);
    auto l = ri->LockForWrite();
    l->mutable_data()->pb.CopyFrom(metadata);

    if (!l->data().is_active() && !l->data().is_deleted_or_failed()) {
      // Replication was not fully setup.
      LOG(WARNING) << "Universe replication in transient state: " << producer_id;

      // TODO: Should we delete all failed universe replication items?
    }

    // Add universe replication info to the universe replication map.
    catalog_manager_->universe_replication_map_[ri->id()] = ri;
    l->Commit();

    LOG(INFO) << "Loaded metadata for universe replication " << ri->ToString();
    VLOG(1) << "Metadata for universe replication " << ri->ToString() << ": "
            << metadata.ShortDebugString();

    return Status::OK();
  }

 private:
  CatalogManager *catalog_manager_;

  DISALLOW_COPY_AND_ASSIGN(UniverseReplicationLoader);
};

////////////////////////////////////////////////////////////
// CatalogManager
////////////////////////////////////////////////////////////

CatalogManager::~CatalogManager() {
  if (StartShutdown()) {
    CompleteShutdown();
  }
}

void CatalogManager::CompleteShutdown() {
  snapshot_coordinator_.Shutdown();
  // Call shutdown on base class before exiting derived class destructor
  // because BgTasks is part of base & uses this derived class on Shutdown.
  super::CompleteShutdown();
}

Status CatalogManager::RunLoaders(int64_t term) {
  RETURN_NOT_OK(super::RunLoaders(term));

  // Clear the snapshots.
  non_txn_snapshot_ids_map_.clear();

  // Clear CDC stream map.
  cdc_stream_map_.clear();

  // Clear universe replication map.
  universe_replication_map_.clear();

  LOG(INFO) << __func__ << ": Loading snapshots into memory.";
  unique_ptr<SnapshotLoader> snapshot_loader(new SnapshotLoader(this));
  RETURN_NOT_OK_PREPEND(
      sys_catalog_->Visit(snapshot_loader.get()),
      "Failed while visiting snapshots in sys catalog");

  LOG(INFO) << __func__ << ": Loading CDC streams into memory.";
  auto cdc_stream_loader = std::make_unique<CDCStreamLoader>(this);
  RETURN_NOT_OK_PREPEND(
      sys_catalog_->Visit(cdc_stream_loader.get()),
      "Failed while visiting CDC streams in sys catalog");

  LOG(INFO) << __func__ << ": Loading universe replication info into memory.";
  auto universe_replication_loader = std::make_unique<UniverseReplicationLoader>(this);
  RETURN_NOT_OK_PREPEND(
      sys_catalog_->Visit(universe_replication_loader.get()),
      "Failed while visiting universe replication info in sys catalog");

  return Status::OK();
}

Status CatalogManager::CreateSnapshot(const CreateSnapshotRequestPB* req,
                                      CreateSnapshotResponsePB* resp,
                                      RpcContext* rpc) {
  LOG(INFO) << "Servicing CreateSnapshot request: " << req->ShortDebugString();
  RETURN_NOT_OK(CheckOnline());

  if (FLAGS_enable_transaction_snapshots && req->transaction_aware()) {
    return CreateTransactionAwareSnapshot(*req, resp, rpc);
  }

  return CreateNonTransactionAwareSnapshot(req, resp, rpc);
}

Status CatalogManager::CreateNonTransactionAwareSnapshot(
    const CreateSnapshotRequestPB* req,
    CreateSnapshotResponsePB* resp,
    RpcContext* rpc) {
  SnapshotId snapshot_id;
  {
    std::lock_guard<LockType> l(lock_);
    TRACE("Acquired catalog manager lock");

    // Verify that the system is not in snapshot creating/restoring state.
    if (!current_snapshot_id_.empty()) {
      return STATUS(IllegalState,
                    Format(
                        "Current snapshot id: $0. Parallel snapshot operations are not supported"
                        ": $1", current_snapshot_id_, req),
                    MasterError(MasterErrorPB::PARALLEL_SNAPSHOT_OPERATION));
    }

    // Create a new snapshot UUID.
    snapshot_id = GenerateIdUnlocked(SysRowEntry::SNAPSHOT);
  }

  vector<scoped_refptr<TabletInfo>> all_tablets;

  // Create in memory snapshot data descriptor.
  scoped_refptr<SnapshotInfo> snapshot(new SnapshotInfo(snapshot_id));
  snapshot->mutable_metadata()->StartMutation();
  snapshot->mutable_metadata()->mutable_dirty()->pb.set_state(SysSnapshotEntryPB::CREATING);

  auto tables = VERIFY_RESULT(CollectTables(req->tables(),
                                            req->add_indexes(),
                                            true /* include_parent_colocated_table */));
  for (const auto& table : tables) {
    RETURN_NOT_OK(snapshot->AddEntries(table));
    all_tablets.insert(all_tablets.end(), table.tablet_infos.begin(), table.tablet_infos.end());
  }

  VLOG(1) << "Snapshot " << snapshot->ToString()
          << ": PB=" << snapshot->mutable_metadata()->mutable_dirty()->pb.DebugString();

  // Write the snapshot data descriptor to the system catalog (in "creating" state).
  RETURN_NOT_OK(CheckLeaderStatus(
      sys_catalog_->AddItem(snapshot.get(), leader_ready_term()),
      "inserting snapshot into sys-catalog"));
  TRACE("Wrote snapshot to system catalog");

  // Commit in memory snapshot data descriptor.
  snapshot->mutable_metadata()->CommitMutation();

  // Put the snapshot data descriptor to the catalog manager.
  {
    std::lock_guard<LockType> l(lock_);
    TRACE("Acquired catalog manager lock");

    // Verify that the snapshot does not exist.
    auto inserted = non_txn_snapshot_ids_map_.emplace(snapshot_id, snapshot).second;
    RSTATUS_DCHECK(inserted, IllegalState, Format("Snapshot already exists: $0", snapshot_id));
    current_snapshot_id_ = snapshot_id;
  }

  // Send CreateSnapshot requests to all TServers (one tablet - one request).
  for (const scoped_refptr<TabletInfo>& tablet : all_tablets) {
    TRACE("Locking tablet");
    auto l = tablet->LockForRead();

    LOG(INFO) << "Sending CreateTabletSnapshot to tablet: " << tablet->ToString();

    // Send Create Tablet Snapshot request to each tablet leader.
    SendCreateTabletSnapshotRequest(
        tablet, snapshot_id, HybridTime::kInvalid, TabletSnapshotOperationCallback());
  }

  resp->set_snapshot_id(snapshot_id);
  LOG(INFO) << "Successfully started snapshot " << snapshot_id << " creation";
  return Status::OK();
}

void CatalogManager::Submit(std::unique_ptr<tablet::Operation> operation) {
  operation->state()->SetTablet(tablet_peer()->tablet());
  tablet_peer()->Submit(std::move(operation), leader_ready_term());
}

Status CatalogManager::CreateTransactionAwareSnapshot(
    const CreateSnapshotRequestPB& req, CreateSnapshotResponsePB* resp, rpc::RpcContext* rpc) {
  SysRowEntries entries;
  auto tables = VERIFY_RESULT(CollectTables(req.tables(),
                                            req.add_indexes(),
                                            true /* include_parent_colocated_table */));
  for (const auto& table : tables) {
    // TODO(txn_snapshot) use single lock to resolve all tables to tablets
    SnapshotInfo::AddEntries(table, entries.mutable_entries(), /* tablet_infos= */ nullptr);
  }

  auto snapshot_id = VERIFY_RESULT(snapshot_coordinator_.Create(
      entries, req.imported(), master_->clock()->MaxGlobalNow(), rpc->GetClientDeadline()));
  resp->set_snapshot_id(snapshot_id.data(), snapshot_id.size());
  return Status::OK();
}

Status CatalogManager::ListSnapshots(const ListSnapshotsRequestPB* req,
                                     ListSnapshotsResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  auto txn_snapshot_id = TryFullyDecodeTxnSnapshotId(req->snapshot_id());
  {
    std::shared_lock<LockType> l(lock_);
    TRACE("Acquired catalog manager lock");

    if (!current_snapshot_id_.empty()) {
      resp->set_current_snapshot_id(current_snapshot_id_);
    }

    auto setup_snapshot_pb_lambda = [resp](scoped_refptr<SnapshotInfo> snapshot_info) {
      auto snapshot_lock = snapshot_info->LockForRead();

      SnapshotInfoPB* const snapshot = resp->add_snapshots();
      snapshot->set_id(snapshot_info->id());
      *snapshot->mutable_entry() = snapshot_info->metadata().state().pb;
    };

    if (req->has_snapshot_id()) {
      if (!txn_snapshot_id) {
        TRACE("Looking up snapshot");
        scoped_refptr<SnapshotInfo> snapshot_info =
            FindPtrOrNull(non_txn_snapshot_ids_map_, req->snapshot_id());
        if (snapshot_info == nullptr) {
          return STATUS(InvalidArgument, "Could not find snapshot", req->snapshot_id(),
                        MasterError(MasterErrorPB::SNAPSHOT_NOT_FOUND));
        }

        setup_snapshot_pb_lambda(snapshot_info);
      }
    } else {
      for (const SnapshotInfoMap::value_type& entry : non_txn_snapshot_ids_map_) {
        setup_snapshot_pb_lambda(entry.second);
      }
    }
  }

  return snapshot_coordinator_.ListSnapshots(
      txn_snapshot_id, req->list_deleted_snapshots(), resp);
}

Status CatalogManager::ListSnapshotRestorations(const ListSnapshotRestorationsRequestPB* req,
                                                ListSnapshotRestorationsResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  TxnSnapshotRestorationId restoration_id = TxnSnapshotRestorationId::Nil();
  if (!req->restoration_id().empty()) {
    restoration_id = VERIFY_RESULT(FullyDecodeTxnSnapshotRestorationId(req->restoration_id()));
  }
  TxnSnapshotId snapshot_id = TxnSnapshotId::Nil();
  if (!req->snapshot_id().empty()) {
    snapshot_id = VERIFY_RESULT(FullyDecodeTxnSnapshotId(req->snapshot_id()));
  }

  return snapshot_coordinator_.ListRestorations(restoration_id, snapshot_id, resp);
}

Status CatalogManager::RestoreSnapshot(const RestoreSnapshotRequestPB* req,
                                       RestoreSnapshotResponsePB* resp) {
  LOG(INFO) << "Servicing RestoreSnapshot request: " << req->ShortDebugString();
  RETURN_NOT_OK(CheckOnline());

  auto txn_snapshot_id = TryFullyDecodeTxnSnapshotId(req->snapshot_id());
  if (txn_snapshot_id) {
    TxnSnapshotRestorationId id = VERIFY_RESULT(snapshot_coordinator_.Restore(txn_snapshot_id));
    resp->set_restoration_id(id.data(), id.size());
    return Status::OK();
  }

  return RestoreNonTransactionAwareSnapshot(req->snapshot_id());
}

Status CatalogManager::RestoreNonTransactionAwareSnapshot(const string& snapshot_id) {
  std::lock_guard<LockType> l(lock_);
  TRACE("Acquired catalog manager lock");

  if (!current_snapshot_id_.empty()) {
    return STATUS(
        IllegalState,
        Format(
            "Current snapshot id: $0. Parallel snapshot operations are not supported: $1",
            current_snapshot_id_, snapshot_id),
        MasterError(MasterErrorPB::PARALLEL_SNAPSHOT_OPERATION));
  }

  TRACE("Looking up snapshot");
  scoped_refptr<SnapshotInfo> snapshot = FindPtrOrNull(non_txn_snapshot_ids_map_, snapshot_id);
  if (snapshot == nullptr) {
    return STATUS(InvalidArgument, "Could not find snapshot", snapshot_id,
                  MasterError(MasterErrorPB::SNAPSHOT_NOT_FOUND));
  }

  auto snapshot_lock = snapshot->LockForWrite();

  if (snapshot_lock->data().started_deleting()) {
    return STATUS(NotFound, "The snapshot was deleted", snapshot_id,
                  MasterError(MasterErrorPB::SNAPSHOT_NOT_FOUND));
  }

  if (!snapshot_lock->data().is_complete()) {
    return STATUS(IllegalState, "The snapshot state is not complete", snapshot_id,
                  MasterError(MasterErrorPB::SNAPSHOT_IS_NOT_READY));
  }

  TRACE("Updating snapshot metadata on disk");
  SysSnapshotEntryPB& snapshot_pb = snapshot_lock->mutable_data()->pb;
  snapshot_pb.set_state(SysSnapshotEntryPB::RESTORING);

  // Update tablet states.
  SetTabletSnapshotsState(SysSnapshotEntryPB::RESTORING, &snapshot_pb);

  // Update sys-catalog with the updated snapshot state.
  // The mutation will be aborted when 'l' exits the scope on early return.
  RETURN_NOT_OK(CheckLeaderStatus(
      sys_catalog_->UpdateItem(snapshot.get(), leader_ready_term()),
      "updating snapshot in sys-catalog"));

  // CataloManager lock 'lock_' is still locked here.
  current_snapshot_id_ = snapshot_id;

  // Restore all entries.
  for (const SysRowEntry& entry : snapshot_pb.entries()) {
    RETURN_NOT_OK(RestoreEntry(entry, snapshot_id));
  }

  // Commit in memory snapshot data descriptor.
  TRACE("Committing in-memory snapshot state");
  snapshot_lock->Commit();

  LOG(INFO) << "Successfully started snapshot " << snapshot->ToString() << " restoring";
  return Status::OK();
}

Status CatalogManager::RestoreEntry(const SysRowEntry& entry, const SnapshotId& snapshot_id) {
  switch (entry.type()) {
    case SysRowEntry::NAMESPACE: { // Restore NAMESPACES.
      TRACE("Looking up namespace");
      scoped_refptr<NamespaceInfo> ns = FindPtrOrNull(namespace_ids_map_, entry.id());
      if (ns == nullptr) {
        // Restore Namespace.
        // TODO: implement
        LOG(INFO) << "Restoring: NAMESPACE id = " << entry.id();

        return STATUS(NotSupported, Substitute(
            "Not implemented: restoring namespace: id=$0", entry.type()));
      }
      break;
    }
    case SysRowEntry::TABLE: { // Restore TABLES.
      TRACE("Looking up table");
      scoped_refptr<TableInfo> table = FindPtrOrNull(*table_ids_map_, entry.id());
      if (table == nullptr) {
        // Restore Table.
        // TODO: implement
        LOG(INFO) << "Restoring: TABLE id = " << entry.id();

        return STATUS(NotSupported, Substitute(
            "Not implemented: restoring table: id=$0", entry.type()));
      }
      break;
    }
    case SysRowEntry::TABLET: { // Restore TABLETS.
      TRACE("Looking up tablet");
      scoped_refptr<TabletInfo> tablet = FindPtrOrNull(*tablet_map_, entry.id());
      if (tablet == nullptr) {
        // Restore Tablet.
        // TODO: implement
        LOG(INFO) << "Restoring: TABLET id = " << entry.id();

        return STATUS(NotSupported, Substitute(
            "Not implemented: restoring tablet: id=$0", entry.type()));
      } else {
        TRACE("Locking tablet");
        auto l = tablet->LockForRead();

        LOG(INFO) << "Sending RestoreTabletSnapshot to tablet: " << tablet->ToString();
        // Send RestoreSnapshot requests to all TServers (one tablet - one request).
        SendRestoreTabletSnapshotRequest(tablet, snapshot_id, TabletSnapshotOperationCallback());
      }
      break;
    }
    default:
      return STATUS_FORMAT(
          InternalError, "Unexpected entry type in the snapshot: $0", entry.type());
  }

  return Status::OK();
}

Status CatalogManager::DeleteSnapshot(const DeleteSnapshotRequestPB* req,
                                      DeleteSnapshotResponsePB* resp,
                                      RpcContext* rpc) {
  LOG(INFO) << "Servicing DeleteSnapshot request: " << req->ShortDebugString();
  RETURN_NOT_OK(CheckOnline());

  auto txn_snapshot_id = TryFullyDecodeTxnSnapshotId(req->snapshot_id());
  if (txn_snapshot_id) {
    return snapshot_coordinator_.Delete(txn_snapshot_id, rpc->GetClientDeadline());
  }

  return DeleteNonTransactionAwareSnapshot(req->snapshot_id());
}

Status CatalogManager::DeleteNonTransactionAwareSnapshot(const SnapshotId& snapshot_id) {
  std::lock_guard<LockType> l(lock_);
  TRACE("Acquired catalog manager lock");

  TRACE("Looking up snapshot");
  scoped_refptr<SnapshotInfo> snapshot = FindPtrOrNull(
      non_txn_snapshot_ids_map_, snapshot_id);
  if (snapshot == nullptr) {
    return STATUS(InvalidArgument, "Could not find snapshot", snapshot_id,
                  MasterError(MasterErrorPB::SNAPSHOT_NOT_FOUND));
  }

  auto snapshot_lock = snapshot->LockForWrite();

  if (snapshot_lock->data().started_deleting()) {
    return STATUS(NotFound, "The snapshot was deleted", snapshot_id,
                  MasterError(MasterErrorPB::SNAPSHOT_NOT_FOUND));
  }

  if (snapshot_lock->data().is_restoring()) {
    return STATUS(InvalidArgument, "The snapshot is being restored now", snapshot_id,
                  MasterError(MasterErrorPB::PARALLEL_SNAPSHOT_OPERATION));
  }

  TRACE("Updating snapshot metadata on disk");
  SysSnapshotEntryPB& snapshot_pb = snapshot_lock->mutable_data()->pb;
  snapshot_pb.set_state(SysSnapshotEntryPB::DELETING);

  // Update tablet states.
  SetTabletSnapshotsState(SysSnapshotEntryPB::DELETING, &snapshot_pb);

  // Update sys-catalog with the updated snapshot state.
  // The mutation will be aborted when 'l' exits the scope on early return.
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->UpdateItem(snapshot.get(), leader_ready_term()),
      "updating snapshot in sys-catalog"));

  // Send DeleteSnapshot requests to all TServers (one tablet - one request).
  for (const SysRowEntry& entry : snapshot_pb.entries()) {
    if (entry.type() == SysRowEntry::TABLET) {
      TRACE("Looking up tablet");
      scoped_refptr<TabletInfo> tablet = FindPtrOrNull(*tablet_map_, entry.id());
      if (tablet == nullptr) {
        LOG(WARNING) << "Deleting tablet not found " << entry.id();
      } else {
        TRACE("Locking tablet");
        auto l = tablet->LockForRead();

        LOG(INFO) << "Sending DeleteTabletSnapshot to tablet: " << tablet->ToString();
        // Send DeleteSnapshot requests to all TServers (one tablet - one request).
        SendDeleteTabletSnapshotRequest(tablet, snapshot_id, TabletSnapshotOperationCallback());
      }
    }
  }

  // Commit in memory snapshot data descriptor.
  TRACE("Committing in-memory snapshot state");
  snapshot_lock->Commit();

  LOG(INFO) << "Successfully started snapshot " << snapshot->ToString() << " deletion";
  return Status::OK();
}

Status CatalogManager::ImportSnapshotPreprocess(const SysSnapshotEntryPB& snapshot_pb,
                                                ImportSnapshotMetaResponsePB* resp,
                                                NamespaceMap* namespace_map,
                                                ExternalTableSnapshotDataMap* tables_data) {
  for (const SysRowEntry& entry : snapshot_pb.entries()) {
    switch (entry.type()) {
      case SysRowEntry::NAMESPACE: // Recreate NAMESPACE.
        RETURN_NOT_OK(ImportNamespaceEntry(entry, namespace_map));
        break;
      case SysRowEntry::TABLE: { // Create TABLE metadata.
          LOG_IF(DFATAL, entry.id().empty()) << "Empty entry id";
          ExternalTableSnapshotData& data = (*tables_data)[entry.id()];

          if (data.old_table_id.empty()) {
            data.old_table_id = entry.id();
            data.table_meta = resp->mutable_tables_meta()->Add();
            data.tablet_id_map = data.table_meta->mutable_tablets_ids();
            data.table_entry_pb = VERIFY_RESULT(ParseFromSlice<SysTablesEntryPB>(entry.data()));
          } else {
            LOG(WARNING) << "Ignoring duplicate table with id " << entry.id();
          }

          LOG_IF(DFATAL, data.old_table_id.empty()) << "Not initialized table id";
        }
        break;
      case SysRowEntry::TABLET: // Preprocess original tablets.
        RETURN_NOT_OK(PreprocessTabletEntry(entry, tables_data));
        break;
      case SysRowEntry::UNKNOWN: FALLTHROUGH_INTENDED;
      case SysRowEntry::CLUSTER_CONFIG: FALLTHROUGH_INTENDED;
      case SysRowEntry::REDIS_CONFIG: FALLTHROUGH_INTENDED;
      case SysRowEntry::UDTYPE: FALLTHROUGH_INTENDED;
      case SysRowEntry::ROLE: FALLTHROUGH_INTENDED;
      case SysRowEntry::SYS_CONFIG: FALLTHROUGH_INTENDED;
      case SysRowEntry::CDC_STREAM: FALLTHROUGH_INTENDED;
      case SysRowEntry::UNIVERSE_REPLICATION: FALLTHROUGH_INTENDED;
      case SysRowEntry::SNAPSHOT:
        FATAL_INVALID_ENUM_VALUE(SysRowEntry::Type, entry.type());
    }
  }

  return Status::OK();
}

Status CatalogManager::ImportSnapshotCreateObject(const SysSnapshotEntryPB& snapshot_pb,
                                                  ImportSnapshotMetaResponsePB* resp,
                                                  NamespaceMap* namespace_map,
                                                  ExternalTableSnapshotDataMap* tables_data,
                                                  CreateObjects create_objects) {
  // Create ONLY TABLES or ONLY INDEXES in accordance to the argument.
  for (const SysRowEntry& entry : snapshot_pb.entries()) {
    if (entry.type() == SysRowEntry::TABLE) {
      ExternalTableSnapshotData& data = (*tables_data)[entry.id()];
      if ((create_objects == CreateObjects::kOnlyIndexes) == data.is_index()) {
        RETURN_NOT_OK(ImportTableEntry(*namespace_map, *tables_data, &data));
      }
    }
  }

  return Status::OK();
}

Status CatalogManager::ImportSnapshotWaitForTables(const SysSnapshotEntryPB& snapshot_pb,
                                                   ImportSnapshotMetaResponsePB* resp,
                                                   ExternalTableSnapshotDataMap* tables_data) {
  for (const SysRowEntry& entry : snapshot_pb.entries()) {
    if (entry.type() == SysRowEntry::TABLE) {
      ExternalTableSnapshotData& data = (*tables_data)[entry.id()];
      if (!data.is_index()) {
        RETURN_NOT_OK(WaitForCreateTableToFinish(data.new_table_id));
      }
    }
  }

  return Status::OK();
}

Status CatalogManager::ImportSnapshotProcessTablets(const SysSnapshotEntryPB& snapshot_pb,
                                                    ImportSnapshotMetaResponsePB* resp,
                                                    ExternalTableSnapshotDataMap* tables_data) {
  for (const SysRowEntry& entry : snapshot_pb.entries()) {
    if (entry.type() == SysRowEntry::TABLET) {
      // Create tablets IDs map.
      RETURN_NOT_OK(ImportTabletEntry(entry, tables_data));
    }
  }

  return Status::OK();
}

template <class RespClass>
void ProcessDeleteObjectStatus(const string& obj_name,
                               const string& id,
                               const RespClass& resp,
                               const Status& s) {
  Status result = s;
  if (result.ok() && resp.has_error()) {
    result = StatusFromPB(resp.error().status());
    LOG_IF(DFATAL, result.ok()) << "Expecting error status";
  }

  if (!result.ok()) {
    LOG(WARNING) << "Failed to delete new " << obj_name << " with id=" << id << ": " << result;
  }
}

void CatalogManager::DeleteNewSnapshotObjects(const NamespaceMap& namespace_map,
                                              const ExternalTableSnapshotDataMap& tables_data) {
  for (const ExternalTableSnapshotDataMap::value_type& entry : tables_data) {
    const TableId& old_id = entry.first;
    const TableId& new_id = entry.second.new_table_id;
    const TableType type = entry.second.table_entry_pb.table_type();

    // Do not delete YSQL objects - it must be deleted via PG API.
    if (new_id.empty() || new_id == old_id || type == TableType::PGSQL_TABLE_TYPE) {
      continue;
    }

    LOG(INFO) << "Deleting new table with id=" << new_id << " old id=" << old_id;
    DeleteTableRequestPB req;
    DeleteTableResponsePB resp;
    req.mutable_table()->set_table_id(new_id);
    req.set_is_index_table(entry.second.is_index());
    ProcessDeleteObjectStatus("table", new_id, resp, DeleteTable(&req, &resp, nullptr));
  }

  for (const NamespaceMap::value_type& entry : namespace_map) {
    const NamespaceId& old_id = entry.first;
    const NamespaceId& new_id = entry.second.first;
    const YQLDatabase& db_type = entry.second.second;

    // Do not delete YSQL objects - it must be deleted via PG API.
    if (new_id.empty() || new_id == old_id || db_type == YQL_DATABASE_PGSQL) {
      continue;
    }

    LOG(INFO) << "Deleting new namespace with id=" << new_id << " old id=" << old_id;
    DeleteNamespaceRequestPB req;
    DeleteNamespaceResponsePB resp;
    req.mutable_namespace_()->set_id(new_id);
    ProcessDeleteObjectStatus(
        "namespace", new_id, resp, DeleteNamespace(&req, &resp, nullptr));
  }
}

Status CatalogManager::ImportSnapshotMeta(const ImportSnapshotMetaRequestPB* req,
                                          ImportSnapshotMetaResponsePB* resp) {
  LOG(INFO) << "Servicing ImportSnapshotMeta request: " << req->ShortDebugString();
  RETURN_NOT_OK(CheckOnline());

  NamespaceMap namespace_map;
  ExternalTableSnapshotDataMap tables_data;
  bool successful_exit = false;

  auto se = ScopeExit([this, &namespace_map, &tables_data, &successful_exit] {
    if (!successful_exit) {
      DeleteNewSnapshotObjects(namespace_map, tables_data);
    }
  });

  const SysSnapshotEntryPB& snapshot_pb = req->snapshot().entry();

  // PHASE 1: Recreate namespaces, create table's meta data.
  RETURN_NOT_OK(ImportSnapshotPreprocess(snapshot_pb, resp, &namespace_map, &tables_data));

  // PHASE 2: Recreate ONLY tables.
  RETURN_NOT_OK(ImportSnapshotCreateObject(
      snapshot_pb, resp, &namespace_map, &tables_data, CreateObjects::kOnlyTables));

  // PHASE 3: Wait for all tables creation complete.
  RETURN_NOT_OK(ImportSnapshotWaitForTables(snapshot_pb, resp, &tables_data));

  // PHASE 4: Recreate ONLY indexes.
  RETURN_NOT_OK(ImportSnapshotCreateObject(
      snapshot_pb, resp, &namespace_map, &tables_data, CreateObjects::kOnlyIndexes));

  // PHASE 5: Restore tablets.
  RETURN_NOT_OK(ImportSnapshotProcessTablets(snapshot_pb, resp, &tables_data));

  successful_exit = true;
  return Status::OK();
}

Status CatalogManager::ChangeEncryptionInfo(const ChangeEncryptionInfoRequestPB* req,
                                            ChangeEncryptionInfoResponsePB* resp) {
  auto l = cluster_config_->LockForWrite();
  auto encryption_info = l->mutable_data()->pb.mutable_encryption_info();

  RETURN_NOT_OK(encryption_manager_->ChangeEncryptionInfo(req, encryption_info));

  l->mutable_data()->pb.set_version(l->mutable_data()->pb.version() + 1);
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term()),
      "updating cluster config in sys-catalog"));
  l->Commit();

  std::lock_guard<simple_spinlock> lock(should_send_universe_key_registry_mutex_);
  for (auto& entry : should_send_universe_key_registry_) {
    entry.second = true;
  }

  return Status::OK();
}

Status CatalogManager::IsEncryptionEnabled(const IsEncryptionEnabledRequestPB* req,
                                           IsEncryptionEnabledResponsePB* resp) {
  auto l = cluster_config_->LockForRead();
  const auto& encryption_info = l->data().pb.encryption_info();
  return encryption_manager_->IsEncryptionEnabled(encryption_info, resp);
}

Status CatalogManager::ImportNamespaceEntry(const SysRowEntry& entry,
                                            NamespaceMap* namespace_map) {
  LOG_IF(DFATAL, entry.type() != SysRowEntry::NAMESPACE)
      << "Unexpected entry type: " << entry.type();

  SysNamespaceEntryPB meta = VERIFY_RESULT(ParseFromSlice<SysNamespaceEntryPB>(entry.data()));
  const YQLDatabase db_type = GetDatabaseType(meta);
  NamespaceData& ns_data = (*namespace_map)[entry.id()];
  ns_data.second = db_type;

  TRACE("Looking up namespace");
  // First of all try to find the namespace by ID. It will work if we are restoring the backup
  // on the original cluster where the backup was created.
  scoped_refptr<NamespaceInfo> ns;
  {
    SharedLock<LockType> l(lock_);
    ns = FindPtrOrNull(namespace_ids_map_, entry.id());
  }

  if (ns != nullptr && ns->name() == meta.name() && ns->state() == SysNamespaceEntryPB::RUNNING) {
    ns_data.first = entry.id();
    return Status::OK();
  }

  // If the namespace was not found by ID, it's ok on a new cluster OR if the namespace was
  // deleted and created again. In both cases the namespace can be found by NAME.
  if (db_type == YQL_DATABASE_PGSQL) {
    // YSQL database must be created via external call. Find it by name.
    {
      SharedLock<LockType> l(lock_);
      ns = FindPtrOrNull(namespace_names_mapper_[db_type], meta.name());
    }

    if (ns == nullptr) {
      return STATUS(InvalidArgument, "YSQL database must exist", meta.name(),
                    MasterError(MasterErrorPB::NAMESPACE_NOT_FOUND));
    }
    if (ns->state() != SysNamespaceEntryPB::RUNNING) {
      return STATUS(InvalidArgument, "Found YSQL database must be running", meta.name(),
                    MasterError(MasterErrorPB::NAMESPACE_NOT_FOUND));
    }

    auto ns_lock = ns->LockForRead();
    ns_data.first = ns->id();
  } else {
    CreateNamespaceRequestPB req;
    CreateNamespaceResponsePB resp;
    req.set_name(meta.name());
    const Status s = CreateNamespace(&req, &resp, nullptr);

    if (!s.ok() && !s.IsAlreadyPresent()) {
      return s.CloneAndAppend("Failed to create namespace");
    }

    if (s.IsAlreadyPresent()) {
      LOG(INFO) << "Using existing namespace " << meta.name() << ": " << resp.id();
    }

    ns_data.first = resp.id();
  }
  return Status::OK();
}

Status CatalogManager::RecreateTable(const NamespaceId& new_namespace_id,
                                     const ExternalTableSnapshotDataMap& table_map,
                                     ExternalTableSnapshotData* table_data) {
  const SysTablesEntryPB& meta = DCHECK_NOTNULL(table_data)->table_entry_pb;

  CreateTableRequestPB req;
  CreateTableResponsePB resp;
  req.set_name(meta.name());
  req.set_table_type(meta.table_type());
  req.set_num_tablets(table_data->num_tablets);
  req.mutable_namespace_()->set_id(new_namespace_id);
  *req.mutable_partition_schema() = meta.partition_schema();
  *req.mutable_replication_info() = meta.replication_info();

  SchemaPB* const schema = req.mutable_schema();
  *schema = meta.schema();
  schema->mutable_table_properties()->set_num_tablets(table_data->num_tablets);

  // Setup Index info.
  if (table_data->is_index()) {
    TRACE("Looking up indexed table");
    // First of all try to attach to the new copy of the referenced table,
    // because the table restored from the snapshot is preferred.
    // For that try to map old indexed table ID into new table ID.
    ExternalTableSnapshotDataMap::const_iterator it = table_map.find(meta.indexed_table_id());
    const bool using_existing_table = (it == table_map.end());

    if (using_existing_table) {
      LOG(INFO) << "Try to use old indexed table ID " << meta.indexed_table_id();
      req.set_indexed_table_id(meta.indexed_table_id());
    } else {
      LOG(INFO) << "Found new table ID " << it->second.new_table_id << " for old table ID "
                << meta.indexed_table_id() << " from the snapshot.";
      req.set_indexed_table_id(it->second.new_table_id);
    }

    scoped_refptr<TableInfo> indexed_table;
    {
      SharedLock<LockType> l(lock_);
      // Try to find the specified indexed table by id.
      indexed_table = FindPtrOrNull(*table_ids_map_, req.indexed_table_id());
    }

    if (indexed_table == nullptr) {
      return STATUS(
          InvalidArgument, Format("Indexed table not found by id: $0", req.indexed_table_id()),
          MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }

    LOG(INFO) << "Found indexed table by ID " << req.indexed_table_id();

    // Ensure the main table schema (including column ids) was not changed.
    if (!using_existing_table) {
      Schema new_indexed_schema, src_indexed_schema;
      RETURN_NOT_OK(indexed_table->GetSchema(&new_indexed_schema));
      RETURN_NOT_OK(SchemaFromPB(it->second.table_entry_pb.schema(), &src_indexed_schema));

      if (!new_indexed_schema.Equals(src_indexed_schema)) {
        return STATUS(
            InternalError,
            Format("Recreated table has changes in schema: new schema: {$0}, source schema: {$1}",
                   new_indexed_schema.ToString(), src_indexed_schema.ToString()),
            MasterError(MasterErrorPB::SNAPSHOT_FAILED));
      }
    }

    req.set_is_local_index(meta.is_local_index());
    req.set_is_unique_index(meta.is_unique_index());
    req.set_skip_index_backfill(true);
    // Setup IndexInfoPB - self descriptor.
    IndexInfoPB* const index_info_pb = req.mutable_index_info();
    *index_info_pb = meta.index_info();
    index_info_pb->clear_table_id();
    index_info_pb->set_indexed_table_id(req.indexed_table_id());

    // Reset column ids.
    for (int i = 0; i < index_info_pb->columns_size(); ++i) {
      index_info_pb->mutable_columns(i)->clear_column_id();
    }
  }

  RETURN_NOT_OK(CreateTable(&req, &resp, /* RpcContext */nullptr));
  table_data->new_table_id = resp.table_id();
  return Status::OK();
}

Status CatalogManager::ImportTableEntry(const NamespaceMap& namespace_map,
                                        const ExternalTableSnapshotDataMap& table_map,
                                        ExternalTableSnapshotData* table_data) {
  const SysTablesEntryPB& meta = DCHECK_NOTNULL(table_data)->table_entry_pb;
  bool is_parent_colocated_table = false;

  table_data->old_namespace_id = meta.namespace_id();
  LOG_IF(DFATAL, table_data->old_namespace_id.empty()) << "No namespace id";

  LOG_IF(DFATAL, namespace_map.find(table_data->old_namespace_id) == namespace_map.end())
      << "Namespace not found: " << table_data->old_namespace_id;
  const NamespaceId new_namespace_id =
      namespace_map.find(table_data->old_namespace_id)->second.first;
  LOG_IF(DFATAL, new_namespace_id.empty()) << "No namespace id";

  Schema schema;
  RETURN_NOT_OK(SchemaFromPB(meta.schema(), &schema));
  const vector<ColumnId>& column_ids = schema.column_ids();
  scoped_refptr<TableInfo> table;

  // Create new table if namespace was changed.
  if (new_namespace_id == table_data->old_namespace_id) {
    TRACE("Looking up table");
    {
      SharedLock<LockType> l(lock_);
      table = FindPtrOrNull(*table_ids_map_, table_data->old_table_id);
    }

    // Check table is active, table name and table schema are equal to backed up ones.
    if (table != nullptr) {
      auto table_lock = table->LockForRead();
      if (table->is_running() && table->name() == meta.name()) {
        // Check the found table schema.
        Schema persisted_schema;
        RETURN_NOT_OK(table->GetSchema(&persisted_schema));
        // Schema::Equals() compares only column names & types. Check the column ids separately.
        if (!persisted_schema.Equals(schema) || persisted_schema.column_ids() != column_ids) {
          const string msg = Format("Found by id $0 $1 table $2 in namespace $3 has "
              "schema={$4}, expected={$5}", table_data->old_table_id,
              TableType_Name(meta.table_type()), meta.name(), new_namespace_id,
              persisted_schema, schema);
          LOG(WARNING) << msg;
          return STATUS(InvalidArgument, msg, MasterError(MasterErrorPB::SNAPSHOT_FAILED));
        }
      } else {
        table.reset();
      }
    }
  }

  if (table == nullptr) {
    if (meta.table_type() == TableType::PGSQL_TABLE_TYPE) {
      // YSQL table must be created via external call. Find it by name.
      // Expecting the table name is unique in the YSQL database.

      if (meta.colocated() && IsColocatedParentTableId(table_data->old_table_id)) {
        // For the parent colocated table we need to generate the new_table_id ourselves
        // since the names will not match.
        // For normal colocated tables, we are still able to follow the normal table flow, so no
        // need to generate the new_table_id ourselves.
        table_data->new_table_id = new_namespace_id + kColocatedParentTableIdSuffix;
        is_parent_colocated_table = true;
      } else {
        DCHECK(table_data->new_table_id.empty());
        SharedLock<LockType> l(lock_);

        for (const auto& entry : *table_ids_map_) {
          table = entry.second;
          auto ltm = table->LockForRead();

          if (table->is_running() &&
              new_namespace_id == table->namespace_id() &&
              meta.name() == ltm->data().name() &&
              ((table_data->is_index() && IsUserIndexUnlocked(*table)) ||
                  (!table_data->is_index() && IsUserTableUnlocked(*table)))) {
            // Found the new YSQL table by name.
            if (table_data->new_table_id.empty()) {
                table_data->new_table_id = entry.first;
            } else if (table_data->new_table_id != entry.first) {
              return STATUS(InvalidArgument,
                            Format("Found 2 YSQL tables with the same name: $0 - $1, $2",
                                  meta.name(), table_data->new_table_id, entry.first),
                            MasterError(MasterErrorPB::SNAPSHOT_FAILED));
            }
          }
        }

        if (table_data->new_table_id.empty()) {
          return STATUS(InvalidArgument, Format("YSQL table not found: $0", meta.name()),
                        MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
        }
      }
    } else {
      RETURN_NOT_OK(RecreateTable(new_namespace_id, table_map, table_data));
    }

    TRACE("Looking up new table");
    {
      SharedLock<LockType> l(lock_);
      table = FindPtrOrNull(*table_ids_map_, table_data->new_table_id);
    }

    if (table == nullptr) {
      return STATUS(InternalError, Format("Created table not found: $0", table_data->new_table_id),
                    MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }

    // Don't do schema validation/column updates on the parent colocated table.
    // However, still do the validation for regular colocated tables.
    if (!is_parent_colocated_table) {
      Schema persisted_schema;
      {
        TRACE("Locking table");
        auto table_lock = table->LockForRead();
        RETURN_NOT_OK(table->GetSchema(&persisted_schema));
      }

      // Ignore 'nullable' attribute - due to difference in implementation
      // of PgCreateTable::AddColumn() and PgAlterTable::AddColumn().
      struct CompareColumnsExceptNullable {
        bool operator ()(const ColumnSchema& a, const ColumnSchema& b) {
          return ColumnSchema::CompHashKey(a, b) && ColumnSchema::CompSortingType(a, b) &&
              ColumnSchema::CompTypeInfo(a, b) && ColumnSchema::CompName(a, b);
        }
      } comparator;
      // Schema::Equals() compares only column names & types. It does not compare the column ids.
      if (!persisted_schema.Equals(schema, comparator)
          || persisted_schema.column_ids().size() != column_ids.size()) {
        return STATUS(InternalError,
                      Format("Invalid created table schema={$0}, expected={$1}",
                            persisted_schema,
                            schema),
                      MasterError(MasterErrorPB::SNAPSHOT_FAILED));
      }

      // Update the table column ids if it's not equal to the stored ids.
      if (persisted_schema.column_ids() != column_ids) {
        if (meta.table_type() != TableType::PGSQL_TABLE_TYPE) {
          LOG(WARNING) << "Unexpected wrong column ids in " << TableType_Name(meta.table_type())
                      << " table " << meta.name() << " in namespace id " << new_namespace_id;
        }

        LOG(INFO) << "Restoring column ids in " << TableType_Name(meta.table_type()) << " table "
                  << meta.name() << " in namespace id " << new_namespace_id;
        auto l = table->LockForWrite();
        size_t col_idx = 0;
        for (auto& column : *l->mutable_data()->pb.mutable_schema()->mutable_columns()) {
          // Expecting here correct schema (columns - order, names, types), but with only wrong
          // column ids. Checking correct column order and column names below.
          if (column.name() != schema.column(col_idx).name()) {
              return STATUS(InternalError,
                            Format("Unexpected column name for index=$0: name=$1, expected name=$2",
                                  col_idx,
                                  schema.column(col_idx).name(),
                                  column.name()),
                            MasterError(MasterErrorPB::SNAPSHOT_FAILED));
          }
          // Copy the column id from imported (original) schema.
          column.set_id(column_ids[col_idx++]);
        }

        l->mutable_data()->pb.set_next_column_id(schema.max_col_id() + 1);
        l->mutable_data()->pb.set_version(l->data().pb.version() + 1);
        // Update sys-catalog with the new table schema.
        RETURN_NOT_OK(sys_catalog_->UpdateItem(table.get(), leader_ready_term()));
        l->Commit();
        // Update the new table schema in tablets.
        SendAlterTableRequest(table);
      }
    }
  } else {
    table_data->new_table_id = table_data->old_table_id;
  }

  // Set the type of the table in the response pb (default is TABLE so only set if colocated).
  if (meta.colocated()) {
    if (is_parent_colocated_table) {
      table_data->table_meta->set_table_type(
          ImportSnapshotMetaResponsePB_TableType_PARENT_COLOCATED_TABLE);
    } else {
      table_data->table_meta->set_table_type(
          ImportSnapshotMetaResponsePB_TableType_COLOCATED_TABLE);
    }
  }

  vector<scoped_refptr<TabletInfo>> new_tablets;
  {
    TRACE("Locking table");
    auto table_lock = table->LockForRead();
    table->GetAllTablets(&new_tablets);
  }

  for (const scoped_refptr<TabletInfo>& tablet : new_tablets) {
    auto tablet_lock = tablet->LockForRead();
    const PartitionPB& partition_pb = tablet->metadata().state().pb.partition();
    const ExternalTableSnapshotData::PartitionKeys key(
        partition_pb.partition_key_start(), partition_pb.partition_key_end());
    table_data->new_tablets_map[key] = tablet->id();
  }

  IdPairPB* const namespace_ids = table_data->table_meta->mutable_namespace_ids();
  namespace_ids->set_new_id(new_namespace_id);
  namespace_ids->set_old_id(table_data->old_namespace_id);

  IdPairPB* const table_ids = table_data->table_meta->mutable_table_ids();
  table_ids->set_new_id(table_data->new_table_id);
  table_ids->set_old_id(table_data->old_table_id);

  return Status::OK();
}

Status CatalogManager::PreprocessTabletEntry(const SysRowEntry& entry,
                                             ExternalTableSnapshotDataMap* table_map) {
  LOG_IF(DFATAL, entry.type() != SysRowEntry::TABLET) << "Unexpected entry type: " << entry.type();

  SysTabletsEntryPB meta = VERIFY_RESULT(ParseFromSlice<SysTabletsEntryPB>(entry.data()));

  ExternalTableSnapshotData& table_data = (*table_map)[meta.table_id()];
  ++table_data.num_tablets;
  return Status::OK();
}

Status CatalogManager::ImportTabletEntry(const SysRowEntry& entry,
                                         ExternalTableSnapshotDataMap* table_map) {
  LOG_IF(DFATAL, entry.type() != SysRowEntry::TABLET) << "Unexpected entry type: " << entry.type();

  SysTabletsEntryPB meta = VERIFY_RESULT(ParseFromSlice<SysTabletsEntryPB>(entry.data()));

  LOG_IF(DFATAL, table_map->find(meta.table_id()) == table_map->end())
      << "Table not found: " << meta.table_id();
  ExternalTableSnapshotData& table_data = (*table_map)[meta.table_id()];

  if (meta.colocated() && table_data.tablet_id_map->size() >= 1) {
    LOG(INFO) << "Already processed this colocated tablet: " << entry.id();
    return Status::OK();
  }

  // Update tablets IDs map.
  if (table_data.new_table_id == table_data.old_table_id) {
    TRACE("Looking up tablet");
    SharedLock<LockType> l(lock_);
    scoped_refptr<TabletInfo> tablet = FindPtrOrNull(*tablet_map_, entry.id());

    if (tablet != nullptr) {
      IdPairPB* const pair = table_data.tablet_id_map->Add();
      pair->set_old_id(entry.id());
      pair->set_new_id(entry.id());
      return Status::OK();
    }
  }

  const PartitionPB& partition_pb = meta.partition();
  const ExternalTableSnapshotData::PartitionKeys key(
      partition_pb.partition_key_start(), partition_pb.partition_key_end());
  const ExternalTableSnapshotData::PartitionToIdMap::const_iterator it =
      table_data.new_tablets_map.find(key);

  if (it == table_data.new_tablets_map.end()) {
    return STATUS(NotFound,
                  Format("Not found new tablet with expected partition keys: $0 - $1",
                         partition_pb.partition_key_start(),
                         partition_pb.partition_key_end()),
                  MasterError(MasterErrorPB::INTERNAL_ERROR));
  }

  IdPairPB* const pair = table_data.tablet_id_map->Add();
  pair->set_old_id(entry.id());
  pair->set_new_id(it->second);
  return Status::OK();
}

const Schema& CatalogManager::schema() {
  return sys_catalog()->schema();
}

TabletInfos CatalogManager::GetTabletInfos(const std::vector<TabletId>& ids) {
  TabletInfos result;
  result.reserve(ids.size());
  SharedLock<LockType> l(lock_);
  for (const auto& id : ids) {
    auto it = tablet_map_->find(id);
    result.push_back(it != tablet_map_->end() ? it->second : nullptr);
  }
  return result;
}

void CatalogManager::SendCreateTabletSnapshotRequest(
    const scoped_refptr<TabletInfo>& tablet, const std::string& snapshot_id,
    HybridTime snapshot_hybrid_time, TabletSnapshotOperationCallback callback) {
  auto call = std::make_shared<AsyncTabletSnapshotOp>(
      master_, AsyncTaskPool(), tablet, snapshot_id,
      tserver::TabletSnapshotOpRequestPB::CREATE_ON_TABLET);
  call->SetSnapshotHybridTime(snapshot_hybrid_time);
  call->SetCallback(std::move(callback));
  tablet->table()->AddTask(call);
  WARN_NOT_OK(ScheduleTask(call), "Failed to send create snapshot request");
}

void CatalogManager::SendRestoreTabletSnapshotRequest(
    const scoped_refptr<TabletInfo>& tablet,
    const string& snapshot_id,
    TabletSnapshotOperationCallback callback) {
  auto call = std::make_shared<AsyncTabletSnapshotOp>(
      master_, AsyncTaskPool(), tablet, snapshot_id,
      tserver::TabletSnapshotOpRequestPB::RESTORE);
  call->SetCallback(std::move(callback));
  tablet->table()->AddTask(call);
  WARN_NOT_OK(ScheduleTask(call), "Failed to send restore snapshot request");
}

void CatalogManager::SendDeleteTabletSnapshotRequest(const scoped_refptr<TabletInfo>& tablet,
                                                     const string& snapshot_id,
                                                     TabletSnapshotOperationCallback callback) {
  auto call = std::make_shared<AsyncTabletSnapshotOp>(
      master_, AsyncTaskPool(), tablet, snapshot_id,
      tserver::TabletSnapshotOpRequestPB::DELETE_ON_TABLET);
  call->SetCallback(std::move(callback));
  tablet->table()->AddTask(call);
  WARN_NOT_OK(ScheduleTask(call), "Failed to send delete snapshot request");
}

rpc::Scheduler& CatalogManager::Scheduler() {
  return master_->messenger()->scheduler();
}

bool CatalogManager::IsLeader() {
  auto peer = tablet_peer();
  if (!peer) {
    return false;
  }
  auto consensus = peer->shared_consensus();
  if (!consensus) {
    return false;
  }
  auto leader_status = consensus->GetLeaderStatus(/* allow_stale= */ true);
  return leader_status == consensus::LeaderStatus::LEADER_AND_READY;
}

void CatalogManager::HandleCreateTabletSnapshotResponse(TabletInfo *tablet, bool error) {
  LOG(INFO) << "Handling Create Tablet Snapshot Response for tablet "
            << DCHECK_NOTNULL(tablet)->ToString() << (error ? "  ERROR" : "  OK");

  // Get the snapshot data descriptor from the catalog manager.
  scoped_refptr<SnapshotInfo> snapshot;
  {
    std::lock_guard<LockType> manager_l(lock_);
    TRACE("Acquired catalog manager lock");

    if (current_snapshot_id_.empty()) {
      LOG(WARNING) << "No active snapshot: " << current_snapshot_id_;
      return;
    }

    snapshot = FindPtrOrNull(non_txn_snapshot_ids_map_, current_snapshot_id_);

    if (!snapshot) {
      LOG(WARNING) << "Snapshot not found: " << current_snapshot_id_;
      return;
    }
  }

  if (!snapshot->IsCreateInProgress()) {
    LOG(WARNING) << "Snapshot is not in creating state: " << snapshot->id();
    return;
  }

  auto tablet_l = tablet->LockForRead();
  auto l = snapshot->LockForWrite();
  RepeatedPtrField<SysSnapshotEntryPB_TabletSnapshotPB>* tablet_snapshots =
      l->mutable_data()->pb.mutable_tablet_snapshots();
  int num_tablets_complete = 0;

  for (int i = 0; i < tablet_snapshots->size(); ++i) {
    SysSnapshotEntryPB_TabletSnapshotPB* tablet_info = tablet_snapshots->Mutable(i);

    if (tablet_info->id() == tablet->id()) {
      tablet_info->set_state(error ? SysSnapshotEntryPB::FAILED : SysSnapshotEntryPB::COMPLETE);
    }

    if (tablet_info->state() == SysSnapshotEntryPB::COMPLETE) {
      ++num_tablets_complete;
    }
  }

  // Finish the snapshot.
  bool finished = true;
  if (error) {
    l->mutable_data()->pb.set_state(SysSnapshotEntryPB::FAILED);
    LOG(WARNING) << "Failed snapshot " << snapshot->id() << " on tablet " << tablet->id();
  } else if (num_tablets_complete == tablet_snapshots->size()) {
    l->mutable_data()->pb.set_state(SysSnapshotEntryPB::COMPLETE);
    LOG(INFO) << "Completed snapshot " << snapshot->id();
  } else {
    finished = false;
  }

  if (finished) {
    std::lock_guard<LockType> manager_l(lock_);
    TRACE("Acquired catalog manager lock");
    current_snapshot_id_ = "";
  }

  VLOG(1) << "Snapshot: " << snapshot->id()
          << " PB: " << l->mutable_data()->pb.DebugString()
          << " Complete " << num_tablets_complete << " tablets from " << tablet_snapshots->size();

  const Status s = sys_catalog_->UpdateItem(snapshot.get(), leader_ready_term());
  if (!s.ok()) {
    return (void)CheckStatus(s, "updating snapshot in sys-catalog");
  }

  l->Commit();
}

void CatalogManager::HandleRestoreTabletSnapshotResponse(TabletInfo *tablet, bool error) {
  LOG(INFO) << "Handling Restore Tablet Snapshot Response for tablet "
            << DCHECK_NOTNULL(tablet)->ToString() << (error ? "  ERROR" : "  OK");

  // Get the snapshot data descriptor from the catalog manager.
  scoped_refptr<SnapshotInfo> snapshot;
  {
    std::lock_guard<LockType> manager_l(lock_);
    TRACE("Acquired catalog manager lock");

    if (current_snapshot_id_.empty()) {
      LOG(WARNING) << "No restoring snapshot: " << current_snapshot_id_;
      return;
    }

    snapshot = FindPtrOrNull(non_txn_snapshot_ids_map_, current_snapshot_id_);

    if (!snapshot) {
      LOG(WARNING) << "Restoring snapshot not found: " << current_snapshot_id_;
      return;
    }
  }

  if (!snapshot->IsRestoreInProgress()) {
    LOG(WARNING) << "Snapshot is not in restoring state: " << snapshot->id();
    return;
  }

  auto tablet_l = tablet->LockForRead();
  auto l = snapshot->LockForWrite();
  RepeatedPtrField<SysSnapshotEntryPB_TabletSnapshotPB>* tablet_snapshots =
      l->mutable_data()->pb.mutable_tablet_snapshots();
  int num_tablets_complete = 0;

  for (int i = 0; i < tablet_snapshots->size(); ++i) {
    SysSnapshotEntryPB_TabletSnapshotPB* tablet_info = tablet_snapshots->Mutable(i);

    if (tablet_info->id() == tablet->id()) {
      tablet_info->set_state(error ? SysSnapshotEntryPB::FAILED : SysSnapshotEntryPB::COMPLETE);
    }

    if (tablet_info->state() == SysSnapshotEntryPB::COMPLETE) {
      ++num_tablets_complete;
    }
  }

  // Finish the snapshot.
  if (error || num_tablets_complete == tablet_snapshots->size()) {
    if (error) {
      l->mutable_data()->pb.set_state(SysSnapshotEntryPB::FAILED);
      LOG(WARNING) << "Failed restoring snapshot " << snapshot->id()
                   << " on tablet " << tablet->id();
    } else {
      LOG_IF(DFATAL, num_tablets_complete != tablet_snapshots->size())
          << "Wrong number of tablets";
      l->mutable_data()->pb.set_state(SysSnapshotEntryPB::COMPLETE);
      LOG(INFO) << "Restored snapshot " << snapshot->id();
    }

    std::lock_guard<LockType> manager_l(lock_);
    TRACE("Acquired catalog manager lock");
    current_snapshot_id_ = "";
  }

  VLOG(1) << "Snapshot: " << snapshot->id()
          << " PB: " << l->mutable_data()->pb.DebugString()
          << " Complete " << num_tablets_complete << " tablets from " << tablet_snapshots->size();

  const Status s = sys_catalog_->UpdateItem(snapshot.get(), leader_ready_term());
  if (!s.ok()) {
    return (void)CheckStatus(s, "updating snapshot in sys-catalog");
  }

  l->Commit();
}

void CatalogManager::HandleDeleteTabletSnapshotResponse(
    SnapshotId snapshot_id, TabletInfo *tablet, bool error) {
  LOG(INFO) << "Handling Delete Tablet Snapshot Response for tablet "
            << DCHECK_NOTNULL(tablet)->ToString() << (error ? "  ERROR" : "  OK");

  // Get the snapshot data descriptor from the catalog manager.
  scoped_refptr<SnapshotInfo> snapshot;
  {
    std::lock_guard<LockType> manager_l(lock_);
    TRACE("Acquired catalog manager lock");

    snapshot = FindPtrOrNull(non_txn_snapshot_ids_map_, snapshot_id);

    if (!snapshot) {
      LOG(WARNING) << "Snapshot not found: " << snapshot_id;
      return;
    }
  }

  if (!snapshot->IsDeleteInProgress()) {
    LOG(WARNING) << "Snapshot is not in deleting state: " << snapshot->id();
    return;
  }

  auto tablet_l = tablet->LockForRead();
  auto l = snapshot->LockForWrite();
  RepeatedPtrField<SysSnapshotEntryPB_TabletSnapshotPB>* tablet_snapshots =
      l->mutable_data()->pb.mutable_tablet_snapshots();
  int num_tablets_complete = 0;

  for (int i = 0; i < tablet_snapshots->size(); ++i) {
    SysSnapshotEntryPB_TabletSnapshotPB* tablet_info = tablet_snapshots->Mutable(i);

    if (tablet_info->id() == tablet->id()) {
      tablet_info->set_state(error ? SysSnapshotEntryPB::FAILED : SysSnapshotEntryPB::DELETED);
    }

    if (tablet_info->state() != SysSnapshotEntryPB::DELETING) {
      ++num_tablets_complete;
    }
  }

  if (num_tablets_complete == tablet_snapshots->size()) {
    // Delete the snapshot.
    l->mutable_data()->pb.set_state(SysSnapshotEntryPB::DELETED);
    LOG(INFO) << "Deleted snapshot " << snapshot->id();

    const Status s = sys_catalog_->DeleteItem(snapshot.get(), leader_ready_term());

    std::lock_guard<LockType> manager_l(lock_);
    TRACE("Acquired catalog manager lock");

    if (current_snapshot_id_ == snapshot_id) {
      current_snapshot_id_ = "";
    }

    // Remove it from the maps.
    TRACE("Removing from maps");
    if (non_txn_snapshot_ids_map_.erase(snapshot_id) < 1) {
      LOG(WARNING) << "Could not remove snapshot " << snapshot_id << " from map";
    }

    if (!s.ok()) {
      return (void)CheckStatus(s, "deleting snapshot from sys-catalog");
    }
  } else if (error) {
    l->mutable_data()->pb.set_state(SysSnapshotEntryPB::FAILED);
    LOG(WARNING) << "Failed snapshot " << snapshot->id() << " deletion on tablet " << tablet->id();

    const Status s = sys_catalog_->UpdateItem(snapshot.get(), leader_ready_term());
    if (!s.ok()) {
      return (void)CheckStatus(s, "updating snapshot in sys-catalog");
    }
  }

  l->Commit();

  VLOG(1) << "Deleting snapshot: " << snapshot->id()
          << " PB: " << l->mutable_data()->pb.DebugString()
          << " Complete " << num_tablets_complete << " tablets from " << tablet_snapshots->size();
}

void CatalogManager::DumpState(std::ostream* out, bool on_disk_dump) const {
  super::DumpState(out, on_disk_dump);

  // TODO: dump snapshots
}

Status CatalogManager::CheckValidReplicationInfo(const ReplicationInfoPB& replication_info,
                                                 const TSDescriptorVector& all_ts_descs,
                                                 const vector<Partition>& partitions,
                                                 CreateTableResponsePB* resp) {
  TSDescriptorVector ts_descs;
  GetTsDescsFromPlacementInfo(replication_info.live_replicas(), all_ts_descs, &ts_descs);
  RETURN_NOT_OK(super::CheckValidPlacementInfo(replication_info.live_replicas(), ts_descs,
                                               partitions, resp));
  for (int i = 0; i < replication_info.read_replicas_size(); i++) {
    GetTsDescsFromPlacementInfo(replication_info.read_replicas(i), all_ts_descs, &ts_descs);
    RETURN_NOT_OK(super::CheckValidPlacementInfo(replication_info.read_replicas(i), ts_descs,
                                                 partitions, resp));
  }
  return Status::OK();
}

Status CatalogManager::HandlePlacementUsingReplicationInfo(
    const ReplicationInfoPB& replication_info,
    const TSDescriptorVector& all_ts_descs,
    consensus::RaftConfigPB* config) {
  TSDescriptorVector ts_descs;
  GetTsDescsFromPlacementInfo(replication_info.live_replicas(), all_ts_descs, &ts_descs);
  RETURN_NOT_OK(super::HandlePlacementUsingPlacementInfo(replication_info.live_replicas(),
                                                      ts_descs,
                                                      consensus::RaftPeerPB::VOTER, config));
  for (int i = 0; i < replication_info.read_replicas_size(); i++) {
    GetTsDescsFromPlacementInfo(replication_info.read_replicas(i), all_ts_descs, &ts_descs);
    RETURN_NOT_OK(super::HandlePlacementUsingPlacementInfo(replication_info.read_replicas(i),
                                                           ts_descs,
                                                           consensus::RaftPeerPB::OBSERVER,
                                                           config));
  }
  return Status::OK();
}

void CatalogManager::GetTsDescsFromPlacementInfo(const PlacementInfoPB& placement_info,
                                                 const TSDescriptorVector& all_ts_descs,
                                                 TSDescriptorVector* ts_descs) {
  ts_descs->clear();
  for (const auto& ts_desc : all_ts_descs) {
    TSDescriptor* ts_desc_ent = down_cast<TSDescriptor*>(ts_desc.get());
    if (placement_info.has_placement_uuid()) {
      string placement_uuid = placement_info.placement_uuid();
      if (ts_desc_ent->placement_uuid() == placement_uuid) {
        ts_descs->push_back(ts_desc);
      }
    } else if (ts_desc_ent->placement_uuid() == "") {
      // Since the placement info has no placement id, we know it is live, so we add this ts.
      ts_descs->push_back(ts_desc);
    }
  }
}

template <typename Registry, typename Mutex>
bool ShouldResendRegistry(
    const std::string& ts_uuid, bool has_registration, Registry* registry, Mutex* mutex) {
  bool should_resend_registry;
  {
    std::lock_guard<Mutex> lock(*mutex);
    auto it = registry->find(ts_uuid);
    should_resend_registry = (it == registry->end() || it->second || has_registration);
    if (it == registry->end()) {
      registry->emplace(ts_uuid, false);
    } else {
      it->second = false;
    }
  }
  return should_resend_registry;
}

Status CatalogManager::FillHeartbeatResponse(const TSHeartbeatRequestPB* req,
                                             TSHeartbeatResponsePB* resp) {
  SysClusterConfigEntryPB cluster_config;
  RETURN_NOT_OK(GetClusterConfig(&cluster_config));
  RETURN_NOT_OK(FillHeartbeatResponseEncryption(cluster_config, req, resp));
  return FillHeartbeatResponseCDC(cluster_config, req, resp);
}


Status CatalogManager::FillHeartbeatResponseCDC(const SysClusterConfigEntryPB& cluster_config,
                                                const TSHeartbeatRequestPB* req,
                                                TSHeartbeatResponsePB* resp) {
  resp->set_cluster_config_version(cluster_config.version());
  if (!cluster_config.has_consumer_registry() ||
      req->cluster_config_version() >= cluster_config.version()) {
    return Status::OK();
  }
  *resp->mutable_consumer_registry() = cluster_config.consumer_registry();
  return Status::OK();
}

Status CatalogManager::FillHeartbeatResponseEncryption(
    const SysClusterConfigEntryPB& cluster_config,
    const TSHeartbeatRequestPB* req,
    TSHeartbeatResponsePB* resp) {
  const auto& ts_uuid = req->common().ts_instance().permanent_uuid();
  if (!cluster_config.has_encryption_info() ||
      !ShouldResendRegistry(ts_uuid, req->has_registration(), &should_send_universe_key_registry_,
                            &should_send_universe_key_registry_mutex_)) {
    return Status::OK();
  }

  const auto& encryption_info = cluster_config.encryption_info();
  RETURN_NOT_OK(encryption_manager_->FillHeartbeatResponseEncryption(encryption_info, resp));

  return Status::OK();
}

void CatalogManager::SetTabletSnapshotsState(SysSnapshotEntryPB::State state,
                                             SysSnapshotEntryPB* snapshot_pb) {
  RepeatedPtrField<SysSnapshotEntryPB_TabletSnapshotPB>* tablet_snapshots =
      snapshot_pb->mutable_tablet_snapshots();

  for (int i = 0; i < tablet_snapshots->size(); ++i) {
    SysSnapshotEntryPB_TabletSnapshotPB* tablet_info = tablet_snapshots->Mutable(i);
    tablet_info->set_state(state);
  }
}

Status CatalogManager::CreateCdcStateTableIfNeeded(rpc::RpcContext *rpc) {
  TableIdentifierPB table_identifier;
  table_identifier.set_table_name(kCdcStateTableName);
  table_identifier.mutable_namespace_()->set_name(kSystemNamespaceName);

  // Check that the namespace exists.
  scoped_refptr<NamespaceInfo> ns_info;
  RETURN_NOT_OK(FindNamespace(table_identifier.namespace_(), &ns_info));
  if (!ns_info) {
    return STATUS(NotFound, "Namespace does not exist", kSystemNamespaceName);
  }

  // If CDC state table exists do nothing, otherwise create it.
  scoped_refptr<TableInfo> table_info;
  RETURN_NOT_OK(FindTable(table_identifier, &table_info));

  if (!table_info) {
    // Set up a CreateTable request internally.
    CreateTableRequestPB req;
    CreateTableResponsePB resp;
    req.set_name(kCdcStateTableName);
    req.mutable_namespace_()->CopyFrom(table_identifier.namespace_());
    req.set_table_type(TableType::YQL_TABLE_TYPE);

    client::YBSchemaBuilder schema_builder;
    schema_builder.AddColumn(master::kCdcTabletId)->HashPrimaryKey()->Type(DataType::STRING);
    schema_builder.AddColumn(master::kCdcStreamId)->PrimaryKey()->Type(DataType::STRING);
    schema_builder.AddColumn(master::kCdcCheckpoint)->Type(DataType::STRING);
    schema_builder.AddColumn(master::kCdcData)->Type(QLType::CreateTypeMap(
        DataType::STRING, DataType::STRING));
    schema_builder.AddColumn(master::kCdcLastReplicationTime)->Type(DataType::TIMESTAMP);

    client::YBSchema yb_schema;
    CHECK_OK(schema_builder.Build(&yb_schema));

    auto schema = yb::client::internal::GetSchema(yb_schema);
    SchemaToPB(schema, req.mutable_schema());
    // Explicitly set the number tablets if the corresponding flag is set, otherwise CreateTable
    // will use the same defaults as for regular tables.
    if (FLAGS_cdc_state_table_num_tablets > 0) {
      req.mutable_schema()->mutable_table_properties()->set_num_tablets(
          FLAGS_cdc_state_table_num_tablets);
    }

    Status s = CreateTable(&req, &resp, rpc);
    // We do not lock here so it is technically possible that the table was already created.
    // If so, there is nothing to do so we just ignore the "AlreadyPresent" error.
    if (!s.ok() && !s.IsAlreadyPresent()) {
      return s;
    }
  }
  return Status::OK();
}

Status CatalogManager::IsCdcStateTableCreated(IsCreateTableDoneResponsePB* resp) {
  IsCreateTableDoneRequestPB req;

  req.mutable_table()->set_table_name(kCdcStateTableName);
  req.mutable_table()->mutable_namespace_()->set_name(kSystemNamespaceName);

  return IsCreateTableDone(&req, resp);
}

// Helper class to print a vector of CDCStreamInfo pointers.
namespace {
  template<class CDCStreamInfoPointer>
  std::string JoinStreamsCSVLine(std::vector<CDCStreamInfoPointer> cdc_streams) {
    std::vector<CDCStreamId> cdc_stream_ids;
    for (const auto& cdc_stream : cdc_streams) {
      cdc_stream_ids.push_back(cdc_stream->id());
    }
    return JoinCSVLine(cdc_stream_ids);
  }
} // namespace


Status CatalogManager::DeleteCDCStreamsForTable(const TableId& table_id) {
  return DeleteCDCStreamsForTables({table_id});
}

Status CatalogManager::DeleteCDCStreamsForTables(const vector<TableId>& table_ids) {
  std::ostringstream tid_stream;
  for (const auto& tid : table_ids) {
    tid_stream << " " << tid;
  }
  LOG(INFO) << "Deleting CDC streams for tables:" << tid_stream.str();

  std::vector<scoped_refptr<CDCStreamInfo>> streams;
  for (const auto& tid : table_ids) {
    auto newstreams = FindCDCStreamsForTable(tid);
    streams.insert(streams.end(), newstreams.begin(), newstreams.end());
  }

  if (streams.empty()) {
    return Status::OK();
  }

  // Do not delete them here, just mark them as DELETING and the catalog manager background thread
  // will handle the deletion.
  return MarkCDCStreamsAsDeleting(streams);
}

std::vector<scoped_refptr<CDCStreamInfo>> CatalogManager::FindCDCStreamsForTable(
    const TableId& table_id) {
  std::vector<scoped_refptr<CDCStreamInfo>> streams;
  std::shared_lock<LockType> l(lock_);

  for (const auto& entry : cdc_stream_map_) {
    auto ltm = entry.second->LockForRead();

    if (ltm->data().table_id() == table_id && !ltm->data().started_deleting()) {
      streams.push_back(entry.second);
    }
  }
  return streams;
}

void CatalogManager::GetAllCDCStreams(std::vector<scoped_refptr<CDCStreamInfo>>* streams) {
  streams->clear();
  streams->reserve(cdc_stream_map_.size());
  std::shared_lock<LockType> l(lock_);
  for (const CDCStreamInfoMap::value_type& e : cdc_stream_map_) {
    if (!e.second->LockForRead()->data().is_deleting()) {
      streams->push_back(e.second);
    }
  }
}

Status CatalogManager::CreateCDCStream(const CreateCDCStreamRequestPB* req,
                                       CreateCDCStreamResponsePB* resp,
                                       rpc::RpcContext* rpc) {
  LOG(INFO) << "CreateCDCStream from " << RequestorString(rpc)
            << ": " << req->DebugString();

  RETURN_NOT_OK(CheckOnline());

  TableIdentifierPB table_identifier;
  table_identifier.set_table_id(req->table_id());

  scoped_refptr<TableInfo> table;
  RETURN_NOT_OK(FindTable(table_identifier, &table));
  if (table == nullptr) {
    return STATUS(NotFound, "Table not found", req->table_id(),
                  MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  {
    auto l = table->LockForRead();
    if (l->data().started_deleting()) {
      return STATUS(NotFound, "Table does not exist", req->table_id(),
                    MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  AlterTableRequestPB alter_table_req;
  alter_table_req.mutable_table()->set_table_id(req->table_id());
  alter_table_req.set_wal_retention_secs(FLAGS_cdc_wal_retention_time_secs);
  AlterTableResponsePB alter_table_resp;
  Status s = this->AlterTable(&alter_table_req, &alter_table_resp, rpc);
  if (!s.ok()) {
    return STATUS(InternalError,
                  "Unable to change the WAL retention time for table", req->table_id(),
                  MasterError(MasterErrorPB::INTERNAL_ERROR));
  }

  scoped_refptr<CDCStreamInfo> stream;
  {
    TRACE("Acquired catalog manager lock");
    std::lock_guard<LockType> l(lock_);

    // Construct the CDC stream if the producer wasn't bootstrapped.
    CDCStreamId stream_id;
    stream_id = GenerateIdUnlocked(SysRowEntry::CDC_STREAM);

    stream = make_scoped_refptr<CDCStreamInfo>(stream_id);
    stream->mutable_metadata()->StartMutation();
    SysCDCStreamEntryPB *metadata = &stream->mutable_metadata()->mutable_dirty()->pb;
    metadata->set_table_id(table->id());
    metadata->mutable_options()->CopyFrom(req->options());

    // Add the stream to the in-memory map.
    cdc_stream_map_[stream->id()] = stream;
    resp->set_stream_id(stream->id());
  }
  TRACE("Inserted new CDC stream into CatalogManager maps");

  // Update the on-disk system catalog.
  RETURN_NOT_OK(CheckLeaderStatusAndSetupError(
      sys_catalog_->AddItem(stream.get(), leader_ready_term()),
      "inserting CDC stream into sys-catalog", resp));
  TRACE("Wrote CDC stream to sys-catalog");

  // Commit the in-memory state.
  stream->mutable_metadata()->CommitMutation();
  LOG(INFO) << "Created CDC stream " << stream->ToString();

  RETURN_NOT_OK(CreateCdcStateTableIfNeeded(rpc));
  return Status::OK();
}

Status CatalogManager::DeleteCDCStream(const DeleteCDCStreamRequestPB* req,
                                       DeleteCDCStreamResponsePB* resp,
                                       rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing DeleteCDCStream request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  RETURN_NOT_OK(CheckOnline());

  if (req->stream_id_size() < 1) {
    return STATUS(InvalidArgument, "No CDC Stream ID given", req->ShortDebugString(),
                  MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  std::vector<scoped_refptr<CDCStreamInfo>> streams;
  {
    std::shared_lock<LockType> l(lock_);
    for (const auto& stream_id : req->stream_id()) {
      auto stream = FindPtrOrNull(cdc_stream_map_, stream_id);

      if (stream == nullptr || stream->LockForRead()->data().is_deleting()) {
        return STATUS(NotFound, "CDC stream does not exist", req->ShortDebugString(),
                      MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
      }
      streams.push_back(stream);
    }
  }

  // Do not delete them here, just mark them as DELETING and the catalog manager background thread
  // will handle the deletion.
  Status s = MarkCDCStreamsAsDeleting(streams);
  if (!s.ok()) {
    if (s.IsIllegalState()) {
      PANIC_RPC(rpc, s.message().ToString());
    }
    return CheckIfNoLongerLeaderAndSetupError(s, resp);
  }

  LOG(INFO) << "Successfully deleted CDC streams " << JoinStreamsCSVLine(streams)
            << " per request from " << RequestorString(rpc);

  return Status::OK();
}

Status CatalogManager::MarkCDCStreamsAsDeleting(
    const std::vector<scoped_refptr<CDCStreamInfo>>& streams) {
  std::vector<std::unique_ptr<CDCStreamInfo::lock_type>> locks;
  std::vector<CDCStreamInfo*> streams_to_mark;
  locks.reserve(streams.size());
  for (auto& stream : streams) {
    auto l = stream->LockForWrite();
    l->mutable_data()->pb.set_state(SysCDCStreamEntryPB::DELETING);
    locks.push_back(std::move(l));
    streams_to_mark.push_back(stream.get());
  }
  // The mutation will be aborted when 'l' exits the scope on early return.
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->UpdateItems(streams_to_mark, leader_ready_term()),
      "updating CDC streams in sys-catalog"));
  LOG(INFO) << "Successfully marked streams " << JoinStreamsCSVLine(streams_to_mark)
            << " as DELETING in sys catalog";
  for (auto& lock : locks) {
    lock->Commit();
  }
  return Status::OK();
}

Status CatalogManager::FindCDCStreamsMarkedAsDeleting(
    std::vector<scoped_refptr<CDCStreamInfo>>* streams) {
  TRACE("Acquired catalog manager lock");
  std::shared_lock<LockType> l(lock_);
  for (const CDCStreamInfoMap::value_type& entry : cdc_stream_map_) {
    auto ltm = entry.second->LockForRead();
    if (ltm->data().is_deleting()) {
      LOG(INFO) << "Stream " << entry.second->id() << " was marked as DELETING";
      streams->push_back(entry.second);
    }
  }
  return Status::OK();
}

Status CatalogManager::CleanUpDeletedCDCStreams(
    const std::vector<scoped_refptr<CDCStreamInfo>>& streams) {
  RETURN_NOT_OK(CheckOnline());

  auto ybclient = master_->async_client_initializer().client();

  // First. For each deleted stream, delete the cdc state rows.
  // Delete all the entries in cdc_state table that contain all the deleted cdc streams.
  client::TableHandle cdc_table;
  const client::YBTableName cdc_state_table_name(
      YQL_DATABASE_CQL, master::kSystemNamespaceName, master::kCdcStateTableName);
  Status s = cdc_table.Open(cdc_state_table_name, ybclient);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to open table " << master::kCdcStateTableName
                 << " to delete stream ids entries: " << s;
    return s.CloneAndPrepend("Unable to open cdc_state table");
  }

  std::shared_ptr<client::YBSession> session = ybclient->NewSession();
  std::vector<std::pair<CDCStreamId, std::shared_ptr<client::YBqlWriteOp>>> stream_ops;
  std::set<CDCStreamId> failed_streams;
  for (const auto& stream : streams) {
    LOG(INFO) << "Deleting rows for stream " << stream->id();
    vector<scoped_refptr<TabletInfo>> tablets;
    scoped_refptr<TableInfo> table;
    {
      TRACE("Acquired catalog manager lock");
      SharedLock<LockType> l(lock_);
      table = FindPtrOrNull(*table_ids_map_, stream->table_id());
    }
    // GetAllTablets locks lock_ in shared mode.
    if (table) {
      table->GetAllTablets(&tablets);
    }

    for (const auto& tablet : tablets) {
      const auto delete_op = cdc_table.NewDeleteOp();
      auto* delete_req = delete_op->mutable_request();

      QLAddStringHashValue(delete_req, tablet->tablet_id());
      QLAddStringRangeValue(delete_req, stream->id());
      s = session->Apply(delete_op);
      stream_ops.push_back(std::make_pair(stream->id(), delete_op));
      LOG(INFO) << "Deleting stream " << stream->id() << " for tablet " << tablet->tablet_id()
              << " with request " << delete_req->ShortDebugString();
      if (!s.ok()) {
        LOG(WARNING) << "Unable to delete stream with id "
                     << stream->id() << " from table " << master::kCdcStateTableName
                     << " for tablet " << tablet->tablet_id()
                     << ". Status: " << s
                     << ", Response: " << delete_op->response().ShortDebugString();
      }
    }
  }
  // Flush all the delete operations.
  s = session->Flush();
  if (!s.ok()) {
    LOG(ERROR) << "Unable to flush operations to delete cdc streams: " << s;
    return s.CloneAndPrepend("Error deleting cdc stream rows from cdc_state table");
  }

  for (const auto& e : stream_ops) {
    if (!e.second->succeeded()) {
      LOG(WARNING) << "Error deleting cdc_state row with tablet id "
                   << e.second->request().hashed_column_values(0).value().string_value()
                   << " and stream id "
                   << e.second->request().range_column_values(0).value().string_value()
                   << ": " << e.second->response().status();
      failed_streams.insert(e.first);
    }
  }

  // TODO: Read cdc_state table and verify that there are not rows with the specified cdc stream
  // and keep those in the map in the DELETED state to retry later.

  std::vector<std::unique_ptr<CDCStreamInfo::lock_type>> locks;
  locks.reserve(streams.size() - failed_streams.size());
  std::vector<CDCStreamInfo*> streams_to_delete;
  streams_to_delete.reserve(streams.size() - failed_streams.size());

  // Delete from sys catalog only those streams that were successfully delete from cdc_state.
  for (auto& stream : streams) {
    if (failed_streams.find(stream->id()) == failed_streams.end()) {
      locks.push_back(stream->LockForWrite());
      streams_to_delete.push_back(stream.get());
    }
  }

  // The mutation will be aborted when 'l' exits the scope on early return.
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->DeleteItems(streams_to_delete, leader_ready_term()),
      "deleting CDC streams from sys-catalog"));
  LOG(INFO) << "Successfully deleted streams " << JoinStreamsCSVLine(streams_to_delete)
            << " from sys catalog";

  // Remove it from the map.
  TRACE("Removing from CDC stream maps");
  {
    std::lock_guard<LockType> l(lock_);
    for (const auto& stream : streams_to_delete) {
      if (cdc_stream_map_.erase(stream->id()) < 1) {
        return STATUS(IllegalState, "Could not remove CDC stream from map", stream->id());
      }
    }
  }
  LOG(INFO) << "Successfully deleted streams " << JoinStreamsCSVLine(streams_to_delete)
            << " from stream map";

  for (auto& lock : locks) {
    lock->Commit();
  }
  return Status::OK();
}

Status CatalogManager::GetCDCStream(const GetCDCStreamRequestPB* req,
                                    GetCDCStreamResponsePB* resp,
                                    rpc::RpcContext* rpc) {
  LOG(INFO) << "GetCDCStream from " << RequestorString(rpc)
            << ": " << req->DebugString();
  RETURN_NOT_OK(CheckOnline());


  if (!req->has_stream_id()) {
    return STATUS(InvalidArgument, "CDC Stream ID must be provided", req->ShortDebugString(),
                  MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  scoped_refptr<CDCStreamInfo> stream;
  {
    std::shared_lock<LockType> l(lock_);
    stream = FindPtrOrNull(cdc_stream_map_, req->stream_id());
  }

  if (stream == nullptr || stream->LockForRead()->data().is_deleting()) {
    return STATUS(NotFound, "Could not find CDC stream", req->ShortDebugString(),
                  MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  auto stream_lock = stream->LockForRead();

  CDCStreamInfoPB* stream_info = resp->mutable_stream();

  stream_info->set_stream_id(stream->id());
  stream_info->set_table_id(stream_lock->data().table_id());
  stream_info->mutable_options()->CopyFrom(stream_lock->data().options());

  return Status::OK();
}

Status CatalogManager::ListCDCStreams(const ListCDCStreamsRequestPB* req,
                                      ListCDCStreamsResponsePB* resp) {

  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<TableInfo> table;
  bool filter_table = req->has_table_id();
  if (filter_table) {
    // Lookup the table and verify that it exists.
    TableIdentifierPB table_identifier;
    table_identifier.set_table_id(req->table_id());

    RETURN_NOT_OK(FindTable(table_identifier, &table));
    if (table == nullptr) {
      return STATUS(NotFound, "Table not found", req->table_id(),
                    MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  std::shared_lock<LockType> l(lock_);

  for (const CDCStreamInfoMap::value_type& entry : cdc_stream_map_) {
    auto ltm = entry.second->LockForRead();

    if ((filter_table && table->id() != ltm->data().table_id()) || ltm->data().is_deleting()) {
      continue; // Skip deleting/deleted streams and streams from other tables.
    }

    CDCStreamInfoPB* stream = resp->add_streams();
    stream->set_stream_id(entry.second->id());
    stream->set_table_id(ltm->data().table_id());
    stream->mutable_options()->CopyFrom(ltm->data().options());
  }
  return Status::OK();
}

bool CatalogManager::CDCStreamExistsUnlocked(const CDCStreamId& stream_id) {
  LOG_IF(DFATAL, !lock_.is_locked()) << "CatalogManager lock must be taken";
  scoped_refptr<CDCStreamInfo> stream = FindPtrOrNull(cdc_stream_map_, stream_id);
  if (stream == nullptr || stream->LockForRead()->data().is_deleting()) {
    return false;
  }
  return true;
}

/*
 * UniverseReplication is setup in 4 stages within the Catalog Manager
 * 1. SetupUniverseReplication: Validates user input & requests Producer schema.
 * 2. GetTableSchemaCallback:   Validates Schema compatibility & requests Producer CDC init.
 * 3. AddCDCStreamToUniverseAndInitConsumer:  Setup RPC connections for CDC Streaming
 * 4. InitCDCConsumer:          Initializes the Consumer settings to begin tailing data
 */
Status CatalogManager::SetupUniverseReplication(const SetupUniverseReplicationRequestPB* req,
                                                SetupUniverseReplicationResponsePB* resp,
                                                rpc::RpcContext* rpc) {
  LOG(INFO) << "SetupUniverseReplication from " << RequestorString(rpc)
            << ": " << req->DebugString();

  // Sanity checking section.
  RETURN_NOT_OK(CheckOnline());

  if (!req->has_producer_id()) {
    return STATUS(InvalidArgument, "Producer universe ID must be provided",
                  req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  if (req->producer_master_addresses_size() <= 0) {
    return STATUS(InvalidArgument, "Producer master address must be provided",
                  req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  if (req->producer_bootstrap_ids().size() > 0 &&
      req->producer_bootstrap_ids().size() != req->producer_table_ids().size()) {
    return STATUS(InvalidArgument, "Number of bootstrap ids must be equal to number of tables",
                  req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  std::unordered_map<TableId, std::string> table_id_to_bootstrap_id;

  if (req->producer_bootstrap_ids_size() > 0) {
    for (int i = 0; i < req->producer_table_ids().size(); i++) {
      table_id_to_bootstrap_id[req->producer_table_ids(i)] = req->producer_bootstrap_ids(i);
    }
  }

  // We assume that the list of table ids is unique.
  if (req->producer_bootstrap_ids().size() > 0 &&
      req->producer_table_ids().size() != table_id_to_bootstrap_id.size()) {
    return STATUS(InvalidArgument, "When providing bootstrap ids, "
                  "the list of tables must be unique", req->ShortDebugString(),
                  MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  scoped_refptr<UniverseReplicationInfo> ri;
  {
    TRACE("Acquired catalog manager lock");
    std::shared_lock<LockType> l(lock_);

    if (FindPtrOrNull(universe_replication_map_, req->producer_id()) != nullptr) {
      return STATUS(InvalidArgument, "Producer already present", req->producer_id(),
                    MasterError(MasterErrorPB::INVALID_REQUEST));
    }
  }

  // Create an entry in the system catalog DocDB for this new universe replication.
  ri = new UniverseReplicationInfo(req->producer_id());
  ri->mutable_metadata()->StartMutation();
  SysUniverseReplicationEntryPB *metadata = &ri->mutable_metadata()->mutable_dirty()->pb;
  metadata->set_producer_id(req->producer_id());
  metadata->mutable_producer_master_addresses()->CopyFrom(req->producer_master_addresses());
  metadata->mutable_tables()->CopyFrom(req->producer_table_ids());
  metadata->set_state(SysUniverseReplicationEntryPB::INITIALIZING);

  RETURN_NOT_OK(CheckLeaderStatusAndSetupError(
      sys_catalog_->AddItem(ri.get(), leader_ready_term()),
      "inserting universe replication info into sys-catalog", resp));
  TRACE("Wrote universe replication info to sys-catalog");

  // Commit the in-memory state now that it's added to the persistent catalog.
  ri->mutable_metadata()->CommitMutation();
  LOG(INFO) << "Setup universe replication from producer " << ri->ToString();

  {
    std::lock_guard<LockType> l(lock_);
    universe_replication_map_[ri->id()] = ri;
  }

  // Initialize the CDC Stream by querying the Producer server for RPC sanity checks.
  auto result = ri->GetOrCreateCDCRpcTasks(req->producer_master_addresses());
  if (!result.ok()) {
    MarkUniverseReplicationFailed(ri);
    return result.status().CloneAndAddErrorCode(MasterError(MasterErrorPB::INVALID_REQUEST));
  }
  std::shared_ptr<CDCRpcTasks> cdc_rpc = *result;

  // For each table, run an async RPC task to verify a sufficient Producer:Consumer schema match.
  for (int i = 0; i < req->producer_table_ids_size(); i++) {

    // SETUP CONTINUES after this async call.
    Status s;
    if (IsColocatedParentTableId(req->producer_table_ids(i))) {
      auto tables_info = std::make_shared<std::vector<client::YBTableInfo>>();
      s = cdc_rpc->client()->GetColocatedTabletSchemaById(
          req->producer_table_ids(i), tables_info,
          Bind(&enterprise::CatalogManager::GetColocatedTabletSchemaCallback, Unretained(this),
               ri->id(), tables_info, table_id_to_bootstrap_id));
    } else {
      auto table_info = std::make_shared<client::YBTableInfo>();
      s = cdc_rpc->client()->GetTableSchemaById(
          req->producer_table_ids(i), table_info,
          Bind(&enterprise::CatalogManager::GetTableSchemaCallback, Unretained(this),
               ri->id(), table_info, table_id_to_bootstrap_id));
    }

    if (!s.ok()) {
      MarkUniverseReplicationFailed(ri);
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
    }
  }

  LOG(INFO) << "Started schema validation for universe replication " << ri->ToString();
  return Status::OK();
}

void CatalogManager::MarkUniverseReplicationFailed(
    scoped_refptr<UniverseReplicationInfo> universe) {
  auto l = universe->LockForWrite();
  if (l->data().pb.state() == SysUniverseReplicationEntryPB::DELETED) {
    l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::DELETED_ERROR);
  } else {
    l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::FAILED);
  }

  // Update sys_catalog.
  const Status s = sys_catalog_->UpdateItem(universe.get(), leader_ready_term());
  if (!s.ok()) {
    return (void)CheckStatus(s, "updating universe replication info in sys-catalog");
  }
  l->Commit();
}

Status CatalogManager::ValidateTableSchema(
    const std::shared_ptr<client::YBTableInfo>& info,
    const std::unordered_map<TableId, std::string>& table_bootstrap_ids,
    TableId* consumer_table_id) {
  // Get corresponding table schema on local universe.
  GetTableSchemaRequestPB req;
  GetTableSchemaResponsePB resp;

  auto* table = req.mutable_table();
  table->set_table_name(info->table_name.table_name());
  table->mutable_namespace_()->set_name(info->table_name.namespace_name());
  table->mutable_namespace_()->set_database_type(
      GetDatabaseTypeForTable(client::YBTable::ClientToPBTableType(info->table_type)));

  // Since YSQL tables are not present in table map, we first need to list tables to get the table
  // ID and then get table schema.
  // Remove this once table maps are fixed for YSQL.
  ListTablesRequestPB list_req;
  ListTablesResponsePB list_resp;

  list_req.set_name_filter(info->table_name.table_name());
  Status status = ListTables(&list_req, &list_resp);
  if (!status.ok() || list_resp.has_error()) {
    return STATUS(NotFound, Substitute("Error while listing table: $0", status.ToString()));
  }

  // TODO: This does not work for situation where tables in different YSQL schemas have the same
  // name. This will be fixed as part of #1476.
  for (const auto& t : list_resp.tables()) {
    if (t.name() == info->table_name.table_name() &&
        t.namespace_().name() == info->table_name.namespace_name()) {
      table->set_table_id(t.id());
      break;
    }
  }

  if (!table->has_table_id()) {
    return STATUS(NotFound,
        Substitute("Could not find matching table for $0", info->table_name.ToString()));
  }

  // We have a table match.  Now get the table schema and validate
  status = GetTableSchema(&req, &resp);
  if (!status.ok() || resp.has_error()) {
    return STATUS(NotFound, Substitute("Error while getting table schema: $0", status.ToString()));
  }

  auto result = info->schema.EquivalentForDataCopy(resp.schema());
  if (!result.ok() || !*result) {
    return STATUS(IllegalState,
        Substitute("Source and target schemas don't match: "
                   "Source: $0, Target: $1, Source schema: $2, Target schema: $3",
                   info->table_id, resp.identifier().table_id(),
                   info->schema.ToString(), resp.schema().DebugString()));
  }

  // Still need to make map of table id to resp table id (to add to validated map)
  // For colocated tables, only add the parent table since we only added the parent table to the
  // original pb (we use the number of tables in the pb to determine when validation is done).
  if (info->colocated) {
    // For now we require that colocated tables have the same table oid.
    auto source_oid = CHECK_RESULT(GetPgsqlTableOid(info->table_id));
    auto target_oid = CHECK_RESULT(GetPgsqlTableOid(resp.identifier().table_id()));
    if (source_oid != target_oid) {
    return STATUS(IllegalState,
        Substitute("Source and target table oids don't match for colocated table: "
                   "Source: $0, Target: $1, Source table oid: $2, Target table oid: $3",
                   info->table_id, resp.identifier().table_id(), source_oid, target_oid));
    }
    string parent_table_id = resp.identifier().namespace_().id() + kColocatedParentTableIdSuffix;
    *consumer_table_id = parent_table_id;
  } else {
    *consumer_table_id = resp.identifier().table_id();
  }

  return Status::OK();
}

Status CatalogManager::AddValidatedTableAndCreateCdcStreams(
      scoped_refptr<UniverseReplicationInfo> universe,
      const std::unordered_map<TableId, std::string>& table_bootstrap_ids,
      const TableId& producer_table,
      const TableId& consumer_table) {
  auto l = universe->LockForWrite();
  auto master_addresses = l->data().pb.producer_master_addresses();

  auto res = universe->GetOrCreateCDCRpcTasks(master_addresses);
  if (!res.ok()) {
    l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::FAILED);
    const Status s = sys_catalog_->UpdateItem(universe.get(), leader_ready_term());
    if (!s.ok()) {
      return CheckStatus(s, "updating universe replication info in sys-catalog");
    }
    l->Commit();
    return STATUS(InternalError,
        Substitute("Error while setting up client for producer $0", universe->id()));
  }
  std::shared_ptr<CDCRpcTasks> cdc_rpc = *res;
  vector<TableId> validated_tables;

  if (l->data().is_deleted_or_failed()) {
    // Nothing to do since universe is being deleted.
    return STATUS(Aborted, "Universe is being deleted");
  }

  auto map = l->mutable_data()->pb.mutable_validated_tables();
  (*map)[producer_table] = consumer_table;

  // Now, all tables are validated.
  if (l->mutable_data()->pb.validated_tables_size() == l->mutable_data()->pb.tables_size()) {
    l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::VALIDATED);
    auto tbl_iter = l->data().pb.tables();
    validated_tables.insert(validated_tables.begin(), tbl_iter.begin(), tbl_iter.end());
  }

  // TODO: end of config validation should be where SetupUniverseReplication exits back to user
  LOG(INFO) << "UpdateItem in AddValidatedTable";

  // Update sys_catalog.
  Status status = sys_catalog_->UpdateItem(universe.get(), leader_ready_term());
  if (!status.ok()) {
    LOG(ERROR) << "Error during UpdateItem: " << status;
    return CheckStatus(status, "updating universe replication info in sys-catalog");
  }
  l->Commit();

  // Create CDC stream for each validated table, after persisting the replication state change.
  if (!validated_tables.empty()) {
    std::unordered_map<std::string, std::string> options;
    options.reserve(2);
    options.emplace(cdc::kRecordType, CDCRecordType_Name(cdc::CDCRecordType::CHANGE));
    options.emplace(cdc::kRecordFormat, CDCRecordFormat_Name(cdc::CDCRecordFormat::WAL));

    for (const auto& table : validated_tables) {
      string producer_bootstrap_id;
      auto it = table_bootstrap_ids.find(table);
      if (it != table_bootstrap_ids.end()) {
        producer_bootstrap_id = it->second;
      }
      if (!producer_bootstrap_id.empty()) {
        auto table_id = std::make_shared<TableId>();
        auto stream_options = std::make_shared<std::unordered_map<std::string, std::string>>();
        cdc_rpc->client()->GetCDCStream(producer_bootstrap_id, table_id, stream_options,
            std::bind(&enterprise::CatalogManager::GetCDCStreamCallback, this,
                producer_bootstrap_id, table_id, stream_options, universe->id(), table,
                std::placeholders::_1));
      } else {
        cdc_rpc->client()->CreateCDCStream(
            table, options,
            std::bind(&enterprise::CatalogManager::AddCDCStreamToUniverseAndInitConsumer, this,
                universe->id(), table, std::placeholders::_1));
      }
    }
  }
  return Status::OK();
}

void CatalogManager::GetTableSchemaCallback(
    const std::string& universe_id, const std::shared_ptr<client::YBTableInfo>& info,
    const std::unordered_map<TableId, std::string>& table_bootstrap_ids, const Status& s) {
  // First get the universe.
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    std::shared_lock<LockType> catalog_lock(lock_);
    TRACE("Acquired catalog manager lock");

    universe = FindPtrOrNull(universe_replication_map_, universe_id);
    if (universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << universe_id;
      return;
    }
  }

  if (!s.ok()) {
    MarkUniverseReplicationFailed(universe);
    LOG(ERROR) << "Error getting schema for table " << info->table_id << ": " << s;
    return;
  }

  // Validate the table schema.
  TableId table_id;
  Status status = ValidateTableSchema(info, table_bootstrap_ids, &table_id);
  if (!status.ok()) {
    MarkUniverseReplicationFailed(universe);
    LOG(ERROR) << "Found error while validating table schema for table " << info->table_id
               << ": " << status;
    return;
  }

  status = AddValidatedTableAndCreateCdcStreams(universe,
                                                table_bootstrap_ids,
                                                info->table_id,
                                                table_id);
  if (!status.ok()) {
    LOG(ERROR) << "Found error while adding validated table to system catalog: " << info->table_id
               << ": " << status;
    return;
  }
}

void CatalogManager::GetColocatedTabletSchemaCallback(
    const std::string& universe_id, const std::shared_ptr<std::vector<client::YBTableInfo>>& infos,
    const std::unordered_map<TableId, std::string>& table_bootstrap_ids, const Status& s) {
  // First get the universe.
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    std::shared_lock<LockType> catalog_lock(lock_);
    TRACE("Acquired catalog manager lock");

    universe = FindPtrOrNull(universe_replication_map_, universe_id);
    if (universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << universe_id;
      return;
    }
  }

  if (!s.ok()) {
    MarkUniverseReplicationFailed(universe);
    std::ostringstream oss;
    for (int i = 0; i < infos->size(); ++i) {
      oss << ((i == 0) ? "" : ", ") << (*infos)[i].table_id;
    }
    LOG(ERROR) << "Error getting schema for tables: [ " << oss.str() << " ]: " << s;
    return;
  }

  if (infos->empty()) {
    LOG(WARNING) << "Received empty list of tables to validate: " << s;
    return;
  }

  // Validate table schemas.
  std::unordered_set<TableId> producer_parent_table_ids;
  std::unordered_set<TableId> consumer_parent_table_ids;
  for (const auto& info : *infos) {
    // Verify that we have a colocated table.
    if (!info.colocated) {
      MarkUniverseReplicationFailed(universe);
      LOG(ERROR) << "Received non-colocated table: " << info.table_id;
      return;
    }
    // Validate each table, and get the parent colocated table id for the consumer.
    TableId consumer_parent_table_id;
    Status table_status = ValidateTableSchema(std::make_shared<client::YBTableInfo>(info),
                                              table_bootstrap_ids,
                                              &consumer_parent_table_id);
    if (!table_status.ok()) {
      MarkUniverseReplicationFailed(universe);
      LOG(ERROR) << "Found error while validating table schema for table " << info.table_id
                 << ": " << table_status;
      return;
    }
    // Store the parent table ids.
    producer_parent_table_ids.insert(
        info.table_name.namespace_id() + kColocatedParentTableIdSuffix);
    consumer_parent_table_ids.insert(consumer_parent_table_id);
  }

  // Verify that we only found one producer and one consumer colocated parent table id.
  if (producer_parent_table_ids.size() != 1) {
    MarkUniverseReplicationFailed(universe);
    std::ostringstream oss;
    for (auto it = producer_parent_table_ids.begin(); it != producer_parent_table_ids.end(); ++it) {
      oss << ((it == producer_parent_table_ids.begin()) ? "" : ", ") << *it;
    }
    LOG(ERROR) << "Found incorrect number of producer colocated parent table ids."
               << "Expected 1, but found: [ " << oss.str() << " ]";
    return;
  }
  if (consumer_parent_table_ids.size() != 1) {
    MarkUniverseReplicationFailed(universe);
    std::ostringstream oss;
    for (auto it = consumer_parent_table_ids.begin(); it != consumer_parent_table_ids.end(); ++it) {
      oss << ((it == consumer_parent_table_ids.begin()) ? "" : ", ") << *it;
    }
    LOG(ERROR) << "Found incorrect number of consumer colocated parent table ids."
               << "Expected 1, but found: [ " << oss.str() << " ]";
    return;
  }

  Status status = AddValidatedTableAndCreateCdcStreams(universe,
                                                       table_bootstrap_ids,
                                                       *producer_parent_table_ids.begin(),
                                                       *consumer_parent_table_ids.begin());
  if (!status.ok()) {
    LOG(ERROR) << "Found error while adding validated table to system catalog: "
               << *producer_parent_table_ids.begin() << ": " << status;
    return;
  }
}

void CatalogManager::GetCDCStreamCallback(
    const CDCStreamId& bootstrap_id,
    std::shared_ptr<TableId> table_id,
    std::shared_ptr<std::unordered_map<std::string, std::string>> options,
    const std::string& universe_id,
    const TableId& table,
    const Status& s) {
  if (!s.ok()) {
    LOG(ERROR) << "Unable to find bootstrap id " << bootstrap_id;
    AddCDCStreamToUniverseAndInitConsumer(universe_id, table, s);
  } else {
    if (*table_id != table) {
      const Status invalid_bootstrap_id_status = STATUS_FORMAT(
          InvalidArgument, "Invalid bootstrap id for table $0. Bootstrap id $1 belongs to table $2",
          table, bootstrap_id, *table_id);
      LOG(ERROR) << invalid_bootstrap_id_status;
      AddCDCStreamToUniverseAndInitConsumer(universe_id, table, invalid_bootstrap_id_status);
    }
    // todo check options
    AddCDCStreamToUniverseAndInitConsumer(universe_id, table, bootstrap_id);
  }
}

void CatalogManager::AddCDCStreamToUniverseAndInitConsumer(
    const std::string& universe_id, const TableId& table_id, const Result<CDCStreamId>& stream_id) {
  scoped_refptr<UniverseReplicationInfo> universe;
  {
    std::shared_lock<LockType> catalog_lock(lock_);
    TRACE("Acquired catalog manager lock");

    universe = FindPtrOrNull(universe_replication_map_, universe_id);
    if (universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << universe_id;
      return;
    }
  }

  if (!stream_id.ok()) {
    LOG(ERROR) << "Error setting up CDC stream for table " << table_id;
    MarkUniverseReplicationFailed(universe);
    return;
  }

  bool merge_alter = false;
  {
    auto l = universe->LockForWrite();
    if (l->data().is_deleted_or_failed()) {
      // Nothing to do if universe is being deleted.
      return;
    }

    auto map = l->mutable_data()->pb.mutable_table_streams();
    (*map)[table_id] = *stream_id;

    // This functions as a barrier: waiting for the last RPC call from GetTableSchemaCallback.
    if (l->mutable_data()->pb.table_streams_size() == l->data().pb.tables_size()) {
      // All tables successfully validated! Register CDC consumers & start replication.
      LOG(INFO) << "Registering CDC consumers for universe " << universe->id();

      auto validated_tables = l->data().pb.validated_tables();

      std::vector<CDCConsumerStreamInfo> consumer_info;
      consumer_info.reserve(l->data().pb.tables_size());
      for (const auto& table : validated_tables) {
        CDCConsumerStreamInfo info;
        info.producer_table_id = table.first;
        info.consumer_table_id = table.second;
        info.stream_id = (*map)[info.producer_table_id];
        consumer_info.push_back(info);
      }

      std::vector<HostPort> hp;
      HostPortsFromPBs(l->data().pb.producer_master_addresses(), &hp);

      Status s = InitCDCConsumer(consumer_info, HostPort::ToCommaSeparatedString(hp),
          l->data().pb.producer_id());
      if (!s.ok()) {
        LOG(ERROR) << "Error registering subscriber: " << s;
        l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::FAILED);
      } else {
        GStringPiece original_producer_id(universe->id());
        if (original_producer_id.ends_with(".ALTER")) {
          // Don't enable ALTER universes, merge them into the main universe instead.
          merge_alter = true;
        } else {
          l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::ACTIVE);
        }
      }
    }

    // Update sys_catalog with new producer table id info.
    Status status = sys_catalog_->UpdateItem(universe.get(), leader_ready_term());
    if (!status.ok()) {
      return (void)CheckStatus(status, "updating universe replication info in sys-catalog");
    }
    l->Commit();
  }

  // If this is an 'alter', merge back into primary command now that setup is a success.
  if (merge_alter) {
    MergeUniverseReplication(universe);
  }
}

Status CatalogManager::InitCDCConsumer(
    const std::vector<CDCConsumerStreamInfo>& consumer_info,
    const std::string& master_addrs,
    const std::string& producer_universe_uuid) {

  std::unordered_set<HostPort, HostPortHash> tserver_addrs;
  // Get the tablets in the consumer table.
  cdc::ProducerEntryPB producer_entry;
  for (const auto& stream_info : consumer_info) {
    GetTableLocationsRequestPB consumer_table_req;
    consumer_table_req.set_max_returned_locations(std::numeric_limits<int32_t>::max());
    GetTableLocationsResponsePB consumer_table_resp;
    TableIdentifierPB table_identifer;
    table_identifer.set_table_id(stream_info.consumer_table_id);
    *(consumer_table_req.mutable_table()) = table_identifer;
    RETURN_NOT_OK(GetTableLocations(&consumer_table_req, &consumer_table_resp));
    cdc::StreamEntryPB stream_entry;
    // Get producer tablets and map them to the consumer tablets
    RETURN_NOT_OK(CreateTabletMapping(
        stream_info.producer_table_id, stream_info.consumer_table_id, producer_universe_uuid,
        master_addrs, consumer_table_resp, &tserver_addrs, &stream_entry));
    (*producer_entry.mutable_stream_map())[stream_info.stream_id] = std::move(stream_entry);
  }

  // Log the Network topology of the Producer Cluster
  auto master_addrs_list = StringSplit(master_addrs, ',');
  producer_entry.mutable_master_addrs()->Reserve(master_addrs_list.size());
  for (const auto& addr : master_addrs_list) {
    auto hp = VERIFY_RESULT(HostPort::FromString(addr, 0));
    HostPortToPB(hp, producer_entry.add_master_addrs());
  }

  producer_entry.mutable_tserver_addrs()->Reserve(tserver_addrs.size());
  for (const auto& addr : tserver_addrs) {
    HostPortToPB(addr, producer_entry.add_tserver_addrs());
  }

  auto l = cluster_config_->LockForWrite();
  auto producer_map = l->mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
  auto it = producer_map->find(producer_universe_uuid);
  if (it != producer_map->end()) {
    return STATUS(InvalidArgument, "Already created a consumer for this universe");
  }

  // TServers will use the ClusterConfig to create CDC Consumers for applicable local tablets.
  (*producer_map)[producer_universe_uuid] = std::move(producer_entry);
  l->mutable_data()->pb.set_version(l->mutable_data()->pb.version() + 1);
  RETURN_NOT_OK(CheckStatus(
      sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term()),
      "updating cluster config in sys-catalog"));
  l->Commit();

  return Status::OK();
}

void CatalogManager::MergeUniverseReplication(scoped_refptr<UniverseReplicationInfo> universe) {
  // Merge back into primary command now that setup is a success.
  GStringPiece original_producer_id(universe->id());
  if (!original_producer_id.ends_with(".ALTER")) {
    return;
  }
  original_producer_id.remove_suffix(sizeof(".ALTER")-1 /* exclude \0 ending */);
  LOG(INFO) << "Merging CDC universe: " << universe->id()
            << " into " << original_producer_id.ToString();

  scoped_refptr<UniverseReplicationInfo> original_universe;
  {
    std::shared_lock<LockType> catalog_lock(lock_);
    TRACE("Acquired catalog manager lock");

    original_universe = FindPtrOrNull(universe_replication_map_, original_producer_id.ToString());
    if (original_universe == nullptr) {
      LOG(ERROR) << "Universe not found: " << original_producer_id.ToString();
      return;
    }
  }
  // Merge Cluster Config for TServers.
  {
    auto cl = cluster_config_->LockForWrite();
    auto pm = cl->mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    auto original_producer_entry = pm->find(original_universe->id());
    auto alter_producer_entry = pm->find(universe->id());
    if (original_producer_entry != pm->end() && alter_producer_entry != pm->end()) {
      // Merge the Tables from the Alter into the original.
      auto as = alter_producer_entry->second.stream_map();
      original_producer_entry->second.mutable_stream_map()->insert(as.begin(), as.end());
      // Delete the Alter
      pm->erase(alter_producer_entry);
    } else {
      LOG(WARNING) << "Could not find both universes in Cluster Config: " << universe->id();
    }
    cl->mutable_data()->pb.set_version(cl->mutable_data()->pb.version() + 1);
    const Status s = sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term());
    if (!s.ok()) {
      return (void)CheckStatus(s, "updating cluster config in sys-catalog");
    }
    cl->Commit();
  }
  // Merge Master Config on Consumer. (no need for Producer changes, since it uses stream_id)
  {
    auto original_lock = original_universe->LockForWrite();
    auto alter_lock = universe->LockForWrite();
    // Merge Table->StreamID mapping.
    auto at = alter_lock->mutable_data()->pb.mutable_tables();
    original_lock->mutable_data()->pb.mutable_tables()->MergeFrom(*at);
    at->Clear();
    auto as = alter_lock->mutable_data()->pb.mutable_table_streams();
    original_lock->mutable_data()->pb.mutable_table_streams()->insert(as->begin(), as->end());
    as->clear();
    auto av = alter_lock->mutable_data()->pb.mutable_validated_tables();
    original_lock->mutable_data()->pb.mutable_validated_tables()->insert(av->begin(), av->end());
    av->clear();
    alter_lock->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::DELETED);

    vector<UniverseReplicationInfo*> universes{original_universe.get(), universe.get()};
    const Status s = sys_catalog_->UpdateItems(universes, leader_ready_term());
    if (!s.ok()) {
      return (void)CheckStatus(s, "updating universe replication entries in sys-catalog");
    }
    alter_lock->Commit();
    original_lock->Commit();
  }
  // TODO: universe_replication_map_.erase(universe->id()) at a later time.
  //       TwoDCTest.AlterUniverseReplicationTables crashes due to undiagnosed race right now.
  LOG(INFO) << "Done with Merging " << universe->id() << " into " << original_universe->id();
}

Status CatalogManager::DeleteUniverseReplication(const DeleteUniverseReplicationRequestPB* req,
                                                 DeleteUniverseReplicationResponsePB* resp,
                                                 rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing DeleteUniverseReplication request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  RETURN_NOT_OK(CheckOnline());

  if (!req->has_producer_id()) {
    return STATUS(InvalidArgument, "Producer universe ID required", req->ShortDebugString(),
                  MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  scoped_refptr<UniverseReplicationInfo> ri;
  {
    std::shared_lock<LockType> catalog_lock(lock_);
    TRACE("Acquired catalog manager lock");

    ri = FindPtrOrNull(universe_replication_map_, req->producer_id());
    if (ri == nullptr) {
      return STATUS(NotFound, "Universe replication info does not exist",
                    req->ShortDebugString(), MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  auto l = ri->LockForWrite();
  l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::DELETED);

  // Delete subscribers on the Consumer Registry (removes from TServers).
  LOG(INFO) << "Deleting subscribers for producer " << req->producer_id();
  {
    auto cl = cluster_config_->LockForWrite();
    auto producer_map = cl->mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    auto it = producer_map->find(req->producer_id());
    if (it != producer_map->end()) {
      producer_map->erase(it);
      cl->mutable_data()->pb.set_version(cl->mutable_data()->pb.version() + 1);
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term()),
          "updating cluster config in sys-catalog"));
      cl->Commit();
    }
  }

  // Delete CDC stream config on the Producer.
  if (!l->data().pb.table_streams().empty()) {
    auto result = ri->GetOrCreateCDCRpcTasks(l->data().pb.producer_master_addresses());
    if (!result.ok()) {
      LOG(WARNING) << "Unable to create cdc rpc task. CDC streams won't be deleted: " << result;
    } else {
      auto cdc_rpc = *result;
      vector<CDCStreamId> streams;
      for (const auto& table : l->data().pb.table_streams()) {
        streams.push_back(table.second);
      }
      auto s = cdc_rpc->client()->DeleteCDCStream(streams);
      if (!s.ok()) {
        LOG(WARNING) << "Unable to delete CDC stream " << s;
      }
    }
  }

  // Delete universe in the Universe Config.
  DeleteUniverseReplicationUnlocked(ri);
  l->Commit();

  LOG(INFO) << "Processed delete universe replication " << ri->ToString()
            << " per request from " << RequestorString(rpc);

  return Status::OK();
}

void CatalogManager::DeleteUniverseReplicationUnlocked(
    scoped_refptr<UniverseReplicationInfo> universe) {
  // Assumes that caller has locked universe.
  Status s = sys_catalog_->DeleteItem(universe.get(), leader_ready_term());
  if (!s.ok()) {
    LOG(ERROR) << "An error occured while updating sys-catalog: " << s
               << ": universe_id: " << universe->id();
    return;
  }
  // Remove it from the map.
  std::lock_guard<LockType> catalog_lock(lock_);
  if (universe_replication_map_.erase(universe->id()) < 1) {
    LOG(ERROR) << "An error occured while removing replication info from map: " << s
               << ": universe_id: " << universe->id();
  }
}

Status CatalogManager::SetUniverseReplicationEnabled(
    const SetUniverseReplicationEnabledRequestPB* req,
    SetUniverseReplicationEnabledResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing SetUniverseReplicationEnabled request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  // Sanity Checking Cluster State and Input.
  RETURN_NOT_OK(CheckOnline());

  if (!req->has_producer_id()) {
    return STATUS(InvalidArgument, "Producer universe ID must be provided",
                  req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }
  if (!req->has_is_enabled()) {
    return STATUS(InvalidArgument, "Must explicitly set whether to enable",
                  req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  scoped_refptr<UniverseReplicationInfo> universe;
  {
    std::shared_lock<LockType> l(lock_);

    universe = FindPtrOrNull(universe_replication_map_, req->producer_id());
    if (universe == nullptr) {
      return STATUS(NotFound, "Could not find CDC producer universe",
                    req->ShortDebugString(), MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  // Update the Master's Universe Config with the new state.
  {
    auto l = universe->LockForWrite();
    if (l->data().pb.state() != SysUniverseReplicationEntryPB::DISABLED &&
        l->data().pb.state() != SysUniverseReplicationEntryPB::ACTIVE) {
      return STATUS(
          InvalidArgument,
          Format("Universe Replication in invalid state: $0.  Retry or Delete.",
              SysUniverseReplicationEntryPB::State_Name(l->data().pb.state())),
          req->ShortDebugString(),
          MasterError(MasterErrorPB::INVALID_REQUEST));
    }
    if (req->is_enabled()) {
        l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::ACTIVE);
    } else { // DISABLE.
        l->mutable_data()->pb.set_state(SysUniverseReplicationEntryPB::DISABLED);
    }
    RETURN_NOT_OK(CheckStatus(
        sys_catalog_->UpdateItem(universe.get(), leader_ready_term()),
        "updating universe replication info in sys-catalog"));
    l->Commit();
  }

  // Modify the Consumer Registry, which will fan out this info to all TServers on heartbeat.
  {
    auto l = cluster_config_->LockForWrite();
    auto producer_map = l->mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
    auto it = producer_map->find(req->producer_id());
    if (it == producer_map->end()) {
      LOG(WARNING) << "Valid Producer Universe not in Consumer Registry: " << req->producer_id();
      return STATUS(NotFound, "Could not find CDC producer universe",
                    req->ShortDebugString(), MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
    (*it).second.set_disable_stream(!req->is_enabled());
    l->mutable_data()->pb.set_version(l->mutable_data()->pb.version() + 1);
    RETURN_NOT_OK(CheckStatus(
        sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term()),
        "updating cluster config in sys-catalog"));
    l->Commit();
  }

  return Status::OK();
}

Status CatalogManager::AlterUniverseReplication(const AlterUniverseReplicationRequestPB* req,
                                                AlterUniverseReplicationResponsePB* resp,
                                                rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing AlterUniverseReplication request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  // Sanity Checking Cluster State and Input.
  RETURN_NOT_OK(CheckOnline());

  if (!req->has_producer_id()) {
    return STATUS(InvalidArgument, "Producer universe ID must be provided",
                  req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  // Verify that there is an existing Universe config
  scoped_refptr<UniverseReplicationInfo> original_ri;
  {
    std::shared_lock<LockType> l(lock_);

    original_ri = FindPtrOrNull(universe_replication_map_, req->producer_id());
    if (original_ri == nullptr) {
      return STATUS(NotFound, "Could not find CDC producer universe",
                    req->ShortDebugString(), MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  // Currently, config options are mutually exclusive to simplify transactionality.
  int config_count = (req->producer_master_addresses_size() > 0 ? 1 : 0) +
                     (req->producer_table_ids_to_remove_size() > 0 ? 1 : 0) +
                     (req->producer_table_ids_to_add_size() > 0 ? 1 : 0);
  if (config_count != 1) {
    return STATUS(InvalidArgument, "Only 1 Alter operation per request currently supported",
                  req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  // Config logic...
  if (req->producer_master_addresses_size() > 0) {
    // 'set_master_addresses'
    // TODO: Verify the input. Setup an RPC Task, ListTables, ensure same.

    // 1a. Persistent Config: Update the Universe Config for Master.
    {
      auto l = original_ri->LockForWrite();
      l->mutable_data()->pb.mutable_producer_master_addresses()->CopyFrom(
          req->producer_master_addresses());
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->UpdateItem(original_ri.get(), leader_ready_term()),
          "updating universe replication info in sys-catalog"));
      l->Commit();
    }
    // 1b. Persistent Config: Update the Consumer Registry (updates TServers)
    {
      auto l = cluster_config_->LockForWrite();
      auto producer_map = l->mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
      auto it = producer_map->find(req->producer_id());
      if (it == producer_map->end()) {
        LOG(WARNING) << "Valid Producer Universe not in Consumer Registry: " << req->producer_id();
        return STATUS(NotFound, "Could not find CDC producer universe",
                      req->ShortDebugString(), MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
      }
      (*it).second.mutable_master_addrs()->CopyFrom(req->producer_master_addresses());
      l->mutable_data()->pb.set_version(l->mutable_data()->pb.version() + 1);
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term()),
          "updating cluster config in sys-catalog"));
      l->Commit();
    }
    // 2. Memory Update: Change cdc_rpc_tasks (Master cache)
    {
      auto result = original_ri->GetOrCreateCDCRpcTasks(req->producer_master_addresses());
      if (!result.ok()) {
        return result.status().CloneAndAddErrorCode(MasterError(MasterErrorPB::INTERNAL_ERROR));
      }
    }
  } else if (req->producer_table_ids_to_remove_size() > 0) {
    // 'remove_table'
    auto it = req->producer_table_ids_to_remove();
    std::set<string> table_ids_to_remove(it.begin(), it.end());
    // Filter out any tables that aren't in the existing replication config.
    {
      auto l = original_ri->LockForRead();
      auto tbl_iter = l->data().pb.tables();
      std::set<string> existing_tables(tbl_iter.begin(), tbl_iter.end()), filtered_list;
      set_intersection(table_ids_to_remove.begin(), table_ids_to_remove.end(),
                       existing_tables.begin(), existing_tables.end(),
                       std::inserter(filtered_list, filtered_list.begin()));
      filtered_list.swap(table_ids_to_remove);
    }

    vector<CDCStreamId> streams_to_remove;
    // 1. Update the Consumer Registry (removes from TServers).
    {
      auto cl = cluster_config_->LockForWrite();
      auto pm = cl->mutable_data()->pb.mutable_consumer_registry()->mutable_producer_map();
      auto producer_entry = pm->find(req->producer_id());
      if (producer_entry != pm->end()) {
        // Remove the Tables Specified (not part of the key).
        auto stream_map = producer_entry->second.mutable_stream_map();
        for (auto& p : *stream_map) {
          if (table_ids_to_remove.count(p.second.producer_table_id()) > 0) {
            streams_to_remove.push_back(p.first);
          }
        }
        if (streams_to_remove.size() == stream_map->size()) {
          // If this ends with an empty Map, disallow and force user to delete.
          LOG(WARNING) << "CDC 'remove_table' tried to remove all tables." << req->producer_id();
          return STATUS(
              InvalidArgument,
              "Cannot remove all tables with alter. Use delete_universe_replication instead.",
              req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
        } else if (streams_to_remove.empty()) {
          // If this doesn't delete anything, notify the user.
          return STATUS(InvalidArgument, "Removal matched no entries.",
                        req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
        }
        for (auto& key : streams_to_remove) {
          stream_map->erase(stream_map->find(key));
        }
      }
      cl->mutable_data()->pb.set_version(cl->mutable_data()->pb.version() + 1);
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term()),
          "updating cluster config in sys-catalog"));
      cl->Commit();
    }
    // 2. Remove from Master Configs on Producer and Consumer.
    {
      auto l = original_ri->LockForWrite();
      if (!l->data().pb.table_streams().empty()) {
        // Delete Relevant Table->StreamID mappings on Consumer.
        auto table_streams = l->mutable_data()->pb.mutable_table_streams();
        auto validated_tables = l->mutable_data()->pb.mutable_validated_tables();
        for (auto& key : table_ids_to_remove) {
          table_streams->erase(table_streams->find(key));
          validated_tables->erase(validated_tables->find(key));
        }
        for (int i = 0; i < l->mutable_data()->pb.tables_size(); i++) {
          if (table_ids_to_remove.count(l->mutable_data()->pb.tables(i)) > 0) {
            l->mutable_data()->pb.mutable_tables()->DeleteSubrange(i, 1);
            --i;
          }
        }
        // Delete CDC stream config on the Producer.
        auto result = original_ri->GetOrCreateCDCRpcTasks(l->data().pb.producer_master_addresses());
        if (!result.ok()) {
          LOG(WARNING) << "Unable to create cdc rpc task. CDC streams won't be deleted: " << result;
        } else {
          auto s = (*result)->client()->DeleteCDCStream(streams_to_remove);
          if (!s.ok()) {
            std::stringstream os;
            std::copy(streams_to_remove.begin(), streams_to_remove.end(),
                      std::ostream_iterator<CDCStreamId>(os, ", "));
            LOG(WARNING) << "Unable to delete CDC streams: " << os.str() << s;
          }
        }
      }
      RETURN_NOT_OK(CheckStatus(
          sys_catalog_->UpdateItem(original_ri.get(), leader_ready_term()),
          "updating universe replication info in sys-catalog"));
      l->Commit();
    }
  } else if (req->producer_table_ids_to_add_size() > 0) {
    // 'add_table'
    string alter_producer_id = req->producer_id() + ".ALTER";

    // Verify no 'alter' command running.
    scoped_refptr<UniverseReplicationInfo> alter_ri;
    {
      std::shared_lock<LockType> l(lock_);
      alter_ri = FindPtrOrNull(universe_replication_map_, alter_producer_id);
    }
    {
      if (alter_ri != nullptr) {
        LOG(INFO) << "Found " << alter_producer_id << "... Removing";
        if (alter_ri->LockForRead()->data().is_deleted_or_failed()) {
          // Delete previous Alter if it's completed but failed.
          master::DeleteUniverseReplicationRequestPB delete_req;
          delete_req.set_producer_id(alter_ri->id());
          master::DeleteUniverseReplicationResponsePB delete_resp;
          Status s = DeleteUniverseReplication(&delete_req, &delete_resp, rpc);
          if (!s.ok()) {
            resp->mutable_error()->Swap(delete_resp.mutable_error());
            return s;
          }
        } else {
          return STATUS(InvalidArgument, "Alter for CDC producer currently running",
                        req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
        }
      }
    }
    // Only add new tables.  Ignore tables that are currently being replicated.
    auto tid_iter = req->producer_table_ids_to_add();
    unordered_set<string> new_tables(tid_iter.begin(), tid_iter.end());
    {
      auto l = original_ri->LockForRead();
      for(auto t : l->data().pb.tables()) {
        auto pos = new_tables.find(t);
        if (pos != new_tables.end()) {
          new_tables.erase(pos);
        }
      }
    }
    if (new_tables.empty()) {
      return STATUS(InvalidArgument, "CDC producer already contains all requested tables",
                    req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
    }

    // 1. create an ALTER table request that mirrors the original 'setup_replication'.
    master::SetupUniverseReplicationRequestPB setup_req;
    master::SetupUniverseReplicationResponsePB setup_resp;
    setup_req.set_producer_id(alter_producer_id);
    setup_req.mutable_producer_master_addresses()->CopyFrom(
        original_ri->LockForRead()->data().pb.producer_master_addresses());
    for (auto t : new_tables) {
      setup_req.add_producer_table_ids(t);
    }

    // 2. run the 'setup_replication' pipeline on the ALTER Table
    Status s = SetupUniverseReplication(&setup_req, &setup_resp, rpc);
    if (!s.ok()) {
      resp->mutable_error()->Swap(setup_resp.mutable_error());
      return s;
    }
    // NOTE: ALTER merges back into original after completion.
  }

  return Status::OK();
}

Status CatalogManager::GetUniverseReplication(const GetUniverseReplicationRequestPB* req,
                                              GetUniverseReplicationResponsePB* resp,
                                              rpc::RpcContext* rpc) {
  LOG(INFO) << "GetUniverseReplication from " << RequestorString(rpc)
            << ": " << req->DebugString();
  RETURN_NOT_OK(CheckOnline());

  if (!req->has_producer_id()) {
    return STATUS(InvalidArgument, "Producer universe ID must be provided",
                  req->ShortDebugString(), MasterError(MasterErrorPB::INVALID_REQUEST));
  }

  scoped_refptr<UniverseReplicationInfo> universe;
  {
    std::shared_lock<LockType> l(lock_);

    universe = FindPtrOrNull(universe_replication_map_, req->producer_id());
    if (universe == nullptr) {
      return STATUS(NotFound, "Could not find CDC producer universe",
                    req->ShortDebugString(), MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
    }
  }

  auto l = universe->LockForRead();
  resp->mutable_entry()->CopyFrom(l->data().pb);
  return Status::OK();
}

void CatalogManager::Started() {
  snapshot_coordinator_.Start();
}

} // namespace enterprise
}  // namespace master
}  // namespace yb
