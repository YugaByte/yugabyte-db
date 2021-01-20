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
#include "yb/master/backfill_index.h"

#include <stdlib.h>

#include <algorithm>
#include <bitset>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>
#include <boost/optional.hpp>
#include <boost/thread/shared_mutex.hpp>
#include "yb/common/common_flags.h"
#include "yb/common/partial_row.h"
#include "yb/common/partition.h"
#include "yb/common/roles_permissions.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/quorum_util.h"
#include "yb/gutil/atomicops.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/mathlimits.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/escaping.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/sysinfo.h"
#include "yb/gutil/walltime.h"
#include "yb/master/async_rpc_tasks.h"
#include "yb/master/catalog_loaders.h"
#include "yb/master/catalog_manager_bg_tasks.h"
#include "yb/master/catalog_manager_util.h"
#include "yb/master/cluster_balance.h"
#include "yb/master/master.h"
#include "yb/master/master.pb.h"
#include "yb/master/master.proxy.h"
#include "yb/master/master_util.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/system_tablet.h"
#include "yb/master/tasks_tracker.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"
#include "yb/master/yql_aggregates_vtable.h"
#include "yb/master/yql_auth_resource_role_permissions_index.h"
#include "yb/master/yql_auth_role_permissions_vtable.h"
#include "yb/master/yql_auth_roles_vtable.h"
#include "yb/master/yql_columns_vtable.h"
#include "yb/master/yql_empty_vtable.h"
#include "yb/master/yql_functions_vtable.h"
#include "yb/master/yql_indexes_vtable.h"
#include "yb/master/yql_keyspaces_vtable.h"
#include "yb/master/yql_local_vtable.h"
#include "yb/master/yql_partitions_vtable.h"
#include "yb/master/yql_peers_vtable.h"
#include "yb/master/yql_size_estimates_vtable.h"
#include "yb/master/yql_tables_vtable.h"
#include "yb/master/yql_triggers_vtable.h"
#include "yb/master/yql_types_vtable.h"
#include "yb/master/yql_views_vtable.h"

#include "yb/docdb/doc_rowwise_iterator.h"

#include "yb/rpc/messenger.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/tablet/operations/change_metadata_operation.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"

#include "yb/tserver/tserver_admin.proxy.h"
#include "yb/yql/redis/redisserver/redis_constants.h"

#include "yb/util/crypt.h"
#include "yb/util/debug-util.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/math_util.h"
#include "yb/util/monotime.h"
#include "yb/util/random_util.h"
#include "yb/util/rw_mutex.h"
#include "yb/util/stopwatch.h"
#include "yb/util/thread.h"
#include "yb/util/thread_restrictions.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"
#include "yb/util/tsan_util.h"
#include "yb/util/uuid.h"

#include "yb/client/client.h"
#include "yb/client/meta_cache.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_table_name.h"

#include "yb/tserver/remote_bootstrap_client.h"

DEFINE_int32(ysql_index_backfill_rpc_timeout_ms, 30 * 60 * 1000, // 30 min.
             "Timeout used by the master when attempting to backfill a YSQL tablet during index "
             "creation.");
TAG_FLAG(ysql_index_backfill_rpc_timeout_ms, advanced);
TAG_FLAG(ysql_index_backfill_rpc_timeout_ms, runtime);

DEFINE_int32(index_backfill_rpc_timeout_ms, 1 * 30 * 1000, // 30 sec.
             "Timeout used by the master when attempting to backfill a tablet "
             "during index creation.");
TAG_FLAG(index_backfill_rpc_timeout_ms, advanced);
TAG_FLAG(index_backfill_rpc_timeout_ms, runtime);

DEFINE_int32(index_backfill_rpc_max_retries, 150,
             "Number of times to retry backfilling a tablet chunk "
             "during index creation.");
TAG_FLAG(index_backfill_rpc_max_retries, advanced);
TAG_FLAG(index_backfill_rpc_max_retries, runtime);

DEFINE_int32(index_backfill_rpc_max_delay_ms, 10 * 60 * 1000, // 10 min.
             "Maximum delay before retrying a backfill tablet chunk request "
             "during index creation.");
TAG_FLAG(index_backfill_rpc_max_delay_ms, advanced);
TAG_FLAG(index_backfill_rpc_max_delay_ms, runtime);

DEFINE_int32(index_backfill_wait_for_alter_table_completion_ms, 100,
             "Delay before retrying to see if an in-progress alter table has "
             "completed, during index backfill.");
TAG_FLAG(index_backfill_wait_for_alter_table_completion_ms, advanced);
TAG_FLAG(index_backfill_wait_for_alter_table_completion_ms, runtime);

DEFINE_test_flag(int32, slowdown_backfill_alter_table_rpcs_ms, 0,
    "Slows down the send alter table rpc's so that the master may be stopped between "
    "different phases.");

