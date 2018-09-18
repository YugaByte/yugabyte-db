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
#include "yb/common/wire_protocol.h"
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
using yb::consensus::RaftConfigPB;

namespace yb {
namespace tablet {

const int64 kNoDurableMemStore = -1;
const std::string kIntentsSubdir = "intents";
const std::string kIntentsDBSuffix = ".intents";

// ============================================================================
//  Tablet Metadata
// ============================================================================

Status TabletMetadata::CreateNew(FsManager* fs_manager,
                                 const string& table_id,
                                 const string& tablet_id,
                                 const string& table_name,
                                 const TableType table_type,
                                 const Schema& schema,
                                 const PartitionSchema& partition_schema,
                                 const Partition& partition,
                                 const boost::optional<IndexInfo>& index_info,
                                 const TabletDataState& initial_tablet_data_state,
                                 scoped_refptr<TabletMetadata>* metadata,
                                 const string& data_root_dir,
                                 const string& wal_root_dir) {

  // Verify that no existing tablet exists with the same ID.
  if (fs_manager->env()->FileExists(fs_manager->GetTabletMetadataPath(tablet_id))) {
    return STATUS(AlreadyPresent, "Tablet already exists", tablet_id);
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

  auto table_dir = Substitute("table-$0", table_id);
  auto tablet_dir = Substitute("tablet-$0", tablet_id);

  auto wal_table_top_dir = JoinPathSegments(wal_top_dir, table_dir);
  auto wal_dir = JoinPathSegments(wal_table_top_dir, tablet_dir);

  auto rocksdb_dir = JoinPathSegments(
      data_top_dir, FsManager::kRocksDBDirName, table_dir, tablet_dir);

  scoped_refptr<TabletMetadata> ret(new TabletMetadata(fs_manager,
                                                       table_id,
                                                       tablet_id,
                                                       table_name,
                                                       table_type,
                                                       rocksdb_dir,
                                                       wal_dir,
                                                       schema,
                                                       partition_schema,
                                                       partition,
                                                       index_info,
                                                       initial_tablet_data_state));
  RETURN_NOT_OK(ret->Flush());
  metadata->swap(ret);
  return Status::OK();
}

Status TabletMetadata::Load(FsManager* fs_manager,
                            const string& tablet_id,
                            scoped_refptr<TabletMetadata>* metadata) {
  scoped_refptr<TabletMetadata> ret(new TabletMetadata(fs_manager, tablet_id));
  RETURN_NOT_OK(ret->LoadFromDisk());
  metadata->swap(ret);
  return Status::OK();
}

Status TabletMetadata::LoadOrCreate(FsManager* fs_manager,
                                    const string& table_id,
                                    const string& tablet_id,
                                    const string& table_name,
                                    TableType table_type,
                                    const Schema& schema,
                                    const PartitionSchema& partition_schema,
                                    const Partition& partition,
                                    const boost::optional<IndexInfo>& index_info,
                                    const TabletDataState& initial_tablet_data_state,
                                    scoped_refptr<TabletMetadata>* metadata) {
  Status s = Load(fs_manager, tablet_id, metadata);
  if (s.ok()) {
    if (!(*metadata)->schema().Equals(schema)) {
      return STATUS(Corruption, Substitute("Schema on disk ($0) does not "
        "match expected schema ($1)", (*metadata)->schema().ToString(),
        schema.ToString()));
    }
    return Status::OK();
  } else if (s.IsNotFound()) {
    return CreateNew(fs_manager, table_id, tablet_id, table_name, table_type,
                     schema, partition_schema, partition, index_info,
                     initial_tablet_data_state, metadata);
  } else {
    return s;
  }
}

Status TabletMetadata::DeleteTabletData(TabletDataState delete_type,
                                        const yb::OpId& last_logged_opid) {
  CHECK(delete_type == TABLET_DATA_DELETED ||
        delete_type == TABLET_DATA_TOMBSTONED)
      << "DeleteTabletData() called with unsupported delete_type on tablet "
      << tablet_id_ << ": " << TabletDataState_Name(delete_type)
      << " (" << delete_type << ")";

  // First add all of our blocks to the orphan list
  // and clear our rowsets. This serves to erase all the data.
  //
  // We also set the state in our persisted metadata to indicate that
  // we have been deleted.
  {
    std::lock_guard<LockType> l(data_lock_);
    tablet_data_state_ = delete_type;
    if (last_logged_opid) {
      tombstone_last_logged_opid_ = last_logged_opid;
    }
  }

  rocksdb::Options rocksdb_options;
  TabletOptions tablet_options;
  docdb::InitRocksDBOptions(
      &rocksdb_options, tablet_id_, nullptr /* statistics */, tablet_options);

  LOG(INFO) << "Destroying regular db at: " << rocksdb_dir_;
  rocksdb::Status status = rocksdb::DestroyDB(rocksdb_dir_, rocksdb_options);

  if (!status.ok()) {
    LOG(ERROR) << "Failed to destroy regular DB at: " << rocksdb_dir_ << ": " << status;
  } else {
    LOG(INFO) << "Successfully destroyed regular DB at: " << rocksdb_dir_;
  }

  auto intents_dir = rocksdb_dir_ + kIntentsDBSuffix;
  if (fs_manager_->env()->FileExists(intents_dir)) {
    auto status = rocksdb::DestroyDB(intents_dir, rocksdb_options);

    if (!status.ok()) {
      LOG(ERROR) << "Failed to destroy provisional records DB at: " << intents_dir << ": "
                 << status;
    } else {
      LOG(INFO) << "Successfully destroyed provisional records DB at: " << intents_dir;
    }
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

Status TabletMetadata::DeleteSuperBlock() {
  std::lock_guard<LockType> l(data_lock_);
  if (!orphaned_blocks_.empty()) {
    return STATUS(InvalidArgument, "The metadata for tablet " + tablet_id_ +
                                   " still references orphaned blocks. "
                                   "Call DeleteTabletData() first");
  }
  if (tablet_data_state_ != TABLET_DATA_DELETED) {
    return STATUS(IllegalState,
        Substitute("Tablet $0 is not in TABLET_DATA_DELETED state. "
                   "Call DeleteTabletData(TABLET_DATA_DELETED) first. "
                   "Tablet data state: $1 ($2)",
                   tablet_id_,
                   TabletDataState_Name(tablet_data_state_),
                   tablet_data_state_));
  }

  string path = fs_manager_->GetTabletMetadataPath(tablet_id_);
  RETURN_NOT_OK_PREPEND(fs_manager_->env()->DeleteFile(path),
                        "Unable to delete superblock for tablet " + tablet_id_);
  return Status::OK();
}

TabletMetadata::TabletMetadata(FsManager* fs_manager,
                               string table_id,
                               string tablet_id,
                               string table_name,
                               TableType table_type,
                               const string rocksdb_dir,
                               const string wal_dir,
                               const Schema& schema,
                               PartitionSchema partition_schema,
                               Partition partition,
                               const boost::optional<IndexInfo>& index_info,
                               const TabletDataState& tablet_data_state)
    : state_(kNotWrittenYet),
      table_id_(std::move(table_id)),
      tablet_id_(std::move(tablet_id)),
      partition_(std::move(partition)),
      fs_manager_(fs_manager),
      last_durable_mrs_id_(kNoDurableMemStore),
      schema_(new Schema(schema)),
      schema_version_(0),
      table_name_(std::move(table_name)),
      table_type_(table_type),
      index_info_(index_info),
      rocksdb_dir_(rocksdb_dir),
      wal_dir_(wal_dir),
      partition_schema_(std::move(partition_schema)),
      tablet_data_state_(tablet_data_state) {
  CHECK(schema_->has_column_ids());
  CHECK_GT(schema_->num_key_columns(), 0);
}

TabletMetadata::~TabletMetadata() {
  STLDeleteElements(&old_schemas_);
  delete schema_;
}

TabletMetadata::TabletMetadata(FsManager* fs_manager, string tablet_id)
    : state_(kNotLoadedYet),
      tablet_id_(std::move(tablet_id)),
      fs_manager_(fs_manager),
      schema_(nullptr) {}

Status TabletMetadata::LoadFromDisk() {
  TRACE_EVENT1("tablet", "TabletMetadata::LoadFromDisk",
               "tablet_id", tablet_id_);

  CHECK_EQ(state_, kNotLoadedYet);

  TabletSuperBlockPB superblock;
  RETURN_NOT_OK(ReadSuperBlockFromDisk(&superblock));
  RETURN_NOT_OK_PREPEND(LoadFromSuperBlock(superblock),
                        "Failed to load data from superblock protobuf");
  state_ = kInitialized;
  return Status::OK();
}

Status TabletMetadata::LoadFromSuperBlock(const TabletSuperBlockPB& superblock) {
  vector<BlockId> orphaned_blocks;

  VLOG(2) << "Loading TabletMetadata from SuperBlockPB:" << std::endl
          << superblock.DebugString();

  {
    std::lock_guard<LockType> l(data_lock_);

    // Verify that the tablet id matches with the one in the protobuf
    if (superblock.tablet_id() != tablet_id_) {
      return STATUS(Corruption, "Expected id=" + tablet_id_ +
                                " found " + superblock.tablet_id(),
                                superblock.DebugString());
    }

    table_id_ = superblock.table_id();
    last_durable_mrs_id_ = superblock.last_durable_mrs_id();

    table_name_ = superblock.table_name();
    table_type_ = superblock.table_type();
    index_map_.FromPB(superblock.indexes());
    if (superblock.has_index_info()) {
      index_info_.emplace(superblock.index_info());
    }
    rocksdb_dir_ = superblock.rocksdb_dir();
    wal_dir_ = superblock.wal_dir();

    uint32_t schema_version = superblock.schema_version();
    gscoped_ptr<Schema> schema(new Schema());
    RETURN_NOT_OK_PREPEND(SchemaFromPB(superblock.schema(), schema.get()),
                          "Failed to parse Schema from superblock " +
                          superblock.ShortDebugString());
    SetSchemaUnlocked(schema.Pass(), schema_version);

    // This check provides backwards compatibility with the
    // flexible-partitioning changes introduced in KUDU-818.
    if (superblock.has_partition()) {
      RETURN_NOT_OK(PartitionSchema::FromPB(superblock.partition_schema(),
                                            *schema_, &partition_schema_));
      Partition::FromPB(superblock.partition(), &partition_);
    } else {
      LOG(FATAL) << "partition field must be set in superblock: " << superblock.ShortDebugString();
    }

    tablet_data_state_ = superblock.tablet_data_state();

    deleted_cols_.clear();
    for (const DeletedColumnPB& deleted_col : superblock.deleted_cols()) {
      DeletedColumn col;
      RETURN_NOT_OK(DeletedColumn::FromPB(deleted_col, &col));
      deleted_cols_.push_back(col);
    }

    for (const BlockIdPB& block_pb : superblock.orphaned_blocks()) {
      orphaned_blocks.push_back(BlockId::FromPB(block_pb));
    }
    AddOrphanedBlocksUnlocked(orphaned_blocks);

    if (superblock.has_tombstone_last_logged_opid()) {
      tombstone_last_logged_opid_ = yb::OpId::FromPB(superblock.tombstone_last_logged_opid());
    } else {
      tombstone_last_logged_opid_ = OpId();
    }
  }

  // Now is a good time to clean up any orphaned blocks that may have been
  // left behind from a crash just after replacing the superblock.
  if (!fs_manager()->read_only()) {
    DeleteOrphanedBlocks(orphaned_blocks);
  }

  return Status::OK();
}

void TabletMetadata::AddOrphanedBlocks(const vector<BlockId>& blocks) {
  std::lock_guard<LockType> l(data_lock_);
  AddOrphanedBlocksUnlocked(blocks);
}

void TabletMetadata::AddOrphanedBlocksUnlocked(const vector<BlockId>& blocks) {
  DCHECK(data_lock_.is_locked());
  orphaned_blocks_.insert(blocks.begin(), blocks.end());
}

void TabletMetadata::DeleteOrphanedBlocks(const vector<BlockId>& blocks) {
  if (PREDICT_FALSE(!FLAGS_enable_tablet_orphaned_block_deletion)) {
    LOG_WITH_PREFIX(WARNING) << "Not deleting " << blocks.size()
        << " block(s) from disk. Block deletion disabled via "
        << "--enable_tablet_orphaned_block_deletion=false";
    return;
  }

  vector<BlockId> deleted;
  for (const BlockId& b : blocks) {
    Status s = fs_manager()->DeleteBlock(b);
    // If we get NotFound, then the block was actually successfully
    // deleted before. So, we can remove it from our orphaned block list
    // as if it was a success.
    if (!s.ok() && !s.IsNotFound()) {
      WARN_NOT_OK(s, Substitute("Could not delete block $0", b.ToString()));
      continue;
    }

    deleted.push_back(b);
  }

  // Remove the successfully-deleted blocks from the set.
  {
    std::lock_guard<LockType> l(data_lock_);
    for (const BlockId& b : deleted) {
      orphaned_blocks_.erase(b);
    }
  }
}

void TabletMetadata::PinFlush() {
  std::lock_guard<LockType> l(data_lock_);
  CHECK_GE(num_flush_pins_, 0);
  num_flush_pins_++;
  VLOG(1) << "Number of flush pins: " << num_flush_pins_;
}

Status TabletMetadata::UnPinFlush() {
  std::unique_lock<LockType> l(data_lock_);
  CHECK_GT(num_flush_pins_, 0);
  num_flush_pins_--;
  if (needs_flush_) {
    l.unlock();
    RETURN_NOT_OK(Flush());
  }
  return Status::OK();
}

Status TabletMetadata::Flush() {
  TRACE_EVENT1("tablet", "TabletMetadata::Flush",
               "tablet_id", tablet_id_);

  MutexLock l_flush(flush_lock_);
  vector<BlockId> orphaned;
  TabletSuperBlockPB pb;
  {
    std::lock_guard<LockType> l(data_lock_);
    CHECK_GE(num_flush_pins_, 0);
    if (num_flush_pins_ > 0) {
      needs_flush_ = true;
      LOG(INFO) << "Not flushing: waiting for " << num_flush_pins_ << " pins to be released.";
      return Status::OK();
    }
    needs_flush_ = false;

    RETURN_NOT_OK(ToSuperBlockUnlocked(&pb));

    // Make a copy of the orphaned blocks list which corresponds to the superblock
    // that we're writing. It's important to take this local copy to avoid a race
    // in which another thread may add new orphaned blocks to the 'orphaned_blocks_'
    // set while we're in the process of writing the new superblock to disk. We don't
    // want to accidentally delete those blocks before that next metadata update
    // is persisted. See KUDU-701 for details.
    orphaned.assign(orphaned_blocks_.begin(), orphaned_blocks_.end());
  }
  RETURN_NOT_OK(ReplaceSuperBlockUnlocked(pb));
  TRACE("Metadata flushed");
  l_flush.Unlock();

  // Now that the superblock is written, try to delete the orphaned blocks.
  //
  // If we crash just before the deletion, we'll retry when reloading from
  // disk; the orphaned blocks were persisted as part of the superblock.
  DeleteOrphanedBlocks(orphaned);

  return Status::OK();
}

Status TabletMetadata::ReplaceSuperBlock(const TabletSuperBlockPB &pb) {
  {
    MutexLock l(flush_lock_);
    RETURN_NOT_OK_PREPEND(ReplaceSuperBlockUnlocked(pb), "Unable to replace superblock");
  }

  RETURN_NOT_OK_PREPEND(LoadFromSuperBlock(pb),
                        "Failed to load data from superblock protobuf");

  return Status::OK();
}

Status TabletMetadata::ReplaceSuperBlockUnlocked(const TabletSuperBlockPB &pb) {
  flush_lock_.AssertAcquired();

  string path = fs_manager_->GetTabletMetadataPath(tablet_id_);
  RETURN_NOT_OK_PREPEND(pb_util::WritePBContainerToPath(
                            fs_manager_->env(), path, pb,
                            pb_util::OVERWRITE, pb_util::SYNC),
                        Substitute("Failed to write tablet metadata $0", tablet_id_));

  return Status::OK();
}

Status TabletMetadata::ReadSuperBlockFromDisk(TabletSuperBlockPB* superblock) const {
  string path = fs_manager_->GetTabletMetadataPath(tablet_id_);
  RETURN_NOT_OK_PREPEND(
      pb_util::ReadPBContainerFromPath(fs_manager_->env(), path, superblock),
      Substitute("Could not load tablet metadata from $0", path));
  if (superblock->table_type() == TableType::REDIS_TABLE_TYPE &&
      superblock->table_name() == "transactions") {
    superblock->set_table_type(TableType::TRANSACTION_STATUS_TABLE_TYPE);
  }
  return Status::OK();
}

Status TabletMetadata::ToSuperBlock(TabletSuperBlockPB* super_block) const {
  // acquire the lock so that rowsets_ doesn't get changed until we're finished.
  std::lock_guard<LockType> l(data_lock_);
  return ToSuperBlockUnlocked(super_block);
}

Status TabletMetadata::ToSuperBlockUnlocked(TabletSuperBlockPB* super_block) const {
  DCHECK(data_lock_.is_locked());
  // Convert to protobuf
  TabletSuperBlockPB pb;
  pb.set_table_id(table_id_);
  pb.set_tablet_id(tablet_id_);
  partition_.ToPB(pb.mutable_partition());
  pb.set_last_durable_mrs_id(last_durable_mrs_id_);
  pb.set_schema_version(schema_version_);
  partition_schema_.ToPB(pb.mutable_partition_schema());
  pb.set_table_name(table_name_);
  pb.set_table_type(table_type_);
  index_map_.ToPB(pb.mutable_indexes());
  if (index_info_) {
    index_info_->ToPB(pb.mutable_index_info());
  }
  pb.set_rocksdb_dir(rocksdb_dir_);
  pb.set_wal_dir(wal_dir_);

  DCHECK(schema_->has_column_ids());
  RETURN_NOT_OK_PREPEND(SchemaToPB(*schema_, pb.mutable_schema()),
                        "Couldn't serialize schema into superblock");

  pb.set_tablet_data_state(tablet_data_state_);
  if (tombstone_last_logged_opid_) {
    tombstone_last_logged_opid_.ToPB(pb.mutable_tombstone_last_logged_opid());
  }

  for (const BlockId& block_id : orphaned_blocks_) {
    block_id.CopyToPB(pb.mutable_orphaned_blocks()->Add());
  }

  for (const DeletedColumn& deleted_col : deleted_cols_) {
    deleted_col.CopyToPB(pb.mutable_deleted_cols()->Add());
  }

  super_block->Swap(&pb);
  return Status::OK();
}

void TabletMetadata::SetSchema(const Schema& schema, uint32_t version) {
  gscoped_ptr<Schema> new_schema(new Schema(schema));
  std::lock_guard<LockType> l(data_lock_);
  SetSchemaUnlocked(new_schema.Pass(), version);
}

void TabletMetadata::SetIndexMap(IndexMap&& index_map) {
  std::lock_guard<LockType> l(data_lock_);
  index_map_ = std::move(index_map);
}

void TabletMetadata::SetSchemaUnlocked(gscoped_ptr<Schema> new_schema, uint32_t version) {
  DCHECK(new_schema->has_column_ids());

  Schema* old_schema = schema_;
  // "Release" barrier ensures that, when we publish the new Schema object,
  // all of its initialization is also visible.
  base::subtle::Release_Store(reinterpret_cast<AtomicWord*>(&schema_),
                              reinterpret_cast<AtomicWord>(new_schema.release()));
  if (PREDICT_TRUE(old_schema)) {
    old_schemas_.push_back(old_schema);
  }
  schema_version_ = version;
}

void TabletMetadata::SetTableName(const string& table_name) {
  std::lock_guard<LockType> l(data_lock_);
  table_name_ = table_name;
}

string TabletMetadata::table_name() const {
  std::lock_guard<LockType> l(data_lock_);
  DCHECK_NE(state_, kNotLoadedYet);
  return table_name_;
}

uint32_t TabletMetadata::schema_version() const {
  std::lock_guard<LockType> l(data_lock_);
  DCHECK_NE(state_, kNotLoadedYet);
  return schema_version_;
}

string TabletMetadata::data_root_dir() const {
  if (rocksdb_dir_.empty()) {
    return "";
  } else {
    auto data_root_dir = DirName(DirName(rocksdb_dir_));
    if (strcmp(BaseName(data_root_dir).c_str(), FsManager::kRocksDBDirName) == 0) {
      data_root_dir = DirName(data_root_dir);
    }
    return data_root_dir;
  }
}

string TabletMetadata::wal_root_dir() const {
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

void TabletMetadata::set_tablet_data_state(TabletDataState state) {
  std::lock_guard<LockType> l(data_lock_);
  tablet_data_state_ = state;
}

string TabletMetadata::LogPrefix() const {
  return Substitute("T $0 P $1: ", tablet_id_, fs_manager_->uuid());
}

TabletDataState TabletMetadata::tablet_data_state() const {
  std::lock_guard<LockType> l(data_lock_);
  return tablet_data_state_;
}

} // namespace tablet
} // namespace yb
