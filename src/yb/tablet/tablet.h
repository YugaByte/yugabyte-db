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
#ifndef YB_TABLET_TABLET_H_
#define YB_TABLET_TABLET_H_

#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "yb/client/client.h"
#include "yb/client/meta_data_cache.h"
#include "yb/client/transaction_manager.h"

#include "yb/common/schema.h"
#include "yb/common/transaction.h"
#include "yb/common/ql_storage_interface.h"

#include "yb/docdb/docdb.pb.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb_compaction_filter.h"
#include "yb/docdb/doc_operation.h"
#include "yb/docdb/ql_rocksdb_storage.h"
#include "yb/docdb/shared_lock_manager.h"

#include "yb/gutil/atomicops.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/thread_annotations.h"

#include "yb/rocksdb/cache.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/statistics.h"
#include "yb/rocksdb/write_batch.h"

#include "yb/rpc/rpc_fwd.h"

#include "yb/tablet/abstract_tablet.h"
#include "yb/tablet/mvcc.h"
#include "yb/tablet/operations/snapshot_operation.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_options.h"
#include "yb/tablet/transaction_participant.h"

#include "yb/util/countdown_latch.h"
#include "yb/util/enums.h"
#include "yb/util/locks.h"
#include "yb/util/metrics.h"
#include "yb/util/operation_counter.h"
#include "yb/util/semaphore.h"
#include "yb/util/slice.h"
#include "yb/util/status.h"
#include "yb/util/strongly_typed_bool.h"
#include "yb/util/threadpool.h"



namespace rocksdb {
class DB;
}

namespace yb {

class MemTracker;
class MetricEntity;
class RowChangeList;

namespace docdb {
class ConsensusFrontier;
}

namespace log {
class LogAnchorRegistry;
}

namespace server {
class Clock;
}

class MaintenanceManager;
class MaintenanceOp;
class MaintenanceOpStats;

namespace tablet {

class ChangeMetadataOperationState;
class ScopedReadOperation;
class TabletRetentionPolicy;
class TransactionCoordinator;
class TransactionCoordinatorContext;
class TransactionParticipant;
class TruncateOperationState;
class WriteOperationState;

struct TabletMetrics;
struct TransactionApplyData;

using docdb::LockBatch;

YB_STRONGLY_TYPED_BOOL(IncludeIntents);
YB_STRONGLY_TYPED_BOOL(Destroy);
YB_STRONGLY_TYPED_BOOL(DisableFlushOnShutdown);

YB_DEFINE_ENUM(FlushMode, (kSync)(kAsync));

enum class FlushFlags {
  kNone = 0,

  kRegular = 1,
  kIntents = 2,

  kAll = kRegular | kIntents
};

inline FlushFlags operator|(FlushFlags lhs, FlushFlags rhs) {
  return static_cast<FlushFlags>(to_underlying(lhs) | to_underlying(rhs));
}

inline FlushFlags operator&(FlushFlags lhs, FlushFlags rhs) {
  return static_cast<FlushFlags>(to_underlying(lhs) & to_underlying(rhs));
}

inline bool HasFlags(FlushFlags lhs, FlushFlags rhs) {
  return (lhs & rhs) != FlushFlags::kNone;
}

class WriteOperation;

using AddTableListener = std::function<Status(const TableInfo&)>;
using DocWriteOperationCallback =
    boost::function<void(std::unique_ptr<WriteOperation>, const Status&)>;

class TabletScopedIf : public RefCountedThreadSafe<TabletScopedIf> {
 public:
  virtual std::string Key() const = 0;
 protected:
  friend class RefCountedThreadSafe<TabletScopedIf>;
  virtual ~TabletScopedIf() { }
};

YB_STRONGLY_TYPED_BOOL(AllowBootstrappingState);

class Tablet : public AbstractTablet, public TransactionIntentApplier {
 public:
  class CompactionFaultHooks;
  class FlushCompactCommonHooks;
  class FlushFaultHooks;

  // A function that returns the current majority-replicated hybrid time leader lease, or waits
  // until a hybrid time leader lease with at least the given hybrid time is acquired
  // (first argument), or a timeout occurs (second argument). HybridTime::kInvalid is returned
  // in case of a timeout.
  using HybridTimeLeaseProvider = std::function<Result<FixedHybridTimeLease>(
      HybridTime, CoarseTimePoint)>;
  using TransactionIdSet = std::unordered_set<TransactionId, TransactionIdHash>;

