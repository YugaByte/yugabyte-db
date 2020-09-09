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
#ifndef YB_TABLET_TABLET_METADATA_H
#define YB_TABLET_TABLET_METADATA_H

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <boost/optional/optional_fwd.hpp>

#include "yb/common/entity_ids.h"
#include "yb/common/index.h"
#include "yb/common/partition.h"
#include "yb/common/schema.h"
#include "yb/consensus/opid_util.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/callback.h"
#include "yb/gutil/dynamic_annotations.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"

#include "yb/tablet/metadata.pb.h"
#include "yb/tablet/tablet_fwd.h"

#include "yb/util/mutex.h"
#include "yb/util/opid.h"
#include "yb/util/opid.pb.h"
#include "yb/util/status.h"
#include "yb/util/status_callback.h"

namespace yb {
namespace tablet {

extern const int64 kNoDurableMemStore;
extern const std::string kIntentsSubdir;
extern const std::string kIntentsDBSuffix;
extern const std::string kSnapshotsDirSuffix;

  // Table info.
struct TableInfo {
  // Table id, name and type.
  std::string table_id;
  std::string namespace_name;
  std::string table_name;
  TableType table_type;

  // The table schema, secondary index map, index info (for index table only) and schema version.
  Schema schema;
  IndexMap index_map;
  std::unique_ptr<IndexInfo> index_info;
  uint32_t schema_version = 0;

  // Partition schema of the table.
  PartitionSchema partition_schema;

  // A vector of column IDs that have been deleted, so that the compaction filter can free the
  // associated memory. As of 01/2019, deleted column IDs are persisted forever, even if all the
  // associated data has been discarded. In the future, we can garbage collect such column IDs to
  // make sure this vector doesn't grow too large.
  std::vector<DeletedColumn> deleted_cols;

  // We use the retention time from the primary table.
  uint32_t wal_retention_secs = 0;

  TableInfo() = default;
  TableInfo(std::string table_id,
            std::string namespace_name,
            std::string table_name,
            TableType table_type,
            const Schema& schema,
            const IndexMap& index_map,
            const boost::optional<IndexInfo>& index_info,
            uint32_t schema_version,
            PartitionSchema partition_schema);
  TableInfo(const TableInfo& other,
            const Schema& schema,
            const IndexMap& index_map,
            const std::vector<DeletedColumn>& deleted_cols,
            uint32_t schema_version);

  CHECKED_STATUS LoadFromPB(const TableInfoPB& pb);
  void ToPB(TableInfoPB* pb) const;

  std::string ToString() const {
    TableInfoPB pb;
    ToPB(&pb);
    return pb.ShortDebugString();
  }
};

// Describes KV-store. Single KV-store is backed by one or two RocksDB instances, depending on
// whether distributed transactions are enabled for the table. KV-store for sys catalog could
// contain multiple tables.
struct KvStoreInfo {
  explicit KvStoreInfo(const KvStoreId& kv_store_id_) : kv_store_id(kv_store_id_) {}

  KvStoreInfo(const KvStoreId& kv_store_id_, const std::string& rocksdb_dir_)
      : kv_store_id(kv_store_id_),
        rocksdb_dir(rocksdb_dir_) {}

  CHECKED_STATUS LoadFromPB(const KvStoreInfoPB& pb, TableId primary_table_id);

  CHECKED_STATUS LoadTablesFromPB(
      google::protobuf::RepeatedPtrField<TableInfoPB> pbs, TableId primary_table_id);

  void ToPB(TableId primary_table_id, KvStoreInfoPB* pb) const;

  KvStoreId kv_store_id;

  // The directory where the regular RocksDB data for this KV-store is stored. For KV-stores having
  // tables with distributed transactions enabled an additional RocksDB is created in directory at
  // `rocksdb_dir + kIntentsDBSuffix` path.
  std::string rocksdb_dir;

  // Optional inclusive lower bound and exclusive upper bound for keys served by this KV-store.
  // See docdb::KeyBounds.
  std::string lower_bound_key;
  std::string upper_bound_key;

  // See KvStoreInfoPB field with the same name.
  bool has_been_fully_compacted = false;

