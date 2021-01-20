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
// ================================================================================================
//
// The catalog manager handles the current list of tables
// and tablets in the cluster, as well as their current locations.
// Since most operations in the master go through these data
// structures, locking is carefully managed here to prevent unnecessary
// contention and deadlocks:
//
// - each structure has an internal spinlock used for operations that
//   are purely in-memory (eg the current status of replicas)
// - data that is persisted on disk is stored in separate PersistentTable(t)Info
//   structs. These are managed using copy-on-write so that writers may block
//   writing them back to disk while not impacting concurrent readers.
//
// Usage rules:
// - You may obtain READ locks in any order. READ locks should never block,
//   since they only conflict with COMMIT which is a purely in-memory operation.
//   Thus they are deadlock-free.
// - If you need a WRITE lock on both a table and one or more of its tablets,
//   acquire the lock on the table first. This strict ordering prevents deadlocks.
//
// ================================================================================================

#include "yb/master/catalog_manager.h"
#include "yb/master/catalog_manager-internal.h"

#include <stdlib.h>

#include <algorithm>
#include <bitset>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include <boost/optional.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <glog/logging.h>
#include <google/protobuf/text_format.h>
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
#include "yb/master/backfill_index.h"
#include "yb/master/catalog_loaders.h"
#include "yb/master/catalog_manager_bg_tasks.h"
#include "yb/master/catalog_manager_util.h"
#include "yb/master/cluster_balance.h"
#include "yb/master/encryption_manager.h"
#include "yb/master/master.h"
#include "yb/master/master.pb.h"
#include "yb/master/master.proxy.h"
#include "yb/master/master_error.h"
#include "yb/master/master_util.h"
#include "yb/master/sys_catalog_constants.h"
#include "yb/master/sys_catalog_initialization.h"
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

#include "yb/tserver/ts_tablet_manager.h"

#include "yb/tablet/operations/change_metadata_operation.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_retention_policy.h"

#include "yb/tserver/tserver_admin.proxy.h"

#include "yb/util/crypt.h"
#include "yb/util/debug-util.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/math_util.h"
#include "yb/util/monotime.h"
#include "yb/util/random_util.h"
#include "yb/util/rw_mutex.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/stopwatch.h"
#include "yb/util/thread.h"
#include "yb/util/thread_restrictions.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"
#include "yb/util/tsan_util.h"
#include "yb/util/uuid.h"

#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/meta_cache.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_table_name.h"

#include "yb/tserver/remote_bootstrap_client.h"
#include "yb/tserver/remote_bootstrap_snapshots.h"

#include "yb/yql/redis/redisserver/redis_constants.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"
#include "yb/util/shared_lock.h"

using namespace std::literals;

DEFINE_int32(master_ts_rpc_timeout_ms, 30 * 1000,  // 30 sec
             "Timeout used for the Master->TS async rpc calls.");
TAG_FLAG(master_ts_rpc_timeout_ms, advanced);

DEFINE_int32(tablet_creation_timeout_ms, 30 * 1000,  // 30 sec
             "Timeout used by the master when attempting to create tablet "
             "replicas during table creation.");
TAG_FLAG(tablet_creation_timeout_ms, advanced);

DEFINE_bool(catalog_manager_wait_for_new_tablets_to_elect_leader, true,
            "Whether the catalog manager should wait for a newly created tablet to "
            "elect a leader before considering it successfully created. "
            "This is disabled in some tests where we explicitly manage leader "
            "election.");
TAG_FLAG(catalog_manager_wait_for_new_tablets_to_elect_leader, hidden);

DEFINE_int32(catalog_manager_inject_latency_in_delete_table_ms, 0,
             "Number of milliseconds that the master will sleep in DeleteTable.");
TAG_FLAG(catalog_manager_inject_latency_in_delete_table_ms, hidden);

DECLARE_int32(catalog_manager_bg_task_wait_ms);

DEFINE_int32(replication_factor, 3,
             "Default number of replicas for tables that do not have the num_replicas set.");
TAG_FLAG(replication_factor, advanced);

DEFINE_int32(max_create_tablets_per_ts, 50,
             "The number of tablets per TS that can be requested for a new table.");
TAG_FLAG(max_create_tablets_per_ts, advanced);

DEFINE_int32(catalog_manager_report_batch_size, 1,
            "The max number of tablets evaluated in the heartbeat as a single SysCatalog update.");
TAG_FLAG(catalog_manager_report_batch_size, advanced);

DEFINE_int32(master_failover_catchup_timeout_ms, 30 * 1000 * yb::kTimeMultiplier,  // 30 sec
             "Amount of time to give a newly-elected leader master to load"
             " the previous master's metadata and become active. If this time"
             " is exceeded, the node crashes.");
TAG_FLAG(master_failover_catchup_timeout_ms, advanced);
TAG_FLAG(master_failover_catchup_timeout_ms, experimental);

DEFINE_bool(master_tombstone_evicted_tablet_replicas, true,
            "Whether the Master should tombstone (delete) tablet replicas that "
            "are no longer part of the latest reported raft config.");
TAG_FLAG(master_tombstone_evicted_tablet_replicas, hidden);
DECLARE_bool(master_ignore_deleted_on_load);

// Temporary.  Can be removed after long-run testing.
DEFINE_bool(master_ignore_stale_cstate, true,
            "Whether Master processes the raft config when the version is lower.");
TAG_FLAG(master_ignore_stale_cstate, hidden);

DEFINE_bool(catalog_manager_check_ts_count_for_create_table, true,
            "Whether the master should ensure that there are enough live tablet "
            "servers to satisfy the provided replication count before allowing "
            "a table to be created.");
TAG_FLAG(catalog_manager_check_ts_count_for_create_table, hidden);

METRIC_DEFINE_gauge_uint32(cluster, num_tablet_servers_live,
                           "Number of live tservers in the cluster", yb::MetricUnit::kUnits,
                           "The number of tablet servers that have responded or done a heartbeat "
                           "in the time interval defined by the gflag "
                           "FLAGS_tserver_unresponsive_timeout_ms.");

METRIC_DEFINE_gauge_uint32(cluster, num_tablet_servers_dead,
                           "Number of dead tservers in the cluster", yb::MetricUnit::kUnits,
                           "The number of tablet servers that have not responded or done a "
                           "heartbeat in the time interval defined by the gflag "
                           "FLAGS_tserver_unresponsive_timeout_ms.");

DEFINE_test_flag(uint64, inject_latency_during_remote_bootstrap_secs, 0,
                 "Number of seconds to sleep during a remote bootstrap.");

DEFINE_test_flag(uint64, inject_latency_during_tablet_report_ms, 0,
                 "Number of milliseconds to sleep during the processing of a tablet batch.");

DEFINE_test_flag(bool, catalog_manager_simulate_system_table_create_failure, false,
                 "This is only used in tests to simulate a failure where the table information is "
                 "persisted in syscatalog, but the tablet information is not yet persisted and "
                 "there is a failure.");

DEFINE_string(cluster_uuid, "", "Cluster UUID to be used by this cluster");
TAG_FLAG(cluster_uuid, hidden);

DECLARE_int32(yb_num_shards_per_tserver);

DEFINE_uint64(transaction_table_num_tablets, 0,
    "Number of tablets to use when creating the transaction status table."
    "0 to use the same default num tablets as for regular tables.");

DEFINE_bool(master_enable_metrics_snapshotter, false, "Should metrics snapshotter be enabled");

DEFINE_uint64(metrics_snapshots_table_num_tablets, 0,
    "Number of tablets to use when creating the metrics snapshots table."
    "0 to use the same default num tablets as for regular tables.");

DEFINE_bool(disable_index_backfill, false,
    "A kill switch to disable multi-stage backfill for YCQL indexes.");
TAG_FLAG(disable_index_backfill, runtime);
TAG_FLAG(disable_index_backfill, hidden);

DEFINE_bool(disable_index_backfill_for_non_txn_tables, true,
    "A kill switch to disable multi-stage backfill for user encorced YCQL indexes. "
    "Note that setting this to true may cause the create index flow to be slow. "
    "This is needed to ensure the safety of the index backfill process. See also "
    "index_backfill_upperbound_for_user_enforced_txn_duration_ms");
TAG_FLAG(disable_index_backfill_for_non_txn_tables, runtime);
TAG_FLAG(disable_index_backfill_for_non_txn_tables, hidden);

DEFINE_int32(ysql_backfill_is_create_table_done_delay_ms, 0,
    "Time to wait after IsCreateTableDone for an index using online schema migration finds the"
    " index in the master index map.");
TAG_FLAG(ysql_backfill_is_create_table_done_delay_ms, hidden);
TAG_FLAG(ysql_backfill_is_create_table_done_delay_ms, runtime);

DEFINE_bool(enable_transactional_ddl_gc, true,
    "A kill switch for transactional DDL GC. Temporary safety measure.");
TAG_FLAG(enable_transactional_ddl_gc, runtime);
TAG_FLAG(enable_transactional_ddl_gc, hidden);

DEFINE_bool(
    hide_pg_catalog_table_creation_logs, false,
    "Whether to hide detailed log messages for PostgreSQL catalog table creation. "
    "This cuts down test logs significantly.");
TAG_FLAG(hide_pg_catalog_table_creation_logs, hidden);

DEFINE_test_flag(int32, simulate_slow_table_create_secs, 0,
    "Simulates a slow table creation by sleeping after the table has been added to memory.");

DEFINE_test_flag(int32, simulate_slow_system_tablet_bootstrap_secs, 0,
    "Simulates a slow tablet bootstrap by adding a sleep before system tablet init.");

DEFINE_test_flag(bool, return_error_if_namespace_not_found, false,
    "Return an error from ListTables if a namespace id is not found in the map");

DEFINE_test_flag(bool, hang_on_namespace_transition, false,
    "Used in tests to simulate a lapse between issuing a namespace op and final processing.");

DEFINE_test_flag(bool, simulate_crash_after_table_marked_deleting, false,
    "Crash yb-master after table's state is set to DELETING. This skips tablets deletion.");

DEFINE_bool(master_drop_table_after_task_response, true,
            "Mark a table as DELETED as soon as we get all the responses from all the TS.");
TAG_FLAG(master_drop_table_after_task_response, advanced);
TAG_FLAG(master_drop_table_after_task_response, runtime);

DECLARE_int32(yb_client_admin_operation_timeout_sec);

DEFINE_test_flag(bool, tablegroup_master_only, false,
                 "This is only for MasterTest to be able to test tablegroups without the"
                 " transaction status table being created.");

DEFINE_bool(enable_register_ts_from_raft, false, "Whether to register a tserver from the consensus "
                                                 "information of a reported tablet.");

DECLARE_int32(tserver_unresponsive_timeout_ms);

DEFINE_bool(use_create_table_leader_hint, true,
            "Whether the Master should hint which replica for each tablet should "
            "be leader initially on tablet creation.");
TAG_FLAG(use_create_table_leader_hint, runtime);

DEFINE_test_flag(bool, create_table_leader_hint_min_lexicographic, false,
                 "Whether the Master should hint replica with smallest lexicographic rank for each "
                 "tablet as leader initially on tablet creation.");

DEFINE_int32(tablet_split_limit_per_table, 256,
             "Limit of the number of tablets per table for tablet splitting. Limitation is "
             "disabled if this value is set to 0.");

DEFINE_double(heartbeat_safe_deadline_ratio, .20,
              "When the heartbeat deadline has this percentage of time remaining, "
              "the master should halt tablet report processing so it can respond in time.");
DECLARE_int32(heartbeat_rpc_timeout_ms);
DECLARE_CAPABILITY(TabletReportLimit);

DEFINE_int32(partitions_vtable_cache_refresh_secs, 30,
             "Amount of time to wait before refreshing the system.partitions cached vtable.");

DEFINE_int32(txn_table_wait_min_ts_count, 1,
             "Minimum Number of TS to wait for before creating the transaction status table."
             " Default value is 1. We wait for atleast --replication_factor if this value"
             " is smaller than that");
TAG_FLAG(txn_table_wait_min_ts_count, advanced);

namespace yb {
namespace master {

using std::atomic;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

using namespace std::placeholders;

using base::subtle::NoBarrier_Load;
using base::subtle::NoBarrier_CompareAndSwap;
using consensus::kMinimumTerm;
using consensus::CONSENSUS_CONFIG_COMMITTED;
using consensus::CONSENSUS_CONFIG_ACTIVE;
using consensus::COMMITTED_OPID;
using consensus::Consensus;
using consensus::ConsensusMetadata;
using consensus::ConsensusServiceProxy;
using consensus::ConsensusStatePB;
using consensus::GetConsensusRole;
using consensus::RaftPeerPB;
using consensus::StartRemoteBootstrapRequestPB;
using rpc::RpcContext;
using strings::Substitute;
using tablet::TABLET_DATA_COPYING;
using tablet::TABLET_DATA_DELETED;
using tablet::TABLET_DATA_READY;
using tablet::TABLET_DATA_TOMBSTONED;
using tablet::TabletDataState;
using tablet::RaftGroupMetadata;
using tablet::RaftGroupMetadataPtr;
using tablet::TabletPeer;
using tablet::RaftGroupStatePB;
using tablet::TabletStatusListener;
using tablet::TabletStatusPB;
using tserver::HandleReplacingStaleTablet;
using tserver::TabletServerErrorPB;
using master::MasterServiceProxy;
using yb::pgwrapper::PgWrapper;
using yb::server::MasterAddressesToString;

using yb::client::YBClient;
using yb::client::YBClientBuilder;
using yb::client::YBColumnSchema;
using yb::client::YBSchema;
using yb::client::YBSchemaBuilder;
using yb::client::YBTable;
using yb::client::YBTableCreator;
using yb::client::YBTableName;

namespace {

// Macros to access index information in CATALOG.
//
// NOTES from file master.proto for SysTablesEntryPB.
// - For index table: [to be deprecated and replaced by "index_info"]
//     optional bytes indexed_table_id = 13; // Indexed table id of this index.
//     optional bool is_local_index = 14 [ default = false ];  // Whether this is a local index.
//     optional bool is_unique_index = 15 [ default = false ]; // Whether this is a unique index.
// - During transition period, we have to consider both fields and the following macros help
//   avoiding duplicate protobuf version check thru out our code.

#define PROTO_GET_INDEXED_TABLE_ID(tabpb) \
  (tabpb.has_index_info() ? tabpb.index_info().indexed_table_id() \
                          : tabpb.indexed_table_id())

#define PROTO_GET_IS_LOCAL(tabpb) \
  (tabpb.has_index_info() ? tabpb.index_info().is_local() \
                          : tabpb.is_local_index())

#define PROTO_GET_IS_UNIQUE(tabpb) \
  (tabpb.has_index_info() ? tabpb.index_info().is_unique() \
                          : tabpb.is_unique_index())

#define PROTO_IS_INDEX(tabpb) \
  (tabpb.has_index_info() || !tabpb.indexed_table_id().empty())

#define PROTO_IS_TABLE(tabpb) \
  (!tabpb.has_index_info() && tabpb.indexed_table_id().empty())

#define PROTO_PTR_IS_INDEX(tabpb) \
  (tabpb->has_index_info() || !tabpb->indexed_table_id().empty())

#define PROTO_PTR_IS_TABLE(tabpb) \
  (!tabpb->has_index_info() && tabpb->indexed_table_id().empty())

#if (0)
// Once the deprecated fields are obsolete, the above macros should be defined as the following.
#define PROTO_GET_INDEXED_TABLE_ID(tabpb) (tabpb.index_info().indexed_table_id())
#define PROTO_GET_IS_LOCAL(tabpb) (tabpb.index_info().is_local())
#define PROTO_GET_IS_UNIQUE(tabpb) (tabpb.index_info().is_unique())
#define PROTO_IS_INDEX(tabpb) (tabpb.has_index_info())
#define PROTO_IS_TABLE(tabpb) (!tabpb.has_index_info())
#define PROTO_PTR_IS_INDEX(tabpb) (tabpb->has_index_info())
#define PROTO_PTR_IS_TABLE(tabpb) (!tabpb->has_index_info())

#endif

class IndexInfoBuilder {
 public:
  explicit IndexInfoBuilder(IndexInfoPB* index_info) : index_info_(*index_info) {
    DVLOG(3) << " After " << __PRETTY_FUNCTION__ << " index_info_ is " << yb::ToString(index_info_);
  }

  void ApplyProperties(const TableId& indexed_table_id, bool is_local, bool is_unique) {
    index_info_.set_indexed_table_id(indexed_table_id);
    index_info_.set_version(0);
    index_info_.set_is_local(is_local);
    index_info_.set_is_unique(is_unique);
    DVLOG(3) << " After " << __PRETTY_FUNCTION__ << " index_info_ is " << yb::ToString(index_info_);
  }

  CHECKED_STATUS ApplyColumnMapping(const Schema& indexed_schema, const Schema& index_schema) {
    for (size_t i = 0; i < index_schema.num_columns(); i++) {
      const auto& col_name = index_schema.column(i).name();
      const auto indexed_col_idx = indexed_schema.find_column(col_name);
      if (PREDICT_FALSE(indexed_col_idx == Schema::kColumnNotFound)) {
        return STATUS(NotFound, "The indexed table column does not exist", col_name);
      }
      auto* col = index_info_.add_columns();
      col->set_column_id(index_schema.column_id(i));
      col->set_indexed_column_id(indexed_schema.column_id(indexed_col_idx));
    }
    index_info_.set_hash_column_count(index_schema.num_hash_key_columns());
    index_info_.set_range_column_count(index_schema.num_range_key_columns());

    for (size_t i = 0; i < indexed_schema.num_hash_key_columns(); i++) {
      index_info_.add_indexed_hash_column_ids(indexed_schema.column_id(i));
    }
    for (size_t i = indexed_schema.num_hash_key_columns(); i < indexed_schema.num_key_columns();
        i++) {
      index_info_.add_indexed_range_column_ids(indexed_schema.column_id(i));
    }
    DVLOG(3) << " After " << __PRETTY_FUNCTION__ << " index_info_ is " << yb::ToString(index_info_);
    return Status::OK();
  }

 private:
  IndexInfoPB& index_info_;
};

template<class RespClass>
Status CheckIfTableDeletedOrNotRunning(TableInfo::lock_type* lock, RespClass* resp) {
  // This covers both in progress and fully deleted objects.
  if (lock->data().started_deleting()) {
    Status s = STATUS_SUBSTITUTE(NotFound,
        "The object '$0.$1' does not exist", lock->data().namespace_id(), lock->data().name());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }
  if (!lock->data().is_running()) {
    Status s = STATUS_SUBSTITUTE(ServiceUnavailable,
        "The object '$0.$1' is not running", lock->data().namespace_id(), lock->data().name());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }
  return Status::OK();
}

#define RETURN_NAMESPACE_NOT_FOUND(s, resp)                                       \
  do {                                                                            \
    if (PREDICT_FALSE(!s.ok())) {                                                 \
      if (s.IsNotFound()) {                                                       \
        return SetupError(                                                        \
            resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);        \
      }                                                                           \
      return s;                                                                   \
    }                                                                             \
  } while (false)

MasterErrorPB_Code NamespaceMasterError(SysNamespaceEntryPB_State state) {
  switch (state) {
    case SysNamespaceEntryPB::PREPARING: FALLTHROUGH_INTENDED;
    case SysNamespaceEntryPB::DELETING:
      return MasterErrorPB::IN_TRANSITION_CAN_RETRY;
    case SysNamespaceEntryPB::DELETED: FALLTHROUGH_INTENDED;
    case SysNamespaceEntryPB::FAILED: FALLTHROUGH_INTENDED;
    case SysNamespaceEntryPB::RUNNING:
      return MasterErrorPB::INTERNAL_ERROR;
    default:
      FATAL_INVALID_ENUM_VALUE(SysNamespaceEntryPB_State, state);
  }
}

size_t GetNameMapperIndex(YQLDatabase db_type) {
  switch (db_type) {
    case YQL_DATABASE_UNKNOWN: break;
    case YQL_DATABASE_CQL: return 1;
    case YQL_DATABASE_PGSQL: return 2;
    case YQL_DATABASE_REDIS: return 3;
  }
  CHECK(false) << "Unexpected db type " << db_type;
  return 0;
}

bool IsIndexBackfillEnabled(TableType table_type, bool is_transactional) {
  // Fetch the runtime flag to prevent any issues from the updates to flag while processing.
  const bool disabled =
      (table_type == PGSQL_TABLE_TYPE
          ? GetAtomicFlag(&FLAGS_ysql_disable_index_backfill)
          : GetAtomicFlag(&FLAGS_disable_index_backfill) ||
      (!is_transactional && GetAtomicFlag(&FLAGS_disable_index_backfill_for_non_txn_tables)));
  return !disabled;
}

constexpr auto kDefaultYQLPartitionsRefreshBgTaskSleep = 10s;

}  // anonymous namespace

////////////////////////////////////////////////////////////
// CatalogManager
////////////////////////////////////////////////////////////

CatalogManager::NamespaceInfoMap& CatalogManager::NamespaceNameMapper::operator[](
    YQLDatabase db_type) {
  return typed_maps_[GetNameMapperIndex(db_type)];
}

const CatalogManager::NamespaceInfoMap& CatalogManager::NamespaceNameMapper::operator[](
    YQLDatabase db_type) const {
  return typed_maps_[GetNameMapperIndex(db_type)];
}

void CatalogManager::NamespaceNameMapper::clear() {
  for (auto& m : typed_maps_) {
    m.clear();
  }
}

CatalogManager::CatalogManager(Master* master)
    : master_(master),
      rng_(GetRandomSeed32()),
      tablet_exists_(false),
      state_(kConstructed),
      leader_ready_term_(-1),
      leader_lock_(RWMutex::Priority::PREFER_WRITING),
      load_balance_policy_(new enterprise::ClusterLoadBalancer(this)),
      permissions_manager_(std::make_unique<PermissionsManager>(this)),
      tasks_tracker_(new TasksTracker(IsUserInitiated::kFalse)),
      jobs_tracker_(new TasksTracker(IsUserInitiated::kTrue)),
      encryption_manager_(new EncryptionManager()),
      ysql_transaction_(this, master_) {
  yb::InitCommonFlags();
  CHECK_OK(ThreadPoolBuilder("leader-initialization")
           .set_max_threads(1)
           .Build(&leader_initialization_pool_));
  CHECK_OK(ThreadPoolBuilder("CatalogManagerBGTasks").Build(&background_tasks_thread_pool_));
  ysql_transaction_.set_thread_pool(background_tasks_thread_pool_.get());
  CHECK_OK(ThreadPoolBuilder("async-tasks")
           .Build(&async_task_pool_));

  if (master_) {
    sys_catalog_.reset(new SysCatalogTable(
        master_, master_->metric_registry(),
        Bind(&CatalogManager::ElectedAsLeaderCb, Unretained(this))));
  }
}

CatalogManager::~CatalogManager() {
  if (StartShutdown()) {
    CompleteShutdown();
  }
}

Status CatalogManager::Init() {
  {
    std::lock_guard<simple_spinlock> l(state_lock_);
    CHECK_EQ(kConstructed, state_);
    state_ = kStarting;
  }

  // Initialize the metrics emitted by the catalog manager.
  metric_num_tablet_servers_live_ =
    METRIC_num_tablet_servers_live.Instantiate(master_->metric_entity_cluster(), 0);

  metric_num_tablet_servers_dead_ =
    METRIC_num_tablet_servers_dead.Instantiate(master_->metric_entity_cluster(), 0);

  RETURN_NOT_OK_PREPEND(InitSysCatalogAsync(),
                        "Failed to initialize sys tables async");

  if (PREDICT_FALSE(FLAGS_TEST_simulate_slow_system_tablet_bootstrap_secs > 0)) {
    LOG_WITH_PREFIX(INFO) << "Simulating slow system tablet bootstrap";
    SleepFor(MonoDelta::FromSeconds(FLAGS_TEST_simulate_slow_system_tablet_bootstrap_secs));
  }

  // WaitUntilRunning() must run outside of the lock as to prevent
  // deadlock. This is safe as WaitUntilRunning waits for another
  // thread to finish its work and doesn't itself depend on any state
  // within CatalogManager. Need not start sys catalog or background tasks
  // when we are started in shell mode.
  if (!master_->opts().IsShellMode()) {
    RETURN_NOT_OK_PREPEND(sys_catalog_->WaitUntilRunning(),
                          "Failed waiting for the catalog tablet to run");
    std::vector<consensus::RaftPeerPB> masters_raft;
    RETURN_NOT_OK(master_->ListRaftConfigMasters(&masters_raft));
    HostPortSet hps;
    for (const auto& peer : masters_raft) {
      if (master_->instance_pb().permanent_uuid() == peer.permanent_uuid()) {
        continue;
      }
      HostPort hp = HostPortFromPB(DesiredHostPort(peer, master_->MakeCloudInfoPB()));
      hps.insert(hp);
    }
    RETURN_NOT_OK(encryption_manager_->AddPeersToGetUniverseKeyFrom(hps));
    RETURN_NOT_OK(EnableBgTasks());
  }

  {
    std::lock_guard<simple_spinlock> l(state_lock_);
    CHECK_EQ(kStarting, state_);
    state_ = kRunning;
  }

  Started();

  return Status::OK();
}

Status CatalogManager::ChangeEncryptionInfo(const ChangeEncryptionInfoRequestPB* req,
                                            ChangeEncryptionInfoResponsePB* resp) {
  return STATUS(InvalidCommand, "Command only supported in enterprise build.");
}

Status CatalogManager::ElectedAsLeaderCb() {
  time_elected_leader_ = MonoTime::Now();
  return leader_initialization_pool_->SubmitClosure(
      Bind(&CatalogManager::LoadSysCatalogDataTask, Unretained(this)));
}

Status CatalogManager::WaitUntilCaughtUpAsLeader(const MonoDelta& timeout) {
  string uuid = master_->fs_manager()->uuid();
  Consensus* consensus = tablet_peer()->consensus();
  ConsensusStatePB cstate = consensus->ConsensusState(CONSENSUS_CONFIG_ACTIVE);
  if (!cstate.has_leader_uuid() || cstate.leader_uuid() != uuid) {
    return STATUS_SUBSTITUTE(IllegalState,
        "Node $0 not leader. Consensus state: $1", uuid, cstate.ShortDebugString());
  }

  // Wait for all transactions to be committed.
  const CoarseTimePoint deadline = CoarseMonoClock::now() + timeout;
  {
    tablet::HistoryCutoffPropagationDisabler disabler(tablet_peer()->tablet()->RetentionPolicy());
    RETURN_NOT_OK(tablet_peer()->operation_tracker()->WaitForAllToFinish(timeout));
  }

  RETURN_NOT_OK(tablet_peer()->consensus()->WaitForLeaderLeaseImprecise(deadline));
  return Status::OK();
}

void CatalogManager::LoadSysCatalogDataTask() {
  auto consensus = tablet_peer()->shared_consensus();
  const int64_t term = consensus->ConsensusState(CONSENSUS_CONFIG_ACTIVE).current_term();
  Status s = WaitUntilCaughtUpAsLeader(
      MonoDelta::FromMilliseconds(FLAGS_master_failover_catchup_timeout_ms));

  int64_t term_after_wait = consensus->ConsensusState(CONSENSUS_CONFIG_ACTIVE).current_term();
  if (term_after_wait != term) {
    // If we got elected leader again while waiting to catch up then we will get another callback to
    // update state from sys_catalog, so bail now.
    //
    // If we failed when waiting, i.e. could not acquire a leader lease, this could be due to us
    // becoming a follower. If we're not partitioned away, we'll know about a new term soon.
    LOG_WITH_PREFIX(INFO)
        << "Term change from " << term << " to " << term_after_wait
        << " while waiting for master leader catchup. Not loading sys catalog metadata. "
        << "Status of waiting: " << s;
    return;
  }

  if (!s.ok()) {
    // This could happen e.g. if we are a partitioned-away leader that failed to acquire a leader
    // lease.
    //
    // TODO: handle this cleanly by transitioning to a follower without crashing.
    LOG_WITH_PREFIX(WARNING) << "Failed waiting for node to catch up after master election: " << s;

    if (s.IsTimedOut()) {
      LOG_WITH_PREFIX(FATAL) << "Shutting down due to unavailability of other masters after"
                             << " election. TODO: Abdicate instead.";
    }
    return;
  }

  LOG_WITH_PREFIX(INFO) << "Loading table and tablet metadata into memory for term " << term;
  LOG_SLOW_EXECUTION(WARNING, 1000, LogPrefix() + "Loading metadata into memory") {
    Status status = VisitSysCatalog(term);
    if (!status.ok()) {
      {
        std::lock_guard<simple_spinlock> l(state_lock_);
        if (state_ == kClosing) {
          LOG_WITH_PREFIX(INFO)
              << "Error loading sys catalog; because shutdown is in progress. term " << term
              << " status : " << status;
          return;
        }
      }
      auto new_term = consensus->ConsensusState(CONSENSUS_CONFIG_ACTIVE).current_term();
      if (new_term != term) {
        LOG_WITH_PREFIX(INFO)
            << "Error loading sys catalog; but that's OK as term was changed from " << term
            << " to " << new_term << ": " << status;
        return;
      }
      LOG_WITH_PREFIX(FATAL) << "Failed to load sys catalog: " << status;
    }
  }

  std::lock_guard<simple_spinlock> l(state_lock_);
  leader_ready_term_ = term;
  LOG_WITH_PREFIX(INFO) << "Completed load of sys catalog in term " << term;
}

CHECKED_STATUS CatalogManager::WaitForWorkerPoolTests(const MonoDelta& timeout) const {
  if (!async_task_pool_->WaitFor(timeout)) {
    return STATUS(TimedOut, "Worker Pool hasn't finished processing tasks");
  }
  return Status::OK();
}

Status CatalogManager::VisitSysCatalog(int64_t term) {
  // Block new catalog operations, and wait for existing operations to finish.
  LOG_WITH_PREFIX(INFO)
      << __func__ << ": Wait on leader_lock_ for any existing operations to finish.";
  auto start = std::chrono::steady_clock::now();
  std::lock_guard<RWMutex> leader_lock_guard(leader_lock_);
  auto finish = std::chrono::steady_clock::now();

  static const auto kLongLockAcquisitionLimit = RegularBuildVsSanitizers(100ms, 750ms);
  if (finish > start + kLongLockAcquisitionLimit) {
    LOG_WITH_PREFIX(WARNING) << "Long wait on leader_lock_: " << yb::ToString(finish - start);
  }

  LOG_WITH_PREFIX(INFO)
      << __func__ << ": Acquire catalog manager lock_ before loading sys catalog.";
  std::lock_guard<LockType> lock(lock_);
  VLOG(3) << __func__ << ": Acquired the catalog manager lock_";

  // Abort any outstanding tasks. All TableInfos are orphaned below, so
  // it's important to end their tasks now; otherwise Shutdown() will
  // destroy master state used by these tasks.
  std::vector<scoped_refptr<TableInfo>> tables;
  AppendValuesFromMap(*table_ids_map_, &tables);
  AbortAndWaitForAllTasks(tables);

  // Clear internal maps and run data loaders.
  RETURN_NOT_OK(RunLoaders(term));

  // Prepare various default system configurations.
  RETURN_NOT_OK(PrepareDefaultSysConfig(term));

  if ((FLAGS_use_initial_sys_catalog_snapshot || FLAGS_enable_ysql) &&
      !FLAGS_initial_sys_catalog_snapshot_path.empty() &&
      !FLAGS_create_initial_sys_catalog_snapshot) {
    if (!namespace_ids_map_.empty() || !system_tablets_.empty()) {
      LOG_WITH_PREFIX(INFO)
          << "This is an existing cluster, not initializing from a sys catalog snapshot.";
    } else {
      Result<bool> dir_exists =
          Env::Default()->DoesDirectoryExist(FLAGS_initial_sys_catalog_snapshot_path);
      if (dir_exists.ok() && *dir_exists) {
        bool initdb_was_already_done = false;
        {
          auto l = ysql_catalog_config_->LockForRead();
          initdb_was_already_done = l->data().pb.ysql_catalog_config().initdb_done();
        }
        if (initdb_was_already_done) {
          LOG_WITH_PREFIX(INFO)
              << "initdb has been run before, no need to restore sys catalog from "
              << "the initial snapshot";
        } else {
          LOG_WITH_PREFIX(INFO) << "Restoring snapshot in sys catalog";
          Status restore_status = RestoreInitialSysCatalogSnapshot(
              FLAGS_initial_sys_catalog_snapshot_path,
              sys_catalog_->tablet_peer().get(),
              term);
          if (!restore_status.ok()) {
            LOG_WITH_PREFIX(ERROR) << "Failed restoring snapshot in sys catalog";
            return restore_status;
          }

          LOG_WITH_PREFIX(INFO) << "Re-initializing cluster config";
          cluster_config_.reset();
          RETURN_NOT_OK(PrepareDefaultClusterConfig(term));

          LOG_WITH_PREFIX(INFO) << "Restoring snapshot completed, considering initdb finished";
          RETURN_NOT_OK(InitDbFinished(Status::OK(), term));
          RETURN_NOT_OK(RunLoaders(term));
        }
      } else {
        LOG_WITH_PREFIX(WARNING)
            << "Initial sys catalog snapshot directory does not exist: "
            << FLAGS_initial_sys_catalog_snapshot_path
            << (dir_exists.ok() ? "" : ", status: " + dir_exists.status().ToString());
      }
    }
  }

  // Create the system namespaces (created only if they don't already exist).
  RETURN_NOT_OK(PrepareDefaultNamespaces(term));

  // Create the system tables (created only if they don't already exist).
  RETURN_NOT_OK(PrepareSystemTables(term));

  // Create the default cassandra (created only if they don't already exist).
  RETURN_NOT_OK(permissions_manager_->PrepareDefaultRoles(term));

  // If this is the first time we start up, we have no config information as default. We write an
  // empty version 0.
  RETURN_NOT_OK(PrepareDefaultClusterConfig(term));

  permissions_manager_->BuildRecursiveRolesUnlocked();

  if (FLAGS_enable_ysql) {
    // Number of TS to wait for before creating the txn table.
    auto wait_ts_count = std::max(FLAGS_txn_table_wait_min_ts_count, FLAGS_replication_factor);

    LOG_WITH_PREFIX(INFO)
        << "YSQL is enabled, will create the transaction status table when "
        << wait_ts_count << " tablet servers are online";
    master_->ts_manager()->SetTSCountCallback(wait_ts_count, [this, wait_ts_count] {
      LOG_WITH_PREFIX(INFO)
          << wait_ts_count
          << " tablet servers registered, creating the transaction status table";
      // Retry table creation until it succeedes. It might fail initially because placement UUID
      // of live replicas is set through an RPC from YugaWare, and we won't be able to calculate
      // the number of primary (non-read-replica) tablet servers until that happens.
      while (true) {
        const auto s = CreateTransactionsStatusTableIfNeeded(/* rpc */ nullptr);
        if (s.ok()) {
          break;
        }
        LOG_WITH_PREFIX(WARNING) << "Failed creating transaction status table, waiting: " << s;
        if (s.IsShutdownInProgress()) {
          return;
        }
        SleepFor(MonoDelta::FromSeconds(1));
      }
      LOG_WITH_PREFIX(INFO) << "Finished creating transaction status table asynchronously";
    });
  }

  if (!StartRunningInitDbIfNeeded(term)) {
    // If we are not running initdb, this is an existing cluster, and we need to check whether we
    // need to do a one-time migration to make YSQL system catalog tables transactional.
    RETURN_NOT_OK(MakeYsqlSysCatalogTablesTransactional(
        table_ids_map_.CheckOut().get_ptr(), sys_catalog_.get(), ysql_catalog_config_.get(), term));
  }

  return Status::OK();
}

template <class Loader>
Status CatalogManager::Load(const std::string& title, const int64_t term) {
  LOG_WITH_PREFIX(INFO) << __func__ << ": Loading " << title << " into memory.";
  std::unique_ptr<Loader> loader = std::make_unique<Loader>(this, term);
  RETURN_NOT_OK_PREPEND(
      sys_catalog_->Visit(loader.get()),
      "Failed while visiting " + title + " in sys catalog");
  return Status::OK();
}

Status CatalogManager::RunLoaders(int64_t term) {
  // Clear the table and tablet state.
  table_names_map_.clear();
  auto table_ids_map_checkout = table_ids_map_.CheckOut();
  table_ids_map_checkout->clear();

  auto tablet_map_checkout = tablet_map_.CheckOut();
  tablet_map_checkout->clear();

  // Clear the namespace mappings.
  namespace_ids_map_.clear();
  namespace_names_mapper_.clear();

  // Clear the type mappings.
  udtype_ids_map_.clear();
  udtype_names_map_.clear();

  // Clear the current cluster config.
  cluster_config_.reset();

  // Clear the roles mapping.
  permissions_manager()->ClearRolesUnlocked();

  // Clear redis config mapping.
  redis_config_map_.clear();

  // Clear ysql catalog config.
  ysql_catalog_config_.reset();

  // Clear recent tasks.
  tasks_tracker_->Reset();

  // Clear recent jobs.
  jobs_tracker_->Reset();

  std::vector<std::shared_ptr<TSDescriptor>> descs;
  master_->ts_manager()->GetAllDescriptors(&descs);
  for (const auto& ts_desc : descs) {
    ts_desc->set_has_tablet_report(false);
  }

  RETURN_NOT_OK(Load<TableLoader>("tables", term));
  RETURN_NOT_OK(Load<TabletLoader>("tablets", term));
  RETURN_NOT_OK(Load<NamespaceLoader>("namespaces", term));
  RETURN_NOT_OK(Load<UDTypeLoader>("user-defined types", term));
  RETURN_NOT_OK(Load<ClusterConfigLoader>("cluster configuration", term));
  RETURN_NOT_OK(Load<RoleLoader>("roles", term));
  RETURN_NOT_OK(Load<RedisConfigLoader>("Redis config", term));
  RETURN_NOT_OK(Load<SysConfigLoader>("sys config", term));

  return Status::OK();
}

Status CatalogManager::PrepareDefaultClusterConfig(int64_t term) {
  if (cluster_config_) {
    LOG_WITH_PREFIX(INFO)
        << "Cluster configuration has already been set up, skipping re-initialization.";
    return Status::OK();
  }

  // Create default.
  SysClusterConfigEntryPB config;
  config.set_version(0);

  std::string cluster_uuid_source;
  if (!FLAGS_cluster_uuid.empty()) {
    Uuid uuid;
    RETURN_NOT_OK(uuid.FromString(FLAGS_cluster_uuid));
    config.set_cluster_uuid(FLAGS_cluster_uuid);
    cluster_uuid_source = "from the --cluster_uuid flag";
  } else {
    auto uuid = Uuid::Generate();
    config.set_cluster_uuid(to_string(uuid));
    cluster_uuid_source = "(randomly generated)";
  }
  LOG_WITH_PREFIX(INFO)
      << "Setting cluster UUID to " << config.cluster_uuid() << " " << cluster_uuid_source;

  // Create in memory object.
  cluster_config_ = new ClusterConfigInfo();

  // Prepare write.
  auto l = cluster_config_->LockForWrite();
  l->mutable_data()->pb = std::move(config);

  // Write to sys_catalog and in memory.
  RETURN_NOT_OK(sys_catalog_->AddItem(cluster_config_.get(), term));
  l->Commit();

  return Status::OK();
}

Status CatalogManager::PrepareDefaultSysConfig(int64_t term) {
  RETURN_NOT_OK(permissions_manager()->PrepareDefaultSecurityConfigUnlocked(term));

  if (!ysql_catalog_config_) {
    SysYSQLCatalogConfigEntryPB ysql_catalog_config;
    ysql_catalog_config.set_version(0);

    // Create in memory objects.
    ysql_catalog_config_ = new SysConfigInfo(kYsqlCatalogConfigType);

    // Prepare write.
    auto l = ysql_catalog_config_->LockForWrite();
    *l->mutable_data()->pb.mutable_ysql_catalog_config() = std::move(ysql_catalog_config);

    // Write to sys_catalog and in memory.
    RETURN_NOT_OK(sys_catalog_->AddItem(ysql_catalog_config_.get(), term));
    l->Commit();
  }

  return Status::OK();
}

bool CatalogManager::StartRunningInitDbIfNeeded(int64_t term) {
  if (!ShouldAutoRunInitDb(ysql_catalog_config_.get(), pg_proc_exists_)) {
    return false;
  }

  string master_addresses_str = MasterAddressesToString(
      *master_->opts().GetMasterAddresses());

  initdb_future_ = std::async(std::launch::async, [this, master_addresses_str, term] {
    if (FLAGS_create_initial_sys_catalog_snapshot) {
      initial_snapshot_writer_.emplace();
    }

    Status status = PgWrapper::InitDbForYSQL(master_addresses_str, "/tmp");

    if (FLAGS_create_initial_sys_catalog_snapshot && status.ok()) {
      Status write_snapshot_status = initial_snapshot_writer_->WriteSnapshot(
          sys_catalog_->tablet_peer()->tablet(),
          FLAGS_initial_sys_catalog_snapshot_path);
      if (!write_snapshot_status.ok()) {
        status = write_snapshot_status;
      }
    }
    Status finish_status = InitDbFinished(status, term);
    if (!finish_status.ok()) {
      if (status.ok()) {
        status = finish_status;
      }
      LOG_WITH_PREFIX(WARNING)
          << "Failed to set initdb as finished in sys catalog: " << finish_status;
    }
    return status;
  });
  return true;
}

Status CatalogManager::PrepareDefaultNamespaces(int64_t term) {
  RETURN_NOT_OK(PrepareNamespace(
      YQL_DATABASE_CQL, kSystemNamespaceName, kSystemNamespaceId, term));
  RETURN_NOT_OK(PrepareNamespace(
      YQL_DATABASE_CQL, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId, term));
  RETURN_NOT_OK(PrepareNamespace(
      YQL_DATABASE_CQL, kSystemAuthNamespaceName, kSystemAuthNamespaceId, term));
  return Status::OK();
}

Status CatalogManager::PrepareSystemTables(int64_t term) {
  // Prepare sys catalog table.
  RETURN_NOT_OK(PrepareSysCatalogTable(term));

  // Create the required system tables here.
  RETURN_NOT_OK((PrepareSystemTableTemplate<PeersVTable>(
      kSystemPeersTableName, kSystemNamespaceName, kSystemNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<LocalVTable>(
      kSystemLocalTableName, kSystemNamespaceName, kSystemNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLKeyspacesVTable>(
      kSystemSchemaKeyspacesTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId,
      term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLTablesVTable>(
      kSystemSchemaTablesTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLColumnsVTable>(
      kSystemSchemaColumnsTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLSizeEstimatesVTable>(
      kSystemSizeEstimatesTableName, kSystemNamespaceName, kSystemNamespaceId, term)));

  // Empty tables.
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLAggregatesVTable>(
      kSystemSchemaAggregatesTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId,
      term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLFunctionsVTable>(
      kSystemSchemaFunctionsTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId,
      term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLIndexesVTable>(
      kSystemSchemaIndexesTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLTriggersVTable>(
      kSystemSchemaTriggersTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLViewsVTable>(
      kSystemSchemaViewsTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<QLTypesVTable>(
      kSystemSchemaTypesTableName, kSystemSchemaNamespaceName, kSystemSchemaNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLPartitionsVTable>(
      kSystemPartitionsTableName, kSystemNamespaceName, kSystemNamespaceId, term)));

  // System auth tables.
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLAuthRolesVTable>(
      kSystemAuthRolesTableName, kSystemAuthNamespaceName, kSystemAuthNamespaceId, term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLAuthRolePermissionsVTable>(
      kSystemAuthRolePermissionsTableName, kSystemAuthNamespaceName, kSystemAuthNamespaceId,
      term)));
  RETURN_NOT_OK((PrepareSystemTableTemplate<YQLAuthResourceRolePermissionsIndexVTable>(
      kSystemAuthResourceRolePermissionsIndexTableName, kSystemAuthNamespaceName,
      kSystemAuthNamespaceId, term)));

  // Ensure kNumSystemTables is in-sync with the system tables created.
  LOG_IF(DFATAL, system_tablets_.size() != kNumSystemTables)
      << "kNumSystemTables is " << kNumSystemTables << " but " << system_tablets_.size()
      << " tables were created";

  // Cache the system.partitions tablet so we can access it in RebuildYQLSystemPartitions.
  RETURN_NOT_OK(GetYQLPartitionsVTable(&system_partitions_tablet_));

  return Status::OK();
}

Status CatalogManager::PrepareSysCatalogTable(int64_t term) {
  // Prepare sys catalog table info.
  auto sys_catalog_table_iter = table_ids_map_->find(kSysCatalogTableId);
  if (sys_catalog_table_iter == table_ids_map_->end()) {
    scoped_refptr<TableInfo> table = NewTableInfo(kSysCatalogTableId);
    table->mutable_metadata()->StartMutation();
    SysTablesEntryPB& metadata = table->mutable_metadata()->mutable_dirty()->pb;
    metadata.set_state(SysTablesEntryPB::RUNNING);
    metadata.set_namespace_id(kSystemSchemaNamespaceId);
    metadata.set_name(kSysCatalogTableName);
    metadata.set_table_type(TableType::YQL_TABLE_TYPE);
    SchemaToPB(sys_catalog_->schema_, metadata.mutable_schema());
    metadata.set_version(0);

    auto table_ids_map_checkout = table_ids_map_.CheckOut();
    sys_catalog_table_iter = table_ids_map_checkout->emplace(table->id(), table).first;
    table_names_map_[{kSystemSchemaNamespaceId, kSysCatalogTableName}] = table;
    table->set_is_system();

    RETURN_NOT_OK(sys_catalog_->AddItem(table.get(), term));
    table->mutable_metadata()->CommitMutation();
  }

  // Prepare sys catalog tablet info.
  if (tablet_map_->count(kSysCatalogTabletId) == 0) {
    scoped_refptr<TableInfo> table = sys_catalog_table_iter->second;
    scoped_refptr<TabletInfo> tablet(new TabletInfo(table, kSysCatalogTabletId));
    tablet->mutable_metadata()->StartMutation();
    SysTabletsEntryPB& metadata = tablet->mutable_metadata()->mutable_dirty()->pb;
    metadata.set_state(SysTabletsEntryPB::RUNNING);

    auto l = table->LockForRead();
    PartitionSchema partition_schema;
    RETURN_NOT_OK(PartitionSchema::FromPB(l->data().pb.partition_schema(),
                                          sys_catalog_->schema_,
                                          &partition_schema));
    vector<Partition> partitions;
    RETURN_NOT_OK(partition_schema.CreatePartitions(1, &partitions));
    partitions[0].ToPB(metadata.mutable_partition());
    metadata.set_table_id(table->id());
    metadata.add_table_ids(table->id());

    table->set_is_system();
    table->AddTablet(tablet.get());

    auto tablet_map_checkout = tablet_map_.CheckOut();
    (*tablet_map_checkout)[tablet->tablet_id()] = tablet;

    RETURN_NOT_OK(sys_catalog_->AddItem(tablet.get(), term));
    tablet->mutable_metadata()->CommitMutation();
  }

  system_tablets_[kSysCatalogTabletId] = sys_catalog_->tablet_peer_->shared_tablet();

  return Status::OK();
}

template <class T>
Status CatalogManager::PrepareSystemTableTemplate(const TableName& table_name,
                                                  const NamespaceName& namespace_name,
                                                  const NamespaceId& namespace_id,
                                                  int64_t term) {
  YQLVirtualTable* vtable = new T(table_name, namespace_name, master_);
  return PrepareSystemTable(
      table_name, namespace_name, namespace_id, vtable->schema(), term, vtable);
}

Status CatalogManager::PrepareSystemTable(const TableName& table_name,
                                          const NamespaceName& namespace_name,
                                          const NamespaceId& namespace_id,
                                          const Schema& schema,
                                          int64_t term,
                                          YQLVirtualTable* vtable) {
  std::unique_ptr<YQLVirtualTable> yql_storage(vtable);

  scoped_refptr<TableInfo> table = FindPtrOrNull(table_names_map_,
                                                 std::make_pair(namespace_id, table_name));
  bool create_table = true;
  if (table != nullptr) {
    LOG_WITH_PREFIX(INFO) << "Table " << namespace_name << "." << table_name << " already created";

    // Mark the table as a system table.
    table->set_is_system();

    Schema persisted_schema;
    RETURN_NOT_OK(table->GetSchema(&persisted_schema));
    if (!persisted_schema.Equals(schema)) {
      LOG_WITH_PREFIX(INFO)
          << "Updating schema of " << namespace_name << "." << table_name << " ...";
      auto l = table->LockForWrite();
      SchemaToPB(schema, l->mutable_data()->pb.mutable_schema());
      l->mutable_data()->pb.set_version(l->data().pb.version() + 1);

      // Update sys-catalog with the new table schema.
      RETURN_NOT_OK(sys_catalog_->UpdateItem(table.get(), term));
      l->Commit();
    }

    // There might have been a failure after writing the table but before writing the tablets. As
    // a result, if we don't find any tablets, we try to create the tablets only again.
    vector<scoped_refptr<TabletInfo>> tablets;
    table->GetAllTablets(&tablets);
    if (!tablets.empty()) {
      // Initialize the appropriate system tablet.
      DCHECK_EQ(1, tablets.size());
      auto tablet = tablets[0];
      system_tablets_[tablet->tablet_id()] =
          std::make_shared<SystemTablet>(schema, std::move(yql_storage), tablet->tablet_id());
      return Status::OK();
    } else {
      // Table is already created, only need to create tablets now.
      LOG_WITH_PREFIX(INFO)
          << "Creating tablets for " << namespace_name << "." << table_name << " ...";
      create_table = false;
    }
  }

  vector<TabletInfo*> tablets;

  // Create partitions.
  vector<Partition> partitions;
  PartitionSchemaPB partition_schema_pb;
  partition_schema_pb.set_hash_schema(PartitionSchemaPB::MULTI_COLUMN_HASH_SCHEMA);
  PartitionSchema partition_schema;
  RETURN_NOT_OK(PartitionSchema::FromPB(partition_schema_pb, schema, &partition_schema));
  RETURN_NOT_OK(partition_schema.CreatePartitions(1, &partitions));

  if (create_table) {
    // Fill in details for the system table.
    CreateTableRequestPB req;
    req.set_name(table_name);
    req.set_table_type(TableType::YQL_TABLE_TYPE);

    RETURN_NOT_OK(CreateTableInMemory(
        req, schema, partition_schema, true /* create_tablets */, namespace_id, namespace_name,
        partitions, nullptr, &tablets, nullptr, &table));
    // Mark the table as a system table.
    LOG_WITH_PREFIX(INFO) << "Inserted new " << namespace_name << "." << table_name
                          << " table info into CatalogManager maps";
    // Update the on-disk table state to "running".
    table->mutable_metadata()->mutable_dirty()->pb.set_state(SysTablesEntryPB::RUNNING);
    RETURN_NOT_OK(sys_catalog_->AddItem(table.get(), term));
    LOG_WITH_PREFIX(INFO) << "Wrote table to system catalog: " << ToString(table) << ", tablets: "
                          << ToString(tablets);
  } else {
    // Still need to create the tablets.
    RETURN_NOT_OK(CreateTabletsFromTable(partitions, table, &tablets));
  }

  DCHECK_EQ(1, tablets.size());
  // We use LOG_ASSERT here since this is expected to crash in some unit tests.
  LOG_ASSERT(!FLAGS_TEST_catalog_manager_simulate_system_table_create_failure);

  // Write Tablets to sys-tablets (in "running" state since we don't want the loadbalancer to
  // assign these tablets since this table is virtual).
  for (TabletInfo *tablet : tablets) {
    tablet->mutable_metadata()->mutable_dirty()->pb.set_state(SysTabletsEntryPB::RUNNING);
  }
  RETURN_NOT_OK(sys_catalog_->AddItems(tablets, term));
  LOG_WITH_PREFIX(INFO) << "Wrote tablets to system catalog: " << ToString(tablets);

  // Commit the in-memory state.
  if (create_table) {
    table->mutable_metadata()->CommitMutation();
  }

  for (TabletInfo *tablet : tablets) {
    tablet->mutable_metadata()->CommitMutation();
  }
  // Mark the table as a system table.
  table->set_is_system();

  // Finally create the appropriate tablet object.
  auto tablet = tablets[0];
  system_tablets_[tablet->tablet_id()] =
      std::make_shared<SystemTablet>(schema, std::move(yql_storage), tablet->tablet_id());
  return Status::OK();
}

bool CatalogManager::IsYcqlNamespace(const NamespaceInfo& ns) {
  return ns.database_type() == YQLDatabase::YQL_DATABASE_CQL;
}

bool CatalogManager::IsYcqlTable(const TableInfo& table) {
  return table.GetTableType() == TableType::YQL_TABLE_TYPE && table.id() != kSysCatalogTableId;
}

Status CatalogManager::PrepareNamespace(
    YQLDatabase db_type, const NamespaceName& name, const NamespaceId& id, int64_t term) {

  scoped_refptr<NamespaceInfo> ns = FindPtrOrNull(namespace_ids_map_, id);
  if (ns != nullptr) {
    LOG_WITH_PREFIX(INFO)
        << "Keyspace " << ns->ToString() << " already created, skipping initialization";
    return Status::OK();
  }

  // Create entry.
  SysNamespaceEntryPB ns_entry;
  ns_entry.set_name(name);
  ns_entry.set_database_type(db_type);
  ns_entry.set_state(SysNamespaceEntryPB::RUNNING);

  // Create in memory object.
  ns = new NamespaceInfo(id);

  // Prepare write.
  auto l = ns->LockForWrite();
  l->mutable_data()->pb = std::move(ns_entry);

  namespace_ids_map_[id] = ns;
  namespace_names_mapper_[db_type][l->mutable_data()->pb.name()] = ns;

  // Write to sys_catalog and in memory.
  RETURN_NOT_OK(sys_catalog_->AddItem(ns.get(), term));
  l->Commit();

  LOG_WITH_PREFIX(INFO) << "Created default keyspace: " << ns->ToString();
  return Status::OK();
}

Status CatalogManager::CheckLocalHostInMasterAddresses() {
  auto local_hostport = master_->first_rpc_address();
  std::vector<IpAddress> local_addrs;

  if (local_hostport.address().is_unspecified()) {
    auto status = GetLocalAddresses(&local_addrs, AddressFilter::ANY);
    if (!status.ok() || local_addrs.empty()) {
      LOG(WARNING) << "Could not enumerate network interfaces due to " << status << ", found "
                   << local_addrs.size() << " local addresses.";
      return Status::OK();
    }
  } else {
    local_addrs.push_back(local_hostport.address());
  }

  std::vector<Endpoint> resolved_addresses;
  Status s = server::ResolveMasterAddresses(master_->opts().GetMasterAddresses(),
                                            &resolved_addresses);
  RETURN_NOT_OK(s);

  for (auto const &addr : resolved_addresses) {
    if (addr.address().is_unspecified() ||
        std::find(local_addrs.begin(), local_addrs.end(), addr.address()) !=
            local_addrs.end()) {
      return Status::OK();
    }
  }
  return STATUS_SUBSTITUTE(IllegalState,
      "None of the local addresses are present in master_addresses $0.",
      master_->opts().master_addresses_flag);
}

Status CatalogManager::InitSysCatalogAsync() {
  std::lock_guard<LockType> l(lock_);

  // Optimistically try to load data from disk.
  Status s = sys_catalog_->Load(master_->fs_manager());

  if (!s.ok() && s.IsNotFound()) {
    // We have yet to intialize the syscatalog metadata, need to create the metadata file.
    LOG(INFO) << "Did not find previous SysCatalogTable data on disk. " << s;

    if (!master_->opts().AreMasterAddressesProvided()) {
      master_->SetShellMode(true);
      LOG(INFO) << "Starting master in shell mode.";
      return Status::OK();
    }

    RETURN_NOT_OK(CheckLocalHostInMasterAddresses());
    RETURN_NOT_OK_PREPEND(sys_catalog_->CreateNew(master_->fs_manager()),
        Substitute("Encountered errors during system catalog initialization:"
                   "\n\tError on Load: $0\n\tError on CreateNew: ", s.ToString()));

    return Status::OK();
  }

  return s;
}

bool CatalogManager::IsInitialized() const {
  std::lock_guard<simple_spinlock> l(state_lock_);
  return state_ == kRunning;
}

// TODO - delete this API after HandleReportedTablet() usage is removed.
Status CatalogManager::CheckIsLeaderAndReady() const {
  std::lock_guard<simple_spinlock> l(state_lock_);
  if (PREDICT_FALSE(state_ != kRunning)) {
    return STATUS_SUBSTITUTE(ServiceUnavailable,
        "Catalog manager is shutting down. State: $0", state_);
  }
  string uuid = master_->fs_manager()->uuid();
  if (master_->opts().IsShellMode()) {
    // Consensus and other internal fields should not be checked when is shell mode.
    return STATUS_SUBSTITUTE(IllegalState,
        "Catalog manager of $0 is in shell mode, not the leader", uuid);
  }
  Consensus* consensus = tablet_peer()->consensus();
  if (consensus == nullptr) {
    return STATUS(IllegalState, "Consensus has not been initialized yet");
  }
  ConsensusStatePB cstate = consensus->ConsensusState(CONSENSUS_CONFIG_COMMITTED);
  if (PREDICT_FALSE(!cstate.has_leader_uuid() || cstate.leader_uuid() != uuid)) {
    return STATUS_SUBSTITUTE(IllegalState,
        "Not the leader. Local UUID: $0, Consensus state: $1", uuid, cstate.ShortDebugString());
  }
  if (PREDICT_FALSE(leader_ready_term_ != cstate.current_term())) {
    return STATUS_SUBSTITUTE(ServiceUnavailable,
        "Leader not yet ready to serve requests: ready term $0 vs cstate term $1",
        leader_ready_term_, cstate.current_term());
  }
  return Status::OK();
}

const std::shared_ptr<tablet::TabletPeer> CatalogManager::tablet_peer() const {
  return sys_catalog_->tablet_peer();
}

RaftPeerPB::Role CatalogManager::Role() const {
  if (!IsInitialized() || master_->opts().IsShellMode()) {
    return RaftPeerPB::NON_PARTICIPANT;
  }

  return tablet_peer()->consensus()->role();
}

bool CatalogManager::StartShutdown() {
  {
    std::lock_guard<simple_spinlock> l(state_lock_);
    if (state_ == kClosing) {
      VLOG(2) << "CatalogManager already shut down";
      return false;
    }
    state_ = kClosing;
  }

  refresh_yql_partitions_task_.StartShutdown();

  if (sys_catalog_) {
    sys_catalog_->StartShutdown();
  }

  return true;
}

void CatalogManager::CompleteShutdown() {
  // Shutdown the Catalog Manager background thread (load balancing).
  refresh_yql_partitions_task_.CompleteShutdown();
  if (background_tasks_) {
    background_tasks_->Shutdown();
  }
  if (background_tasks_thread_pool_) {
    background_tasks_thread_pool_->Shutdown();
  }
  if (leader_initialization_pool_) {
    leader_initialization_pool_->Shutdown();
  }
  if (async_task_pool_) {
    async_task_pool_->Shutdown();
  }

  // Mark all outstanding table tasks as aborted and wait for them to fail.
  //
  // There may be an outstanding table visitor thread modifying the table map,
  // so we must make a copy of it before we iterate. It's OK if the visitor
  // adds more entries to the map even after we finish; it won't start any new
  // tasks for those entries.
  vector<scoped_refptr<TableInfo>> copy;
  {
    SharedLock<LockType> l(lock_);
    AppendValuesFromMap(*table_ids_map_, &copy);
  }
  AbortAndWaitForAllTasks(copy);

  // Shut down the underlying storage for tables and tablets.
  if (sys_catalog_) {
    sys_catalog_->CompleteShutdown();
  }

  // Reset the jobs/tasks tracker.
  tasks_tracker_->Reset();
  jobs_tracker_->Reset();

  if (initdb_future_ && initdb_future_->wait_for(0s) != std::future_status::ready) {
    LOG(WARNING) << "initdb is still running, waiting for it to complete.";
    initdb_future_->wait();
    LOG(INFO) << "Finished running initdb, proceeding with catalog manager shutdown.";
  }
}

Status CatalogManager::CheckOnline() const {
  State state;
  {
    std::lock_guard<simple_spinlock> l(state_lock_);
    state = state_;
  }

  if (PREDICT_FALSE(state == State::kClosing)) {
    return STATUS(ShutdownInProgress, "CatalogManager is shutting down");
  }
  if (PREDICT_FALSE(state != State::kRunning)) {
    return STATUS(ServiceUnavailable, "CatalogManager is not running");
  }
  return Status::OK();
}

Status CatalogManager::AbortTableCreation(TableInfo* table,
                                          const vector<TabletInfo*>& tablets,
                                          const Status& s,
                                          CreateTableResponsePB* resp) {
  LOG(WARNING) << s;

  const TableId table_id = table->id();
  const TableName table_name = table->mutable_metadata()->mutable_dirty()->pb.name();
  const NamespaceId table_namespace_id =
      table->mutable_metadata()->mutable_dirty()->pb.namespace_id();
  vector<string> tablet_ids_to_erase;
  for (TabletInfo* tablet : tablets) {
    tablet_ids_to_erase.push_back(tablet->tablet_id());
  }

  LOG(INFO) << "Aborting creation of table '" << table_name << "', erasing table and tablets (" <<
      JoinStrings(tablet_ids_to_erase, ",") << ") from in-memory state.";

  // Since this is a failed creation attempt, it's safe to just abort
  // all tasks, as (by definition) no tasks may be pending against a
  // table that has failed to successfully create.
  table->AbortTasksAndClose();
  table->WaitTasksCompletion();

  std::lock_guard<LockType> l(lock_);

  // Call AbortMutation() manually, as otherwise the lock won't be released.
  for (TabletInfo* tablet : tablets) {
    tablet->mutable_metadata()->AbortMutation();
  }
  table->mutable_metadata()->AbortMutation();
  auto tablet_map_checkout = tablet_map_.CheckOut();
  for (const TabletId& tablet_id_to_erase : tablet_ids_to_erase) {
    CHECK_EQ(tablet_map_checkout->erase(tablet_id_to_erase), 1)
        << "Unable to erase tablet " << tablet_id_to_erase << " from tablet map.";
  }

  auto table_ids_map_checkout = table_ids_map_.CheckOut();
  table_names_map_.erase({table_namespace_id, table_name}); // Not present if PGSQL table.
  CHECK_EQ(table_ids_map_checkout->erase(table_id), 1)
      << "Unable to erase table with id " << table_id << " from table ids map.";

  return CheckIfNoLongerLeaderAndSetupError(s, resp);
}

Result<ReplicationInfoPB> CatalogManager::ResolveReplicationInfo(
  const ReplicationInfoPB& table_replication_info) {

  if (IsReplicationInfoSet(table_replication_info)) {
      // The table has custom replication info set for it, return it if valid.
      RETURN_NOT_OK(ValidateTableReplicationInfo(table_replication_info));
      return table_replication_info;
  }

  // Table level replication info not set. Return cluster level
  // replication info.
  auto l = cluster_config_->LockForRead();
  return l->data().pb.replication_info();
}

bool CatalogManager::IsReplicationInfoSet(const ReplicationInfoPB& replication_info) {
  const auto& live_placement_info = replication_info.live_replicas();
  if (!(live_placement_info.placement_blocks().empty() &&
        live_placement_info.num_replicas() <= 0 &&
        live_placement_info.placement_uuid().empty()) ||
      !replication_info.read_replicas().empty() ||
      !replication_info.affinitized_leaders().empty()) {

      return true;
  }
  return false;
}

Status CatalogManager::ValidateTableReplicationInfo(const ReplicationInfoPB& replication_info) {
  if (!IsReplicationInfoSet(replication_info)) {
    return STATUS(InvalidArgument, "No replication info set.");
  }
  // We don't support setting any other fields other than live replica placements for now.
  if (!replication_info.read_replicas().empty() ||
      !replication_info.affinitized_leaders().empty()) {

      return STATUS(InvalidArgument, "Only live placement info can be set for table "
          "level replication info.");
  }
  // Today we support setting table level replication info only in clusters where read replica
  // placements is not set. Return error if the cluster has read replica placements set.
  auto l = cluster_config_->LockForRead();
  const ReplicationInfoPB& cluster_replication_info = l->data().pb.replication_info();
  // TODO(bogdan): figure this out when we expand on geopartition support.
  // if (!cluster_replication_info.read_replicas().empty() ||
  //     !cluster_replication_info.affinitized_leaders().empty()) {

  //     return STATUS(InvalidArgument, "Setting table level replication info is not supported "
  //         "for clusters with read replica placements");
  // }
  // If the replication info has placement_uuid set, verify that it matches the cluster
  // placement_uuid.
  if (replication_info.live_replicas().placement_uuid().empty()) {
    return Status::OK();
  }
  if (replication_info.live_replicas().placement_uuid() !=
      cluster_replication_info.live_replicas().placement_uuid()) {

      return STATUS(InvalidArgument, "Placement uuid for table level replication info "
          "must match that of the cluster's live placement info.");
  }
  return Status::OK();
}

Status CatalogManager::AddIndexInfoToTable(const scoped_refptr<TableInfo>& indexed_table,
                                           const IndexInfoPB& index_info,
                                           CreateTableResponsePB* resp) {
  LOG(INFO) << "AddIndexInfoToTable to " << indexed_table->ToString() << "  IndexInfo "
            << yb::ToString(index_info);
  TRACE("Locking indexed table");
  auto l = DCHECK_NOTNULL(indexed_table)->LockForWrite();
  RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));

  // Make sure that the index appears to not have been added to the table until the tservers apply
  // the alter and respond back.
  // Heed issue #6233.
  if (!l->data().pb.has_fully_applied_schema()) {
    MultiStageAlterTable::CopySchemaDetailsToFullyApplied(&l->mutable_data()->pb);
  }

  // Add index info to indexed table and increment schema version.
  l->mutable_data()->pb.add_indexes()->CopyFrom(index_info);
  l->mutable_data()->pb.set_version(l->mutable_data()->pb.version() + 1);
  l->mutable_data()->set_state(SysTablesEntryPB::ALTERING,
                               Substitute("Alter table version=$0 ts=$1",
                                          l->mutable_data()->pb.version(),
                                          LocalTimeAsString()));

  // Update sys-catalog with the new indexed table info.
  TRACE("Updating indexed table metadata on disk");
  RETURN_NOT_OK(sys_catalog_->UpdateItem(indexed_table.get(), leader_ready_term()));

  // Update the in-memory state.
  TRACE("Committing in-memory state");
  l->Commit();

  SendAlterTableRequest(indexed_table);

  return Status::OK();
}

Status CatalogManager::CreateCopartitionedTable(const CreateTableRequestPB& req,
                                                CreateTableResponsePB* resp,
                                                rpc::RpcContext* rpc,
                                                Schema schema,
                                                scoped_refptr<NamespaceInfo> ns) {
  scoped_refptr<TableInfo> parent_table_info;
  Status s;
  PartitionSchema partition_schema;
  std::vector<Partition> partitions;

  const NamespaceId& namespace_id = ns->id();
  const NamespaceName& namespace_name = ns->name();

  std::lock_guard<LockType> l(lock_);
  TRACE("Acquired catalog manager lock");
  parent_table_info = FindPtrOrNull(*table_ids_map_,
                                    schema.table_properties().CopartitionTableId());
  if (parent_table_info == nullptr) {
    s = STATUS(NotFound, "The object does not exist: copartitioned table with id",
               schema.table_properties().CopartitionTableId());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  scoped_refptr<TableInfo> this_table_info;
  std::vector<TabletInfo *> tablets;
  TabletInfos scoped_ref_tablets;
  // Verify that the table does not exist.
  this_table_info = FindPtrOrNull(table_names_map_, {namespace_id, req.name()});

  if (this_table_info != nullptr) {
    s = STATUS_SUBSTITUTE(AlreadyPresent,
        "Object '$0.$1' already exists",
        GetNamespaceNameUnlocked(this_table_info), this_table_info->name());
    LOG(WARNING) << "Found table: " << this_table_info->ToStringWithState()
                 << ". Failed creating copartitioned table with error: "
                 << s.ToString() << " Request:\n" << req.DebugString();
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_ALREADY_PRESENT, s);
  }
  // Don't add copartitioned tables to Namespaces that aren't running.
  if (ns->state() != SysNamespaceEntryPB::RUNNING) {
    Status s = STATUS_SUBSTITUTE(TryAgain,
        "Namespace not running (State=$0).  Cannot create $1.$2",
        ns->state(), ns->name(), req.name() );
    return SetupError(resp->mutable_error(), NamespaceMasterError(ns->state()), s);
  }

  // TODO: pass index_info for copartitioned index.
  RETURN_NOT_OK(CreateTableInMemory(
      req, schema, partition_schema, false /* create_tablets */, namespace_id, namespace_name,
      partitions, nullptr, nullptr, resp, &this_table_info));

  TRACE("Inserted new table info into CatalogManager maps");

  // NOTE: the table is already locked for write at this point,
  // since the CreateTableInfo function leave it in that state.
  // It will get committed at the end of this function.
  // Sanity check: the table should be in "preparing" state.
  CHECK_EQ(SysTablesEntryPB::PREPARING, this_table_info->metadata().dirty().pb.state());
  parent_table_info->GetAllTablets(&scoped_ref_tablets);
  for (auto tablet : scoped_ref_tablets) {
    tablets.push_back(tablet.get());
    tablet->mutable_metadata()->StartMutation();
    tablet->mutable_metadata()->mutable_dirty()->pb.add_table_ids(this_table_info->id());
  }

  // Update Tablets about new table id to sys-tablets.
  s = sys_catalog_->UpdateItems(tablets, leader_ready_term());
  if (PREDICT_FALSE(!s.ok())) {
    return AbortTableCreation(this_table_info.get(), tablets, s.CloneAndPrepend(
        Substitute("An error occurred while inserting to sys-tablets: $0", s.ToString())), resp);
  }
  TRACE("Wrote tablets to system table");

  // Update the on-disk table state to "running".
  this_table_info->AddTablets(tablets);
  this_table_info->mutable_metadata()->mutable_dirty()->pb.set_state(SysTablesEntryPB::RUNNING);
  s = sys_catalog_->AddItem(this_table_info.get(), leader_ready_term());
  if (PREDICT_FALSE(!s.ok())) {
    return AbortTableCreation(this_table_info.get(), tablets, s.CloneAndPrepend(
        Substitute("An error occurred while inserting to sys-tablets: $0",
                   s.ToString())), resp);
  }
  TRACE("Wrote table to system table");

  // Commit the in-memory state.
  this_table_info->mutable_metadata()->CommitMutation();

  for (TabletInfo *tablet : tablets) {
    tablet->mutable_metadata()->CommitMutation();
  }

  for (const auto& tablet : scoped_ref_tablets) {
    SendCopartitionTabletRequest(tablet, this_table_info);
  }

  LOG(INFO) << "Successfully created table " << this_table_info->ToString()
            << " per request from " << RequestorString(rpc);
  return Status::OK();
}

namespace {

Result<std::array<PartitionPB, kNumSplitParts>> CreateNewTabletsPartition(
    const TabletInfo& tablet_info, const std::string& split_partition_key) {
  const auto& source_partition = tablet_info.LockForRead()->data().pb.partition();

  if (source_partition.partition_key_start() == split_partition_key ||
      source_partition.partition_key_end() == split_partition_key) {
    return STATUS_FORMAT(
        InvalidArgument,
        "Can't split tablet $0 (partition_key_start: $1 partition_key_end: $2) by partition "
        "boundary (split_key: $3)",
        tablet_info.tablet_id(), source_partition.partition_key_start(),
        source_partition.partition_key_end(), split_partition_key);
  }

  std::array<PartitionPB, kNumSplitParts> new_tablets_partition;

  new_tablets_partition.fill(source_partition);

  new_tablets_partition[0].set_partition_key_end(split_partition_key);
  new_tablets_partition[1].set_partition_key_start(split_partition_key);
  static_assert(kNumSplitParts == 2, "We expect tablet to be split into 2 new tablets here");

  return new_tablets_partition;
}

}  // namespace

Status CatalogManager::TEST_SplitTablet(
    const scoped_refptr<TabletInfo>& source_tablet_info, docdb::DocKeyHash split_hash_code) {
  RETURN_NOT_OK(CheckOnline());
  return DoSplitTablet(source_tablet_info, split_hash_code);
}

Status CatalogManager::DoSplitTablet(
    const scoped_refptr<TabletInfo>& source_tablet_info, const std::string& split_encoded_key,
    const std::string& split_partition_key) {
  if (source_tablet_info->colocated()) {
    return STATUS_FORMAT(
        NotSupported, "Tablet splitting is not supported for colocated tables, tablet_id: $0",
        source_tablet_info->tablet_id());
  }
  if (FLAGS_tablet_split_limit_per_table != 0 &&
      source_tablet_info->table()->NumTablets() >= FLAGS_tablet_split_limit_per_table) {
    // TODO(tsplit): Avoid tablet server of scanning tablets for the tables that already
    //  reached the split limit of tablet #6220
    return STATUS_EC_FORMAT(IllegalState, MasterError(MasterErrorPB::REACHED_SPLIT_LIMIT),
                            "Too many tablets for the table, table_id: $0, limit: $1",
                            source_tablet_info->table()->id(), FLAGS_tablet_split_limit_per_table);
  }

  LOG(INFO) << "Got tablet to split: " << source_tablet_info->ToString();

  const auto source_table_lock = source_tablet_info->table()->LockForWrite();
  const auto source_tablet_lock = source_tablet_info->LockForWrite();

  std::array<PartitionPB, kNumSplitParts> new_tablets_partition = VERIFY_RESULT(
      CreateNewTabletsPartition(*source_tablet_info, split_partition_key));

  std::array<TabletId, kNumSplitParts> new_tablet_ids;
  for (int i = 0; i < kNumSplitParts; ++i) {
    if (i < source_tablet_lock->data().pb.split_tablet_ids_size()) {
      // Post-split tablet `i` has been already registered.
      new_tablet_ids[i] = source_tablet_lock->data().pb.split_tablet_ids(i);
    } else {
      auto* new_tablet_info = VERIFY_RESULT(RegisterNewTabletForSplit(
          source_tablet_info.get(), new_tablets_partition[i], source_table_lock.get()));

      new_tablet_ids[i] = new_tablet_info->id();
      source_tablet_lock->mutable_data()->pb.add_split_tablet_ids(new_tablet_info->id());
    }
  }
  source_tablet_lock->Commit();
  source_table_lock->Commit();

  // TODO(tsplit): what if source tablet will be deleted before or during TS leader is processing
  // split? Add unit-test.
  SendSplitTabletRequest(
      source_tablet_info, new_tablet_ids, split_encoded_key, split_partition_key);

  return Status::OK();
}

Status CatalogManager::DoSplitTablet(
    const scoped_refptr<TabletInfo>& source_tablet_info, docdb::DocKeyHash split_hash_code) {
  docdb::KeyBytes split_encoded_key;
  docdb::DocKeyEncoderAfterTableIdStep(&split_encoded_key)
      .Hash(split_hash_code, std::vector<docdb::PrimitiveValue>());

  const auto split_partition_key = PartitionSchema::EncodeMultiColumnHashValue(split_hash_code);

  return DoSplitTablet(source_tablet_info, split_encoded_key.ToStringBuffer(), split_partition_key);
}

Result<scoped_refptr<TabletInfo>> CatalogManager::GetTabletInfo(const TabletId& tablet_id) {
  RETURN_NOT_OK(CheckOnline());

  std::lock_guard<LockType> l(lock_);
  TRACE("Acquired catalog manager lock");

  const auto tablet_info = FindPtrOrNull(*tablet_map_, tablet_id);
  SCHECK(tablet_info != nullptr, NotFound, Format("Tablet $0 not found", tablet_id));

  return tablet_info;
}

Status CatalogManager::SplitTablet(
    const TabletId& tablet_id, const std::string& split_encoded_key,
    const std::string& split_partition_key) {
  const auto source_tablet_info = VERIFY_RESULT(GetTabletInfo(tablet_id));

  return DoSplitTablet(source_tablet_info, split_encoded_key, split_partition_key);
}

Status CatalogManager::SplitTablet(
    const SplitTabletRequestPB* req, SplitTabletResponsePB* resp, rpc::RpcContext* rpc) {
  RETURN_NOT_OK(CheckOnline());

  const auto source_tablet_id = req->tablet_id();
  const auto source_tablet_info = VERIFY_RESULT(GetTabletInfo(source_tablet_id));
  const auto source_partition = source_tablet_info->LockForRead()->data().pb.partition();

  const auto start_hash_code = source_partition.partition_key_start().empty()
      ? 0
      : PartitionSchema::DecodeMultiColumnHashValue(source_partition.partition_key_start());

  const auto end_hash_code = source_partition.partition_key_end().empty()
      ? std::numeric_limits<docdb::DocKeyHash>::max()
      : PartitionSchema::DecodeMultiColumnHashValue(source_partition.partition_key_end());

  const auto split_hash_code = (start_hash_code + end_hash_code) / 2;

  return DoSplitTablet(source_tablet_info, split_hash_code);
}

Status CatalogManager::DeleteTablet(
    const DeleteTabletRequestPB* req, DeleteTabletResponsePB* resp, rpc::RpcContext* rpc) {
  RETURN_NOT_OK(CheckOnline());

  const auto& tablet_id = req->tablet_id();
  const auto tablet_info = VERIFY_RESULT(GetTabletInfo(tablet_id));

  const auto& table_info = tablet_info->table();

  RETURN_NOT_OK(CheckIfForbiddenToDeleteTabletOf(table_info));

  RETURN_NOT_OK(CatalogManagerUtil::CheckIfCanDeleteSingleTablet(tablet_info));

  DeleteTabletAndSendRequests(tablet_info, "Tablet deleted upon request at " + LocalTimeAsString());
  return Status::OK();
}

namespace {

CHECKED_STATUS ValidateCreateTableSchema(const Schema& schema, CreateTableResponsePB* resp) {
  if (schema.num_key_columns() <= 0) {
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA,
                      STATUS(InvalidArgument, "Must specify at least one key column"));
  }
  for (int i = 0; i < schema.num_key_columns(); i++) {
    if (!IsTypeAllowableInKey(schema.column(i).type_info())) {
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA,
                        STATUS(InvalidArgument, "Invalid datatype for primary key column"));
    }
  }
  return Status::OK();
}

}  // namespace

Status CatalogManager::CreatePgsqlSysTable(const CreateTableRequestPB* req,
                                           CreateTableResponsePB* resp) {
  LOG(INFO) << "CreatePgsqlSysTable: " << req->name();
  // Lookup the namespace and verify if it exists.
  TRACE("Looking up namespace");
  scoped_refptr<NamespaceInfo> ns;
  RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->namespace_(), &ns), resp);
  const NamespaceId& namespace_id = ns->id();
  const NamespaceName& namespace_name = ns->name();

  Schema schema;
  RETURN_NOT_OK(SchemaFromPB(req->schema(), &schema));
  // If the schema contains column ids, we are copying a Postgres table from one namespace to
  // another. Anyway, validate the schema.
  RETURN_NOT_OK(ValidateCreateTableSchema(schema, resp));
  if (!schema.has_column_ids()) {
    schema.InitColumnIdsByDefault();
  }
  schema.mutable_table_properties()->set_is_ysql_catalog_table(true);

  // Verify no hash partition schema is specified.
  if (req->partition_schema().has_hash_schema()) {
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA,
                      STATUS(InvalidArgument,
                             "PostgreSQL system catalog tables are non-partitioned"));
  }

  if (req->table_type() != TableType::PGSQL_TABLE_TYPE) {
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA,
                      STATUS_FORMAT(
                          InvalidArgument,
                          "Expected table type to be PGSQL_TABLE_TYPE ($0), got $1 ($2)",
                          PGSQL_TABLE_TYPE,
                          TableType_Name(req->table_type())));

  }

  // Create partition schema and one partition.
  PartitionSchema partition_schema;
  vector<Partition> partitions;
  RETURN_NOT_OK(partition_schema.CreatePartitions(1, &partitions));

  // Create table info in memory.
  scoped_refptr<TableInfo> table;
  vector<TabletInfo*> tablets;
  scoped_refptr<TabletInfo> sys_catalog_tablet;
  {
    std::lock_guard<LockType> l(lock_);
    TRACE("Acquired catalog manager lock");

    // Verify that the table does not exist.
    table = FindPtrOrNull(table_names_map_, {namespace_id, req->name()});
    if (table != nullptr) {
      Status s = STATUS_SUBSTITUTE(AlreadyPresent,
          "Object '$0.$1' already exists", ns->name(), table->name());
      LOG(WARNING) << "Found table: " << table->ToStringWithState()
                   << ". Failed creating PostgreSQL system table with error: "
                   << s.ToString() << " Request:\n" << req->DebugString();
      return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_ALREADY_PRESENT, s);
    }

    RETURN_NOT_OK(CreateTableInMemory(
        *req, schema, partition_schema, false /* create_tablets */, namespace_id, namespace_name,
        partitions, nullptr /* index_info */, nullptr /* tablets */, resp, &table));

    sys_catalog_tablet = tablet_map_->find(kSysCatalogTabletId)->second;
  }
  {
    auto tablet_lock = sys_catalog_tablet->LockForWrite();
    tablet_lock->mutable_data()->pb.add_table_ids(table->id());

    Status s = sys_catalog_->UpdateItem(sys_catalog_tablet.get(), leader_ready_term());
    if (PREDICT_FALSE(!s.ok())) {
      return AbortTableCreation(table.get(), tablets, s.CloneAndPrepend(
        "An error occurred while inserting to sys-tablets: "), resp);
    }
    table->set_is_system();
    table->AddTablet(sys_catalog_tablet.get());
    tablet_lock->Commit();
  }
  TRACE("Inserted new table info into CatalogManager maps");

  // Update the on-disk table state to "running".
  table->mutable_metadata()->mutable_dirty()->pb.set_state(SysTablesEntryPB::RUNNING);
  Status s = sys_catalog_->AddItem(table.get(), leader_ready_term());
  if (PREDICT_FALSE(!s.ok())) {
    return AbortTableCreation(table.get(), tablets, s.CloneAndPrepend(
      "An error occurred while inserting to sys-tablets: "), resp);
  }
  TRACE("Wrote table to system table");

  // Commit the in-memory state.
  table->mutable_metadata()->CommitMutation();

  tserver::ChangeMetadataRequestPB change_req;
  change_req.set_tablet_id(kSysCatalogTabletId);
  auto& add_table = *change_req.mutable_add_table();

  add_table.set_table_id(req->table_id());
  add_table.set_table_type(TableType::PGSQL_TABLE_TYPE);
  add_table.set_table_name(req->name());
  SchemaToPB(schema, add_table.mutable_schema());
  add_table.set_schema_version(0);

  partition_schema.ToPB(add_table.mutable_partition_schema());

  RETURN_NOT_OK(tablet::SyncReplicateChangeMetadataOperation(
      &change_req, sys_catalog_->tablet_peer().get(), leader_ready_term()));

  if (initial_snapshot_writer_) {
    initial_snapshot_writer_->AddMetadataChange(change_req);
  }
  return Status::OK();
}

Status CatalogManager::ReservePgsqlOids(const ReservePgsqlOidsRequestPB* req,
                                        ReservePgsqlOidsResponsePB* resp,
                                        rpc::RpcContext* rpc) {
  RETURN_NOT_OK(CheckOnline());

  VLOG(1) << "ReservePgsqlOids request: " << req->ShortDebugString();

  // Lookup namespace
  scoped_refptr<NamespaceInfo> ns;
  {
    SharedLock<LockType> l(lock_);
    ns = FindPtrOrNull(namespace_ids_map_, req->namespace_id());
  }
  if (!ns) {
    return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND,
                      STATUS(NotFound, "Namespace not found", req->namespace_id()));
  }

  // Reserve oids.
  auto l = ns->LockForWrite();

  uint32_t begin_oid = l->data().pb.next_pg_oid();
  if (begin_oid < req->next_oid()) {
    begin_oid = req->next_oid();
  }
  if (begin_oid == std::numeric_limits<uint32_t>::max()) {
    LOG(WARNING) << Format("No more object identifier is available for Postgres database $0 ($1)",
                           l->data().pb.name(), req->namespace_id());
    return SetupError(resp->mutable_error(), MasterErrorPB::UNKNOWN_ERROR,
                      STATUS(InvalidArgument, "No more object identifier is available"));
  }

  uint32_t end_oid = begin_oid + req->count();
  if (end_oid < begin_oid) {
    end_oid = std::numeric_limits<uint32_t>::max(); // Handle wraparound.
  }

  resp->set_begin_oid(begin_oid);
  resp->set_end_oid(end_oid);
  l->mutable_data()->pb.set_next_pg_oid(end_oid);

  // Update the on-disk state.
  const Status s = sys_catalog_->UpdateItem(ns.get(), leader_ready_term());
  if (!s.ok()) {
    return SetupError(resp->mutable_error(), MasterErrorPB::UNKNOWN_ERROR, s);
  }

  // Commit the in-memory state.
  l->Commit();

  VLOG(1) << "ReservePgsqlOids response: " << resp->ShortDebugString();

  return Status::OK();
}

Status CatalogManager::GetYsqlCatalogConfig(const GetYsqlCatalogConfigRequestPB* req,
                                            GetYsqlCatalogConfigResponsePB* resp,
                                            rpc::RpcContext* rpc) {
  RETURN_NOT_OK(CheckOnline());
  VLOG(1) << "GetYsqlCatalogConfig request: " << req->ShortDebugString();
  auto l = CHECK_NOTNULL(ysql_catalog_config_.get())->LockForRead();
  resp->set_version(l->data().pb.ysql_catalog_config().version());

  return Status::OK();
}

Status CatalogManager::CopyPgsqlSysTables(const NamespaceId& namespace_id,
                                          const std::vector<scoped_refptr<TableInfo>>& tables) {
  const uint32_t database_oid = CHECK_RESULT(GetPgsqlDatabaseOid(namespace_id));
  vector<TableId> source_table_ids;
  vector<TableId> target_table_ids;
  for (const auto& table : tables) {
    CreateTableRequestPB table_req;
    CreateTableResponsePB table_resp;

    const uint32_t table_oid = VERIFY_RESULT(GetPgsqlTableOid(table->id()));
    const TableId table_id = GetPgsqlTableId(database_oid, table_oid);

    // Hold read lock until rows from the table are copied also.
    auto l = table->LockForRead();

    // Skip shared table.
    if (l->data().pb.is_pg_shared_table()) {
      continue;
    }

    table_req.set_name(l->data().pb.name());
    table_req.mutable_namespace_()->set_id(namespace_id);
    table_req.set_table_type(PGSQL_TABLE_TYPE);
    table_req.mutable_schema()->CopyFrom(l->data().schema());
    table_req.set_is_pg_catalog_table(true);
    table_req.set_table_id(table_id);

    if (PROTO_IS_INDEX(l->data().pb)) {
      const uint32_t indexed_table_oid =
        VERIFY_RESULT(GetPgsqlTableOid(PROTO_GET_INDEXED_TABLE_ID(l->data().pb)));
      const TableId indexed_table_id = GetPgsqlTableId(database_oid, indexed_table_oid);

      // Set index_info.
      // Previously created INDEX wouldn't have the attribute index_info.
      if (l->data().pb.has_index_info()) {
        table_req.mutable_index_info()->CopyFrom(l->data().pb.index_info());
        table_req.mutable_index_info()->set_indexed_table_id(indexed_table_id);
      }

      // Set deprecated field for index_info.
      table_req.set_indexed_table_id(indexed_table_id);
      table_req.set_is_local_index(PROTO_GET_IS_LOCAL(l->data().pb));
      table_req.set_is_unique_index(PROTO_GET_IS_UNIQUE(l->data().pb));
    }

    auto s = CreatePgsqlSysTable(&table_req, &table_resp);
    if (!s.ok()) {
      return s.CloneAndPrepend(Substitute(
          "Failure when creating PGSQL System Tables: $0", table_resp.error().ShortDebugString()));
    }

    source_table_ids.push_back(table->id());
    target_table_ids.push_back(table_id);
  }
  RETURN_NOT_OK(
      sys_catalog_->CopyPgsqlTables(source_table_ids, target_table_ids, leader_ready_term()));
  return Status::OK();
}

// Create a new table.
// See README file in this directory for a description of the design.
Status CatalogManager::CreateTable(const CreateTableRequestPB* orig_req,
                                   CreateTableResponsePB* resp,
                                   rpc::RpcContext* rpc) {
  DVLOG(3) << __PRETTY_FUNCTION__ << " Begin. " << orig_req->DebugString();
  RETURN_NOT_OK(CheckOnline());

  const bool is_pg_table = orig_req->table_type() == PGSQL_TABLE_TYPE;
  const bool is_pg_catalog_table = is_pg_table && orig_req->is_pg_catalog_table();
  if (!is_pg_catalog_table || !FLAGS_hide_pg_catalog_table_creation_logs) {
    LOG(INFO) << "CreateTable from " << RequestorString(rpc)
                << ":\n" << orig_req->DebugString();
  } else {
    LOG(INFO) << "CreateTable from " << RequestorString(rpc) << ": " << orig_req->name();
  }

  const bool is_transactional = orig_req->schema().table_properties().is_transactional();
  // If this is a transactional table, we need to create the transaction status table (if it does
  // not exist already).
  if (is_transactional && (!is_pg_catalog_table || !FLAGS_create_initial_sys_catalog_snapshot)) {
    Status s = CreateTransactionsStatusTableIfNeeded(rpc);
    if (!s.ok()) {
      return s.CloneAndPrepend("Error while creating transaction status table");
    }
  } else {
    VLOG(1)
        << "Not attempting to create a transaction status table:\n"
        << "  " << EXPR_VALUE_FOR_LOG(is_transactional) << "\n "
        << "  " << EXPR_VALUE_FOR_LOG(is_pg_catalog_table) << "\n "
        << "  " << EXPR_VALUE_FOR_LOG(FLAGS_create_initial_sys_catalog_snapshot);
  }

  if (is_pg_catalog_table) {
    return CreatePgsqlSysTable(orig_req, resp);
  }

  Status s;
  const char* const object_type = PROTO_PTR_IS_TABLE(orig_req) ? "table" : "index";

  // Copy the request, so we can fill in some defaults.
  CreateTableRequestPB req = *orig_req;

  // Lookup the namespace and verify if it exists.
  TRACE("Looking up namespace");
  scoped_refptr<NamespaceInfo> ns;
  RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req.namespace_(), &ns), resp);
  auto ns_lock = ns->LockForRead();
  if (ns->database_type() != GetDatabaseTypeForTable(req.table_type())) {
    Status s = STATUS(NotFound, "Namespace not found");
    return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
  }
  const NamespaceId& namespace_id = ns->id();
  const NamespaceName& namespace_name = ns->name();

  // For index table, find the table info
  scoped_refptr<TableInfo> indexed_table;
  if (PROTO_IS_INDEX(req)) {
    TRACE("Looking up indexed table");
    indexed_table = GetTableInfo(req.indexed_table_id());
    if (indexed_table == nullptr) {
      return STATUS_SUBSTITUTE(
            NotFound, "The indexed table $0 does not exist", req.indexed_table_id());
    }

    TRACE("Locking indexed table");
    auto l_indexed_tbl = indexed_table->LockForRead();
    RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l_indexed_tbl.get(), resp));
  }

  // Determine if this table should be colocated. If not specified, the table should be colocated if
  // and only if the namespace is colocated.
  bool colocated = ns->colocated();
  if (!req.colocated()) {
    // Opt out of colocation if the request says so.
    colocated = false;
  } else if (indexed_table && !indexed_table->colocated()) {
    // Opt out of colocation if the indexed table opted out of colocation.
    colocated = false;
  }

  // TODO: If this is a colocated index table in a colocated database, convert any hash partition
  // columns into range partition columns. This is because postgres does not know that this index
  // table is in a colocated database. When we get to the "tablespaces" step where we store this
  // into PG metadata, then PG will know if db/table is colocated and do the work there.
  if ((colocated || req.has_tablegroup_id()) && PROTO_IS_INDEX(req)) {
    for (auto& col_pb : *req.mutable_schema()->mutable_columns()) {
      col_pb.set_is_hash_key(false);
    }
  }

  // Validate schema.
  Schema schema;
  RETURN_NOT_OK(SchemaFromPB(req.schema(), &schema));
  RETURN_NOT_OK(ValidateCreateTableSchema(schema, resp));

  // checking that referenced user-defined types (if any) exist.
  {
    SharedLock<LockType> l(lock_);
    for (int i = 0; i < schema.num_columns(); i++) {
      for (const auto &udt_id : schema.column(i).type()->GetUserDefinedTypeIds()) {
        if (FindPtrOrNull(udtype_ids_map_, udt_id) == nullptr) {
          Status s = STATUS(InvalidArgument, "Referenced user-defined type not found");
          return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
        }
      }
    }
  }
  // TODO (ENG-1860) The referenced namespace and types retrieved/checked above could be deleted
  // some time between this point and table creation below.

  // Usually the column ids are available if it's called on the backup-restoring code path
  // (from CatalogManager::RecreateTable). Else the column ids must be empty in the client schema.
  if (!schema.has_column_ids()) {
    schema.InitColumnIdsByDefault();
  }

  if (schema.table_properties().HasCopartitionTableId()) {
    return CreateCopartitionedTable(req, resp, rpc, schema, ns);
  }

  if (colocated || req.has_tablegroup_id()) {
    // If the table is colocated, then there should be no hash partition columns.
    // Do the same for tables that are being placed in tablegroups.
    if (schema.num_hash_key_columns() > 0) {
      Status s = STATUS(InvalidArgument, "Cannot colocate hash partitioned table");
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
    }
  } else if (
      !req.partition_schema().has_hash_schema() && !req.partition_schema().has_range_schema()) {
    // If neither hash nor range schema have been specified by the protobuf request, we assume the
    // table uses a hash schema, and we use the table_type and hash_key to determine the hashing
    // scheme (redis or multi-column) that should be used.
    if (req.table_type() == REDIS_TABLE_TYPE) {
      req.mutable_partition_schema()->set_hash_schema(PartitionSchemaPB::REDIS_HASH_SCHEMA);
    } else if (schema.num_hash_key_columns() > 0) {
      req.mutable_partition_schema()->set_hash_schema(PartitionSchemaPB::MULTI_COLUMN_HASH_SCHEMA);
    } else {
      Status s = STATUS(InvalidArgument, "Unknown table type or partitioning method");
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
    }
  }

  // Verify that custom placement policy has not been specified for colocated table.
  if (colocated && IsReplicationInfoSet(req.replication_info())) {
    Status s = STATUS(InvalidArgument, "Custom placement policy should not be set for "
      "colocated tables");
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_TABLE_REPLICATION_INFO, s);
  }

  // Get placement info.
  const ReplicationInfoPB& replication_info = VERIFY_RESULT(
  ResolveReplicationInfo(req.replication_info()));

  // Calculate number of tablets to be used.
  int num_tablets = req.schema().table_properties().num_tablets();
  if (num_tablets <= 0) {
    num_tablets = req.num_tablets();
  }

  if (num_tablets <= 0) {
    SharedLock<LockType> l(blacklist_lock_);
    // Use default as client could have gotten the value before any tserver had heartbeated
    // to (a new) master leader.
    TSDescriptorVector ts_descs;
    master_->ts_manager()->GetAllLiveDescriptorsInCluster(
        &ts_descs, replication_info.live_replicas().placement_uuid(), blacklistState.tservers_);
    num_tablets = ts_descs.size() * (is_pg_table ? FLAGS_ysql_num_shards_per_tserver
                                                 : FLAGS_yb_num_shards_per_tserver);
    LOG(INFO) << "Setting default tablets to " << num_tablets << " with "
              << ts_descs.size() << " primary servers";
  }
  schema.mutable_table_properties()->SetNumTablets(num_tablets);

  // Create partitions.
  PartitionSchema partition_schema;
  vector<Partition> partitions;
  if (colocated || req.has_tablegroup_id()) {
    RETURN_NOT_OK(partition_schema.CreatePartitions(1, &partitions));
    req.clear_partition_schema();
    req.set_num_tablets(1);
  } else {
    s = PartitionSchema::FromPB(req.partition_schema(), schema, &partition_schema);
    RETURN_NOT_OK(partition_schema.CreatePartitions(num_tablets, &partitions));
  }

  // For index table, populate the index info.
  IndexInfoPB index_info;

  const bool index_backfill_enabled =
      IsIndexBackfillEnabled(orig_req->table_type(), is_transactional);
  if (req.has_index_info()) {
    // Current message format.
    index_info.CopyFrom(req.index_info());

    // Assign column-ids that have just been computed and assigned to "index_info".
    if (!is_pg_table) {
      DCHECK_EQ(index_info.columns().size(), schema.num_columns())
        << "Number of columns are not the same between index_info and index_schema";
      // int colidx = 0;
      for (int colidx = 0; colidx < schema.num_columns(); colidx++) {
        index_info.mutable_columns(colidx)->set_column_id(schema.column_id(colidx));
      }
    }
  } else if (req.has_indexed_table_id()) {
    // Old client message format when rolling upgrade (Not having "index_info").
    IndexInfoBuilder index_info_builder(&index_info);
    index_info_builder.ApplyProperties(req.indexed_table_id(),
        req.is_local_index(), req.is_unique_index());
    if (orig_req->table_type() != PGSQL_TABLE_TYPE) {
      Schema indexed_schema;
      RETURN_NOT_OK(indexed_table->GetSchema(&indexed_schema));
      RETURN_NOT_OK(index_info_builder.ApplyColumnMapping(indexed_schema, schema));
    }
  }

  if ((req.has_index_info() || req.has_indexed_table_id()) &&
      index_backfill_enabled &&
      !req.skip_index_backfill()) {
    // Start off the index table with major compactions disabled. We need this to retain the delete
    // markers until the backfill process is completed.  No need to set index_permissions in the
    // index table.
    schema.SetRetainDeleteMarkers(true);
  }

  LOG(INFO) << "CreateTable with IndexInfo " << yb::ToString(index_info);
  TSDescriptorVector all_ts_descs;
  master_->ts_manager()->GetAllLiveDescriptors(&all_ts_descs);
  s = CheckValidReplicationInfo(replication_info, all_ts_descs, partitions, resp);
  if (!s.ok()) {
    return s;
  }

  scoped_refptr<TableInfo> table;
  vector<TabletInfo*> tablets;
  bool tablets_exist;
  bool tablegroup_tablets_exist = false;

  {
    std::lock_guard<LockType> l(lock_);
    auto ns_lock = ns->LockForRead();
    TRACE("Acquired catalog manager lock");

    tablets_exist =
        colocated && colocated_tablet_ids_map_.find(ns->id()) != colocated_tablet_ids_map_.end();
    // Verify that the table does not exist.
    table = FindPtrOrNull(table_names_map_, {namespace_id, req.name()});

    if (table != nullptr) {
      s = STATUS_SUBSTITUTE(AlreadyPresent,
              "Object '$0.$1' already exists", ns->name(), table->name());
      LOG(WARNING) << "Found table: " << table->ToStringWithState()
                   << ". Failed creating table with error: "
                   << s.ToString() << " Request:\n" << orig_req->DebugString();
      // If the table already exists, we set the response table_id field to the id of the table that
      // already exists. This is necessary because before we return the error to the client (or
      // success in case of a "CREATE TABLE IF NOT EXISTS" request) we want to wait for the existing
      // table to be available to receive requests. And we need the table id for that.
      resp->set_table_id(table->id());
      return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_ALREADY_PRESENT, s);
    }

    // Namespace state validity check:
    // 1. Allow Namespaces that are RUNNING
    // 2. Allow Namespaces that are PREPARING under 2 situations
    //    2a. System Namespaces.
    //    2b. The parent table from a Colocated Namespace.
    const auto parent_table_name = ns->id() + kColocatedParentTableNameSuffix;
    bool valid_ns_state = (ns->state() == SysNamespaceEntryPB::RUNNING) ||
      (ns->state() == SysNamespaceEntryPB::PREPARING &&
        (ns->name() == kSystemNamespaceName || req.name() == parent_table_name));
    if (!valid_ns_state) {
      Status s = STATUS_SUBSTITUTE(TryAgain, "Invalid Namespace State ($0).  Cannot create $1.$2",
          SysNamespaceEntryPB::State_Name(ns->state()), ns->name(), req.name() );
      return SetupError(resp->mutable_error(), NamespaceMasterError(ns->state()), s);
    }

    // Check whether this CREATE TABLE request which has a tablegroup_id is for a normal user table
    // or the request to create the parent table for the tablegroup. This is done by checking the
    // catalog manager maps.
    if (req.has_tablegroup_id() &&
        tablegroup_tablet_ids_map_.find(ns->id()) != tablegroup_tablet_ids_map_.end() &&
        tablegroup_tablet_ids_map_[ns->id()].find(req.tablegroup_id()) !=
        tablegroup_tablet_ids_map_[ns->id()].end()) {
      tablegroup_tablets_exist = true;
    }

    RETURN_NOT_OK(CreateTableInMemory(
        req, schema, partition_schema,
        !tablets_exist && !tablegroup_tablets_exist /* create_tablets */, namespace_id,
        namespace_name, partitions, &index_info, &tablets, resp, &table));

    // Section is executed when a table is either the parent table or a user table in a tablegroup.
    // It additionally sets the table metadata (and tablet metadata if this is the parent table)
    // to have the colocated property so we can take advantage of code reuse.
    if (req.has_tablegroup_id()) {
      table->mutable_metadata()->mutable_dirty()->pb.set_colocated(true);
      if (tablegroup_tablets_exist) {
        // If the table is not a tablegroup parent table, it performs a lookup for the proper tablet
        // to place the table on as a child table.
        scoped_refptr<TabletInfo> tablet =
            tablegroup_tablet_ids_map_[ns->id()][req.tablegroup_id()];
        RSTATUS_DCHECK(
            tablet->colocated(), InternalError,
            "The tablet for tablegroup should be colocated.");
        tablets.push_back(tablet.get());
        auto tablet_lock = tablet->LockForWrite();
        tablet_lock->mutable_data()->pb.add_table_ids(table->id());
        RETURN_NOT_OK(sys_catalog_->UpdateItem(tablet.get(), leader_ready_term()));
        tablet_lock->Commit();

        tablet->mutable_metadata()->StartMutation();
        table->AddTablets(tablets);
        tablegroup_ids_map_[req.tablegroup_id()]->AddChildTable(table->id());
      } else {
        // If the table is a tablegroup parent table, it creates a dummy tablet for the tablegroup
        // along with updating the catalog manager maps.
        RSTATUS_DCHECK_EQ(
            tablets.size(), 1, InternalError,
            "Only one tablet should be created for each tablegroup");
        tablets[0]->mutable_metadata()->mutable_dirty()->pb.set_colocated(true);
        // Update catalog manager maps for tablegroups
        tablegroup_tablet_ids_map_[ns->id()][req.tablegroup_id()] =
            tablet_map_->find(tablets[0]->id())->second;
      }
    } else if (colocated) {
      table->mutable_metadata()->mutable_dirty()->pb.set_colocated(true);
      // if the tablet already exists, add the tablet to tablets
      if (tablets_exist) {
        scoped_refptr<TabletInfo> tablet = colocated_tablet_ids_map_[ns->id()];
        RSTATUS_DCHECK(
            tablet->colocated(), InternalError,
            "The tablet for colocated database should be colocated.");
        tablets.push_back(tablet.get());
        auto tablet_lock = tablet->LockForWrite();
        tablet_lock->mutable_data()->pb.add_table_ids(table->id());
        RETURN_NOT_OK(sys_catalog_->UpdateItem(tablet.get(), leader_ready_term()));
        tablet_lock->Commit();

        tablet->mutable_metadata()->StartMutation();
        table->AddTablets(tablets);
      } else {  // Record the tablet
        RSTATUS_DCHECK_EQ(
            tablets.size(), 1, InternalError,
            "Only one tablet should be created for each colocated database");
        tablets[0]->mutable_metadata()->mutable_dirty()->pb.set_colocated(true);
        colocated_tablet_ids_map_[ns->id()] = tablet_map_->find(tablets[0]->id())->second;
      }
    }
  }

  // Tables with a transaction should be rolled back if the transaction does not get committed.
  // Store this on the table persistent state until the transaction has been a verified success.
  TransactionMetadata txn;
  if (req.has_transaction() && FLAGS_enable_transactional_ddl_gc) {
    table->mutable_metadata()->mutable_dirty()->pb.mutable_transaction()->
        CopyFrom(req.transaction());
    txn = VERIFY_RESULT(TransactionMetadata::FromPB(req.transaction()));
    RSTATUS_DCHECK(!txn.status_tablet.empty(), Corruption, "Given incomplete Transaction");
  }

  if (PREDICT_FALSE(FLAGS_TEST_simulate_slow_table_create_secs > 0) &&
      req.table_type() != TableType::TRANSACTION_STATUS_TABLE_TYPE) {
    LOG(INFO) << "Simulating slow table creation";
    SleepFor(MonoDelta::FromSeconds(FLAGS_TEST_simulate_slow_table_create_secs));
  }

  // NOTE: the table and tablets are already locked for write at this point,
  // since the CreateTableInfo/CreateTabletInfo functions leave them in that state.
  // They will get committed at the end of this function.
  // Sanity check: the tables and tablets should all be in "preparing" state.
  CHECK_EQ(SysTablesEntryPB::PREPARING, table->metadata().dirty().pb.state());
  if (tablets_exist || tablegroup_tablets_exist) {
    TRACE("Inserted new table and updating tablet info into CatalogManager maps");
    VLOG(1) << "Inserted new table and updating tablet info into "
               "CatalogManager maps";
    s = sys_catalog_->UpdateItems(tablets, leader_ready_term());
  } else {
    TRACE("Inserted new table and tablet info into CatalogManager maps");
    VLOG(1) << "Inserted new table and tablet info into CatalogManager maps";
    for (const TabletInfo *tablet : tablets) {
      CHECK_EQ(SysTabletsEntryPB::PREPARING, tablet->metadata().dirty().pb.state());
    }
    // Write Tablets to sys-tablets (in "preparing" state).
    s = sys_catalog_->AddItems(tablets, leader_ready_term());
  }

  if (PREDICT_FALSE(!s.ok())) {
    return AbortTableCreation(table.get(), tablets,
                              s.CloneAndPrepend(
                                  Substitute("An error occurred while inserting to sys-tablets: $0",
                                             s.ToString())),
                              resp);
  }
  TRACE("Wrote tablets to system table");

  // Update the on-disk table state to "running".
  table->mutable_metadata()->mutable_dirty()->pb.set_state(SysTablesEntryPB::RUNNING);
  s = sys_catalog_->AddItem(table.get(), leader_ready_term());
  if (PREDICT_FALSE(!s.ok())) {
    return AbortTableCreation(table.get(), tablets,
                              s.CloneAndPrepend(
                                  Substitute("An error occurred while inserting to sys-tablets: $0",
                                             s.ToString())),
                              resp);
  }
  TRACE("Wrote table to system table");

  // For index table, insert index info in the indexed table.  However, for backwards compatibility,
  // don't insert index info for YSQL tables when index backfill is disabled.
  if ((req.has_index_info() || req.has_indexed_table_id()) &&
      (index_backfill_enabled || !is_pg_table)) {
    if (index_backfill_enabled && !req.skip_index_backfill()) {
      if (is_pg_table) {
        // YSQL: start at some permission before backfill.  The real enforcement happens with
        // pg_index system table's indislive and indisready columns.  Choose WRITE_AND_DELETE
        // because it will probably be less confusing.
        index_info.set_index_permissions(INDEX_PERM_WRITE_AND_DELETE);
      } else {
        // YCQL
        index_info.set_index_permissions(INDEX_PERM_DELETE_ONLY);
      }
    }
    s = AddIndexInfoToTable(indexed_table, index_info, resp);
    if (PREDICT_FALSE(!s.ok())) {
      return AbortTableCreation(table.get(), tablets,
                                s.CloneAndPrepend(
                                    Substitute("An error occurred while inserting index info: $0",
                                               s.ToString())),
                                resp);
    }
  }

  // Commit the in-memory state.
  table->mutable_metadata()->CommitMutation();

  for (TabletInfo *tablet : tablets) {
    tablet->mutable_metadata()->CommitMutation();
  }

  if ((colocated && tablets_exist) || (req.has_tablegroup_id() && tablegroup_tablets_exist)) {
    auto call =
        std::make_shared<AsyncAddTableToTablet>(master_, AsyncTaskPool(), tablets[0], table);
    table->AddTask(call);
    WARN_NOT_OK(ScheduleTask(call), "Failed to send AddTableToTablet request");
  }

  if (req.has_creator_role_name()) {
    const NamespaceName& keyspace_name = req.namespace_().name();
    const TableName& table_name = req.name();
    RETURN_NOT_OK(permissions_manager_->GrantPermissions(
        req.creator_role_name(),
        get_canonical_table(keyspace_name, table_name),
        table_name,
        keyspace_name,
        all_permissions_for_resource(ResourceType::TABLE),
        ResourceType::TABLE,
        resp));
  }

  // Verify Transaction gets committed, which occurs after table create finishes.
  if (req.has_transaction() && PREDICT_TRUE(FLAGS_enable_transactional_ddl_gc)) {
    LOG(INFO) << "Enqueuing table for Transaction Verification: " << req.name();
    std::function<Status(bool)> when_done =
        std::bind(&CatalogManager::VerifyTablePgLayer, this, table, _1);
    WARN_NOT_OK(background_tasks_thread_pool_->SubmitFunc(
        std::bind(&YsqlTransactionDdl::VerifyTransaction, &ysql_transaction_, txn, when_done)),
        "Could not submit VerifyTransaction to thread pool");
  }

  LOG(INFO) << "Successfully created " << object_type << " " << table->ToString()
            << " per request from " << RequestorString(rpc);
  background_tasks_->Wake();

  if (FLAGS_master_enable_metrics_snapshotter &&
      !(req.table_type() == TableType::YQL_TABLE_TYPE &&
        namespace_id == kSystemNamespaceId &&
        req.name() == kMetricsSnapshotsTableName)) {
    Status s = CreateMetricsSnapshotsTableIfNeeded(rpc);
    if (!s.ok()) {
      return s.CloneAndPrepend("Error while creating metrics snapshots table");
    }
  }

  DVLOG(3) << __PRETTY_FUNCTION__ << " Done.";
  return Status::OK();
}

Status CatalogManager::VerifyTablePgLayer(scoped_refptr<TableInfo> table, bool rpc_success) {
  // Upon Transaction completion, check pg system table using OID to ensure SUCCESS.
  const uint32_t database_oid = VERIFY_RESULT(GetPgsqlDatabaseOidByTableId(table->id()));
  const auto pg_table_id = GetPgsqlTableId(database_oid, kPgClassTableOid);
  auto entry_exists = VERIFY_RESULT(
      ysql_transaction_.PgEntryExists(pg_table_id, GetPgsqlTableOid(table->id())));
  auto l = table->LockForWrite();
  auto& metadata = table->mutable_metadata()->mutable_dirty()->pb;

  SCHECK(metadata.state() == SysTablesEntryPB::RUNNING ||
         metadata.state() == SysTablesEntryPB::ALTERING, Aborted,
         Substitute("Unexpected table state ($0), abandoning transaction GC work for $1",
                    SysTablesEntryPB_State_Name(metadata.state()), table->ToString()));

  // #5981: Mark un-retryable rpc failures as pass to avoid infinite retry of GC'd txns.
  const bool txn_check_passed = entry_exists || !rpc_success;

  if (txn_check_passed) {
    // Remove the transaction from the entry since we're done processing it.
    metadata.clear_transaction();
    RETURN_NOT_OK(sys_catalog_->UpdateItem(table.get(), leader_ready_term()));
    if (entry_exists) {
      LOG(INFO) << "Table transaction succeeded: " << table->ToString();
    } else {
      LOG(WARNING) << "Unknown RPC failure, removing transaction on table: " << table->ToString();
    }
    // Commit the namespace in-memory state.
    l->Commit();
  } else {
    LOG(INFO) << "Table transaction failed, deleting: " << table->ToString();
    // Async enqueue delete.
    DeleteTableRequestPB del_tbl_req;
    del_tbl_req.mutable_table()->set_table_name(table->name());
    del_tbl_req.mutable_table()->set_table_id(table->id());
    del_tbl_req.set_is_index_table(table->is_index());

    RETURN_NOT_OK(background_tasks_thread_pool_->SubmitFunc( [this, del_tbl_req]() {
      DeleteTableResponsePB del_tbl_resp;
      WARN_NOT_OK(DeleteTable(&del_tbl_req, &del_tbl_resp, nullptr),
          "Failed to Delete Table with failed transaction");
    }));
  }
  return Status::OK();
}

Status CatalogManager::CreateTabletsFromTable(const vector<Partition>& partitions,
                                              const scoped_refptr<TableInfo>& table,
                                              std::vector<TabletInfo*>* tablets) {
  // Create the TabletInfo objects in state PREPARING.
  for (const Partition& partition : partitions) {
    PartitionPB partition_pb;
    partition.ToPB(&partition_pb);
    tablets->push_back(CreateTabletInfo(table.get(), partition_pb));
  }

  // Add the table/tablets to the in-memory map for the assignment.
  table->AddTablets(*tablets);
  auto tablet_map_checkout = tablet_map_.CheckOut();
  for (TabletInfo* tablet : *tablets) {
    InsertOrDie(tablet_map_checkout.get_ptr(), tablet->tablet_id(), tablet);
  }

  return Status::OK();
}

int CatalogManager::GetNumReplicasFromPlacementInfo(const PlacementInfoPB& placement_info) {
  return placement_info.num_replicas() > 0 ?
      placement_info.num_replicas() : FLAGS_replication_factor;
}

Status CatalogManager::CheckValidReplicationInfo(const ReplicationInfoPB& replication_info,
                                                 const TSDescriptorVector& all_ts_descs,
                                                 const vector<Partition>& partitions,
                                                 CreateTableResponsePB* resp) {
  return CheckValidPlacementInfo(replication_info.live_replicas(), all_ts_descs, partitions, resp);
}

Status CatalogManager::CheckValidPlacementInfo(const PlacementInfoPB& placement_info,
                                               const TSDescriptorVector& ts_descs,
                                               const vector<Partition>& partitions,
                                               CreateTableResponsePB* resp) {
  // Verify that the total number of tablets is reasonable, relative to the number
  // of live tablet servers.
  int num_live_tservers = ts_descs.size();
  int num_replicas = GetNumReplicasFromPlacementInfo(placement_info);
  int max_tablets = FLAGS_max_create_tablets_per_ts * num_live_tservers;
  Status s;
  string msg;
  if (num_replicas > 1 && max_tablets > 0 && partitions.size() > max_tablets) {
    msg = Substitute("The requested number of tablets ($0) is over the permitted maximum ($1)",
                     partitions.size(), max_tablets);
    s = STATUS(InvalidArgument, msg);
    LOG(WARNING) << msg;
    return SetupError(resp->mutable_error(), MasterErrorPB::TOO_MANY_TABLETS, s);
  }

  // Verify that the number of replicas isn't larger than the number of live tablet
  // servers.
  if (FLAGS_catalog_manager_check_ts_count_for_create_table &&
      num_replicas > num_live_tservers) {
    msg = Substitute("Not enough live tablet servers to create table with replication factor $0. "
                     "$1 tablet servers are alive.", num_replicas, num_live_tservers);
    LOG(WARNING) << msg
                 << ". Placement info: " << placement_info.ShortDebugString()
                 << ", replication factor flag: " << FLAGS_replication_factor;
    s = STATUS(InvalidArgument, msg);
    return SetupError(resp->mutable_error(), MasterErrorPB::REPLICATION_FACTOR_TOO_HIGH, s);
  }

  // Verify that placement requests are reasonable and we can satisfy the minimums.
  if (!placement_info.placement_blocks().empty()) {
    int minimum_sum = 0;
    for (const auto& pb : placement_info.placement_blocks()) {
      minimum_sum += pb.min_num_replicas();
      if (!pb.has_cloud_info()) {
        msg = Substitute("Got placement info without cloud info set: $0", pb.ShortDebugString());
        s = STATUS(InvalidArgument, msg);
        LOG(WARNING) << msg;
        return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
      }
    }

    if (minimum_sum > num_replicas) {
      msg = Substitute("Sum of minimum replicas per placement ($0) is greater than num_replicas "
                       " ($1)", minimum_sum, num_replicas);
      s = STATUS(InvalidArgument, msg);
      LOG(WARNING) << msg;
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
    }
  }
  return Status::OK();
}

Status CatalogManager::CreateTableInMemory(const CreateTableRequestPB& req,
                                           const Schema& schema,
                                           const PartitionSchema& partition_schema,
                                           const bool create_tablets,
                                           const NamespaceId& namespace_id,
                                           const NamespaceName& namespace_name,
                                           const std::vector<Partition>& partitions,
                                           IndexInfoPB* index_info,
                                           std::vector<TabletInfo*>* tablets,
                                           CreateTableResponsePB* resp,
                                           scoped_refptr<TableInfo>* table) {
  // Add the new table in "preparing" state.
  *table = CreateTableInfo(req, schema, partition_schema, namespace_id, namespace_name, index_info);
  const TableId& table_id = (*table)->id();
  auto table_ids_map_checkout = table_ids_map_.CheckOut();
  (*table_ids_map_checkout)[table_id] = *table;
  // Do not add Postgres tables to the name map as the table name is not unique in a namespace.
  if (req.table_type() != PGSQL_TABLE_TYPE) {
    table_names_map_[{namespace_id, req.name()}] = *table;
  }

  if (create_tablets) {
    RETURN_NOT_OK(CreateTabletsFromTable(partitions, *table, tablets));
  }

  if (resp != nullptr) {
    resp->set_table_id(table_id);
  }

  HandleNewTableId(table_id);

  return Status::OK();
}

Status CatalogManager::CreateTransactionsStatusTableIfNeeded(rpc::RpcContext *rpc) {
  TableIdentifierPB table_indentifier;
  table_indentifier.set_table_name(kTransactionsTableName);
  table_indentifier.mutable_namespace_()->set_name(kSystemNamespaceName);

  // Check that the namespace exists.
  scoped_refptr<NamespaceInfo> ns_info;
  RETURN_NOT_OK(FindNamespace(table_indentifier.namespace_(), &ns_info));
  if (!ns_info) {
    return STATUS(NotFound, "Namespace does not exist", kSystemNamespaceName);
  }

  // If status table exists, do nothing, otherwise create it.
  scoped_refptr<TableInfo> table_info;
  RETURN_NOT_OK(FindTable(table_indentifier, &table_info));

  if (table_info) {
    VLOG(1) << "Transaction status table already exists, not creating.";
    return Status::OK();
  }

  LOG(INFO) << "Creating the transaction status table";
  // Set up a CreateTable request internally.
  CreateTableRequestPB req;
  CreateTableResponsePB resp;
  req.set_name(kTransactionsTableName);
  req.mutable_namespace_()->set_name(kSystemNamespaceName);
  req.set_table_type(TableType::TRANSACTION_STATUS_TABLE_TYPE);

  // Explicitly set the number tablets if the corresponding flag is set, otherwise CreateTable
  // will use the same defaults as for regular tables.
  if (FLAGS_transaction_table_num_tablets > 0) {
    req.mutable_schema()->mutable_table_properties()->set_num_tablets(
        FLAGS_transaction_table_num_tablets);
    req.set_num_tablets(FLAGS_transaction_table_num_tablets);
  }

  ColumnSchema hash(kRedisKeyColumnName, BINARY, /* is_nullable */ false, /* is_hash_key */ true);
  ColumnSchemaToPB(hash, req.mutable_schema()->mutable_columns()->Add());

  Status s = CreateTable(&req, &resp, rpc);
  // We do not lock here so it is technically possible that the table was already created.
  // If so, there is nothing to do so we just ignore the "AlreadyPresent" error.
  if (!s.ok() && !s.IsAlreadyPresent()) {
    return s;
  }

  return Status::OK();
}

Status CatalogManager::CreateMetricsSnapshotsTableIfNeeded(rpc::RpcContext *rpc) {
  TableIdentifierPB table_indentifier;
  table_indentifier.set_table_name(kMetricsSnapshotsTableName);
  table_indentifier.mutable_namespace_()->set_name(kSystemNamespaceName);

  // Check that the namespace exists.
  scoped_refptr<NamespaceInfo> ns_info;
  RETURN_NOT_OK(FindNamespace(table_indentifier.namespace_(), &ns_info));
  if (!ns_info) {
    return STATUS(NotFound, "Namespace does not exist", kSystemNamespaceName);
  }

  // If status table exists do nothing, otherwise create it.
  scoped_refptr<TableInfo> table_info;
  RETURN_NOT_OK(FindTable(table_indentifier, &table_info));

  if (!table_info) {
    // Set up a CreateTable request internally.
    CreateTableRequestPB req;
    CreateTableResponsePB resp;
    req.set_name(kMetricsSnapshotsTableName);
    req.mutable_namespace_()->set_name(kSystemNamespaceName);
    req.set_table_type(TableType::YQL_TABLE_TYPE);

    // Explicitly set the number tablets if the corresponding flag is set, otherwise CreateTable
    // will use the same defaults as for regular tables.
    if (FLAGS_metrics_snapshots_table_num_tablets > 0) {
      req.mutable_schema()->mutable_table_properties()->set_num_tablets(
          FLAGS_metrics_snapshots_table_num_tablets);
      req.set_num_tablets(FLAGS_metrics_snapshots_table_num_tablets);
    }

    // Schema description: "node" refers to tserver uuid. "entity_type" can be either
    // "tserver" or "table". "entity_id" is uuid of corresponding tserver or table.
    // "metric" is the name of the metric and "value" is its val. "ts" is time at
    // which the snapshot was recorded. "details" is a json column for future extensibility.

    YBSchemaBuilder schemaBuilder;
    schemaBuilder.AddColumn("node")->Type(STRING)->HashPrimaryKey()->NotNull();
    schemaBuilder.AddColumn("entity_type")->Type(STRING)->PrimaryKey()->NotNull();
    schemaBuilder.AddColumn("entity_id")->Type(STRING)->PrimaryKey()->NotNull();
    schemaBuilder.AddColumn("metric")->Type(STRING)->PrimaryKey()->NotNull();
    schemaBuilder.AddColumn("ts")->Type(TIMESTAMP)->PrimaryKey()->NotNull()->
      SetSortingType(ColumnSchema::SortingType::kDescending);
    schemaBuilder.AddColumn("value")->Type(INT64);
    schemaBuilder.AddColumn("details")->Type(JSONB);

    YBSchema ybschema;
    CHECK_OK(schemaBuilder.Build(&ybschema));

    auto schema = yb::client::internal::GetSchema(ybschema);
    SchemaToPB(schema, req.mutable_schema());

    Status s = CreateTable(&req, &resp, rpc);
    // We do not lock here so it is technically possible that the table was already created.
    // If so, there is nothing to do so we just ignore the "AlreadyPresent" error.
    if (!s.ok() && !s.IsAlreadyPresent()) {
      return s;
    }
  }
  return Status::OK();
}

Status CatalogManager::IsCreateTableDone(const IsCreateTableDoneRequestPB* req,
                                         IsCreateTableDoneResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<TableInfo> table;

  // 1. Lookup the table and verify if it exists.
  TRACE("Looking up table");
  RETURN_NOT_OK(FindTable(req->table(), &table));
  if (table == nullptr) {
    Status s = STATUS(NotFound, "The object does not exist", req->table().ShortDebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  TRACE("Locking table");
  auto l = table->LockForRead();
  RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));
  const auto& pb = l->data().pb;

  // 2. Verify if the create is in-progress.
  TRACE("Verify if the table creation is in progress for $0", table->ToString());
  resp->set_done(!table->IsCreateInProgress());

  // 3. Set any current errors, if we are experiencing issues creating the table. This will be
  // bubbled up to the MasterService layer. If it is an error, it gets wrapped around in
  // MasterErrorPB::UNKNOWN_ERROR.
  RETURN_NOT_OK(table->GetCreateTableErrorStatus());

  // 4. For index table:
  //   a. If backfill is enabled, check if an index is present in indexed table's index map.
  //   b. Otherwise check if alter schema is done on the indexed table as well.
  // TODO(alex, amit): While (4.a) sounds like it should be enabled for both YSQL and YCQL,
  //    currently it makes YCQL index backfill unstable - which is indicated by intermittent
  //    failures of various tests under CppCassandraDriverTest - mostly TestCreateIndex.
  if (resp->done() && PROTO_IS_INDEX(pb)) {
    auto& indexed_table_id = PROTO_GET_INDEXED_TABLE_ID(pb);
    if (pb.table_type() == PGSQL_TABLE_TYPE &&
        IsUserCreatedTable(*table) &&
        IsIndexBackfillEnabled(pb.table_type(),
                               pb.schema().table_properties().is_transactional())) {
      GetTableSchemaRequestPB get_schema_req;
      GetTableSchemaResponsePB get_schema_resp;
      get_schema_req.mutable_table()->set_table_id(indexed_table_id);
      const Status s = GetTableSchema(&get_schema_req, &get_schema_resp);
      if (!s.ok()) {
        resp->mutable_error()->Swap(get_schema_resp.mutable_error());
        return s;
      }

      resp->set_done(false);
      for (const auto& index : get_schema_resp.indexes()) {
        if (index.has_table_id() && index.table_id() == table->id()) {
          resp->set_done(true);
          // This wait was to give tservers time to apply schema changes and hopefully avoid
          // consistency issues.  It should now be fixed, but it still exists for now because the
          // gflag was part of the 2.5 release.  Later, it shouldn't be a big deal to remove this
          // and the gflag.
          SleepFor(MonoDelta::FromMilliseconds(FLAGS_ysql_backfill_is_create_table_done_delay_ms));
          break;
        }
      }
    } else {
      // TODO(alex, amit): We probably should be fine doing something like (a) case here, since we
      //                   shouldn't care if other indexes are being created
      IsAlterTableDoneRequestPB alter_table_req;
      IsAlterTableDoneResponsePB alter_table_resp;
      alter_table_req.mutable_table()->set_table_id(indexed_table_id);
      const Status s = IsAlterTableDone(&alter_table_req, &alter_table_resp);
      if (!s.ok()) {
        resp->mutable_error()->Swap(alter_table_resp.mutable_error());
        return s;
      }
      resp->set_done(alter_table_resp.done());
    }
  }

  // If this is a transactional table we are not done until the transaction status table is created.
  // However, if we are currently initializing the system catalog snapshot, we don't create the
  // transactions table.
  if (!FLAGS_create_initial_sys_catalog_snapshot &&
      resp->done() && pb.schema().table_properties().is_transactional()) {
    RETURN_NOT_OK(IsTransactionStatusTableCreated(resp));
  }

  // We are not done until the metrics snapshots table is created.
  if (FLAGS_master_enable_metrics_snapshotter && resp->done() &&
      !(table->GetTableType() == TableType::YQL_TABLE_TYPE &&
        table->namespace_id() == kSystemNamespaceId &&
        table->name() == kMetricsSnapshotsTableName)) {
    RETURN_NOT_OK(IsMetricsSnapshotsTableCreated(resp));
  }

  // If this is a colocated table and there is a pending AddTableToTablet task then we are not done.
  if (resp->done() && pb.colocated()) {
    resp->set_done(!table->HasTasks(MonitoredTask::Type::ASYNC_ADD_TABLE_TO_TABLET));
  }

  return Status::OK();
}

Status CatalogManager::IsCreateTableInProgress(const TableId& table_id,
                                               CoarseTimePoint deadline,
                                               bool* create_in_progress) {
  DCHECK_ONLY_NOTNULL(create_in_progress);
  DCHECK(!table_id.empty());

  IsCreateTableDoneRequestPB req;
  IsCreateTableDoneResponsePB resp;
  req.mutable_table()->set_table_id(table_id);
  RETURN_NOT_OK(IsCreateTableDone(&req, &resp));

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  *create_in_progress = !resp.done();
  return Status::OK();
}

Status CatalogManager::WaitForCreateTableToFinish(const TableId& table_id) {
  MonoDelta default_admin_operation_timeout(
      MonoDelta::FromSeconds(FLAGS_yb_client_admin_operation_timeout_sec));
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout;

  return client::RetryFunc(
      deadline, "Waiting on Create Table to be completed", "Timed out waiting for Table Creation",
      std::bind(&CatalogManager::IsCreateTableInProgress, this, table_id, _1, _2));
}

Status CatalogManager::IsTransactionStatusTableCreated(IsCreateTableDoneResponsePB* resp) {
  IsCreateTableDoneRequestPB req;

  req.mutable_table()->set_table_name(kTransactionsTableName);
  req.mutable_table()->mutable_namespace_()->set_name(kSystemNamespaceName);

  return IsCreateTableDone(&req, resp);
}

Status CatalogManager::IsMetricsSnapshotsTableCreated(IsCreateTableDoneResponsePB* resp) {
  IsCreateTableDoneRequestPB req;

  req.mutable_table()->set_table_name(kMetricsSnapshotsTableName);
  req.mutable_table()->mutable_namespace_()->set_name(kSystemNamespaceName);
  req.mutable_table()->mutable_namespace_()->set_database_type(YQLDatabase::YQL_DATABASE_CQL);

  return IsCreateTableDone(&req, resp);
}

std::string CatalogManager::GenerateId(boost::optional<const SysRowEntry::Type> entity_type) {
  SharedLock<LockType> l(lock_);
  return GenerateIdUnlocked(entity_type);
}

std::string CatalogManager::GenerateIdUnlocked(
    boost::optional<const SysRowEntry::Type> entity_type) {
  while (true) {
    // Generate id and make sure it is unique within its category.
    std::string id = oid_generator_.Next();
    if (!entity_type) {
      return id;
    }
    switch (*entity_type) {
      case SysRowEntry::NAMESPACE:
        if (FindPtrOrNull(namespace_ids_map_, id) == nullptr) return id;
        break;
      case SysRowEntry::TABLE:
        if (FindPtrOrNull(*table_ids_map_, id) == nullptr) return id;
        break;
      case SysRowEntry::TABLET:
        if (FindPtrOrNull(*tablet_map_, id) == nullptr) return id;
        break;
      case SysRowEntry::UDTYPE:
        if (FindPtrOrNull(udtype_ids_map_, id) == nullptr) return id;
        break;
      case SysRowEntry::SNAPSHOT:
        return id;
      case SysRowEntry::CDC_STREAM:
        if (!CDCStreamExistsUnlocked(id)) return id;
        break;
      case SysRowEntry::UNKNOWN: FALLTHROUGH_INTENDED;
      case SysRowEntry::CLUSTER_CONFIG: FALLTHROUGH_INTENDED;
      case SysRowEntry::ROLE: FALLTHROUGH_INTENDED;
      case SysRowEntry::REDIS_CONFIG: FALLTHROUGH_INTENDED;
      case SysRowEntry::UNIVERSE_REPLICATION: FALLTHROUGH_INTENDED;
      case SysRowEntry::SYS_CONFIG:
        LOG(DFATAL) << "Invalid id type: " << *entity_type;
        return id;
    }
  }
}

scoped_refptr<TableInfo> CatalogManager::CreateTableInfo(const CreateTableRequestPB& req,
                                                         const Schema& schema,
                                                         const PartitionSchema& partition_schema,
                                                         const NamespaceId& namespace_id,
                                                         const NamespaceName& namespace_name,
                                                         IndexInfoPB* index_info) {
  DCHECK(schema.has_column_ids());
  TableId table_id
      = !req.table_id().empty() ? req.table_id() : GenerateIdUnlocked(SysRowEntry::TABLE);
  scoped_refptr<TableInfo> table = NewTableInfo(table_id);
  table->mutable_metadata()->StartMutation();
  SysTablesEntryPB *metadata = &table->mutable_metadata()->mutable_dirty()->pb;
  metadata->set_state(SysTablesEntryPB::PREPARING);
  metadata->set_name(req.name());
  metadata->set_table_type(req.table_type());
  metadata->set_namespace_id(namespace_id);
  metadata->set_namespace_name(namespace_name);
  metadata->set_version(0);
  metadata->set_next_column_id(ColumnId(schema.max_col_id() + 1));
  if (req.has_replication_info()) {
    metadata->mutable_replication_info()->CopyFrom(req.replication_info());
  }
  // Use the Schema object passed in, since it has the column IDs already assigned,
  // whereas the user request PB does not.
  SchemaToPB(schema, metadata->mutable_schema());
  partition_schema.ToPB(metadata->mutable_partition_schema());
  // For index table, set index details (indexed table id and whether the index is local).
  if (req.has_index_info()) {
    metadata->mutable_index_info()->CopyFrom(req.index_info());

    // Set the deprecated fields also for compatibility reasons.
    metadata->set_indexed_table_id(req.index_info().indexed_table_id());
    metadata->set_is_local_index(req.index_info().is_local());
    metadata->set_is_unique_index(req.index_info().is_unique());

    // Setup index info.
    if (index_info != nullptr) {
      index_info->set_table_id(table->id());
      metadata->mutable_index_info()->CopyFrom(*index_info);
    }
  } else if (req.has_indexed_table_id()) {
    // Read data from the deprecated field and update the new fields.
    metadata->mutable_index_info()->set_indexed_table_id(req.indexed_table_id());
    metadata->mutable_index_info()->set_is_local(req.is_local_index());
    metadata->mutable_index_info()->set_is_unique(req.is_unique_index());

    // Set the deprecated fields also for compatibility reasons.
    metadata->set_indexed_table_id(req.indexed_table_id());
    metadata->set_is_local_index(req.is_local_index());
    metadata->set_is_unique_index(req.is_unique_index());

    // Setup index info.
    if (index_info != nullptr) {
      index_info->set_table_id(table->id());
      metadata->mutable_index_info()->CopyFrom(*index_info);
    }
  }

  if (req.is_pg_shared_table()) {
    metadata->set_is_pg_shared_table(true);
  }

  return table;
}

TabletInfo* CatalogManager::CreateTabletInfo(TableInfo* table,
                                             const PartitionPB& partition) {
  TabletInfo* tablet = new TabletInfo(table, GenerateIdUnlocked(SysRowEntry::TABLET));
  tablet->mutable_metadata()->StartMutation();
  SysTabletsEntryPB *metadata = &tablet->mutable_metadata()->mutable_dirty()->pb;
  metadata->set_state(SysTabletsEntryPB::PREPARING);
  metadata->mutable_partition()->CopyFrom(partition);
  metadata->set_table_id(table->id());
  // This is important: we are setting the first table id in the table_ids list
  // to be the id of the original table that creates the tablet.
  metadata->add_table_ids(table->id());
  return tablet;
}

Status CatalogManager::RemoveTableIdsFromTabletInfo(
    TabletInfoPtr tablet_info,
    unordered_set<TableId> tables_to_remove) {
  auto tablet_lock = tablet_info->LockForWrite();

  google::protobuf::RepeatedPtrField<std::string> new_table_ids;
  for (const auto& table_id : tablet_lock->data().pb.table_ids()) {
    if (tables_to_remove.find(table_id) == tables_to_remove.end()) {
      *new_table_ids.Add() = std::move(table_id);
    }
  }
  tablet_lock->mutable_data()->pb.mutable_table_ids()->Swap(&new_table_ids);

  RETURN_NOT_OK(sys_catalog_->UpdateItem(tablet_info.get(), leader_ready_term()));
  tablet_lock->Commit();
  return Status::OK();
}

Status CatalogManager::FindTable(const TableIdentifierPB& table_identifier,
                                 scoped_refptr<TableInfo> *table_info) {
  SharedLock<LockType> l(lock_);

  if (table_identifier.has_table_id()) {
    *table_info = FindPtrOrNull(*table_ids_map_, table_identifier.table_id());
  } else if (table_identifier.has_table_name()) {
    NamespaceId namespace_id;

    if (table_identifier.has_namespace_()) {
      const auto& namespace_info = table_identifier.namespace_();
      if (namespace_info.has_id()) {
        namespace_id = namespace_info.id();
      } else if (namespace_info.has_name()) {
        // Find namespace by its name.
        scoped_refptr<NamespaceInfo> ns = FindPtrOrNull(
            namespace_names_mapper_[GetDatabaseType(namespace_info)],
            namespace_info.name());

        if (ns == nullptr) {
          // The namespace was not found. This is a correct case. Just return NULL.
          *table_info = nullptr;
          return Status::OK();
        }

        namespace_id = ns->id();
      } else {
        return STATUS(InvalidArgument, "Neither keyspace id or keyspace name are specified");
      }
    }

    if (namespace_id.empty()) {
      return STATUS(InvalidArgument, "No namespace used");
    }

    *table_info = FindPtrOrNull(table_names_map_, {namespace_id, table_identifier.table_name()});
  } else {
    return STATUS(InvalidArgument, "Neither table id or table name are specified");
  }
  return Status::OK();
}

Status CatalogManager::FindNamespaceUnlocked(const NamespaceIdentifierPB& ns_identifier,
                                             scoped_refptr<NamespaceInfo>* ns_info) const {
  if (ns_identifier.has_id()) {
    *ns_info = FindPtrOrNull(namespace_ids_map_, ns_identifier.id());
    if (*ns_info == nullptr) {
      return STATUS(NotFound, "Keyspace identifier not found", ns_identifier.id());
    }
  } else if (ns_identifier.has_name()) {
    auto db = GetDatabaseType(ns_identifier);
    *ns_info = FindPtrOrNull(namespace_names_mapper_[db], ns_identifier.name());
    if (*ns_info == nullptr) {
      return STATUS(NotFound, "Keyspace name not found", ns_identifier.name());
    }
  } else {
    return STATUS(NotFound, "Neither keyspace id nor keyspace name is specified.");
  }
  return Status::OK();
}

Status CatalogManager::FindNamespace(const NamespaceIdentifierPB& ns_identifier,
                                     scoped_refptr<NamespaceInfo>* ns_info) const {
  SharedLock<LockType> l(lock_);
  return FindNamespaceUnlocked(ns_identifier, ns_info);
}

Result<TableDescription> CatalogManager::DescribeTable(const TableIdentifierPB& table_identifier) {
  TableDescription result;

  // Lookup the table and verify it exists.
  TRACE("Looking up table");
  RETURN_NOT_OK(FindTable(table_identifier, &result.table_info));
  if (result.table_info == nullptr) {
    return STATUS(NotFound, "Object does not exist", table_identifier.ShortDebugString(),
                  MasterError(MasterErrorPB::OBJECT_NOT_FOUND));
  }

  NamespaceId namespace_id;
  {
    TRACE("Locking table");
    auto l = result.table_info->LockForRead();

    if (result.table_info->IsCreateInProgress()) {
      return STATUS(IllegalState, "Table creation is in progress", result.table_info->ToString(),
                    MasterError(MasterErrorPB::TABLE_CREATION_IS_IN_PROGRESS));
    }

    result.table_info->GetAllTablets(&result.tablet_infos);

    namespace_id = result.table_info->namespace_id();
  }

  {
    TRACE("Looking up namespace");
    SharedLock<LockType> l(lock_);

    result.namespace_info = FindPtrOrNull(namespace_ids_map_, namespace_id);
    if (result.namespace_info == nullptr) {
      return STATUS(
          InvalidArgument, "Could not find namespace by namespace id", namespace_id,
          MasterError(MasterErrorPB::NAMESPACE_NOT_FOUND));
    }
  }

  return result;
}

// Truncate a Table.
Status CatalogManager::TruncateTable(const TruncateTableRequestPB* req,
                                     TruncateTableResponsePB* resp,
                                     rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing TruncateTable request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  RETURN_NOT_OK(CheckOnline());

  for (int i = 0; i < req->table_ids_size(); i++) {
    RETURN_NOT_OK(TruncateTable(req->table_ids(i), resp, rpc));
  }

  return Status::OK();
}

Status CatalogManager::TruncateTable(const TableId& table_id,
                                     TruncateTableResponsePB* resp,
                                     rpc::RpcContext* rpc) {
  // Lookup the table and verify if it exists.
  TRACE(Substitute("Looking up object by id $0", table_id));
  scoped_refptr<TableInfo> table;
  {
    SharedLock<LockType> cm_shared_lock(lock_);
    table = FindPtrOrNull(*table_ids_map_, table_id);
    if (table == nullptr) {
      Status s = STATUS_SUBSTITUTE(NotFound, "The object with id $0 does not exist", table_id);
      return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
    }
  }

  TRACE(Substitute("Locking object with id $0", table_id));
  auto l = table->LockForRead();
  RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));

  // Truncate on a colocated table should not hit master because it should be handled by a write
  // DML that creates a table-level tombstone.
  LOG_IF(WARNING, IsColocatedUserTable(*table)) << "cannot truncate a colocated table on master";

  // Send a Truncate() request to each tablet in the table.
  SendTruncateTableRequest(table);

  LOG(INFO) << "Successfully initiated TRUNCATE for " << table->ToString() << " per request from "
            << RequestorString(rpc);
  background_tasks_->Wake();

  // Truncate indexes also.
  // Note: PG table does not have references to indexes in the base table, so associated indexes
  //       must be truncated from the PG code separately.
  const bool is_index = PROTO_IS_INDEX(l->data().pb);
  DCHECK(!is_index || l->data().pb.indexes().empty()) << "indexes should be empty for index table";
  for (const auto& index_info : l->data().pb.indexes()) {
    RETURN_NOT_OK(TruncateTable(index_info.table_id(), resp, rpc));
  }

  return Status::OK();
}

void CatalogManager::SendTruncateTableRequest(const scoped_refptr<TableInfo>& table) {
  vector<scoped_refptr<TabletInfo>> tablets;
  table->GetAllTablets(&tablets);
  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    SendTruncateTabletRequest(tablet);
  }
}

void CatalogManager::SendTruncateTabletRequest(const scoped_refptr<TabletInfo>& tablet) {
  LOG_WITH_PREFIX(INFO) << "Truncating tablet " << tablet->id();
  auto call = std::make_shared<AsyncTruncate>(master_, AsyncTaskPool(), tablet);
  tablet->table()->AddTask(call);
  WARN_NOT_OK(
      ScheduleTask(call),
      Substitute("Failed to send truncate request for tablet $0", tablet->id()));
}

Status CatalogManager::IsTruncateTableDone(const IsTruncateTableDoneRequestPB* req,
                                           IsTruncateTableDoneResponsePB* resp) {
  LOG(INFO) << "Servicing IsTruncateTableDone request for table id " << req->table_id();
  RETURN_NOT_OK(CheckOnline());

  // Lookup the truncated table.
  TRACE("Looking up table $0", req->table_id());
  std::lock_guard<LockType> l_map(lock_);
  scoped_refptr<TableInfo> table = FindPtrOrNull(*table_ids_map_, req->table_id());

  if (table == nullptr) {
    Status s = STATUS(NotFound, "The object does not exist: table with id", req->table_id());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  TRACE("Locking table");
  auto l = table->LockForRead();
  RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));

  resp->set_done(!table->HasTasks(MonitoredTask::Type::ASYNC_TRUNCATE_TABLET));
  return Status::OK();
}

// Note: only used by YSQL as of 2020-10-29.
Status CatalogManager::BackfillIndex(
    const BackfillIndexRequestPB* req,
    BackfillIndexResponsePB* resp,
    rpc::RpcContext* rpc) {
  TableIdentifierPB index_table_identifier = req->index_identifier();

  scoped_refptr<TableInfo> index_table;
  RETURN_NOT_OK(FindTable(index_table_identifier, &index_table));
  if (index_table == nullptr) {
    Status s = STATUS(
        NotFound,
        "The index does not exist",
        index_table_identifier.ShortDebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }
  if (index_table->GetTableType() != PGSQL_TABLE_TYPE) {
    // This request is only supported for YSQL for now.  YCQL has its own mechanism.
    return STATUS(
        InvalidArgument,
        "Unexpected non-YSQL table",
        index_table_identifier.ShortDebugString());
  }

  // Collect indexed_table.
  scoped_refptr<TableInfo> indexed_table;
  {
    auto l = index_table->LockForRead();
    TableId indexed_table_id = PROTO_GET_INDEXED_TABLE_ID(l->data().pb);
    resp->mutable_table_identifier()->set_table_id(indexed_table_id);
    indexed_table = GetTableInfo(indexed_table_id);
  }

  // TODO(jason): when ready to use INDEX_PERM_DO_BACKFILL for resuming backfill across master
  // leader changes, replace the following (issue #6218).

  // Collect index_info_pb.
  IndexInfoPB index_info_pb;
  indexed_table->GetIndexInfo(index_table->id()).ToPB(&index_info_pb);
  if (index_info_pb.index_permissions() != INDEX_PERM_WRITE_AND_DELETE) {
    return SetupError(
        resp->mutable_error(),
        MasterErrorPB::INVALID_SCHEMA,
        STATUS_FORMAT(
            InvalidArgument,
            "Expected WRITE_AND_DELETE perm, got $0",
            IndexPermissions_Name(index_info_pb.index_permissions())));
  }

  return MultiStageAlterTable::StartBackfillingData(
      this,
      indexed_table,
      index_info_pb);
}

Status CatalogManager::MarkIndexInfoFromTableForDeletion(
    const TableId& indexed_table_id, const TableId& index_table_id, bool multi_stage,
    DeleteTableResponsePB* resp) {
  // Lookup the indexed table and verify if it exists.
  scoped_refptr<TableInfo> indexed_table = GetTableInfo(indexed_table_id);
  if (indexed_table == nullptr) {
    LOG(WARNING) << "Indexed table " << indexed_table_id << " for index "
                 << index_table_id << " not found";
    return Status::OK();
  }

  if (resp) {
    NamespaceIdentifierPB nsId;
    nsId.set_id(indexed_table->namespace_id());
    scoped_refptr<NamespaceInfo> nsInfo;
    RETURN_NOT_OK(FindNamespace(nsId, &nsInfo));
    auto* resp_indexed_table = resp->mutable_indexed_table();
    resp_indexed_table->mutable_namespace_()->set_name(nsInfo->name());
    resp_indexed_table->set_table_name(indexed_table->name());
    resp_indexed_table->set_table_id(indexed_table_id);
  }
  if (multi_stage) {
    RETURN_NOT_OK(MultiStageAlterTable::UpdateIndexPermission(
        this, indexed_table,
        {{index_table_id, IndexPermissions::INDEX_PERM_WRITE_AND_DELETE_WHILE_REMOVING}}));
  } else {
    RETURN_NOT_OK(DeleteIndexInfoFromTable(indexed_table_id, index_table_id));
  }

  // Actual Deletion of the index info will happen asynchronously after all the
  // tablets move to the new IndexPermission of DELETE_ONLY_WHILE_REMOVING.
  SendAlterTableRequest(indexed_table);
  return Status::OK();
}

Status CatalogManager::DeleteIndexInfoFromTable(
    const TableId& indexed_table_id, const TableId& index_table_id) {
  scoped_refptr<TableInfo> indexed_table = GetTableInfo(indexed_table_id);
  if (indexed_table == nullptr) {
    LOG(WARNING) << "Indexed table " << indexed_table_id << " for index " << index_table_id
                 << " not found";
    return Status::OK();
  }
  TRACE("Locking indexed table");
  auto l = indexed_table->LockForWrite();
  auto &indexed_table_data = *l->mutable_data();

  // Heed issue #6233.
  if (!l->data().pb.has_fully_applied_schema()) {
    MultiStageAlterTable::CopySchemaDetailsToFullyApplied(&indexed_table_data.pb);
  }
  auto *indexes = indexed_table_data.pb.mutable_indexes();
  for (int i = 0; i < indexes->size(); i++) {
    if (indexes->Get(i).table_id() == index_table_id) {

      indexes->DeleteSubrange(i, 1);

      indexed_table_data.pb.set_version(indexed_table_data.pb.version() + 1);
      indexed_table_data.set_state(SysTablesEntryPB::ALTERING,
                                   Substitute("Alter table version=$0 ts=$1",
                                              indexed_table_data.pb.version(),
                                              LocalTimeAsString()));

      // Update sys-catalog with the deleted indexed table info.
      TRACE("Updating indexed table metadata on disk");
      RETURN_NOT_OK(sys_catalog_->UpdateItem(indexed_table.get(), leader_ready_term()));

      // Update the in-memory state.
      TRACE("Committing in-memory state");
      l->Commit();
      return Status::OK();
    }
  }

  LOG(WARNING) << "Index " << index_table_id << " not found in indexed table " << indexed_table_id;
  return Status::OK();
}

Status CatalogManager::DeleteTable(
    const DeleteTableRequestPB* req, DeleteTableResponsePB* resp, rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing DeleteTable request from " << RequestorString(rpc) << ": "
            << req->ShortDebugString();

  RETURN_NOT_OK(CheckOnline());

  if (req->is_index_table()) {
    TRACE("Looking up index");
    TableIdentifierPB table_identifier = req->table();
    scoped_refptr<TableInfo> table;
    RETURN_NOT_OK(FindTable(table_identifier, &table));
    if (table == nullptr) {
      Status s = STATUS(NotFound, "The object does not exist", table_identifier.ShortDebugString());
      return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
    }
    TableId table_id = table->id();
    resp->set_table_id(table_id);
    TableId indexed_table_id;
    {
      auto l = table->LockForRead();
      indexed_table_id = PROTO_GET_INDEXED_TABLE_ID(l->data().pb);
    }
    scoped_refptr<TableInfo> indexed_table = GetTableInfo(indexed_table_id);
    const bool is_pg_table = indexed_table->GetTableType() == PGSQL_TABLE_TYPE;
    bool is_transactional;
    {
      Schema index_schema;
      RETURN_NOT_OK(table->GetSchema(&index_schema));
      is_transactional = index_schema.table_properties().is_transactional();
    }
    const bool index_backfill_enabled =
        IsIndexBackfillEnabled(table->GetTableType(), is_transactional);
    if (!is_pg_table && index_backfill_enabled) {
      return MarkIndexInfoFromTableForDeletion(
          indexed_table_id, table_id, /* multi_stage */ true, resp);
    }
  }

  return DeleteTableInternal(req, resp, rpc);
}

// Delete a Table
//  - Update the table state to "DELETING".
//  - Issue DeleteTablet tasks to all said tablets.
//  - Update all the underlying tablet states as "DELETED".
//
// This order of events can help us guarantee that:
//  - If a table is DELETING/DELETED, we do not add further tasks to it.
//  - A DeleteTable is done when a table is either DELETING or DELETED and has no running tasks.
//  - If a table is DELETING and it has no tasks on it, then it is safe to mark DELETED.
//
// We are lazy about deletions.
//
// IMPORTANT: If modifying, consider updating DeleteYsqlDBTables(), the bulk deletion API.
Status CatalogManager::DeleteTableInternal(
    const DeleteTableRequestPB* req, DeleteTableResponsePB* resp, rpc::RpcContext* rpc) {
  vector<scoped_refptr<TableInfo>> tables;
  vector<unique_ptr<TableInfo::lock_type>> table_locks;

  RETURN_NOT_OK(DeleteTableInMemory(req->table(), req->is_index_table(),
                                    true /* update_indexed_table */,
                                    &tables, &table_locks, resp, rpc));

  // Delete any CDC streams that are set up on this table.
  TRACE("Deleting CDC streams on table");
  RETURN_NOT_OK(DeleteCDCStreamsForTable(resp->table_id()));

  // Update the in-memory state.
  TRACE("Committing in-memory state");
  for (int i = 0; i < table_locks.size(); i++) {
    table_locks[i]->Commit();
  }

  if (PREDICT_FALSE(FLAGS_catalog_manager_inject_latency_in_delete_table_ms > 0)) {
    LOG(INFO) << "Sleeping in CatalogManager::DeleteTable for " <<
        FLAGS_catalog_manager_inject_latency_in_delete_table_ms << " ms";
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_catalog_manager_inject_latency_in_delete_table_ms));
  }

  for (const scoped_refptr<TableInfo> &table : tables) {
    // Send a DeleteTablet() request to each tablet replica in the table.
    DeleteTabletsAndSendRequests(table);
    // Send a RemoveTableFromTablet() request to each colocated parent tablet replica in the table.
    if (IsColocatedUserTable(*table)) {
      auto call = std::make_shared<AsyncRemoveTableFromTablet>(
          master_, AsyncTaskPool(), table->GetColocatedTablet(), table);
      table->AddTask(call);
      WARN_NOT_OK(ScheduleTask(call), "Failed to send RemoveTableFromTablet request");
    }
  }

  // If there are any permissions granted on this table find them and delete them. This is necessary
  // because we keep track of the permissions based on the canonical resource name which is a
  // combination of the keyspace and table names, so if another table with the same name is created
  // (in the same keyspace where the previous one existed), and the permissions were not deleted at
  // the time of the previous table deletion, then the permissions that existed for the previous
  // table will automatically be granted to the new table even though this wasn't the intention.
  string canonical_resource = get_canonical_table(req->table().namespace_().name(),
                                                  req->table().table_name());
  RETURN_NOT_OK(permissions_manager_->RemoveAllPermissionsForResource(canonical_resource, resp));

  LOG(INFO) << "Successfully initiated deletion of "
            << (req->is_index_table() ? "index" : "table") << " with "
            << req->table().DebugString() << " per request from " << RequestorString(rpc);
  // Asynchronously cleans up the final memory traces of the deleted database.
  background_tasks_->Wake();
  return Status::OK();
}

Status CatalogManager::DeleteTableInMemory(const TableIdentifierPB& table_identifier,
                                           const bool is_index_table,
                                           const bool update_indexed_table,
                                           vector<scoped_refptr<TableInfo>>* tables,
                                           vector<unique_ptr<TableInfo::lock_type>>* table_lcks,
                                           DeleteTableResponsePB* resp,
                                           rpc::RpcContext* rpc) {
  // TODO(NIC): How to handle a DeleteTable request when the namespace is being deleted?
  const char* const object_type = is_index_table ? "index" : "table";
  const bool cascade_delete_index = is_index_table && !update_indexed_table;

  scoped_refptr<TableInfo> table;

  // Lookup the table and verify if it exists.
  TRACE(Substitute("Looking up $0", object_type));
  RETURN_NOT_OK(FindTable(table_identifier, &table));
  if (table == nullptr) {
    if (cascade_delete_index) {
      LOG(WARNING) << "Index " << table_identifier.DebugString() << " not found";
      return Status::OK();
    } else {
      Status s = STATUS(NotFound, "The object does not exist", table_identifier.ShortDebugString());
      return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
    }
  }

  TRACE(Substitute("Locking $0", object_type));
  auto l = table->LockForWrite();
  resp->set_table_id(table->id());

  if (is_index_table == PROTO_IS_TABLE(l->data().pb)) {
    Status s = STATUS(NotFound, "The object does not exist");
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  if (l->data().started_deleting()) {
    if (cascade_delete_index) {
      LOG(WARNING) << "Index " << table_identifier.DebugString() << " was deleted";
      return Status::OK();
    } else {
      Status s = STATUS(NotFound, "The object was deleted", l->data().pb.state_msg());
      return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
    }
  }

  TRACE("Updating metadata on disk");
  // Update the metadata for the on-disk state.
  l->mutable_data()->set_state(SysTablesEntryPB::DELETING,
                               Substitute("Started deleting at $0", LocalTimeAsString()));

  // Update sys-catalog with the removed table state.
  Status s = sys_catalog_->UpdateItem(table.get(), leader_ready_term());

  if (PREDICT_FALSE(FLAGS_TEST_simulate_crash_after_table_marked_deleting)) {
    return Status::OK();
  }

  if (!s.ok()) {
    // The mutation will be aborted when 'l' exits the scope on early return.
    s = s.CloneAndPrepend(Substitute("An error occurred while updating sys tables: $0",
                                     s.ToString()));
    LOG(WARNING) << s.ToString();
    return CheckIfNoLongerLeaderAndSetupError(s, resp);
  }

  // Update the internal table maps.
  // Exclude Postgres tables which are not in the name map.
  if (l->data().table_type() != PGSQL_TABLE_TYPE) {
    TRACE("Removing from by-name map");
    std::lock_guard<LockType> l_map(lock_);
    if (table_names_map_.erase({l->data().namespace_id(), l->data().name()}) != 1) {
      PANIC_RPC(rpc, "Could not remove table from map, name=" + table->ToString());
    }
    table_ids_map_.Commit();
  }

  // For regular (indexed) table, delete all its index tables if any. Else for index table, delete
  // index info from the indexed table.
  if (!is_index_table) {
    TableIdentifierPB index_identifier;
    for (auto index : l->data().pb.indexes()) {
      index_identifier.set_table_id(index.table_id());
      RETURN_NOT_OK(DeleteTableInMemory(index_identifier, true /* is_index_table */,
                                        false /* update_indexed_table */, tables,
                                        table_lcks, resp, rpc));
    }
  } else if (update_indexed_table) {
    s = MarkIndexInfoFromTableForDeletion(
        PROTO_GET_INDEXED_TABLE_ID(l->data().pb), table->id(), /* multi_stage */ false, resp);
    if (!s.ok()) {
      s = s.CloneAndPrepend(Substitute("An error occurred while deleting index info: $0",
                                       s.ToString()));
      LOG(WARNING) << s.ToString();
      return CheckIfNoLongerLeaderAndSetupError(s, resp);
    }
  }

  table->AbortTasks();

  // For regular (indexed) table, insert table info and lock in the front of the list. Else for
  // index table, append them to the end. We do so so that we will commit and delete the indexed
  // table first before its indexes.
  if (!is_index_table) {
    tables->insert(tables->begin(), table);
    table_lcks->insert(table_lcks->begin(), std::move(l));
  } else {
    tables->push_back(table);
    table_lcks->push_back(std::move(l));
  }

  return Status::OK();
}

unique_ptr<TableInfo::lock_type> CatalogManager::MaybeTransitionTableToDeleted(
    scoped_refptr<TableInfo> table) {
  if (table->HasTasks()) {
    return nullptr;
  }
  {
    auto lock = table->LockForRead();

    // For any table in DELETING state, we will want to mark it as DELETED once all its respective
    // tablets have been successfully removed from tservers.
    if (!lock->data().is_deleting()) {
      return nullptr;
    }
  }
  // The current relevant order of operations during a DeleteTable is:
  // 1) Mark the table as DELETING
  // 2) Abort the current table tasks
  // 3) Per tablet, send DeleteTable requests to all TS, then mark that tablet as DELETED
  //
  // This creates a race, wherein, after 2, HasTasks can be false, but we still have not
  // gotten to point 3, which would add further tasks for the deletes.
  //
  // However, HasTasks is cheaper than AreAllTabletsDeleted...
  if (!table->AreAllTabletsDeleted() &&
      !IsSystemTable(*table) &&
      !IsColocatedUserTable(*table)) {
    return nullptr;
  }

  auto lock = table->LockForWrite();
  if (lock->data().is_deleting()) {
    // Update the metadata for the on-disk state.
    LOG(INFO) << "Marking table as DELETED: " << table->ToString();
    lock->mutable_data()->set_state(SysTablesEntryPB::DELETED,
        Substitute("Deleted with tablets at $0", LocalTimeAsString()));
    return lock;
  }
  return nullptr;
}

void CatalogManager::CleanUpDeletedTables() {
  // TODO(bogdan): Cache tables being deleted to make this iterate only over those?
  vector<scoped_refptr<TableInfo>> tables_to_delete;
  // Garbage collecting.
  // Going through all tables under the global lock, copying them to not hold lock for too long.
  TableInfoMap copy_of_table_by_id_map;
  {
    std::lock_guard<LockType> l_map(lock_);
    copy_of_table_by_id_map = *table_ids_map_;
  }
  // Mark the tables as DELETED and remove them from the in-memory maps.
  vector<TableInfo*> tables_to_update_on_disk;
  vector<unique_ptr<TableInfo::lock_type>> table_locks;
  for (const auto& it : copy_of_table_by_id_map) {
    const auto& table = it.second;
    auto lock = MaybeTransitionTableToDeleted(table);
    if (lock) {
      table_locks.push_back(std::move(lock));
      tables_to_update_on_disk.push_back(table.get());
    }
  }
  if (tables_to_update_on_disk.size() > 0) {
    Status s = sys_catalog_->UpdateItems(tables_to_update_on_disk, leader_ready_term());
    if (!s.ok()) {
      LOG(WARNING) << "Error marking tables as DELETED: " << s.ToString();
      return;
    }
    // Update the table in-memory info as DELETED after we've removed them from the maps.
    for (auto& lock : table_locks) {
      lock->Commit();
    }
    // TODO: Check if we want to delete the totally deleted table from the sys_catalog here.
    // TODO: SysCatalog::DeleteItem() if we've DELETED all user tables in a DELETING namespace.
    // TODO: Also properly handle namespace_ids_map_.erase(table->namespace_id())
  }
}

Status CatalogManager::IsDeleteTableDone(const IsDeleteTableDoneRequestPB* req,
                                         IsDeleteTableDoneResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  // Lookup the deleted table.
  TRACE("Looking up table $0", req->table_id());
  std::lock_guard<LockType> l_map(lock_);
  scoped_refptr<TableInfo> table = FindPtrOrNull(*table_ids_map_, req->table_id());

  if (table == nullptr) {
    LOG(INFO) << "Servicing IsDeleteTableDone request for table id "
              << req->table_id() << ": deleted (not found)";
    resp->set_done(true);
    return Status::OK();
  }

  TRACE("Locking table");
  auto l = table->LockForRead();

  if (!l->data().started_deleting()) {
    LOG(WARNING) << "Servicing IsDeleteTableDone request for table id "
                 << req->table_id() << ": NOT deleted";
    Status s = STATUS(IllegalState, "The object was NOT deleted", l->data().pb.state_msg());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  // Temporary fix for github issue #5290.
  // TODO: Wait till deletion completed for tablegroup parent table.
  if (IsTablegroupParentTable(*table)) {
    LOG(INFO) << "Servicing IsDeleteTableDone request for tablegroup parent table id "
              << req->table_id() << ": deleting. Skipping wait for DELETED state.";
    resp->set_done(true);
    return Status::OK();
  }

  if (l->data().is_deleted()) {
    LOG(INFO) << "Servicing IsDeleteTableDone request for table id "
              << req->table_id() << ": totally deleted";
      resp->set_done(true);
  } else {
    LOG(INFO) << "Servicing IsDeleteTableDone request for table id " << req->table_id()
              << ((!IsColocatedUserTable(*table)) ? ": deleting tablets" : "");

    std::vector<std::shared_ptr<TSDescriptor>> descs;
    master_->ts_manager()->GetAllDescriptors(&descs);
    for (auto& ts_desc : descs) {
      LOG(INFO) << "Deleting on " << ts_desc->permanent_uuid() << ": "
                << ts_desc->PendingTabletDeleteToString();
    }

    resp->set_done(false);
  }

  return Status::OK();
}

namespace {

CHECKED_STATUS ApplyAlterSteps(const SysTablesEntryPB& current_pb,
                               const AlterTableRequestPB* req,
                               Schema* new_schema,
                               ColumnId* next_col_id) {
  const SchemaPB& current_schema_pb = current_pb.schema();
  Schema cur_schema;
  RETURN_NOT_OK(SchemaFromPB(current_schema_pb, &cur_schema));

  SchemaBuilder builder(cur_schema);
  if (current_pb.has_next_column_id()) {
    builder.set_next_column_id(ColumnId(current_pb.next_column_id()));
  }
  if (current_pb.has_colocated() && current_pb.colocated()) {
    if (current_schema_pb.table_properties().is_ysql_catalog_table()) {
      Uuid cotable_id;
      RETURN_NOT_OK(cotable_id.FromHexString(req->table().table_id()));
      builder.set_cotable_id(cotable_id);
    } else {
      uint32_t pgtable_id = VERIFY_RESULT(GetPgsqlTableOid(req->table().table_id()));
      builder.set_pgtable_id(pgtable_id);
    }
  }

  for (const AlterTableRequestPB::Step& step : req->alter_schema_steps()) {
    switch (step.type()) {
      case AlterTableRequestPB::ADD_COLUMN: {
        if (!step.has_add_column()) {
          return STATUS(InvalidArgument, "ADD_COLUMN missing column info");
        }

        // Verify that encoding is appropriate for the new column's type.
        ColumnSchemaPB new_col_pb = step.add_column().schema();
        if (new_col_pb.has_id()) {
          return STATUS_SUBSTITUTE(InvalidArgument,
              "column $0: client should not specify column id", new_col_pb.ShortDebugString());
        }
        ColumnSchema new_col = ColumnSchemaFromPB(new_col_pb);

        RETURN_NOT_OK(builder.AddColumn(new_col, false));
        break;
      }

      case AlterTableRequestPB::DROP_COLUMN: {
        if (!step.has_drop_column()) {
          return STATUS(InvalidArgument, "DROP_COLUMN missing column info");
        }

        if (cur_schema.is_key_column(step.drop_column().name())) {
          return STATUS(InvalidArgument, "cannot remove a key column");
        }

        RETURN_NOT_OK(builder.RemoveColumn(step.drop_column().name()));
        break;
      }

      case AlterTableRequestPB::RENAME_COLUMN: {
        if (!step.has_rename_column()) {
          return STATUS(InvalidArgument, "RENAME_COLUMN missing column info");
        }

        RETURN_NOT_OK(builder.RenameColumn(
        step.rename_column().old_name(),
        step.rename_column().new_name()));
        break;
      }

        // TODO: EDIT_COLUMN.

      default: {
        return STATUS_SUBSTITUTE(InvalidArgument, "Invalid alter step type: $0", step.type());
      }
    }
  }

  if (req->has_alter_properties()) {
    RETURN_NOT_OK(builder.AlterProperties(req->alter_properties()));
  }

  *new_schema = builder.Build();
  *next_col_id = builder.next_column_id();
  return Status::OK();
}

} // namespace

Status CatalogManager::AlterTable(const AlterTableRequestPB* req,
                                  AlterTableResponsePB* resp,
                                  rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing AlterTable request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<TableInfo> table;

  // Lookup the table and verify if it exists.
  TRACE("Looking up table");
  RETURN_NOT_OK(FindTable(req->table(), &table));
  if (table == nullptr) {
    Status s = STATUS(NotFound, "The object does not exist", req->table().ShortDebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  NamespaceId new_namespace_id;

  if (req->has_new_namespace()) {
    // Lookup the new namespace and verify if it exists.
    TRACE("Looking up new namespace");
    scoped_refptr<NamespaceInfo> ns;
    NamespaceIdentifierPB namespace_identifier = req->new_namespace();
    // Use original namespace_id as new_namespace_id for YSQL tables.
    if (table->GetTableType() == PGSQL_TABLE_TYPE && !namespace_identifier.has_id()) {
      namespace_identifier.set_id(table->namespace_id());
    }
    RETURN_NAMESPACE_NOT_FOUND(FindNamespace(namespace_identifier, &ns), resp);

    auto ns_lock = ns->LockForRead();
    new_namespace_id = ns->id();
    // Don't use Namespaces that aren't running.
    if (ns->state() != SysNamespaceEntryPB::RUNNING) {
      Status s = STATUS_SUBSTITUTE(TryAgain,
          "Namespace not running (State=$0).  Cannot create $1.$2",
          SysNamespaceEntryPB::State_Name(ns->state()), ns->name(), table->name() );
      return SetupError(resp->mutable_error(), NamespaceMasterError(ns->state()), s);
    }
  }

  TRACE("Locking table");
  auto l = table->LockForWrite();
  RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));

  bool has_changes = false;
  auto& table_pb = l->mutable_data()->pb;
  const TableName table_name = l->data().name();
  const NamespaceId namespace_id = l->data().namespace_id();
  const TableName new_table_name = req->has_new_table_name() ? req->new_table_name() : table_name;

  // Calculate new schema for the on-disk state, not persisted yet.
  Schema new_schema;
  ColumnId next_col_id = ColumnId(l->data().pb.next_column_id());
  if (req->alter_schema_steps_size() || req->has_alter_properties()) {
    TRACE("Apply alter schema");
    Status s = ApplyAlterSteps(l->data().pb, req, &new_schema, &next_col_id);
    if (!s.ok()) {
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
    }
    DCHECK_NE(next_col_id, 0);
    DCHECK_EQ(new_schema.find_column_by_id(next_col_id),
              static_cast<int>(Schema::kColumnNotFound));
    has_changes = true;
  }

  // Try to acquire the new table name.
  if (req->has_new_namespace() || req->has_new_table_name()) {

    if (new_namespace_id.empty()) {
      const Status s = STATUS(InvalidArgument, "No namespace used");
      return SetupError(resp->mutable_error(), MasterErrorPB::NO_NAMESPACE_USED, s);
    }

    std::lock_guard<LockType> catalog_lock(lock_);
    VLOG(3) << __func__ << ": Acquired the catalog manager lock_";

    TRACE("Acquired catalog manager lock");

    // Verify that the table does not exist.
    scoped_refptr<TableInfo> other_table = FindPtrOrNull(
        table_names_map_, {new_namespace_id, new_table_name});
    if (other_table != nullptr) {
      Status s = STATUS_SUBSTITUTE(AlreadyPresent,
          "Object '$0.$1' already exists",
          GetNamespaceNameUnlocked(new_namespace_id), other_table->name());
      LOG(WARNING) << "Found table: " << other_table->ToStringWithState()
                   << ". Failed alterring table with error: "
                   << s.ToString() << " Request:\n" << req->DebugString();
      return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_ALREADY_PRESENT, s);
    }

    // Acquire the new table name (now we have 2 name for the same table).
    table_names_map_[{new_namespace_id, new_table_name}] = table;
    table_pb.set_namespace_id(new_namespace_id);
    table_pb.set_name(new_table_name);

    has_changes = true;
  }

  // Check if there has been any changes to the placement policies for this table.
  if (req->has_replication_info()) {
    // If this is a colocated table, it does not make sense to set placement
    // policy for this table, as the tablet associated with it is shared by
    // multiple tables.
    if (table->colocated()) {
      const Status s = STATUS(InvalidArgument,
          "Placement policy cannot be altered for a colocated table");
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
    }
    // Validate table replication info.
    RETURN_NOT_OK(ValidateTableReplicationInfo(req->replication_info()));
    table_pb.mutable_replication_info()->CopyFrom(req->replication_info());
    has_changes = true;
  }

  // TODO(hector): Simplify the AlterSchema workflow to avoid doing the same checks on every layer
  // this request goes through: https://github.com/YugaByte/yugabyte-db/issues/1882.
  if (req->has_wal_retention_secs()) {
    if (has_changes) {
      const Status s = STATUS(InvalidArgument,
          "wal_retention_secs cannot be altered concurrently with other properties");
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
    }
    // TODO(hector): Handle co-partitioned tables:
    // https://github.com/YugaByte/yugabyte-db/issues/1905.
    table_pb.set_wal_retention_secs(req->wal_retention_secs());
    has_changes = true;
  }

  if (!has_changes) {
    if (req->has_force_send_alter_request() && req->force_send_alter_request()) {
      SendAlterTableRequest(table, req);
    }
    // Skip empty requests...
    return Status::OK();
  }

  // Serialize the schema Increment the version number.
  if (new_schema.initialized()) {
    if (!l->data().pb.has_fully_applied_schema()) {
      // The idea here is that if we are in the middle of updating the schema
      // from one state to another, then YBClients will be given the older
      // version until the schema is updated on all the tablets.
      // As of Dec 2019, this may lead to some rejected operations/retries during
      // the index backfill. See #3284 for possible optimizations.
      MultiStageAlterTable::CopySchemaDetailsToFullyApplied(&table_pb);
    }
    SchemaToPB(new_schema, table_pb.mutable_schema());
  }

  // Only increment the version number if it is a schema change (AddTable change goes through a
  // different path and it's not processed here).
  if (!req->has_wal_retention_secs()) {
    table_pb.set_version(table_pb.version() + 1);
  }
  table_pb.set_next_column_id(next_col_id);
  l->mutable_data()->set_state(
      SysTablesEntryPB::ALTERING,
      Substitute("Alter table version=$0 ts=$1", table_pb.version(), LocalTimeAsString()));

  // Update sys-catalog with the new table schema.
  TRACE("Updating metadata on disk");
  Status s = sys_catalog_->UpdateItem(table.get(), leader_ready_term());
  if (!s.ok()) {
    s = s.CloneAndPrepend(
        Substitute("An error occurred while updating sys-catalog tables entry: $0",
                   s.ToString()));
    LOG(WARNING) << s.ToString();
    if (table->GetTableType() != PGSQL_TABLE_TYPE &&
        (req->has_new_namespace() || req->has_new_table_name())) {
      std::lock_guard<LockType> catalog_lock(lock_);
      VLOG(3) << __func__ << ": Acquired the catalog manager lock_";
      CHECK_EQ(table_names_map_.erase({new_namespace_id, new_table_name}), 1);
    }
    // TableMetadaLock follows RAII paradigm: when it leaves scope,
    // 'l' will be unlocked, and the mutation will be aborted.
    return CheckIfNoLongerLeaderAndSetupError(s, resp);
  }

  // Remove the old name.
  if (req->has_new_namespace() || req->has_new_table_name()) {
    TRACE("Removing (namespace, table) combination ($0, $1) from by-name map",
        namespace_id, table_name);
    std::lock_guard<LockType> l_map(lock_);
    if (table->GetTableType() != PGSQL_TABLE_TYPE &&
        table_names_map_.erase({namespace_id, table_name}) != 1) {
      PANIC_RPC(rpc, "Could not remove table from map, name=" + l->data().name());
    }
  }

  // Update the in-memory state.
  TRACE("Committing in-memory state");
  l->Commit();

  SendAlterTableRequest(table, req);

  LOG(INFO) << "Successfully initiated ALTER TABLE (pending tablet schema updates) for "
            << table->ToString() << " per request from " << RequestorString(rpc);
  return Status::OK();
}

Status CatalogManager::IsAlterTableDone(const IsAlterTableDoneRequestPB* req,
                                        IsAlterTableDoneResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<TableInfo> table;

  // 1. Lookup the table and verify if it exists.
  TRACE("Looking up table");
  RETURN_NOT_OK(FindTable(req->table(), &table));
  if (table == nullptr) {
    Status s = STATUS(NotFound, "The object does not exist", req->table().ShortDebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  TRACE("Locking table");
  auto l = table->LockForRead();
  RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));

  // 2. Verify if the alter is in-progress.
  TRACE("Verify if there is an alter operation in progress for $0", table->ToString());
  resp->set_schema_version(l->data().pb.version());
  resp->set_done(l->data().pb.state() != SysTablesEntryPB::ALTERING);

  return Status::OK();
}

Result<TabletInfo*> CatalogManager::RegisterNewTabletForSplit(
    TabletInfo* source_tablet_info, const PartitionPB& partition,
    TableInfo::lock_type* table_write_lock) {
  const auto tablet_lock = source_tablet_info->LockForRead();

  auto table = source_tablet_info->table();
  TabletInfo* new_tablet;
  {
    std::lock_guard<LockType> l(lock_);
    new_tablet = CreateTabletInfo(table.get(), partition);
  }
  const auto& source_tablet_meta = tablet_lock->data().pb;

  auto& new_tablet_meta = new_tablet->mutable_metadata()->mutable_dirty()->pb;
  new_tablet_meta.set_state(SysTabletsEntryPB::CREATING);
  new_tablet_meta.mutable_committed_consensus_state()->CopyFrom(
      source_tablet_meta.committed_consensus_state());
  new_tablet_meta.set_split_depth(source_tablet_meta.split_depth() + 1);
  new_tablet_meta.set_split_parent_tablet_id(source_tablet_info->tablet_id());
  // TODO(tsplit): consider and handle failure scenarios, for example:
  // - Crash or leader failover before sending out the split tasks.
  // - Long enough partition while trying to send out the splits so that they timeout and
  //   not get executed.
  {
    std::lock_guard<LockType> l(lock_);

    auto& table_pb = table_write_lock->mutable_data()->pb;
    table_pb.set_partitions_version(table_pb.partitions_version() + 1);

    RETURN_NOT_OK(sys_catalog_->UpdateItem(table.get(), leader_ready_term()));
    // If we crash here - we will have new partitions version with the same set of tablets which
    // is harmless.
    // If we first save new_tablet to syscatalog and then crash - we would have table with old
    // partitions version, but new set of tablets which would break invariant that table partitions
    // set is not changed within the same partitions version.
    // TODO: rework this after https://github.com/yugabyte/yugabyte-db/issues/4912 is implemented.
    RETURN_NOT_OK(sys_catalog_->AddItem(new_tablet, leader_ready_term()));

    table->AddTablet(new_tablet);
    // TODO: We use this pattern in other places, but what if concurrent thread accesses not yet
    // committed TabletInfo from the `table` ?
    new_tablet->mutable_metadata()->CommitMutation();

    auto tablet_map_checkout = tablet_map_.CheckOut();
    (*tablet_map_checkout)[new_tablet->id()] = new_tablet;
  }
  LOG(INFO) << "Registered new tablet " << new_tablet->tablet_id()
            << " (" << AsString(partition) << ") to split the tablet "
            << source_tablet_info->tablet_id()
            << " (" << AsString(source_tablet_meta.partition())
            << ") for table " << table->ToString();

  return new_tablet;
}

Status CatalogManager::GetTableSchema(const GetTableSchemaRequestPB* req,
                                      GetTableSchemaResponsePB* resp) {
  VLOG(1) << "Servicing GetTableSchema request for " << req->ShortDebugString();

  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<TableInfo> table;

  // Lookup the table and verify if it exists.
  TRACE("Looking up table");
  RETURN_NOT_OK(FindTable(req->table(), &table));
  if (table == nullptr) {
    Status s = STATUS(NotFound, "The object does not exist", req->table().ShortDebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  TRACE("Locking table");
  auto l = table->LockForRead();
  RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));

  if (l->data().pb.has_fully_applied_schema()) {
    // An AlterTable is in progress; fully_applied_schema is the last
    // schema that has reached every TS.
    DCHECK(l->data().pb.state() == SysTablesEntryPB::ALTERING);
    resp->mutable_schema()->CopyFrom(l->data().pb.fully_applied_schema());
    resp->set_version(l->data().pb.fully_applied_schema_version());
    resp->mutable_indexes()->CopyFrom(l->data().pb.fully_applied_indexes());
    if (l->data().pb.has_fully_applied_index_info()) {
      resp->set_obsolete_indexed_table_id(PROTO_GET_INDEXED_TABLE_ID(l->data().pb));
      *resp->mutable_index_info() = l->data().pb.fully_applied_index_info();
    }
    VLOG(1) << "Returning"
            << "\nfully_applied_schema with version "
            << l->data().pb.fully_applied_schema_version()
            << ":\n"
            << yb::ToString(l->data().pb.fully_applied_indexes())
            << "\ninstead of schema with version "
            << l->data().pb.version()
            << ":\n"
            << yb::ToString(l->data().pb.indexes());
  } else {
    // There's no AlterTable, the regular schema is "fully applied".
    resp->mutable_schema()->CopyFrom(l->data().pb.schema());
    resp->set_version(l->data().pb.version());
    resp->mutable_indexes()->CopyFrom(l->data().pb.indexes());
    if (l->data().pb.has_index_info()) {
      resp->set_obsolete_indexed_table_id(PROTO_GET_INDEXED_TABLE_ID(l->data().pb));
      *resp->mutable_index_info() = l->data().pb.index_info();
    }
    VLOG(3) << "Returning"
            << "\nschema with version "
            << l->data().pb.version()
            << ":\n"
            << yb::ToString(l->data().pb.indexes());
  }
  resp->mutable_partition_schema()->CopyFrom(l->data().pb.partition_schema());
  if (IsReplicationInfoSet(l->data().pb.replication_info())) {
    resp->mutable_replication_info()->CopyFrom(l->data().pb.replication_info());
  }
  resp->set_create_table_done(!table->IsCreateInProgress());
  resp->set_table_type(table->metadata().state().pb.table_type());
  resp->mutable_identifier()->set_table_name(l->data().pb.name());
  resp->mutable_identifier()->set_table_id(table->id());
  resp->mutable_identifier()->mutable_namespace_()->set_id(table->namespace_id());
  NamespaceIdentifierPB nsid;
  nsid.set_id(table->namespace_id());
  scoped_refptr<NamespaceInfo> nsinfo;
  if (FindNamespace(nsid, &nsinfo).ok()) {
    resp->mutable_identifier()->mutable_namespace_()->set_name(nsinfo->name());
  }

  // Get namespace name by id.
  SharedLock<LockType> l_map(lock_);
  TRACE("Looking up namespace");
  const scoped_refptr<NamespaceInfo> ns = FindPtrOrNull(namespace_ids_map_, table->namespace_id());

  if (ns == nullptr) {
    Status s = STATUS_SUBSTITUTE(
        NotFound, "Could not find namespace by namespace id $0 for request $1.",
        table->namespace_id(), req->DebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
  }

  resp->mutable_identifier()->mutable_namespace_()->set_name(ns->name());

  resp->set_colocated(table->colocated());

  VLOG(1) << "Serviced GetTableSchema request for " << req->ShortDebugString() << " with "
          << yb::ToString(*resp);
  return Status::OK();
}

Status CatalogManager::GetColocatedTabletSchema(const GetColocatedTabletSchemaRequestPB* req,
                                                GetColocatedTabletSchemaResponsePB* resp) {
  VLOG(1) << "Servicing GetColocatedTabletSchema request for " << req->ShortDebugString();
  RETURN_NOT_OK(CheckOnline());

  // Lookup the given parent colocated table and verify if it exists.
  scoped_refptr<TableInfo> parent_colocated_table;
  {
    TRACE("Looking up table");
    RETURN_NOT_OK(FindTable(req->parent_colocated_table(), &parent_colocated_table));
    TRACE("Locking table");
    auto l = parent_colocated_table->LockForRead();
    RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));
  }

  if (!parent_colocated_table->colocated() || !IsColocatedParentTable(*parent_colocated_table)) {
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_TABLE_TYPE,
                      STATUS(InvalidArgument, "Table provided is not a parent colocated table"));
  }

  // Next get all the user tables that are in the database.
  ListTablesRequestPB listTablesReq;
  ListTablesResponsePB ListTablesResp;

  listTablesReq.mutable_namespace_()->set_id(parent_colocated_table->namespace_id());
  listTablesReq.mutable_namespace_()->set_database_type(YQL_DATABASE_PGSQL);
  listTablesReq.set_exclude_system_tables(true);
  Status status = ListTables(&listTablesReq, &ListTablesResp);
  if (!status.ok() || ListTablesResp.has_error()) {
    LOG(ERROR) << "Error while listing tables: " << status;
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, status);
  }

  // Get the table schema for each colocated table.
  for (const auto& t : ListTablesResp.tables()) {
    // Need to check if this table is colocated first.
    scoped_refptr<TableInfo> table;
    TableIdentifierPB t_pb;
    t_pb.set_table_id(t.id());
    TRACE("Looking up table");
    RETURN_NOT_OK(FindTable(t_pb, &table));

    if (table->colocated()) {
      // Now we can get the schema for this table.
      GetTableSchemaRequestPB schemaReq;
      GetTableSchemaResponsePB schemaResp;
      schemaReq.mutable_table()->Swap(&t_pb);
      status = GetTableSchema(&schemaReq, &schemaResp);
      if (!status.ok() || schemaResp.has_error()) {
        LOG(ERROR) << "Error while getting table schema: " << status;
        return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, status);
      }
      resp->add_get_table_schema_response_pbs()->Swap(&schemaResp);
    }
  }

  return Status::OK();
}

Status CatalogManager::ListTables(const ListTablesRequestPB* req,
                                  ListTablesResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  NamespaceId namespace_id;

  // Validate namespace.
  if (req->has_namespace_()) {
    scoped_refptr<NamespaceInfo> ns;

    // Lookup the namespace and verify if it exists.
    RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->namespace_(), &ns), resp);

    auto ns_lock = ns->LockForRead();
    namespace_id = ns->id();

    // Don't list tables with a namespace that isn't running.
    if (ns->state() != SysNamespaceEntryPB::RUNNING) {
      LOG(INFO) << "ListTables request for a Namespace not running (State="
                << SysNamespaceEntryPB::State_Name(ns->state()) << ")";
      return Status::OK();
    }
  }

  bool has_rel_filter = req->relation_type_filter_size() > 0;
  bool include_user_table = has_rel_filter ? false : true;
  bool include_user_index = has_rel_filter ? false : true;
  bool include_system_table = req->exclude_system_tables() ? false
      : (has_rel_filter ? false : true);

  for (const auto &relation : req->relation_type_filter()) {
    if (relation == SYSTEM_TABLE_RELATION) {
      include_system_table = true;
    } else if (relation == USER_TABLE_RELATION) {
      include_user_table = true;
    } else if (relation == INDEX_TABLE_RELATION) {
      include_user_index = true;
    }
  }

  SharedLock<LockType> l(lock_);
  RelationType relation_type;

  for (const auto& entry : *table_ids_map_) {
    auto& table_info = *entry.second;
    auto ltm = table_info.LockForRead();

    if (!ltm->data().is_running()) continue;

    if (!namespace_id.empty() && namespace_id != table_info.namespace_id()) {
      continue; // Skip tables from other namespaces.
    }

    if (req->has_name_filter()) {
      size_t found = ltm->data().name().find(req->name_filter());
      if (found == string::npos) {
        continue;
      }
    }

    if (IsUserIndexUnlocked(table_info)) {
      if (!include_user_index) {
        continue;
      }
      relation_type = INDEX_TABLE_RELATION;
    } else if (IsUserTableUnlocked(table_info)) {
      if (!include_user_table) {
        continue;
      }
      relation_type = USER_TABLE_RELATION;
    } else {
      if (!include_system_table) {
        continue;
      }
      relation_type = SYSTEM_TABLE_RELATION;
    }

    scoped_refptr<NamespaceInfo> ns;
    NamespaceIdentifierPB ns_identifier;
    ns_identifier.set_id(ltm->data().namespace_id());
    auto s = FindNamespaceUnlocked(ns_identifier, &ns);
    if (ns.get() == nullptr || ns->state() != SysNamespaceEntryPB::RUNNING) {
      if (PREDICT_FALSE(FLAGS_TEST_return_error_if_namespace_not_found)) {
        RETURN_NAMESPACE_NOT_FOUND(s, resp);
      }
      LOG(ERROR) << "Unable to find namespace with id " << ltm->data().namespace_id()
                 << " for table " << ltm->data().name();
      continue;
    }

    ListTablesResponsePB::TableInfo *table = resp->add_tables();
    {
      auto l = ns->LockForRead();
      table->mutable_namespace_()->set_id(ns->id());
      table->mutable_namespace_()->set_name(ns->name());
      table->mutable_namespace_()->set_database_type(ns->database_type());
    }
    table->set_id(entry.second->id());
    table->set_name(ltm->data().name());
    table->set_table_type(ltm->data().table_type());
    table->set_relation_type(relation_type);
  }
  return Status::OK();
}

scoped_refptr<TableInfo> CatalogManager::GetTableInfo(const TableId& table_id) {
  SharedLock<LockType> l(lock_);
  return FindPtrOrNull(*table_ids_map_, table_id);
}

scoped_refptr<TableInfo> CatalogManager::GetTableInfoFromNamespaceNameAndTableName(
    YQLDatabase db_type, const NamespaceName& namespace_name, const TableName& table_name) {
  SharedLock<LockType> l(lock_);
  const auto ns = FindPtrOrNull(namespace_names_mapper_[db_type], namespace_name);
  return ns
    ? FindPtrOrNull(table_names_map_, {ns->id(), table_name})
    : nullptr;
}

scoped_refptr<TableInfo> CatalogManager::GetTableInfoUnlocked(const TableId& table_id) {
  return FindPtrOrNull(*table_ids_map_, table_id);
}

void CatalogManager::GetAllTables(std::vector<scoped_refptr<TableInfo>> *tables,
                                  bool includeOnlyRunningTables) {
  tables->clear();
  SharedLock<LockType> l(lock_);
  for (const auto& e : *table_ids_map_) {
    if (includeOnlyRunningTables && !e.second->is_running()) {
      continue;
    }
    tables->push_back(e.second);
  }
}

void CatalogManager::GetAllNamespaces(std::vector<scoped_refptr<NamespaceInfo>>* namespaces,
                                      bool includeOnlyRunningNamespaces) {
  namespaces->clear();
  SharedLock<LockType> l(lock_);
  for (const NamespaceInfoMap::value_type& e : namespace_ids_map_) {
    if (includeOnlyRunningNamespaces && e.second->state() != SysNamespaceEntryPB::RUNNING) {
      continue;
    }
    namespaces->push_back(e.second);
  }
}

void CatalogManager::GetAllUDTypes(std::vector<scoped_refptr<UDTypeInfo>>* types) {
  types->clear();
  SharedLock<LockType> l(lock_);
  for (const UDTypeInfoMap::value_type& e : udtype_ids_map_) {
    types->push_back(e.second);
  }
}

std::vector<std::shared_ptr<MonitoredTask>> CatalogManager::GetRecentTasks() {
  return tasks_tracker_->GetTasks();
}

std::vector<std::shared_ptr<MonitoredTask>> CatalogManager::GetRecentJobs() {
  return jobs_tracker_->GetTasks();
}

NamespaceName CatalogManager::GetNamespaceNameUnlocked(const NamespaceId& id) const  {
  const scoped_refptr<NamespaceInfo> ns = FindPtrOrNull(namespace_ids_map_, id);
  return ns == nullptr ? NamespaceName() : ns->name();
}

NamespaceName CatalogManager::GetNamespaceName(const NamespaceId& id) const {
  TRACE("Acquired catalog manager lock");
  SharedLock<LockType> l(lock_);
  return GetNamespaceNameUnlocked(id);
}

NamespaceName CatalogManager::GetNamespaceNameUnlocked(
    const scoped_refptr<TableInfo>& table) const  {
  return GetNamespaceNameUnlocked(table->namespace_id());
}

NamespaceName CatalogManager::GetNamespaceName(const scoped_refptr<TableInfo>& table) const {
  return GetNamespaceName(table->namespace_id());
}

bool CatalogManager::IsSystemTable(const TableInfo& table) const {
  return table.is_system();
}

// True if table is created by user.
// Table can be regular table or index in this case.
bool CatalogManager::IsUserCreatedTable(const TableInfo& table) const {
  SharedLock<LockType> l(lock_);
  return IsUserCreatedTableUnlocked(table);
}

bool CatalogManager::IsUserCreatedTableUnlocked(const TableInfo& table) const {
  if (table.GetTableType() == PGSQL_TABLE_TYPE || table.GetTableType() == YQL_TABLE_TYPE) {
    if (!IsSystemTable(table) && !IsSequencesSystemTable(table) &&
        GetNamespaceNameUnlocked(table.namespace_id()) != kSystemNamespaceName &&
        !IsColocatedParentTable(table) &&
        !IsTablegroupParentTable(table)) {
      return true;
    }
  }
  return false;
}

bool CatalogManager::IsUserTable(const TableInfo& table) const {
  SharedLock<LockType> l(lock_);
  return IsUserTableUnlocked(table);
}

bool CatalogManager::IsUserTableUnlocked(const TableInfo& table) const {
  return IsUserCreatedTableUnlocked(table) && table.indexed_table_id().empty();
}

bool CatalogManager::IsUserIndex(const TableInfo& table) const {
  SharedLock<LockType> l(lock_);
  return IsUserIndexUnlocked(table);
}

bool CatalogManager::IsUserIndexUnlocked(const TableInfo& table) const {
  return IsUserCreatedTableUnlocked(table) && !table.indexed_table_id().empty();
}

bool CatalogManager::IsColocatedParentTableId(const TableId& table_id) const {
  return table_id.find(kColocatedParentTableIdSuffix) != std::string::npos;
}

bool CatalogManager::IsColocatedParentTable(const TableInfo& table) const {
  return IsColocatedParentTableId(table.id());
}

bool CatalogManager::IsTablegroupParentTable(const TableInfo& table) const {
  return table.id().find(kTablegroupParentTableIdSuffix) != std::string::npos;
}

bool CatalogManager::IsColocatedUserTable(const TableInfo& table) const {
  return table.colocated() && !IsColocatedParentTable(table)
                           && !IsTablegroupParentTable(table);
}

bool CatalogManager::IsSequencesSystemTable(const TableInfo& table) const {
  if (table.GetTableType() == PGSQL_TABLE_TYPE && !IsColocatedParentTable(table)
                                               && !IsTablegroupParentTable(table)) {
    // This case commonly occurs during unit testing. Avoid unnecessary assert within Get().
    if (!IsPgsqlId(table.namespace_id()) || !IsPgsqlId(table.id())) {
      LOG(WARNING) << "Not PGSQL IDs " << table.namespace_id() << ", " << table.id();
      return false;
    }
    Result<uint32_t> database_oid = GetPgsqlDatabaseOid(table.namespace_id());
    if (!database_oid.ok()) {
      LOG(WARNING) << "Invalid Namespace ID " << table.namespace_id();
      return false;
    }
    Result<uint32_t> table_oid = GetPgsqlTableOid(table.id());
    if (!table_oid.ok()) {
      LOG(WARNING) << "Invalid Table ID " << table.id();
      return false;
    }
    if (*database_oid == kPgSequencesDataDatabaseOid && *table_oid == kPgSequencesDataTableOid) {
      return true;
    }
  }
  return false;
}

void CatalogManager::NotifyTabletDeleteFinished(const TabletServerId& tserver_uuid,
                                                const TabletId& tablet_id,
                                                scoped_refptr<TableInfo> table) {
  shared_ptr<TSDescriptor> ts_desc;
  if (!master_->ts_manager()->LookupTSByUUID(tserver_uuid, &ts_desc)) {
    LOG(WARNING) << "Unable to find tablet server " << tserver_uuid;
  } else if (!ts_desc->IsTabletDeletePending(tablet_id)) {
    LOG(WARNING) << "Pending delete for tablet " << tablet_id << " in ts "
                 << tserver_uuid << " doesn't exist";
  } else {
    LOG(INFO) << "Clearing pending delete for tablet " << tablet_id << " in ts " << tserver_uuid;
    ts_desc->ClearPendingTabletDelete(tablet_id);
  }
  if (FLAGS_master_drop_table_after_task_response) {
    // Since this is called after every successful async DeleteTablet, it's possible if all tasks
    // complete, for us to mark the table as DELETED asap. This is desirable as clients will wait
    // for this before returning success to the user.
    //
    // However, if tasks fail, timeout, or are aborted, we still have the background thread as a
    // catch all.
    auto lock = MaybeTransitionTableToDeleted(table);
    if (!lock) {
      return;
    }
    vector<TableInfo*> tables_to_delete({table.get()});
    Status s = sys_catalog_->UpdateItems(tables_to_delete, leader_ready_term());
    if (!s.ok()) {
      LOG(WARNING) << "Error marking table as DELETED: " << s.ToString();
      return;
    }
    lock->Commit();
  }
}

bool CatalogManager::ReplicaMapDiffersFromConsensusState(const scoped_refptr<TabletInfo>& tablet,
                                                         const ConsensusStatePB& cstate) {
  auto locs = tablet->GetReplicaLocations();
  if (locs->size() != cstate.config().peers_size()) {
    return true;
  }
  for (auto iter = cstate.config().peers().begin(); iter != cstate.config().peers().end(); iter++) {
      if (locs->find(iter->permanent_uuid()) == locs->end()) {
        return true;
      }
  }
  return false;
}

Status CatalogManager::ProcessTabletReport(TSDescriptor* ts_desc,
                                           const TabletReportPB& full_report,
                                           TabletReportUpdatesPB* full_report_update,
                                           RpcContext* rpc) {
  int num_tablets = full_report.updated_tablets_size();
  TRACE_EVENT2("master", "ProcessTabletReport",
               "requestor", rpc->requestor_string(),
               "num_tablets", num_tablets);

  if (VLOG_IS_ON(2)) {
    VLOG(2) << "Received tablet report from " << RequestorString(rpc) << "("
            << ts_desc->permanent_uuid() << "): " << full_report.DebugString();
  }

  if (!ts_desc->has_tablet_report() && full_report.is_incremental()) {
    string msg = "Received an incremental tablet report when a full one was needed";
    LOG(WARNING) << "Invalid tablet report from " << ts_desc->permanent_uuid() << ": " << msg;
    // We should respond with success in order to send reply that we need full report.
    return Status::OK();
  }

  // TODO: on a full tablet report, we may want to iterate over the tablets we think
  // the server should have, compare vs the ones being reported, and somehow mark
  // any that have been "lost" (eg somehow the tablet metadata got corrupted or something).

  // Maps a tablet ID to its corresponding tablet report (owned by 'full_report').
  map<TabletId, const ReportedTabletPB*> reports;

  // Maps a tablet ID to its corresponding TabletInfo.
  map<TabletId, scoped_refptr<TabletInfo>> tablet_infos;

  // Tablet Deletes to process after the catalog lock below.
  set<TabletId> tablets_to_delete;

  {
    // Lock the catalog to iterate over tablet_ids_map_ & table_ids_map_.
    SharedLock<LockType> catalog_lock(lock_);

    // Fill the above variables before processing
    full_report_update->mutable_tablets()->Reserve(num_tablets);
    for (const ReportedTabletPB& report : full_report.updated_tablets()) {
      const string& tablet_id = report.tablet_id();

      // 1a. Find the tablet, deleting/skipping it if it can't be found.
      scoped_refptr<TabletInfo> tablet = FindPtrOrNull(*tablet_map_, tablet_id);
      if (!tablet) {
        // It'd be unsafe to ask the tserver to delete this tablet without first
        // replicating something to our followers (i.e. to guarantee that we're
        // the leader). For example, if we were a rogue master, we might be
        // deleting a tablet created by a new master accidentally. But masters
        // retain metadata for deleted tablets forever, so a tablet can only be
        // truly unknown in the event of a serious misconfiguration, such as a
        // tserver heartbeating to the wrong cluster. Therefore, it should be
        // reasonable to ignore it and wait for an operator fix the situation.
        if (FLAGS_master_ignore_deleted_on_load &&
            report.tablet_data_state() == TABLET_DATA_DELETED) {
          VLOG(1) << "Ignoring report from unknown tablet " << tablet_id;
        } else {
          LOG(WARNING) << "Ignoring report from unknown tablet " << tablet_id;
        }
        // Every tablet in the report that is processed gets a heartbeat response entry.
        ReportedTabletUpdatesPB* update = full_report_update->add_tablets();
        update->set_tablet_id(tablet_id);
        continue;
      }
      if (!tablet->table() || FindOrNull(*table_ids_map_, tablet->table()->id()) == nullptr) {
        auto table_id = tablet->table() == nullptr ? "(null)" : tablet->table()->id();
        LOG(INFO) << "Got report from an orphaned tablet " << tablet_id << " on table " << table_id;
        tablets_to_delete.insert(tablet_id);
        // Every tablet in the report that is processed gets a heartbeat response entry.
        ReportedTabletUpdatesPB* update = full_report_update->add_tablets();
        update->set_tablet_id(tablet_id);
        continue;
      }

      // 1b. Found the tablet, update local state. If multiple tablets with the
      // same ID are in the report, all but the last one will be ignored.
      reports[tablet_id] = &report;
      tablet_infos[tablet_id] = tablet;
    }
  }

  // Process any delete requests from orphaned tablets, identified above.
  for (auto tablet_id : tablets_to_delete) {
    SendDeleteTabletRequest(tablet_id, TABLET_DATA_DELETED, boost::none, nullptr, ts_desc,
        "Report from an orphaned tablet");
  }

  // Calculate the deadline for this expensive loop coming up.
  const auto safe_deadline = rpc->GetClientDeadline() -
    (FLAGS_heartbeat_rpc_timeout_ms * 1ms * FLAGS_heartbeat_safe_deadline_ratio);

  // Doing batched processing with inner 'for' loops.  Ensure we iterate all tablets with 'while'.
  auto tablet_iter = tablet_infos.begin();
  while (tablet_iter != tablet_infos.end()) {
    // Keeps track of all RPCs that should be sent when we're done with a single batch.
    vector<shared_ptr<RetryingTSRpcTask>> rpcs;

    // 2a. First Pass. Iterate in TabletId Order to discover all Table locks we'll need.
    //     Need to acquire both types of locks in Id order to prevent deadlock.
    map<TableId, unique_ptr<TableInfo::lock_type>> table_read_locks; // used for unlock.
    map<TabletId, unique_ptr<TabletInfo::lock_type>> tablet_write_locks; // used for unlock.
    {
      map<TableId, scoped_refptr<TableInfo>> tables_to_lock;
      auto tablet_iter_for_table_locks = tablet_iter;
      for (auto i = 0;
          i < FLAGS_catalog_manager_report_batch_size
            && tablet_iter_for_table_locks != tablet_infos.end();
          ++i, ++tablet_iter_for_table_locks) {
        const scoped_refptr<TabletInfo>& tablet = tablet_iter_for_table_locks->second;
        const scoped_refptr<TableInfo>& table = tablet->table();
        tables_to_lock[table->id()] = table;
      }
      for (auto& id_and_table : tables_to_lock) {
        table_read_locks[id_and_table.first] = id_and_table.second->LockForRead();
      }
    }
    // 2b. Second Pass.  Process each tablet. This may not be in the order that the tablets
    // appear in 'full_report', but that has no bearing on correctness.
    vector<TabletInfo*> mutated_tablets; // refcount protected by 'tablet_infos'
    auto tablet_iter_for_schema_changes = tablet_iter;
    for (auto i = 0;
         i < FLAGS_catalog_manager_report_batch_size && tablet_iter != tablet_infos.end();
         ++i, ++tablet_iter) {
      const string& tablet_id = tablet_iter->first;
      const scoped_refptr<TabletInfo>& tablet = tablet_iter->second;
      const scoped_refptr<TableInfo>& table = tablet->table();
      const ReportedTabletPB& report = *FindOrDie(reports, tablet_id);

      // Prepare an heartbeat response entry for this tablet, now that we're going to process it.
      // Every tablet in the report that is processed gets one, even if there are no changes to it.
      ReportedTabletUpdatesPB* update = full_report_update->add_tablets();
      update->set_tablet_id(tablet_id);

      // Get tablet lock on demand.  This works in the batch case because the loop is ordered.
      tablet_write_locks[tablet_id] = tablet->LockForWrite();
      auto& table_lock = table_read_locks[table->id()];
      auto& tablet_lock = tablet_write_locks[tablet_id];

      TRACE_EVENT1("master", "HandleReportedTablet", "tablet_id", report.tablet_id());
      RETURN_NOT_OK_PREPEND(CheckIsLeaderAndReady(),
          Substitute("This master is no longer the leader, unable to handle report for tablet $0",
              tablet_id));

      VLOG(3) << "tablet report: " << report.ShortDebugString();

      // 3. Delete the tablet if it (or its table) have been deleted.
      if (tablet_lock->data().is_deleted() ||
          table_lock->data().started_deleting()) {
        const string msg = tablet_lock->data().pb.state_msg();
        update->set_state_msg(msg);
        LOG(INFO) << "Got report from deleted tablet " << tablet->ToString()
                  << " (" << msg << "): Sending delete request for this tablet";
        // TODO(unknown): Cancel tablet creation, instead of deleting, in cases
        // where that might be possible (tablet creation timeout & replacement).
        rpcs.emplace_back(std::make_shared<AsyncDeleteReplica>(
            master_, AsyncTaskPool(), ts_desc->permanent_uuid(), table, tablet_id,
            TABLET_DATA_DELETED, boost::none, msg));
        continue;
      }

      if (!table_lock->data().is_running()) {
        const string msg = tablet_lock->data().pb.state_msg();
        LOG(INFO) << "Got report from tablet " << tablet->tablet_id()
                  << " for non-running table " << table->ToString() << ": " << msg;
        update->set_state_msg(msg);
        continue;
      }

      // 4. Tombstone a replica that is no longer part of the Raft config (and
      // not already tombstoned or deleted outright).
      //
      // If the report includes a committed raft config, we only tombstone if
      // the opid_index is strictly less than the latest reported committed
      // config. This prevents us from spuriously deleting replicas that have
      // just been added to the committed config and are in the process of copying.
      const ConsensusStatePB &prev_cstate = tablet_lock->data().pb.committed_consensus_state();
      const int64_t prev_opid_index = prev_cstate.config().opid_index();
      const int64_t report_opid_index = (report.has_committed_consensus_state() &&
          report.committed_consensus_state().config().has_opid_index()) ?
            report.committed_consensus_state().config().opid_index() :
            consensus::kInvalidOpIdIndex;
      if (FLAGS_master_tombstone_evicted_tablet_replicas &&
          report.tablet_data_state() != TABLET_DATA_TOMBSTONED &&
          report.tablet_data_state() != TABLET_DATA_DELETED &&
          report_opid_index < prev_opid_index &&
          !IsRaftConfigMember(ts_desc->permanent_uuid(), prev_cstate.config())) {
        const string delete_msg = (report_opid_index == consensus::kInvalidOpIdIndex) ?
            "Replica has no consensus available" :
            Substitute("Replica with old config index $0", report_opid_index);
        rpcs.emplace_back(std::make_shared<AsyncDeleteReplica>(
            master_, AsyncTaskPool(), ts_desc->permanent_uuid(), table, tablet_id,
            TABLET_DATA_TOMBSTONED, prev_opid_index,
            Substitute("$0 (current committed config index is $1)",
                delete_msg, prev_opid_index)));
        continue;
      }

      // 5. Skip a non-deleted tablet which reports an error.
      if (report.has_error()) {
        Status s = StatusFromPB(report.error());
        DCHECK(!s.ok());
        DCHECK_EQ(report.state(), tablet::FAILED);
        LOG(WARNING) << "Tablet " << tablet->ToString() << " has failed on TS "
                     << ts_desc->permanent_uuid() << ": " << s.ToString();
        continue;
      }

      // 6. Process the report's consensus state.
      // The report will not have a committed_consensus_state if it is in the
      // middle of starting up, such as during tablet bootstrap.
      // If we received an incremental report, and the tablet is starting up, we will update the
      // replica so that the balancer knows how many tablets are in the middle of remote bootstrap.
      if (report.has_committed_consensus_state()) {
        ConsensusStatePB cstate = report.committed_consensus_state();
        bool tablet_was_mutated = false;

        // 6a. The master only processes reports for replicas with committed
        // consensus configurations since it needs the committed index to only
        // cache the most up-to-date config. Since it's possible for TOMBSTONED
        // replicas with no ConsensusMetadata on disk to be reported as having no
        // committed config opid_index, we skip over those replicas.
        if (!cstate.config().has_opid_index()) {
          LOG(WARNING) << "Missing opid_index in reported config:\n" << report.DebugString();
          continue;
        }
        if (PREDICT_TRUE(FLAGS_master_ignore_stale_cstate) &&
              (cstate.current_term() < prev_cstate.current_term() ||
               report_opid_index < prev_opid_index)) {
          LOG(WARNING) << "Stale heartbeat for Tablet " << tablet->ToString()
                       << " on TS " << ts_desc->permanent_uuid()
                       << "cstate=" << cstate.ShortDebugString()
                       << ", prev_cstate=" << prev_cstate.ShortDebugString();
          continue;
        }

        // 6b. Disregard the leader state if the reported leader is not a member
        // of the committed config.
        if (cstate.leader_uuid().empty() ||
            !IsRaftConfigMember(cstate.leader_uuid(), cstate.config())) {
          cstate.clear_leader_uuid();
          tablet_was_mutated = true;
        }

        // 6c. Mark the tablet as RUNNING if it makes sense to do so.
        //
        // We need to wait for a leader before marking a tablet as RUNNING, or
        // else we could incorrectly consider a tablet created when only a
        // minority of its replicas were successful. In that case, the tablet
        // would be stuck in this bad state forever.
        // - FLAG added to avoid waiting during mock tests.
        if (!tablet_lock->data().is_running() &&
            report.state() == tablet::RUNNING &&
              (cstate.has_leader_uuid() ||
              !FLAGS_catalog_manager_wait_for_new_tablets_to_elect_leader)) {
          DCHECK_EQ(SysTabletsEntryPB::CREATING, tablet_lock->data().pb.state())
              << "Tablet in unexpected state: " << tablet->ToString()
              << ": " << tablet_lock->data().pb.ShortDebugString();
          VLOG(1) << "Tablet " << tablet->ToString() << " is now online";
          tablet_lock->mutable_data()->set_state(SysTabletsEntryPB::RUNNING,
              "Tablet reported with an active leader");
          tablet_was_mutated = true;
        }

        // 6d. Update the consensus state if:
        // - A config change operation was committed (reflected by a change to
        //   the committed config's opid_index).
        // - The new cstate has a leader, and either the old cstate didn't, or
        //   there was a term change.
        if (cstate.config().opid_index() > prev_cstate.config().opid_index() ||
            (cstate.has_leader_uuid() &&
                (!prev_cstate.has_leader_uuid() ||
                    cstate.current_term() > prev_cstate.current_term()))) {

          // 6d(i). Retain knowledge of the leader even if it wasn't reported in
          // the latest config.
          //
          // When a config change is reported to the master, it may not include the
          // leader because the follower doing the reporting may not know who the
          // leader is yet (it may have just started up). It is safe to reuse
          // the previous leader if the reported cstate has the same term as the
          // previous cstate, and the leader was known for that term.
          if (cstate.current_term() == prev_cstate.current_term()) {
            if (!cstate.has_leader_uuid() && prev_cstate.has_leader_uuid()) {
              cstate.set_leader_uuid(prev_cstate.leader_uuid());
              // Sanity check to detect consensus divergence bugs.
            } else if (cstate.has_leader_uuid() && prev_cstate.has_leader_uuid() &&
                cstate.leader_uuid() != prev_cstate.leader_uuid()) {
              string msg = Substitute("Previously reported cstate for tablet $0 gave "
                                      "a different leader for term $1 than the current cstate. "
                                      "Previous cstate: $2. Current cstate: $3.",
                  tablet->ToString(), cstate.current_term(),
                  prev_cstate.ShortDebugString(), cstate.ShortDebugString());
              LOG(DFATAL) << msg;
              continue;
            }
          }

          // 6d(ii). Delete any replicas from the previous config that are not in the new one.
          if (FLAGS_master_tombstone_evicted_tablet_replicas) {
            unordered_set<string> current_member_uuids;
            for (const consensus::RaftPeerPB &peer : cstate.config().peers()) {
              InsertOrDie(&current_member_uuids, peer.permanent_uuid());
            }
            for (const consensus::RaftPeerPB &prev_peer : prev_cstate.config().peers()) {
              const string& peer_uuid = prev_peer.permanent_uuid();
              if (!ContainsKey(current_member_uuids, peer_uuid)) {
                // Don't delete a tablet server that hasn't reported in yet (Bootstrapping).
                shared_ptr<TSDescriptor> ts_desc;
                if (!master_->ts_manager()->LookupTSByUUID(peer_uuid, &ts_desc)) {
                  continue;
                }
                // Otherwise, the TabletServer needs to remove this peer.
                rpcs.emplace_back(std::make_shared<AsyncDeleteReplica>(
                    master_, AsyncTaskPool(), peer_uuid, table, tablet_id,
                    TABLET_DATA_TOMBSTONED, prev_cstate.config().opid_index(),
                    Substitute("TS $0 not found in new config with opid_index $1",
                        peer_uuid, cstate.config().opid_index())));
              }
            }
          }
          // 6d(iii). Update the in-memory ReplicaLocations for this tablet using the new config.
          VLOG(2) << "Updating replicas for tablet " << tablet_id
                << " using config reported by " << ts_desc->permanent_uuid()
                << " to that committed in log index " << cstate.config().opid_index()
                << " with leader state from term " << cstate.current_term();
          ReconcileTabletReplicasInLocalMemoryWithReport(
            tablet, ts_desc->permanent_uuid(), cstate, report);

          // 6d(iv). Update the consensus state. Don't use 'prev_cstate' after this.
          LOG(INFO) << "Tablet: " << tablet->tablet_id() << " reported consensus state change."
                    << " New consensus state: " << cstate.ShortDebugString()
                    << " from " << ts_desc->permanent_uuid();
          DCHECK(tablet_lock->is_write_locked());
          *tablet_lock->mutable_data()->pb.mutable_committed_consensus_state() = cstate;
          tablet_was_mutated = true;
        } else {
          // Report opid_index is equal to the previous opid_index. If some
          // replica is reporting the same consensus configuration we already know about, but we
          // haven't yet heard from all the tservers in the config, update the in-memory
          // ReplicaLocations.
          LOG(INFO) << "Peer " << ts_desc->permanent_uuid() << " sent "
                    << (full_report.is_incremental() ? "incremental" : "full tablet")
                    << " report for " << tablet->tablet_id()
                    << ", prev state op id: " << prev_cstate.config().opid_index()
                    << ", prev state term: " << prev_cstate.current_term()
                    << ", prev state has_leader_uuid: " << prev_cstate.has_leader_uuid()
                    << ". Consensus state: " << cstate.ShortDebugString();
          if (GetAtomicFlag(&FLAGS_enable_register_ts_from_raft) &&
              ReplicaMapDiffersFromConsensusState(tablet, cstate)) {
             ReconcileTabletReplicasInLocalMemoryWithReport(
               tablet, ts_desc->permanent_uuid(), cstate, report);
          } else {
            UpdateTabletReplicaInLocalMemory(ts_desc, &cstate, report, tablet);
          }
        }

        if (FLAGS_use_create_table_leader_hint &&
            !cstate.has_leader_uuid() && cstate.current_term() == 0) {
          StartElectionIfReady(cstate, tablet.get());
        }

        // 7. Send an AlterSchema RPC if the tablet has an old schema version.
        if (report.has_schema_version() &&
            report.schema_version() != table_lock->data().pb.version()) {
          if (report.schema_version() > table_lock->data().pb.version()) {
            LOG(ERROR) << "TS " << ts_desc->permanent_uuid()
                       << " has reported a schema version greater than the current one "
                       << " for tablet " << tablet->ToString()
                       << ". Expected version " << table_lock->data().pb.version()
                       << " got " << report.schema_version()
                       << " (corruption)";
          } else {
            // TODO: For Alter (rolling apply to tablets), this is an expected transitory state.
            LOG(INFO) << "TS " << ts_desc->permanent_uuid()
                      << " does not have the latest schema for tablet " << tablet->ToString()
                      << ". Expected version " << table_lock->data().pb.version()
                      << " got " << report.schema_version();
          }
          // It's possible that the tablet being reported is a laggy replica, and in fact
          // the leader has already received an AlterTable RPC. That's OK, though --
          // it'll safely ignore it if we send another.
          rpcs.emplace_back(std::make_shared<AsyncAlterTable>(master_, AsyncTaskPool(), tablet));
        }

        // 8. If the tablet was mutated, add it to the tablets to be re-persisted.
        //
        // Done here and not on a per-mutation basis to avoid duplicate entries.
        if (tablet_was_mutated) {
          mutated_tablets.push_back(tablet.get());
        }
      } else if (full_report.is_incremental() &&
          (report.state() == tablet::NOT_STARTED || report.state() == tablet::BOOTSTRAPPING)) {
        // When a tablet server is restarted, it sends a full tablet report with all of its tablets
        // in the NOT_STARTED state, so this would make the load balancer think that all the
        // tablets are being remote bootstrapped at once, so only process incremental reports here.
        UpdateTabletReplicaInLocalMemory(ts_desc, nullptr /* consensus */, report, tablet);
      }
    } // Finished one round of batch processing.

    // 9. Unlock the tables; we no longer need to access their state.
    for (auto& l : table_read_locks) {
      l.second->Unlock();
    }
    table_read_locks.clear();

    // 10. Write all tablet mutations to the catalog table.
    //
    // SysCatalogTable::Write will short-circuit the case where the data has not
    // in fact changed since the previous version and avoid any unnecessary mutations.
    if (!mutated_tablets.empty()) {
      Status s = sys_catalog_->UpdateItems(mutated_tablets, leader_ready_term());
      if (!s.ok()) {
        LOG(WARNING) << "Error updating tablets: " << s.ToString() << ". Tablet report was: "
                     << full_report.ShortDebugString();
        return s;
      }
    }

    // 11. Publish the in-memory tablet mutations and release the locks.
    for (auto& l : tablet_write_locks) {
      l.second->Commit();
    }
    tablet_write_locks.clear();

    // 12. Third Pass. Process all tablet schema version changes.
    // (This is separate from tablet state mutations because only table on-disk state is changed.)
    for (auto i = 0;
        i < FLAGS_catalog_manager_report_batch_size
          && tablet_iter_for_schema_changes != tablet_infos.end();
        ++i, ++tablet_iter_for_schema_changes) {
      const string& tablet_id = tablet_iter_for_schema_changes->first;
      const scoped_refptr<TabletInfo>& tablet = tablet_iter_for_schema_changes->second;
      const ReportedTabletPB& report = *FindOrDie(reports, tablet_id);
      if (report.has_schema_version()) {
        auto leader = tablet->GetLeader();
        if (leader.ok() && leader.get()->permanent_uuid() == ts_desc->permanent_uuid()) {
          RETURN_NOT_OK(HandleTabletSchemaVersionReport(tablet.get(), report.schema_version()));
        }
      }
    }

    // 13. Send all queued RPCs.
    for (auto& rpc : rpcs) {
      DCHECK(rpc->table());
      rpc->table()->AddTask(rpc);
      WARN_NOT_OK(ScheduleTask(rpc), Substitute("Failed to send $0", rpc->description()));
    }
    rpcs.clear();

    // 14. Check deadline. Need to exit before processing all batches if we're close to timing out.
    if (ts_desc->HasCapability(CAPABILITY_TabletReportLimit) && tablet_iter != tablet_infos.end()) {
      // [TESTING] Inject latency before processing a batch to test deadline.
      if (PREDICT_FALSE(FLAGS_TEST_inject_latency_during_tablet_report_ms > 0)) {
        LOG(INFO) << "Sleeping in CatalogManager::ProcessTabletReport for "
                  << FLAGS_TEST_inject_latency_during_tablet_report_ms << " ms";
        SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_inject_latency_during_tablet_report_ms));
      }

      // Return from here at configured safe heartbeat deadline to give the response packet time.
      if (safe_deadline < CoarseMonoClock::Now()) {
        LOG(INFO) << "Reached Heartbeat deadline. Returning early after processing "
                  << full_report_update->tablets_size() << " tablets";
        full_report_update->set_processing_truncated(true);
        return Status::OK();
      }
    }
  } // Loop to process the next batch until fully iterated.

  if (!full_report.is_incremental()) {
    // A full report may take multiple heartbeats.
    // The TS communicates how much is left to process for the full report beyond this specific HB.
    bool completed_full_report = !full_report.has_remaining_tablet_count()
                               || full_report.remaining_tablet_count() == 0;
    if (full_report.updated_tablets_size() == 0) {
      LOG(INFO) << ts_desc->permanent_uuid() << " sent full tablet report with 0 tablets.";
    } else if (!ts_desc->has_tablet_report()) {
      LOG(INFO) << ts_desc->permanent_uuid()
                << (completed_full_report ? " finished" : " receiving") << " first full report: "
                << full_report.updated_tablets_size() << " tablets.";
    }
    // We have a tablet report only once we're done processing all the chunks of the initial report.
    ts_desc->set_has_tablet_report(completed_full_report);
  }

  // 14. Queue background processing if we had updates.
  if (full_report.updated_tablets_size() > 0) {
    background_tasks_->WakeIfHasPendingUpdates();
  }

  return Status::OK();
}

Status CatalogManager::CreateTablegroup(const CreateTablegroupRequestPB* req,
                                        CreateTablegroupResponsePB* resp,
                                        rpc::RpcContext* rpc) {

  CreateTableRequestPB ctreq;
  CreateTableResponsePB ctresp;

  // Sanity check for PB fields.
  if (!req->has_id() || !req->has_namespace_id() || !req->has_namespace_name()) {
    Status s = STATUS(InvalidArgument, "Improper CREATE TABLEGROUP request (missing fields).");
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
  }

  // Use the tablegroup id as the prefix for the parent table id.
  const auto parent_table_id = req->id() + kTablegroupParentTableIdSuffix;
  const auto parent_table_name = req->id() + kTablegroupParentTableNameSuffix;
  ctreq.set_name(parent_table_name);
  ctreq.set_table_id(parent_table_id);
  ctreq.mutable_namespace_()->set_name(req->namespace_name());
  ctreq.mutable_namespace_()->set_id(req->namespace_id());
  ctreq.set_table_type(PGSQL_TABLE_TYPE);
  ctreq.set_tablegroup_id(req->id());

  YBSchemaBuilder schemaBuilder;
  schemaBuilder.AddColumn("parent_column")->Type(BINARY)->PrimaryKey()->NotNull();
  YBSchema ybschema;
  CHECK_OK(schemaBuilder.Build(&ybschema));
  auto schema = yb::client::internal::GetSchema(ybschema);
  SchemaToPB(schema, ctreq.mutable_schema());
  if (!FLAGS_TEST_tablegroup_master_only) {
    ctreq.mutable_schema()->mutable_table_properties()->set_is_transactional(true);
  }

  // Create a parent table, which will create the tablet.
  Status s = CreateTable(&ctreq, &ctresp, rpc);
  resp->set_parent_table_id(ctresp.table_id());
  resp->set_parent_table_name(parent_table_name);

  // Carry over error.
  if (ctresp.has_error()) {
    resp->mutable_error()->Swap(ctresp.mutable_error());
  }

  // We do not lock here so it is technically possible that the table was already created.
  // If so, there is nothing to do so we just ignore the "AlreadyPresent" error.
  if (!s.ok() && !s.IsAlreadyPresent()) {
    LOG(WARNING) << "Tablegroup creation failed: " << s.ToString();
    return s;
  }

  // Update catalog manager maps
  SharedLock<LockType> catalog_lock(lock_);
  TRACE("Acquired catalog manager lock");
  TablegroupInfo *tg = new TablegroupInfo(req->id(), req->namespace_id());
  tablegroup_ids_map_[req->id()] = tg;

  return s;
}

Status CatalogManager::DeleteTablegroup(const DeleteTablegroupRequestPB* req,
                                        DeleteTablegroupResponsePB* resp,
                                        rpc::RpcContext* rpc) {
  DeleteTableRequestPB dtreq;
  DeleteTableResponsePB dtresp;

  // Sanity check for PB fields
  if (!req->has_id() || !req->has_namespace_id()) {
    Status s = STATUS(InvalidArgument, "Improper DELETE TABLEGROUP request (missing fields).");
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
  }

  // Use the tablegroup id as the prefix for the parent table id.
  const auto parent_table_id = req->id() + kTablegroupParentTableIdSuffix;
  const auto parent_table_name = req->id() + kTablegroupParentTableNameSuffix;

  dtreq.mutable_table()->set_table_name(parent_table_name);
  dtreq.mutable_table()->set_table_id(parent_table_id);
  dtreq.set_is_index_table(false);

  Status s = DeleteTable(&dtreq, &dtresp, rpc);
  resp->set_parent_table_id(dtresp.table_id());

  // Carry over error.
  if (dtresp.has_error()) {
    resp->mutable_error()->Swap(dtresp.mutable_error());
    return s;
  }

  // Perform map updates.
  SharedLock<LockType> catalog_lock(lock_);
  TRACE("Acquired catalog manager lock");
  tablegroup_ids_map_.erase(req->id());
  tablegroup_tablet_ids_map_[req->namespace_id()].erase(req->id());

  LOG(INFO) << "Deleted table " << parent_table_name;
  return s;
}

Status CatalogManager::ListTablegroups(const ListTablegroupsRequestPB* req,
                                       ListTablegroupsResponsePB* resp,
                                       rpc::RpcContext* rpc) {
  RETURN_NOT_OK(CheckOnline());

  SharedLock<LockType> l(lock_);

  if (!req->has_namespace_id()) {
    Status s = STATUS(InvalidArgument, "Improper ListTablegroups request (missing fields).");
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_SCHEMA, s);
  }

  if (tablegroup_tablet_ids_map_.find(req->namespace_id()) == tablegroup_tablet_ids_map_.end()) {
    return STATUS(NotFound, "Tablegroups not found for namespace id: ", req->namespace_id());
  }

  for (const auto& entry : tablegroup_tablet_ids_map_[req->namespace_id()]) {
    const TablegroupId tgid = entry.first;
    if (tablegroup_ids_map_.find(tgid) == tablegroup_ids_map_.end()) {
      LOG(WARNING) << "Tablegroup info in " << req->namespace_id()
                   << " not found for tablegroup id: " << tgid;
      continue;
    }
    scoped_refptr<TablegroupInfo> tginfo = tablegroup_ids_map_[tgid];

    TablegroupIdentifierPB *tg = resp->add_tablegroups();
    tg->set_id(tginfo->id());
    tg->set_namespace_id(tginfo->namespace_id());
  }
  return Status::OK();
}

bool CatalogManager::HasTablegroups() {
  SharedLock<LockType> l(lock_);
  return !tablegroup_ids_map_.empty();
}

Status CatalogManager::CreateNamespace(const CreateNamespaceRequestPB* req,
                                       CreateNamespaceResponsePB* resp,
                                       rpc::RpcContext* rpc) {
  RETURN_NOT_OK(CheckOnline());
  Status return_status;

  // Copy the request, so we can fill in some defaults.
  LOG(INFO) << "CreateNamespace from " << RequestorString(rpc)
            << ": " << req->DebugString();

  scoped_refptr<NamespaceInfo> ns;
  std::vector<scoped_refptr<TableInfo>> pgsql_tables;
  TransactionMetadata txn;
  const auto db_type = GetDatabaseType(*req);
  {
    std::lock_guard<LockType> l(lock_);
    TRACE("Acquired catalog manager lock");

    // Validate the user request.

    // Verify that the namespace does not already exist.
    ns = FindPtrOrNull(namespace_ids_map_, req->namespace_id()); // Same ID.
    if (ns == nullptr && db_type != YQL_DATABASE_PGSQL) {
      // PGSQL databases have name uniqueness handled at a different layer, so ignore overlaps.
      ns = FindPtrOrNull(namespace_names_mapper_[db_type], req->name());
    }
    if (ns != nullptr) {
      resp->set_id(ns->id());
      return_status = STATUS_SUBSTITUTE(AlreadyPresent, "Keyspace '$0' already exists",
                                        req->name());
      LOG(WARNING) << "Found keyspace: " << ns->id() << ". Failed creating keyspace with error: "
                   << return_status.ToString() << " Request:\n" << req->DebugString();
      return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_ALREADY_PRESENT,
                        return_status);
    }

    // Add the new namespace.

    // Create unique id for this new namespace.
    NamespaceId new_id = !req->namespace_id().empty() ? req->namespace_id()
                                                      : GenerateIdUnlocked(SysRowEntry::NAMESPACE);
    ns = new NamespaceInfo(new_id);
    ns->mutable_metadata()->StartMutation();
    SysNamespaceEntryPB *metadata = &ns->mutable_metadata()->mutable_dirty()->pb;
    metadata->set_name(req->name());
    metadata->set_database_type(db_type);
    metadata->set_colocated(req->colocated());
    metadata->set_state(SysNamespaceEntryPB::PREPARING);

    // For namespace created for a Postgres database, save the list of tables and indexes for
    // for the database that need to be copied.
    if (db_type == YQL_DATABASE_PGSQL) {
      if (req->source_namespace_id().empty()) {
        metadata->set_next_pg_oid(req->next_pg_oid());
      } else {
        const auto source_oid = GetPgsqlDatabaseOid(req->source_namespace_id());
        if (!source_oid.ok()) {
          return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND,
                            source_oid.status());
        }
        for (const auto& iter : *table_ids_map_) {
          const auto& table_id = iter.first;
          const auto& table = iter.second;
          if (IsPgsqlId(table_id) && CHECK_RESULT(GetPgsqlDatabaseOid(table_id)) == *source_oid) {
            // Since indexes have dependencies on the base tables, put the tables in the front.
            const bool is_table = table->indexed_table_id().empty();
            pgsql_tables.insert(is_table ? pgsql_tables.begin() : pgsql_tables.end(), table);
          }
        }

        scoped_refptr<NamespaceInfo> source_ns = FindPtrOrNull(namespace_ids_map_,
                                                               req->source_namespace_id());
        if (!source_ns) {
          return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND,
                            STATUS(NotFound, "Source keyspace not found",
                                   req->source_namespace_id()));
        }
        auto source_ns_lock = source_ns->LockForRead();
        metadata->set_next_pg_oid(source_ns_lock->data().pb.next_pg_oid());
      }
    }

    // NS with a Transaction should be rolled back if the transaction does not get Committed.
    // Store this on the NS for now and use it later.
    if (req->has_transaction() && PREDICT_TRUE(FLAGS_enable_transactional_ddl_gc)) {
      metadata->mutable_transaction()->CopyFrom(req->transaction());
      txn = VERIFY_RESULT(TransactionMetadata::FromPB(req->transaction()));
      RSTATUS_DCHECK(!txn.status_tablet.empty(), Corruption, "Given incomplete Transaction");
    }

    // Add the namespace to the in-memory map for the assignment.
    namespace_ids_map_[ns->id()] = ns;
    namespace_names_mapper_[db_type][req->name()] = ns;

    resp->set_id(ns->id());
  }
  TRACE("Inserted new keyspace info into CatalogManager maps");

  // Update the on-disk system catalog.
  return_status = sys_catalog_->AddItem(ns.get(), leader_ready_term());
  if (!return_status.ok()) {
    LOG(WARNING) << "Keyspace creation failed:" << return_status.ToString();
    {
      std::lock_guard<LockType> l(lock_);
      namespace_ids_map_.erase(ns->id());
      namespace_names_mapper_[db_type].erase(req->name());
    }
    ns->mutable_metadata()->AbortMutation();
    return CheckIfNoLongerLeaderAndSetupError(return_status, resp);
  }
  TRACE("Wrote keyspace to sys-catalog");
  // Commit the namespace in-memory state.
  ns->mutable_metadata()->CommitMutation();

  LOG(INFO) << "Created keyspace " << ns->ToString();

  if (req->has_creator_role_name()) {
    RETURN_NOT_OK(permissions_manager_->GrantPermissions(
        req->creator_role_name(),
        get_canonical_keyspace(req->name()),
        req->name() /* resource name */,
        req->name() /* keyspace name */,
        all_permissions_for_resource(ResourceType::KEYSPACE),
        ResourceType::KEYSPACE,
        resp));
  }

  // Colocated databases need to create a parent tablet to serve as the base storage location.
  if (req->colocated()) {
    CreateTableRequestPB req;
    CreateTableResponsePB resp;
    const auto parent_table_id = ns->id() + kColocatedParentTableIdSuffix;
    const auto parent_table_name = ns->id() + kColocatedParentTableNameSuffix;
    req.set_name(parent_table_name);
    req.set_table_id(parent_table_id);
    req.mutable_namespace_()->set_name(ns->name());
    req.mutable_namespace_()->set_id(ns->id());
    req.set_table_type(GetTableTypeForDatabase(ns->database_type()));
    req.set_colocated(true);

    YBSchemaBuilder schemaBuilder;
    schemaBuilder.AddColumn("parent_column")->Type(BINARY)->PrimaryKey()->NotNull();
    YBSchema ybschema;
    CHECK_OK(schemaBuilder.Build(&ybschema));
    auto schema = yb::client::internal::GetSchema(ybschema);
    SchemaToPB(schema, req.mutable_schema());
    req.mutable_schema()->mutable_table_properties()->set_is_transactional(true);

    // create a parent table, which will create the tablet.
    Status s = CreateTable(&req, &resp, rpc);
    // We do not lock here so it is technically possible that the table was already created.
    // If so, there is nothing to do so we just ignore the "AlreadyPresent" error.
    if (!s.ok() && !s.IsAlreadyPresent()) {
      LOG(WARNING) << "Keyspace creation failed:" << s.ToString();
      // TODO: We should verify this behavior works end-to-end.
      // Diverging in-memory state from disk so the user can issue a delete if no new leader.
      auto l = ns->LockForWrite();
      SysNamespaceEntryPB& metadata = ns->mutable_metadata()->mutable_dirty()->pb;
      metadata.set_state(SysNamespaceEntryPB::FAILED);
      l->Commit();
      return s;
    }
  }

  if ((db_type == YQL_DATABASE_PGSQL && !pgsql_tables.empty()) ||
      PREDICT_FALSE(GetAtomicFlag(&FLAGS_TEST_hang_on_namespace_transition))) {
    // Process the subsequent work in the background thread (normally PGSQL).
    LOG(INFO) << "Keyspace create enqueued for later processing: " << ns->ToString();
    RETURN_NOT_OK(background_tasks_thread_pool_->SubmitFunc(
        std::bind(&CatalogManager::ProcessPendingNamespace, this, ns->id(), pgsql_tables, txn)));
    return Status::OK();
  } else {
    // All work is done, it's now safe to online the namespace (normally YQL).
    auto l = ns->LockForWrite();
    SysNamespaceEntryPB& metadata = ns->mutable_metadata()->mutable_dirty()->pb;
    if (metadata.state() == SysNamespaceEntryPB::PREPARING) {
      metadata.set_state(SysNamespaceEntryPB::RUNNING);
      return_status = sys_catalog_->UpdateItem(ns.get(), leader_ready_term());
      if (!return_status.ok()) {
        // Diverging in-memory state from disk so the user can issue a delete if no new leader.
        LOG(WARNING) << "Keyspace creation failed:" << return_status.ToString();
        metadata.set_state(SysNamespaceEntryPB::FAILED);
        return_status = CheckIfNoLongerLeaderAndSetupError(return_status, resp);
      } else {
        TRACE("Activated keyspace in sys-catalog");
        LOG(INFO) << "Activated keyspace: " << ns->ToString();
      }
      // Commit the namespace in-memory state.
      l->Commit();
    } else {
      LOG(WARNING) << "Keyspace has invalid state (" << metadata.state() << "), aborting create";
    }
  }
  return return_status;
}

void CatalogManager::ProcessPendingNamespace(
    NamespaceId id,
    std::vector<scoped_refptr<TableInfo>> template_tables,
    TransactionMetadata txn) {
  LOG(INFO) << "ProcessPendingNamespace started for " << id;

  // Ensure that we are currently the Leader before handling DDL operations.
  {
    ScopedLeaderSharedLock l(this);
    if (!l.catalog_status().ok() || !l.leader_status().ok()) {
      LOG(WARNING) << "Catalog status failure: " << l.catalog_status().ToString();
      // Don't try again, we have to reset in-memory state after losing leader election.
      return;
    }
  }

  if (PREDICT_FALSE(GetAtomicFlag(&FLAGS_TEST_hang_on_namespace_transition))) {
    LOG(INFO) << "Artificially waiting (" << FLAGS_catalog_manager_bg_task_wait_ms
              << "ms) on namespace creation for " << id;
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_catalog_manager_bg_task_wait_ms));
    WARN_NOT_OK(background_tasks_thread_pool_->SubmitFunc(
        std::bind(&CatalogManager::ProcessPendingNamespace, this, id, template_tables, txn)),
        "Could not submit ProcessPendingNamespaces to thread pool");
    return;
  }

  scoped_refptr<NamespaceInfo> ns;
  {
    std::lock_guard<LockType> l(lock_);
    ns = FindPtrOrNull(namespace_ids_map_, id);;
  }
  if (ns == nullptr) {
    LOG(WARNING) << "Pending Namespace not found to finish creation: " << id;
    return;
  }

  // Copy the system tables necessary to create this namespace.  This can be time-intensive.
  bool success = true;
  if (!template_tables.empty()) {
    auto s = CopyPgsqlSysTables(ns->id(), template_tables);
    WARN_NOT_OK(s, "Error Copying PGSQL System Tables for Pending Namespace");
    success = s.ok();
  }

  // All work is done, change the namespace state regardless of success or failure.
  {
    auto l = ns->LockForWrite();
    SysNamespaceEntryPB& metadata = ns->mutable_metadata()->mutable_dirty()->pb;
    if (metadata.state() == SysNamespaceEntryPB::PREPARING) {
      metadata.set_state(success ? SysNamespaceEntryPB::RUNNING : SysNamespaceEntryPB::FAILED);
      auto s = sys_catalog_->UpdateItem(ns.get(), leader_ready_term());
      if (s.ok()) {
        TRACE("Done processing keyspace");
        LOG(INFO) << (success ? "Processed" : "Failed") << " keyspace: " << ns->ToString();

        // Verify Transaction gets committed, which occurs after namespace create finishes.
        if (success && metadata.has_transaction()) {
          LOG(INFO) << "Enqueuing keyspace for Transaction Verification: " << ns->ToString();
          std::function<Status(bool)> when_done =
              std::bind(&CatalogManager::VerifyNamespacePgLayer, this, ns, _1);
          WARN_NOT_OK(background_tasks_thread_pool_->SubmitFunc(
              std::bind(&YsqlTransactionDdl::VerifyTransaction, &ysql_transaction_,
                        txn, when_done)),
              "Could not submit VerifyTransaction to thread pool");
        }
      } else {
        metadata.set_state(SysNamespaceEntryPB::FAILED);
        if (s.IsIllegalState() || s.IsAborted()) {
          s = STATUS(ServiceUnavailable,
              "operation requested can only be executed on a leader master, but this"
              " master is no longer the leader", s.ToString());
        } else {
          s = s.CloneAndPrepend(Substitute(
              "An error occurred while modifying keyspace to $0 in sys-catalog: $1",
              metadata.state(), s.ToString()));
        }
        LOG(WARNING) << s.ToString();
      }
      // Commit the namespace in-memory state.
      l->Commit();
    } else {
      LOG(WARNING) << "Bad keyspace state (" << metadata.state()
                   << "), abandoning creation work for " << ns->ToString();
    }
  }
}

Status CatalogManager::VerifyNamespacePgLayer(
    scoped_refptr<NamespaceInfo> ns, bool rpc_success) {
  // Upon Transaction completion, check pg system table using OID to ensure SUCCESS.
  const auto pg_table_id = GetPgsqlTableId(atoi(kSystemNamespaceId), kPgDatabaseTableOid);
  auto entry_exists = VERIFY_RESULT(
      ysql_transaction_.PgEntryExists(pg_table_id, GetPgsqlDatabaseOid(ns->id())));
  auto l = ns->LockForWrite();
  SysNamespaceEntryPB& metadata = ns->mutable_metadata()->mutable_dirty()->pb;

  // #5981: Mark un-retryable rpc failures as pass to avoid infinite retry of GC'd txns.
  bool txn_check_passed = entry_exists || !rpc_success;

  if (txn_check_passed) {
    // Passed checks.  Remove the transaction from the entry since we're done processing it.
    SCHECK_EQ(metadata.state(), SysNamespaceEntryPB::RUNNING, Aborted,
              Substitute("Invalid Namespace state ($0), abandoning transaction GC work for $1",
                 SysNamespaceEntryPB_State_Name(metadata.state()), ns->ToString()));
    metadata.clear_transaction();
    RETURN_NOT_OK(sys_catalog_->UpdateItem(ns.get(), leader_ready_term()));
    if (entry_exists) {
      LOG(INFO) << "Namespace transaction succeeded: " << ns->ToString();
    } else {
      LOG(WARNING) << "Unknown RPC Failure, removing transaction on namespace: " << ns->ToString();
    }
    // Commit the namespace in-memory state.
    l->Commit();
  } else {
    // Transaction failed.  We need to delete this Database now.
    SCHECK(metadata.state() == SysNamespaceEntryPB::RUNNING ||
           metadata.state() == SysNamespaceEntryPB::FAILED, Aborted,
           Substitute("Invalid Namespace state ($0), aborting delete.",
                      SysNamespaceEntryPB_State_Name(metadata.state()), ns->ToString()));
    LOG(INFO) << "Namespace transaction failed, deleting: " << ns->ToString();
    metadata.set_state(SysNamespaceEntryPB::DELETING);
    metadata.clear_transaction();
    RETURN_NOT_OK(sys_catalog_->UpdateItem(ns.get(), leader_ready_term()));
    // Commit the namespace in-memory state.
    l->Commit();
    // Async enqueue delete.
    RETURN_NOT_OK(background_tasks_thread_pool_->SubmitFunc(
        std::bind(&CatalogManager::DeleteYsqlDatabaseAsync, this, ns)));
  }
  return Status::OK();
}

// Get the information about an in-progress create operation.
Status CatalogManager::IsCreateNamespaceDone(const IsCreateNamespaceDoneRequestPB* req,
                                             IsCreateNamespaceDoneResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<NamespaceInfo> ns;
  auto ns_pb = req->namespace_();

  // 1. Lookup the namespace and verify it exists.
  TRACE("Looking up keyspace");
  RETURN_NAMESPACE_NOT_FOUND(FindNamespace(ns_pb, &ns), resp);

  TRACE("Locking keyspace");
  auto l = ns->LockForRead();
  auto metadata = l->data().pb;

  switch (metadata.state()) {
    // Success cases. Done and working.
    case SysNamespaceEntryPB::RUNNING:
      if (!ns->colocated()) {
        resp->set_done(true);
      } else {
        // Verify system table created as well, if colocated.
        IsCreateTableDoneRequestPB table_req;
        IsCreateTableDoneResponsePB table_resp;
        const auto parent_table_id = ns->id() + kColocatedParentTableIdSuffix;
        table_req.mutable_table()->set_table_id(parent_table_id);
        auto s = IsCreateTableDone(&table_req, &table_resp);
        resp->set_done(table_resp.done());
        if (!s.ok()) {
          if (table_resp.has_error()) {
            resp->mutable_error()->Swap(table_resp.mutable_error());
          }
          return s;
        }
      }
      break;
    // These states indicate that a create completed but a subsequent remove was requested.
    case SysNamespaceEntryPB::DELETING:
    case SysNamespaceEntryPB::DELETED:
      resp->set_done(true);
      break;
    // Pending cases.  NOT DONE
    case SysNamespaceEntryPB::PREPARING:
      resp->set_done(false);
      break;
    // Failure cases.  Done, but we need to give the user an error message.
    case SysNamespaceEntryPB::FAILED:
      resp->set_done(true);
      return SetupError(resp->mutable_error(), MasterErrorPB::UNKNOWN_ERROR, STATUS(InternalError,
              "Namespace Create Failed: not onlined."));
    default:
      Status s = STATUS_SUBSTITUTE(IllegalState, "IsCreateNamespaceDone failure: state=$0",
                                   SysNamespaceEntryPB_State_Name(metadata.state()));
      LOG(WARNING) << s.ToString();
      resp->set_done(true);
      return SetupError(resp->mutable_error(), MasterErrorPB::UNKNOWN_ERROR, s);
  }

  return Status::OK();
}

Status CatalogManager::DeleteNamespace(const DeleteNamespaceRequestPB* req,
                                       DeleteNamespaceResponsePB* resp,
                                       rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing DeleteNamespace request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<NamespaceInfo> ns;

  // Lookup the namespace and verify if it exists.
  TRACE("Looking up keyspace");
  RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->namespace_(), &ns), resp);

  if (req->has_database_type() && req->database_type() != ns->database_type()) {
    // Could not find the right database to delete.
    Status s = STATUS(NotFound, "Keyspace not found", ns->name());
    return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
  }
  {
    // Don't allow deletion if the namespace is in a transient state.
    auto cur_state = ns->state();
    if (cur_state != SysNamespaceEntryPB::RUNNING && cur_state != SysNamespaceEntryPB::FAILED) {
      if (cur_state == SysNamespaceEntryPB::DELETED) {
        Status s = STATUS(NotFound, "Keyspace already deleted", ns->name());
        return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
      } else {
        Status s = STATUS_SUBSTITUTE(
            TryAgain, "Namespace deletion not allowed when State = $0",
            SysNamespaceEntryPB::State_Name(cur_state));
        return SetupError(resp->mutable_error(), MasterErrorPB::IN_TRANSITION_CAN_RETRY, s);
      }
    }
  }

  // PGSQL has a completely forked implementation because it allows non-empty namespaces on delete.
  if (ns->database_type() == YQL_DATABASE_PGSQL) {
    return DeleteYsqlDatabase(req, resp, rpc);
  }

  TRACE("Locking keyspace");
  auto l = ns->LockForWrite();

  // Only empty namespace can be deleted.
  TRACE("Looking for tables in the keyspace");
  {
    SharedLock<LockType> catalog_lock(lock_);
    VLOG(3) << __func__ << ": Acquired the catalog manager lock_";

    for (const TableInfoMap::value_type& entry : *table_ids_map_) {
      auto ltm = entry.second->LockForRead();

      if (!ltm->data().started_deleting() && ltm->data().namespace_id() == ns->id()) {
        Status s = STATUS(InvalidArgument,
                          Substitute("Cannot delete keyspace which has $0: $1 [id=$2]",
                                     PROTO_IS_TABLE(ltm->data().pb) ? "table" : "index",
                                     ltm->data().name(), entry.second->id()), req->DebugString());
        return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_IS_NOT_EMPTY, s);
      }
    }
  }

  // Only empty namespace can be deleted.
  TRACE("Looking for types in the keyspace");
  {
    SharedLock<LockType> catalog_lock(lock_);
    VLOG(3) << __func__ << ": Acquired the catalog manager lock_";

    for (const UDTypeInfoMap::value_type& entry : udtype_ids_map_) {
      auto ltm = entry.second->LockForRead();

      if (ltm->data().namespace_id() == ns->id()) {
        Status s = STATUS(InvalidArgument,
            Substitute("Cannot delete keyspace which has type: $0 [id=$1]",
                ltm->data().name(), entry.second->id()), req->DebugString());
        return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_IS_NOT_EMPTY, s);
      }
    }
  }

  // [Delete]. Skip the DELETING->DELETED state, since no tables are present in this namespace.
  TRACE("Updating metadata on disk");
  // Update sys-catalog.
  Status s = sys_catalog_->DeleteItem(ns.get(), leader_ready_term());
  if (!s.ok()) {
    // The mutation will be aborted when 'l' exits the scope on early return.
    s = s.CloneAndPrepend(Substitute("An error occurred while updating sys-catalog: $0",
                                     s.ToString()));
    LOG(WARNING) << s.ToString();
    return CheckIfNoLongerLeaderAndSetupError(s, resp);
  }

  // Update the in-memory state.
  TRACE("Committing in-memory state");
  l->Commit();

  // Remove the namespace from all CatalogManager mappings.
  {
    std::lock_guard<LockType> l_map(lock_);
    namespace_names_mapper_[ns->database_type()].erase(ns->name());
    if (namespace_ids_map_.erase(ns->id()) < 1) {
      LOG(WARNING) << Format("Could not remove namespace from maps, id=$1", ns->id());
    }
  }

  // Delete any permissions granted on this keyspace to any role. See comment in DeleteTable() for
  // more details.
  string canonical_resource = get_canonical_keyspace(req->namespace_().name());
  RETURN_NOT_OK(permissions_manager_->RemoveAllPermissionsForResource(canonical_resource, resp));

  LOG(INFO) << "Successfully deleted keyspace " << ns->ToString()
            << " per request from " << RequestorString(rpc);
  return Status::OK();
}

Status CatalogManager::DeleteYsqlDatabase(const DeleteNamespaceRequestPB* req,
                                          DeleteNamespaceResponsePB* resp,
                                          rpc::RpcContext* rpc) {
  // Lookup database.
  scoped_refptr <NamespaceInfo> database;
  RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->namespace_(), &database), resp);

  // Make sure this is a YSQL database.
  if (database->database_type() != YQL_DATABASE_PGSQL) {
    // A non-YSQL namespace is found, but the rpc requests to drop a YSQL database.
    Status s = STATUS(NotFound, "YSQL database not found", database->name());
    return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
  }

  // Set the Namespace to DELETING.
  TRACE("Locking database");
  auto l = database->LockForWrite();
  SysNamespaceEntryPB &metadata = database->mutable_metadata()->mutable_dirty()->pb;
  if (metadata.state() == SysNamespaceEntryPB::RUNNING ||
      metadata.state() == SysNamespaceEntryPB::FAILED) {
    metadata.set_state(SysNamespaceEntryPB::DELETING);
    RETURN_NOT_OK(sys_catalog_->UpdateItem(database.get(), leader_ready_term()));
    TRACE("Marked keyspace for deletion in sys-catalog");
    // Commit the namespace in-memory state.
    l->Commit();
  } else {
    Status s = STATUS_SUBSTITUTE(IllegalState,
        "Keyspace ($0) has invalid state ($1), aborting delete",
        database->name(), metadata.state());
    return SetupError(resp->mutable_error(), MasterErrorPB::INTERNAL_ERROR, s);
  }

  return background_tasks_thread_pool_->SubmitFunc(
    std::bind(&CatalogManager::DeleteYsqlDatabaseAsync, this, database));
}

void CatalogManager::DeleteYsqlDatabaseAsync(scoped_refptr<NamespaceInfo> database) {
  TEST_PAUSE_IF_FLAG(TEST_hang_on_namespace_transition);

  // Lock database before removing content.
  TRACE("Locking database");
  auto l = database->LockForWrite();
  SysNamespaceEntryPB &metadata = database->mutable_metadata()->mutable_dirty()->pb;

  // A DELETED Namespace has finished but was tombstoned to avoid immediately reusing the same ID.
  // We consider a restart enough time, so we just need to remove it from the SysCatalog.
  if (metadata.state() == SysNamespaceEntryPB::DELETED) {
    Status s = sys_catalog_->DeleteItem(database.get(), leader_ready_term());
    WARN_NOT_OK(s, "SysCatalog DeleteItem for Namespace");
    if (!s.ok()) {
      return;
    }
  } else if (metadata.state() == SysNamespaceEntryPB::DELETING) {
    // Delete all tables in the database.
    TRACE("Delete all tables in YSQL database");
    Status s = DeleteYsqlDBTables(database);
    WARN_NOT_OK(s, "DeleteYsqlDBTables failed");
    if (!s.ok()) {
      // Move to FAILED so DeleteNamespace can be reissued by the user.
      metadata.set_state(SysNamespaceEntryPB::FAILED);
      l->Commit();
      return;
    }

    // Once all user-facing data has been offlined, move the Namespace to DELETED state.
    metadata.set_state(SysNamespaceEntryPB::DELETED);
    s = sys_catalog_->UpdateItem(database.get(), leader_ready_term());
    WARN_NOT_OK(s, "SysCatalog Update for Namespace");
    if (!s.ok()) {
      // Move to FAILED so DeleteNamespace can be reissued by the user.
      metadata.set_state(SysNamespaceEntryPB::FAILED);
      l->Commit();
      return;
    }
    TRACE("Marked keyspace as deleted in sys-catalog");
  } else {
    LOG(WARNING) << "Keyspace (" << database->name() << ") has invalid state ("
                 << metadata.state() << "), aborting delete";
    return;
  }

  // Remove namespace from CatalogManager name mapping.  Will remove ID map after all Tables gone.
  {
    std::lock_guard<LockType> l_map(lock_);
    if (namespace_names_mapper_[database->database_type()].erase(database->name()) < 1) {
      LOG(WARNING) << Format("Could not remove namespace from maps, name=$0, id=$1",
                             database->name(), database->id());
    }
  }

  // Update the in-memory state.
  TRACE("Committing in-memory state");
  l->Commit();

  // DROP completed. Return status.
  LOG(INFO) << "Successfully deleted YSQL database " << database->ToString();
}

// IMPORTANT: If modifying, consider updating DeleteTable(), the singular deletion API.
Status CatalogManager::DeleteYsqlDBTables(const scoped_refptr<NamespaceInfo>& database) {
  TabletInfoPtr sys_tablet_info;
  vector<pair<scoped_refptr<TableInfo>, unique_ptr<TableInfo::lock_type>>> tables;
  unordered_set<TableId> sys_table_ids;
  {
    // Lock the catalog to iterate over table_ids_map_.
    SharedLock<LockType> catalog_lock(lock_);

    sys_tablet_info = tablet_map_->find(kSysCatalogTabletId)->second;

    // Populate tables and sys_table_ids.
    for (const TableInfoMap::value_type& entry : *table_ids_map_) {
      scoped_refptr<TableInfo> table = entry.second;
      auto l = table->LockForWrite();
      if (l->data().namespace_id() != database->id() || l->data().started_deleting()) {
        continue;
      }
      RSTATUS_DCHECK(
          !l->data().pb.is_pg_shared_table(), Corruption, "Shared table found in database");

      if (IsSystemTable(*table)) {
        sys_table_ids.insert(table->id());
      }

      // For regular (indexed) table, insert table info and lock in the front of the list. Else for
      // index table, append them to the end. We do so so that we will commit and delete the indexed
      // table first before its indexes.
      if (PROTO_IS_TABLE(l->data().pb)) {
        tables.insert(tables.begin(), {table, std::move(l)});
      } else {
        tables.push_back({table, std::move(l)});
      }
    }
  }
  // Remove the system tables from RAFT.
  TRACE("Sending system table delete RPCs");
  for (auto &table_id : sys_table_ids) {
    RETURN_NOT_OK(sys_catalog_->DeleteYsqlSystemTable(table_id));
  }
  // Remove the system tables from the system catalog TabletInfo.
  RETURN_NOT_OK(RemoveTableIdsFromTabletInfo(sys_tablet_info, sys_table_ids));

  // Batch remove all relevant CDC streams. Handle before we delete the tables they reference.
  TRACE("Deleting CDC streams on table");
  vector<TableId> id_list;
  id_list.reserve(tables.size());
  for (auto &table_and_lock : tables) {
    id_list.push_back(table_and_lock.first->id());
  }
  RETURN_NOT_OK(DeleteCDCStreamsForTables(id_list));

  // Set all table states to DELETING as one batch RPC call.
  TRACE("Sending delete table batch RPC to sys catalog");
  vector<TableInfo *> tables_rpc;
  tables_rpc.reserve(tables.size());
  for (auto &table_and_lock : tables) {
    tables_rpc.push_back(table_and_lock.first.get());
    auto &l = table_and_lock.second;
    // Mark the table state as DELETING tablets.
    l->mutable_data()->set_state(SysTablesEntryPB::DELETING,
        Substitute("Started deleting at $0", LocalTimeAsString()));
  }
  // Update all the table states in raft in bulk.
  Status s = sys_catalog_->UpdateItems(tables_rpc, leader_ready_term());
  if (!s.ok()) {
    // The mutation will be aborted when 'l' exits the scope on early return.
    s = s.CloneAndPrepend(Substitute("An error occurred while updating sys tables: $0",
                                     s.ToString()));
    LOG(WARNING) << s.ToString();
    return CheckIfNoLongerLeader(s);
  }
  for (auto &table_and_lock : tables) {
    auto &table = table_and_lock.first;
    auto &l = table_and_lock.second;
    // Cancel all table busywork and commit the DELETING change.
    l->Commit();
    table->AbortTasks();
  }

  // Send a DeleteTablet() RPC request to each tablet replica in the table.
  for (auto &table_and_lock : tables) {
    auto &table = table_and_lock.first;
    DeleteTabletsAndSendRequests(table);
  }

  // Invoke any background tasks and return (notably, table cleanup).
  background_tasks_->Wake();
  return Status::OK();
}

// Get the information about an in-progress delete operation.
Status CatalogManager::IsDeleteNamespaceDone(const IsDeleteNamespaceDoneRequestPB* req,
                                             IsDeleteNamespaceDoneResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<NamespaceInfo> ns;
  auto ns_pb = req->namespace_();

  // 1. Lookup the namespace and verify it exists.
  TRACE("Looking up keyspace");
  Status s = FindNamespace(ns_pb, &ns);
  if (!s.ok()) {
    // Namespace no longer exists means success.
    LOG(INFO) << "Servicing IsDeleteNamespaceDone request for "
              << ns_pb.DebugString() << ": deleted (not found)";
    resp->set_done(true);
    return Status::OK();
  }

  TRACE("Locking keyspace");
  auto l = ns->LockForRead();
  auto& metadata = l->data().pb;

  if (metadata.state() == SysNamespaceEntryPB::DELETED) {
    resp->set_done(true);
  } else if (metadata.state() == SysNamespaceEntryPB::DELETING) {
    resp->set_done(false);
  } else {
    Status s = STATUS_SUBSTITUTE(IllegalState,
        "Servicing IsDeleteTableDone request for $0: NOT deleted (state=$1)",
        ns_pb.DebugString(), metadata.state());
    LOG(WARNING) << s.ToString();
    // Done != Successful.  We just want to let the user know the delete has finished processing.
    resp->set_done(true);
    return SetupError(resp->mutable_error(), MasterErrorPB::INTERNAL_ERROR, s);
  }
  return Status::OK();
}

Status CatalogManager::AlterNamespace(const AlterNamespaceRequestPB* req,
                                      AlterNamespaceResponsePB* resp,
                                      rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing AlterNamespace request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<NamespaceInfo> database;
  RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->namespace_(), &database), resp);

  if (req->namespace_().has_database_type() &&
      database->database_type() != req->namespace_().database_type()) {
    Status s = STATUS(NotFound, "Database not found", database->name());
    return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
  }

  TRACE("Locking database");
  auto l = database->LockForWrite();

  // Don't allow an alter if the namespace isn't running.
  if (l->data().pb.state() != SysNamespaceEntryPB::RUNNING) {
    Status s = STATUS_SUBSTITUTE(TryAgain, "Namespace not running.  State = $0",
                                 SysNamespaceEntryPB::State_Name(l->data().pb.state()));
    return SetupError(resp->mutable_error(), NamespaceMasterError(l->data().pb.state()), s);
  }

  const string old_name = l->data().pb.name();

  if (req->has_new_name() && req->new_name() != old_name) {
    const string new_name = req->new_name();

    // Verify that the new name does not exist.
    scoped_refptr<NamespaceInfo> ns;
    NamespaceIdentifierPB ns_identifier;
    ns_identifier.set_name(new_name);
    if (req->namespace_().has_database_type()) {
      ns_identifier.set_database_type(req->namespace_().database_type());
    }
    // TODO: This check will only work for YSQL once we add support for YSQL namespaces in
    // namespace_name_map (#1476).
    std::lock_guard<LockType> catalog_lock(lock_);
    TRACE("Acquired catalog manager lock");
    auto s = FindNamespaceUnlocked(ns_identifier, &ns);
    if (ns != nullptr && req->namespace_().has_database_type() &&
        ns->database_type() == req->namespace_().database_type()) {
      Status s = STATUS_SUBSTITUTE(AlreadyPresent,
          "Keyspace '$0' already exists", ns->name());
      LOG(WARNING) << "Found keyspace: " << ns->id() << ". Failed altering keyspace with error: "
                   << s.ToString() << " Request:\n" << req->DebugString();
      return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_ALREADY_PRESENT, s);
    }

    namespace_names_mapper_[req->namespace_().database_type()][new_name] = database;
    namespace_names_mapper_[req->namespace_().database_type()].erase(old_name);

    l->mutable_data()->pb.set_name(new_name);
  }

  RETURN_NOT_OK(sys_catalog_->UpdateItem(database.get(), leader_ready_term()));

  TRACE("Committing in-memory state");
  l->Commit();

  LOG(INFO) << "Successfully altered keyspace " << req->namespace_().name()
            << " per request from " << RequestorString(rpc);
  return Status::OK();
}

Status CatalogManager::ListNamespaces(const ListNamespacesRequestPB* req,
                                      ListNamespacesResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  SharedLock<LockType> l(lock_);

  for (const auto& entry : namespace_ids_map_) {
    const auto& namespace_info = *entry.second;
    auto ltm = namespace_info.LockForRead();
    // If the request asks for namespaces for a specific database type, filter by the type.
    if (req->has_database_type() && namespace_info.database_type() != req->database_type()) {
      continue;
    }
    // Only return RUNNING namespaces.
    if (namespace_info.state() != SysNamespaceEntryPB::RUNNING) {
      continue;
    }

    NamespaceIdentifierPB *ns = resp->add_namespaces();
    ns->set_id(namespace_info.id());
    ns->set_name(namespace_info.name());
    ns->set_database_type(namespace_info.database_type());
  }
  return Status::OK();
}

Status CatalogManager::GetNamespaceInfo(const GetNamespaceInfoRequestPB* req,
                                        GetNamespaceInfoResponsePB* resp,
                                        rpc::RpcContext* rpc) {
  LOG(INFO) << __func__ << " from " << RequestorString(rpc) << ": " << req->ShortDebugString();
  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<NamespaceInfo> ns;

  // Look up the namespace and verify if it exists.
  if (req->has_namespace_()) {
    TRACE("Looking up namespace");
    RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->namespace_(), &ns), resp);
  }

  resp->mutable_namespace_()->set_id(ns->id());
  resp->mutable_namespace_()->set_name(ns->name());
  resp->mutable_namespace_()->set_database_type(ns->database_type());
  resp->set_colocated(ns->colocated());
  return Status::OK();
}

Status CatalogManager::RedisConfigSet(
    const RedisConfigSetRequestPB* req, RedisConfigSetResponsePB* resp, rpc::RpcContext* rpc) {
  DCHECK(req->has_keyword());
  const auto& key = req->keyword();
  SysRedisConfigEntryPB config_entry;
  config_entry.set_key(key);
  *config_entry.mutable_args() = req->args();
  bool created = false;

  TRACE("Acquired catalog manager lock");
  std::lock_guard<LockType> l_big(lock_);
  scoped_refptr<RedisConfigInfo> cfg = FindPtrOrNull(redis_config_map_, req->keyword());
  if (cfg == nullptr) {
    created = true;
    cfg = new RedisConfigInfo(key);
    redis_config_map_[key] = cfg;
  }

  auto wl = cfg->LockForWrite();
  wl->mutable_data()->pb = std::move(config_entry);
  if (created) {
    CHECK_OK(sys_catalog_->AddItem(cfg.get(), leader_ready_term()));
  } else {
    CHECK_OK(sys_catalog_->UpdateItem(cfg.get(), leader_ready_term()));
  }
  wl->Commit();
  return Status::OK();
}

Status CatalogManager::RedisConfigGet(
    const RedisConfigGetRequestPB* req, RedisConfigGetResponsePB* resp, rpc::RpcContext* rpc) {
  DCHECK(req->has_keyword());
  resp->set_keyword(req->keyword());
  TRACE("Acquired catalog manager lock");
  std::lock_guard<LockType> l_big(lock_);
  scoped_refptr<RedisConfigInfo> cfg = FindPtrOrNull(redis_config_map_, req->keyword());
  if (cfg == nullptr) {
    Status s = STATUS_SUBSTITUTE(NotFound, "Redis config for $0 does not exists", req->keyword());
    return SetupError(resp->mutable_error(), MasterErrorPB::REDIS_CONFIG_NOT_FOUND, s);
  }
  auto rci = cfg->LockForRead();
  resp->mutable_args()->CopyFrom(rci->data().pb.args());
  return Status::OK();
}

Status CatalogManager::CreateUDType(const CreateUDTypeRequestPB* req,
                                    CreateUDTypeResponsePB* resp,
                                    rpc::RpcContext* rpc) {
  LOG(INFO) << "CreateUDType from " << RequestorString(rpc)
            << ": " << req->DebugString();

  RETURN_NOT_OK(CheckOnline());
  Status s;
  scoped_refptr<UDTypeInfo> tp;
  scoped_refptr<NamespaceInfo> ns;

  // Lookup the namespace and verify if it exists.
  if (req->has_namespace_()) {
    TRACE("Looking up namespace");
    RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->namespace_(), &ns), resp);
    if (ns->database_type() != YQLDatabase::YQL_DATABASE_CQL) {
      Status s = STATUS(NotFound, "Namespace not found");
      return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
    }
  }

  // Get all the referenced types (if any).
  std::vector<std::string> referenced_udts;
  for (const QLTypePB& field_type : req->field_types()) {
    QLType::GetUserDefinedTypeIds(field_type, /* transitive = */ true, &referenced_udts);
  }

  {
    TRACE("Acquired catalog manager lock");
    std::lock_guard<LockType> l(lock_);

    // Verify that the type does not exist.
    tp = FindPtrOrNull(udtype_names_map_, std::make_pair(ns->id(), req->name()));

    if (tp != nullptr) {
      s = STATUS_SUBSTITUTE(AlreadyPresent,
          "Type '$0.$1' already exists", ns->name(), req->name());
      LOG(WARNING) << "Found type: " << tp->id() << ". Failed creating type with error: "
                   << s.ToString() << " Request:\n" << req->DebugString();
      return SetupError(resp->mutable_error(), MasterErrorPB::TYPE_ALREADY_PRESENT, s);
    }

    // Verify that all referenced types actually exist.
    for (const auto& udt_id : referenced_udts) {
      if (FindPtrOrNull(udtype_ids_map_, udt_id) == nullptr) {
          // This may be caused by a stale cache (e.g. referenced type name resolves to an old,
          // deleted type). Return InvalidArgument so query layer will clear cache and retry.
          s = STATUS_SUBSTITUTE(InvalidArgument,
          "Type id '$0' referenced by type '$1' does not exist", udt_id, req->name());
        return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
      }
    }

    // Construct the new type (generate fresh name and set fields).
    UDTypeId new_id = GenerateIdUnlocked(SysRowEntry::UDTYPE);
    tp = new UDTypeInfo(new_id);
    tp->mutable_metadata()->StartMutation();
    SysUDTypeEntryPB *metadata = &tp->mutable_metadata()->mutable_dirty()->pb;
    metadata->set_name(req->name());
    metadata->set_namespace_id(ns->id());
    for (const string& field_name : req->field_names()) {
      metadata->add_field_names(field_name);
    }

    for (const QLTypePB& field_type : req->field_types()) {
      metadata->add_field_types()->CopyFrom(field_type);
    }

    // Add the type to the in-memory maps.
    udtype_ids_map_[tp->id()] = tp;
    udtype_names_map_[std::make_pair(ns->id(), req->name())] = tp;
    resp->set_id(tp->id());
  }
  TRACE("Inserted new user-defined type info into CatalogManager maps");

  // Update the on-disk system catalog.
  s = sys_catalog_->AddItem(tp.get(), leader_ready_term());
  if (!s.ok()) {
    s = s.CloneAndPrepend(Substitute(
        "An error occurred while inserting user-defined type to sys-catalog: $0", s.ToString()));
    LOG(WARNING) << s.ToString();
    return CheckIfNoLongerLeaderAndSetupError(s, resp);
  }
  TRACE("Wrote user-defined type to sys-catalog");

  // Commit the in-memory state.
  tp->mutable_metadata()->CommitMutation();
  LOG(INFO) << "Created user-defined type " << tp->ToString();
  return Status::OK();
}

Status CatalogManager::DeleteUDType(const DeleteUDTypeRequestPB* req,
                                    DeleteUDTypeResponsePB* resp,
                                    rpc::RpcContext* rpc) {
  LOG(INFO) << "Servicing DeleteUDType request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();

      RETURN_NOT_OK(CheckOnline());
  scoped_refptr<UDTypeInfo> tp;
  scoped_refptr<NamespaceInfo> ns;

  if (!req->has_type()) {
    Status s = STATUS(InvalidArgument, "No type given", req->DebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
  }

  // Validate namespace.
  if (req->type().has_namespace_()) {
    // Lookup the namespace and verify if it exists.
    TRACE("Looking up namespace");
    RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->type().namespace_(), &ns), resp);
  }

  {
    std::lock_guard<LockType> l(lock_);
    TRACE("Acquired catalog manager lock");

    if (req->type().has_type_id()) {
      tp = FindPtrOrNull(udtype_ids_map_, req->type().type_id());
    } else if (req->type().has_type_name()) {
      tp = FindPtrOrNull(udtype_names_map_, {ns->id(), req->type().type_name()});
    }

    if (tp == nullptr) {
      Status s = STATUS(NotFound, "The type does not exist", req->DebugString());
      return SetupError(resp->mutable_error(), MasterErrorPB::TYPE_NOT_FOUND, s);
    }

    // Checking if any table uses this type.
    // TODO: this could be more efficient.
    for (const TableInfoMap::value_type& entry : *table_ids_map_) {
      auto ltm = entry.second->LockForRead();
      if (!ltm->data().started_deleting()) {
        for (const auto &col : ltm->data().schema().columns()) {
          if (col.type().main() == DataType::USER_DEFINED_TYPE &&
              col.type().udtype_info().id() == tp->id()) {
            Status s = STATUS(QLError,
                Substitute("Cannot delete type '$0.$1'. It is used in column $2 of table $3",
                    ns->name(), tp->name(), col.name(), ltm->data().name()));
            return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
          }
        }
      }
    }

    // Checking if any other type uses this type (i.e. in the case of nested types).
    // TODO: this could be more efficient.
    for (const UDTypeInfoMap::value_type& entry : udtype_ids_map_) {
      auto ltm = entry.second->LockForRead();

      for (int i = 0; i < ltm->data().field_types_size(); i++) {
        std::vector<std::string> referenced_udts;
        // Only need to check direct (non-transitive) type dependencies here.
        // This also means we report more precise errors for in-use types.
        QLType::GetUserDefinedTypeIds(ltm->data().field_types(i),
                                      false /* transitive */,
                                      &referenced_udts);
        auto it = std::find(referenced_udts.begin(), referenced_udts.end(), tp->id());
        if (it != referenced_udts.end()) {
          Status s = STATUS(QLError,
              Substitute("Cannot delete type '$0.$1'. It is used in field $2 of type '$3'",
                  ns->name(), tp->name(), ltm->data().field_names(i), ltm->data().name()));
          return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
        }
      }
    }
  }

  auto l = tp->LockForWrite();

  Status s = sys_catalog_->DeleteItem(tp.get(), leader_ready_term());
  if (!s.ok()) {
    // The mutation will be aborted when 'l' exits the scope on early return.
    s = s.CloneAndPrepend(Substitute("An error occurred while updating sys-catalog: $0",
        s.ToString()));
    LOG(WARNING) << s.ToString();
    return CheckIfNoLongerLeaderAndSetupError(s, resp);
  }

  // Remove it from the maps.
  {
    TRACE("Removing from maps");
    std::lock_guard<LockType> l_map(lock_);
    if (udtype_ids_map_.erase(tp->id()) < 1) {
      PANIC_RPC(rpc, "Could not remove user defined type from map, name=" + l->data().name());
    }
    if (udtype_names_map_.erase({ns->id(), tp->name()}) < 1) {
      PANIC_RPC(rpc, "Could not remove user defined type from map, name=" + l->data().name());
    }
  }

  // Update the in-memory state.
  TRACE("Committing in-memory state");
  l->Commit();

  LOG(INFO) << "Successfully deleted user-defined type " << tp->ToString()
            << " per request from " << RequestorString(rpc);

  return Status::OK();
}

Status CatalogManager::GetUDTypeInfo(const GetUDTypeInfoRequestPB* req,
                                     GetUDTypeInfoResponsePB* resp,
                                     rpc::RpcContext* rpc) {
  LOG(INFO) << "GetUDTypeInfo from " << RequestorString(rpc)
            << ": " << req->DebugString();
      RETURN_NOT_OK(CheckOnline());
  Status s;
  scoped_refptr<UDTypeInfo> tp;
  scoped_refptr<NamespaceInfo> ns;

  if (!req->has_type()) {
    s = STATUS(InvalidArgument, "Cannot get type, no type identifier given", req->DebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::TYPE_NOT_FOUND, s);
  }

  if (req->type().has_type_id()) {
    tp = FindPtrOrNull(udtype_ids_map_, req->type().type_id());
  } else if (req->type().has_type_name() && req->type().has_namespace_()) {
    // Lookup the type and verify if it exists.
    TRACE("Looking up namespace");
    RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->type().namespace_(), &ns), resp);

    tp = FindPtrOrNull(udtype_names_map_, std::make_pair(ns->id(), req->type().type_name()));
  }

  if (tp == nullptr) {
    s = STATUS(InvalidArgument, "Couldn't find type", req->DebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::TYPE_NOT_FOUND, s);
  }

  {
    auto type_lock = tp->LockForRead();

    UDTypeInfoPB* type_info = resp->mutable_udtype();

    type_info->set_name(tp->name());
    type_info->set_id(tp->id());
    type_info->mutable_namespace_()->set_id(type_lock->data().namespace_id());

    for (int i = 0; i < type_lock->data().field_names_size(); i++) {
      type_info->add_field_names(type_lock->data().field_names(i));
    }
    for (int i = 0; i < type_lock->data().field_types_size(); i++) {
      type_info->add_field_types()->CopyFrom(type_lock->data().field_types(i));
    }

    LOG(INFO) << "Retrieved user-defined type " << tp->ToString();
  }
  return Status::OK();
}

Status CatalogManager::ListUDTypes(const ListUDTypesRequestPB* req,
                                   ListUDTypesResponsePB* resp) {

  RETURN_NOT_OK(CheckOnline());

  scoped_refptr<NamespaceInfo> ns;

  // Validate namespace.
  if (req->has_namespace_()) {
    scoped_refptr<NamespaceInfo> ns;

    // Lookup the namespace and verify that it exists.
    RETURN_NAMESPACE_NOT_FOUND(FindNamespace(req->namespace_(), &ns), resp);
  }

  SharedLock<LockType> l(lock_);

  for (const UDTypeInfoByNameMap::value_type& entry : udtype_names_map_) {
    auto ltm = entry.second->LockForRead();

    // key is a pair <namespace_id, type_name>.
    if (!ns->id().empty() && ns->id() != entry.first.first) {
      continue; // Skip types from other namespaces.
    }

    UDTypeInfoPB* udtype = resp->add_udtypes();
    udtype->set_id(entry.second->id());
    udtype->set_name(ltm->data().name());
    for (size_t i = 0; i <= ltm->data().field_names_size(); i++) {
      udtype->add_field_names(ltm->data().field_names(i));
    }
    for (size_t i = 0; i <= ltm->data().field_types_size(); i++) {
      udtype->add_field_types()->CopyFrom(ltm->data().field_types(i));
    }

    if (CHECK_NOTNULL(ns.get())) {
      auto l = ns->LockForRead();
      udtype->mutable_namespace_()->set_id(ns->id());
      udtype->mutable_namespace_()->set_name(ns->name());
    }
  }
  return Status::OK();
}

// For non-enterprise builds, this is a no-op.
Status CatalogManager::DeleteCDCStreamsForTable(const TableId& table) {
  return Status::OK();
}

Status CatalogManager::DeleteCDCStreamsForTables(const vector<TableId>& table_ids) {
  return Status::OK();
}


bool CatalogManager::CDCStreamExistsUnlocked(const CDCStreamId& stream_id) {
  return false;
}

Result<uint64_t> CatalogManager::IncrementYsqlCatalogVersion() {

  auto l = CHECK_NOTNULL(ysql_catalog_config_.get())->LockForWrite();
  uint64_t new_version = l->data().pb.ysql_catalog_config().version() + 1;
  l->mutable_data()->pb.mutable_ysql_catalog_config()->set_version(new_version);

  // Write to sys_catalog and in memory.
  RETURN_NOT_OK(sys_catalog_->UpdateItem(ysql_catalog_config_.get(), leader_ready_term()));
  l->Commit();

  return new_version;
}

Status CatalogManager::InitDbFinished(Status initdb_status, int64_t term) {
  if (initdb_status.ok()) {
    LOG(INFO) << "initdb completed successfully";
  } else {
    LOG(ERROR) << "initdb failed: " << initdb_status;
  }

  auto l = CHECK_NOTNULL(ysql_catalog_config_.get())->LockForWrite();
  auto* mutable_ysql_catalog_config = l->mutable_data()->pb.mutable_ysql_catalog_config();
  mutable_ysql_catalog_config->set_initdb_done(true);
  if (!initdb_status.ok()) {
    mutable_ysql_catalog_config->set_initdb_error(initdb_status.ToString());
  } else {
    mutable_ysql_catalog_config->clear_initdb_error();
  }

  RETURN_NOT_OK(sys_catalog_->AddItem(ysql_catalog_config_.get(), term));
  l->Commit();
  return Status::OK();
}

CHECKED_STATUS CatalogManager::IsInitDbDone(
    const IsInitDbDoneRequestPB* req,
    IsInitDbDoneResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());

  auto l = CHECK_NOTNULL(ysql_catalog_config_.get())->LockForRead();
  const auto& ysql_catalog_config = l->data().pb.ysql_catalog_config();
  resp->set_pg_proc_exists(pg_proc_exists_.load(std::memory_order_acquire));
  resp->set_done(ysql_catalog_config.initdb_done());
  if (ysql_catalog_config.has_initdb_error() &&
      !ysql_catalog_config.initdb_error().empty()) {
    resp->set_initdb_error(ysql_catalog_config.initdb_error());
  }
  return Status::OK();
}

Status CatalogManager::GetYsqlCatalogVersion(uint64_t* catalog_version,
                                             uint64_t* last_breaking_version) {
  RETURN_NOT_OK(CheckOnline());

  auto table_info = GetTableInfo(kPgYbCatalogVersionTableId);
  if (table_info != nullptr) {
    return sys_catalog_->ReadYsqlCatalogVersion(kPgYbCatalogVersionTableId,
                                                catalog_version,
                                                last_breaking_version);
  }

  auto l = ysql_catalog_config_->LockForRead();
  // last_breaking_version is the last version (change) that invalidated ongoing transactions.
  // If using the old (protobuf-based) version method, we do not have any information about
  // breaking changes so assuming every change is a breaking change.
  if (catalog_version) {
    *catalog_version = l->data().pb.ysql_catalog_config().version();
  }
  if (last_breaking_version) {
    *last_breaking_version = l->data().pb.ysql_catalog_config().version();
  }
  return Status::OK();
}

Status CatalogManager::RegisterTsFromRaftConfig(const consensus::RaftPeerPB& peer) {
  NodeInstancePB instance_pb;
  instance_pb.set_permanent_uuid(peer.permanent_uuid());
  instance_pb.set_instance_seqno(0);

  TSRegistrationPB registration_pb;
  auto* common = registration_pb.mutable_common();
  *common->mutable_private_rpc_addresses() = peer.last_known_private_addr();
  *common->mutable_broadcast_addresses() = peer.last_known_broadcast_addr();
  *common->mutable_cloud_info() = peer.cloud_info();

  // Todo(Rahul) : May need to be changed when we implement table level overrides.
  SysClusterConfigEntryPB config;
  RETURN_NOT_OK(GetClusterConfig(&config));
  // If the config has no replication info, use empty string for the placement uuid, otherwise
  // calculate it from the reported peer.
  auto placement_uuid = config.has_replication_info() ?
      VERIFY_RESULT(CatalogManagerUtil::GetPlacementUuidFromRaftPeer(
          config.replication_info(), peer)) : "";
  common->set_placement_uuid(placement_uuid);

  return master_->ts_manager()->RegisterTS(instance_pb, registration_pb, master_->MakeCloudInfoPB(),
                                           &master_->proxy_cache(),
                                           RegisteredThroughHeartbeat::kFalse);
}

void CatalogManager::ReconcileTabletReplicasInLocalMemoryWithReport(
    const scoped_refptr<TabletInfo>& tablet,
    const std::string& sender_uuid,
    const ConsensusStatePB& consensus_state,
    const ReportedTabletPB& report) {
  auto replica_locations = std::make_shared<TabletInfo::ReplicaMap>();
  auto prev_rl = tablet->GetReplicaLocations();

  for (const consensus::RaftPeerPB& peer : consensus_state.config().peers()) {
    shared_ptr<TSDescriptor> ts_desc;
    if (!peer.has_permanent_uuid()) {
      LOG_WITH_PREFIX(WARNING) << "Missing UUID for peer" << peer.ShortDebugString();
      continue;
    }
    if (!master_->ts_manager()->LookupTSByUUID(peer.permanent_uuid(), &ts_desc)) {
      if (!GetAtomicFlag(&FLAGS_enable_register_ts_from_raft)) {
        LOG_WITH_PREFIX(WARNING) << "Tablet server has never reported in. "
        << "Not including in replica locations map yet. Peer: " << peer.ShortDebugString()
        << "; Tablet: " << tablet->ToString();
        continue;
      }

      LOG_WITH_PREFIX(INFO) << "Tablet server has never reported in. Registering the ts using "
                            << "the raft config. Peer: " << peer.ShortDebugString()
                            << "; Tablet: " << tablet->ToString();
      Status s = RegisterTsFromRaftConfig(peer);
      if (!s.ok()) {
        LOG_WITH_PREFIX(WARNING) << "Could not register ts from raft config: " << s
                                 << " Skip updating the replica map.";
        continue;
      }

      // Guaranteed to find the ts since we just registered.
      master_->ts_manager()->LookupTSByUUID(peer.permanent_uuid(), &ts_desc);
      if (!ts_desc.get()) {
        LOG_WITH_PREFIX(WARNING) << "Could not find ts with uuid " << peer.permanent_uuid()
                                 << " after registering from raft config. Skip updating the replica"
                                 << " map.";
        continue;
      }
    }

    // Do not update replicas in the NOT_STARTED or BOOTSTRAPPING state (unless they are stale).
    bool use_existing = false;
    const TabletReplica* existing_replica;
    if (peer.permanent_uuid() != sender_uuid) {
      auto it = prev_rl->find(ts_desc->permanent_uuid());
      if (it != prev_rl->end()) {
        existing_replica = &it->second;
        // IsStarting returns true if state == NOT_STARTED or state == BOOTSTRAPPING.
        use_existing = existing_replica->IsStarting() && !existing_replica->IsStale();
      }
    }
    if (use_existing) {
      InsertOrDie(replica_locations.get(), existing_replica->ts_desc->permanent_uuid(),
          *existing_replica);
    } else {
      TabletReplica replica;
      CreateNewReplicaForLocalMemory(ts_desc.get(), &consensus_state, report, &replica);
      InsertOrDie(replica_locations.get(), replica.ts_desc->permanent_uuid(), replica);
    }
  }

  // Update the local tablet replica set. This deviates from persistent state during bootstrapping.
  tablet->SetReplicaLocations(replica_locations);
  tablet_locations_version_.fetch_add(1, std::memory_order_acq_rel);
}

void CatalogManager::UpdateTabletReplicaInLocalMemory(TSDescriptor* ts_desc,
                                                      const ConsensusStatePB* consensus_state,
                                                      const ReportedTabletPB& report,
                                                      const scoped_refptr<TabletInfo>& tablet) {
  TabletReplica replica;
  CreateNewReplicaForLocalMemory(ts_desc, consensus_state, report, &replica);
  tablet->UpdateReplicaLocations(replica);
  tablet_locations_version_.fetch_add(1, std::memory_order_acq_rel);
}

void CatalogManager::CreateNewReplicaForLocalMemory(TSDescriptor* ts_desc,
                                                    const ConsensusStatePB* consensus_state,
                                                    const ReportedTabletPB& report,
                                                    TabletReplica* new_replica) {
  // Tablets in state NOT_STARTED or BOOTSTRAPPING don't have a consensus.
  if (consensus_state == nullptr) {
    new_replica->role = RaftPeerPB::NON_PARTICIPANT;
    new_replica->member_type = RaftPeerPB::UNKNOWN_MEMBER_TYPE;
  } else {
    CHECK(consensus_state != nullptr) << "No cstate: " << ts_desc->permanent_uuid()
                                      << " - " << report.state();
    new_replica->role = GetConsensusRole(ts_desc->permanent_uuid(), *consensus_state);
    new_replica->member_type = GetConsensusMemberType(ts_desc->permanent_uuid(), *consensus_state);
  }
  if (report.has_processing_parent_data()) {
      new_replica->processing_parent_data = report.processing_parent_data();
  }
  new_replica->state = report.state();
  new_replica->ts_desc = ts_desc;
  if (!ts_desc->registered_through_heartbeat()) {
    new_replica->time_updated = MonoTime::Now() - ts_desc->TimeSinceHeartbeat();
  }
}

Status CatalogManager::GetTabletPeer(const TabletId& tablet_id,
                                     std::shared_ptr<TabletPeer>* ret_tablet_peer) const {
  // Note: CatalogManager has only one table, 'sys_catalog', with only
  // one tablet.

  if (PREDICT_FALSE(!IsInitialized())) {
    // Master puts up the consensus service first and then initiates catalog manager's creation
    // asynchronously. So this case is possible, but harmless. The RPC will simply be retried.
    // Previously, because we weren't checking for this condition, we would fatal down stream.
    const string& reason = "CatalogManager is not yet initialized";
    YB_LOG_EVERY_N(WARNING, 1000) << reason;
    return STATUS(ServiceUnavailable, reason);
  }

  CHECK(sys_catalog_) << "sys_catalog_ must be initialized!";

  if (master_->opts().IsShellMode()) {
    return STATUS_SUBSTITUTE(NotFound,
        "In shell mode: no tablet_id $0 exists in CatalogManager.", tablet_id);
  }

  if (sys_catalog_->tablet_id() == tablet_id && sys_catalog_->tablet_peer().get() != nullptr &&
      sys_catalog_->tablet_peer()->CheckRunning().ok()) {
    *ret_tablet_peer = tablet_peer();
  } else {
    return STATUS_SUBSTITUTE(NotFound,
        "no SysTable in the RUNNING state exists with tablet_id $0 in CatalogManager", tablet_id);
  }
  return Status::OK();
}

const NodeInstancePB& CatalogManager::NodeInstance() const {
  return master_->instance_pb();
}

Status CatalogManager::GetRegistration(ServerRegistrationPB* reg) const {
  return master_->GetRegistration(reg, server::RpcOnly::kTrue);
}

Status CatalogManager::UpdateMastersListInMemoryAndDisk() {
  DCHECK(master_->opts().IsShellMode());

  if (!master_->opts().IsShellMode()) {
    return STATUS(IllegalState, "Cannot update master's info when process is not in shell mode.");
  }

  consensus::ConsensusStatePB consensus_state;
  RETURN_NOT_OK(GetCurrentConfig(&consensus_state));

  if (!consensus_state.has_config()) {
    return STATUS(NotFound, "No Raft config found.");
  }

  RETURN_NOT_OK(sys_catalog_->ConvertConfigToMasterAddresses(consensus_state.config()));
  RETURN_NOT_OK(sys_catalog_->CreateAndFlushConsensusMeta(master_->fs_manager(),
                                                          consensus_state.config(),
                                                          consensus_state.current_term()));

  return Status::OK();
}

Status CatalogManager::EnableBgTasks() {
  std::lock_guard<LockType> l(lock_);
  background_tasks_.reset(new CatalogManagerBgTasks(this));
  RETURN_NOT_OK_PREPEND(background_tasks_->Init(),
                        "Failed to initialize catalog manager background tasks");
  // Add bg thread to rebuild the system partitions thread.
  refresh_yql_partitions_task_.Bind(&master_->messenger()->scheduler());
  RETURN_NOT_OK(background_tasks_thread_pool_->SubmitFunc(
      std::bind(&CatalogManager::RebuildYQLSystemPartitions, this)));
  return Status::OK();
}

Status CatalogManager::StartRemoteBootstrap(const StartRemoteBootstrapRequestPB& req) {
  const TabletId& tablet_id = req.tablet_id();
  std::unique_lock<std::mutex> l(remote_bootstrap_mtx_, std::try_to_lock);
  if (!l.owns_lock()) {
    return STATUS_SUBSTITUTE(AlreadyPresent,
        "Remote bootstrap of tablet $0 already in progress", tablet_id);
  }

  if (!master_->opts().IsShellMode()) {
    return STATUS(IllegalState, "Cannot bootstrap a master which is not in shell mode.");
  }

  LOG(INFO) << "Starting remote bootstrap: " << req.ShortDebugString();

  HostPort bootstrap_peer_addr = HostPortFromPB(DesiredHostPort(
      req.source_broadcast_addr(), req.source_private_addr(), req.source_cloud_info(),
      master_->MakeCloudInfoPB()));

  const string& bootstrap_peer_uuid = req.bootstrap_peer_uuid();
  int64_t leader_term = req.caller_term();

  std::shared_ptr<TabletPeer> old_tablet_peer;
  RaftGroupMetadataPtr meta;
  bool replacing_tablet = false;

  if (tablet_exists_) {
    old_tablet_peer = tablet_peer();
    // Nothing to recover if the remote bootstrap client start failed the last time.
    if (old_tablet_peer) {
      meta = old_tablet_peer->tablet_metadata();
      replacing_tablet = true;
    }
  }

  if (replacing_tablet) {
    // Make sure the existing tablet peer is shut down and tombstoned.
    RETURN_NOT_OK(tserver::HandleReplacingStaleTablet(meta,
                                                      old_tablet_peer,
                                                      tablet_id,
                                                      master_->fs_manager()->uuid(),
                                                      leader_term));
  }

  LOG_WITH_PREFIX(INFO) << " Initiating remote bootstrap from peer " << bootstrap_peer_uuid
            << " (" << bootstrap_peer_addr.ToString() << ").";

  auto rb_client = std::make_unique<tserver::RemoteBootstrapClient>(
      tablet_id, master_->fs_manager());

  // Download and persist the remote superblock in TABLET_DATA_COPYING state.
  if (replacing_tablet) {
    RETURN_NOT_OK(rb_client->SetTabletToReplace(meta, leader_term));
  }
  RETURN_NOT_OK(rb_client->Start(
      bootstrap_peer_uuid, &master_->proxy_cache(), bootstrap_peer_addr, &meta));
  // This SetupTabletPeer is needed by rb_client to perform the remote bootstrap/fetch.
  // And the SetupTablet below to perform "local bootstrap" cannot be done until the remote fetch
  // has succeeded. So keeping them seperate for now.
  sys_catalog_->SetupTabletPeer(meta);
  if (PREDICT_FALSE(FLAGS_TEST_inject_latency_during_remote_bootstrap_secs)) {
    LOG(INFO) << "Injecting " << FLAGS_TEST_inject_latency_during_remote_bootstrap_secs
              << " seconds of latency for test";
    SleepFor(MonoDelta::FromSeconds(FLAGS_TEST_inject_latency_during_remote_bootstrap_secs));
  }

  // From this point onward, the superblock is persisted in TABLET_DATA_COPYING
  // state, and we need to tombstone the tablet if additional steps prior to
  // getting to a TABLET_DATA_READY state fail.
  tablet_exists_ = true;

  // Download all of the remote files.
  TOMBSTONE_NOT_OK(rb_client->FetchAll(tablet_peer()->status_listener()),
                   meta,
                   master_->fs_manager()->uuid(),
                   Substitute("Remote bootstrap: Unable to fetch data from remote peer $0 ($1)",
                              bootstrap_peer_uuid, bootstrap_peer_addr.ToString()),
                   nullptr);

  // Write out the last files to make the new replica visible and update the
  // TabletDataState in the superblock to TABLET_DATA_READY.
  // Finish() will call EndRemoteSession() and wait for the leader to successfully submit a
  // ChangeConfig request (to change this master's role from PRE_VOTER or PRE_OBSERVER to VOTER or
  // OBSERVER respectively). If the RPC times out, we will ignore the error (since the leader could
  // have successfully submitted the ChangeConfig request and failed to respond before in time)
  // and check the committed config until we find that this master's role has changed, or until we
  // time out which will cause us to tombstone the tablet.
  TOMBSTONE_NOT_OK(rb_client->Finish(),
                   meta,
                   master_->fs_manager()->uuid(),
                   "Remote bootstrap: Failed calling Finish()",
                   nullptr);

  // Synchronous tablet open for "local bootstrap".
  RETURN_NOT_OK(tserver::ShutdownAndTombstoneTabletPeerNotOk(
      sys_catalog_->OpenTablet(meta), sys_catalog_->tablet_peer(), meta,
      master_->fs_manager()->uuid(), "Remote bootstrap: Failed opening sys catalog"));

  // Set up the in-memory master list and also flush the cmeta.
  RETURN_NOT_OK(UpdateMastersListInMemoryAndDisk());

  master_->SetShellMode(false);

  // Call VerifyChangeRoleSucceeded only after we have set shell mode to false. Otherwise,
  // CatalogManager::GetTabletPeer will always return an error, and the consensus will never get
  // updated.
  auto status = rb_client->VerifyChangeRoleSucceeded(
      sys_catalog_->tablet_peer()->shared_consensus());

  if (!status.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Remote bootstrap finished. "
                             << "Failed calling VerifyChangeRoleSucceeded: "
                             << status.ToString();
  } else {
    LOG_WITH_PREFIX(INFO) << "Remote bootstrap finished successfully";
  }

  LOG(INFO) << "Master completed remote bootstrap and is out of shell mode.";

  RETURN_NOT_OK(EnableBgTasks());

  return Status::OK();
}

void CatalogManager::SendAlterTableRequest(const scoped_refptr<TableInfo>& table,
                                           const AlterTableRequestPB* req) {
  vector<scoped_refptr<TabletInfo>> tablets;
  table->GetAllTablets(&tablets);

  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    auto call = std::make_shared<AsyncAlterTable>(master_, AsyncTaskPool(), tablet, table);
    tablet->table()->AddTask(call);
    WARN_NOT_OK(ScheduleTask(call), "Failed to send alter table request");
  }
}

void CatalogManager::SendCopartitionTabletRequest(const scoped_refptr<TabletInfo>& tablet,
                                                  const scoped_refptr<TableInfo>& table) {
  auto call = std::make_shared<AsyncCopartitionTable>(master_, AsyncTaskPool(), tablet, table);
  table->AddTask(call);
  WARN_NOT_OK(ScheduleTask(call), "Failed to send copartition table request");
}

void CatalogManager::SendSplitTabletRequest(
    const scoped_refptr<TabletInfo>& tablet, std::array<TabletId, kNumSplitParts> new_tablet_ids,
    const std::string& split_encoded_key, const std::string& split_partition_key) {
  VLOG(2) << "Scheduling SplitTablet request to leader tserver for source tablet ID: "
          << tablet->tablet_id() << ", after-split tablet IDs: " << AsString(new_tablet_ids);
  auto call = std::make_shared<AsyncSplitTablet>(
      master_, AsyncTaskPool(), tablet, new_tablet_ids, split_encoded_key, split_partition_key);
  tablet->table()->AddTask(call);
  WARN_NOT_OK(
      ScheduleTask(call),
      Format("Failed to send split tablet request for tablet $0", tablet->tablet_id()));
}

void CatalogManager::DeleteTabletReplicas(TabletInfo* tablet, const std::string& msg) {
  auto locations = tablet->GetReplicaLocations();
  LOG(INFO) << "Sending DeleteTablet for " << locations->size()
            << " replicas of tablet " << tablet->tablet_id();
  for (const TabletInfo::ReplicaMap::value_type& r : *locations) {
    SendDeleteTabletRequest(tablet->tablet_id(), TABLET_DATA_DELETED,
                            boost::none, tablet->table(), r.second.ts_desc, msg);
  }
}

Status CatalogManager::CheckIfForbiddenToDeleteTabletOf(const scoped_refptr<TableInfo>& table) {
  // Do not delete the system catalog tablet.
  if (IsSystemTable(*table)) {
    return STATUS(InvalidArgument, "It is not allowed to delete system tables");
  }
  // Do not delete the tablet of a colocated table.
  if (IsColocatedUserTable(*table)) {
    return STATUS(InvalidArgument, "It is not allowed to delete tablets of the colocated tables.");
  }
  return Status::OK();
}

void CatalogManager::DeleteTabletsAndSendRequests(const scoped_refptr<TableInfo>& table) {
  if (!CheckIfForbiddenToDeleteTabletOf(table).ok()) {
    return;
  }

  vector<scoped_refptr<TabletInfo>> tablets;
  table->GetAllTablets(&tablets);

  string deletion_msg = "Table deleted at " + LocalTimeAsString();

  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    DeleteTabletAndSendRequests(tablet, deletion_msg);
  }
  if (IsColocatedParentTable(*table)) {
    SharedLock<LockType> catalog_lock(lock_);
    colocated_tablet_ids_map_.erase(table->namespace_id());
  } else if (IsTablegroupParentTable(*table)) {
    // In the case of dropped database/tablegroup parent table, need to delete tablegroup info.
    SharedLock<LockType> catalog_lock(lock_);
    for (auto tgroup : tablegroup_tablet_ids_map_[table->namespace_id()]) {
      tablegroup_ids_map_.erase(tgroup.first);
    }
    tablegroup_tablet_ids_map_.erase(table->namespace_id());
  }
}

void CatalogManager::DeleteTabletAndSendRequests(
    const scoped_refptr<TabletInfo>& tablet, const std::string& deletion_msg) {
  LOG(INFO) << "Deleting tablet " << tablet->tablet_id() << " ...";
  DeleteTabletReplicas(tablet.get(), deletion_msg);

  auto tablet_lock = tablet->LockForWrite();
  tablet_lock->mutable_data()->set_state(SysTabletsEntryPB::DELETED, deletion_msg);
  CHECK_OK(sys_catalog_->UpdateItem(tablet.get(), leader_ready_term()));
  tablet_lock->Commit();
  LOG(INFO) << "Deleted tablet " << tablet->tablet_id();
}

void CatalogManager::SendDeleteTabletRequest(
    const TabletId& tablet_id,
    TabletDataState delete_type,
    const boost::optional<int64_t>& cas_config_opid_index_less_or_equal,
    const scoped_refptr<TableInfo>& table,
    TSDescriptor* ts_desc,
    const string& reason) {
  LOG_WITH_PREFIX(INFO) << "Deleting tablet " << tablet_id << " on peer "
                        << ts_desc->permanent_uuid() << " with delete type "
                        << TabletDataState_Name(delete_type) << " (" << reason << ")";
  auto call = std::make_shared<AsyncDeleteReplica>(master_, AsyncTaskPool(),
      ts_desc->permanent_uuid(), table, tablet_id, delete_type,
      cas_config_opid_index_less_or_equal, reason);
  if (table != nullptr) {
    table->AddTask(call);
  }

  auto status = ScheduleTask(call);
  WARN_NOT_OK(status, Substitute("Failed to send delete request for tablet $0", tablet_id));
  // TODO(bogdan): does the pending delete semantics need to change?
  if (status.ok()) {
    ts_desc->AddPendingTabletDelete(tablet_id);
  }
}

void CatalogManager::SendLeaderStepDownRequest(
    const scoped_refptr<TabletInfo>& tablet, const ConsensusStatePB& cstate,
    const string& change_config_ts_uuid, bool should_remove,
    const string& new_leader_uuid) {
  auto task = std::make_shared<AsyncTryStepDown>(
      master_, AsyncTaskPool(), tablet, cstate, change_config_ts_uuid, should_remove,
      new_leader_uuid);
  tablet->table()->AddTask(task);
  Status status = ScheduleTask(task);
  WARN_NOT_OK(status, Substitute("Failed to send new $0 request", task->type_name()));
}

// TODO: refactor this into a joint method with the add one.
void CatalogManager::SendRemoveServerRequest(
    const scoped_refptr<TabletInfo>& tablet, const ConsensusStatePB& cstate,
    const string& change_config_ts_uuid) {
  // Check if the user wants the leader to be stepped down.
  auto task = std::make_shared<AsyncRemoveServerTask>(
      master_, AsyncTaskPool(), tablet, cstate, change_config_ts_uuid);
  tablet->table()->AddTask(task);
  WARN_NOT_OK(ScheduleTask(task), Substitute("Failed to send new $0 request", task->type_name()));
}

void CatalogManager::SendAddServerRequest(
    const scoped_refptr<TabletInfo>& tablet, RaftPeerPB::MemberType member_type,
    const ConsensusStatePB& cstate, const string& change_config_ts_uuid) {
  auto task = std::make_shared<AsyncAddServerTask>(master_, AsyncTaskPool(), tablet, member_type,
      cstate, change_config_ts_uuid);
  tablet->table()->AddTask(task);
  WARN_NOT_OK(
      ScheduleTask(task),
      Substitute("Failed to send AddServer of tserver $0 to tablet $1",
                 change_config_ts_uuid, tablet.get()->ToString()));
}

void CatalogManager::GetPendingServerTasksUnlocked(
    const TableId &table_uuid,
    TabletToTabletServerMap *add_replica_tasks_map,
    TabletToTabletServerMap *remove_replica_tasks_map,
    TabletToTabletServerMap *stepdown_leader_tasks_map) {

  auto table = GetTableInfoUnlocked(table_uuid);
  for (const auto& task : table->GetTasks()) {
    TabletToTabletServerMap* outputMap = nullptr;
    if (task->type() == MonitoredTask::ASYNC_ADD_SERVER) {
      outputMap = add_replica_tasks_map;
    } else if (task->type() == MonitoredTask::ASYNC_REMOVE_SERVER) {
      outputMap = remove_replica_tasks_map;
    } else if (task->type() == MonitoredTask::ASYNC_TRY_STEP_DOWN) {
      // Store new_leader_uuid instead of change_config_ts_uuid.
      auto raft_task = static_cast<AsyncTryStepDown*>(task.get());
      (*stepdown_leader_tasks_map)[raft_task->tablet_id()] = raft_task->new_leader_uuid();
      continue;
    }
    if (outputMap) {
      auto raft_task = static_cast<CommonInfoForRaftTask*>(task.get());
      (*outputMap)[raft_task->tablet_id()] = raft_task->change_config_ts_uuid();
    }
  }
}

void CatalogManager::ExtractTabletsToProcess(
    TabletInfos *tablets_to_delete,
    TabletInfos *tablets_to_process) {
  SharedLock<LockType> l(lock_);

  // TODO: At the moment we loop through all the tablets
  //       we can keep a set of tablets waiting for "assignment"
  //       or just a counter to avoid to take the lock and loop through the tablets
  //       if everything is "stable".

  for (const TabletInfoMap::value_type& entry : *tablet_map_) {
    scoped_refptr<TabletInfo> tablet = entry.second;
    auto table = tablet->table();
    if (!table) {
      // Tablet is orphaned or in preparing state, continue.
      continue;
    }

    // acquire table lock before tablets.
    auto table_lock = table->LockForRead();
    auto tablet_lock = tablet->LockForRead();

    // If the table is deleted or the tablet was replaced at table creation time.
    if (tablet_lock->data().is_deleted() || table_lock->data().started_deleting()) {
      // Process this table deletion only once (tombstones for table may remain longer).
      if (table_ids_map_->find(tablet->table()->id()) != table_ids_map_->end()) {
        tablets_to_delete->push_back(tablet);
      }
      // Don't process deleted tables regardless.
      continue;
    }

    // Running tablets.
    if (tablet_lock->data().is_running()) {
      // TODO: handle last update > not responding timeout?
      continue;
    }

    // Tablets not yet assigned or with a report just received.
    tablets_to_process->push_back(tablet);
  }
}

bool CatalogManager::AreTablesDeleting() {
  SharedLock<LockType> catalog_lock(lock_);

  for (const TableInfoMap::value_type& entry : *table_ids_map_) {
    scoped_refptr<TableInfo> table(entry.second);
    auto table_lock = table->LockForRead();
    // TODO(jason): possibly change this to started_deleting when we begin removing DELETED tables
    // from table_ids_map_ (see CleanUpDeletedTables).
    if (table_lock->data().is_deleting()) {
      return true;
    }
  }
  return false;
}

struct DeferredAssignmentActions {
  vector<TabletInfo*> tablets_to_add;
  vector<TabletInfo*> tablets_to_update;
  vector<TabletInfo*> needs_create_rpc;
};

void CatalogManager::HandleAssignPreparingTablet(TabletInfo* tablet,
                                                 DeferredAssignmentActions* deferred) {
  // The tablet was just created (probably by a CreateTable RPC).
  // Update the state to "creating" to be ready for the creation request.
  tablet->mutable_metadata()->mutable_dirty()->set_state(
    SysTabletsEntryPB::CREATING, "Sending initial creation of tablet");
  deferred->tablets_to_update.push_back(tablet);
  deferred->needs_create_rpc.push_back(tablet);
  VLOG(1) << "Assign new tablet " << tablet->ToString();
}

void CatalogManager::HandleAssignCreatingTablet(TabletInfo* tablet,
                                                DeferredAssignmentActions* deferred,
                                                vector<scoped_refptr<TabletInfo>>* new_tablets) {
  MonoDelta time_since_updated =
      MonoTime::Now().GetDeltaSince(tablet->last_update_time());
  int64_t remaining_timeout_ms =
      FLAGS_tablet_creation_timeout_ms - time_since_updated.ToMilliseconds();

  if (tablet->LockForRead()->data().pb.has_split_parent_tablet_id()) {
    // No need to recreate post-split tablets, since this is always done on source tablet replicas.
    VLOG(2) << "Post-split tablet " << AsString(tablet) << " still being created.";
    return;
  }
  // Skip the tablet if the assignment timeout is not yet expired.
  if (remaining_timeout_ms > 0) {
    VLOG(2) << "Tablet " << tablet->ToString() << " still being created. "
            << remaining_timeout_ms << "ms remain until timeout.";
    return;
  }

  const PersistentTabletInfo& old_info = tablet->metadata().state();

  // The "tablet creation" was already sent, but we didn't receive an answer
  // within the timeout. So the tablet will be replaced by a new one.
  TabletInfo *replacement;
  {
    std::lock_guard<LockType> l_maps(lock_);
    replacement = CreateTabletInfo(tablet->table().get(), old_info.pb.partition());
  }
  LOG(WARNING) << "Tablet " << tablet->ToString() << " was not created within "
               << "the allowed timeout. Replacing with a new tablet "
               << replacement->tablet_id();

  tablet->table()->AddTablet(replacement);
  {
    std::lock_guard<LockType> l_maps(lock_);
    auto tablet_map_checkout = tablet_map_.CheckOut();
    (*tablet_map_checkout)[replacement->tablet_id()] = replacement;
  }

  // Mark old tablet as replaced.
  tablet->mutable_metadata()->mutable_dirty()->set_state(
    SysTabletsEntryPB::REPLACED,
    Substitute("Replaced by $0 at $1",
               replacement->tablet_id(), LocalTimeAsString()));

  // Mark new tablet as being created.
  replacement->mutable_metadata()->mutable_dirty()->set_state(
    SysTabletsEntryPB::CREATING,
    Substitute("Replacement for $0", tablet->tablet_id()));

  deferred->tablets_to_update.push_back(tablet);
  deferred->tablets_to_add.push_back(replacement);
  deferred->needs_create_rpc.push_back(replacement);
  VLOG(1) << "Replaced tablet " << tablet->tablet_id()
          << " with " << replacement->tablet_id()
          << " (table " << tablet->table()->ToString() << ")";

  new_tablets->push_back(replacement);
}

// TODO: we could batch the IO onto a background thread.
Status CatalogManager::HandleTabletSchemaVersionReport(
    TabletInfo *tablet, uint32_t version, const scoped_refptr<TableInfo>& table_info) {
  scoped_refptr<TableInfo> table;
  if (table_info) {
    table = table_info;
  } else {
    table = tablet->table();
  }

  // Update the schema version if it's the latest.
  tablet->set_reported_schema_version(table->id(), version);
  VLOG(1) << "Tablet " << tablet->tablet_id() << " reported version " << version;

  // Verify if it's the last tablet report, and the alter completed.
  {
    auto l = table->LockForRead();
    if (l->data().pb.state() != SysTablesEntryPB::ALTERING) {
      VLOG(2) << "Table " << table->ToString() << " is not altering";
      return Status::OK();
    }

    uint32_t current_version = l->data().pb.version();
    if (table->IsAlterInProgress(current_version)) {
      VLOG(2) << "Table " << table->ToString() << " has IsAlterInProgress ("
              << current_version << ")";
      return Status::OK();
    }
  }

  return MultiStageAlterTable::LaunchNextTableInfoVersionIfNecessary(this, table, version);
}

// Helper class to commit TabletInfo mutations at the end of a scope.
namespace {

class ScopedTabletInfoCommitter {
 public:
  explicit ScopedTabletInfoCommitter(const TabletInfos* tablets)
    : tablets_(DCHECK_NOTNULL(tablets)),
      aborted_(false) {
  }

  // This method is not thread safe. Must be called by the same thread
  // that would destroy this instance.
  void Abort() {
    for (const scoped_refptr<TabletInfo>& tablet : *tablets_) {
      tablet->mutable_metadata()->AbortMutation();
    }
    aborted_ = true;
  }

  void Commit() {
    if (PREDICT_TRUE(!aborted_)) {
      for (const scoped_refptr<TabletInfo>& tablet : *tablets_) {
        tablet->mutable_metadata()->CommitMutation();
      }
    }
  }

  // Commit the transactions.
  ~ScopedTabletInfoCommitter() {
    Commit();
  }

 private:
  const TabletInfos* tablets_;
  bool aborted_;
};
}  // anonymous namespace

Status CatalogManager::ProcessPendingAssignments(const TabletInfos& tablets) {
  VLOG(1) << "Processing pending assignments";

  // Take write locks on all tablets to be processed, and ensure that they are
  // unlocked at the end of this scope.
  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    tablet->mutable_metadata()->StartMutation();
  }
  ScopedTabletInfoCommitter unlocker_in(&tablets);

  // Any tablets created by the helper functions will also be created in a
  // locked state, so we must ensure they are unlocked before we return to
  // avoid deadlocks.
  TabletInfos new_tablets;
  ScopedTabletInfoCommitter unlocker_out(&new_tablets);

  DeferredAssignmentActions deferred;

  // Iterate over each of the tablets and handle it, whatever state
  // it may be in. The actions required for the tablet are collected
  // into 'deferred'.
  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    SysTabletsEntryPB::State t_state = tablet->metadata().state().pb.state();

    switch (t_state) {
      case SysTabletsEntryPB::PREPARING:
        HandleAssignPreparingTablet(tablet.get(), &deferred);
        break;

      case SysTabletsEntryPB::CREATING:
        HandleAssignCreatingTablet(tablet.get(), &deferred, &new_tablets);
        break;

      default:
        VLOG(2) << "Nothing to do for tablet " << tablet->tablet_id() << ": state = "
                << SysTabletsEntryPB_State_Name(t_state);
        break;
    }
  }

  // Nothing to do.
  if (deferred.tablets_to_add.empty() &&
      deferred.tablets_to_update.empty() &&
      deferred.needs_create_rpc.empty()) {
    return Status::OK();
  }

  // For those tablets which need to be created in this round, assign replicas.
  TSDescriptorVector ts_descs;
  {
    SharedLock<LockType> l(blacklist_lock_);
    master_->ts_manager()->GetAllLiveDescriptors(&ts_descs, blacklistState.tservers_);
  }
  Status s;
  unordered_set<TableInfo*> ok_status_tables;
  for (TabletInfo *tablet : deferred.needs_create_rpc) {
    // NOTE: if we fail to select replicas on the first pass (due to
    // insufficient Tablet Servers being online), we will still try
    // again unless the tablet/table creation is cancelled.
    s = SelectReplicasForTablet(ts_descs, tablet);
    if (!s.ok()) {
      s = s.CloneAndPrepend(Substitute(
          "An error occurred while selecting replicas for tablet $0: $1",
          tablet->tablet_id(), s.ToString()));
      tablet->table()->SetCreateTableErrorStatus(s);
      break;
    } else {
      ok_status_tables.emplace(tablet->table().get());
    }
  }

  // Update the sys catalog with the new set of tablets/metadata.
  if (s.ok()) {
    // If any of the ok_status_tables had an error in the previous iterations, we
    // need to clear up the error status to reflect that all the create tablets have now
    // succeded.
    for (TableInfo* table : ok_status_tables) {
      table->SetCreateTableErrorStatus(Status::OK());
    }

    s = sys_catalog_->AddAndUpdateItems(deferred.tablets_to_add,
                                        deferred.tablets_to_update,
                                        leader_ready_term());
    if (!s.ok()) {
      s = s.CloneAndPrepend("An error occurred while persisting the updated tablet metadata");
    }
  }

  if (!s.ok()) {
    LOG(WARNING) << "Aborting the current task due to error: " << s.ToString();
    // If there was an error, abort any mutations started by the current task.
    // NOTE: Lock order should be lock_ -> table -> tablet.
    // We currently have a bunch of tablets locked and need to unlock first to ensure this holds.
    map<TabletId, pair<scoped_refptr<TableInfo>, string /* partition key */>> tablet_ids_to_remove;
    for (scoped_refptr<TabletInfo>& new_tablet : new_tablets) {
      tablet_ids_to_remove[new_tablet->tablet_id()] = make_pair(
          new_tablet->table(),
          new_tablet->metadata().dirty().pb.partition().partition_key_start()
          );
    }

    unlocker_out.Abort(); // tablet.unlock
    unlocker_in.Abort();
    for (auto &tablet_id_to_remove : tablet_ids_to_remove) {
      TableInfo* table = tablet_id_to_remove.second.first.get();
      auto l_table = table->LockForWrite(); // table.lock
      if (table->RemoveTablet(tablet_id_to_remove.second.second)) {
        VLOG(1) << "Removed tablet " << tablet_id_to_remove.first << " from "
            "table " << l_table->data().name();
      }
    }
    {
      std::lock_guard <LockType> l(lock_); // lock_.lock
      auto tablet_map_checkout = tablet_map_.CheckOut();
      for (auto &tablet_id_to_remove : tablet_ids_to_remove) {
        // Potential race condition above, but it's okay if a background thread deleted this.
        tablet_map_checkout->erase(tablet_id_to_remove.first);
      }
    }
    return s;
  }

  // Send DeleteTablet requests to tablet servers serving deleted tablets.
  // This is asynchronous / non-blocking.
  for (auto* tablet : deferred.tablets_to_update) {
    if (tablet->metadata().dirty().is_deleted()) {
      DeleteTabletReplicas(tablet, tablet->metadata().dirty().pb.state_msg());
    }
  }
  // Send the CreateTablet() requests to the servers. This is asynchronous / non-blocking.
  SendCreateTabletRequests(deferred.needs_create_rpc);
  return Status::OK();
}

Status CatalogManager::SelectReplicasForTablet(const TSDescriptorVector& ts_descs,
                                               TabletInfo* tablet) {
  auto table_guard = tablet->table()->LockForRead();

  if (!table_guard->data().pb.IsInitialized()) {
    return STATUS_SUBSTITUTE(InvalidArgument,
        "TableInfo for tablet $0 is not initialized (aborted CreateTable attempt?)",
        tablet->tablet_id());
  }

  const ReplicationInfoPB& replication_info = VERIFY_RESULT(ResolveReplicationInfo(
    table_guard->data().pb.replication_info()));

  // Select the set of replicas for the tablet.
  ConsensusStatePB* cstate = tablet->mutable_metadata()->mutable_dirty()
          ->pb.mutable_committed_consensus_state();
  VLOG_WITH_FUNC(3) << "Committed consensus state: " << AsString(cstate);
  cstate->set_current_term(kMinimumTerm);
  consensus::RaftConfigPB *config = cstate->mutable_config();
  config->set_opid_index(consensus::kInvalidOpIdIndex);

  Status s = HandlePlacementUsingReplicationInfo(replication_info, ts_descs, config);
  if (!s.ok()) {
    return s;
  }

  std::ostringstream out;
  out << "Initial tserver uuids for tablet " << tablet->tablet_id() << ": ";
  for (const RaftPeerPB& peer : config->peers()) {
    out << peer.permanent_uuid() << " ";
  }

  if (VLOG_IS_ON(0)) {
    out.str();
  }

  VLOG_WITH_FUNC(3) << "Committed consensus state has been updated to: " << AsString(cstate);

  return Status::OK();
}

Status CatalogManager::HandlePlacementUsingReplicationInfo(
    const ReplicationInfoPB& replication_info,
    const TSDescriptorVector& all_ts_descs,
    consensus::RaftConfigPB* config) {
  return HandlePlacementUsingPlacementInfo(replication_info.live_replicas(),
                                           all_ts_descs, RaftPeerPB::VOTER, config);
}

Status CatalogManager::HandlePlacementUsingPlacementInfo(const PlacementInfoPB& placement_info,
                                                         const TSDescriptorVector& ts_descs,
                                                         RaftPeerPB::MemberType member_type,
                                                         consensus::RaftConfigPB* config) {
  int nreplicas = GetNumReplicasFromPlacementInfo(placement_info);
  if (ts_descs.size() < nreplicas) {
    return STATUS_SUBSTITUTE(InvalidArgument,
        "Not enough tablet servers in the requested placements. Need at least $0, have $1",
        nreplicas, ts_descs.size());
  }
  // Keep track of servers we've already selected, so that we don't attempt to
  // put two replicas on the same host.
  set<shared_ptr<TSDescriptor>> already_selected_ts;
  if (placement_info.placement_blocks().empty()) {
    // If we don't have placement info, just place the replicas as before, distributed across the
    // whole cluster.
    SelectReplicas(ts_descs, nreplicas, config, &already_selected_ts, member_type);
  } else {
    // TODO(bogdan): move to separate function
    //
    // If we do have placement info, we'll try to use the same power of two algorithm, but also
    // match the requested policies. We'll assign the minimum requested replicas in each combination
    // of cloud.region.zone and then if we still have leftover replicas, we'll assign those
    // in any of the allowed areas.
    unordered_map<string, vector<shared_ptr<TSDescriptor>>> allowed_ts_by_pi;
    vector<shared_ptr<TSDescriptor>> all_allowed_ts;

    // Keep map from ID to PlacementBlockPB, as protos only have repeated, not maps.
    unordered_map<string, PlacementBlockPB> pb_by_id;
    for (const auto& pb : placement_info.placement_blocks()) {
      const auto& cloud_info = pb.cloud_info();
      string placement_id = TSDescriptor::generate_placement_id(cloud_info);
      pb_by_id[placement_id] = pb;
    }

    // Build the sets of allowed TSs.
    for (const auto& ts : ts_descs) {
      bool added_to_all = false;
      for (const auto& pi_entry : pb_by_id) {
        if (ts->MatchesCloudInfo(pi_entry.second.cloud_info())) {
          allowed_ts_by_pi[pi_entry.first].push_back(ts);

          if (!added_to_all) {
            added_to_all = true;
            all_allowed_ts.push_back(ts);
          }
        }
      }
    }

    // Fail early if we don't have enough tablet servers in the areas requested.
    if (all_allowed_ts.size() < nreplicas) {
      return STATUS_SUBSTITUTE(InvalidArgument,
          "Not enough tablet servers in the requested placements. Need at least $0, have $1",
          nreplicas, all_allowed_ts.size());
    }

    // Loop through placements and assign to respective available TSs.
    for (const auto& entry : allowed_ts_by_pi) {
      const auto& available_ts_descs = entry.second;
      int num_replicas = pb_by_id[entry.first].min_num_replicas();
      num_replicas = num_replicas > 0 ? num_replicas : FLAGS_replication_factor;
      if (available_ts_descs.size() < num_replicas) {
        return STATUS_SUBSTITUTE(InvalidArgument,
            "Not enough tablet servers in $0. Need at least $1 but only have $2.", entry.first,
            num_replicas, available_ts_descs.size());
      }
      SelectReplicas(available_ts_descs, num_replicas, config, &already_selected_ts, member_type);
    }

    int replicas_left = nreplicas - already_selected_ts.size();
    DCHECK_GE(replicas_left, 0);
    if (replicas_left > 0) {
      // No need to do an extra check here, as we checked early if we have enough to cover all
      // requested placements and checked individually per placement info, if we could cover the
      // minimums.
      SelectReplicas(all_allowed_ts, replicas_left, config, &already_selected_ts, member_type);
    }
  }
  return Status::OK();
}

void CatalogManager::SendCreateTabletRequests(const vector<TabletInfo*>& tablets) {
  for (TabletInfo *tablet : tablets) {
    const consensus::RaftConfigPB& config =
        tablet->metadata().dirty().pb.committed_consensus_state().config();
    tablet->set_last_update_time(MonoTime::Now());
    for (const RaftPeerPB& peer : config.peers()) {
      auto task = std::make_shared<AsyncCreateReplica>(master_, AsyncTaskPool(),
          peer.permanent_uuid(), tablet);
      tablet->table()->AddTask(task);
      WARN_NOT_OK(ScheduleTask(task), "Failed to send new tablet request");
    }
  }
}

// If responses have been received from sufficient replicas (including hinted leader),
// pick proposed leader and start election.
void CatalogManager::StartElectionIfReady(
    const consensus::ConsensusStatePB& cstate, TabletInfo* tablet) {
  auto replicas = tablet->GetReplicaLocations();
  int num_voters = 0;
  for (const auto& peer : cstate.config().peers()) {
    if (peer.member_type() == RaftPeerPB::VOTER) {
      ++num_voters;
    }
  }
  int majority_size = num_voters / 2 + 1;
  int running_voters = 0;
  for (const auto& replica : *replicas) {
    if (replica.second.member_type == RaftPeerPB::VOTER) {
      ++running_voters;
    }
  }

  VLOG_WITH_PREFIX(4)
      << __func__ << ": T " << tablet->tablet_id() << ": " << AsString(*replicas) << ", voters: "
      << running_voters << "/" << majority_size;

  if (running_voters < majority_size) {
    VLOG_WITH_PREFIX(4) << __func__ << ": Not enough voters";
    return;
  }

  ReplicationInfoPB replication_info;
  {
    auto l = cluster_config_->LockForRead();
    replication_info = l->data().pb.replication_info();
  }

  // Find tservers that can be leaders for a tablet.
  TSDescriptorVector ts_descs;
  {
    SharedLock<LockType> l(blacklist_lock_);
    master_->ts_manager()->GetAllLiveDescriptors(&ts_descs, leaderBlacklistState.tservers_);
  }

  std::vector<std::string> possible_leaders;
  for (const auto& replica : *replicas) {
    for (const auto& ts_desc : ts_descs) {
      if (ts_desc->permanent_uuid() == replica.first) {
        if (ts_desc->IsAcceptingLeaderLoad(replication_info)) {
          possible_leaders.push_back(replica.first);
        }
        break;
      }
    }
  }

  if (FLAGS_TEST_create_table_leader_hint_min_lexicographic) {
    std::string min_lexicographic;
    for (const auto& peer : cstate.config().peers()) {
      if (peer.member_type() == RaftPeerPB::VOTER) {
        if (min_lexicographic.empty() || peer.permanent_uuid() < min_lexicographic) {
          min_lexicographic = peer.permanent_uuid();
        }
      }
    }
    if (min_lexicographic.empty() || !replicas->count(min_lexicographic)) {
      LOG_WITH_PREFIX(INFO)
          << __func__ << ": Min lexicographic is not yet ready: " << min_lexicographic;
      return;
    }
    possible_leaders = { min_lexicographic };
  }

  if (possible_leaders.empty()) {
    VLOG_WITH_PREFIX(4) << __func__ << ": Cannot pick candidate";
    return;
  }

  if (!tablet->InitiateElection()) {
    VLOG_WITH_PREFIX(4) << __func__ << ": Already initiated";
    return;
  }

  const auto& protege = RandomElement(possible_leaders);

  LOG_WITH_PREFIX(INFO)
      << "Starting election at " << tablet->tablet_id() << " in favor of " << protege;

  auto task = std::make_shared<AsyncStartElection>(master_, AsyncTaskPool(), protege, tablet);
  tablet->table()->AddTask(task);
  WARN_NOT_OK(task->Run(), "Failed to send new tablet start election request");
}

shared_ptr<TSDescriptor> CatalogManager::PickBetterReplicaLocation(
    const TSDescriptorVector& two_choices) {
  DCHECK_EQ(two_choices.size(), 2);

  const auto& a = two_choices[0];
  const auto& b = two_choices[1];

  // When creating replicas, we consider two aspects of load:
  //   (1) how many tablet replicas are already on the server, and
  //   (2) how often we've chosen this server recently.
  //
  // The first factor will attempt to put more replicas on servers that
  // are under-loaded (eg because they have newly joined an existing cluster, or have
  // been reformatted and re-joined).
  //
  // The second factor will ensure that we take into account the recent selection
  // decisions even if those replicas are still in the process of being created (and thus
  // not yet reported by the server). This is important because, while creating a table,
  // we batch the selection process before sending any creation commands to the
  // servers themselves.
  //
  // TODO: in the future we may want to factor in other items such as available disk space,
  // actual request load, etc.
  double load_a = a->RecentReplicaCreations() + a->num_live_replicas();
  double load_b = b->RecentReplicaCreations() + b->num_live_replicas();
  if (load_a < load_b) {
    return a;
  } else if (load_b < load_a) {
    return b;
  } else {
    // If the load is the same, we can just pick randomly.
    return two_choices[rng_.Uniform(2)];
  }
}

shared_ptr<TSDescriptor> CatalogManager::SelectReplica(
    const TSDescriptorVector& ts_descs,
    const set<shared_ptr<TSDescriptor>>& excluded) {
  // The replica selection algorithm follows the idea from
  // "Power of Two Choices in Randomized Load Balancing"[1]. For each replica,
  // we randomly select two tablet servers, and then assign the replica to the
  // less-loaded one of the two. This has some nice properties:
  //
  // 1) because the initial selection of two servers is random, we get good
  //    spreading of replicas across the cluster. In contrast if we sorted by
  //    load and always picked under-loaded servers first, we'd end up causing
  //    all tablets of a new table to be placed on an empty server. This wouldn't
  //    give good load balancing of that table.
  //
  // 2) because we pick the less-loaded of two random choices, we do end up with a
  //    weighting towards filling up the underloaded one over time, without
  //    the extreme scenario above.
  //
  // 3) because we don't follow any sequential pattern, every server is equally
  //    likely to replicate its tablets to every other server. In contrast, a
  //    round-robin design would enforce that each server only replicates to its
  //    adjacent nodes in the TS sort order, limiting recovery bandwidth (see
  //    KUDU-1317).
  //
  // [1] http://www.eecs.harvard.edu/~michaelm/postscripts/mythesis.pdf

  // Pick two random servers, excluding those we've already picked.
  // If we've only got one server left, 'two_choices' will actually
  // just contain one element.
  vector<shared_ptr<TSDescriptor>> two_choices;
  rng_.ReservoirSample(ts_descs, 2, excluded, &two_choices);

  if (two_choices.size() == 2) {
    // Pick the better of the two.
    return PickBetterReplicaLocation(two_choices);
  }

  // If we couldn't randomly sample two servers, it's because we only had one
  // more non-excluded choice left.
  CHECK_EQ(1, two_choices.size()) << "ts_descs: " << ts_descs.size()
                                  << " already_sel: " << excluded.size();
  return two_choices[0];
}

void CatalogManager::SelectReplicas(
    const TSDescriptorVector& ts_descs, int nreplicas, consensus::RaftConfigPB* config,
    set<shared_ptr<TSDescriptor>>* already_selected_ts, RaftPeerPB::MemberType member_type) {
  DCHECK_LE(nreplicas, ts_descs.size());

  for (int i = 0; i < nreplicas; ++i) {
    // We have to derefence already_selected_ts here, as the inner mechanics uses ReservoirSample,
    // which in turn accepts only a reference to the set, not a pointer. Alternatively, we could
    // have passed it in as a non-const reference, but that goes against our argument passing
    // convention.
    //
    // TODO(bogdan): see if we indeed want to switch back to non-const reference.
    shared_ptr<TSDescriptor> ts = SelectReplica(ts_descs, *already_selected_ts);
    InsertOrDie(already_selected_ts, ts);

    // Increment the number of pending replicas so that we take this selection into
    // account when assigning replicas for other tablets of the same table. This
    // value decays back to 0 over time.
    ts->IncrementRecentReplicaCreations();

    TSRegistrationPB reg = ts->GetRegistration();

    RaftPeerPB *peer = config->add_peers();
    peer->set_permanent_uuid(ts->permanent_uuid());

    // TODO: This is temporary, we will use only UUIDs.
    TakeRegistration(reg.mutable_common(), peer);
    peer->set_member_type(member_type);
  }
}

Status CatalogManager::ConsensusStateToTabletLocations(const consensus::ConsensusStatePB& cstate,
                                                       TabletLocationsPB* locs_pb) {
  for (const consensus::RaftPeerPB& peer : cstate.config().peers()) {
    TabletLocationsPB_ReplicaPB* replica_pb = locs_pb->add_replicas();
    if (!peer.has_permanent_uuid()) {
      return STATUS_SUBSTITUTE(IllegalState, "Missing UUID $0", peer.ShortDebugString());
    }
    replica_pb->set_role(GetConsensusRole(peer.permanent_uuid(), cstate));
    if (peer.has_member_type()) {
      replica_pb->set_member_type(peer.member_type());
    } else {
      replica_pb->set_member_type(RaftPeerPB::UNKNOWN_MEMBER_TYPE);
    }
    TSInfoPB* tsinfo_pb = replica_pb->mutable_ts_info();
    tsinfo_pb->set_permanent_uuid(peer.permanent_uuid());
    CopyRegistration(peer, tsinfo_pb);
  }
  return Status::OK();
}

Status CatalogManager::BuildLocationsForTablet(const scoped_refptr<TabletInfo>& tablet,
                                               TabletLocationsPB* locs_pb) {
  {
    auto l_tablet = tablet->LockForRead();
    locs_pb->set_table_id(l_tablet->data().pb.table_id());
    *locs_pb->mutable_table_ids() = l_tablet->data().pb.table_ids();
  }

  // For system tables, the set of replicas is always the set of masters.
  if (system_tablets_.find(tablet->id()) != system_tablets_.end()) {
    consensus::ConsensusStatePB master_consensus;
    RETURN_NOT_OK(GetCurrentConfig(&master_consensus));
    locs_pb->set_tablet_id(tablet->tablet_id());
    locs_pb->set_stale(false);
    RETURN_NOT_OK(ConsensusStateToTabletLocations(master_consensus, locs_pb));
    return Status::OK();
  }

  TSRegistrationPB reg;

  std::shared_ptr<const TabletInfo::ReplicaMap> locs;
  consensus::ConsensusStatePB cstate;
  {
    auto l_tablet = tablet->LockForRead();
    if (PREDICT_FALSE(l_tablet->data().is_deleted())) {
      std::vector<TabletId> split_tablet_ids;
      for (const auto& split_tablet_id : l_tablet->data().pb.split_tablet_ids()) {
        split_tablet_ids.push_back(split_tablet_id);
      }
      return STATUS(
          NotFound, "Tablet deleted", l_tablet->data().pb.state_msg(),
          SplitChildTabletIdsData(split_tablet_ids));
    }

    if (PREDICT_FALSE(!l_tablet->data().is_running())) {
      return STATUS_FORMAT(ServiceUnavailable, "Tablet $0 not running", tablet->id());
    }

    locs = tablet->GetReplicaLocations();
    if (locs->empty() && l_tablet->data().pb.has_committed_consensus_state()) {
      cstate = l_tablet->data().pb.committed_consensus_state();
    }

    const auto& metadata = tablet->metadata().state().pb;
    locs_pb->mutable_partition()->CopyFrom(metadata.partition());
    locs_pb->set_split_depth(metadata.split_depth());
    locs_pb->set_split_parent_tablet_id(metadata.split_parent_tablet_id());
    for (const auto& split_tablet_id : metadata.split_tablet_ids()) {
      *locs_pb->add_split_tablet_ids() = split_tablet_id;
    }
  }

  locs_pb->set_tablet_id(tablet->tablet_id());
  locs_pb->set_stale(locs->empty());

  // If the locations are cached.
  if (!locs->empty()) {
    if (cstate.IsInitialized() && locs->size() != cstate.config().peers_size()) {
      LOG(WARNING) << "Cached tablet replicas " << locs->size() << " does not match consensus "
                   << cstate.config().peers_size();
    }

    for (const TabletInfo::ReplicaMap::value_type& replica : *locs) {
      TabletLocationsPB_ReplicaPB* replica_pb = locs_pb->add_replicas();
      replica_pb->set_role(replica.second.role);
      replica_pb->set_member_type(replica.second.member_type);
      auto tsinfo_pb = replica.second.ts_desc->GetTSInformationPB();

      TSInfoPB* out_ts_info = replica_pb->mutable_ts_info();
      out_ts_info->set_permanent_uuid(tsinfo_pb->tserver_instance().permanent_uuid());
      CopyRegistration(tsinfo_pb->registration().common(), out_ts_info);
      out_ts_info->set_placement_uuid(tsinfo_pb->registration().common().placement_uuid());
      *out_ts_info->mutable_capabilities() = tsinfo_pb->registration().capabilities();
    }
    return Status::OK();
  }

  // If the locations were not cached.
  // TODO: Why would this ever happen? See KUDU-759.
  if (cstate.IsInitialized()) {
    RETURN_NOT_OK(ConsensusStateToTabletLocations(cstate, locs_pb));
  }

  return Status::OK();
}

Result<shared_ptr<tablet::AbstractTablet>> CatalogManager::GetSystemTablet(const TabletId& id) {
  RETURN_NOT_OK(CheckOnline());

  const auto iter = system_tablets_.find(id);
  if (iter == system_tablets_.end()) {
    return STATUS_SUBSTITUTE(InvalidArgument, "$0 is not a valid system tablet id", id);
  }
  return iter->second;
}

Status CatalogManager::GetTabletLocations(const TabletId& tablet_id, TabletLocationsPB* locs_pb) {
  RETURN_NOT_OK(CheckOnline());
  scoped_refptr<TabletInfo> tablet_info;
  {
    SharedLock<LockType> l(lock_);
    if (!FindCopy(*tablet_map_, tablet_id, &tablet_info)) {
      return STATUS_SUBSTITUTE(NotFound, "Unknown tablet $0", tablet_id);
    }
  }
  Status s = GetTabletLocations(tablet_info, locs_pb);

  int num_replicas = 0;
  if (GetReplicationFactorForTablet(tablet_info, &num_replicas).ok() && num_replicas > 0 &&
      locs_pb->replicas().size() != num_replicas) {
    YB_LOG_EVERY_N_SECS(WARNING, 1)
        << "Expected replicas " << num_replicas << " but found "
        << locs_pb->replicas().size() << " for tablet " << tablet_info->id() << ": "
        << locs_pb->ShortDebugString() << THROTTLE_MSG;
  }
  return s;
}

Status CatalogManager::GetTabletLocations(
    scoped_refptr<TabletInfo> tablet_info, TabletLocationsPB* locs_pb) {
  RETURN_NOT_OK(CheckOnline());

  DCHECK_EQ(locs_pb->replicas().size(), 0);
  locs_pb->mutable_replicas()->Clear();
  return BuildLocationsForTablet(tablet_info, locs_pb);
}

Status CatalogManager::GetTableLocations(const GetTableLocationsRequestPB* req,
                                         GetTableLocationsResponsePB* resp) {
  RETURN_NOT_OK(CheckOnline());
  VLOG(4) << "GetTableLocations: " << req->ShortDebugString();

  // If start-key is > end-key report an error instead of swap the two
  // since probably there is something wrong app-side.
  if (req->has_partition_key_start() && req->has_partition_key_end()
      && req->partition_key_start() > req->partition_key_end()) {
    return STATUS(InvalidArgument, "start partition key is greater than the end partition key");
  }

  if (req->max_returned_locations() <= 0) {
    return STATUS(InvalidArgument, "max_returned_locations must be greater than 0");
  }

  scoped_refptr<TableInfo> table;
  RETURN_NOT_OK(FindTable(req->table(), &table));

  if (table == nullptr) {
    Status s = STATUS(NotFound, "The object does not exist", req->table().ShortDebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
  }

  if (table->IsCreateInProgress()) {
    resp->set_creating(true);
  }

  auto l = table->LockForRead();
  RETURN_NOT_OK(CheckIfTableDeletedOrNotRunning(l.get(), resp));

  vector<scoped_refptr<TabletInfo>> tablets_in_range;
  table->GetTabletsInRange(req, &tablets_in_range);

  bool require_tablets_runnings = req->require_tablets_running();

  int expected_live_replicas = 0;
  int expected_read_replicas = 0;
  GetExpectedNumberOfReplicas(&expected_live_replicas, &expected_read_replicas);
  for (const scoped_refptr<TabletInfo>& tablet : tablets_in_range) {
    TabletLocationsPB* locs_pb = resp->add_tablet_locations();
    locs_pb->set_expected_live_replicas(expected_live_replicas);
    locs_pb->set_expected_read_replicas(expected_read_replicas);
    auto status = BuildLocationsForTablet(tablet, locs_pb);
    if (!status.ok()) {
      // Not running.
      if (require_tablets_runnings) {
        resp->mutable_tablet_locations()->Clear();
        return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, status);
      }
      resp->mutable_tablet_locations()->RemoveLast();
    }
  }

  resp->set_table_type(l->data().pb.table_type());
  resp->set_partitions_version(l->data().pb.partitions_version());

  return Status::OK();
}

Status CatalogManager::GetCurrentConfig(consensus::ConsensusStatePB* cpb) const {
  auto tablet_peer = sys_catalog_->tablet_peer();
  auto consensus = tablet_peer ? tablet_peer->shared_consensus() : nullptr;
  if (!consensus) {
    std::string uuid = master_->fs_manager()->uuid();
    return STATUS_FORMAT(IllegalState, "Node $0 peer not initialized.", uuid);
  }

  *cpb = consensus->ConsensusState(CONSENSUS_CONFIG_COMMITTED);

  return Status::OK();
}

void CatalogManager::DumpState(std::ostream* out, bool on_disk_dump) const {
  NamespaceInfoMap namespace_ids_copy;
  TableInfoMap ids_copy;
  TableInfoByNameMap names_copy;
  TabletInfoMap tablets_copy;

  // Copy the internal state so that, if the output stream blocks,
  // we don't end up holding the lock for a long time.
  {
    SharedLock<LockType> l(lock_);
    namespace_ids_copy = namespace_ids_map_;
    ids_copy = *table_ids_map_;
    names_copy = table_names_map_;
    tablets_copy = *tablet_map_;
  }

  *out << "Dumping current state of master.\nNamespaces:\n";
  for (const NamespaceInfoMap::value_type& e : namespace_ids_copy) {
    NamespaceInfo* t = e.second.get();
    auto l = t->LockForRead();
    const NamespaceName& name = l->data().name();

    *out << t->id() << ":\n";
    *out << "  name: \"" << strings::CHexEscape(name) << "\"\n";
    *out << "  metadata: " << l->data().pb.ShortDebugString() << "\n";
  }

  *out << "Tables:\n";
  for (const TableInfoMap::value_type& e : ids_copy) {
    TableInfo* t = e.second.get();
    auto l = t->LockForRead();
    const TableName& name = l->data().name();
    const NamespaceId& namespace_id = l->data().namespace_id();
    // Find namespace by its ID.
    scoped_refptr<NamespaceInfo> ns = FindPtrOrNull(namespace_ids_copy, namespace_id);

    *out << t->id() << ":\n";
    *out << "  namespace id: \"" << strings::CHexEscape(namespace_id) << "\"\n";

    if (ns != nullptr) {
      *out << "  namespace name: \"" << strings::CHexEscape(ns->name()) << "\"\n";
    }

    *out << "  name: \"" << strings::CHexEscape(name) << "\"\n";
    // Erase from the map, so later we can check that we don't have
    // any orphaned tables in the by-name map that aren't in the
    // by-id map.
    if (names_copy.erase({namespace_id, name}) != 1) {
      *out << "  [not present in by-name map]\n";
    }
    *out << "  metadata: " << l->data().pb.ShortDebugString() << "\n";

    *out << "  tablets:\n";

    vector<scoped_refptr<TabletInfo>> table_tablets;
    t->GetAllTablets(&table_tablets);
    for (const scoped_refptr<TabletInfo>& tablet : table_tablets) {
      auto l_tablet = tablet->LockForRead();
      *out << "    " << tablet->tablet_id() << ": "
           << l_tablet->data().pb.ShortDebugString() << "\n";

      if (tablets_copy.erase(tablet->tablet_id()) != 1) {
        *out << "  [ERROR: not present in CM tablet map!]\n";
      }
    }
  }

  if (!tablets_copy.empty()) {
    *out << "Orphaned tablets (not referenced by any table):\n";
    for (const TabletInfoMap::value_type& entry : tablets_copy) {
      const scoped_refptr<TabletInfo>& tablet = entry.second;
      auto l_tablet = tablet->LockForRead();
      *out << "    " << tablet->tablet_id() << ": "
           << l_tablet->data().pb.ShortDebugString() << "\n";
    }
  }

  if (!names_copy.empty()) {
    *out << "Orphaned tables (in by-name map, but not id map):\n";
    for (const TableInfoByNameMap::value_type& e : names_copy) {
      *out << e.second->id() << ":\n";
      *out << "  namespace id: \"" << strings::CHexEscape(e.first.first) << "\"\n";
      *out << "  name: \"" << CHexEscape(e.first.second) << "\"\n";
    }
  }

  master_->DumpMasterOptionsInfo(out);

  if (on_disk_dump) {
    consensus::ConsensusStatePB cur_consensus_state;
    // TODO: proper error handling below.
    CHECK_OK(GetCurrentConfig(&cur_consensus_state));
    *out << "Current raft config: " << cur_consensus_state.ShortDebugString() << "\n";
  }
}

Status CatalogManager::PeerStateDump(const vector<RaftPeerPB>& peers,
                                     const DumpMasterStateRequestPB* req,
                                     DumpMasterStateResponsePB* resp) {
  std::unique_ptr<MasterServiceProxy> peer_proxy;
  Endpoint sockaddr;
  MonoTime timeout = MonoTime::Now();
  DumpMasterStateRequestPB peer_req;
  rpc::RpcController rpc;

  timeout.AddDelta(MonoDelta::FromMilliseconds(FLAGS_master_ts_rpc_timeout_ms));
  rpc.set_deadline(timeout);
  peer_req.set_on_disk(req->on_disk());
  peer_req.set_return_dump_as_string(req->return_dump_as_string());
  string dump;

  for (const RaftPeerPB& peer : peers) {
    HostPort hostport = HostPortFromPB(DesiredHostPort(peer, master_->MakeCloudInfoPB()));
    peer_proxy.reset(new MasterServiceProxy(&master_->proxy_cache(), hostport));

    DumpMasterStateResponsePB peer_resp;
    rpc.Reset();

    RETURN_NOT_OK(peer_proxy->DumpState(peer_req, &peer_resp, &rpc));

    if (peer_resp.has_error()) {
      LOG(WARNING) << "Hit err " << peer_resp.ShortDebugString() << " during peer "
        << peer.ShortDebugString() << " state dump.";
      return StatusFromPB(peer_resp.error().status());
    } else if (req->return_dump_as_string()) {
      dump += peer_resp.dump();
    }
  }

  if (req->return_dump_as_string()) {
    resp->set_dump(resp->dump() + dump);
  }
  return Status::OK();
}

void CatalogManager::ReportMetrics() {
  // Report metrics on how many tservers are alive.
  TSDescriptorVector ts_descs;
  master_->ts_manager()->GetAllLiveDescriptors(&ts_descs);
  const int32 num_live_servers = ts_descs.size();
  metric_num_tablet_servers_live_->set_value(num_live_servers);

  master_->ts_manager()->GetAllDescriptors(&ts_descs);
  metric_num_tablet_servers_dead_->set_value(ts_descs.size() - num_live_servers);
}

std::string CatalogManager::LogPrefix() const {
  if (tablet_peer()) {
    return consensus::MakeTabletLogPrefix(
        tablet_peer()->tablet_id(), tablet_peer()->permanent_uuid());
  } else {
    return consensus::MakeTabletLogPrefix(
        kSysCatalogTabletId, master_->fs_manager()->uuid());
  }
}

void CatalogManager::SetLoadBalancerEnabled(bool is_enabled) {
  load_balance_policy_->SetLoadBalancerEnabled(is_enabled);
}

bool CatalogManager::IsLoadBalancerEnabled() {
  return load_balance_policy_->IsLoadBalancerEnabled();
}

MonoDelta CatalogManager::TimeSinceElectedLeader() {
  return MonoTime::Now() - time_elected_leader_;
}

Status CatalogManager::GoIntoShellMode() {
  if (master_->IsShellMode()) {
    return STATUS(IllegalState, "Master is already in shell mode.");
  }

  LOG(INFO) << "Starting going into shell mode.";
  master_->SetShellMode(true);

  {
    std::lock_guard<LockType> l(lock_);
    RETURN_NOT_OK(sys_catalog_->GoIntoShellMode());
    background_tasks_->Shutdown();
    background_tasks_.reset();
  }
  {
    std::lock_guard<std::mutex> l(remote_bootstrap_mtx_);
    tablet_exists_ = false;
  }

  LOG(INFO) << "Done going into shell mode.";

  return Status::OK();
}

Status CatalogManager::GetClusterConfig(GetMasterClusterConfigResponsePB* resp) {
  return GetClusterConfig(resp->mutable_cluster_config());
}

Status CatalogManager::GetClusterConfig(SysClusterConfigEntryPB* config) {
  DCHECK(cluster_config_) << "Missing cluster config for master!";
  auto l = cluster_config_->LockForRead();
  *config = l->data().pb;
  return Status::OK();
}

Status CatalogManager::SetBlackList(const BlacklistPB& blacklist) {
  if (!blacklistState.tservers_.empty()) {
    LOG(WARNING) << "Overwriting " << blacklistState.ToString()
                 << " with new size " << blacklist.hosts_size()
                 << " and initial load " << blacklist.initial_replica_load() << ".";
    blacklistState.Reset();
  }

  for (const auto& pb : blacklist.hosts()) {
    blacklistState.tservers_.insert(HostPortFromPB(pb));
  }

  // Set the initial load.
  if (blacklist.has_initial_replica_load()) {
    blacklistState.initial_load_ = blacklist.initial_replica_load();
  }

  LOG(INFO) << "Set blacklist size = " << blacklistState.tservers_.size() << " with load "
            << blacklistState.initial_load_;

  return Status::OK();
}

Status CatalogManager::SetLeaderBlacklist(const BlacklistPB& leader_blacklist) {
  if (!leaderBlacklistState.tservers_.empty()) {
    LOG(WARNING) << "Overwriting " << leaderBlacklistState.ToString()
                 << " with new size " << leader_blacklist.hosts_size()
                 << " and initial load " << leader_blacklist.initial_leader_load() << ".";
    leaderBlacklistState.Reset();
  }

  for (const auto& pb : leader_blacklist.hosts()) {
    leaderBlacklistState.tservers_.insert(HostPortFromPB(pb));
  }

  // Set the initial leader load.
  if (leader_blacklist.has_initial_leader_load()) {
    leaderBlacklistState.initial_load_ = leader_blacklist.initial_leader_load();
  }

  LOG(INFO) << "Set leader blacklist size = " << leaderBlacklistState.tservers_.size()
            << " with load " << leaderBlacklistState.initial_load_;

  return Status::OK();
}

Status CatalogManager::SetClusterConfig(
    const ChangeMasterClusterConfigRequestPB* req, ChangeMasterClusterConfigResponsePB* resp) {
  SysClusterConfigEntryPB config(req->cluster_config());

  {
    std::lock_guard <LockType> blacklist_lock(blacklist_lock_);

    // Save the list of blacklisted servers to be used for completion checking.
    if (config.has_server_blacklist()) {
      RETURN_NOT_OK(SetBlackList(config.server_blacklist()));

      config.mutable_server_blacklist()->set_initial_replica_load(
          GetNumRelevantReplicas(blacklistState, false /* leaders_only */));
      blacklistState.initial_load_ = config.server_blacklist().initial_replica_load();
    }

    // Save the list of leader blacklist to be used for completion checking.
    if (config.has_leader_blacklist()) {
      RETURN_NOT_OK(SetLeaderBlacklist(config.leader_blacklist()));

      config.mutable_leader_blacklist()->set_initial_leader_load(
          GetNumRelevantReplicas(leaderBlacklistState, true /* leaders_only */));
      leaderBlacklistState.initial_load_ = config.leader_blacklist().initial_leader_load();
    }
  }

  auto l = cluster_config_->LockForWrite();
  // We should only set the config, if the caller provided us with a valid update to the
  // existing config.
  if (l->data().pb.version() != config.version()) {
    Status s = STATUS_SUBSTITUTE(IllegalState,
      "Config version does not match, got $0, but most recent one is $1. Should call Get again",
      config.version(), l->data().pb.version());
    return SetupError(resp->mutable_error(), MasterErrorPB::CONFIG_VERSION_MISMATCH, s);
  }

  if (config.cluster_uuid() != l->data().pb.cluster_uuid()) {
    Status s = STATUS(InvalidArgument, "Config cluster UUID cannot be updated");
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_CLUSTER_CONFIG, s);
  }

  // TODO(bogdan): should this live here?
  const ReplicationInfoPB& replication_info = config.replication_info();
  for (int i = 0; i < replication_info.read_replicas_size(); i++) {
    if (!replication_info.read_replicas(i).has_placement_uuid()) {
      Status s = STATUS(IllegalState,
                        "All read-only clusters must have a placement uuid specified");
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_CLUSTER_CONFIG, s);
    }
  }

  l->mutable_data()->pb.CopyFrom(config);
  // Bump the config version, to indicate an update.
  l->mutable_data()->pb.set_version(config.version() + 1);

  LOG(INFO) << "Updating cluster config to " << config.version() + 1;

  RETURN_NOT_OK(sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term()));

  l->Commit();

  return Status::OK();
}

Status CatalogManager::SetPreferredZones(
    const SetPreferredZonesRequestPB* req, SetPreferredZonesResponsePB* resp) {
  auto l = cluster_config_->LockForWrite();
  auto replication_info = l->mutable_data()->pb.mutable_replication_info();
  replication_info->clear_affinitized_leaders();

  Status s;
  for (const auto& cloud_info : req->preferred_zones()) {
    s = CatalogManagerUtil::DoesPlacementInfoContainCloudInfo(replication_info->live_replicas(),
                                                              cloud_info);
    if (!s.ok()) {
      return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_CLUSTER_CONFIG, s);
    }
    *replication_info->add_affinitized_leaders() = cloud_info;
  }

  l->mutable_data()->pb.set_version(l->mutable_data()->pb.version() + 1);

  LOG(INFO) << "Updating cluster config to " << l->mutable_data()->pb.version();

  s = sys_catalog_->UpdateItem(cluster_config_.get(), leader_ready_term());
  if (!s.ok()) {
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_CLUSTER_CONFIG, s);
  }

  l->Commit();

  return Status::OK();
}

Status CatalogManager::GetReplicationFactor(int* num_replicas) {
  DCHECK(cluster_config_) << "Missing cluster config for master!";
  auto l = cluster_config_->LockForRead();
  const ReplicationInfoPB& replication_info = l->data().pb.replication_info();
  *num_replicas = GetNumReplicasFromPlacementInfo(replication_info.live_replicas());
  return Status::OK();
}

Status CatalogManager::GetReplicationFactorForTablet(const scoped_refptr<TabletInfo>& tablet,
    int* num_replicas) {
  // For system tables, the set of replicas is always the set of masters.
  if (system_tablets_.find(tablet->id()) != system_tablets_.end()) {
    consensus::ConsensusStatePB master_consensus;
    RETURN_NOT_OK(GetCurrentConfig(&master_consensus));
    *num_replicas = master_consensus.config().peers().size();
    return Status::OK();
  }
  return GetReplicationFactor(num_replicas);
}

void CatalogManager::GetExpectedNumberOfReplicas(int* num_live_replicas, int* num_read_replicas) {
  auto l = cluster_config_->LockForRead();
  const ReplicationInfoPB& replication_info = l->data().pb.replication_info();
  *num_live_replicas = GetNumReplicasFromPlacementInfo(replication_info.live_replicas());
  for (const auto& read_replica_placement_info : replication_info.read_replicas()) {
    *num_read_replicas = read_replica_placement_info.num_replicas();
  }
}

string CatalogManager::placement_uuid() const {
  DCHECK(cluster_config_) << "Missing cluster config for master!";
  auto l = cluster_config_->LockForRead();
  const ReplicationInfoPB& replication_info = l->data().pb.replication_info();
  return replication_info.live_replicas().placement_uuid();
}

Status CatalogManager::IsLoadBalanced(const IsLoadBalancedRequestPB* req,
                                      IsLoadBalancedResponsePB* resp) {
  TSDescriptorVector ts_descs;
  master_->ts_manager()->GetAllLiveDescriptors(&ts_descs);

  if (req->has_expected_num_servers() && req->expected_num_servers() > ts_descs.size()) {
    Status s = STATUS_SUBSTITUTE(IllegalState,
        "Found $0, which is below the expected number of servers $1.",
        ts_descs.size(), req->expected_num_servers());
    return SetupError(resp->mutable_error(), MasterErrorPB::CAN_RETRY_LOAD_BALANCE_CHECK, s);
  }

  Status s = load_balance_policy_->IsIdle();
  if (!s.ok()) {
    return SetupError(resp->mutable_error(), MasterErrorPB::CAN_RETRY_LOAD_BALANCE_CHECK, s);
  }

  return Status::OK();
}

Status CatalogManager::IsLoadBalancerIdle(const IsLoadBalancerIdleRequestPB* req,
                                          IsLoadBalancerIdleResponsePB* resp) {
  Status s = load_balance_policy_->IsIdle();
  if (!s.ok()) {
    return SetupError(resp->mutable_error(), MasterErrorPB::LOAD_BALANCER_RECENTLY_ACTIVE, s);
  }

  return Status::OK();
}

Status CatalogManager::AreLeadersOnPreferredOnly(const AreLeadersOnPreferredOnlyRequestPB* req,
                                                 AreLeadersOnPreferredOnlyResponsePB* resp) {
  // If we have cluster replication info, then only fetch live tservers (ignore read replicas).
  TSDescriptorVector ts_descs;
  string live_replicas_placement_uuid = "";
  {
    auto l = cluster_config_->LockForRead();
    const ReplicationInfoPB& cluster_replication_info = l->data().pb.replication_info();
    if (cluster_replication_info.has_live_replicas()) {
      live_replicas_placement_uuid = cluster_replication_info.live_replicas().placement_uuid();
    }
  }

  {
    SharedLock<LockType> l(blacklist_lock_);
    if (live_replicas_placement_uuid.empty()) {
      master_->ts_manager()->GetAllLiveDescriptors(&ts_descs, blacklistState.tservers_);
    } else {
      master_->ts_manager()->GetAllLiveDescriptorsInCluster(
          &ts_descs, live_replicas_placement_uuid, blacklistState.tservers_);
    }
  }

  SysClusterConfigEntryPB config;
  RETURN_NOT_OK(GetClusterConfig(&config));

  // Only need to fetch if txn tables are not using preferred zones.
  vector<scoped_refptr<TableInfo>> tables;
  if (!FLAGS_transaction_tables_use_preferred_zones) {
    master_->catalog_manager()->GetAllTables(&tables, true /* include only running tables */);
  }

  Status s = CatalogManagerUtil::AreLeadersOnPreferredOnly(ts_descs,
                                                           config.replication_info(),
                                                           tables);
  if (!s.ok()) {
    return SetupError(
        resp->mutable_error(), MasterErrorPB::CAN_RETRY_ARE_LEADERS_ON_PREFERRED_ONLY_CHECK, s);
  }

  return Status::OK();
}

void BlacklistState::Reset() {
  tservers_.clear();
  initial_load_ = 0;
}

std::string BlacklistState::ToString() {
  return Substitute("Blacklist has $0 servers, initial load is $1.",
                    tservers_.size(), initial_load_);
}

int64_t CatalogManager::GetNumRelevantReplicas(const BlacklistState& state, bool leaders_only) {
  int64_t res = 0;
  std::lock_guard <LockType> tablet_map_lock(lock_);
  for (const TabletInfoMap::value_type& entry : *tablet_map_) {
    scoped_refptr<TabletInfo> tablet = entry.second;
    auto l = tablet->LockForRead();
    // Not checking being created on purpose as we do not want initial load to be under accounted.
    if (!tablet->table() ||
        PREDICT_FALSE(l->data().is_deleted())) {
      continue;
    }

    auto locs = tablet->GetReplicaLocations();
    for (const TabletInfo::ReplicaMap::value_type& replica : *locs) {
      if (leaders_only && replica.second.role != RaftPeerPB::LEADER) {
        continue;
      }
      TSRegistrationPB reg = replica.second.ts_desc->GetRegistration();
      bool found = false;
      for (const auto& hp : reg.common().private_rpc_addresses()) {
        if (state.tservers_.count(HostPortFromPB(hp)) != 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        for (const auto& hp : reg.common().broadcast_addresses()) {
          if (state.tservers_.count(HostPortFromPB(hp)) != 0) {
            found = true;
            break;
          }
        }
      }
      if (found) {
        res++;
      }
    }
  }

  return res;
}

Status CatalogManager::FillHeartbeatResponse(const TSHeartbeatRequestPB* req,
                                             TSHeartbeatResponsePB* resp) {
  return Status::OK();
}

Status CatalogManager::GetLoadMoveCompletionPercent(GetLoadMovePercentResponsePB* resp) {
  return GetLoadMoveCompletionPercent(resp, false);
}

Status CatalogManager::GetLeaderBlacklistCompletionPercent(GetLoadMovePercentResponsePB* resp) {
  return GetLoadMoveCompletionPercent(resp, true);
}

Status CatalogManager::GetLoadMoveCompletionPercent(GetLoadMovePercentResponsePB* resp,
    bool blacklist_leader) {
  SharedLock<LockType> l(blacklist_lock_);

  BlacklistState& state = (blacklist_leader) ? leaderBlacklistState : blacklistState;
  int64_t blacklist_replicas = GetNumRelevantReplicas(state, blacklist_leader);

  // On change of master leader, initial_load_ information may be lost temporarily. Reset to
  // current value to avoid reporting progress percent as 100. Note that doing so will report
  // progress percent as 0 instead.
  if (state.initial_load_ < blacklist_replicas) {
    LOG(INFO) << "Reset blacklist initial load from " << state.initial_load_
      <<  " to " << blacklist_replicas;
    state.initial_load_ = blacklist_replicas;
  }

  LOG(INFO) << "Blacklisted count " << blacklist_replicas
            << " across " << state.tservers_.size()
            << " servers, with initial load " << state.initial_load_;

  // Case when a blacklisted servers did not have any starting load.
  if (state.initial_load_ == 0) {
    resp->set_percent(100);
    return Status::OK();
  }

  resp->set_percent(
      100 - (static_cast<double>(blacklist_replicas) * 100 / state.initial_load_));
  resp->set_remaining(blacklist_replicas);
  resp->set_total(state.initial_load_);

  return Status::OK();
}

void CatalogManager::AbortAndWaitForAllTasks(const vector<scoped_refptr<TableInfo>>& tables) {
  for (const auto& t : tables) {
    VLOG(1) << "Aborting tasks for table " << t->ToString();
    t->AbortTasksAndClose();
  }
  for (const auto& t : tables) {
    VLOG(1) << "Waiting on Aborting tasks for table " << t->ToString();
    t->WaitTasksCompletion();
  }
  VLOG(1) << "Waiting on Aborting tasks done";
}

void CatalogManager::HandleNewTableId(const TableId& table_id) {
  if (table_id == kPgProcTableId) {
    // Needed to track whether initdb has started running.
    pg_proc_exists_.store(true, std::memory_order_release);
  }
}

scoped_refptr<TableInfo> CatalogManager::NewTableInfo(TableId id) {
  return make_scoped_refptr<TableInfo>(id, tasks_tracker_);
}

Status CatalogManager::ScheduleTask(std::shared_ptr<RetryingTSRpcTask> task) {
  Status s = async_task_pool_->SubmitFunc([task]() {
      WARN_NOT_OK(task->Run(), "Failed task");
  });
  // If we are not able to enqueue, abort the task.
  if (!s.ok()) {
    task->AbortAndReturnPrevState(s);
  }
  return s;
}

Result<vector<TableDescription>> CatalogManager::CollectTables(
    const google::protobuf::RepeatedPtrField<TableIdentifierPB>& tables,
    bool add_indexes,
    bool include_parent_colocated_table) {
  vector<TableDescription> all_tables;
  unordered_set<NamespaceId> parent_colocated_table_ids;

  for (const auto& table_id_pb : tables) {
    TableDescription table_description = VERIFY_RESULT(DescribeTable(table_id_pb));
    if (include_parent_colocated_table && table_description.table_info->colocated()) {
      // If a table is colocated, add its parent colocated table as well.
      const auto parent_table_id =
          table_description.namespace_info->id() + kColocatedParentTableIdSuffix;
      auto result = parent_colocated_table_ids.insert(parent_table_id);
      if (result.second) {
        // We have not processed this parent table id yet, so do that now.
        TableIdentifierPB parent_table_pb;
        parent_table_pb.set_table_id(parent_table_id);
        parent_table_pb.mutable_namespace_()->set_id(table_description.namespace_info->id());
        TableDescription parent_table_description = VERIFY_RESULT(DescribeTable(parent_table_pb));
        all_tables.push_back(parent_table_description);
      }
    }
    all_tables.push_back(table_description);

    if (add_indexes) {
      TRACE(Substitute("Locking object with id $0", table_description.table_info->id()));
      auto l = table_description.table_info->LockForRead();

      if (table_description.table_info->is_index()) {
        return STATUS(InvalidArgument, "Expected table, but found index",
                      table_description.table_info->id(),
                      MasterError(MasterErrorPB::INVALID_TABLE_TYPE));
      }

      if (l->data().table_type() == PGSQL_TABLE_TYPE) {
        return STATUS(InvalidArgument, "Getting indexes for YSQL table is not supported",
                      table_description.table_info->id(),
                      MasterError(MasterErrorPB::INVALID_TABLE_TYPE));
      }

      for (const auto& index_info : l->data().pb.indexes()) {
        LOG_IF(DFATAL, table_description.table_info->id() != index_info.indexed_table_id())
                << "Wrong indexed table id in index descriptor";
        TableIdentifierPB index_id_pb;
        index_id_pb.set_table_id(index_info.table_id());
        index_id_pb.mutable_namespace_()->set_id(table_description.namespace_info->id());
        all_tables.push_back(VERIFY_RESULT(DescribeTable(index_id_pb)));
      }
    }
  }

  return all_tables;
}

Status CatalogManager::GetYQLPartitionsVTable(std::shared_ptr<SystemTablet>* tablet) {
  scoped_refptr<TableInfo> table = FindPtrOrNull(table_names_map_,
      std::make_pair(kSystemNamespaceId, kSystemPartitionsTableName));
  SCHECK(table != nullptr, NotFound, "YQL system.partitions table not found");

  vector<scoped_refptr<TabletInfo>> tablets;
  table->GetAllTablets(&tablets);
  SCHECK(tablets.size() == 1, NotFound, "YQL system.partitions tablet not found");
  *tablet = std::dynamic_pointer_cast<SystemTablet>(
      VERIFY_RESULT(GetSystemTablet(tablets[0]->tablet_id())));
  return Status::OK();
}

void CatalogManager::RebuildYQLSystemPartitions() {
  if (FLAGS_partitions_vtable_cache_refresh_secs > 0) {
    ScopedLeaderSharedLock l(this);
    if (l.catalog_status().ok() && l.leader_status().ok()) {
      if (system_partitions_tablet_ != nullptr) {
        auto s = down_cast<const YQLPartitionsVTable&>(
            system_partitions_tablet_->QLStorage()).GenerateAndCacheData();
        if (!s.ok()) {
          LOG(ERROR) << "Error rebuilding system.partitions: " << s.ToString();
        }
      } else {
        LOG(ERROR) << "Error finding system.partitions vtable.";
      }
    }
  }

  auto wait_time = FLAGS_partitions_vtable_cache_refresh_secs * 1s;
  if (wait_time <= 0s) {
    wait_time = kDefaultYQLPartitionsRefreshBgTaskSleep;
  }
  refresh_yql_partitions_task_.Schedule([this](const Status& status) {
    WARN_NOT_OK(
        background_tasks_thread_pool_->SubmitFunc(
            std::bind(&CatalogManager::RebuildYQLSystemPartitions, this)),
        "Failed to schedule: RebuildYQLSystemPartitions");
  }, wait_time);
}

}  // namespace master
}  // namespace yb