  // Create a new tablet.
  //
  // If 'metric_registry' is non-NULL, then this tablet will create a 'tablet' entity
  // within the provided registry. Otherwise, no metrics are collected.
  explicit Tablet(const TabletInitData& data);

  ~Tablet();

  // Open the tablet.
  // Upon completion, the tablet enters the kBootstrapping state.
  CHECKED_STATUS Open();

  CHECKED_STATUS EnableCompactions(ScopedRWOperationPause* operation_pause);

  Result<std::string> BackfillIndexesForYsql(
      const std::vector<IndexInfo>& indexes,
      const std::string& backfill_from,
      const CoarseTimePoint deadline,
      const HybridTime read_time,
      const HostPort& pgsql_proxy_bind_address,
      const std::string& database_name,
      const uint64_t postgres_auth_key);
  Result<std::string> BackfillIndexes(const std::vector<IndexInfo>& indexes,
                                      const std::string& backfill_from,
                                      const CoarseTimePoint deadline,
                                      const HybridTime read_time);

  CHECKED_STATUS UpdateIndexInBatches(
      const QLTableRow& row,
      const std::vector<IndexInfo>& indexes,
      const HybridTime write_time,
      std::vector<std::pair<const IndexInfo*, QLWriteRequestPB>>* index_requests,
      CoarseTimePoint* last_flushed_at);

  CHECKED_STATUS FlushIndexBatchIfRequired(
      std::vector<std::pair<const IndexInfo*, QLWriteRequestPB>>* index_requests,
      bool force_flush,
      const HybridTime write_time,
      CoarseTimePoint* last_flushed_at);

  CHECKED_STATUS
  FlushWithRetries(
      std::shared_ptr<client::YBSession> session,
      const std::vector<std::shared_ptr<client::YBqlWriteOp>>& write_ops,
      int num_retries);

  // Mark that the tablet has finished bootstrapping.
  // This transitions from kBootstrapping to kOpen state.
  void MarkFinishedBootstrapping();

  // This can be called to proactively prevent new operations from being handled, even before
  // Shutdown() is called.
  // Returns true if it was the first call to StartShutdown.
  bool StartShutdown();
  bool IsShutdownRequested() const {
    return shutdown_requested_.load(std::memory_order::memory_order_acquire);
  }

  void CompleteShutdown(IsDropTable is_drop_table = IsDropTable::kFalse);

  CHECKED_STATUS ImportData(const std::string& source_dir);

  Result<docdb::ApplyTransactionState> ApplyIntents(const TransactionApplyData& data) override;

  CHECKED_STATUS RemoveIntents(const RemoveIntentsData& data, const TransactionId& id) override;

  CHECKED_STATUS RemoveIntents(
      const RemoveIntentsData& data, const TransactionIdSet& transactions) override;

  // Finish the Prepare phase of a write transaction.
  //
  // Starts an MVCC transaction and assigns a timestamp for the transaction.
  //
  // This should always be done _after_ any relevant row locks are acquired
  // (using CreatePreparedInsert/CreatePreparedMutate). This ensures that,
  // within each row, timestamps only move forward. If we took a timestamp before
  // getting the row lock, we could have the following situation:
  //
  //   Thread 1         |  Thread 2
  //   ----------------------
  //   Start tx 1       |
  //                    |  Start tx 2
  //                    |  Obtain row lock
  //                    |  Update row
  //                    |  Commit tx 2
  //   Obtain row lock  |
  //   Delete row       |
  //   Commit tx 1
  //
  // This would cause the mutation list to look like: @t1: DELETE, @t2: UPDATE
  // which is invalid, since we expect to be able to be able to replay mutations
  // in increasing timestamp order on a given row.
  //
  // TODO: rename this to something like "FinishPrepare" or "StartApply", since
  // it's not the first thing in a transaction!
  void StartOperation(WriteOperationState* operation_state);

  // Apply all of the row operations associated with this transaction.
  CHECKED_STATUS ApplyRowOperations(
      WriteOperationState* operation_state,
      AlreadyAppliedToRegularDB already_applied_to_regular_db = AlreadyAppliedToRegularDB::kFalse);

  CHECKED_STATUS ApplyOperationState(
      const OperationState& operation_state, int64_t batch_idx,
      const docdb::KeyValueWriteBatchPB& write_batch,
      AlreadyAppliedToRegularDB already_applied_to_regular_db = AlreadyAppliedToRegularDB::kFalse);