  // Map of tables sharing this KV-store indexed by the table id.
  // If pieces of the same table live in the same Raft group they should be located in different
  // KV-stores.
  std::unordered_map<TableId, TableInfoPtr> tables;
};

// At startup, the TSTabletManager will load a RaftGroupMetadata for each
// super block found in the tablets/ directory, and then instantiate
// Raft groups from this data.
class RaftGroupMetadata : public RefCountedThreadSafe<RaftGroupMetadata> {
 public:
  // Create metadata for a new Raft group. This assumes that the given superblock
  // has not been written before, and writes out the initial superblock with
  // the provided parameters.
  // data_root_dir and wal_root_dir dictates which disk this Raft group will
  // use in the respective directories.
  // If empty string is passed in, it will be randomly chosen.
  static CHECKED_STATUS CreateNew(FsManager* fs_manager,
                                  const std::string& table_id,
                                  const RaftGroupId& raft_group_id,
                                  const std::string& namespace_name,
                                  const std::string& table_name,
                                  const TableType table_type,
                                  const Schema& schema,
                                  const IndexMap& index_map,
                                  const PartitionSchema& partition_schema,
                                  const Partition& partition,
                                  const boost::optional<IndexInfo>& index_info,
                                  const uint32_t schema_version,
                                  const TabletDataState& initial_tablet_data_state,
                                  RaftGroupMetadataPtr* metadata,
                                  const std::string& data_root_dir = std::string(),
                                  const std::string& wal_root_dir = std::string(),
                                  const bool colocated = false);

  // Load existing metadata from disk.
  static CHECKED_STATUS Load(FsManager* fs_manager,
                             const RaftGroupId& raft_group_id,
                             RaftGroupMetadataPtr* metadata);

  // Try to load an existing Raft group. If it does not exist, create it.
  // If it already existed, verifies that the schema of the Raft group matches the
  // provided 'schema'.
  //
  // This is mostly useful for tests which instantiate Raft groups directly.
  static CHECKED_STATUS LoadOrCreate(FsManager* fs_manager,
                                     const std::string& table_id,
                                     const RaftGroupId& raft_group_id,
                                     const std::string& namespace_name,
                                     const std::string& table_name,
                                     const TableType table_type,
                                     const Schema& schema,
                                     const PartitionSchema& partition_schema,
                                     const Partition& partition,
                                     const boost::optional<IndexInfo>& index_info,
                                     const TabletDataState& initial_tablet_data_state,
                                     RaftGroupMetadataPtr* metadata);

  Result<TableInfoPtr> GetTableInfo(const TableId& table_id) const;
  Result<TableInfoPtr> GetTableInfoUnlocked(const TableId& table_id) const;

  const RaftGroupId& raft_group_id() const {
    DCHECK_NE(state_, kNotLoadedYet);
    return raft_group_id_;
  }

  // Returns the partition of the Raft group.
  std::shared_ptr<Partition> partition() const {
    DCHECK_NE(state_, kNotLoadedYet);
    std::lock_guard<MutexType> lock(data_mutex_);
    return partition_;
  }

  // Returns the primary table id. For co-located tables, the primary table is the table this Raft
  // group was first created for. For single-tenant table, it is the primary table.
  TableId table_id() const {
    DCHECK_NE(state_, kNotLoadedYet);
    std::lock_guard<MutexType> lock(data_mutex_);
    return primary_table_id_;
  }

  // Returns the name, type, schema, index map, schema, etc of the table.
  std::string namespace_name(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    if (table_id.empty()) {
      return primary_table_info()->namespace_name;
    }
    const auto& table_info = CHECK_RESULT(GetTableInfo(table_id));
    return table_info->namespace_name;
  }

  std::string table_name(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    if (table_id.empty()) {
      return primary_table_info()->table_name;
    }
    const auto& table_info = CHECK_RESULT(GetTableInfo(table_id));
    return table_info->table_name;
  }

  TableType table_type(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    if (table_id.empty()) {
      return primary_table_info()->table_type;
    }
    const auto& table_info = CHECK_RESULT(GetTableInfo(table_id));
    return table_info->table_type;
  }

  yb::SchemaPtr schema(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    const TableInfoPtr table_info =
        table_id.empty() ? primary_table_info() : CHECK_RESULT(GetTableInfo(table_id));
    return yb::SchemaPtr(table_info, &table_info->schema);
  }