namespace yb {
namespace master {

using namespace std::literals;
using strings::Substitute;
using tserver::TabletServerErrorPB;

namespace {

// Peek into pg_index table to get an index (boolean) status from YSQL perspective.
Result<bool> GetPgIndexStatus(
    CatalogManager* catalog_manager,
    const TableId& idx_id,
    const std::string& status_col_name) {
  const auto pg_index_id =
      GetPgsqlTableId(VERIFY_RESULT(GetPgsqlDatabaseOid(idx_id)), kPgIndexTableOid);

  const tablet::Tablet* catalog_tablet =
      catalog_manager->sys_catalog()->tablet_peer()->tablet();
  const Schema& pg_index_schema =
      VERIFY_RESULT(catalog_tablet->metadata()->GetTableInfo(pg_index_id))->schema;

  Schema projection;
  RETURN_NOT_OK(pg_index_schema.CreateProjectionByNames({"indexrelid", status_col_name},
                                                        &projection,
                                                        pg_index_schema.num_key_columns()));

  const auto indexrelid_col_id = VERIFY_RESULT(projection.ColumnIdByName("indexrelid")).rep();
  const auto status_col_id     = VERIFY_RESULT(projection.ColumnIdByName(status_col_name)).rep();

  const auto idx_oid = VERIFY_RESULT(GetPgsqlTableOid(idx_id));

  auto iter = VERIFY_RESULT(catalog_tablet->NewRowIterator(projection.CopyWithoutColumnIds(),
                                                           boost::none /* transaction_id */,
                                                           {} /* read_hybrid_time */,
                                                           pg_index_id));

  // Filtering by 'indexrelid' == idx_oid.
  {
    auto doc_iter = down_cast<docdb::DocRowwiseIterator*>(iter.get());
    PgsqlConditionPB cond;
    cond.add_operands()->set_column_id(indexrelid_col_id);
    cond.set_op(QL_OP_EQUAL);
    cond.add_operands()->mutable_value()->set_uint32_value(idx_oid);
    docdb::DocPgsqlScanSpec spec(projection,
                                 rocksdb::kDefaultQueryId,
                                 {} /* hashed_components */,
                                 &cond,
                                 boost::none /* hash_code */,
                                 boost::none /* max_hash_code */,
                                 nullptr /* where_expr */);
    RETURN_NOT_OK(doc_iter->Init(spec));
  }

  // Expecting one row at most.
  QLTableRow row;
  if (VERIFY_RESULT(iter->HasNext())) {
    RETURN_NOT_OK(iter->NextRow(&row));
    return row.GetColumn(status_col_id)->bool_value();
  }

  // For practical purposes, an absent index is the same as having false status column value.
  return false;
}

// Before advancing index permissions, we need to make sure Postgres side has advanced sufficiently
// - that the state tracked in pg_index haven't fallen behind from the desired permission
// for more than one step.
Result<bool> ShouldProceedWithPgsqlIndexPermissionUpdate(
    CatalogManager* catalog_manager,
    const TableId& idx_id,
    IndexPermissions new_perm) {
  // TODO(alex, jason): Add the appropriate cases for dropping index path
  switch (new_perm) {
    case INDEX_PERM_WRITE_AND_DELETE: {
      auto live = VERIFY_RESULT(GetPgIndexStatus(catalog_manager, idx_id, "indislive"));
      if (!live) {
        VLOG(1) << "Index " << idx_id << " is not yet live, skipping permission update";
      }
      return live;
    }
    case INDEX_PERM_DO_BACKFILL: {
      auto ready = VERIFY_RESULT(GetPgIndexStatus(catalog_manager, idx_id, "indisready"));
      if (!ready) {
        VLOG(1) << "Index " << idx_id << " is not yet ready, skipping permission update";
      }
      return ready;
    }
    default:
      // No need to wait for anything
      return true;
  }
}

} // namespace

void MultiStageAlterTable::CopySchemaDetailsToFullyApplied(SysTablesEntryPB* pb) {
  VLOG(4) << "Setting fully_applied_schema_version to " << pb->version();
  pb->mutable_fully_applied_schema()->CopyFrom(pb->schema());
  pb->set_fully_applied_schema_version(pb->version());
  pb->mutable_fully_applied_indexes()->CopyFrom(pb->indexes());
  if (pb->has_index_info()) {
    pb->mutable_fully_applied_index_info()->CopyFrom(pb->index_info());
  }
}

Status MultiStageAlterTable::ClearAlteringState(
    CatalogManager* catalog_manager,
    const scoped_refptr<TableInfo>& table,
    uint32_t expected_version) {
  auto l = table->LockForWrite();
  uint32_t current_version = l->data().pb.version();
  if (expected_version != current_version) {
    return STATUS(AlreadyPresent, "Table has already moved to a different version.");
  }
  l->mutable_data()->pb.clear_fully_applied_schema();
  l->mutable_data()->pb.clear_fully_applied_schema_version();
  l->mutable_data()->pb.clear_fully_applied_indexes();
  l->mutable_data()->pb.clear_fully_applied_index_info();
  l->mutable_data()->set_state(
      SysTablesEntryPB::RUNNING, Substitute("Current schema version=$0", current_version));

  Status s =
      catalog_manager->sys_catalog_->UpdateItem(table.get(), catalog_manager->leader_ready_term());
  if (!s.ok()) {
    LOG(WARNING) << "An error occurred while updating sys-tables: " << s.ToString()
                 << ". This master may not be the leader anymore.";
    return s;
  }

  l->Commit();
  LOG(INFO) << table->ToString() << " - Alter table completed version=" << current_version;
  return Status::OK();
}

Result<bool> MultiStageAlterTable::UpdateIndexPermission(
    CatalogManager* catalog_manager,
    const scoped_refptr<TableInfo>& indexed_table,
    const std::unordered_map<TableId, IndexPermissions>& perm_mapping,
    boost::optional<uint32_t> current_version) {
  DVLOG(3) << __PRETTY_FUNCTION__ << " " << yb::ToString(*indexed_table);
  if (FLAGS_TEST_slowdown_backfill_alter_table_rpcs_ms > 0) {
    TRACE("Sleeping for $0 ms", FLAGS_TEST_slowdown_backfill_alter_table_rpcs_ms);
    DVLOG(3) << __PRETTY_FUNCTION__ << " " << yb::ToString(*indexed_table) << " sleeping for "
             << FLAGS_TEST_slowdown_backfill_alter_table_rpcs_ms
             << "ms BEFORE updating the index permission to " << ToString(perm_mapping);
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_slowdown_backfill_alter_table_rpcs_ms));
    DVLOG(3) << __PRETTY_FUNCTION__ << " Done Sleeping";
    TRACE("Done Sleeping");
  }

