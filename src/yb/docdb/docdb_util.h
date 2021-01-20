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

// Utilities for docdb operations.

#ifndef YB_DOCDB_DOCDB_UTIL_H
#define YB_DOCDB_DOCDB_UTIL_H

#include "yb/common/schema.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/doc_write_batch.h"
#include "yb/docdb/docdb_compaction_filter.h"
#include "yb/docdb/primitive_value.h"

namespace yb {
namespace docdb {

struct ExternalIntent {
  DocPath doc_path;
  Value value;
};

// A wrapper around a RocksDB instance and provides utility functions on top of it, such as
// compacting the history until a certain point. This is used in the bulk load tool. This is also
// convenient base class for GTest test classes, because it exposes member functions such as
// rocksdb() and write_options().
class DocDBRocksDBUtil {

 public:
  DocDBRocksDBUtil() {}
  explicit DocDBRocksDBUtil(InitMarkerBehavior init_marker_behavior)
      : init_marker_behavior_(init_marker_behavior) {
  }

  virtual ~DocDBRocksDBUtil() {}
  virtual CHECKED_STATUS InitRocksDBDir() = 0;

  // Initializes RocksDB options, should be called after constructor, because it uses virtual
  // function BlockCacheSize.
  virtual CHECKED_STATUS InitRocksDBOptions() = 0;
  virtual std::string tablet_id() = 0;

  // Size of block cache for RocksDB, 0 means don't use block cache.
  virtual size_t block_cache_size() const { return 16 * 1024 * 1024; }

  rocksdb::DB* rocksdb();
  rocksdb::DB* intents_db();
  DocDB doc_db() { return { rocksdb(), intents_db(), &KeyBounds::kNoBounds }; }

  CHECKED_STATUS InitCommonRocksDBOptions();

  const rocksdb::WriteOptions& write_options() const { return write_options_; }

  const rocksdb::Options& regular_db_options() const { return regular_db_options_; }

  CHECKED_STATUS OpenRocksDB();

  void CloseRocksDB();
  CHECKED_STATUS ReopenRocksDB();
  CHECKED_STATUS DestroyRocksDB();
  void ResetMonotonicCounter();

  // Populates the given RocksDB write batch from the given DocWriteBatch. If a valid hybrid_time is
  // specified, it is appended to every key.
  CHECKED_STATUS PopulateRocksDBWriteBatch(
      const DocWriteBatch& dwb,
      rocksdb::WriteBatch *rocksdb_write_batch,
      HybridTime hybrid_time = HybridTime::kInvalid,
      bool decode_dockey = true,
      bool increment_write_id = true,
      PartialRangeKeyIntents partial_range_key_intents = PartialRangeKeyIntents::kTrue) const;

  // Writes the given DocWriteBatch to RocksDB. We substitue the hybrid time, if provided.
  CHECKED_STATUS WriteToRocksDB(
      const DocWriteBatch& write_batch,
      const HybridTime& hybrid_time,
      bool decode_dockey = true,
      bool increment_write_id = true,
      PartialRangeKeyIntents partial_range_key_intents = PartialRangeKeyIntents::kTrue);

  // The same as WriteToRocksDB but also clears the write batch afterwards.
  CHECKED_STATUS WriteToRocksDBAndClear(DocWriteBatch* dwb, const HybridTime& hybrid_time,
                                        bool decode_dockey = true, bool increment_write_id = true);

  // Writes value fully determined by its index using DefaultWriteBatch.
  CHECKED_STATUS WriteSimple(int index);

  void SetHistoryCutoffHybridTime(HybridTime history_cutoff);

  // Produces a string listing the contents of the entire RocksDB database, with every key and value
  // decoded as a DocDB key/value and converted to a human-readable string representation.
  std::string DocDBDebugDumpToStr();

  // ----------------------------------------------------------------------------------------------
  // SetPrimitive taking a Value

  CHECKED_STATUS SetPrimitive(
      const DocPath& doc_path,
      const Value& value,
      HybridTime hybrid_time,
      const ReadHybridTime& read_ht = ReadHybridTime::Max());

  CHECKED_STATUS SetPrimitive(
      const DocPath& doc_path,
      const PrimitiveValue& value,
      HybridTime hybrid_time,
      const ReadHybridTime& read_ht = ReadHybridTime::Max());