  // Apply a set of RocksDB row operations.
  // If rocksdb_write_batch is specified it could contain preencoded RocksDB operations.
  CHECKED_STATUS ApplyKeyValueRowOperations(
      int64_t batch_idx, // index of this batch in its transaction
      const docdb::KeyValueWriteBatchPB& put_batch,
      const rocksdb::UserFrontiers* frontiers,
      HybridTime hybrid_time,
      AlreadyAppliedToRegularDB already_applied_to_regular_db = AlreadyAppliedToRegularDB::kFalse);

  void WriteToRocksDB(
      const rocksdb::UserFrontiers* frontiers,
      rocksdb::WriteBatch* write_batch,
      docdb::StorageDbType storage_db_type);

  //------------------------------------------------------------------------------------------------
  // Redis Request Processing.
  // Takes a Redis WriteRequestPB as input with its redis_write_batch.
  // Constructs a WriteRequestPB containing a serialized WriteBatch that will be
  // replicated by Raft. (Makes a copy, it is caller's responsibility to deallocate
  // write_request afterwards if it is no longer needed).
  // The operation acquires the necessary locks required to correctly serialize concurrent write
  // operations to same/conflicting part of the key/sub-key space. The locks acquired are returned
  // via the 'keys_locked' vector, so that they may be unlocked later when the operation has been
  // committed.
  void KeyValueBatchFromRedisWriteBatch(std::unique_ptr<WriteOperation> operation);

  CHECKED_STATUS HandleRedisReadRequest(
      CoarseTimePoint deadline,
      const ReadHybridTime& read_time,
      const RedisReadRequestPB& redis_read_request,
      RedisResponsePB* response) override;

  //------------------------------------------------------------------------------------------------
  // CQL Request Processing.
  CHECKED_STATUS HandleQLReadRequest(
      CoarseTimePoint deadline,
      const ReadHybridTime& read_time,
      const QLReadRequestPB& ql_read_request,
      const TransactionMetadataPB& transaction_metadata,
      QLReadRequestResult* result) override;

  CHECKED_STATUS CreatePagingStateForRead(
      const QLReadRequestPB& ql_read_request, const size_t row_count,
      QLResponsePB* response) const override;

  // The QL equivalent of KeyValueBatchFromRedisWriteBatch, works similarly.
  void KeyValueBatchFromQLWriteBatch(std::unique_ptr<WriteOperation> operation);

  //------------------------------------------------------------------------------------------------
  // Postgres Request Processing.
  CHECKED_STATUS HandlePgsqlReadRequest(
      CoarseTimePoint deadline,
      const ReadHybridTime& read_time,
      bool is_explicit_request_read_time,
      const PgsqlReadRequestPB& pgsql_read_request,
      const TransactionMetadataPB& transaction_metadata,
      PgsqlReadRequestResult* result,
      size_t* num_rows_read) override;

  CHECKED_STATUS CreatePagingStateForRead(
      const PgsqlReadRequestPB& pgsql_read_request, const size_t row_count,
      PgsqlResponsePB* response) const override;

  CHECKED_STATUS PreparePgsqlWriteOperations(WriteOperation* operation);
  void KeyValueBatchFromPgsqlWriteBatch(std::unique_ptr<WriteOperation> operation);

  // Create a new row iterator which yields the rows as of the current MVCC
  // state of this tablet.
  // The returned iterator is not initialized.
  Result<std::unique_ptr<common::YQLRowwiseIteratorIf>> NewRowIterator(
      const Schema& projection,
      const boost::optional<TransactionId>& transaction_id,
      const ReadHybridTime read_hybrid_time = {},
      const TableId& table_id = "",
      CoarseTimePoint deadline = CoarseTimePoint::max(),
      AllowBootstrappingState allow_bootstrapping_state = AllowBootstrappingState::kFalse) const;
  Result<std::unique_ptr<common::YQLRowwiseIteratorIf>> NewRowIterator(
      const TableId& table_id) const;

  //------------------------------------------------------------------------------------------------
  // Makes RocksDB Flush.
  CHECKED_STATUS Flush(FlushMode mode,
                       FlushFlags flags = FlushFlags::kAll,
                       int64_t ignore_if_flushed_after_tick = rocksdb::FlushOptions::kNeverIgnore);

  CHECKED_STATUS WaitForFlush();

  // Prepares the transaction context for the alter schema operation.
  // An error will be returned if the specified schema is invalid (e.g.
  // key mismatch, or missing IDs)
  CHECKED_STATUS CreatePreparedChangeMetadata(
      ChangeMetadataOperationState *operation_state,
      const Schema* schema);

