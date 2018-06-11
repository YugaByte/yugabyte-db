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

#include "yb/common/index.h"
#include "yb/common/partition.h"
#include "yb/common/schema.h"
#include "yb/consensus/opid_util.h"
#include "yb/fs/block_id.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/callback.h"
#include "yb/gutil/dynamic_annotations.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"

#include "yb/tablet/metadata.pb.h"

#include "yb/util/mutex.h"
#include "yb/util/opid.h"
#include "yb/util/opid.pb.h"
#include "yb/util/status.h"
#include "yb/util/status_callback.h"

namespace yb {
namespace tablet {

extern const int64 kNoDurableMemStore;

// Manages the "blocks tracking" for the specified tablet.
//
// TabletMetadata is owned by the Tablet. As new blocks are written to store
// the tablet's data, the Tablet calls Flush() to persist the block list
// on disk.
//
// At startup, the TSTabletManager will load a TabletMetadata for each
// super block found in the tablets/ directory, and then instantiate
// tablets from this data.
class TabletMetadata : public RefCountedThreadSafe<TabletMetadata> {
 public:
  // Create metadata for a new tablet. This assumes that the given superblock
  // has not been written before, and writes out the initial superblock with
  // the provided parameters.
  // data_root_dir and wal_root_dir dictates which disk this tablet will
  // use in the respective directories.
  // If empty string is passed in, it will be randomly chosen.
  static CHECKED_STATUS CreateNew(FsManager* fs_manager,
                          const std::string& table_id,
                          const std::string& tablet_id,
                          const std::string& table_name,
                          const TableType table_type,
                          const Schema& schema,
                          const PartitionSchema& partition_schema,
                          const Partition& partition,
                          const TabletDataState& initial_tablet_data_state,
                          scoped_refptr<TabletMetadata>* metadata,
                          const std::string& data_root_dir = std::string(),
                          const std::string& wal_root_dir = std::string());

  // Load existing metadata from disk.
  static CHECKED_STATUS Load(FsManager* fs_manager,
                     const std::string& tablet_id,
                     scoped_refptr<TabletMetadata>* metadata);

  // Try to load an existing tablet. If it does not exist, create it.
  // If it already existed, verifies that the schema of the tablet matches the
  // provided 'schema'.
  //
  // This is mostly useful for tests which instantiate tablets directly.
  static CHECKED_STATUS LoadOrCreate(FsManager* fs_manager,
                             const std::string& table_id,
                             const std::string& tablet_id,
                             const std::string& table_name,
                             const TableType table_type,
                             const Schema& schema,
                             const PartitionSchema& partition_schema,
                             const Partition& partition,
                             const TabletDataState& initial_tablet_data_state,
                             scoped_refptr<TabletMetadata>* metadata);

  const std::string& tablet_id() const {
    DCHECK_NE(state_, kNotLoadedYet);
    return tablet_id_;
  }

  // Returns the partition of the tablet.
  const Partition& partition() const {
    return partition_;
  }

  std::string table_id() const {
    DCHECK_NE(state_, kNotLoadedYet);
    return table_id_;
  }

  std::string table_name() const;

  TableType table_type() const;

  std::string rocksdb_dir() const { return rocksdb_dir_; }

  std::string wal_dir() const { return wal_dir_; }

  // Given the data directory of a tablet, returns the data root dir for that tablet.
  // For example,
  //  Given /mnt/d0/tserver/data1/data/rocksdb/0c966bbae43f470f8afbb3de648ed46a
  //  Returns /mnt/d0/tserver/data1/data
  std::string data_root_dir() const;

  // Given the WAL directory of a tablet, returns the wal root dir for that tablet.
  // For example,
  //  Given /mnt/d0/tserver/wal1/wals/0c966bbae43f470f8afbb3de648ed46a
  //  Returns /mnt/d0/tserver/wal1/wals
  std::string wal_root_dir() const;

  uint32_t schema_version() const;