  bool permissions_updated = false;
  {
    TRACE("Locking indexed table");
    auto l = indexed_table->LockForWrite();
    auto& indexed_table_data = *l->mutable_data();
    auto& indexed_table_pb = indexed_table_data.pb;
    if (current_version && *current_version != indexed_table_pb.version()) {
      LOG(INFO) << "The table schema version "
                << "seems to have already been updated to " << indexed_table_pb.version()
                << " We wanted to do this update at " << *current_version;
      return STATUS_SUBSTITUTE(
          AlreadyPresent, "Schema was already updated to $0 before we got to it (expected $1).",
          indexed_table_pb.version(), *current_version);
    }

    CopySchemaDetailsToFullyApplied(&indexed_table_pb);
    bool is_pgsql = indexed_table_pb.table_type() == TableType::PGSQL_TABLE_TYPE;
    for (int i = 0; i < indexed_table_pb.indexes_size(); i++) {
      IndexInfoPB* idx_pb = indexed_table_pb.mutable_indexes(i);
      auto& idx_table_id = idx_pb->table_id();
      if (perm_mapping.find(idx_table_id) != perm_mapping.end()) {
        const auto new_perm = perm_mapping.at(idx_table_id);
        // TODO(alex, amit): Non-OK status here should be converted to TryAgain,
        //                   which should be handled on an upper level.
        if (is_pgsql && !VERIFY_RESULT(ShouldProceedWithPgsqlIndexPermissionUpdate(catalog_manager,
                                                                                   idx_table_id,
                                                                                   new_perm))) {
          continue;
        }
        idx_pb->set_index_permissions(new_perm);
        permissions_updated = true;
      }
    }

    if (permissions_updated) {
      indexed_table_pb.set_version(indexed_table_pb.version() + 1);
    } else {
      VLOG(1) << "Index permissions update skipped, leaving schema_version at "
              << indexed_table_pb.version();
    }
    indexed_table_data.set_state(SysTablesEntryPB::ALTERING,
                                 Substitute("Alter table version=$0 ts=$1",
                                            indexed_table_pb.version(),
                                            LocalTimeAsString()));

    // Update sys-catalog with the new indexed table info.
    TRACE("Updating indexed table metadata on disk");
    RETURN_NOT_OK(catalog_manager->sys_catalog_->UpdateItem(
        indexed_table.get(), catalog_manager->leader_ready_term()));

    // Update the in-memory state.
    TRACE("Committing in-memory state");
    l->Commit();
  }
  if (PREDICT_FALSE(FLAGS_TEST_slowdown_backfill_alter_table_rpcs_ms > 0)) {
    TRACE("Sleeping for $0 ms",
          FLAGS_TEST_slowdown_backfill_alter_table_rpcs_ms);
    DVLOG(3) << __PRETTY_FUNCTION__ << " " << yb::ToString(*indexed_table) << " sleeping for "
             << FLAGS_TEST_slowdown_backfill_alter_table_rpcs_ms
             << "ms AFTER updating the index permission to " << ToString(perm_mapping);
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_slowdown_backfill_alter_table_rpcs_ms));
    DVLOG(3) << __PRETTY_FUNCTION__ << " Done Sleeping";
    TRACE("Done Sleeping");
  }
  return permissions_updated;
}

Status MultiStageAlterTable::StartBackfillingData(
    CatalogManager *catalog_manager,
    const scoped_refptr<TableInfo> &indexed_table, const IndexInfoPB index_pb) {
  if (indexed_table->IsBackfilling()) {
    LOG(WARNING) << __func__ << " Not starting backfill for "
                 << indexed_table->ToString() << ": one is already in progress";
    return STATUS(AlreadyPresent, "Backfill already in progress");
  }

  VLOG(1) << __func__ << " starting backfill on " << indexed_table->ToString()
          << " for " << index_pb.table_id();
  {
    TRACE("Locking indexed table");
    auto l = indexed_table->LockForWrite();
    auto &indexed_table_data = *l->mutable_data();
    CopySchemaDetailsToFullyApplied(&indexed_table_data.pb);
    if (indexed_table->GetTableType() == TableType::PGSQL_TABLE_TYPE) {
      auto& indexed_table_pb = indexed_table_data.pb;
      indexed_table_data.set_state(SysTablesEntryPB::ALTERING,
                                   Substitute("Alter table version=$0 ts=$1",
                                              indexed_table_pb.version(),
                                              LocalTimeAsString()));
    }
    // Update sys-catalog with the new indexed table info.
    TRACE("Updating indexed table metadata on disk");
    RETURN_NOT_OK_PREPEND(
        catalog_manager->sys_catalog_->UpdateItem(
            indexed_table.get(), catalog_manager->leader_ready_term()),
        "Updating indexed table metadata on disk. Abandoning.");

    // Update the in-memory state.
    TRACE("Committing in-memory state");
    l->Commit();
  }
  indexed_table->SetIsBackfilling(true);

  scoped_refptr<NamespaceInfo> ns_info;
  {
    NamespaceIdentifierPB ns_identifier;
    ns_identifier.set_id(indexed_table->namespace_id());
    RETURN_NOT_OK_PREPEND(
        catalog_manager->FindNamespace(ns_identifier, &ns_info),
        "Unable to get namespace info for backfill");
  }
  auto backfill_table = std::make_shared<BackfillTable>(
      catalog_manager->master_, catalog_manager->AsyncTaskPool(),
      indexed_table, std::vector<IndexInfoPB>{index_pb}, ns_info);
  backfill_table->Launch();
  return Status::OK();
}

// Returns true, if the said IndexPermissions is a transient state.
// Returns false, if it is a state where the index can be. viz: READ_WRITE_AND_DELETE
// INDEX_UNUSED is considered transcient because it needs to delete the index.
bool IsTransientState(IndexPermissions perm) {
  return perm != INDEX_PERM_READ_WRITE_AND_DELETE && perm != INDEX_PERM_NOT_USED;
}

IndexPermissions NextPermission(IndexPermissions perm) {
  switch (perm) {
    case INDEX_PERM_DELETE_ONLY:
      return INDEX_PERM_WRITE_AND_DELETE;
    case INDEX_PERM_WRITE_AND_DELETE:
      return INDEX_PERM_DO_BACKFILL;
    case INDEX_PERM_DO_BACKFILL:
      CHECK(false) << "Not expected to be here.";
      return INDEX_PERM_DELETE_ONLY;
    case INDEX_PERM_READ_WRITE_AND_DELETE:
      CHECK(false) << "Not expected to be here.";
      return INDEX_PERM_DELETE_ONLY;
    case INDEX_PERM_WRITE_AND_DELETE_WHILE_REMOVING:
      return INDEX_PERM_DELETE_ONLY_WHILE_REMOVING;
    case INDEX_PERM_DELETE_ONLY_WHILE_REMOVING:
      return INDEX_PERM_INDEX_UNUSED;
    case INDEX_PERM_INDEX_UNUSED:
    case INDEX_PERM_NOT_USED:
      CHECK(false) << "Not expected to be here.";
      return INDEX_PERM_DELETE_ONLY;
  }
  CHECK(false) << "Not expected to be here.";
  return INDEX_PERM_DELETE_ONLY;
}