  // Apply the Schema of the specified operation.
  CHECKED_STATUS AlterSchema(ChangeMetadataOperationState* operation_state);

  // Used to update the tablets on the index table that the index has been backfilled.
  // This means that major compactions can now garbage collect delete markers.
  CHECKED_STATUS MarkBackfillDone();

  // Change wal_retention_secs in the metadata.
  CHECKED_STATUS AlterWalRetentionSecs(ChangeMetadataOperationState* operation_state);

  // Apply replicated add table operation.
  CHECKED_STATUS AddTable(const TableInfoPB& table_info);

  // Apply replicated remove table operation.
  CHECKED_STATUS RemoveTable(const std::string& table_id);

  // Truncate this tablet by resetting the content of RocksDB.
  CHECKED_STATUS Truncate(TruncateOperationState* state);

  // Verbosely dump this entire tablet to the logs. This is only
  // really useful when debugging unit tests failures where the tablet
  // has a very small number of rows.
  CHECKED_STATUS DebugDump(vector<std::string> *lines = NULL);

  const yb::SchemaPtr schema() const {
    return metadata_->schema();
  }

  // Returns a reference to the key projection of the tablet schema.
  // The schema keys are immutable.
  const Schema& key_schema() const { return key_schema_; }

  // Return the MVCC manager for this tablet.
  MvccManager* mvcc_manager() { return &mvcc_; }

  docdb::SharedLockManager* shared_lock_manager() { return &shared_lock_manager_; }

  std::atomic<int64_t>* monotonic_counter() { return &monotonic_counter_; }

  // Set the conter to at least 'value'.
  void UpdateMonotonicCounter(int64_t value);

  const RaftGroupMetadata *metadata() const { return metadata_.get(); }
  RaftGroupMetadata *metadata() { return metadata_.get(); }

  rocksdb::Env& rocksdb_env() const;

  const std::string& tablet_id() const override { return metadata_->raft_group_id(); }

  // Return the metrics for this tablet.
  // May be NULL in unit tests, etc.
  TabletMetrics* metrics() { return metrics_.get(); }

  // Return handle to the metric entity of this tablet.
  const scoped_refptr<MetricEntity>& GetMetricEntity() const { return metric_entity_; }

  // Returns a reference to this tablet's memory tracker.
  const std::shared_ptr<MemTracker>& mem_tracker() const { return mem_tracker_; }

  TableType table_type() const override { return table_type_; }

  // Returns true if a RocksDB-backed tablet has any SSTables.
  Result<bool> HasSSTables() const;

  // Returns the maximum persistent op id from all SSTables in RocksDB.
  // First for regular records and second for intents.
  // When invalid_if_no_new_data is true then function would return invalid op id when no new
  // data is present in corresponding db.
  Result<DocDbOpIds> MaxPersistentOpId(bool invalid_if_no_new_data = false) const;

  // Returns the maximum persistent hybrid_time across all SSTables in RocksDB.
  Result<HybridTime> MaxPersistentHybridTime() const;

  // Returns oldest mutable memtable write hybrid time in RocksDB or HybridTime::kMax if memtable
  // is empty.
  Result<HybridTime> OldestMutableMemtableWriteHybridTime() const;

  // For non-kudu table type fills key-value batch in transaction state request and updates
  // request in state. Due to acquiring locks it can block the thread.
  void AcquireLocksAndPerformDocOperations(std::unique_ptr<WriteOperation> operation);

  // Given a propopsed "history cutoff" timestamp, returns either that value, if possible, or a
  // smaller value corresponding to the oldest active reader, whichever is smaller. This ensures
  // that data needed by active read operations is not compacted away.
  //
  // Also updates the "earliest allowed read time" of the tablet to be equal to the returned value,
  // (if it is still lower than the value about to be returned), so that new readers with timestamps
  // earlier than that will be rejected.
  HybridTime UpdateHistoryCutoff(HybridTime proposed_cutoff);

  const scoped_refptr<server::Clock> &clock() const {
    return clock_;
  }

  yb::SchemaPtr GetSchema(const std::string& table_id = "") const override {
    if (table_id.empty()) {
      return metadata_->schema();
    }
    auto table_info = CHECK_RESULT(metadata_->GetTableInfo(table_id));
    return yb::SchemaPtr(table_info, &table_info->schema);
  }