  void SetSchema(const Schema& schema, uint32_t version);

  const IndexMap& index_map() const { return index_map_; }
  void SetIndexMap(IndexMap&& index_map);

  void SetTableName(const std::string& table_name);

  // Return a reference to the current schema.
  // This pointer will be valid until the TabletMetadata is destructed,
  // even if the schema is changed.
  const Schema& schema() const {
    const Schema* s = reinterpret_cast<const Schema*>(
        base::subtle::Acquire_Load(reinterpret_cast<const AtomicWord*>(&schema_)));
    return *s;
  }

  // Returns the partition schema of the tablet's table.
  const PartitionSchema& partition_schema() const {
    return partition_schema_;
  }

  // Set / get the remote bootstrap / tablet data state.
  void set_tablet_data_state(TabletDataState state);
  TabletDataState tablet_data_state() const;

  // Increments flush pin count by one: if flush pin count > 0,
  // metadata will _not_ be flushed to disk during Flush().
  void PinFlush();

  // Decrements flush pin count by one: if flush pin count is zero,
  // metadata will be flushed to disk during the next call to Flush()
  // or -- if Flush() had been called after a call to PinFlush() but
  // before this method was called -- Flush() will be called inside
  // this method.
  CHECKED_STATUS UnPinFlush();

  CHECKED_STATUS Flush();

  // Adds the blocks referenced by 'block_ids' to 'orphaned_blocks_'.
  //
  // This set will be written to the on-disk metadata in any subsequent
  // flushes.
  //
  // Blocks are removed from this set after they are successfully deleted
  // in a call to DeleteOrphanedBlocks().
  void AddOrphanedBlocks(const std::vector<BlockId>& block_ids);

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

  // Permanently deletes the superblock from the disk.
  // DeleteTabletData() must first be called and the tablet data state must be
  // TABLET_DATA_DELETED.
  // Returns Status::InvalidArgument if the list of orphaned blocks is not empty.
  // Returns Status::IllegalState if the tablet data state is not TABLET_DATA_DELETED.
  CHECKED_STATUS DeleteSuperBlock();

  FsManager *fs_manager() const { return fs_manager_; }

  int64_t last_durable_mrs_id() const { return last_durable_mrs_id_; }

  void SetLastDurableMrsIdForTests(int64_t mrs_id) { last_durable_mrs_id_ = mrs_id; }

  yb::OpId tombstone_last_logged_opid() const { return tombstone_last_logged_opid_; }

  // Loads the currently-flushed superblock from disk into the given protobuf.
  CHECKED_STATUS ReadSuperBlockFromDisk(TabletSuperBlockPB* superblock) const;

  // Sets *super_block to the serialized form of the current metadata.
  CHECKED_STATUS ToSuperBlock(TabletSuperBlockPB* super_block) const;

  // Fully replace a superblock (used for bootstrap).
  CHECKED_STATUS ReplaceSuperBlock(const TabletSuperBlockPB &pb);

  const std::vector<DeletedColumn>& GetDeletedColumns() const {
    return deleted_cols_;
  }

  void AddDeletedColumn(const DeletedColumn& col) {
    deleted_cols_.push_back(col);
  }

 private:
  friend class RefCountedThreadSafe<TabletMetadata>;
  friend class MetadataTest;

  // Compile time assert that no one deletes TabletMetadata objects.
  ~TabletMetadata();

  // Constructor for creating a new tablet.
  //
  // TODO: get rid of this many-arg constructor in favor of just passing in a
  // SuperBlock, which already contains all of these fields.
  TabletMetadata(FsManager* fs_manager,
                 std::string table_id,
                 std::string tablet_id,
                 std::string table_name,
                 TableType table_type,
                 const std::string rocksdb_dir,
                 const std::string wal_dir,
                 const Schema& schema,
                 PartitionSchema partition_schema,
                 Partition partition,
                 const TabletDataState& tablet_data_state);