  CHECKED_STATUS AddExternalIntents(
      const TransactionId& txn_id,
      const std::vector<ExternalIntent>& intents,
      const Uuid& involved_tablet,
      HybridTime hybrid_time);

  CHECKED_STATUS InsertSubDocument(
      const DocPath& doc_path,
      const SubDocument& value,
      HybridTime hybrid_time,
      MonoDelta ttl = Value::kMaxTtl,
      const ReadHybridTime& read_ht = ReadHybridTime::Max());

  CHECKED_STATUS ExtendSubDocument(
      const DocPath& doc_path,
      const SubDocument& value,
      HybridTime hybrid_time,
      MonoDelta ttl = Value::kMaxTtl,
      const ReadHybridTime& read_ht = ReadHybridTime::Max());

  CHECKED_STATUS ExtendList(
      const DocPath& doc_path,
      const SubDocument& value,
      HybridTime hybrid_time,
      const ReadHybridTime& read_ht = ReadHybridTime::Max());

  CHECKED_STATUS ReplaceInList(
      const DocPath &doc_path,
      const std::vector<int>& indexes,
      const std::vector<SubDocument>& values,
      const ReadHybridTime& read_ht,
      const HybridTime& hybrid_time,
      const rocksdb::QueryId query_id,
      MonoDelta default_ttl = Value::kMaxTtl,
      MonoDelta ttl = Value::kMaxTtl,
      UserTimeMicros user_timestamp = Value::kInvalidUserTimestamp);

  CHECKED_STATUS DeleteSubDoc(
      const DocPath& doc_path,
      HybridTime hybrid_time,
      const ReadHybridTime& read_ht = ReadHybridTime::Max());

  void DocDBDebugDumpToConsole();

  CHECKED_STATUS FlushRocksDbAndWait();

  void SetTableTTL(uint64_t ttl_msec);

  void SetCurrentTransactionId(const TransactionId& txn_id) {
    current_txn_id_ = txn_id;
  }

  void SetTransactionIsolationLevel(IsolationLevel isolation_level) {
    txn_isolation_level_ = isolation_level;
  }

  void ResetCurrentTransactionId() {
    current_txn_id_.reset();
  }

  CHECKED_STATUS DisableCompactions() {
    regular_db_options_.compaction_style = rocksdb::kCompactionStyleNone;
    intents_db_options_.compaction_style = rocksdb::kCompactionStyleNone;
    return ReopenRocksDB();
  }

  CHECKED_STATUS ReinitDBOptions();

  std::atomic<int64_t>& monotonic_counter() {
    return monotonic_counter_;
  }

  DocWriteBatch MakeDocWriteBatch();
  DocWriteBatch MakeDocWriteBatch(InitMarkerBehavior init_marker_behavior);

  void SetInitMarkerBehavior(InitMarkerBehavior init_marker_behavior);

  // Returns DocWriteBatch created with MakeDocWriteBatch.
  // Could be used when single DocWriteBatch is enough.
  DocWriteBatch& DefaultDocWriteBatch();

 protected:
  std::string IntentsDBDir();

  std::unique_ptr<rocksdb::DB> regular_db_;
  std::unique_ptr<rocksdb::DB> intents_db_;
  rocksdb::Options regular_db_options_;
  rocksdb::Options intents_db_options_;
  std::string rocksdb_dir_;

  // This is used for auto-assigning op ids to RocksDB write batches to emulate what a tablet would
  // do in production.
  rocksdb::OpId op_id_;

  std::shared_ptr<rocksdb::Cache> block_cache_;
  std::shared_ptr<ManualHistoryRetentionPolicy> retention_policy_ {
      std::make_shared<ManualHistoryRetentionPolicy>() };

  rocksdb::WriteOptions write_options_;
  Schema schema_;
  boost::optional<TransactionId> current_txn_id_;
  mutable IntraTxnWriteId intra_txn_write_id_ = 0;
  IsolationLevel txn_isolation_level_ = IsolationLevel::NON_TRANSACTIONAL;
  InitMarkerBehavior init_marker_behavior_ = InitMarkerBehavior::kOptional;

 private:
  std::atomic<int64_t> monotonic_counter_{0};
  boost::optional<DocWriteBatch> doc_write_batch_;
};

}  // namespace docdb
}  // namespace yb

#endif // YB_DOCDB_DOCDB_UTIL_H