Status MultiStageAlterTable::LaunchNextTableInfoVersionIfNecessary(
    CatalogManager* catalog_manager, const scoped_refptr<TableInfo>& indexed_table,
    uint32_t current_version) {
  DVLOG(3) << __PRETTY_FUNCTION__ << " " << yb::ToString(*indexed_table);

  const bool is_ysql_table = (indexed_table->GetTableType() == TableType::PGSQL_TABLE_TYPE);

  std::unordered_map<TableId, IndexPermissions> indexes_to_update;
  vector<IndexInfoPB> indexes_to_backfill;
  vector<IndexInfoPB> indexes_to_delete;
  {
    TRACE("Locking indexed table");
    VLOG(1) << ("Locking indexed table");
    auto l = indexed_table->LockForRead();
    if (current_version != l->data().pb.version()) {
      LOG(WARNING) << "Somebody launched the next version before we got to it.";
      return Status::OK();
    }

    // Attempt to find an index that requires us to just launch the next state (i.e. not backfill)
    for (int i = 0; i < l->data().pb.indexes_size(); i++) {
      const IndexInfoPB& idx_pb = l->data().pb.indexes(i);
      if (!idx_pb.has_index_permissions()) {
        continue;
      }
      if (idx_pb.index_permissions() == INDEX_PERM_DO_BACKFILL) {
        indexes_to_backfill.emplace_back(idx_pb);
      } else if (idx_pb.index_permissions() == INDEX_PERM_INDEX_UNUSED) {
        indexes_to_delete.emplace_back(idx_pb);
      // For YSQL, there should never be indexes to update from master side because postgres drives
      // permission changes.
      } else if (idx_pb.index_permissions() != INDEX_PERM_READ_WRITE_AND_DELETE && !is_ysql_table) {
        indexes_to_update.emplace(idx_pb.table_id(), NextPermission(idx_pb.index_permissions()));
      }
    }
  }

  if (indexes_to_update.empty() &&
      indexes_to_delete.empty() &&
      indexes_to_backfill.empty()) {
    TRACE("Not necessary to launch next version");
    VLOG(1) << "Not necessary to launch next version";
    return ClearAlteringState(catalog_manager, indexed_table, current_version);
  }

  // For YSQL online schema migration of indexes, instead of master driving the schema changes,
  // postgres will drive it.  Postgres will use three of the DocDB index permissions:
  //
  // - INDEX_PERM_WRITE_AND_DELETE (set from the start)
  // - INDEX_PERM_READ_WRITE_AND_DELETE (set by master)
  // - INDEX_PERM_WRITE_AND_DELETE_WHILE_REMOVING (set by master)
  //
  // This changes how we treat indexes_to_foo:
  //
  // - indexes_to_update should always be empty because we never want master to set index
  //   permissions.
  // - indexes_to_delete is impossible to be nonempty, and, in the future, when we do use
  //   INDEX_PERM_INDEX_UNUSED, we want to use some other delete trigger that makes sure no
  //   transactions are left using the index.  Prepare for that by doing nothing when nonempty.
  // - indexes_to_backfill is impossible to be nonempty, but, in the future, we want to set
  //   INDEX_PERM_DO_BACKFILL so that backfill resumes on master leader changes.  Prepare for that
  //   by handling indexes_to_backfill like for YCQL.
  //
  // TODO(jason): when using INDEX_PERM_DO_BACKFILL, update this comment (issue #6218).

  if (!indexes_to_update.empty()) {
    Result<bool> permissions_updated =
        VERIFY_RESULT(UpdateIndexPermission(catalog_manager, indexed_table, indexes_to_update,
                                            current_version));

    if (!permissions_updated.ok()) {
      LOG(WARNING) << "Could not update index permissions."
                   << " Possible that the master-leader has changed, or a race "
                   << "with another thread trying to launch next version: "
                   << permissions_updated.ToString();
    }

    if (permissions_updated.ok() && *permissions_updated) {
      catalog_manager->SendAlterTableRequest(indexed_table);
      return Status::OK();
    }
  }

  IndexInfoPB index_info_to_update;
  if (!indexes_to_delete.empty()) {
    index_info_to_update = indexes_to_delete[0];
    VLOG(3) << "Deleting the index and the entry in the indexed table for "
            << yb::ToString(index_info_to_update);
    DeleteTableRequestPB req;
    DeleteTableResponsePB resp;
    req.mutable_table()->set_table_id(index_info_to_update.table_id());
    req.set_is_index_table(true);
    RETURN_NOT_OK(catalog_manager->DeleteTableInternal(&req, &resp, nullptr));
    return Status::OK();
  }

  if (!indexes_to_backfill.empty()) {
    // TODO(Amit): Batch backfill for different indexes.
    index_info_to_update = indexes_to_backfill[0];
    VLOG(3) << "Start backfilling for " << yb::ToString(index_info_to_update);
    TRACE("Starting backfill process");
    VLOG(1) << "Starting backfill process";
    WARN_NOT_OK(
        StartBackfillingData(catalog_manager, indexed_table.get(), index_info_to_update),
        "Could not launch Backfill");
    return Status::OK();
  }

  return Status::OK();
}

// -----------------------------------------------------------------------------------------------
// BackfillTableJob
// -----------------------------------------------------------------------------------------------
std::string BackfillTableJob::description() const {
  const std::shared_ptr<BackfillTable> retain_bt = backfill_table_;
  auto curr_state = state();
  if (!IsStateTerminal(curr_state) && retain_bt) {
    return retain_bt->description();
  } else if (curr_state == MonitoredTaskState::kFailed) {
    return Format("Backfilling $0 Failed", index_ids_);
  } else if (curr_state == MonitoredTaskState::kAborted) {
    return Format("Backfilling $0 Aborted", index_ids_);
  } else {
    DCHECK(curr_state == MonitoredTaskState::kComplete);
    return Format("Backfilling $0 Done", index_ids_);
  }
}

MonitoredTaskState BackfillTableJob::AbortAndReturnPrevState(const Status& status) {
  auto old_state = state();
  while (!IsStateTerminal(old_state)) {
    if (state_.compare_exchange_strong(old_state,
                                       MonitoredTaskState::kAborted)) {
      return old_state;
    }
    old_state = state();
  }
  return old_state;
}

