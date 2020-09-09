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

#ifndef YB_MASTER_BACKFILL_INDEX_H
#define YB_MASTER_BACKFILL_INDEX_H

#include <string>
#include <vector>

#include "yb/common/index.h"
#include "yb/common/partition.h"
#include "yb/master/async_rpc_tasks.h"
#include "yb/master/catalog_entity_info.h"
#include "yb/server/monitored_task.h"
#include "yb/util/format.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/status.h"

namespace yb {
namespace master {

class CatalogManager;

// Implements a multi-stage alter table. As of Dec 30 2019, used for adding an
// index to an existing table, such that the index can be backfilled with
// historic data in an online manner.
//
class MultiStageAlterTable {
 public:
  // Launches the next stage of the multi stage schema change. Updates the
  // table info, upon the completion of an alter table round if we are in the
  // middle of an index backfill. Will update the IndexPermission from
  // INDEX_PERM_DELETE_ONLY -> INDEX_PERM_WRITE_AND_DELETE -> BACKFILL
  static Status LaunchNextTableInfoVersionIfNecessary(
      CatalogManager* mgr, const scoped_refptr<TableInfo>& Info, uint32_t current_version);

  // Clears the ALTERING state for the given table and updates it to RUNNING.
  // If the version has changed and does not match the expected version no
  // change is made.
  static Status ClearAlteringState(
      CatalogManager* mgr, const scoped_refptr<TableInfo>& table, uint32_t expected_version);

  // Copies the current schema, schema_version, indexes and index_info
  // into their fully_applied_* equivalents. This is useful to ensure
  // that the master returns the fully applied version of the table schema
  // while the next alter table is in progress.
  static void CopySchemaDetailsToFullyApplied(SysTablesEntryPB* state);

  // Updates and persists the IndexPermission corresponding to the index_table_id for
  // the indexed_table's TableInfo.
  // Returns whether any permissions were actually updated (leading to a version being incremented).
  static Result<bool> UpdateIndexPermission(
      CatalogManager* mgr, const scoped_refptr<TableInfo>& indexed_table,
      const std::unordered_map<TableId, IndexPermissions>& perm_mapping,
      boost::optional<uint32_t> current_version = boost::none);

 private:
  // Start Index Backfill process/step for the specified table/index.
  static Status
  StartBackfillingData(CatalogManager *catalog_manager,
                       const scoped_refptr<TableInfo> &indexed_table,
                       IndexInfoPB idx_info);
};

class BackfillTablet;
class BackfillChunk;
class BackfillTableJob;

// This class is responsible for backfilling the specified indexes on the
// indexed_table.
class BackfillTable : public std::enable_shared_from_this<BackfillTable> {
 public:
  BackfillTable(Master *master, ThreadPool *callback_pool,
                const scoped_refptr<TableInfo> &indexed_table,
                std::vector<IndexInfoPB> indexes,
                const scoped_refptr<NamespaceInfo> &ns_info);

  void Launch();

  Status UpdateSafeTime(const Status& s, HybridTime ht);

  void Done(const Status& s);

  Master* master() { return master_; }

  ThreadPool* threadpool() { return callback_pool_; }

  const std::vector<IndexInfoPB>& indexes() const { return indexes_to_build_; }

  std::string index_ids() const {
    return index_ids_;
  }

  int32_t schema_version() const { return schema_version_; }

  std::string LogPrefix() const;

  std::string description() const;

  bool done() const {
    return done_.load(std::memory_order_acquire);
  }

  bool timestamp_chosen() const {
    return timestamp_chosen_.load(std::memory_order_acquire);
  }

  HybridTime read_time_for_backfill() const {
    std::lock_guard<simple_spinlock> l(mutex_);
    return read_time_for_backfill_;
  }

  int64_t leader_term() const {
    return leader_term_;
  }

  const std::string GetNamespaceName() const;

 private:
  void LaunchComputeSafeTimeForRead();