  Schema GetKeySchema(const std::string& table_id = "") const {
    if (table_id.empty()) {
      return key_schema_;
    }
    auto table_info = CHECK_RESULT(metadata_->GetTableInfo(table_id));
    return table_info->schema.CreateKeyProjection();
  }

  const common::YQLStorageIf& QLStorage() const override {
    return *ql_storage_;
  }

  // Provide a way for write operations to wait when tablet schema is
  // being changed.
  ScopedRWOperationPause PauseWritePermits(CoarseTimePoint deadline);
  ScopedRWOperation GetPermitToWrite(CoarseTimePoint deadline);

  // Used from tests
  const std::shared_ptr<rocksdb::Statistics>& regulardb_statistics() const {
    return regulardb_statistics_;
  }

  const std::shared_ptr<rocksdb::Statistics>& intentsdb_statistics() const {
    return intentsdb_statistics_;
  }

  TransactionCoordinator* transaction_coordinator() {
    return transaction_coordinator_.get();
  }

  TransactionParticipant* transaction_participant() const {
    return transaction_participant_.get();
  }

  // Returns true if the tablet was created after a split but it has not yet had data from it's
  // parent which are now outside of its key range removed.
  bool StillHasParentDataAfterSplit();

  void ForceRocksDBCompactInTest();

  CHECKED_STATUS ForceFullRocksDBCompact();

  docdb::DocDB doc_db() const { return { regular_db_.get(), intents_db_.get(), &key_bounds_ }; }

  // Returns approximate middle key for tablet split:
  // - for hash-based partitions: encoded hash code in order to split by hash code.
  // - for range-based partitions: encoded doc key in order to split by row.
  Result<std::string> GetEncodedMiddleSplitKey() const;

  std::string TEST_DocDBDumpStr(IncludeIntents include_intents = IncludeIntents::kFalse);

  void TEST_DocDBDumpToContainer(
      IncludeIntents include_intents, std::unordered_set<std::string>* out);

  size_t TEST_CountRegularDBRecords();

  CHECKED_STATUS CreateReadIntents(
      const TransactionMetadataPB& transaction_metadata,
      const google::protobuf::RepeatedPtrField<QLReadRequestPB>& ql_batch,
      const google::protobuf::RepeatedPtrField<PgsqlReadRequestPB>& pgsql_batch,
      docdb::KeyValueWriteBatchPB* out);

  uint64_t GetCurrentVersionSstFilesSize() const;
  uint64_t GetCurrentVersionSstFilesUncompressedSize() const;
  uint64_t GetCurrentVersionNumSSTFiles() const;

  void ListenNumSSTFilesChanged(std::function<void()> listener);

  // Returns the number of memtables in intents and regular db-s.
  std::pair<int, int> GetNumMemtables() const;

  void SetHybridTimeLeaseProvider(HybridTimeLeaseProvider provider) {
    ht_lease_provider_ = std::move(provider);
  }

  void SetMemTableFlushFilterFactory(std::function<rocksdb::MemTableFilter()> factory) {
    mem_table_flush_filter_factory_ = std::move(factory);
  }

  // When a compaction starts with a particular "history cutoff" timestamp, it calls this function
  // to disallow reads at a time lower than that history cutoff timestamp, to avoid reading
  // invalid/incomplete data.
  //
  // Returns true if the new history cutoff timestamp was successfully registered, or false if
  // it can't be used because there are pending reads at lower timestamps.
  HybridTime Get(HybridTime lower_bound);

  bool ShouldApplyWrite();

  rocksdb::DB* TEST_db() const {
    return regular_db_.get();
  }

  rocksdb::DB* TEST_intents_db() const {
    return intents_db_.get();
  }

  CHECKED_STATUS TEST_SwitchMemtable();

  // Initialize RocksDB's max persistent op id and hybrid time to that of the operation state.
  // Necessary for cases like truncate or restore snapshot when RocksDB is reset.
  CHECKED_STATUS ModifyFlushedFrontier(
      const docdb::ConsensusFrontier& value,
      rocksdb::FrontierModificationMode mode);

  // Get the isolation level of the given transaction from the metadata stored in the provisional
  // records RocksDB.
  Result<IsolationLevel> GetIsolationLevel(const TransactionMetadataPB& transaction) override;

  // Creates an on-disk sub tablet of this tablet with specified ID, partition and key bounds.
  // Flushes this tablet data onto disk before creating sub tablet.
  // Also updates flushed frontier for regular and intents DBs to match split_op_id and
  // split_op_hybrid_time.
  // In case of error sub-tablet could be partially persisted on disk.
  Result<RaftGroupMetadataPtr> CreateSubtablet(
      const TabletId& tablet_id, const Partition& partition, const docdb::KeyBounds& key_bounds,
      const yb::OpId& split_op_id, const HybridTime& split_op_hybrid_time);