void BackfillTableJob::SetState(MonitoredTaskState new_state) {
  auto old_state = state();
  if (!IsStateTerminal(old_state)) {
    if (state_.compare_exchange_strong(old_state, new_state) && IsStateTerminal(new_state)) {
      MarkDone();
    }
  }
}
// -----------------------------------------------------------------------------------------------
// BackfillTable
// -----------------------------------------------------------------------------------------------
BackfillTable::BackfillTable(Master *master, ThreadPool *callback_pool,
                             const scoped_refptr<TableInfo> &indexed_table,
                             std::vector<IndexInfoPB> indexes,
                             const scoped_refptr<NamespaceInfo> &ns_info)
    : master_(master), callback_pool_(callback_pool),
      indexed_table_(indexed_table), indexes_to_build_(indexes), ns_info_(ns_info) {
  LOG_IF(DFATAL, indexes_to_build_.size() != 1)
      << "As of Dec 2019, we only support "
      << "building one index at a time. indexes_to_build_.size() = "
      << indexes_to_build_.size();

  std::ostringstream out;
  out << "{ ";
  bool first = true;
  for (const auto &index_info : indexes_to_build_) {
    if (!first) {
      out << ", ";
    }
    out << master_->catalog_manager()->GetTableInfo(index_info.table_id())->name();
    first = false;
  }
  out << " }";
  index_ids_ = out.str();

  auto l = indexed_table_->LockForRead();
  schema_version_ = indexed_table_->metadata().state().pb.version();
  leader_term_ = master_->catalog_manager()->leader_ready_term();
  const auto &properties =
      indexed_table_->metadata().state().pb.schema().table_properties();
  if (properties.has_backfilling_timestamp() &&
      read_time_for_backfill_.FromUint64(properties.backfilling_timestamp()).ok()) {
    timestamp_chosen_.store(true, std::memory_order_release);
    VLOG_WITH_PREFIX(1) << "Will be using " << read_time_for_backfill_
                        << " for backfill";
  } else {
    read_time_for_backfill_ = HybridTime::kInvalid;
    timestamp_chosen_.store(false, std::memory_order_release);
  }
  done_.store(false, std::memory_order_release);
}

void BackfillTable::Launch() {
  backfill_job_ = std::make_shared<BackfillTableJob>(shared_from_this());
  backfill_job_->SetState(MonitoredTaskState::kRunning);
  master_->catalog_manager()->jobs_tracker_->AddTask(backfill_job_);
  if (!timestamp_chosen_.load(std::memory_order_acquire)) {
    LaunchComputeSafeTimeForRead();
  } else {
    LaunchBackfill();
  }
}

void BackfillTable::LaunchComputeSafeTimeForRead() {
  vector<scoped_refptr<TabletInfo>> tablets;
  indexed_table_->GetAllTablets(&tablets);

  num_tablets_.store(tablets.size(), std::memory_order_release);
  tablets_pending_.store(tablets.size(), std::memory_order_release);
  auto min_cutoff = master()->clock()->Now();
  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    auto get_safetime = std::make_shared<GetSafeTimeForTablet>(
        shared_from_this(), tablet, min_cutoff);
    get_safetime->Launch();
  }
}

std::string BackfillTable::LogPrefix() const {
  return Format("Backfill Index Table(s) $0 ", index_ids_);
}

std::string BackfillTable::description() const {
  auto num_pending = tablets_pending_.load(std::memory_order_acquire);
  auto num_tablets = num_tablets_.load(std::memory_order_acquire);
  return Format(
      "Backfill Index Table(s) $0 : $1", index_ids_,
      (timestamp_chosen()
           ? (done() ? Format("Backfill $0/$1 tablets done", num_pending, num_tablets)
                     : Format("Backfilling $0/$1 tablets", num_pending, num_tablets))
           : Format("Waiting to GetSafeTime from $0/$1 tablets", num_pending, num_tablets)));
}

const std::string BackfillTable::GetNamespaceName() const {
  return ns_info_->name();
}

Status BackfillTable::UpdateSafeTime(const Status& s, HybridTime ht) {
  if (!s.ok()) {
    // Move on to ABORTED permission.
    LOG_WITH_PREFIX(ERROR)
        << "Failed backfill. Could not compute safe time for "
        << yb::ToString(indexed_table_) << " " << s;
    if (!timestamp_chosen_.exchange(true)) {
      RETURN_NOT_OK_PREPEND(AlterTableStateToAbort(),
                            "Failed to mark backfill as failed. Abandoning.");
    }
    return Status::OK();
  }

  // Need to guard this.
  HybridTime read_timestamp;
  {
    std::lock_guard<simple_spinlock> l(mutex_);
    VLOG(2) << "Updating read_time_for_backfill_ to max{ "
            << read_time_for_backfill_.ToString() << ", " << ht.ToString()
            << " }.";
    read_time_for_backfill_.MakeAtLeast(ht);
    read_timestamp = read_time_for_backfill_;
  }

  // If OK then move on to doing backfill.
  if (!timestamp_chosen() && --tablets_pending_ == 0) {
    LOG_WITH_PREFIX(INFO) << "Completed fetching SafeTime for the table "
                          << yb::ToString(indexed_table_) << " will be using "
                          << read_timestamp.ToString();
    {
      auto l = indexed_table_->LockForWrite();
      l->mutable_data()
          ->pb.mutable_schema()
          ->mutable_table_properties()
          ->set_backfilling_timestamp(read_timestamp.ToUint64());
      RETURN_NOT_OK_PREPEND(
          master_->catalog_manager()->sys_catalog_->UpdateItem(
              indexed_table_.get(), leader_term()),
          "Failed to persist backfilling timestamp. Abandoning.");
      l->Commit();
    }
    VLOG_WITH_PREFIX(2) << "Saved " << read_timestamp
                        << " as backfilling_timestamp";
    timestamp_chosen_.store(true, std::memory_order_release);
    LaunchBackfill();
  }
  return Status::OK();
}

void BackfillTable::LaunchBackfill() {
  VLOG_WITH_PREFIX(1) << "launching backfill with timestamp: "
                      << read_time_for_backfill_;
  vector<scoped_refptr<TabletInfo>> tablets;
  indexed_table_->GetAllTablets(&tablets);

  num_tablets_.store(tablets.size(), std::memory_order_release);
  tablets_pending_.store(tablets.size(), std::memory_order_release);
  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    auto backfill_tablet = std::make_shared<BackfillTablet>(shared_from_this(), tablet);
    backfill_tablet->Launch();
  }
}