  void LaunchBackfill();

  CHECKED_STATUS AlterTableStateToSuccess();

  CHECKED_STATUS AlterTableStateToAbort();

  CHECKED_STATUS ClearCheckpointStateInTablets();

  // We want to prevent major compactions from garbage collecting delete markers
  // on an index table, until the backfill process is complete.
  // This API is used at the end of a successful backfill to enable major compactions
  // to gc delete markers on an index table.
  CHECKED_STATUS
  AllowCompactionsToGCDeleteMarkers(const TableId &index_table_id);

  // Send the "backfill done request" to all tablets of the specified table.
  CHECKED_STATUS SendRpcToAllowCompactionsToGCDeleteMarkers(
      const scoped_refptr<TableInfo> &index_table);

  // Send the "backfill done request" to the specified tablet.
  CHECKED_STATUS SendRpcToAllowCompactionsToGCDeleteMarkers(
      const scoped_refptr<TabletInfo> &index_table_tablet);

  Master* master_;
  ThreadPool* callback_pool_;
  const scoped_refptr<TableInfo> indexed_table_;
  const std::vector<IndexInfoPB> indexes_to_build_;
  int32_t schema_version_;
  int64_t leader_term_;

  std::string index_ids_;
  std::atomic_bool done_{false};
  std::atomic_bool timestamp_chosen_{false};
  std::atomic<size_t> tablets_pending_;
  std::atomic<size_t> num_tablets_;
  std::shared_ptr<BackfillTableJob> backfill_job_;
  mutable simple_spinlock mutex_;
  HybridTime read_time_for_backfill_ GUARDED_BY(mutex_){HybridTime::kMin};
  const scoped_refptr<NamespaceInfo> ns_info_;
};

class BackfillTableJob : public MonitoredTask {
 public:
  explicit BackfillTableJob(std::shared_ptr<BackfillTable> backfill_table)
      : start_timestamp_(MonoTime::Now()), backfill_table_(backfill_table),
        index_ids_(backfill_table_->index_ids()) {}

  Type type() const override { return BACKFILL_TABLE; }

  std::string type_name() const override { return "Backfill Table"; }

  MonoTime start_timestamp() const override { return start_timestamp_; }

  MonoTime completion_timestamp() const override {
    return completion_timestamp_;
  }

  std::string description() const override;

  MonitoredTaskState state() const override {
    return state_.load(std::memory_order_acquire);
  }

  void SetState(MonitoredTaskState new_state);

  MonitoredTaskState AbortAndReturnPrevState(const Status& status) override;

  void MarkDone() {
    completion_timestamp_ = MonoTime::Now();
    backfill_table_ = nullptr;
  }

 private:
  MonoTime start_timestamp_, completion_timestamp_;
  std::atomic<MonitoredTaskState> state_{MonitoredTaskState::kWaiting};
  std::shared_ptr<BackfillTable> backfill_table_;
  std::string index_ids_;
};

// A background task which is responsible for backfilling rows from a given
// tablet in the indexed table.
class BackfillTablet : public std::enable_shared_from_this<BackfillTablet> {
 public:
  BackfillTablet(
      std::shared_ptr<BackfillTable> backfill_table, const scoped_refptr<TabletInfo>& tablet);

  void Launch() { LaunchNextChunkOrDone(); }

  void LaunchNextChunkOrDone();
  void Done(const Status& status, const std::string& optional_next_row);

  Master* master() { return backfill_table_->master(); }

  ThreadPool* threadpool() { return backfill_table_->threadpool(); }

  HybridTime read_time_for_backfill() {
    return backfill_table_->read_time_for_backfill();
  }

  const std::vector<IndexInfoPB>& indexes() { return backfill_table_->indexes(); }

  std::string index_ids() { return backfill_table_->index_ids(); }

  int32_t schema_version() { return backfill_table_->schema_version(); }

  const scoped_refptr<TabletInfo> tablet() { return tablet_; }