  // Scans the intent db. Potentially takes a long time. Used for testing/debugging.
  Result<int64_t> CountIntents();

  // Flushed intents db if necessary.
  void FlushIntentsDbIfNecessary(const yb::OpId& lastest_log_entry_op_id);

  bool is_sys_catalog() const { return is_sys_catalog_; }
  bool IsTransactionalRequest(bool is_ysql_request) const override;

  void SetCleanupPool(ThreadPool* thread_pool);

  TabletSnapshots& snapshots() {
    return *snapshots_;
  }

  SnapshotCoordinator* snapshot_coordinator() {
    return snapshot_coordinator_;
  }

  // Allows us to add tablet-specific information that will get deref'd when the tablet does.
  void AddAdditionalMetadata(const std::string& key, std::shared_ptr<void> additional_metadata) {
    std::lock_guard<std::mutex> lock(control_path_mutex_);
    additional_metadata_.emplace(key, std::move(additional_metadata));
  }

  std::shared_ptr<void> GetAdditionalMetadata(const std::string& key) {
    std::lock_guard<std::mutex> lock(control_path_mutex_);
    auto val = additional_metadata_.find(key);
    return (val != additional_metadata_.end()) ? val->second : nullptr;
  }

  void InitRocksDBOptions(rocksdb::Options* options, const std::string& log_prefix);

  TabletRetentionPolicy* RetentionPolicy() override {
    return retention_policy_.get();
  }

  // Triggers a compaction on this tablet if it is the result of a tablet split but has not yet been
  // compacted. Assumes ownership of the provided thread pool token, and uses it to submit the
  // compaction task. It is an error to call this method if a post-split compaction has been
  // triggered previously by this tablet.
  CHECKED_STATUS TriggerPostSplitCompactionIfNeeded(
    std::function<std::unique_ptr<ThreadPoolToken>()> get_token_for_compaction);

 private:
  friend class Iterator;
  friend class TabletPeerTest;
  friend class ScopedReadOperation;
  friend class TabletComponent;

  class RegularRocksDbListener;

  FRIEND_TEST(TestTablet, TestGetLogRetentionSizeForIndex);

  void StartDocWriteOperation(
      std::unique_ptr<WriteOperation> operation,
      ScopedRWOperation scoped_read_operation,
      DocWriteOperationCallback callback);

  CHECKED_STATUS OpenKeyValueTablet();
  virtual CHECKED_STATUS CreateTabletDirectories(const string& db_dir, FsManager* fs);

  void DocDBDebugDump(std::vector<std::string> *lines);

  CHECKED_STATUS PrepareTransactionWriteBatch(
      int64_t batch_idx, // index of this batch in its transaction
      const docdb::KeyValueWriteBatchPB& put_batch,
      HybridTime hybrid_time,
      rocksdb::WriteBatch* rocksdb_write_batch);

  Result<TransactionOperationContextOpt> CreateTransactionOperationContext(
      const TransactionMetadataPB& transaction_metadata,
      bool is_ysql_catalog_table) const;

  TransactionOperationContextOpt CreateTransactionOperationContext(
      const boost::optional<TransactionId>& transaction_id,
      bool is_ysql_catalog_table) const;

  // Pause any new read/write operations and wait for all pending read/write operations to finish.
  ScopedRWOperationPause PauseReadWriteOperations(Stop stop = Stop::kFalse);

  CHECKED_STATUS ResetRocksDBs(Destroy destroy, DisableFlushOnShutdown disable_flush_on_shutdown);

  CHECKED_STATUS DoEnableCompactions();

  void PreventCallbacksFromRocksDBs(DisableFlushOnShutdown disable_flush_on_shutdown);

  std::string LogPrefix() const;

  std::string LogPrefix(docdb::StorageDbType db_type) const;

  Result<bool> IsQueryOnlyForTablet(const PgsqlReadRequestPB& pgsql_read_request) const;

  Result<bool> HasScanReachedMaxPartitionKey(
      const PgsqlReadRequestPB& pgsql_read_request, const string& partition_key) const;

  // Sets metadata_cache_ to nullptr. This is done atomically to avoid race conditions.
  void ResetYBMetaDataCache();