void BackfillTable::Done(const Status& s) {
  if (!s.ok()) {
    // Move on to ABORTED permission.
    LOG_WITH_PREFIX(ERROR) << "failed to backfill the index: " << s;
    if (!done_.exchange(true)) {
      // Set error message.
      {
        auto l = indexed_table_->LockForWrite();
        auto& indexed_table_data = *l->mutable_data();
        auto& indexed_table_pb = indexed_table_data.pb;
        for (int i = 0; i < indexed_table_pb.indexes_size(); i++) {
          IndexInfoPB* idx_pb = indexed_table_pb.mutable_indexes(i);
          // TODO(jason): fix this when we start batching indexes (issue #4785).
          if (idx_pb->table_id() == indexes()[0].table_id()) {
            idx_pb->set_backfill_error_message(s.message().ToBuffer());
            break;
          }
        }
        WARN_NOT_OK(master_->catalog_manager()->sys_catalog_->UpdateItem(
                        indexed_table_.get(), leader_term()),
                    "Could not add error message.");
        l->Commit();
      }
      // Now start aborting.
      WARN_NOT_OK(AlterTableStateToAbort(),
                  "Failed to mark backfill as failed.");
    } else {
      LOG_WITH_PREFIX(INFO)
          << "Somebody else already aborted the index backfill.";
    }
    return;
  }

  // If OK then move on to READ permissions.
  if (!done() && --tablets_pending_ == 0) {
    LOG_WITH_PREFIX(INFO) << "Completed backfilling the index table.";
    done_.store(true, std::memory_order_release);
    WARN_NOT_OK(AlterTableStateToSuccess(), "Failed to complete backfill.");
  }
}

Status BackfillTable::AlterTableStateToSuccess() {
  const TableId& index_table_id = indexes()[0].table_id();
  RETURN_NOT_OK_PREPEND(
      MultiStageAlterTable::UpdateIndexPermission(
          master_->catalog_manager(),
          indexed_table_,
          {{index_table_id, INDEX_PERM_READ_WRITE_AND_DELETE}},
          boost::none),
      "Could not update permission to "
      "INDEX_PERM_READ_WRITE_AND_DELETE. Possible that the "
      "master-leader has changed.");

  VLOG(1) << "Sending alter table requests to the Indexed table";
  master_->catalog_manager()->SendAlterTableRequest(indexed_table_);
  VLOG(1) << "DONE Sending alter table requests to the Indexed table";
  RETURN_NOT_OK(AllowCompactionsToGCDeleteMarkers(index_table_id));

  VLOG(1) << __func__ << " done backfill on " << indexed_table_->ToString()
          << " for " << index_table_id;
  indexed_table_->SetIsBackfilling(false);
  backfill_job_->SetState(MonitoredTaskState::kComplete);
  return ClearCheckpointStateInTablets();
}

Status BackfillTable::AlterTableStateToAbort() {
  const TableId& index_table_id = indexes()[0].table_id();
  RETURN_NOT_OK_PREPEND(
      MultiStageAlterTable::UpdateIndexPermission(
          master_->catalog_manager(), indexed_table_,
          {{index_table_id, INDEX_PERM_WRITE_AND_DELETE_WHILE_REMOVING}}, boost::none),
      "Could not update permission to "
      "INDEX_PERM_WRITE_AND_DELETE_WHILE_REMOVING. Possible that the "
      "master-leader has changed.");
  master_->catalog_manager()->SendAlterTableRequest(indexed_table_);
  indexed_table_->SetIsBackfilling(false);
  backfill_job_->SetState(MonitoredTaskState::kFailed);
  return ClearCheckpointStateInTablets();
}

Status BackfillTable::ClearCheckpointStateInTablets() {
  vector<scoped_refptr<TabletInfo>> tablets;
  indexed_table_->GetAllTablets(&tablets);
  std::vector<TabletInfo*> tablet_ptrs;
  const auto& idx_id = indexes()[0].table_id();
  for (scoped_refptr<TabletInfo>& tablet : tablets) {
    tablet_ptrs.push_back(tablet.get());
    tablet->mutable_metadata()->StartMutation();
    tablet->mutable_metadata()->mutable_dirty()->pb.mutable_backfilled_until()->erase(idx_id);
  }
  RETURN_NOT_OK_PREPEND(
      master()->catalog_manager()->sys_catalog()->UpdateItems(tablet_ptrs,
                                                              leader_term()),
      "Could not persist that the table is done backfilling.");
  for (scoped_refptr<TabletInfo>& tablet : tablets) {
    VLOG(2) << "Done backfilling the table. " << yb::ToString(tablet)
            << " clearing backfilled_until";
    tablet->mutable_metadata()->CommitMutation();
  }

  {
    auto l = indexed_table_->LockForWrite();
    l->mutable_data()
        ->pb.mutable_schema()
        ->mutable_table_properties()
        ->clear_backfilling_timestamp();
    RETURN_NOT_OK_PREPEND(master_->catalog_manager()->sys_catalog_->UpdateItem(
                              indexed_table_.get(), leader_term()),
                          "Could not clear backfilling timestamp.");
    l->Commit();
  }
  VLOG_WITH_PREFIX(2) << "Cleared backfilling timestamp.";
  return Status::OK();
}

Status BackfillTable::AllowCompactionsToGCDeleteMarkers(
    const TableId &index_table_id) {
  DVLOG(3) << __PRETTY_FUNCTION__;
  scoped_refptr<TableInfo> index_table_info;
  TableIdentifierPB index_table_id_pb;
  index_table_id_pb.set_table_id(index_table_id);
  RETURN_NOT_OK_PREPEND(
      master_->catalog_manager()->FindTable(index_table_id_pb,
                                            &index_table_info),
      yb::Format("Could not find table info for the index table $0 to enable "
                 "compactions. "
                 "This is ok in case somebody issued a delete index.",
                 yb::ToString(index_table_id)));

  // Add a sleep here to wait until the Table is fully created.
  bool is_ready = false;
  bool first_run = true;
  do {
    if (!first_run) {
      YB_LOG_EVERY_N_SECS(INFO, 1) << "Waiting for the previous alter table to "
                                      "complete on the index table "
                                   << yb::ToString(index_table_id);
      SleepFor(
          MonoDelta::FromMilliseconds(FLAGS_index_backfill_wait_for_alter_table_completion_ms));
    }
    first_run = false;
    {
      VLOG(2) << __func__ << ": Trying to lock index table for Read";
      auto l = index_table_info->LockForRead();
      is_ready = (l->data().pb.state() == SysTablesEntryPB::RUNNING);
    }
    VLOG(2) << __func__ << ": Unlocked index table for Read";
  } while (!is_ready);
  {
    TRACE("Locking index table");
    VLOG(2) << __func__ << ": Trying to lock index table for Write";
    auto l = index_table_info->LockForWrite();
    VLOG(2) << __func__ << ": locked index table for Write";
    l->mutable_data()->pb.mutable_schema()->mutable_table_properties()
        ->set_retain_delete_markers(false);

    // Update sys-catalog with the new indexed table info.
    TRACE("Updating index table metadata on disk");
    RETURN_NOT_OK_PREPEND(
        master_->catalog_manager()->sys_catalog_->UpdateItem(
            index_table_info.get(), leader_term()),
        yb::Format(
            "Could not update index_table_info for $0 to enable compactions.",
            index_table_id));

    // Update the in-memory state.
    TRACE("Committing in-memory state");
    l->Commit();
  }
  VLOG(2) << __func__ << ": Unlocked index table for Read";
  VLOG(1) << "Sending backfill done requests to the Index table";
  RETURN_NOT_OK(SendRpcToAllowCompactionsToGCDeleteMarkers(index_table_info));
  VLOG(1) << "DONE Sending backfill done requests to the Index table";
  return Status::OK();
}