  std::shared_ptr<IndexMap> index_map(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    const TableInfoPtr table_info =
        table_id.empty() ? primary_table_info() : CHECK_RESULT(GetTableInfo(table_id));
    return std::shared_ptr<IndexMap>(table_info, &table_info->index_map);
  }

  uint32_t schema_version(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    const TableInfoPtr table_info =
        table_id.empty() ? primary_table_info() : CHECK_RESULT(GetTableInfo(table_id));
    return table_info->schema_version;
  }

  const std::string& indexed_tablet_id(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    static const std::string kEmptyString = "";
    std::lock_guard<MutexType> lock(data_mutex_);
    const TableInfoPtr table_info = table_id.empty() ?
        primary_table_info_unlocked() : CHECK_RESULT(GetTableInfoUnlocked(table_id));
    const auto* index_info = table_info->index_info.get();
    return index_info ? index_info->indexed_table_id() : kEmptyString;
  }

  bool is_local_index(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    std::lock_guard<MutexType> lock(data_mutex_);
    const TableInfoPtr table_info = table_id.empty() ?
        primary_table_info_unlocked() : CHECK_RESULT(GetTableInfoUnlocked(table_id));
    const auto* index_info = table_info->index_info.get();
    return index_info && index_info->is_local();
  }

  bool is_unique_index(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    std::lock_guard<MutexType> lock(data_mutex_);
    const TableInfoPtr table_info = table_id.empty() ?
        primary_table_info_unlocked() : CHECK_RESULT(GetTableInfoUnlocked(table_id));
    const auto* index_info = table_info->index_info.get();
    return index_info && index_info->is_unique();
  }

  std::vector<ColumnId> index_key_column_ids(const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    std::lock_guard<MutexType> lock(data_mutex_);
    const TableInfoPtr table_info = table_id.empty() ?
        primary_table_info_unlocked() : CHECK_RESULT(GetTableInfoUnlocked(table_id));
    const auto* index_info = table_info->index_info.get();
    return index_info ? index_info->index_key_column_ids() : std::vector<ColumnId>();
  }

  // Returns the partition schema of the Raft group's tables.
  const std::shared_ptr<PartitionSchema> partition_schema() const {
    DCHECK_NE(state_, kNotLoadedYet);
    const TableInfoPtr table_info = primary_table_info();
    return std::shared_ptr<PartitionSchema>(table_info, &table_info->partition_schema);
  }

  const std::shared_ptr<std::vector<DeletedColumn>> deleted_cols(
      const TableId& table_id = "") const {
    DCHECK_NE(state_, kNotLoadedYet);
    const TableInfoPtr table_info =
        table_id.empty() ? primary_table_info() : CHECK_RESULT(GetTableInfo(table_id));
    return std::shared_ptr<std::vector<DeletedColumn>>(table_info, &table_info->deleted_cols);
  }

  std::string rocksdb_dir() const { return kv_store_.rocksdb_dir; }
  std::string intents_rocksdb_dir() const { return kv_store_.rocksdb_dir + kIntentsDBSuffix; }
  std::string snapshots_dir() const { return kv_store_.rocksdb_dir + kSnapshotsDirSuffix; }

  std::string lower_bound_key() const { return kv_store_.lower_bound_key; }
  std::string upper_bound_key() const { return kv_store_.upper_bound_key; }

  std::string wal_dir() const { return wal_dir_; }

  // Set the WAL retention time for the primary table.
  void set_wal_retention_secs(uint32 wal_retention_secs);

  // Returns the wal retention time for the primary table.
  uint32_t wal_retention_secs() const;

  CHECKED_STATUS set_cdc_min_replicated_index(int64 cdc_min_replicated_index);

  int64_t cdc_min_replicated_index() const;

  bool has_been_fully_compacted() const {
    std::lock_guard<MutexType> lock(data_mutex_);
    return kv_store_.has_been_fully_compacted;
  }

  void set_has_been_fully_compacted(const bool& value) {
    std::lock_guard<MutexType> lock(data_mutex_);
    kv_store_.has_been_fully_compacted = value;
  }

  // Returns the data root dir for this Raft group, for example:
  // /mnt/d0/yb-data/tserver/data
  // TODO(#79): rework when we have more than one KV-store (and data roots) per Raft group.
  std::string data_root_dir() const;

  // Returns the WAL root dir for this Raft group, for example:
  // /mnt/d0/yb-data/tserver/wals
  std::string wal_root_dir() const;