  // Constructor for loading an existing tablet.
  TabletMetadata(FsManager* fs_manager, std::string tablet_id);

  void SetSchemaUnlocked(gscoped_ptr<Schema> schema, uint32_t version);

  CHECKED_STATUS LoadFromDisk();

  // Update state of metadata to that of the given superblock PB.
  CHECKED_STATUS LoadFromSuperBlock(const TabletSuperBlockPB& superblock);

  CHECKED_STATUS ReadSuperBlock(TabletSuperBlockPB *pb);

  // Fully replace superblock.
  // Requires 'flush_lock_'.
  CHECKED_STATUS ReplaceSuperBlockUnlocked(const TabletSuperBlockPB &pb);

  // Requires 'data_lock_'.
  CHECKED_STATUS ToSuperBlockUnlocked(TabletSuperBlockPB* super_block) const;

  // Requires 'data_lock_'.
  void AddOrphanedBlocksUnlocked(const std::vector<BlockId>& block_ids);

  // Deletes the provided 'blocks' on disk.
  //
  // All blocks that are successfully deleted are removed from the
  // 'orphaned_blocks_' set.
  //
  // Failures are logged, but are not fatal.
  void DeleteOrphanedBlocks(const std::vector<BlockId>& blocks);

  // Return standard "T xxx P yyy" log prefix.
  std::string LogPrefix() const;

  enum State {
    kNotLoadedYet,
    kNotWrittenYet,
    kInitialized
  };
  State state_;

  // Lock protecting the underlying data.
  typedef simple_spinlock LockType;
  mutable LockType data_lock_;

  // Lock protecting flushing the data to disk.
  // If taken together with 'data_lock_', must be acquired first.
  mutable Mutex flush_lock_;

  std::string table_id_;
  const std::string tablet_id_;

  Partition partition_;

  FsManager* const fs_manager_;

  int64_t last_durable_mrs_id_;

  // The current schema version. This is owned by this class.
  // We don't use gscoped_ptr so that we can do an atomic swap.
  Schema* schema_;
  IndexMap index_map_;
  uint32_t schema_version_;
  std::string table_name_;
  TableType table_type_;

  // The directory where the RocksDB data for this tablet is stored.
  std::string rocksdb_dir_;

  // The directory where the write-ahead log for this tablet is stored.
  std::string wal_dir_;

  PartitionSchema partition_schema_;

  // Previous values of 'schema_'.
  // These are currently kept alive forever, under the assumption that
  // a given tablet won't have thousands of "alter table" calls.
  // They are kept alive so that callers of schema() don't need to
  // worry about reference counting or locking.
  std::vector<Schema*> old_schemas_;

  // Protected by 'data_lock_'.
  std::unordered_set<BlockId, BlockIdHash, BlockIdEqual> orphaned_blocks_;

  // The current state of remote bootstrap for the tablet.
  TabletDataState tablet_data_state_;

  // Record of the last opid logged by the tablet before it was last
  // tombstoned. Has no meaning for non-tombstoned tablets.
  yb::OpId tombstone_last_logged_opid_;

  // If this counter is > 0 then Flush() will not write any data to
  // disk.
  int32_t num_flush_pins_ = 0;

  // Set if Flush() is called when num_flush_pins_ is > 0; if true,
  // then next UnPinFlush will call Flush() again to ensure the
  // metadata is persisted.
  bool needs_flush_ = false;

  // A vector of column IDs that have been deleted, so that the compaction filter can free the
  // associated memory. At present, deleted column IDs are persisted forever, even if all the
  // associated data has been discarded. In the future, we can garbage collect such column IDs
  // to make sure this vector doesn't grow too large.
  std::vector<DeletedColumn> deleted_cols_;

  DISALLOW_COPY_AND_ASSIGN(TabletMetadata);
};

extern const std::string kIntentsSubdir;
extern const std::string kIntentsDBSuffix;

} // namespace tablet
} // namespace yb

#endif /* YB_TABLET_TABLET_METADATA_H */