Status BackfillTable::SendRpcToAllowCompactionsToGCDeleteMarkers(
    const scoped_refptr<TableInfo> &table) {
  vector<scoped_refptr<TabletInfo>> tablets;
  table->GetAllTablets(&tablets);

  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    RETURN_NOT_OK(SendRpcToAllowCompactionsToGCDeleteMarkers(tablet));
  }
  return Status::OK();
}

Status BackfillTable::SendRpcToAllowCompactionsToGCDeleteMarkers(
    const scoped_refptr<TabletInfo> &tablet) {
  auto call = std::make_shared<AsyncBackfillDone>(master_, callback_pool_, tablet);
  tablet->table()->AddTask(call);
  RETURN_NOT_OK_PREPEND(
      master_->catalog_manager()->ScheduleTask(call),
      "Failed to send backfill done request");
  return Status::OK();
}

// -----------------------------------------------------------------------------------------------
// BackfillTablet
// -----------------------------------------------------------------------------------------------
BackfillTablet::BackfillTablet(
    std::shared_ptr<BackfillTable> backfill_table, const scoped_refptr<TabletInfo>& tablet)
    : backfill_table_(backfill_table), tablet_(tablet) {
  {
    auto l = tablet_->LockForRead();
    const auto& pb = tablet_->metadata().state().pb;
    Partition::FromPB(pb.partition(), &partition_);
    DCHECK_EQ(backfill_table_->indexes().size(), 1);
    const auto& idx_id = backfill_table_->indexes()[0].table_id();
    if (pb.backfilled_until().find(idx_id) != pb.backfilled_until().end()) {
      next_row_to_backfill_ = pb.backfilled_until().at(idx_id);
      done_.store(next_row_to_backfill_.empty(), std::memory_order_release);
    }
  }
  if (!next_row_to_backfill_.empty()) {
    VLOG(1) << tablet_->ToString() << " resuming backfill from "
            << yb::ToString(next_row_to_backfill_);
  } else if (done()) {
    VLOG(1) << tablet_->ToString() << " backfill already done";
  } else {
    VLOG(1) << tablet_->ToString() << " beginning backfill from "
            << "<start-of-the-tablet>";
  }
}

void BackfillTablet::LaunchNextChunkOrDone() {
  if (done()) {
    backfill_table_->Done(Status::OK());
  } else {
    auto chunk = std::make_shared<BackfillChunk>(shared_from_this(),
                                                 next_row_to_backfill_);
    chunk->Launch();
  }
}

void BackfillTablet::Done(const Status& status, const string& next_row_key) {
  if (!status.ok()) {
    LOG(INFO) << "Failed to backfill the tablet " << yb::ToString(tablet_) << ": " << status;
    backfill_table_->Done(status);
    return;
  }

  next_row_to_backfill_ = next_row_key;
  VLOG(2) << "Done backfilling the tablet " << yb::ToString(tablet_)
          << " until " << yb::ToString(next_row_to_backfill_);
  {
    tablet_->mutable_metadata()->StartMutation();
    for (const auto& idx_info : backfill_table_->indexes()) {
      tablet_->mutable_metadata()->mutable_dirty()->pb.mutable_backfilled_until()->insert(
          {idx_info.table_id(), next_row_to_backfill_});
    }
    WARN_NOT_OK(
        backfill_table_->master()->catalog_manager()->sys_catalog()->UpdateItem(
            tablet_.get(), backfill_table_->leader_term()),
        "Could not persist that the tablet is done backfilling.");
    tablet_->mutable_metadata()->CommitMutation();
  }

  // This is the last chunk.
  if (next_row_to_backfill_.empty()) {
    LOG(INFO) << "Done backfilling the tablet " << yb::ToString(tablet_);
    done_.store(true, std::memory_order_release);
  }

  LaunchNextChunkOrDone();
}

// -----------------------------------------------------------------------------------------------
// GetSafeTimeForTablet
// -----------------------------------------------------------------------------------------------

void GetSafeTimeForTablet::Launch() {
  tablet_->table()->AddTask(shared_from_this());
  Status status = Run();
  // Need to print this after Run() because that's where it picks the TS which description()
  // needs.
  if (status.ok()) {
    VLOG(3) << "Started GetSafeTimeForTablet : " << this->description();
  } else {
    LOG(WARNING) << Substitute("Failed to send GetSafeTime request for $0. ",
                               tablet_->ToString())
                 << status;
  }
}

bool GetSafeTimeForTablet::SendRequest(int attempt) {
  VLOG(1) << __PRETTY_FUNCTION__;
  tserver::GetSafeTimeRequestPB req;
  req.set_dest_uuid(permanent_uuid());
  req.set_tablet_id(tablet_->tablet_id());
  auto now = backfill_table_->master()->clock()->Now().ToUint64();
  req.set_min_hybrid_time_for_backfill(min_cutoff_.ToUint64());
  req.set_propagated_hybrid_time(now);

  ts_admin_proxy_->GetSafeTimeAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG(1) << "Send " << description() << " to " << permanent_uuid()
          << " (attempt " << attempt << "):\n"
          << req.DebugString();
  return true;
}

