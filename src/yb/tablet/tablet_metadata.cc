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

#include "yb/tablet/tablet_metadata.h"

#include <algorithm>
#include <mutex>
#include <string>

#include <gflags/gflags.h>
#include <boost/optional.hpp>
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/options.h"
#include "yb/common/entity_ids.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/gutil/atomicops.h"
#include "yb/gutil/bind.h"
#include "yb/gutil/dynamic_annotations.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/rocksutil/yb_rocksdb_logger.h"
#include "yb/server/metadata.h"
#include "yb/tablet/tablet_options.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/pb_util.h"
#include "yb/util/random.h"
#include "yb/util/status.h"
#include "yb/util/trace.h"

DEFINE_bool(enable_tablet_orphaned_block_deletion, true,
            "Whether to enable deletion of orphaned blocks from disk. "
            "Note: This is only exposed for debugging purposes!");
TAG_FLAG(enable_tablet_orphaned_block_deletion, advanced);
TAG_FLAG(enable_tablet_orphaned_block_deletion, hidden);
TAG_FLAG(enable_tablet_orphaned_block_deletion, runtime);

using std::shared_ptr;

using base::subtle::Barrier_AtomicIncrement;
using strings::Substitute;

using yb::consensus::MinimumOpId;

namespace yb {
namespace tablet {

const int64 kNoDurableMemStore = -1;
const std::string kIntentsSubdir = "intents";
const std::string kIntentsDBSuffix = ".intents";
const std::string kSnapshotsDirSuffix = ".snapshots";

// ============================================================================
//  Raft group metadata
// ============================================================================

TableInfo::TableInfo(std::string table_id,
                     std::string namespace_name,
                     std::string table_name,
                     TableType table_type,
                     const Schema& schema,
                     const IndexMap& index_map,
                     const boost::optional<IndexInfo>& index_info,
                     const uint32_t schema_version,
                     PartitionSchema partition_schema)
    : table_id(std::move(table_id)),
      namespace_name(std::move(namespace_name)),
      table_name(std::move(table_name)),
      table_type(table_type),
      schema(schema),
      index_map(index_map),
      index_info(index_info ? new IndexInfo(*index_info) : nullptr),
      schema_version(schema_version),
      partition_schema(std::move(partition_schema)) {
}

TableInfo::TableInfo(const TableInfo& other,
                     const Schema& schema,
                     const IndexMap& index_map,
                     const std::vector<DeletedColumn>& deleted_cols,
                     const uint32_t schema_version)
    : table_id(other.table_id),
      namespace_name(other.namespace_name),
      table_name(other.table_name),
      table_type(other.table_type),
      schema(schema),
      index_map(index_map),
      index_info(other.index_info ? new IndexInfo(*other.index_info) : nullptr),
      schema_version(schema_version),
      partition_schema(other.partition_schema),
      deleted_cols(other.deleted_cols) {
  this->deleted_cols.insert(this->deleted_cols.end(), deleted_cols.begin(), deleted_cols.end());
}

Status TableInfo::LoadFromPB(const TableInfoPB& pb) {
  table_id = pb.table_id();
  namespace_name = pb.namespace_name();
  table_name = pb.table_name();
  table_type = pb.table_type();

  RETURN_NOT_OK(SchemaFromPB(pb.schema(), &schema));
  if (pb.has_index_info()) {
    index_info.reset(new IndexInfo(pb.index_info()));
  }
  index_map.FromPB(pb.indexes());
  schema_version = pb.schema_version();

  RETURN_NOT_OK(PartitionSchema::FromPB(pb.partition_schema(), schema, &partition_schema));

  for (const DeletedColumnPB& deleted_col : pb.deleted_cols()) {
    DeletedColumn col;
    RETURN_NOT_OK(DeletedColumn::FromPB(deleted_col, &col));
    deleted_cols.push_back(col);
  }

  return Status::OK();
}

void TableInfo::ToPB(TableInfoPB* pb) const {
  pb->set_table_id(table_id);
  pb->set_namespace_name(namespace_name);
  pb->set_table_name(table_name);
  pb->set_table_type(table_type);

  DCHECK(schema.has_column_ids());
  SchemaToPB(schema, pb->mutable_schema());
  if (index_info) {
    index_info->ToPB(pb->mutable_index_info());
  }
  index_map.ToPB(pb->mutable_indexes());
  pb->set_schema_version(schema_version);

  partition_schema.ToPB(pb->mutable_partition_schema());

  for (const DeletedColumn& deleted_col : deleted_cols) {
    deleted_col.CopyToPB(pb->mutable_deleted_cols()->Add());
  }
}

Status KvStoreInfo::LoadTablesFromPB(
    google::protobuf::RepeatedPtrField<TableInfoPB> pbs, TableId primary_table_id) {
  tables.clear();
  for (const auto& table_pb : pbs) {
    auto table_info = std::make_shared<TableInfo>();
    RETURN_NOT_OK(table_info->LoadFromPB(table_pb));
    if (table_info->table_id != primary_table_id) {
      if (table_pb.schema().table_properties().is_ysql_catalog_table()) {
        Uuid cotable_id;
        CHECK_OK(cotable_id.FromHexString(table_info->table_id));
        // TODO(#79): when adding for multiple KV-stores per Raft group support - check if we need
        // to set cotable ID.
        table_info->schema.set_cotable_id(cotable_id);
      } else {
        auto pgtable_id = VERIFY_RESULT(GetPgsqlTableOid(table_info->table_id));
        table_info->schema.set_pgtable_id(pgtable_id);
      }
    }
    tables[table_info->table_id] = std::move(table_info);
  }
  return Status::OK();
}

Status KvStoreInfo::LoadFromPB(const KvStoreInfoPB& pb, TableId primary_table_id) {
  kv_store_id = KvStoreId(pb.kv_store_id());
  rocksdb_dir = pb.rocksdb_dir();
  lower_bound_key = pb.lower_bound_key();
  upper_bound_key = pb.upper_bound_key();
  has_been_fully_compacted = pb.has_been_fully_compacted();
  return LoadTablesFromPB(pb.tables(), primary_table_id);
}

void KvStoreInfo::ToPB(TableId primary_table_id, KvStoreInfoPB* pb) const {
  pb->set_kv_store_id(kv_store_id.ToString());
  pb->set_rocksdb_dir(rocksdb_dir);
  if (lower_bound_key.empty()) {
    pb->clear_lower_bound_key();
  } else {
    pb->set_lower_bound_key(lower_bound_key);
  }
  if (upper_bound_key.empty()) {
    pb->clear_upper_bound_key();
  } else {
    pb->set_upper_bound_key(upper_bound_key);
  }
  pb->set_has_been_fully_compacted(has_been_fully_compacted);

  // Putting primary table first, then all other tables.
  const auto& it = tables.find(primary_table_id);
  if (it != tables.end()) {
    it->second->ToPB(pb->add_tables());
  }
  for (const auto& it : tables) {
    if (it.first != primary_table_id) {
      it.second->ToPB(pb->add_tables());
    }
  }
}

namespace {

std::string MakeTabletDirName(const TabletId& tablet_id) {
  return Format("tablet-$0", tablet_id);
}

} // namespace

// ============================================================================

Status RaftGroupMetadata::CreateNew(FsManager* fs_manager,
                                 const TableId& table_id,
                                 const RaftGroupId& raft_group_id,
                                 const string& namespace_name,
                                 const string& table_name,
                                 const TableType table_type,
                                 const Schema& schema,
                                 const IndexMap& index_map,
                                 const PartitionSchema& partition_schema,
                                 const Partition& partition,
                                 const boost::optional<IndexInfo>& index_info,
                                 const uint32_t schema_version,
                                 const TabletDataState& initial_tablet_data_state,
                                 RaftGroupMetadataPtr* metadata,
                                 const string& data_root_dir,
                                 const string& wal_root_dir,
                                 const bool colocated) {

  // Verify that no existing Raft group exists with the same ID.
  if (fs_manager->env()->FileExists(fs_manager->GetRaftGroupMetadataPath(raft_group_id))) {
    return STATUS(AlreadyPresent, "Raft group already exists", raft_group_id);
  }

  auto wal_top_dir = wal_root_dir;
  auto data_top_dir = data_root_dir;
  // Use the original randomized logic if the indices are not explicitly passed in
  yb::Random rand(GetCurrentTimeMicros());
  if (data_root_dir.empty()) {
    auto data_root_dirs = fs_manager->GetDataRootDirs();
    CHECK(!data_root_dirs.empty()) << "No data root directories found";
    data_top_dir = data_root_dirs[rand.Uniform(data_root_dirs.size())];
  }

  if (wal_root_dir.empty()) {
    auto wal_root_dirs = fs_manager->GetWalRootDirs();
    CHECK(!wal_root_dirs.empty()) << "No wal root directories found";
    wal_top_dir = wal_root_dirs[rand.Uniform(wal_root_dirs.size())];
  }

  const string table_dir_name = Substitute("table-$0", table_id);
  const string tablet_dir_name = MakeTabletDirName(raft_group_id);
  const string wal_dir = JoinPathSegments(wal_top_dir, table_dir_name, tablet_dir_name);
  const string rocksdb_dir = JoinPathSegments(
      data_top_dir, FsManager::kRocksDBDirName, table_dir_name, tablet_dir_name);

  RaftGroupMetadataPtr ret(new RaftGroupMetadata(fs_manager,
                                                       table_id,
                                                       raft_group_id,
                                                       namespace_name,
                                                       table_name,
                                                       table_type,
                                                       rocksdb_dir,
                                                       wal_dir,
                                                       schema,
                                                       index_map,
                                                       partition_schema,
                                                       partition,
                                                       index_info,
                                                       schema_version,
                                                       initial_tablet_data_state,
                                                       colocated));
  RETURN_NOT_OK(ret->Flush());
  metadata->swap(ret);
  return Status::OK();
}

Status RaftGroupMetadata::Load(FsManager* fs_manager,
                            const RaftGroupId& raft_group_id,
                            RaftGroupMetadataPtr* metadata) {
  RaftGroupMetadataPtr ret(new RaftGroupMetadata(fs_manager, raft_group_id));
  RETURN_NOT_OK(ret->LoadFromDisk());
  metadata->swap(ret);
  return Status::OK();
}

Status RaftGroupMetadata::LoadOrCreate(FsManager* fs_manager,
                                    const string& table_id,
                                    const RaftGroupId& raft_group_id,
                                    const string& namespace_name,
                                    const string& table_name,
                                    TableType table_type,
                                    const Schema& schema,
                                    const PartitionSchema& partition_schema,
                                    const Partition& partition,
                                    const boost::optional<IndexInfo>& index_info,
                                    const TabletDataState& initial_tablet_data_state,
                                    RaftGroupMetadataPtr* metadata) {
  Status s = Load(fs_manager, raft_group_id, metadata);
  if (s.ok()) {
    if (!(**metadata).schema()->Equals(schema)) {
      return STATUS(Corruption, Substitute("Schema on disk ($0) does not "
        "match expected schema ($1)", (*metadata)->schema()->ToString(),
        schema.ToString()));
    }
    return Status::OK();
  } else if (s.IsNotFound()) {
    return CreateNew(
        fs_manager, table_id, raft_group_id, namespace_name, table_name, table_type, schema,
        IndexMap(), partition_schema, partition, index_info, 0 /* schema_version */,
        initial_tablet_data_state, metadata);
  } else {
    return s;
  }
}

template <class TablesMap>
CHECKED_STATUS MakeTableNotFound(const TableId& table_id, const RaftGroupId& raft_group_id,
                                 const TablesMap& tables) {
#ifndef NDEBUG
  // This very large message should be logged instead of being appended to STATUS.
  std::string suffix = Format(". Tables: $0.", tables);
  VLOG(1) << "Table " << table_id << " not found in Raft group " << raft_group_id << suffix;
#endif
  return STATUS_FORMAT(NotFound, "Table $0 not found in Raft group $1", table_id, raft_group_id);
}

Result<TableInfoPtr> RaftGroupMetadata::GetTableInfo(const std::string& table_id) const {
  std::lock_guard<MutexType> lock(data_mutex_);
  return GetTableInfoUnlocked(table_id);
}

Result<TableInfoPtr> RaftGroupMetadata::GetTableInfoUnlocked(const std::string& table_id) const {
  const auto& tables = kv_store_.tables;
  const auto id = !table_id.empty() ? table_id : primary_table_id_;
  const auto iter = tables.find(id);
  if (iter == tables.end()) {
    return MakeTableNotFound(table_id, raft_group_id_, tables);
  }
  return iter->second;
}

Status RaftGroupMetadata::DeleteTabletData(TabletDataState delete_type,
                                           const yb::OpId& last_logged_opid) {
  CHECK(delete_type == TABLET_DATA_DELETED ||
        delete_type == TABLET_DATA_TOMBSTONED)
      << "DeleteTabletData() called with unsupported delete_type on tablet "
      << raft_group_id_ << ": " << TabletDataState_Name(delete_type)
      << " (" << delete_type << ")";

  // First add all of our blocks to the orphan list
  // and clear our rowsets. This serves to erase all the data.
  //
  // We also set the state in our persisted metadata to indicate that
  // we have been deleted.
  {
    std::lock_guard<MutexType> lock(data_mutex_);
    tablet_data_state_ = delete_type;
    if (last_logged_opid) {
      tombstone_last_logged_opid_ = last_logged_opid;
    }
  }

  rocksdb::Options rocksdb_options;
  TabletOptions tablet_options;
  std::string log_prefix = consensus::MakeTabletLogPrefix(raft_group_id_, fs_manager_->uuid());
  docdb::InitRocksDBOptions(
      &rocksdb_options, log_prefix, nullptr /* statistics */, tablet_options);

  const auto& rocksdb_dir = this->rocksdb_dir();
  LOG(INFO) << "Destroying regular db at: " << rocksdb_dir;
  rocksdb::Status status = rocksdb::DestroyDB(rocksdb_dir, rocksdb_options);

  if (!status.ok()) {
    LOG(ERROR) << "Failed to destroy regular DB at: " << rocksdb_dir << ": " << status;
  } else {
    LOG(INFO) << "Successfully destroyed regular DB at: " << rocksdb_dir;
  }

  if (fs_manager_->env()->FileExists(rocksdb_dir)) {
    auto s = fs_manager_->env()->DeleteRecursively(rocksdb_dir);
    LOG_IF(WARNING, !s.ok()) << "Unable to delete rocksdb data directory " << rocksdb_dir;
  }

  const auto intents_dir = this->intents_rocksdb_dir();
  if (fs_manager_->env()->FileExists(intents_dir)) {
    status = rocksdb::DestroyDB(intents_dir, rocksdb_options);

    if (!status.ok()) {
      LOG(ERROR) << "Failed to destroy provisional records DB at: " << intents_dir << ": "
                 << status;
    } else {
      LOG(INFO) << "Successfully destroyed provisional records DB at: " << intents_dir;
    }
  }

  if (fs_manager_->env()->FileExists(intents_dir)) {
    auto s = fs_manager_->env()->DeleteRecursively(intents_dir);
    LOG_IF(WARNING, !s.ok()) << "Unable to delete intents directory " << intents_dir;
  }

  const auto snapshots_dir = this->snapshots_dir();
  if (fs_manager_->env()->FileExists(snapshots_dir)) {
    auto s = fs_manager_->env()->DeleteRecursively(snapshots_dir);
    LOG_IF(WARNING, !s.ok()) << "Unable to delete snapshots directory " << snapshots_dir;
  }

  // Flushing will sync the new tablet_data_state_ to disk and will now also
  // delete all the data.
  RETURN_NOT_OK(Flush());

  // Re-sync to disk one more time.
  // This call will typically re-sync with an empty orphaned blocks list
  // (unless deleting any orphans failed during the last Flush()), so that we
  // don't try to re-delete the deleted orphaned blocks on every startup.
  return Flush();
}

bool RaftGroupMetadata::IsTombstonedWithNoRocksDBData() const {
  std::lock_guard<MutexType> lock(data_mutex_);
  const auto& rocksdb_dir = kv_store_.rocksdb_dir;
  const auto intents_dir = rocksdb_dir + kIntentsDBSuffix;
  return tablet_data_state_ == TABLET_DATA_TOMBSTONED &&
      !fs_manager_->env()->FileExists(rocksdb_dir) &&
      !fs_manager_->env()->FileExists(intents_dir);
}

Status RaftGroupMetadata::DeleteSuperBlock() {
  std::lock_guard<MutexType> lock(data_mutex_);
  if (tablet_data_state_ != TABLET_DATA_DELETED) {
    return STATUS(IllegalState,
        Substitute("Tablet $0 is not in TABLET_DATA_DELETED state. "
                   "Call DeleteTabletData(TABLET_DATA_DELETED) first. "
                   "Tablet data state: $1 ($2)",
                   raft_group_id_,
                   TabletDataState_Name(tablet_data_state_),
                   tablet_data_state_));
  }

  string path = fs_manager_->GetRaftGroupMetadataPath(raft_group_id_);
  RETURN_NOT_OK_PREPEND(fs_manager_->env()->DeleteFile(path),
                        "Unable to delete superblock for Raft group " + raft_group_id_);
  return Status::OK();
}

RaftGroupMetadata::RaftGroupMetadata(FsManager* fs_manager,
                               TableId table_id,
                               RaftGroupId raft_group_id,
                               string namespace_name,
                               string table_name,
                               TableType table_type,
                               const string rocksdb_dir,
                               const string wal_dir,
                               const Schema& schema,
                               const IndexMap& index_map,
                               PartitionSchema partition_schema,
                               Partition partition,
                               const boost::optional<IndexInfo>& index_info,
                               const uint32_t schema_version,
                               const TabletDataState& tablet_data_state,
                               const bool colocated)
    : state_(kNotWrittenYet),
      raft_group_id_(std::move(raft_group_id)),
      partition_(std::make_shared<Partition>(std::move(partition))),
      primary_table_id_(table_id),
      kv_store_(KvStoreId(raft_group_id), rocksdb_dir),
      fs_manager_(fs_manager),
      wal_dir_(wal_dir),
      tablet_data_state_(tablet_data_state),
      colocated_(colocated),
      cdc_min_replicated_index_(std::numeric_limits<int64_t>::max()) {
  CHECK(schema.has_column_ids());
  CHECK_GT(schema.num_key_columns(), 0);
  kv_store_.tables.emplace(
      primary_table_id_,
      std::make_shared<TableInfo>(
          std::move(table_id),
          std::move(namespace_name),
          std::move(table_name),
          table_type,
          schema,
          index_map,
          index_info,
          schema_version,
          std::move(partition_schema)));
}

RaftGroupMetadata::~RaftGroupMetadata() {
}

RaftGroupMetadata::RaftGroupMetadata(FsManager* fs_manager, RaftGroupId raft_group_id)
    : state_(kNotLoadedYet),
      raft_group_id_(std::move(raft_group_id)),
      kv_store_(KvStoreId(raft_group_id)),
      fs_manager_(fs_manager) {
}

Status RaftGroupMetadata::LoadFromDisk() {
  TRACE_EVENT1("raft_group", "RaftGroupMetadata::LoadFromDisk",
               "raft_group_id", raft_group_id_);

  CHECK_EQ(state_, kNotLoadedYet);

  RaftGroupReplicaSuperBlockPB superblock;
  RETURN_NOT_OK(ReadSuperBlockFromDisk(&superblock));
  RETURN_NOT_OK_PREPEND(LoadFromSuperBlock(superblock),
                        "Failed to load data from superblock protobuf");
  state_ = kInitialized;
  return Status::OK();
}

Status RaftGroupMetadata::LoadFromSuperBlock(const RaftGroupReplicaSuperBlockPB& superblock) {
  if (!superblock.has_kv_store()) {
    // Backward compatibility for tablet=KV-store=raft-group.
    RaftGroupReplicaSuperBlockPB superblock_migrated(superblock);
    RETURN_NOT_OK(MigrateSuperblock(&superblock_migrated));
    RETURN_NOT_OK(LoadFromSuperBlock(superblock_migrated));
    return Flush();
  }

  VLOG(2) << "Loading RaftGroupMetadata from SuperBlockPB:" << std::endl
          << superblock.DebugString();

  {
    std::lock_guard<MutexType> lock(data_mutex_);

    // Verify that the Raft group id matches with the one in the protobuf.
    if (superblock.raft_group_id() != raft_group_id_) {
      return STATUS(Corruption, "Expected id=" + raft_group_id_ +
                                " found " + superblock.raft_group_id(),
                                superblock.DebugString());
    }
    Partition partition;
    Partition::FromPB(superblock.partition(), &partition);
    partition_ = std::make_shared<Partition>(partition);
    primary_table_id_ = superblock.primary_table_id();
    colocated_ = superblock.colocated();

    RETURN_NOT_OK(kv_store_.LoadFromPB(superblock.kv_store(), primary_table_id_));

    wal_dir_ = superblock.wal_dir();
    tablet_data_state_ = superblock.tablet_data_state();

    if (superblock.has_tombstone_last_logged_opid()) {
      tombstone_last_logged_opid_ = yb::OpId::FromPB(superblock.tombstone_last_logged_opid());
    } else {
      tombstone_last_logged_opid_ = OpId();
    }
    cdc_min_replicated_index_ = superblock.cdc_min_replicated_index();
  }

  return Status::OK();
}

Status RaftGroupMetadata::Flush() {
  TRACE_EVENT1("raft_group", "RaftGroupMetadata::Flush",
               "raft_group_id", raft_group_id_);

  MutexLock l_flush(flush_lock_);
  RaftGroupReplicaSuperBlockPB pb;
  {
    std::lock_guard<MutexType> lock(data_mutex_);
    ToSuperBlockUnlocked(&pb);
  }
  RETURN_NOT_OK(ReplaceSuperBlockUnlocked(pb));
  TRACE("Metadata flushed");

  return Status::OK();
}

Status RaftGroupMetadata::ReplaceSuperBlock(const RaftGroupReplicaSuperBlockPB &pb) {
  {
    MutexLock l(flush_lock_);
    RETURN_NOT_OK_PREPEND(ReplaceSuperBlockUnlocked(pb), "Unable to replace superblock");
  }

  RETURN_NOT_OK_PREPEND(LoadFromSuperBlock(pb),
                        "Failed to load data from superblock protobuf");

  return Status::OK();
}

Status RaftGroupMetadata::ReplaceSuperBlockUnlocked(const RaftGroupReplicaSuperBlockPB &pb) {
  flush_lock_.AssertAcquired();

  string path = fs_manager_->GetRaftGroupMetadataPath(raft_group_id_);
  RETURN_NOT_OK_PREPEND(pb_util::WritePBContainerToPath(
                            fs_manager_->env(), path, pb,
                            pb_util::OVERWRITE, pb_util::SYNC),
                        Substitute("Failed to write Raft group metadata $0", raft_group_id_));

  return Status::OK();
}

Status RaftGroupMetadata::ReadSuperBlockFromDisk(RaftGroupReplicaSuperBlockPB* superblock) const {
  string path = fs_manager_->GetRaftGroupMetadataPath(raft_group_id_);
  RETURN_NOT_OK_PREPEND(
      pb_util::ReadPBContainerFromPath(fs_manager_->env(), path, superblock),
      Substitute("Could not load Raft group metadata from $0", path));
  // Migration for backward compatibility with versions which don't have separate
  // TableType::TRANSACTION_STATUS_TABLE_TYPE.
  if (superblock->obsolete_table_type() == TableType::REDIS_TABLE_TYPE &&
      superblock->obsolete_table_name() == kTransactionsTableName) {
    superblock->set_obsolete_table_type(TableType::TRANSACTION_STATUS_TABLE_TYPE);
  }
  return Status::OK();
}

void RaftGroupMetadata::ToSuperBlock(RaftGroupReplicaSuperBlockPB* superblock) const {
  // acquire the lock so that rowsets_ doesn't get changed until we're finished.
  std::lock_guard<MutexType> lock(data_mutex_);
  ToSuperBlockUnlocked(superblock);
}

void RaftGroupMetadata::ToSuperBlockUnlocked(RaftGroupReplicaSuperBlockPB* superblock) const {
  DCHECK(data_mutex_.is_locked());
  // Convert to protobuf.
  RaftGroupReplicaSuperBlockPB pb;
  pb.set_raft_group_id(raft_group_id_);
  partition_->ToPB(pb.mutable_partition());

  kv_store_.ToPB(primary_table_id_, pb.mutable_kv_store());

  pb.set_wal_dir(wal_dir_);
  pb.set_tablet_data_state(tablet_data_state_);
  if (tombstone_last_logged_opid_) {
    tombstone_last_logged_opid_.ToPB(pb.mutable_tombstone_last_logged_opid());
  }

  pb.set_primary_table_id(primary_table_id_);
  pb.set_colocated(colocated_);
  pb.set_cdc_min_replicated_index(cdc_min_replicated_index_);

  superblock->Swap(&pb);
}

void RaftGroupMetadata::SetSchema(const Schema& schema,
                                  const IndexMap& index_map,
                                  const std::vector<DeletedColumn>& deleted_cols,
                                  const uint32_t version,
                                  const TableId& table_id) {
  DCHECK(schema.has_column_ids());
  std::lock_guard<MutexType> lock(data_mutex_);
  TableId target_table_id = table_id.empty() ? primary_table_id_ : table_id;
  auto result = GetTableInfoUnlocked(target_table_id);
  DCHECK(result.ok());
  TableInfoPtr new_table_info = std::make_shared<TableInfo>(*result.get(),
                                                            schema,
                                                            index_map,
                                                            deleted_cols,
                                                            version);
  if (target_table_id != primary_table_id_) {
    if (schema.table_properties().is_ysql_catalog_table()) {
      Uuid cotable_id;
      CHECK_OK(cotable_id.FromHexString(target_table_id));
      new_table_info->schema.set_cotable_id(cotable_id);
    } else {
      auto result = CHECK_RESULT(GetPgsqlTableOid(target_table_id));
      new_table_info->schema.set_pgtable_id(result);
    }
  }
  VLOG_WITH_PREFIX(1) << raft_group_id_ << " Updating table " << target_table_id
                      << " to Schema version " << version
                      << " from \n" << yb::ToString(kv_store_.tables[target_table_id])
                      << " to \n" << yb::ToString(new_table_info);
  kv_store_.tables[target_table_id].swap(new_table_info);
}

void RaftGroupMetadata::SetPartitionSchema(const PartitionSchema& partition_schema) {
  std::lock_guard<MutexType> lock(data_mutex_);
  auto& tables = kv_store_.tables;
  DCHECK(tables.find(primary_table_id_) != tables.end());
  tables[primary_table_id_]->partition_schema = partition_schema;
}

void RaftGroupMetadata::SetTableName(
    const string& namespace_name, const string& table_name, const TableId& table_id) {
  std::lock_guard<MutexType> lock(data_mutex_);
  auto& tables = kv_store_.tables;
  auto& id = table_id.empty() ? primary_table_id_ : table_id;
  DCHECK(tables.find(id) != tables.end());
  tables[id]->namespace_name = namespace_name;
  tables[id]->table_name = table_name;
}

void RaftGroupMetadata::AddTable(const std::string& table_id,
                              const std::string& namespace_name,
                              const std::string& table_name,
                              const TableType table_type,
                              const Schema& schema,
                              const IndexMap& index_map,
                              const PartitionSchema& partition_schema,
                              const boost::optional<IndexInfo>& index_info,
                              const uint32_t schema_version) {
  DCHECK(schema.has_column_ids());
  TableInfoPtr new_table_info = std::make_shared<TableInfo>(table_id,
                                                            namespace_name,
                                                            table_name,
                                                            table_type,
                                                            schema,
                                                            index_map,
                                                            index_info,
                                                            schema_version,
                                                            partition_schema);
  if (table_id != primary_table_id_) {
    if (schema.table_properties().is_ysql_catalog_table()) {
      Uuid cotable_id;
      CHECK_OK(cotable_id.FromHexString(table_id));
      new_table_info->schema.set_cotable_id(cotable_id);
    } else {
      auto result = CHECK_RESULT(GetPgsqlTableOid(table_id));
      new_table_info->schema.set_pgtable_id(result);
    }
  }
  std::lock_guard<MutexType> lock(data_mutex_);
  auto& tables = kv_store_.tables;
  auto existing_table_iter = tables.find(table_id);
  if (existing_table_iter != tables.end()) {
    const auto& existing_table = *existing_table_iter->second.get();
    if (!existing_table.schema.table_properties().is_ysql_catalog_table() &&
        schema.table_properties().is_ysql_catalog_table()) {
      // This must be the one-time migration with transactional DDL being turned on for the first
      // time on this cluster.
    } else {
      LOG(DFATAL) << "Table " << table_id << " already exists. New table info: "
          << new_table_info->ToString() << ", old table info: " << existing_table.ToString();
    }
  }
  VLOG_WITH_PREFIX(1) << "Updating to Schema version " << schema_version
                      << " from\n" << yb::ToString(tables[table_id])
                      << "\nto\n" << yb::ToString(new_table_info);
  tables[table_id].swap(new_table_info);
}

void RaftGroupMetadata::RemoveTable(const TableId& table_id) {
  std::lock_guard<MutexType> lock(data_mutex_);
  auto& tables = kv_store_.tables;
  tables.erase(table_id);
}

string RaftGroupMetadata::data_root_dir() const {
  const auto& rocksdb_dir = kv_store_.rocksdb_dir;
  if (rocksdb_dir.empty()) {
    return "";
  } else {
    auto data_root_dir = DirName(DirName(rocksdb_dir));
    if (strcmp(BaseName(data_root_dir).c_str(), FsManager::kRocksDBDirName) == 0) {
      data_root_dir = DirName(data_root_dir);
    }
    return data_root_dir;
  }
}

string RaftGroupMetadata::wal_root_dir() const {
  if (wal_dir_.empty()) {
    return "";
  } else {
    auto wal_root_dir = DirName(wal_dir_);
    if (strcmp(BaseName(wal_root_dir).c_str(), FsManager::kWalDirName) != 0) {
      wal_root_dir = DirName(wal_root_dir);
    }
    return wal_root_dir;
  }
}

void RaftGroupMetadata::set_wal_retention_secs(uint32 wal_retention_secs) {
  std::lock_guard<MutexType> lock(data_mutex_);
  auto it = kv_store_.tables.find(primary_table_id_);
  if (it == kv_store_.tables.end()) {
    LOG_WITH_PREFIX(DFATAL) << "Unable to set WAL retention time for primary table "
                            << primary_table_id_;
    return;
  }
  it->second->wal_retention_secs = wal_retention_secs;
  LOG_WITH_PREFIX(INFO) << "Set RaftGroupMetadata wal retention time to "
                        << wal_retention_secs << " seconds";
}

uint32_t RaftGroupMetadata::wal_retention_secs() const {
  std::lock_guard<MutexType> lock(data_mutex_);
  auto it = kv_store_.tables.find(primary_table_id_);
  if (it == kv_store_.tables.end()) {
    return 0;
  }
  return it->second->wal_retention_secs;
}

Status RaftGroupMetadata::set_cdc_min_replicated_index(int64 cdc_min_replicated_index) {
  {
    std::lock_guard<MutexType> lock(data_mutex_);
    cdc_min_replicated_index_ = cdc_min_replicated_index;
  }
  return Flush();
}

int64_t RaftGroupMetadata::cdc_min_replicated_index() const {
  std::lock_guard<MutexType> lock(data_mutex_);
  return cdc_min_replicated_index_;
}

void RaftGroupMetadata::set_tablet_data_state(TabletDataState state) {
  std::lock_guard<MutexType> lock(data_mutex_);
  tablet_data_state_ = state;
}

string RaftGroupMetadata::LogPrefix() const {
  return consensus::MakeTabletLogPrefix(raft_group_id_, fs_manager_->uuid());
}

TabletDataState RaftGroupMetadata::tablet_data_state() const {
  std::lock_guard<MutexType> lock(data_mutex_);
  return tablet_data_state_;
}

std::string RaftGroupMetadata::GetSubRaftGroupWalDir(const RaftGroupId& raft_group_id) const {
  return JoinPathSegments(DirName(wal_dir_), MakeTabletDirName(raft_group_id));
}

std::string RaftGroupMetadata::GetSubRaftGroupDataDir(const RaftGroupId& raft_group_id) const {
  return JoinPathSegments(DirName(kv_store_.rocksdb_dir), MakeTabletDirName(raft_group_id));
}

Result<RaftGroupMetadataPtr> RaftGroupMetadata::CreateSubtabletMetadata(
    const RaftGroupId& raft_group_id, const Partition& partition,
    const std::string& lower_bound_key, const std::string& upper_bound_key) const {
  RaftGroupReplicaSuperBlockPB superblock;
  ToSuperBlock(&superblock);

  RaftGroupMetadataPtr metadata(new RaftGroupMetadata(fs_manager_, raft_group_id_));
  RETURN_NOT_OK(metadata->LoadFromSuperBlock(superblock));
  metadata->raft_group_id_ = raft_group_id;
  metadata->wal_dir_ = GetSubRaftGroupWalDir(raft_group_id);
  metadata->kv_store_.lower_bound_key = lower_bound_key;
  metadata->kv_store_.upper_bound_key = upper_bound_key;
  metadata->kv_store_.rocksdb_dir = GetSubRaftGroupDataDir(raft_group_id);
  metadata->kv_store_.has_been_fully_compacted = false;
  *metadata->partition_ = partition;
  metadata->state_ = kInitialized;
  metadata->tablet_data_state_ = TABLET_DATA_UNKNOWN;
  RETURN_NOT_OK(metadata->Flush());
  return metadata;
}


namespace {
// MigrateSuperblockForDXXXX functions are only needed for backward compatibility with
// YugabyteDB versions which don't have changes from DXXXX revision.
// Each MigrateSuperblockForDXXXX could be removed after all YugabyteDB installations are
// upgraded to have revision DXXXX.

CHECKED_STATUS MigrateSuperblockForD5900(RaftGroupReplicaSuperBlockPB* superblock) {
  // In previous version of superblock format we stored primary table metadata in superblock's
  // top-level fields (deprecated table_* and other). TableInfo objects were stored inside
  // RaftGroupReplicaSuperBlockPB.tables.
  //
  // In new format TableInfo objects and some other top-level fields are moved from superblock's
  // top-level fields into RaftGroupReplicaSuperBlockPB.kv_store. Primary table (see
  // RaftGroupMetadata::primary_table_id_ field description) metadata is stored inside one of
  // RaftGroupReplicaSuperBlockPB.kv_store.tables objects and is referenced by
  // RaftGroupReplicaSuperBlockPB.primary_table_id.
  if (superblock->has_kv_store()) {
    return Status::OK();
  }

  LOG(INFO) << "Migrating superblock for raft group " << superblock->raft_group_id();

  KvStoreInfoPB* kv_store_pb = superblock->mutable_kv_store();
  kv_store_pb->set_kv_store_id(superblock->raft_group_id());
  kv_store_pb->set_rocksdb_dir(superblock->obsolete_rocksdb_dir());
  kv_store_pb->mutable_rocksdb_files()->CopyFrom(superblock->obsolete_rocksdb_files());
  kv_store_pb->mutable_snapshot_files()->CopyFrom(superblock->obsolete_snapshot_files());

  TableInfoPB* primary_table = kv_store_pb->add_tables();
  primary_table->set_table_id(superblock->primary_table_id());
  primary_table->set_table_name(superblock->obsolete_table_name());
  primary_table->set_table_type(superblock->obsolete_table_type());
  primary_table->mutable_schema()->CopyFrom(superblock->obsolete_schema());
  primary_table->set_schema_version(superblock->obsolete_schema_version());
  primary_table->mutable_partition_schema()->CopyFrom(superblock->obsolete_partition_schema());
  primary_table->mutable_indexes()->CopyFrom(superblock->obsolete_indexes());
  primary_table->mutable_index_info()->CopyFrom(superblock->obsolete_index_info());
  primary_table->mutable_deleted_cols()->CopyFrom(superblock->obsolete_deleted_cols());

  kv_store_pb->mutable_tables()->MergeFrom(superblock->obsolete_tables());

  return Status::OK();
}

} // namespace

Status MigrateSuperblock(RaftGroupReplicaSuperBlockPB* superblock) {
  return MigrateSuperblockForD5900(superblock);
}

} // namespace tablet
} // namespace yb