  // Creates a new client::YBMetaDataCache object and atomically assigns it to metadata_cache_.
  void CreateNewYBMetaDataCache();

  // Creates a new shared pointer of the object managed by metadata_cache_. This is done
  // atomically to avoid race conditions.
  std::shared_ptr<client::YBMetaDataCache> YBMetaDataCache();

  void TriggerPostSplitCompactionSync();

  const Schema key_schema_;

  RaftGroupMetadataPtr metadata_;
  TableType table_type_;

  // Lock protecting access to the 'components_' member (i.e the rowsets in the tablet)
  //
  // Shared mode:
  // - Writers take this in shared mode at the same time as they obtain an MVCC hybrid_time
  //   and capture a reference to components_. This ensures that we can use the MVCC hybrid_time
  //   to determine which writers are writing to which components during compaction.
  // - Readers take this in shared mode while capturing their iterators. This ensures that
  //   they see a consistent view when racing against flush/compact.
  //
  // Exclusive mode:
  // - Flushes/compactions take this lock in order to lock out concurrent updates.
  //
  // NOTE: callers should avoid taking this lock for a long time, even in shared mode.
  // This is because the lock has some concept of fairness -- if, while a long reader
  // is active, a writer comes along, then all future short readers will be blocked.
  // TODO: now that this is single-threaded again, we should change it to rw_spinlock
  mutable rw_spinlock component_lock_;

  scoped_refptr<log::LogAnchorRegistry> log_anchor_registry_;
  std::shared_ptr<MemTracker> mem_tracker_;
  std::shared_ptr<MemTracker> block_based_table_mem_tracker_;

  MetricEntityPtr metric_entity_;
  gscoped_ptr<TabletMetrics> metrics_;
  FunctionGaugeDetacher metric_detacher_;

  // A pointer to the server's clock.
  scoped_refptr<server::Clock> clock_;

  MvccManager mvcc_;

  // Lock used to serialize the creation of RocksDB checkpoints.
  mutable std::mutex create_checkpoint_lock_;

  enum State {
    kInitialized,
    kBootstrapping,
    kOpen,
    kShutdown
  };
  State state_ = kInitialized;

  // Fault hooks. In production code, these will always be NULL.
  std::shared_ptr<CompactionFaultHooks> compaction_hooks_;
  std::shared_ptr<FlushFaultHooks> flush_hooks_;
  std::shared_ptr<FlushCompactCommonHooks> common_hooks_;

  // Statistics for the RocksDB database.
  std::shared_ptr<rocksdb::Statistics> regulardb_statistics_;
  std::shared_ptr<rocksdb::Statistics> intentsdb_statistics_;

  // RocksDB database for key-value tables.
  std::unique_ptr<rocksdb::DB> regular_db_;

  std::unique_ptr<rocksdb::DB> intents_db_;

  // Optional key bounds (see docdb::KeyBounds) served by this tablet.
  docdb::KeyBounds key_bounds_;

  std::unique_ptr<common::YQLStorageIf> ql_storage_;

  // This is for docdb fine-grained locking.
  docdb::SharedLockManager shared_lock_manager_;

  // For the block cache and memory manager shared across tablets
  TabletOptions tablet_options_;

  // A lightweight way to reject new operations when the tablet is shutting down. This is used to
  // prevent race conditions between destroying the RocksDB instance and read/write operations.
  std::atomic_bool shutdown_requested_{false};

  // This is a special atomic counter per tablet that increases monotonically.
  // It is like timestamp, but doesn't need locks to read or update.
  // This is raft replicated as well. Each replicate message contains the current number.
  // It is guaranteed to keep increasing for committed entries even across tablet server
  // restarts and leader changes.
  std::atomic<int64_t> monotonic_counter_{0};

  // Number of pending operations. We use this to make sure we don't shut down RocksDB before all
  // pending operations are finished. We don't have a strict definition of an "operation" for the
  // purpose of this counter. We simply wait for this counter to go to zero before shutting down
  // RocksDB.
  //
  // This is marked mutable because read path member functions (which are const) are using this.
  mutable RWOperationCounter pending_op_counter_;

  // Used by Alter/Schema-change ops to pause new write ops from being submitted.
  RWOperationCounter write_ops_being_submitted_counter_;

  std::unique_ptr<TransactionCoordinator> transaction_coordinator_;

  std::unique_ptr<TransactionParticipant> transaction_participant_;

  std::shared_future<client::YBClient*> client_future_;

  // Created only when secondary indexes are present.
  boost::optional<client::TransactionManager> transaction_manager_;