void GetSafeTimeForTablet::HandleResponse(int attempt) {
  VLOG(1) << __PRETTY_FUNCTION__;
  Status status = Status::OK();
  if (resp_.has_error()) {
    status = StatusFromPB(resp_.error().status());

    // Do not retry on a fatal error
    switch (resp_.error().code()) {
      case TabletServerErrorPB::TABLET_NOT_FOUND:
      case TabletServerErrorPB::MISMATCHED_SCHEMA:
      case TabletServerErrorPB::TABLET_HAS_A_NEWER_SCHEMA:
      case TabletServerErrorPB::OPERATION_NOT_SUPPORTED:
        LOG(WARNING) << "TS " << permanent_uuid() << ": GetSafeTime failed for tablet "
                     << tablet_->ToString() << " no further retry: " << status;
        TransitionToFailedState(MonitoredTaskState::kRunning, status);
        break;
      default:
        LOG(WARNING) << "TS " << permanent_uuid() << ": GetSafeTime failed for tablet "
                     << tablet_->ToString() << ": " << status << " code " << resp_.error().code();
        break;
    }
  } else {
    TransitionToCompleteState();
    VLOG(1) << "TS " << permanent_uuid() << ": GetSafeTime complete on tablet "
            << tablet_->ToString();
  }

  server::UpdateClock(resp_, master_->clock());
}

void GetSafeTimeForTablet::UnregisterAsyncTaskCallback() {
  Status status;
  HybridTime safe_time;
  if (resp_.has_error()) {
    status = StatusFromPB(resp_.error().status());
    VLOG(3) << "GetSafeTime for " << tablet_->ToString() << " got an error. Returning "
            << safe_time;
  } else if (state() != MonitoredTaskState::kComplete) {
    status = STATUS_SUBSTITUTE(InternalError, "$0 in state $1", description(),
                               ToString(state()));
  } else {
    safe_time = HybridTime(resp_.safe_time());
    if (safe_time.is_special()) {
      LOG(ERROR) << "GetSafeTime for " << tablet_->ToString() << " got " << safe_time;
    } else {
      VLOG(3) << "GetSafeTime for " << tablet_->ToString() << " got " << safe_time;
    }
  }
  WARN_NOT_OK(backfill_table_->UpdateSafeTime(status, safe_time),
    "Could not UpdateSafeTime");
}

// -----------------------------------------------------------------------------------------------
// BackfillChunk
// -----------------------------------------------------------------------------------------------
void BackfillChunk::Launch() {
  backfill_tablet_->tablet()->table()->AddTask(shared_from_this());
  Status status = Run();
  WARN_NOT_OK(
      status, Substitute(
                  "Failed to send backfill Chunk request for $0",
                  backfill_tablet_->tablet().get()->ToString()));

  // Need to print this after Run() because that's where it picks the TS which description()
  // needs.
  if (status.ok()) {
    LOG(INFO) << "Started BackfillChunk : " << this->description();
  }
}

MonoTime BackfillChunk::ComputeDeadline() {
  MonoTime timeout = MonoTime::Now();
  if (GetTableType() == TableType::PGSQL_TABLE_TYPE) {
    timeout.AddDelta(MonoDelta::FromMilliseconds(FLAGS_ysql_index_backfill_rpc_timeout_ms));
  } else {
    DCHECK(GetTableType() == TableType::YQL_TABLE_TYPE);
    timeout.AddDelta(MonoDelta::FromMilliseconds(FLAGS_index_backfill_rpc_timeout_ms));
  }
  return MonoTime::Earliest(timeout, deadline_);
}

int BackfillChunk::num_max_retries() {
  return FLAGS_index_backfill_rpc_max_retries;
}

int BackfillChunk::max_delay_ms() {
  return FLAGS_index_backfill_rpc_max_delay_ms;
}

bool BackfillChunk::SendRequest(int attempt) {
  VLOG(1) << __PRETTY_FUNCTION__;
  tserver::BackfillIndexRequestPB req;
  req.set_dest_uuid(permanent_uuid());
  req.set_tablet_id(backfill_tablet_->tablet()->tablet_id());
  req.set_read_at_hybrid_time(backfill_tablet_->read_time_for_backfill().ToUint64());
  req.set_schema_version(backfill_tablet_->schema_version());
  req.set_start_key(start_key_);
  if (GetTableType() == TableType::PGSQL_TABLE_TYPE) {
    req.set_namespace_name(backfill_tablet_->GetNamespaceName());
  }
  for (const IndexInfoPB& idx_info : backfill_tablet_->indexes()) {
    req.add_indexes()->CopyFrom(idx_info);
  }
  req.set_propagated_hybrid_time(backfill_tablet_->master()->clock()->Now().ToUint64());

  ts_admin_proxy_->BackfillIndexAsync(req, &resp_, &rpc_, BindRpcCallback());
  VLOG(1) << "Send " << description() << " to " << permanent_uuid()
          << " (attempt " << attempt << "):\n"
          << req.DebugString();
  return true;
}

void BackfillChunk::HandleResponse(int attempt) {
  VLOG(1) << __PRETTY_FUNCTION__;
  Status status;
  if (resp_.has_error()) {
    status = StatusFromPB(resp_.error().status());

    // Do not retry on a fatal error
    switch (resp_.error().code()) {
      case TabletServerErrorPB::MISMATCHED_SCHEMA:
      case TabletServerErrorPB::OPERATION_NOT_SUPPORTED:
      case TabletServerErrorPB::TABLET_HAS_A_NEWER_SCHEMA:
      case TabletServerErrorPB::TABLET_NOT_FOUND:
        LOG(WARNING) << "TS " << permanent_uuid() << ": backfill failed for tablet "
                     << backfill_tablet_->tablet()->ToString()
                     << " no further retry: " << status;
        TransitionToFailedState(MonitoredTaskState::kRunning, status);
        break;
      default:
        LOG(WARNING) << "TS " << permanent_uuid() << ": backfill failed for tablet "
                     << backfill_tablet_->tablet()->ToString() << ": " << status.ToString()
                     << " code " << resp_.error().code();
        break;
    }
  } else {
    TransitionToCompleteState();
    VLOG(1) << "TS " << permanent_uuid() << ": backfill complete on tablet "
            << backfill_tablet_->tablet()->ToString();
  }

  server::UpdateClock(resp_, master_->clock());
}

void BackfillChunk::UnregisterAsyncTaskCallback() {
  Status status;
  if (resp_.has_error()) {
    status = StatusFromPB(resp_.error().status());
  } else if (state() != MonitoredTaskState::kComplete) {
    status = STATUS_SUBSTITUTE(InternalError, "$0 in state $1", description(),
                               ToString(state()));
  }
  backfill_tablet_->Done(status, resp_.backfilled_until());
}

}  // namespace master
}  // namespace yb