  // Set table_id for altering the schema of a colocated user table.
  void SetSchema(const Schema& schema,
                 const IndexMap& index_map,
                 const std::vector<DeletedColumn>& deleted_cols,
                 const uint32_t version,
                 const TableId& table_id = "");

  void SetPartitionSchema(const PartitionSchema& partition_schema);

  void SetTableName(
      const std::string& namespace_name, const std::string& table_name,
      const TableId& table_id = "");

  void AddTable(const std::string& table_id,
                const std::string& namespace_name,
                const std::string& table_name,
                const TableType table_type,
                const Schema& schema,
                const IndexMap& index_map,
                const PartitionSchema& partition_schema,
                const boost::optional<IndexInfo>& index_info,
                const uint32_t schema_version);

  void RemoveTable(const TableId& table_id);

  // Set / get the remote bootstrap / tablet data state.
  void set_tablet_data_state(TabletDataState state);
  TabletDataState tablet_data_state() const;

  CHECKED_STATUS Flush();

  // Mark the superblock to be in state 'delete_type', sync it to disk, and
  // then delete all of the rowsets in this tablet.
  // The metadata (superblock) is not deleted. For that, call DeleteSuperBlock().
  //
  // 'delete_type' must be one of TABLET_DATA_DELETED or TABLET_DATA_TOMBSTONED.
  // 'last_logged_opid' should be set to the last opid in the log, if any is known.
  // If 'last_logged_opid' is not set, then the current value of
  // last_logged_opid is not modified. This is important for roll-forward of
  // partially-tombstoned tablets during crash recovery.
  //
  // Returns only once all data has been removed.
  // The OUT parameter 'was_deleted' can be used by caller to determine if the tablet data was
  // actually deleted from disk or not. For example, in some cases, the tablet may have been
  // already deleted (and are here on a retry) and this operation essentially ends up being a no-op;
  // in such a case, 'was_deleted' will be set to FALSE.
  CHECKED_STATUS DeleteTabletData(TabletDataState delete_type, const yb::OpId& last_logged_opid);

  // Return true if this metadata references no regular data DB nor intents DB and is
  // already marked as tombstoned. If this is the case, then calling DeleteTabletData
  // would be a no-op.
  bool IsTombstonedWithNoRocksDBData() const;

  // Permanently deletes the superblock from the disk.
  // DeleteTabletData() must first be called and the tablet data state must be
  // TABLET_DATA_DELETED.
  // Returns Status::InvalidArgument if the list of orphaned blocks is not empty.
  // Returns Status::IllegalState if the tablet data state is not TABLET_DATA_DELETED.
  CHECKED_STATUS DeleteSuperBlock();

  FsManager *fs_manager() const { return fs_manager_; }

  yb::OpId tombstone_last_logged_opid() const { return tombstone_last_logged_opid_; }

  // Loads the currently-flushed superblock from disk into the given protobuf.
  CHECKED_STATUS ReadSuperBlockFromDisk(RaftGroupReplicaSuperBlockPB* superblock) const;

  // Sets *superblock to the serialized form of the current metadata.
  void ToSuperBlock(RaftGroupReplicaSuperBlockPB* superblock) const;

  // Fully replace a superblock (used for bootstrap).
  CHECKED_STATUS ReplaceSuperBlock(const RaftGroupReplicaSuperBlockPB &pb);

  // Returns a new WAL dir path to be used for new Raft group `raft_group_id` which will be created
  // as a result of this Raft group splitting.
  // Uses the same root dir as for `this` Raft group.
  std::string GetSubRaftGroupWalDir(const RaftGroupId& raft_group_id) const;

  // Returns a new Data dir path to be used for new Raft group `raft_group_id` which will be created
  // as a result of this Raft group splitting.
  // Uses the same root dir as for `this` Raft group.
  std::string GetSubRaftGroupDataDir(const RaftGroupId& raft_group_id) const;

  // Creates a new Raft group metadata for the part of existing tablet contained in this Raft group.
  // Assigns specified Raft group ID, partition and key bounds for a new tablet.
  Result<RaftGroupMetadataPtr> CreateSubtabletMetadata(
      const RaftGroupId& raft_group_id, const Partition& partition,
      const std::string& lower_bound_key, const std::string& upper_bound_key) const;