  // This object should not be accessed directly to avoid race conditions.
  // Use methods YBMetaDataCache, CreateNewYBMetaDataCache, and ResetYBMetaDataCache to read it
  // and modify it.
  std::shared_ptr<client::YBMetaDataCache> metadata_cache_;

  // Created only if it is a unique index tablet.
  boost::optional<Schema> unique_index_key_schema_;

  std::atomic<int64_t> last_committed_write_index_{0};

  HybridTimeLeaseProvider ht_lease_provider_;

  Result<HybridTime> DoGetSafeTime(
      RequireLease require_lease, HybridTime min_allowed, CoarseTimePoint deadline) const override;

  using IndexOps = std::vector<std::pair<
      std::shared_ptr<client::YBqlWriteOp>, docdb::QLWriteOperation*>>;
  void UpdateQLIndexes(std::unique_ptr<WriteOperation> operation);
  void UpdateQLIndexesFlushed(
      WriteOperation* op, const client::YBSessionPtr& session, const client::YBTransactionPtr& txn,
      const IndexOps& index_ops, const Status& status);

  void CompleteQLWriteBatch(std::unique_ptr<WriteOperation> operation, const Status& status);

  Result<bool> IntentsDbFlushFilter(const rocksdb::MemTable& memtable);

  template <class Ids>
  CHECKED_STATUS RemoveIntentsImpl(const RemoveIntentsData& data, const Ids& ids);

  // Tries to find intent .SST files that could be deleted and remove them.
  void CleanupIntentFiles();
  void DoCleanupIntentFiles();

  void RegularDbFilesChanged();

  Result<HybridTime> ApplierSafeTime(HybridTime min_allowed, CoarseTimePoint deadline) override;

  void MinRunningHybridTimeSatisfied() override {
    CleanupIntentFiles();
  }

  template <class Functor>
  uint64_t GetRegularDbStat(const Functor& functor) const;

  std::function<rocksdb::MemTableFilter()> mem_table_flush_filter_factory_;

  client::LocalTabletFilter local_tablet_filter_;

  std::string log_prefix_suffix_;

  IsSysCatalogTablet is_sys_catalog_;
  TransactionsEnabled txns_enabled_;

  std::unique_ptr<ThreadPoolToken> cleanup_intent_files_token_;

  std::unique_ptr<TabletSnapshots> snapshots_;

  SnapshotCoordinator* snapshot_coordinator_ = nullptr;

  mutable std::mutex control_path_mutex_;
  std::unordered_map<std::string, std::shared_ptr<void>> additional_metadata_
    GUARDED_BY(control_path_mutex_);

  std::mutex num_sst_files_changed_listener_mutex_;
  std::function<void()> num_sst_files_changed_listener_
      GUARDED_BY(num_sst_files_changed_listener_mutex_);

  std::shared_ptr<TabletRetentionPolicy> retention_policy_;

  // Thread pool token for manually triggering compactions for tablets created from a split. This
  // member is set when a post-split compaction is triggered on this tablet as the result of a call
  // to TriggerPostSplitCompactionIfNeeded. It is an error to attempt to trigger another post-split
  // compaction if this member is already set, as the existence of this member implies that such a
  // compaction has already been triggered for this instance.
  std::unique_ptr<ThreadPoolToken> post_split_compaction_task_pool_token_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(Tablet);
};

// A helper class to manage read transactions. Grabs and registers a read point with the tablet
// when created, and deregisters the read point when this object is destructed.
class ScopedReadOperation {
 public:
  ScopedReadOperation() : tablet_(nullptr) {}
  ScopedReadOperation(ScopedReadOperation&& rhs)
      : tablet_(rhs.tablet_), read_time_(rhs.read_time_) {
    rhs.tablet_ = nullptr;
  }

  void operator=(ScopedReadOperation&& rhs);

  static Result<ScopedReadOperation> Create(
      AbstractTablet* tablet,
      RequireLease require_lease,
      ReadHybridTime read_time);

  ScopedReadOperation(const ScopedReadOperation&) = delete;
  void operator=(const ScopedReadOperation&) = delete;

  ~ScopedReadOperation();

  const ReadHybridTime& read_time() const { return read_time_; }

  Status status() const { return status_; }

  void Reset();

 private:
  explicit ScopedReadOperation(
      AbstractTablet* tablet, const ReadHybridTime& read_time);

  AbstractTablet* tablet_;
  ReadHybridTime read_time_;
  Status status_;
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_TABLET_H_