  bool done() const {
    return done_.load(std::memory_order_acquire);
  }

  const std::string GetNamespaceName() const { return backfill_table_->GetNamespaceName(); }

 private:
  std::shared_ptr<BackfillTable> backfill_table_;
  const scoped_refptr<TabletInfo> tablet_;
  Partition partition_;

  // if non-empty, corresponds to the row in the tablet up to which
  // backfill has been already processed (non-inclusive). The next
  // request to backfill has to start backfilling from this row till
  // the end of the tablet range.
  std::string next_row_to_backfill_;
  std::atomic_bool done_{false};
};

class GetSafeTimeForTablet : public RetryingTSRpcTask {
 public:
  GetSafeTimeForTablet(
      std::shared_ptr<BackfillTable> backfill_table,
      const scoped_refptr<TabletInfo>& tablet,
      HybridTime min_cutoff)
      : RetryingTSRpcTask(
            backfill_table->master(), backfill_table->threadpool(),
            gscoped_ptr<TSPicker>(new PickLeaderReplica(tablet)), tablet->table().get()),
        backfill_table_(backfill_table),
        tablet_(tablet),
        min_cutoff_(min_cutoff) {
    deadline_ = MonoTime::Max();  // Never time out.
  }

  void Launch();

  Type type() const override { return ASYNC_GET_SAFE_TIME; }

  std::string type_name() const override { return "Get SafeTime for Tablet"; }

  std::string description() const override {
    return yb::Format("GetSafeTime for $0 Backfilling index tables $1",
                      tablet_id(), backfill_table_->index_ids());
  }

 private:
  TabletId tablet_id() const override { return tablet_->id(); }

  void HandleResponse(int attempt) override;

  bool SendRequest(int attempt) override;

  void UnregisterAsyncTaskCallback() override;

  TabletServerId permanent_uuid() {
    return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
  }

  tserver::GetSafeTimeResponsePB resp_;
  const std::shared_ptr<BackfillTable> backfill_table_;
  const scoped_refptr<TabletInfo> tablet_;
  const HybridTime min_cutoff_;
};

// A background task which is responsible for backfilling rows in the partitions
// [start, end) on the indexed table.
class BackfillChunk : public RetryingTSRpcTask {
 public:
  BackfillChunk(std::shared_ptr<BackfillTablet> backfill_tablet,
                const std::string& start_key)
      : RetryingTSRpcTask(backfill_tablet->master(),
                          backfill_tablet->threadpool(),
                          gscoped_ptr<TSPicker>(new PickLeaderReplica(
                              backfill_tablet->tablet())),
                          backfill_tablet->tablet()->table().get()),
        backfill_tablet_(backfill_tablet), start_key_(start_key) {
    deadline_ = MonoTime::Max(); // Never time out.
  }

  void Launch();

  Type type() const override { return ASYNC_BACKFILL_TABLET_CHUNK; }

  std::string type_name() const override { return "Backfill Index Table"; }

  std::string description() const override {
    return yb::Format("Backfilling index_ids $0 : for $1 from $2",
                      backfill_tablet_->index_ids(), tablet_id(),
                      b2a_hex(start_key_));
  }

  MonoTime ComputeDeadline() override;

 private:
  TabletId tablet_id() const override { return backfill_tablet_->tablet()->id(); }

  void HandleResponse(int attempt) override;

  bool SendRequest(int attempt) override;

  void UnregisterAsyncTaskCallback() override;

  TabletServerId permanent_uuid() {
    return target_ts_desc_ != nullptr ? target_ts_desc_->permanent_uuid() : "";
  }

  int num_max_retries() override;
  int max_delay_ms() override;

  tserver::BackfillIndexResponsePB resp_;
  std::shared_ptr<BackfillTablet> backfill_tablet_;
  std::string start_key_;
};

}  // namespace master
}  // namespace yb

#endif  // YB_MASTER_BACKFILL_INDEX_H