  TableInfoPtr primary_table_info() const {
    std::lock_guard<MutexType> lock(data_mutex_);
    return primary_table_info_unlocked();
  }

  bool colocated() const { return colocated_; }

  // Return standard "T xxx P yyy" log prefix.
  std::string LogPrefix() const;

 private:
  typedef simple_spinlock MutexType;

  friend class RefCountedThreadSafe<RaftGroupMetadata>;
  friend class MetadataTest;

  // Compile time assert that no one deletes RaftGroupMetadata objects.
  ~RaftGroupMetadata();

  // Constructor for creating a new Raft group.
  //
  // TODO: get rid of this many-arg constructor in favor of just passing in a
  // SuperBlock, which already contains all of these fields.
  RaftGroupMetadata(FsManager* fs_manager,
                    TableId table_id,
                    RaftGroupId raft_group_id,
                    std::string namespace_name,
                    std::string table_name,
                    TableType table_type,
                    const std::string rocksdb_dir,
                    const std::string wal_dir,
                    const Schema& schema,
                    const IndexMap& index_map,
                    PartitionSchema partition_schema,
                    Partition partition,
                    const boost::optional<IndexInfo>& index_info,
                    const uint32_t schema_version,
                    const TabletDataState& tablet_data_state,
                    const bool colocated = false);

  // Constructor for loading an existing Raft group.
  RaftGroupMetadata(FsManager* fs_manager, RaftGroupId raft_group_id);

  CHECKED_STATUS LoadFromDisk();

  // Update state of metadata to that of the given superblock PB.
  CHECKED_STATUS LoadFromSuperBlock(const RaftGroupReplicaSuperBlockPB& superblock);

  CHECKED_STATUS ReadSuperBlock(RaftGroupReplicaSuperBlockPB *pb);

  // Fully replace superblock.
  // Requires 'flush_lock_'.
  CHECKED_STATUS ReplaceSuperBlockUnlocked(const RaftGroupReplicaSuperBlockPB &pb);

  // Requires 'data_mutex_'.
  void ToSuperBlockUnlocked(RaftGroupReplicaSuperBlockPB* superblock) const;

  const TableInfoPtr primary_table_info_unlocked() const {
    const auto& tables = kv_store_.tables;
    const auto itr = tables.find(primary_table_id_);
    CHECK(itr != tables.end());
    return itr->second;
  }

  enum State {
    kNotLoadedYet,
    kNotWrittenYet,
    kInitialized
  };
  State state_;

  // Lock protecting the underlying data.
  // TODO: consider switching to RW mutex.
  mutable MutexType data_mutex_;

  // Lock protecting flushing the data to disk.
  // If taken together with 'data_mutex_', must be acquired first.
  mutable Mutex flush_lock_;

  RaftGroupId raft_group_id_;
  std::shared_ptr<Partition> partition_;

  // The primary table id. Primary table is the first table this Raft group is created for.
  // Additional tables can be added to this Raft group to co-locate with this table.
  TableId primary_table_id_;

  // KV-store for this Raft group.
  KvStoreInfo kv_store_;

  FsManager* const fs_manager_;

  // The directory where the write-ahead log for this Raft group is stored.
  std::string wal_dir_;

  // The current state of remote bootstrap for the tablet.
  TabletDataState tablet_data_state_;

  // Record of the last opid logged by the tablet before it was last tombstoned. Has no meaning for
  // non-tombstoned tablets.
  yb::OpId tombstone_last_logged_opid_;

  // True if the raft group is for a colocated tablet.
  bool colocated_;

  // The minimum index that has been replicated by the cdc service.
  int64_t cdc_min_replicated_index_ = std::numeric_limits<int64_t>::max();

  DISALLOW_COPY_AND_ASSIGN(RaftGroupMetadata);
};

CHECKED_STATUS MigrateSuperblock(RaftGroupReplicaSuperBlockPB* superblock);

// Checks whether tablet data storage is ready for function, i.e. its creation or bootstrap process
// has been completed and tablet is not deleted and not in process of being deleted.
inline bool CanServeTabletData(TabletDataState state) {
  return state == TabletDataState::TABLET_DATA_READY ||
         state == TabletDataState::TABLET_DATA_SPLIT_COMPLETED;
}

} // namespace tablet
} // namespace yb

#endif /* YB_TABLET_TABLET_METADATA_H */
