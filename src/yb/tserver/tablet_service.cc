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
// The following only applies to changes made to this file as part of YugabyteDB development.
//
// Portions Copyright (c) YugabyteDB, Inc.
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

#include "yb/tserver/tablet_service.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <boost/algorithm/string/replace.hpp>

#include "yb/ash/wait_state.h"

#include "yb/client/transaction.h"
#include "yb/client/transaction_manager.h"
#include "yb/client/transaction_pool.h"

#include "yb/common/colocated_util.h"
#include "yb/common/pg_types.h"
#include "yb/common/pgsql_error.h"
#include "yb/common/ql_value.h"
#include "yb/common/row_mark.h"
#include "yb/common/schema.h"
#include "yb/common/schema_pbutil.h"
#include "yb/common/transaction_error.h"
#include "yb/common/wire_protocol.h"

#include "yb/consensus/consensus_types.pb.h"
#include "yb/consensus/leader_lease.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus_util.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/docdb/cql_operation.h"
#include "yb/docdb/doc_vector_index.h"
#include "yb/docdb/pgsql_operation.h"

#include "yb/dockv/reader_projection.h"

#include "yb/gutil/bind.h"
#include "yb/gutil/casts.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/escaping.h"

#include "yb/qlexpr/index.h"
#include "yb/qlexpr/ql_rowblock.h"

#include "yb/rpc/rpc_context.h"
#include "yb/rpc/sidecars.h"
#include "yb/rpc/thread_pool.h"

#include "yb/server/hybrid_clock.h"

#include "yb/tablet/abstract_tablet.h"
#include "yb/tablet/metadata.pb.h"
#include "yb/tablet/operations.pb.h"
#include "yb/tablet/operations/change_metadata_operation.h"
#include "yb/tablet/operations/clone_operation.h"
#include "yb/tablet/operations/split_operation.h"
#include "yb/tablet/operations/truncate_operation.h"
#include "yb/tablet/operations/update_txn_operation.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/read_result.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_dump_helper.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_vector_indexes.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/transaction_participant.h"
#include "yb/tablet/write_query.h"

#include "yb/tserver/heartbeater.h"
#include "yb/tserver/pg_txn_snapshot_manager.h"
#include "yb/tserver/read_query.h"
#include "yb/tserver/service_util.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_local_lock_manager.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver_error.h"
#include "yb/tserver/tserver_fwd.h"
#include "yb/tserver/tserver_types.pb.h"
#include "yb/tserver/tserver_xcluster_context_if.h"
#include "yb/tserver/xcluster_safe_time_map.h"
#include "yb/tserver/ysql_advisory_lock_table.h"
#include "yb/tserver/ysql_call_home_stats.h"
#include "yb/tserver/ysql_lease.h"

#include "yb/util/async_util.h"
#include "yb/util/backoff_waiter.h"
#include "yb/util/callsite_profiling.h"
#include "yb/util/crc.h"
#include "yb/util/debug-util.h"
#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/enums.h"
#include "yb/util/faststring.h"
#include "yb/util/file_util.h"
#include "yb/util/flags.h"
#include "yb/util/format.h"
#include "yb/util/logging.h"
#include "yb/util/math_util.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/pb_util.h"
#include "yb/util/pg_util.h"
#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"
#include "yb/util/status.h"
#include "yb/util/status_callback.h"
#include "yb/util/status_format.h"
#include "yb/util/status_fwd.h"
#include "yb/util/status_log.h"
#include "yb/util/std_util.h"
#include "yb/util/string_util.h"
#include "yb/util/sync_point.h"
#include "yb/util/trace.h"
#include "yb/util/uuid.h"
#include "yb/util/write_buffer.h"
#include "yb/util/yb_pg_errcodes.h"

#include "yb/yql/pggate/pg_global_view_read.h"
#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"
#include "yb/yql/pgwrapper/ysql_binary_runner.h"
#include "yb/yql/pgwrapper/ysql_upgrade.h"

using namespace std::literals;  // NOLINT

DEPRECATE_FLAG(int32, scanner_default_batch_size_bytes, "10_2022");

DEPRECATE_FLAG(int32, scanner_max_batch_size_bytes, "10_2022");

DEPRECATE_FLAG(int32, scanner_batch_size_rows, "10_2022");

DEPRECATE_FLAG(int32, max_wait_for_safe_time_ms, "02_2024");

DEFINE_RUNTIME_int32(num_concurrent_backfills_allowed, -1,
    "Maximum number of concurrent backfill jobs that is allowed to run.");

DEFINE_RUNTIME_bool(yb_fail_catalog_write_on_catalog_version_mismatch, false,
    "If true, a write to the system catalog (e.g. as part of a DDL) is failed with a catalog "
    "version mismatch error when the issuing PG backend's catalog version does not match the "
    "latest catalog version on the master. This guards against catalog corruption caused by DDLs "
    "issued on related objects within heartbeat delay which leads to execution on possibly stale "
    "catalog cache. Disabled by default: enabling it can cause "
    "an unrelated, otherwise-legitimate DDL to fail whenever a concurrent auto-ANALYZE (or any "
    "other catalog write) bumps the catalog version, even on a completely different table. "
    "If enable_object_locking_for_table_locks is true, then this safety check is not required and "
    "has no effect even if enabled.");

DEFINE_test_flag(bool, tserver_noop_read_write, false, "Respond NOOP to read/write.");

DEFINE_RUNTIME_uint64(index_backfill_upperbound_for_user_enforced_txn_duration_ms, 65000,
    "For Non-Txn tables, it is impossible to know at the tservers "
    "whether or not an 'old transaction' is still active. To avoid "
    "having such old transactions, we assume a bound on the duration "
    "of such transactions (during the backfill process) and wait "
    "it out. This flag denotes a conservative upper bound on the "
    "duration of such user enforced transactions.");
TAG_FLAG(index_backfill_upperbound_for_user_enforced_txn_duration_ms, evolving);

DEFINE_RUNTIME_int32(index_backfill_additional_delay_before_backfilling_ms, 0,
    "Operations that are received by the tserver, and have decided how "
    "the indexes need to be updated (based on the IndexPermission), will "
    "not be added to the list of current transactions until they are "
    "replicated/applied. This delay allows for the GetSafeTime method "
    "to wait for such operations to be replicated/applied. Ideally, this "
    "value should be set to be something larger than the raft-heartbeat-interval "
    "but can be as high as the client_rpc_timeout if we want to be more conservative.");
TAG_FLAG(index_backfill_additional_delay_before_backfilling_ms, evolving);

DEFINE_test_flag(int32, index_backfill_fail_after_random_wait_upto_ms, 0,
    "If set to > 0 BackfillIndex calls will be failed after randomly waiting.");

DEFINE_test_flag(bool, block_backfill_before_index_map, false,
    "If true, BackfillIndex will block right before looking up the index_map. "
    "Used to test the race between table drop and backfill to repro GH#29830.");

DEFINE_RUNTIME_int32(index_backfill_wait_for_old_txns_ms, 0,
    "Index backfill needs to wait for transactions that started before the "
    "WRITE_AND_DELETE phase to commit or abort before choosing a time for "
    "backfilling the index. This is the max time that the GetSafeTime call will "
    "wait for, before it resorts to attempt aborting old transactions. This is "
    "necessary to guard against the pathological active transaction that never "
    "commits from blocking the index backfill forever.");
TAG_FLAG(index_backfill_wait_for_old_txns_ms, evolving);

DEFINE_test_flag(double, respond_write_failed_probability, 0.0,
    "Probability to respond that write request is failed");

DEFINE_test_flag(double, respond_write_with_abort_probability, 0.0,
    "Probability to respond that write request is aborted");

DEFINE_test_flag(bool, rpc_delete_tablet_fail, false, "Should delete tablet RPC fail.");

// Lets one tserver stand in for a build that hashes differently, so the mixed-version case is
// reachable from a test rather than by running two builds.
DEFINE_test_flag(int32, dump_tablet_data_hash_scheme_version, -1,
    "Overrides the hash scheme version DumpTabletData reports. -1 reports the real one, 0 leaves "
    "the field unset as a tserver from before it existed does, and a positive value reports that "
    "version.");

DECLARE_bool(disable_alter_vs_write_mutual_exclusion);
DECLARE_uint64(max_clock_skew_usec);
DECLARE_uint64(transaction_min_running_check_interval_ms);
DECLARE_int64(transaction_rpc_timeout_ms);

DEFINE_test_flag(int32, txn_status_table_tablet_creation_delay_ms, 0,
    "Extra delay to slowdown creation of transaction status table tablet.");

DEFINE_test_flag(int32, leader_stepdown_delay_ms, 0,
    "Amount of time to delay before starting a leader stepdown change.");

DEFINE_test_flag(int32, alter_schema_delay_ms, 0, "Delay before processing AlterSchema.");

DEFINE_test_flag(bool, disable_post_split_tablet_rbs_check, false,
    "If true, bypass any checks made to reject remote boostrap requests for post "
    "split tablets whose parent tablets are still present.");

DEFINE_test_flag(double, fail_tablet_split_probability, 0.0,
    "Probability of failing in TabletServiceAdminImpl::SplitTablet.");

DEFINE_test_flag(bool, pause_tserver_get_split_key, false,
    "Pause before processing a GetSplitKey request.");

DEFINE_test_flag(bool, pause_wait_for_ysql_backends_catalog_version, false,
    "Pause any WaitForYsqlBackendsCatalogVersion requests until flags is reset.");

DEFINE_test_flag(bool, pause_wait_for_lockers, false,
    "Pause any WaitForLockers requests until flags is reset.");

DEFINE_test_flag(bool, fail_wait_for_lockers, false,
    "Fail any WaitForLockers requests received by this tserver.");

DECLARE_int32(heartbeat_interval_ms);
DECLARE_uint64(rocksdb_max_file_size_for_compaction);
DECLARE_uint64(rpc_max_message_size);

DEFINE_UNKNOWN_bool(enable_ysql, true,
    "Enable YSQL on the cluster. This will cause yb-master to initialize sys catalog "
    "tablet from an initial snapshot (in case --initial_sys_catalog_snapshot_path is "
    "specified or can be auto-detected). Also each tablet server will start a PostgreSQL "
    "server as a child process.");

DEFINE_RUNTIME_bool(ysql_allow_duplicating_repeatable_read_queries, true,
    "Response with success when duplicate write request is detected, "
    "if case this request contains read time.");

DECLARE_int32(ysql_transaction_abort_timeout_ms);
DECLARE_bool(ysql_yb_disable_wait_for_backends_catalog_version);

DEFINE_test_flag(string, mini_cluster_pg_host_port, "",
    "The PG host:port used in PostgresMiniclusterTest");

DEFINE_test_flag(bool, fail_alter_schema_after_abort_transactions, false,
    "If true, setup an error status in AlterSchema and respond success to rpc call. "
    "This failure should not cause the TServer to crash but "
    "instead return an error message on the YSQL connection.");

DEFINE_test_flag(bool, txn_status_moved_rpc_force_fail, false,
    "Force updates in transaction status location to fail.");

DEFINE_test_flag(bool, txn_status_moved_rpc_force_fail_retryable, true,
    "Forced updates in transaction status location failure should be retryable "
    "error.");

DEFINE_test_flag(int32, txn_status_moved_rpc_handle_delay_ms, 0,
    "Inject delay to slowdown handling of updates in transaction status location.");

DEFINE_test_flag(bool, block_apply_intent, false,
    "When set, block handling of UpdateTransaction(APPLYING) until the flag is cleared.");

DEFINE_test_flag(bool, skip_aborting_active_transactions_during_schema_change, false,
    "Skip aborting active transactions during schema change");

DEFINE_test_flag(bool, skip_force_superblock_flush, false,
    "Used in tests to skip superblock flush on tablet flush.");

DEFINE_test_flag(uint64, wait_row_mark_exclusive_count, 0,
    "Number of row mark exclusive reads to wait for.");

DEFINE_test_flag(int32, set_tablet_follower_lag_ms, 0,
    "What to report for tablet follower lag on this tserver in CheckTserverTabletHealth.");

DEFINE_test_flag(bool, pause_before_tablet_health_response, false,
    "Whether to pause before responding to CheckTserverTabletHealth.");

DECLARE_bool(ysql_yb_enable_alter_table_rewrite);

DEFINE_test_flag(bool, cdc_sdk_fail_setting_retention_barrier, false,
    "Fail setting retention barrier on newly created tablets");

DEFINE_test_flag(uint32, clone_pg_schema_delay_ms, 0,
    "Delay before processing PgCloneSchema request.");

DEFINE_test_flag(bool, fail_clear_metacache, false,
    "Answer the master's ClearMetacache request with an error.");

DEFINE_test_flag(bool, fail_enable_db_conns, false,
    "Answer the master's EnableDbConns request with an error.");

DEFINE_test_flag(uint32, pause_tablet_compact_flush_ms, 0,
    "Used in tests to pause FlushTablet RPC for the specified number of milliseconds");

DEFINE_test_flag(uint32, pause_remote_pg_query_execution_ms, 0,
    "Used in tests to sleep before executing a remote PG query.");

DEFINE_test_flag(uint64, persistence_reject_stamped_before_ht, 0,
    "When nonzero, reject writes and transaction commits whose origination stamp has an "
    "origination hybrid time below this value. Unstamped ones pass.");

DEFINE_test_flag(bool, persistence_dfatal_unstamped_pgsql_write, false,
    "DFATAL on a YSQL write without an origination stamp to a table outside template1.");

DECLARE_bool(enable_object_locking_for_table_locks);
DECLARE_bool(enable_tiered_storage);
DECLARE_bool(ysql_enable_object_locking_infra);
DECLARE_bool(enable_active_table_metrics_filtering);
DECLARE_uint32(max_active_table_metrics_table_count);

METRIC_DEFINE_gauge_uint64(server, ts_split_op_added, "Split OPs Added to Leader",
    yb::MetricUnit::kOperations, "Number of split operations added to the leader's Raft log.");

DEFINE_RUNTIME_bool(ysql_debug_log_write_requests, false,
    "Print all YSQL write requests received by this process to the log."
    "Note: Enabling this flag might log sensitive information.");
TAG_FLAG(ysql_debug_log_write_requests, advanced);
TAG_FLAG(ysql_debug_log_write_requests, unsafe);

DEFINE_test_flag(bool, cdc_fail_before_setting_barrier, false,
  "Fail CreateTablet before setting CDC retention barriers");

double TEST_delay_create_transaction_probability = 0;

namespace yb::tserver {

YB_STRONGLY_TYPED_HEX_UUID_IMPL(PgTxnSnapshotLocalId);

using consensus::ChangeConfigRequestPB;
using consensus::ChangeConfigResponsePB;
using consensus::Consensus;
using consensus::CONSENSUS_CONFIG_ACTIVE;
using consensus::CONSENSUS_CONFIG_COMMITTED;
using consensus::ConsensusConfigType;
using consensus::ConsensusRequestPB;
using consensus::GetLastOpIdRequestPB;
using consensus::GetNodeInstanceRequestPB;
using consensus::GetNodeInstanceResponsePB;
using consensus::LeaderLeaseStatus;
using consensus::LeaderStepDownRequestPB;
using consensus::LeaderStepDownResponsePB;
using consensus::RunLeaderElectionRequestPB;
using consensus::RunLeaderElectionResponsePB;
using consensus::StartRemoteBootstrapRequestPB;
using consensus::StartRemoteBootstrapResponsePB;
using consensus::UnsafeChangeConfigRequestPB;
using consensus::UnsafeChangeConfigResponsePB;
using consensus::VoteRequestPB;
using consensus::VoteResponsePB;

using google::protobuf::RepeatedPtrField;
using rpc::RpcContext;
using std::shared_ptr;
using std::string;
using std::vector;
using tablet::ChangeMetadataOperation;
using tablet::CloneTabletRequestPB;
using tablet::OnlyAbortTxnsNotUsingTableLocks;
using tablet::OperationCompletionCallback;
using tablet::Tablet;
using tablet::TabletPeer;
using tablet::TabletPeerPtr;
using tablet::TabletStatusPB;
using tablet::TruncateOperation;

namespace {

template <class OriginationInfo>
Status CheckPersistenceTestRejection(const OriginationInfo& origination_info) {
  const auto threshold = FLAGS_TEST_persistence_reject_stamped_before_ht;
  if (PREDICT_TRUE(threshold == 0) || origination_info.origination_ht() >= threshold) {
    return Status::OK();
  }
  return STATUS_FORMAT(
      Expired,
      "TEST: persistence rejected op with connected database $0 with origination time $1 < $2",
      origination_info.database_oid(), HybridTime(origination_info.origination_ht()),
      HybridTime(threshold));
}

void CheckPersistenceStampPresent(const WriteRequestMsg& req) {
  if (PREDICT_TRUE(!FLAGS_TEST_persistence_dfatal_unstamped_pgsql_write) ||
      req.has_origination_info()) {
    return;
  }
  for (const auto& pg_req : req.pgsql_write_batch()) {
    auto db_oid = GetPgsqlDatabaseOidByTableId(pg_req.table_id().ToBuffer());
    // Template1 backends write unstamped, and can only write tables of database 1.
    if (!db_oid.ok() || *db_oid != kTemplate1Oid) {
      LOG(DFATAL) << "YSQL write without origination stamp to table " << pg_req.table_id()
                  << " of database " << db_oid << ": " << req.ShortDebugString();
      return;
    }
  }
}

Result<std::shared_ptr<consensus::RaftConsensus>> GetConsensus(const TabletPeerPtr& tablet_peer) {
  auto result = tablet_peer->GetRaftConsensus();
  if (!result) {
    return result.status().CloneAndAddErrorCode(
        TabletServerError(TabletServerErrorPB::TABLET_NOT_RUNNING));
  }
  return *result;
}

template<class RespClass>
std::shared_ptr<consensus::RaftConsensus> GetConsensusOrRespond(const TabletPeerPtr& tablet_peer,
                                                                RespClass* resp,
                                                                rpc::RpcContext* context) {
  auto result = GetConsensus(tablet_peer);
  if (!result.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), result.status(), context);
    return nullptr;
  }
  return result.get();
}

template<class RespClass>
bool GetConsensusOrRespond(const TabletPeerPtr& tablet_peer,
                           RespClass* resp,
                           rpc::RpcContext* context,
                           shared_ptr<Consensus>* consensus) {
  auto result = GetConsensus(tablet_peer);
  if (!result.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), result.status(), context);
    return false;
  }
  return (*consensus = result.get()) != nullptr;
}

template <class Req, class Resp, class Action>
void PerformAtLeader(
    const Req& req, Resp* resp, rpc::RpcContext* context, TabletServerIf* server,
    const char* op_name, const Action& action) {
  TRACE_EVENT1("tserver", op_name, "tablet_id", req->tablet_id());

  UpdateClock(*req, server->Clock());

  auto tablet_peer = LookupLeaderTabletOrRespond(
      server->tablet_peer_lookup(), req->tablet_id(), resp, context);

  if (!tablet_peer) {
    return;
  }

  auto status = action(tablet_peer);

  if (*context) {
    resp->set_propagated_hybrid_time(server->Clock()->Now().ToUint64());
    if (status.ok()) {
      FillTabletConsensusInfoIfRequestOpIdStale(tablet_peer.peer, req, resp);
      context->RespondSuccess();
    } else {
      SetupErrorAndRespond(resp->mutable_error(), status, context);
    }
  }
}

Result<PgTxnSnapshot> GetLocalPgTxnSnapshotImpl(
    TabletServerIf& server, const std::string& snapshot_id_uuid) {
  return server.GetLocalPgTxnSnapshot(VERIFY_RESULT(
      FullyDecodePgTxnSnapshotLocalId(snapshot_id_uuid)));
}

Status PrintYSQLWriteRequest(
    const WriteRequestMsg& req, const RpcContext& context, const Tablet& tablet) {
  if (req.pgsql_write_batch_size() == 0) {
    return Status::OK();
  }
  const auto& metadata = *tablet.metadata();

  std::stringstream ss;
  ss << "YSQL Write Request: " << std::endl << "From: " << context.requestor_string() << std::endl;
  if (req.has_write_batch() && req.write_batch().has_transaction()) {
    ss << "TransactionId: "
       << VERIFY_RESULT(FullyDecodeTransactionId(req.write_batch().transaction().transaction_id()))
       << std::endl;
  }
  for (const auto& entry : req.pgsql_write_batch()) {
    ss << std::endl;

    auto stmt_type = PgsqlWriteRequestPB::PgsqlStmtType_Name(entry.stmt_type());
    boost::replace_all(stmt_type, "PGSQL_", "");
    ss << stmt_type << ": " << std::endl;

    if (entry.force_catalog_modifications()) {
      ss << "\tforce_catalog_modifications: true" << std::endl;
    }
    const auto table_info = VERIFY_RESULT(metadata.GetTableInfo(entry.table_id()));
    ss << "\tTable: " << table_info->table_name << " [" << entry.table_id() << "]" << std::endl;

    ss << "\tKey: ";
    if (entry.has_ybctid_column_value()) {
      ss << "ybctid [" << entry.ybctid_column_value().ShortDebugString() << "]";
    }
    if (entry.has_ybctid_column_value() && !entry.range_column_values().empty()) {
      ss << ", ";
    }
    if (!entry.range_column_values().empty()) {
      ss << "range " << AsString(entry.range_column_values());
    }
    ss << std::endl;
    if (!entry.column_new_values().empty() || !entry.column_values().empty()) {
      ss << "\tColumns: ";
      ss << AsString(entry.column_new_values());
      if (!entry.column_new_values().empty() && !entry.column_values().empty()) {
        ss << ", ";
      }
      ss << AsString(entry.column_values());
      ss << std::endl;
    }
  }
  LOG(INFO) << ss.str();
  return Status::OK();
}

template <typename Req>
std::optional<uint64_t> GetRecipientLeaseEpoch(const Req& req) {
  return req.has_recipient_lease_epoch() ? std::optional(req.recipient_lease_epoch())
                                         : std::nullopt;
}

} // namespace

typedef ListTabletsResponsePB::StatusAndSchemaPB StatusAndSchemaPB;
typedef ListMasterServersResponsePB::MasterServerAndTypePB MasterServerAndTypePB;

class WriteQueryCompletionCallback {
 public:
  WriteQueryCompletionCallback(
      tablet::TabletPeerPtr tablet_peer,
      std::shared_ptr<rpc::RpcContext> context,
      WriteResponseMsg* response,
      tablet::WriteQuery* query,
      const server::ClockPtr& clock,
      bool trace,
      bool leader_term_set_in_request)
      : tablet_peer_(std::move(tablet_peer)),
        context_(std::move(context)),
        response_(response),
        query_(query),
        clock_(clock),
        include_trace_(trace),
        leader_term_set_in_request_(leader_term_set_in_request),
        trace_(include_trace_ ? Trace::CurrentTrace() : nullptr) {}

  void operator()(Status status) const {
    SCOPED_WAIT_STATUS(OnCpu_Active);
    VLOG(1) << __PRETTY_FUNCTION__ << " completing with status " << status;
    // When we don't need to return any data, we could return success on duplicate request.
    if (status.IsAlreadyPresent() && AllowDuplicateRequest()) {
      status = Status::OK();
    }

    TRACE("Write completing with status $0", yb::ToString(status));

    CopyMetricsToPgsqlResponse();

    if (!status.ok()) {
      if (leader_term_set_in_request_ && status.IsAborted() &&
          status.message().Contains("Operation submitted in term")) {
        // Return a permanent error since term is set the request.
        status = STATUS_FORMAT(InvalidArgument, "Leader term changed");
      }

      if (status.IsTryAgain()) {
          LOG(DETAIL) << "Write failed: " << status;
      } else {
          LOG(INFO) << "Write failed: " << status;
      }

      if (include_trace_ && trace_) {
        response_->dup_trace_buffer(trace_->DumpToString(true));
      }
      SetupErrorAndRespond(get_error(), status, context_.get());
      return;
    }

    // Retrieve the rowblocks returned from the QL write operations and return them as RPC
    // sidecars. Populate the row schema also.
    faststring rows_data;
    for (const auto& ql_write_op : *query_->ql_write_ops()) {
      const auto& ql_write_req = ql_write_op->request();
      auto* ql_write_resp = ql_write_op->response();
      const auto* rowblock = ql_write_op->rowblock();
      SchemaToColumnPBs(rowblock->schema(), ql_write_resp->mutable_column_schemas());
      rowblock->Serialize(ql_write_req.client(), &context_->sidecars().Start());
      ql_write_resp->set_rows_data_sidecar(
          narrow_cast<int32_t>(context_->sidecars().Complete()));
    }

    if (include_trace_ && trace_) {
      response_->dup_trace_buffer(trace_->DumpToString(true));
    }
    response_->set_propagated_hybrid_time(clock_->Now().ToUint64());
    FillTabletConsensusInfoIfRequestOpIdStale(tablet_peer_, query_->client_request(), response_);
    context_->RespondSuccess();
    VLOG(1) << __PRETTY_FUNCTION__ << " RespondedSuccess";
  }

 private:
  TabletServerErrorMsg* get_error() const {
    return response_->mutable_error();
  }

  void CopyMetricsToPgsqlResponse() const {
    auto tablet_metrics = query_->scoped_tablet_metrics();
    auto statistics = query_->scoped_statistics();

    auto [resp_ptr, metrics_capture] = query_->GetPgsqlResponseAndMetricsCapture();
    if (resp_ptr) {
      tablet_metrics.CopyToPgsqlResponse(resp_ptr, metrics_capture);
      statistics.CopyToPgsqlResponse(resp_ptr, metrics_capture);
    }
  }

  bool AllowDuplicateRequest() const {
    if (!query_->ql_write_ops()->empty() ||
        !query_->client_request()->redis_write_batch().empty()) {
      return false;
    }
    return query_->pgsql_write_ops()->empty() ||
           (FLAGS_ysql_allow_duplicating_repeatable_read_queries &&
            query_->client_request()->has_read_time());
  }

  tablet::TabletPeerPtr tablet_peer_;
  const std::shared_ptr<rpc::RpcContext> context_;
  WriteResponseMsg* const response_;
  tablet::WriteQuery* const query_;
  server::ClockPtr clock_;
  const bool include_trace_;
  const bool leader_term_set_in_request_;
  scoped_refptr<Trace> trace_;
};

// Checksums the scan result.
class ScanResultChecksummer {
 public:
  ScanResultChecksummer() {}

  void HandleRow(const Schema& schema, const qlexpr::QLTableRow& row) {
    QLValue value;
    buffer_.clear();
    for (uint32_t col_index = 0; col_index != schema.num_columns(); ++col_index) {
      auto status = row.GetValue(schema.column_id(col_index), &value);
      if (!status.ok()) {
        LOG(WARNING) << "Column " << schema.column_id(col_index)
                     << " not found in " << row.ToString();
        continue;
      }
      buffer_.append(pointer_cast<const char*>(&col_index), sizeof(col_index));
      if (schema.column(col_index).is_nullable()) {
        uint8_t defined = value.IsNull() ? 0 : 1;
        buffer_.append(pointer_cast<const char*>(&defined), sizeof(defined));
      }
      if (!value.IsNull()) {
        value.value().AppendToString(&buffer_);
      }
    }
    crc_->Compute(buffer_.c_str(), buffer_.size(), &agg_checksum_, nullptr);
  }

  // Accessors for initializing / setting the checksum.
  uint64_t agg_checksum() const { return agg_checksum_; }

 private:
  crc::Crc* const crc_ = crc::GetCrc32cInstance();
  uint64_t agg_checksum_ = 0;
  std::string buffer_;
};

Result<std::shared_ptr<tablet::AbstractTablet>> TabletServiceImpl::GetTabletForRead(
  TabletIdView tablet_id, tablet::TabletPeerPtr tablet_peer,
  YBConsistencyLevel consistency_level, tserver::AllowSplitTablet allow_split_tablet,
  tserver::ReadResponseMsg* resp) {
  return GetTablet(server_->tablet_peer_lookup(), tablet_id, std::move(tablet_peer),
                   consistency_level, allow_split_tablet, resp);
}

TabletServiceImpl::TabletServiceImpl(TabletServerIf* server)
    : TabletServerServiceIf(server->MetricEnt()),
      server_(server) {
}

TabletServiceAdminImpl::TabletServiceAdminImpl(TabletServer* server)
    : TabletServerAdminServiceIf(DCHECK_NOTNULL(server)->MetricEnt()), server_(server) {
  ts_split_op_added_ = METRIC_ts_split_op_added.Instantiate(server->MetricEnt(), 0);
}

std::string TabletServiceAdminImpl::LogPrefix() const {
  return server::MakeServerLogPrefix(server_->permanent_uuid());
}

void TabletServiceAdminImpl::BackfillDone(
    const tablet::ChangeMetadataRequestPB* req, ChangeMetadataResponsePB* resp,
    rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "BackfillDone", req, resp, &context)) {
    return;
  }
  DVLOG(3) << "Received BackfillDone RPC: " << req->DebugString();

  server::UpdateClock(*req, server_->Clock());

  // For now, we shall only allow this RPC on the leader.
  auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  auto operation = std::make_unique<ChangeMetadataOperation>(
      tablet.tablet, tablet.peer->log());
  operation->AllocateRequest()->CopyFrom(*req);

  operation->set_completion_callback(
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));

  // Submit the alter schema op. The RPC will be responded to asynchronously.
  tablet.peer->Submit(std::move(operation), tablet.leader_term);
}

void TabletServiceAdminImpl::GetSafeTime(
    const GetSafeTimeRequestPB* req, GetSafeTimeResponsePB* resp, rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "GetSafeTime", req, resp, &context)) {
    return;
  }
  DVLOG(3) << "Received GetSafeTime RPC: " << req->DebugString();

  server::UpdateClock(*req, server_->Clock());

  // For now, we shall only allow this RPC on the leader.
  auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }
  const CoarseTimePoint& deadline = context.GetClientDeadline();
  HybridTime min_hybrid_time(HybridTime::kMin);
  if (req->has_min_hybrid_time_for_backfill()) {
    min_hybrid_time = HybridTime(req->min_hybrid_time_for_backfill());

    // For YSQL, it is not possible for transactions to exist that use the old permissions.  This is
    // especially the case after commit a1729c352896e919f462c614770da443b9982c0a introduces
    // ysql_yb_disable_wait_for_backends_catalog_version=false, which explicitly waits on all
    // possible transactions.  Before that commit, the best-effort, unsafe
    // ysql_yb_index_state_flags_update_delay=1000 setting was used to guarantee that.  Even though
    // correctness guarantees are generally flawed when solely relying on
    // ysql_yb_index_state_flags_update_delay=1000, make a best effort to reduce the scope of that
    // flaw by aborting transactions in case ysql_yb_disable_wait_for_backends_catalog_version=true.
    //
    // For YCQL Transactional tables, wait until there are no pending transactions that started
    // prior to min_hybrid_time. These may not have updated the index correctly, if they
    // happen to commit after the backfill scan, it is possible that they may miss updating
    // the index because the some operations may have taken place prior to min_hybrid_time.
    //
    // For YCQL Non-Txn tables, it is impossible to know at the tservers whether or not an "old
    // transaction" is still active. To avoid having such old transactions, we assume a
    // bound on the length of such transactions (during the backfill process) and wait it
    // out.
    if (tablet.tablet->table_type() == TableType::PGSQL_TABLE_TYPE &&
        !FLAGS_ysql_yb_disable_wait_for_backends_catalog_version) {
      // No need to wait-for/abort transactions.
    } else if (!tablet.tablet->transaction_participant()) {
      min_hybrid_time = min_hybrid_time.AddMilliseconds(
          FLAGS_index_backfill_upperbound_for_user_enforced_txn_duration_ms);
      VLOG(2) << "GetSafeTime called on a user enforced transaction tablet "
              << tablet.peer->tablet_id() << " will wait until "
              << min_hybrid_time << " is safe.";
    } else {
      // Add some extra delay to wait for operations being replicated to be
      // applied.
      SleepFor(MonoDelta::FromMilliseconds(
          FLAGS_index_backfill_additional_delay_before_backfilling_ms));

      auto txn_particpant = tablet.tablet->transaction_participant();
      auto wait_until = CoarseMonoClock::Now() + FLAGS_index_backfill_wait_for_old_txns_ms * 1ms;
      HybridTime min_running_ht;
      for (;;) {
        min_running_ht = txn_particpant->MinRunningHybridTime();
        if ((min_running_ht && min_running_ht >= min_hybrid_time) ||
            CoarseMonoClock::Now() > wait_until) {
          break;
        }
        VLOG(2) << "MinRunningHybridTime is " << min_running_ht
                << " need to wait for " << min_hybrid_time;
        SleepFor(MonoDelta::FromMilliseconds(FLAGS_transaction_min_running_check_interval_ms));
      }

      VLOG(2) << "Finally MinRunningHybridTime is " << min_running_ht;
      if (min_running_ht < min_hybrid_time) {
        VLOG(2) << "Aborting Txns that started prior to " << min_hybrid_time;
        OnlyAbortTxnsNotUsingTableLocks only_abort_txns_not_using_table_locks =
            req->has_only_abort_txns_not_using_table_locks() &&
                    req->only_abort_txns_not_using_table_locks()
                ? OnlyAbortTxnsNotUsingTableLocks::kTrue
                : OnlyAbortTxnsNotUsingTableLocks::kFalse;
        auto s = txn_particpant->StopActiveTxnsPriorTo(
            only_abort_txns_not_using_table_locks, min_hybrid_time, deadline);
        if (!s.ok()) {
          SetupErrorAndRespond(resp->mutable_error(), s, &context);
          return;
        }
      }
    }
  }

  auto safe_time = tablet.tablet->SafeTime(
      tablet::RequireLease::kTrue, min_hybrid_time, deadline);
  if (!safe_time.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), safe_time.status(), &context);
    return;
  }

  resp->set_safe_time(safe_time->ToUint64());
  resp->set_propagated_hybrid_time(server_->Clock()->Now().ToUint64());
  VLOG(1) << "Tablet " << tablet.peer->tablet_id()
          << " returning safe time " << yb::ToString(safe_time);

  context.RespondSuccess();
}

void TabletServiceAdminImpl::BackfillIndex(
    const BackfillIndexRequestPB* req, BackfillIndexResponsePB* resp, rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "BackfillIndex", req, resp, &context)) {
    return;
  }
  DVLOG(3) << "Received BackfillIndex RPC: " << req->DebugString();

  server::UpdateClock(*req, server_->Clock());

  // For now, we shall only allow this RPC on the leader.
  auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  if (req->indexes().empty()) {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS(InvalidArgument, "No indexes given in request"),
        TabletServerErrorPB::OPERATION_NOT_SUPPORTED,
        &context);
    return;
  }

  const CoarseTimePoint &deadline = context.GetClientDeadline();
  const auto coarse_start = CoarseMonoClock::Now();
  {
    SCOPED_WAIT_STATUS(BackfillIndex_WaitForAFreeSlot);
    std::unique_lock<std::mutex> l(backfill_lock_);
    while (num_tablets_backfilling_ >= FLAGS_num_concurrent_backfills_allowed) {
      if (backfill_cond_.wait_until(l, deadline) == std::cv_status::timeout) {
        SetupErrorAndRespond(
            resp->mutable_error(),
            STATUS_FORMAT(ServiceUnavailable,
                          "Already running $0 backfill requests",
                          num_tablets_backfilling_),
            &context);
        return;
      }
    }
    num_tablets_backfilling_++;
  }
  auto se = ScopeExit([this] {
    std::unique_lock<std::mutex> l(this->backfill_lock_);
    this->num_tablets_backfilling_--;
    YB_PROFILE(this->backfill_cond_.notify_all());
  });

  // Wait for SafeTime to get past read_at;
  const HybridTime read_at(req->read_at_hybrid_time());
  DVLOG(1) << "Waiting for safe time to be past " << read_at;
  const auto safe_time =
      tablet.tablet->SafeTime(tablet::RequireLease::kFalse, read_at, deadline);
  DVLOG(1) << "Got safe time " << safe_time.ToString();
  if (!safe_time.ok()) {
    LOG(WARNING) << "Could not get a good enough safe time " << safe_time.ToString();
    SetupErrorAndRespond(resp->mutable_error(), safe_time.status(), &context);
    return;
  }

  // Don't work on the request if we have had to wait more than 50%
  // of the time allocated to us for the RPC.
  // Backfill is a costly operation, we do not want to start working
  // on it if we expect the client (master) to time out the RPC and
  // force us to redo the work.
  const auto coarse_now = CoarseMonoClock::Now();
  if (deadline - coarse_now < coarse_now - coarse_start) {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_FORMAT(
            ServiceUnavailable, "Not enough time left $0", deadline - coarse_now),
        &context);
    return;
  }

  auto max_sleep_ms = FLAGS_TEST_index_backfill_fail_after_random_wait_upto_ms;
  if (max_sleep_ms > 0) {
    auto rand_wait = RandomUniformInt(0, max_sleep_ms);
    LOG(INFO) << "Randomly sleeping for " << rand_wait << " ms before failing";
    SleepFor(1ms * rand_wait);
    SetupErrorAndRespond(
        resp->mutable_error(), STATUS_FORMAT(InvalidArgument, "Failing as requested for testing."),
        TabletServerErrorPB::OPERATION_NOT_SUPPORTED, &context);
    return;
  }

  // TODO(asrivastava): This does not correctly handle colocated tables.
  const uint32_t our_schema_version =
      tablet.peer->tablet_metadata()->primary_table_schema_version();
  const uint32_t their_schema_version = req->schema_version();
  bool all_at_backfill = true;
  bool all_past_backfill = true;
  bool is_pg_table = tablet.tablet->table_type() == TableType::PGSQL_TABLE_TYPE;
  while (FLAGS_TEST_block_backfill_before_index_map) {
    constexpr auto kSpinWait = 100ms;
    YB_LOG_EVERY_N_SECS(INFO, 1) << "Blocking BackfillIndex before index_map lookup";
    SleepFor(kSpinWait);
  }
  auto index_map_result = tablet.peer->tablet_metadata()->index_map(req->indexed_table_id());
  if (!index_map_result.ok()) {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_FORMAT(
            NotFound, "Indexed table $0 not found in tablet $1 metadata: $2",
            req->indexed_table_id(), req->tablet_id(), index_map_result.status().ToString()),
        TabletServerErrorPB::TABLET_NOT_FOUND, &context);
    return;
  }
  const auto& index_map = *index_map_result;
  // For YSQL, take the index info from the request payload and do not consult the tablet's index
  // map at all.  Correctness of the online index build is enforced on the postgres side through
  // pg_index, and the permission state the map carries is about to stop reaching YSQL tablets
  // altogether (#33037).  For YCQL, the tablet's index map is the source of the index info, it
  // must be at exactly the DO_BACKFILL permission, and the permission checks below still apply.
  std::vector<qlexpr::IndexInfo> indexes_to_backfill;
  std::vector<TableId> index_ids;
  for (const auto& idx : req->indexes()) {
    index_ids.push_back(idx.table_id());
    if (is_pg_table) {
      indexes_to_backfill.emplace_back(idx);
      continue;
    }
    auto result = index_map->FindIndex(idx.table_id());
    if (!result) {
      const auto& index_table_id = idx.table_id();
      LOG(INFO) << "index " << index_table_id << " not found in tablet metadata";
      *resp->add_failed_index_ids() = index_table_id;
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS_FORMAT(
              InvalidArgument, "Index $0 not found in index_map. Current schema is $1",
              index_table_id, our_schema_version),
          TabletServerErrorPB::OPERATION_NOT_SUPPORTED, &context);
      return;
    }
    indexes_to_backfill.push_back(**result);
    all_at_backfill &= (*result)->index_permissions() == IndexPermissions::INDEX_PERM_DO_BACKFILL;
    all_past_backfill &= (*result)->index_permissions() > IndexPermissions::INDEX_PERM_DO_BACKFILL;
  }

  if (!is_pg_table) {
    if (all_past_backfill) {
      // This is possible if this tablet completed the backfill, but the master failed over before
      // other tablets could complete.  The new master is redoing the backfill, so it is safe to
      // ignore this request.
      LOG(WARNING) << "Received BackfillIndex RPC: " << req->DebugString()
                   << " after all indexes have moved past DO_BACKFILL. IndexMap is "
                   << AsString(index_map);
      context.RespondSuccess();
      return;
    }

    if (!all_at_backfill) {
      DCHECK_NE(our_schema_version, their_schema_version);
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS_FORMAT(
              InvalidArgument,
              "Tablet has a different schema $0 vs $1. "
              "Requested index is not ready to backfill. IndexMap: $2",
              our_schema_version, their_schema_version, AsString(index_map)),
          TabletServerErrorPB::MISMATCHED_SCHEMA, &context);
      return;
    }
  }

  Status backfill_status;
  std::string backfilled_until;
  std::unordered_set<TableId> failed_indexes;
  uint64_t num_rows_read_from_table_for_backfill = 0;
  double num_rows_backfilled_in_index = 0.0;
  if (is_pg_table) {
    if (!req->has_namespace_name()) {
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS(
              InvalidArgument,
              "Attempted backfill on YSQL table without supplying database name"),
          TabletServerErrorPB::OPERATION_NOT_SUPPORTED,
          &context);
      return;
    }
    // Check if this is an xCluster target of automatic DDL replication, if so then we need to use
    // tablet level read consistency as the ddl_queue table is holding up xCluster safe time.
    auto namespace_id = tablet.peer->GetNamespaceId();
    if (!namespace_id.ok()) {
      SetupErrorAndRespond(
          resp->mutable_error(),
          namespace_id.status().CloneAndPrepend(
              "Failed to get namespace ID for backfill indexes request"),
          TabletServerErrorPB::INVALID_SCHEMA, &context);
      return;
    }
    bool is_xcluster_automatic_mode_target =
        server_->GetXClusterContext().IsTargetAndInAutomaticMode(*namespace_id);

    DCHECK_EQ(indexes_to_backfill.size(), 1)
        << "We don't support batching index backfill in YSQL yet";
    const qlexpr::IndexInfo& index_to_backfill = indexes_to_backfill[0];
    backfill_status = tablet.tablet->BackfillIndexesForYsql(
        index_to_backfill,
        req->start_key(),
        deadline,
        read_at,
        server_->pgsql_proxy_bind_address(),
        req->namespace_name(),
        server_->GetSharedMemoryPostgresAuthKey(),
        is_xcluster_automatic_mode_target,
        &num_rows_read_from_table_for_backfill,
        &num_rows_backfilled_in_index,
        &backfilled_until);
    if (backfill_status.IsIllegalState()) {
      failed_indexes.insert(index_to_backfill.table_id());
      DCHECK_EQ(failed_indexes.size(), 1) << "We don't support batching in YSQL yet";
    }

    resp->mutable_num_rows_backfilled_in_index()->insert(
        {index_to_backfill.table_id(), num_rows_backfilled_in_index});
  } else if (tablet.tablet->table_type() == TableType::YQL_TABLE_TYPE) {
    backfill_status = tablet.tablet->BackfillIndexes(
        indexes_to_backfill,
        req->start_key(),
        deadline,
        read_at,
        &num_rows_read_from_table_for_backfill,
        &backfilled_until,
        &failed_indexes);
  } else {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS(InvalidArgument, "Attempted backfill on tablet of invalid table type"),
        TabletServerErrorPB::OPERATION_NOT_SUPPORTED,
        &context);
    return;
  }
  DVLOG(1) << "Tablet " << tablet.peer->tablet_id() << " backfilled indexes "
           << yb::ToString(index_ids) << " and got " << backfill_status
           << " backfilled until : " << b2a_hex(backfilled_until);

  resp->set_backfilled_until(backfilled_until);
  resp->set_propagated_hybrid_time(server_->Clock()->Now().ToUint64());
  resp->set_num_rows_read_from_table_for_backfill(num_rows_read_from_table_for_backfill);

  if (!backfill_status.ok()) {
    VLOG(2) << " Failed indexes are " << yb::ToString(failed_indexes);
    for (const auto& idx : failed_indexes) {
      *resp->add_failed_index_ids() = idx;
    }
    SetupErrorAndRespond(
        resp->mutable_error(),
        backfill_status,
        (backfill_status.IsIllegalState()
            ? TabletServerErrorPB::OPERATION_NOT_SUPPORTED
            : TabletServerErrorPB::UNKNOWN_ERROR),
        &context);
    return;
  }

  context.RespondSuccess();
}

void TabletServiceAdminImpl::AlterSchema(const tablet::ChangeMetadataRequestPB* req,
                                         ChangeMetadataResponsePB* resp,
                                         rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "ChangeMetadata", req, resp, &context)) {
    return;
  }
  VLOG(1) << "Received Change Metadata RPC: " << req->DebugString();
  if (FLAGS_TEST_alter_schema_delay_ms) {
    LOG(INFO) << __func__ << ": sleeping for " << FLAGS_TEST_alter_schema_delay_ms << "ms";
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_alter_schema_delay_ms));
    LOG(INFO) << __func__ << ": done sleeping for " << FLAGS_TEST_alter_schema_delay_ms << "ms";
  }

  server::UpdateClock(*req, server_->Clock());

  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  tablet::TableInfoPtr table_info;
  if (req->has_alter_table_id()) {
    auto result = tablet.peer->tablet_metadata()->GetTableInfo(req->alter_table_id());
    if (!result.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), result.status(),
                           TabletServerErrorPB::INVALID_SCHEMA, &context);
      return;
    }
    table_info = *result;
  } else {
    table_info = tablet.peer->tablet_metadata()->primary_table_info();
  }
  const Schema& tablet_schema = table_info->schema();
  uint32_t schema_version = table_info->schema_version;
  // Sanity check, to verify that the tablet should have the same schema
  // specified in the request.
  Schema req_schema;
  Status s = SchemaFromPB(req->schema(), &req_schema);
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s, TabletServerErrorPB::INVALID_SCHEMA, &context);
    return;
  }

  // If the schema was already applied, respond as succeeded.
  if (!req->has_wal_retention_secs() && schema_version == req->schema_version()) {

    if (req_schema.Equals(tablet_schema)) {
      context.RespondSuccess();
      return;
    }

    // If the request is to insert a packed schema, respond as succeeded.
    // TODO(#28326): Need to validate that we do have the intermediate schema instead of just
    // returning success.
    if (req->insert_packed_schema()) {
      context.RespondSuccess();
      return;
    }

    schema_version = tablet.peer->tablet_metadata()->schema_version(
        req->has_alter_table_id() ? req->alter_table_id() : "");
    if (schema_version == req->schema_version()) {
      LOG(DFATAL) << "The current schema does not match the request schema."
                  << " version=" << schema_version
                  << " current-schema=" << tablet_schema.ToString()
                  << " request-schema=" << req_schema.ToString()
                  << " (corruption)";
      SetupErrorAndRespond(resp->mutable_error(),
                           STATUS(Corruption, "got a different schema for the same version number"),
                           TabletServerErrorPB::MISMATCHED_SCHEMA, &context);
      return;
    }
  }

  // If the current schema is newer than the one in the request reject the request.
  if (schema_version > req->schema_version()) {
    LOG(WARNING) << "Tablet " << req->tablet_id() << " has a newer schema"
                 << " version=" << schema_version
                 << " req->schema_version()=" << req->schema_version()
                 << "\n current-schema=" << tablet_schema.ToString()
                 << "\n request-schema=" << req_schema.ToString();
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_FORMAT(
            InvalidArgument, "Tablet has a newer schema Tab $0. Req $1 vs Existing version : $2",
            req->tablet_id(), req->schema_version(), schema_version),
        TabletServerErrorPB::TABLET_HAS_A_NEWER_SCHEMA, &context);
    return;
  }

  VLOG(1) << "Tablet updating schema from "
          << " version=" << schema_version << " current-schema=" << tablet_schema.ToString()
          << " to request-schema=" << req_schema.ToString()
          << " for table ID=" << table_info->table_id;

  // There is no need to pause writes for an AlterSchema operation that is only going
  // to be setting retention barriers, as is the case when an AlterSchema is issued in
  // the context of a CDCSDK Create Stream
  ScopedRWOperationPause pause_writes;
  if (!req->has_retention_requester_id() &&
      ((tablet.tablet->table_type() == TableType::YQL_TABLE_TYPE &&
       !FLAGS_disable_alter_vs_write_mutual_exclusion) ||
      tablet.tablet->table_type() == TableType::PGSQL_TABLE_TYPE)) {
    // For schema change operations we will have to pause the write operations
    // until the schema change is done. This will be done synchronously.
    pause_writes = tablet.tablet->PauseWritePermits(context.GetClientDeadline());
    if (!pause_writes.ok()) {
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS(
              TryAgain, "Could not lock the tablet against write operations for schema change"),
          &context);
      return;
    }

    // After write operation is paused, active transactions will be aborted for YSQL transactions.
    if (tablet.tablet->table_type() == TableType::PGSQL_TABLE_TYPE &&
        req->should_abort_active_txns() &&
        !FLAGS_TEST_skip_aborting_active_transactions_during_schema_change) {
      DCHECK(req->has_transaction_id());
      if (tablet.tablet->transaction_participant() == nullptr) {
        auto status = STATUS(
            IllegalState, "Transaction participant is null for tablet " + req->tablet_id());
        LOG(DFATAL) << status;
        SetupErrorAndRespond(
            resp->mutable_error(),
            status,
            &context);
        return;
      }
      HybridTime max_cutoff = HybridTime::kMax;
      CoarseTimePoint deadline =
          CoarseMonoClock::Now() +
          MonoDelta::FromMilliseconds(FLAGS_ysql_transaction_abort_timeout_ms);
      TransactionId txn_id = CHECK_RESULT(TransactionId::FromString(req->transaction_id()));
      OnlyAbortTxnsNotUsingTableLocks only_abort_txns_not_using_table_locks =
          req->has_only_abort_txns_not_using_table_locks() &&
                  req->only_abort_txns_not_using_table_locks()
              ? OnlyAbortTxnsNotUsingTableLocks::kTrue
              : OnlyAbortTxnsNotUsingTableLocks::kFalse;
      LOG(INFO) << "Aborting transactions "
                << (only_abort_txns_not_using_table_locks ? "not using table locks " : "")
                << "that started prior to " << max_cutoff << " for tablet id " << req->tablet_id()
                << " excluding transaction with id " << txn_id;
      // There could be a chance where a transaction does not appear by transaction_participant
      // but has already begun replicating through Raft. Such transactions might succeed rather
      // than get aborted. This race codnition is dismissable for this intermediate solution.
      Status status = tablet.tablet->transaction_participant()->StopActiveTxnsPriorTo(
          only_abort_txns_not_using_table_locks, max_cutoff, deadline, &txn_id);
      if (!status.ok() || PREDICT_FALSE(FLAGS_TEST_fail_alter_schema_after_abort_transactions)) {
        auto status = STATUS(TryAgain, "Transaction abort failed for tablet " + req->tablet_id());
        LOG(WARNING) << status;
        SetupErrorAndRespond(
            resp->mutable_error(),
            status,
            &context);
        return;
      }
    }
  }
  auto operation = std::make_unique<ChangeMetadataOperation>(
      tablet.tablet, tablet.peer->log());
  auto request = operation->AllocateRequest();
  request->CopyFrom(*req);

  // CDC SDK Create Stream Context
  if (req->has_retention_requester_id()) {
    auto status = SetupCDCSDKRetention(req, resp, tablet.peer);
    if (!status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), status, &context);
      return;
    }
  }

  operation->set_completion_callback(
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));
  operation->UsePermitToken(std::move(pause_writes));

  // Submit the alter schema op. The RPC will be responded to asynchronously.
  tablet.peer->Submit(std::move(operation), tablet.leader_term);
}

Status TabletServiceAdminImpl::SetupCDCSDKRetention(const tablet::ChangeMetadataRequestPB* req,
                                                    ChangeMetadataResponsePB* resp,
                                                    const TabletPeerPtr& tablet_peer) {

  tablet::RemoveIntentsData data;
  auto s = tablet_peer->GetLastReplicatedData(&data);
  if (s.ok()) {
    LOG_WITH_PREFIX(INFO) << "Proposed cdc_sdk_snapshot_safe_op_id: " << data.op_id
                          << ", time: " << data.log_ht;
    resp->mutable_cdc_sdk_snapshot_safe_op_id()->set_term(data.op_id.term);
    resp->mutable_cdc_sdk_snapshot_safe_op_id()->set_index(data.op_id.index);
  } else {
    LOG_WITH_PREFIX(WARNING) << "CDCSDK Create Stream context: "
                             << "Could not get snapshot_safe_opid: "
                             << s;
    return s;
  }

  // Now, from this point on, till the Followers Apply the ChangeMetadataOperation,
  // any proposed history cutoff they process can only be less than Now()
  auto require_history_cutoff =
      req->has_cdc_sdk_require_history_cutoff() && req->cdc_sdk_require_history_cutoff();
  auto res = tablet_peer->SetAllInitialCDCSDKRetentionBarriers(
      data.op_id, server_->Clock()->Now(), require_history_cutoff);
  // If there was an error while setting the retention barrier cutoff, respond with an error
  if (!res.ok()) {
    WARN_NOT_OK(res, "CDCSDK Create Stream context: Unable to set history cutoff");
    RETURN_NOT_OK(res);
  }

  return Status::OK();
}


#define VERIFY_RESULT_OR_RETURN(expr) RESULT_CHECKER_HELPER( \
    expr, \
    if (!__result.ok()) { return; });

void TabletServiceImpl::VerifyTableRowRange(
    const VerifyTableRowRangeRequestPB* req,
    VerifyTableRowRangeResponsePB* resp,
    rpc::RpcContext context) {
  DVLOG(3) << "Received VerifyTableRowRange RPC: " << req->DebugString();

  server::UpdateClock(*req, server_->Clock());

  auto peer_tablet =
      LookupTabletPeerOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!peer_tablet) {
    return;
  }

  auto tablet = peer_tablet->tablet;
  bool is_pg_table = tablet->table_type() == TableType::PGSQL_TABLE_TYPE;
  if (is_pg_table) {
    SetupErrorAndRespond(
        resp->mutable_error(), STATUS(NotFound, "Verify operation not supported for PGSQL tables."),
        &context);
    return;
  }

  const CoarseTimePoint& deadline = context.GetClientDeadline();

  // Wait for SafeTime to get past read_at. Without a caller supplied read time verify as of
  // MaxGlobalNow() rather than at the replica's current safe time, which only advances as the
  // leader propagates it and thus may name a snapshot from before writes the caller expects to
  // verify - e.g. a just completed index backfill, whose rows would then all be reported as
  // missing. MaxGlobalNow() rather than Now() because this replica's clock may lag the cluster by
  // up to the max clock skew, and the request is served by any peer, not just the leader.
  const HybridTime read_at =
      req->has_read_time() ? HybridTime(req->read_time()) : server_->Clock()->MaxGlobalNow();
  DVLOG(1) << "Waiting for safe time to be past " << read_at;
  const auto safe_time = tablet->SafeTime(tablet::RequireLease::kFalse, read_at, deadline);
  DVLOG(1) << "Got safe time " << safe_time.ToString();
  if (!safe_time.ok()) {
    // A lagging replica that never reaches read_at before the deadline is an expected outcome, not
    // an invariant violation.
    LOG(WARNING) << "Could not get a good enough safe time " << safe_time.ToString();
    SetupErrorAndRespond(resp->mutable_error(), safe_time.status(), &context);
    return;
  }

  auto valid_read_at = read_at;
  std::string verified_until = "";
  std::unordered_map<TableId, uint64> consistency_stats;

  if (peer_tablet->tablet_peer->tablet_metadata()->primary_table_info()->index_info) {
    auto index_info =
        *peer_tablet->tablet_peer->tablet_metadata()->primary_table_info()->index_info;
    const auto& table_id = index_info.indexed_table_id();
    Status verify_status = tablet->VerifyMainTableConsistencyForCQL(
        table_id, req->start_key(), req->num_rows(), deadline, valid_read_at, &consistency_stats,
        &verified_until);
    if (!verify_status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), verify_status, &context);
      return;
    }

    (*resp->mutable_consistency_stats())[table_id] = consistency_stats[table_id];
  } else {
    const auto& index_map =
        *peer_tablet->tablet_peer->tablet_metadata()->primary_table_info()->index_map;
    vector<qlexpr::IndexInfo> indexes;
    vector<TableId> index_ids;
    if (req->index_ids().empty()) {
      for (auto it = index_map.begin(); it != index_map.end(); it++) {
        indexes.push_back(it->second);
      }
    } else {
      for (const auto& idx : req->index_ids()) {
        auto result = index_map.FindIndex(idx);
        if (result) {
          const auto* index_info = *result;
          indexes.push_back(*index_info);
          index_ids.push_back(index_info->table_id());
        } else {
          LOG(WARNING) << "Index " << idx << " not found in tablet metadata";
        }
      }
    }

    Status verify_status = tablet->VerifyIndexTableConsistencyForCQL(
        indexes, req->start_key(), req->num_rows(), deadline, valid_read_at, &consistency_stats,
        &verified_until);
    if (!verify_status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), verify_status, &context);
      return;
    }

    for (const auto& index : indexes) {
      const auto& table_id = index.table_id();
      (*resp->mutable_consistency_stats())[table_id] = consistency_stats[table_id];
    }
  }
  resp->set_verified_until(verified_until);
  context.RespondSuccess();
}

void TabletServiceImpl::UpdateTransaction(const UpdateTransactionRequestPB* req,
                                          UpdateTransactionResponsePB* resp,
                                          rpc::RpcContext context) {
  TRACE("UpdateTransaction");

  if (req->state().status() == TransactionStatus::CREATED &&
      RandomActWithProbability(TEST_delay_create_transaction_probability)) {
    std::this_thread::sleep_for(
        (FLAGS_transaction_rpc_timeout_ms + RandomUniformInt(-200, 200)) * 1ms);
  }

  VLOG(1) << "UpdateTransaction: " << req->ShortDebugString()
          << ", context: " << context.ToString();
  LOG_IF(DFATAL, !req->has_propagated_hybrid_time())
      << __func__ << " missing propagated hybrid time for "
      << TransactionStatus_Name(req->state().status());
  UpdateClock(*req, server_->Clock());

  LeaderTabletPeer tablet;
  auto txn_status = req->state().status();
  auto cleanup = txn_status == TransactionStatus::IMMEDIATE_CLEANUP ||
                 txn_status == TransactionStatus::GRACEFUL_CLEANUP;
  if (cleanup) {
    auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
        server_->tablet_peer_lookup(), req->tablet_id(), resp, &context));
    tablet.FillTabletPeer(std::move(peer_tablet));
    tablet.leader_term = OpId::kUnknownTerm;
  } else {
    tablet = LookupLeaderTabletOrRespond(
        server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  }
  if (!tablet) {
    return;
  }

  if (txn_status == TransactionStatus::COMMITTED && req->state().has_origination_info()) {
    auto status = CheckPersistenceTestRejection(req->state().origination_info());
    if (!status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), status, &context);
      return;
    }
  }

  auto state = std::make_unique<tablet::UpdateTxnOperation>(tablet.tablet);
  state->AllocateRequest()->CopyFrom(req->state());
  state->set_completion_callback(MakeRpcOperationCompletionCallback(
      std::move(context), resp, server_->Clock()));

  if (req->state().status() == TransactionStatus::APPLYING ||
      req->state().status() == TransactionStatus::PROMOTING ||
      cleanup) {
    if (req->state().status() == TransactionStatus::APPLYING &&
        tablet.tablet->table_type() != TableType::TRANSACTION_STATUS_TABLE_TYPE) {
      TEST_PAUSE_IF_FLAG(TEST_block_apply_intent);
    }
    auto* participant = tablet.tablet->transaction_participant();
    if (participant) {
      participant->Handle(std::move(state), tablet.leader_term);
    } else {
      state->CompleteWithStatus(STATUS_FORMAT(
          InvalidArgument, "Does not have transaction participant to process $0",
          req->state().status()));
    }
  } else {
    auto* coordinator = tablet.tablet->transaction_coordinator();
    if (coordinator) {
      coordinator->Handle(std::move(state), tablet.leader_term);
    } else {
      state->CompleteWithStatus(STATUS_FORMAT(
          InvalidArgument, "Does not have transaction coordinator to process $0",
          req->state().status()));
    }
  }
}

void TabletServiceImpl::GetTransactionStatus(const GetTransactionStatusRequestPB* req,
                                             GetTransactionStatusResponsePB* resp,
                                             rpc::RpcContext context) {
  PerformAtLeader(req, resp, &context, server_, "GetTransactionStatus",
      [req, resp, &context](const LeaderTabletPeer& tablet_peer) {
    auto* transaction_coordinator = tablet_peer.tablet->transaction_coordinator();
    if (!transaction_coordinator) {
      return STATUS_FORMAT(
          InvalidArgument, "No transaction coordinator at tablet $0",
          tablet_peer.peer->tablet_id());
    }
    return transaction_coordinator->GetStatus(
        req->transaction_id(), context.GetClientDeadline(), resp);
  });
}

void TabletServiceImpl::GetOldTransactions(const GetOldTransactionsRequestPB* req,
                                           GetOldTransactionsResponsePB* resp,
                                           rpc::RpcContext context) {
  PerformAtLeader(req, resp, &context, server_, "GetOldTransactions",
      [req, resp, &context](const LeaderTabletPeer& tablet_peer) {
    auto* transaction_coordinator = tablet_peer.tablet->transaction_coordinator();
    if (!transaction_coordinator) {
      return STATUS_FORMAT(
          InvalidArgument, "No transaction coordinator at tablet $0",
          tablet_peer.peer->tablet_id());
    }
    return transaction_coordinator->GetOldTransactions(req, resp, context.GetClientDeadline());
  });
}

void TabletServiceImpl::GetOldSingleShardWaiters(const GetOldSingleShardWaitersRequestPB* req,
                                                 GetOldSingleShardWaitersResponsePB* resp,
                                                rpc::RpcContext context) {
  TRACE("GetOldSingleShardWaiters");

  auto* waiting_txn_registry = server_->tablet_manager()->waiting_txn_registry();
  if (PREDICT_FALSE(!waiting_txn_registry)) {
    LOG(INFO) << "Skipping query for old single shard waiters as the server's "
              << "waiting_txn_registry doesn't exist.";
    context.RespondSuccess();
    return;
  }
  auto s = waiting_txn_registry->GetOldSingleShardWaiters(*req, resp);
  if (s.ok()) {
    context.RespondSuccess();
  } else {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
  }
}

void TabletServiceImpl::GetTransactionStatusAtParticipant(
    const GetTransactionStatusAtParticipantRequestPB* req,
    GetTransactionStatusAtParticipantResponsePB* resp,
    rpc::RpcContext context) {
  PerformAtLeader(req, resp, &context, server_, "GetTransactionStatusAtParticipant",
      [req, resp, &context](const LeaderTabletPeer& tablet_peer) -> Status {
    auto* transaction_participant = tablet_peer.tablet->transaction_participant();
    if (!transaction_participant) {
      return STATUS_FORMAT(
          InvalidArgument, "No transaction participant at tablet $0",
          tablet_peer.peer->tablet_id());
    }

    transaction_participant->GetStatus(
        VERIFY_RESULT(FullyDecodeTransactionId(req->transaction_id())),
        req->required_num_replicated_batches(), tablet_peer.leader_term, resp, &context);
    return Status::OK();
  });
}

void TabletServiceImpl::AbortTransaction(const AbortTransactionRequestPB* req,
                                         AbortTransactionResponsePB* resp,
                                         rpc::RpcContext context) {
  TRACE("AbortTransaction");

  UpdateClock(*req, server_->Clock());

  auto id_or_status = FullyDecodeTransactionId(req->transaction_id());
  if (!id_or_status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), id_or_status.status(), &context);
    return;
  }
  const auto& txn_id = *id_or_status;

  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  server::ClockPtr clock(server_->Clock());
  auto context_ptr = std::make_shared<rpc::RpcContext>(std::move(context));
  tablet.tablet->transaction_coordinator()->Abort(
      txn_id,
      tablet.leader_term,
      req->has_deadlock_reason() ? StatusFromPB(req->deadlock_reason()) : Status::OK(),
      [resp, context_ptr, clock, peer = tablet.peer](Result<TransactionStatusResult> result) {
        resp->set_propagated_hybrid_time(clock->Now().ToUint64());
        Status status;
        if (result.ok()) {
          auto leader_safe_time = peer->LeaderSafeTime();
          if (leader_safe_time.ok()) {
            resp->set_status(result->status);
            if (result->status_time.is_valid()) {
              resp->set_status_hybrid_time(result->status_time.ToUint64());
            }
            // See comment above WaitForSafeTime in TransactionStatusCache::DoGetCommitData
            // for details.
            resp->set_coordinator_safe_time(leader_safe_time->ToUint64());
            context_ptr->RespondSuccess();
            return;
          }

          status = leader_safe_time.status();
        } else {
          status = result.status();
        }
        SetupErrorAndRespond(resp->mutable_error(), status, context_ptr.get());
      });
}

void TabletServiceImpl::UpdateTransactionWaitingForStatus(
    const UpdateTransactionWaitingForStatusRequestPB* req,
    UpdateTransactionWaitingForStatusResponsePB* resp,
    rpc::RpcContext context) {
  UpdateClock(*req, server_->Clock());

  auto tablet = LookupLeaderTabletOrRespond(
    server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  tablet.tablet->transaction_coordinator()->ProcessWaitForReport(
      *req, resp, MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));
}

void TabletServiceImpl::ProbeTransactionDeadlock(
    const ProbeTransactionDeadlockRequestPB* req,
    ProbeTransactionDeadlockResponsePB* resp,
    rpc::RpcContext context) {
  UpdateClock(*req, server_->Clock());

  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  tablet.tablet->transaction_coordinator()->ProcessProbe(
      *req, resp, MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));
}

void TabletServiceImpl::Truncate(const TruncateRequestPB* req,
                                 TruncateResponsePB* resp,
                                 rpc::RpcContext context) {
  TRACE("Truncate");

  UpdateClock(*req, server_->Clock());

  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  auto operation = std::make_unique<TruncateOperation>(tablet.tablet);
  operation->AllocateRequest()->CopyFrom(req->truncate());

  operation->set_completion_callback(
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));

  // Submit the truncate tablet op. The RPC will be responded to asynchronously.
  tablet.peer->Submit(std::move(operation), tablet.leader_term);
}

void TabletServiceImpl::GetCompatibleSchemaVersion(
    const GetCompatibleSchemaVersionRequestPB* req,
    GetCompatibleSchemaVersionResponsePB* resp,
    rpc::RpcContext context) {
  VLOG(1) << "Full request: " << req->DebugString();
  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  Schema req_schema;
  Status s = SchemaFromPB(req->schema(), &req_schema);
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s, TabletServerErrorPB::INVALID_SCHEMA, &context);
    return;
  }

  tablet::TableInfoPtr table_info = nullptr;
  if (req->schema().has_colocated_table_id()) {
    auto result = tablet.peer->tablet_metadata()->GetTableInfo(
        req->schema().colocated_table_id().colocation_id());
    if (!result.ok()) {
      SetupErrorAndRespond(
          resp->mutable_error(), result.status(), TabletServerErrorPB::TABLET_NOT_FOUND, &context);
      return;
    }

    table_info = *result;
  } else {
    table_info = tablet.peer->tablet_metadata()->primary_table_info();
  }

  const Schema& consumer_schema = table_info->schema();
  SchemaVersion schema_version = table_info->schema_version;

  // Check is the current schema already matches. If not,
  // check if there is a compatible schema packing and return
  // the associated schema version.
  if (!req_schema.EquivalentForDataCopy(consumer_schema)) {
    auto result = table_info->GetSchemaPackingVersion(req_schema);
    if (result.ok()) {
      schema_version = *result;
    } else {
      // Also set the latest schema version.
      resp->set_latest_schema_version(schema_version);
      SetupErrorAndRespond(
          resp->mutable_error(), result.status(), TabletServerErrorPB::MISMATCHED_SCHEMA, &context);
      return;
    }
  }

  // Set the compatible consumer schema version and respond.
  VLOG_WITH_FUNC(2) << Format("Compatible schema version $0 for schema $1 found on tablet $2.",
                              schema_version, req_schema.ToString(), req->tablet_id());
  resp->set_compatible_schema_version(schema_version);
  context.RespondSuccess();
}

void TabletServiceAdminImpl::CreateTablet(const CreateTabletRequestPB* req,
                                          CreateTabletResponsePB* resp,
                                          rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "CreateTablet", req, resp, &context)) {
    return;
  }

  CoarseTimePoint deadline = context.GetClientDeadline();
  CoarseTimePoint now = CoarseMonoClock::now();
  auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
  MonoDelta timeout = MonoDelta::FromNanoseconds(duration.count());

  auto status = DoCreateTablet(req, resp, timeout);
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
  } else {
    context.RespondSuccess();
  }
}

Status TabletServiceAdminImpl::DoCreateTablet(const CreateTabletRequestPB* req,
                                              CreateTabletResponsePB* resp,
                                              const MonoDelta& timeout) {
  if (PREDICT_FALSE(FLAGS_TEST_txn_status_table_tablet_creation_delay_ms > 0 &&
                    req->table_type() == TableType::TRANSACTION_STATUS_TABLE_TYPE)) {
    std::this_thread::sleep_for(FLAGS_TEST_txn_status_table_tablet_creation_delay_ms * 1ms);
  }

  DVLOG(3) << "Received CreateTablet RPC: " << yb::ToString(*req);
  TRACE_EVENT1("tserver", "CreateTablet",
               "tablet_id", req->tablet_id());

  Schema schema;
  dockv::PartitionSchema partition_schema;
  auto status = SchemaFromPB(req->schema(), &schema);
  if (status.ok()) {
    DCHECK(schema.has_column_ids());
    status = dockv::PartitionSchema::FromPB(req->partition_schema(), schema, &partition_schema);
  }
  if (!status.ok()) {
    return status.CloneAndAddErrorCode(TabletServerError(TabletServerErrorPB::INVALID_SCHEMA));
  }

  dockv::Partition partition;
  dockv::Partition::FromPB(req->partition(), &partition);

  LOG(INFO) << "Processing CreateTablet for T " << req->tablet_id() << " P " << req->dest_uuid()
            << " (table=" << req->table_name() << " [id=" << req->table_id()
            << "]), partition=" << partition_schema.PartitionDebugString(partition, schema);
  VLOG(1) << "Full request: " << req->DebugString();

  auto table_info = std::make_shared<tablet::TableInfo>(
      consensus::MakeTabletLogPrefix(req->tablet_id(), server_->permanent_uuid()),
      tablet::Primary::kTrue, req->table_id(), req->namespace_name(), req->namespace_id(),
      req->table_name(),
      req->table_type(), schema, qlexpr::IndexMap(),
      req->has_index_info() ? std::optional<qlexpr::IndexInfo>(req->index_info()) : std::nullopt,
      0 /* schema_version */, partition_schema, OpId{}, HybridTime{}, req->pg_table_id(),
      tablet::SkipTableTombstoneCheck(FLAGS_ysql_yb_enable_alter_table_rewrite));

  if (req->has_wal_retention_secs()) {
    table_info->wal_retention_secs = req->wal_retention_secs();
  }

  std::vector<SnapshotScheduleId> snapshot_schedules;
  snapshot_schedules.reserve(req->snapshot_schedules().size());
  for (const auto& id : req->snapshot_schedules()) {
    snapshot_schedules.push_back(VERIFY_RESULT(FullyDecodeSnapshotScheduleId(id)));
  }

  std::unordered_set<StatefulServiceKind> hosted_services;
  for (auto& service_kind : req->hosted_stateful_services()) {
    SCHECK(
        StatefulServiceKind_IsValid(service_kind), InvalidArgument,
        Format("Invalid stateful service kind: $0", service_kind));
    hosted_services.insert((StatefulServiceKind)service_kind);
  }

  bool cdc_sdk_setup_retention =
      req->has_cdc_sdk_set_retention_barriers() && req->cdc_sdk_set_retention_barriers();

  auto const tablet_peer_result = server_->tablet_manager()->CreateNewTablet(
      table_info, req->tablet_id(), partition, req->config(), req->colocated(), snapshot_schedules,
      hosted_services, req->target_storage_tier());
  if (PREDICT_FALSE(!tablet_peer_result.ok())) {
    status = tablet_peer_result.status();
    auto is_already_present = status.IsAlreadyPresent();
    if (is_already_present && cdc_sdk_setup_retention) {
      auto existing_peer_result = server_->tablet_manager()->GetTablet(req->tablet_id());
      if (!existing_peer_result.ok()) {
        return existing_peer_result.status().CloneAndAppend(
            Format("Tablet peer not found for already present tablet $0", req->tablet_id()));
      }

      if ((*existing_peer_result)->state() != tablet::RaftGroupStatePB::FAILED) {
        RETURN_NOT_OK(SetupCDCSDKRetentionOnNewTablet(timeout, resp, *existing_peer_result));
      } else {
        LOG_WITH_PREFIX(WARNING) << "Skipping CDC retention barrier setup for tablet "
                                 << req->tablet_id() << " because the peer is in FAILED state";
      }
    }
    return is_already_present ? status.CloneAndAddErrorCode(
                                    TabletServerError(TabletServerErrorPB::TABLET_ALREADY_EXISTS))
                              : status;
  }

  if (PREDICT_FALSE(FLAGS_TEST_cdc_fail_before_setting_barrier)) {
    return STATUS_FORMAT(IllegalState, "TEST failing before attempting to set retention barrier");
  }

  if (!cdc_sdk_setup_retention) {
    return Status::OK();
  }
  return SetupCDCSDKRetentionOnNewTablet(timeout, resp, *tablet_peer_result);
}

Status TabletServiceAdminImpl::SetupCDCSDKRetentionOnNewTablet(
    const MonoDelta& timeout, CreateTabletResponsePB* resp,
    const tablet::TabletPeerPtr& tablet_peer) {
  auto set_failed_on_error = [&tablet_peer](const Status& status) {
    if (!status.ok()) {
      tablet_peer->SetFailed(status);
    }
    return status;
  };

  // Proceed only if the tablet is not in failed state.
  RETURN_NOT_OK(set_failed_on_error(tablet_peer->WaitUntilConsensusRunning(timeout)));

  tablet::RemoveIntentsData data;
  RETURN_NOT_OK(set_failed_on_error(tablet_peer->GetLastReplicatedData(&data)));

  if (FLAGS_TEST_cdc_sdk_fail_setting_retention_barrier) {
    return set_failed_on_error(
        STATUS_FORMAT(IllegalState, "TEST failing before attempting to set retention barrier"));
  }

  RETURN_NOT_OK(
      set_failed_on_error(ResultToStatus(tablet_peer->SetAllInitialCDCSDKRetentionBarriers(
          data.op_id, server_->Clock()->Now(), true /* require_history_cutoff */))));

  TEST_SYNC_POINT("SetupCDCSDKRetentionOnNewTablet::End");

  data.op_id.ToPB(resp->mutable_cdc_sdk_safe_op_id());
  return Status::OK();
}

void TabletServiceAdminImpl::PrepareDeleteTransactionTablet(
    const PrepareDeleteTransactionTabletRequestPB* req,
    PrepareDeleteTransactionTabletResponsePB* resp,
    rpc::RpcContext context) {
  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }
  auto coordinator = tablet.tablet->transaction_coordinator();
  const CoarseTimePoint& deadline = context.GetClientDeadline();
  if (coordinator) {
    VLOG(1) << "Preparing transaction status tablet " << req->tablet_id() << " for deletion.";
    auto status = coordinator->PrepareForDeletion(deadline);
    if (!status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), status, &context);
      return;
    }
  }
  context.RespondSuccess();
}

void TabletServiceAdminImpl::DeleteTablet(const DeleteTabletRequestPB* req,
                                          DeleteTabletResponsePB* resp,
                                          rpc::RpcContext context) {
  if (PREDICT_FALSE(FLAGS_TEST_rpc_delete_tablet_fail)) {
    context.RespondFailure(STATUS(NetworkError, "Simulating network partition for test"));
    return;
  }

  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "DeleteTablet", req, resp, &context)) {
    return;
  }
  TRACE_EVENT2("tserver", "DeleteTablet",
               "tablet_id", req->tablet_id(),
               "reason", req->reason());

  tablet::TabletDataState delete_type = tablet::TABLET_DATA_UNKNOWN;
  if (req->has_delete_type()) {
    delete_type = req->delete_type();
  }
  LOG(INFO) << "T " << req->tablet_id() << " P " << server_->permanent_uuid()
            << ": Processing DeleteTablet with delete_type " << TabletDataState_Name(delete_type)
            << (req->has_reason() ? (" (" + req->reason() + ")") : "")
            << (req->hide_only() ? " (Hide only)" : "")
            << (req->keep_data() ? " (Not deleting data)" : "") << " from "
            << context.requestor_string();
  VLOG(1) << "Full request: " << req->DebugString();

  std::optional<int64_t> cas_config_opid_index_less_or_equal;
  if (req->has_cas_config_opid_index_less_or_equal()) {
    cas_config_opid_index_less_or_equal = req->cas_config_opid_index_less_or_equal();
  }
  std::optional<TabletServerErrorPB::Code> error_code;
  std::optional<TransactionId> txn_id;
  if (req->has_transaction_id()) {
    txn_id = CHECK_RESULT(TransactionId::FromString(req->transaction_id()));
  }
  Status s = server_->tablet_manager()->DeleteTablet(
      req->tablet_id(), delete_type,
      tablet::ShouldAbortActiveTransactions(req->should_abort_active_txns()),
      cas_config_opid_index_less_or_equal, req->hide_only(), req->keep_data(), &error_code,
      std::move(txn_id), resp);
  if (PREDICT_FALSE(!s.ok())) {
    HandleErrorResponse(resp, &context, s, error_code);
    return;
  }
  context.RespondSuccess();
}

namespace {

class TabletsFlusherBase {
 public:
  explicit TabletsFlusherBase(
      const TabletServiceAdminImpl& service,
      const TSTabletManager::TabletPtrs& tablets,
      FlushTabletsResponsePB& resp)
    : service_(service), tablets_(tablets), resp_(resp)
  {}

  virtual ~TabletsFlusherBase() = default;

  Status Run();

  auto LogPrefix() const {
    return service_.LogPrefix();
  }

 private:
  virtual Status Flush(const tablet::TabletPtr& tablet) = 0;
  virtual Status WaitForFlush(const tablet::TabletPtr& tablet) = 0;

  const TabletServiceAdminImpl& service_;
  const TSTabletManager::TabletPtrs& tablets_;
  FlushTabletsResponsePB& resp_;
};

Status TabletsFlusherBase::Run() {
  for (const auto& tablet : tablets_) {
    resp_.set_failed_tablet_id(tablet->tablet_id());
    RETURN_NOT_OK(Flush(tablet));

    // Refer to https://github.com/yugabyte/yugabyte-db/issues/16116.
    if (!FLAGS_TEST_skip_force_superblock_flush) {
      RETURN_NOT_OK(tablet->FlushSuperblock(tablet::OnlyIfDirty::kTrue));
    }
    resp_.clear_failed_tablet_id();
  }

  // Wait for end of all flush operations.
  for (const auto& tablet : tablets_) {
    resp_.set_failed_tablet_id(tablet->tablet_id());
    RETURN_NOT_OK(WaitForFlush(tablet));
    resp_.clear_failed_tablet_id();
  }

  return Status::OK();
}

class TabletsFlusher final : public TabletsFlusherBase {
 public:
  explicit TabletsFlusher(
    const TabletServiceAdminImpl& service,
      const TSTabletManager::TabletPtrs& tablets,
      const tablet::FlushFlags flush_flags,
      FlushTabletsResponsePB& resp)
      : TabletsFlusherBase(service, tablets, resp),
        flush_flags_(flush_flags) {
    VLOG_WITH_PREFIX(1) << "TabletsFlusher: flush_flags: " << std::to_underlying(flush_flags_);
  }

 private:
  Status Flush(const tablet::TabletPtr& tablet) override {
    return tablet->Flush(
        tablet::FlushMode::kAsync, flush_flags_, rocksdb::FlushReason::kAdminFlush);
  }

  Status WaitForFlush(const tablet::TabletPtr& tablet) override {
    return tablet->WaitForFlush();
  }

  const tablet::FlushFlags flush_flags_;
};

class VectorIndexFlusher : public TabletsFlusherBase {
 public:
  explicit VectorIndexFlusher(
      const TabletServiceAdminImpl& service,
      const TSTabletManager::TabletPtrs& tablets,
      TableIds&& vector_index_ids,
      FlushTabletsResponsePB& resp)
      : TabletsFlusherBase(service, tablets, resp),
        vector_index_ids_(std::move(vector_index_ids)) {
    VLOG_WITH_PREFIX(1) << "VectorIndexFlusher: vector index ids: "
                        << (vector_index_ids_.size() ? AsString(vector_index_ids_) : "all");
  }

 private:
  Status Flush(const tablet::TabletPtr& tablet) override {
    auto [it, inserted] = tablet_vector_indexes_.try_emplace(
      tablet->tablet_id(),
      tablet->vector_indexes().Collect(vector_index_ids_)
    );

    if (!inserted) {
      LOG_WITH_PREFIX(DFATAL)
          << "Flush of vector indexes is already running for tablet " << tablet->tablet_id();
      return Status::OK();
    }

    it->second.Flush();
    return Status::OK();
  }

  Status WaitForFlush(const tablet::TabletPtr& tablet) override {
    auto it = tablet_vector_indexes_.find(tablet->tablet_id());
    if (it == tablet_vector_indexes_.end()) {
      LOG_WITH_PREFIX(DFATAL) << "Vector indexes are not found for tablet " << tablet->tablet_id();
      return Status::OK();
    }

    return it->second.WaitForFlush();
  }

  TableIds vector_index_ids_;
  std::unordered_map<TabletId, tablet::VectorIndexList> tablet_vector_indexes_;
};

bool IsRegularOnly(const FlushTabletsRequestPB& req) {
  return req.flags() == tablet::FLUSH_COMPACT_REGULAR_FOR_TEST_ONLY;
}

bool IsVectorIndexOnly(const FlushTabletsRequestPB& req) {
  return (req.flags() == tablet::FLUSH_COMPACT_VECTOR_INDEX_ONLY) ||
         (req.flags() == tablet::FLUSH_COMPACT_DEFAULT && req.vector_index_ids_size());
}

auto CopyVectorIndexIds(const FlushTabletsRequestPB& req) {
  return TableIds{ req.vector_index_ids().begin(), req.vector_index_ids().end() };
}

Status TriggerFlush(
    const TabletServiceAdminImpl& service,
    const TSTabletManager::TabletPtrs& tablets,
    const FlushTabletsRequestPB& req,
    FlushTabletsResponsePB& resp) {
  // FlushCompactFlags for FLUSH operation:
  // 1. FLUSH_COMPACT_DEFAULT
  //    Flush regular + intents + vector indexes. If vector_index_ids field is not empty,
  //    the value is treated as FLUSH_COMPACT_VECTOR_INDEX_ONLY.
  //
  // 2. FLUSH_COMPACT_REGULAR_FOR_TEST_ONLY
  //    Flush only regular db, used in tests only.
  //
  // 3. FLUSH_COMPACT_VECTOR_INDEX_EXCLUDED
  //    Not applicable for FLUSH operation.
  //
  // 4. FLUSH_COMPACT_VECTOR_INDEX_ONLY
  //    Flush only vector indexes. Empty vector_index_ids means all vector indexes.
  //
  // 5. FLUSH_COMPACT_ALL
  //    Flush regular + intents + vector indexes. Empty vector_index_ids means all vector indexes.
  DCHECK_EQ(req.operation(), FlushTabletsRequestPB::FLUSH);
  SCHECK_FORMAT(
    req.flags() != tablet::FLUSH_COMPACT_VECTOR_INDEX_EXCLUDED, InvalidArgument,
    "Flag [$0] is not supported for FLUSH operation", req.flags());

  if (IsVectorIndexOnly(req)) {
    return VectorIndexFlusher{ service, tablets, CopyVectorIndexIds(req), resp }.Run();
  }

  const tablet::FlushFlags flush_flags =
      IsRegularOnly(req) ? tablet::FlushFlags::kRegular : tablet::FlushFlags::kAllDbs;
  return TabletsFlusher{ service, tablets, flush_flags, resp }.Run();
}

Result<TableIdsPtr> CollectVectorIndexesForCompaction(const FlushTabletsRequestPB& req) {
  if (req.flags() == tablet::FLUSH_COMPACT_VECTOR_INDEX_EXCLUDED) {
    SCHECK(req.vector_index_ids_size() == 0, InvalidArgument,
           "vector_index_ids must not be specified with FLUSH_COMPACT_VECTOR_INDEX_EXCLUDED flag");
    return nullptr;
  }

  return std::make_shared<TableIds>(CopyVectorIndexIds(req));
}

Status TriggerCompact(
    TSTabletManager& tablet_manager,
    const TSTabletManager::TabletPtrs& tablets,
    const FlushTabletsRequestPB& req) {
  // FlushCompactFlags for COMPACT operation:
  // 1. FLUSH_COMPACT_DEFAULT
  //    Compact regular + intents + vector indexes. If vector_index_ids field is not empty,
  //    the value is treated as FLUSH_COMPACT_VECTOR_INDEX_ONLY.
  //
  // 2. FLUSH_COMPACT_REGULAR_FOR_TEST_ONLY
  //    Not applicable for COMPACT operation.
  //
  // 3. FLUSH_COMPACT_VECTOR_INDEX_EXCLUDED
  //    Compacts all storages except vector indexes.
  //
  // 4. FLUSH_COMPACT_VECTOR_INDEX_ONLY
  //    Compact only vector indexes. Empty vector_index_ids means all vector indexes.
  //
  // 5. FLUSH_COMPACT_ALL
  //    Compact regular + intents + vector indexes. Empty vector_index_ids means all vector indexes.
  DCHECK_EQ(req.operation(), FlushTabletsRequestPB::COMPACT);
  SCHECK_FORMAT(
      req.flags() != tablet::FLUSH_COMPACT_REGULAR_FOR_TEST_ONLY, InvalidArgument,
      "Flag [$0] is not supported for COMPACT operation", req.flags());

  AdminCompactionOptions options {
    ShouldWait::kTrue,
    rocksdb::SkipCorruptDataBlocksUnsafe(req.remove_corrupt_data_blocks_unsafe()),
    VERIFY_RESULT(CollectVectorIndexesForCompaction(req)),
    tablet::VectorIndexOnly(IsVectorIndexOnly(req))
  };

  return tablet_manager.TriggerAdminCompaction(tablets, options);
}

} // namespace

void TabletServiceAdminImpl::FlushTablets(const FlushTabletsRequestPB* req,
                                          FlushTabletsResponsePB* resp,
                                          rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "FlushTablets", req, resp, &context)) {
    return;
  }

  if (FLAGS_TEST_pause_tablet_compact_flush_ms) {
    const auto pause_ms = FLAGS_TEST_pause_tablet_compact_flush_ms;
    LOG_WITH_FUNC(INFO) << "Pausing flush tablets request for " << pause_ms << " ms";
    SleepFor(MonoDelta::FromMilliseconds(pause_ms));
    LOG_WITH_FUNC(INFO) << "Resuming flush tablets request";
  }

  if (!req->all_tablets() && req->tablet_ids_size() == 0) {
    const Status s = STATUS(InvalidArgument, "No tablet ids");
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }

  server::UpdateClock(*req, server_->Clock());

  TRACE_EVENT1("tserver", "FlushTablets",
               "TS: ", req->dest_uuid());

  LOG_WITH_PREFIX(INFO) << "Processing FlushTablets from " << context.requestor_string();
  VLOG_WITH_PREFIX(1) << "Full FlushTablets request: " << req->DebugString();
  TabletPeers tablet_peers;
  TSTabletManager::TabletPtrs tablet_ptrs;

  if (req->all_tablets()) {
    tablet_peers = server_->tablet_manager()->GetTabletPeers(&tablet_ptrs);
  } else {
    for (const TabletId& id : req->tablet_ids()) {
      auto tablet_peer = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
          server_->tablet_peer_lookup(), id, resp, &context));
      tablet_peers.push_back(std::move(tablet_peer.tablet_peer));
      auto tablet = tablet_peer.tablet;
      if (tablet != nullptr) {
        tablet_ptrs.push_back(std::move(tablet));
      }
    }
  }
  switch (req->operation()) {
    case FlushTabletsRequestPB::FLUSH: {
      RETURN_UNKNOWN_ERROR_IF_NOT_OK(
          TriggerFlush(*this, tablet_ptrs, *req, *resp),
          resp, &context);
      break;
    }
    case FlushTabletsRequestPB::COMPACT: {
      RETURN_UNKNOWN_ERROR_IF_NOT_OK(
          TriggerCompact(*server_->tablet_manager(), tablet_ptrs, *req),
          resp, &context);
      break;
    }
    case FlushTabletsRequestPB::LOG_GC:
      for (const auto& tablet : tablet_peers) {
        resp->set_failed_tablet_id(tablet->tablet_id());
        RETURN_UNKNOWN_ERROR_IF_NOT_OK(tablet->RunLogGC(req->rollover()), resp, &context);
        resp->clear_failed_tablet_id();
      }
      break;
  }

  context.RespondSuccess();
}

namespace {

TierMigrationStatusPB::State ToTierMigrationStatePB(tablet::TierMigrationStatus::State state) {
  switch (state) {
    case tablet::TierMigrationStatus::State::kNone:
      return TierMigrationStatusPB::NONE;
    case tablet::TierMigrationStatus::State::kInProgress:
      return TierMigrationStatusPB::IN_PROGRESS;
    case tablet::TierMigrationStatus::State::kDone:
      return TierMigrationStatusPB::DONE;
    case tablet::TierMigrationStatus::State::kFailed:
      return TierMigrationStatusPB::FAILED;
  }
  FATAL_INVALID_ENUM_VALUE(tablet::TierMigrationStatus::State, state);
}

void FillTierMigrationStatusPB(
    const tablet::TierMigrationStatus& status, TierMigrationStatusPB* pb) {
  pb->set_state(ToTierMigrationStatePB(status.state));
  pb->set_pass_in_flight(status.pass_in_flight);
  pb->set_files_total(status.files_total);
  pb->set_files_moved(status.files_moved);
  pb->set_files_failed(status.files_failed);
  pb->set_files_deferred(status.files_deferred);
  pb->set_obsoleted(status.obsoleted);
  pb->set_consecutive_failed_passes(status.consecutive_failed_passes);
  if (!status.last_error.ok()) {
    pb->set_last_error(status.last_error.ToString());
  }
}

// Tiered storage is a preview feature. Its admin RPCs are refused, not silently ignored, so a
// caller never believes a migration was started or reads tier info that nothing maintains.
template <class Resp>
bool CheckTieredStorageEnabledOrRespond(
    const char* method_name, Resp* resp, rpc::RpcContext* context) {
  if (FLAGS_enable_tiered_storage) {
    return true;
  }
  SetupErrorAndRespond(
      resp->mutable_error(),
      STATUS_FORMAT(
          NotSupported,
          "$0 requires tiered storage, which is disabled. Set the preview flag "
          "--enable_tiered_storage=true to use it.",
          method_name),
      context);
  return false;
}

}  // namespace

void TabletServiceAdminImpl::AlterTabletTier(
    const AlterTabletTierRequestPB* req,
    AlterTabletTierResponsePB* resp,
    rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "AlterTabletTier", req, resp,
                               &context)) {
    return;
  }
  if (!CheckTieredStorageEnabledOrRespond("AlterTabletTier", resp, &context)) {
    return;
  }

  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context));
  const auto& tablet = peer_tablet.tablet;
  if (!tablet) {
    SetupErrorAndRespond(
        resp->mutable_error(), STATUS(IllegalState, "Tablet not available"), &context);
    return;
  }

  LOG_WITH_PREFIX(INFO) << "Processing AlterTabletTier for tablet " << req->tablet_id()
                        << " -> tier " << req->target_tier() << " from "
                        << context.requestor_string();

  const auto meta = peer_tablet.tablet_peer->tablet_metadata();

  // Keep the disk already resolved for this tier when the tier is unchanged. Re-running the
  // least-loaded-disk policy could pick a different disk within the same tier purely because the
  // first call changed the load counts, forcing a pointless rewrite of every SST.
  Result<uint32_t> path_id = meta->target_storage_tier() == req->target_tier()
      ? server_->tablet_manager()->ResolveTargetTierPathId(meta)
      : server_->tablet_manager()->SelectPathIdForTier(
            *meta, meta->table_id(), req->target_tier());
  if (!path_id.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), path_id.status(), &context);
    return;
  }

  auto status = tablet->AlterTabletTier(req->target_tier(), *path_id);
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status.status(), &context);
    return;
  }

  FillTierMigrationStatusPB(*status, resp->mutable_migration());
  context.RespondSuccess();
}

void TabletServiceAdminImpl::GetTabletTierInfo(
    const GetTabletTierInfoRequestPB* req,
    GetTabletTierInfoResponsePB* resp,
    rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "GetTabletTierInfo", req, resp,
                               &context)) {
    return;
  }
  if (!CheckTieredStorageEnabledOrRespond("GetTabletTierInfo", resp, &context)) {
    return;
  }

  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context));
  const auto& tablet = peer_tablet.tablet;
  if (!tablet) {
    SetupErrorAndRespond(
        resp->mutable_error(), STATUS(IllegalState, "Tablet not available"), &context);
    return;
  }

  auto info = tablet->GetTierInfo();
  if (!info.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), info.status(), &context);
    return;
  }

  resp->set_tablet_id(req->tablet_id());
  if (!info->current_tier.empty()) {
    resp->set_current_tier(info->current_tier);
  }
  if (!info->target_tier.empty()) {
    resp->set_target_tier(info->target_tier);
    resp->set_target_tier_path_id(info->target_tier_path_id);
  }
  resp->set_wal_dir(info->wal_dir);
  resp->set_unmatched_sst_count(info->unmatched_sst_count);

  if (info->migration.state != tablet::TierMigrationStatus::State::kNone) {
    FillTierMigrationStatusPB(info->migration, resp->mutable_migration());
  }

  for (const auto& stats : info->tier_paths) {
    auto* tp_pb = resp->add_tier_paths();
    tp_pb->set_path_id(stats.path_id);
    tp_pb->set_tier(stats.tier);
    tp_pb->set_path(stats.path);
    tp_pb->set_is_home(stats.is_home);
    tp_pb->set_sst_count(stats.sst_count);
    tp_pb->set_total_bytes(stats.total_bytes);
  }

  context.RespondSuccess();
}

void TabletServiceAdminImpl::CountIntents(
    const CountIntentsRequestPB* req,
    CountIntentsResponsePB* resp,
    rpc::RpcContext context) {
  TSTabletManager::TabletPtrs tablet_ptrs;
  TabletPeers tablet_peers = server_->tablet_manager()->GetTabletPeers(&tablet_ptrs);
  int64_t total_intents = 0;
  // TODO: do this in parallel.
  // TODO: per-tablet intent counts.
  for (const auto& tablet : tablet_ptrs) {
    auto num_intents = tablet->CountIntents();
    if (!num_intents.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), num_intents.status(), &context);
      return;
    }
    total_intents += *num_intents;
  }
  resp->set_num_intents(total_intents);
  context.RespondSuccess();
}

void TabletServiceAdminImpl::AddTableToTablet(
    const AddTableToTabletRequestPB* req, AddTableToTabletResponsePB* resp,
    rpc::RpcContext context) {
  auto tablet_id = req->tablet_id();

  const auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), tablet_id, resp, &context);
  if (!tablet) {
    return;
  }
  DVLOG(3) << "Received AddTableToTablet RPC: " << yb::ToString(*req);

  tablet::ChangeMetadataRequestPB change_req;
  *change_req.mutable_add_table() = req->add_table();
  change_req.set_tablet_id(tablet_id);
  Status s = tablet::SyncReplicateChangeMetadataOperation(
      &change_req, tablet.peer.get(), tablet.leader_term);
  if (PREDICT_FALSE(!s.ok())) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceAdminImpl::RemoveTableFromTablet(
    const RemoveTableFromTabletRequestPB* req,
    RemoveTableFromTabletResponsePB* resp,
    rpc::RpcContext context) {
  auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  tablet::ChangeMetadataRequestPB change_req;
  change_req.set_remove_table_id(req->remove_table_id());
  change_req.set_tablet_id(req->tablet_id());
  Status s = tablet::SyncReplicateChangeMetadataOperation(
      &change_req, tablet.peer.get(), tablet.leader_term);
  if (PREDICT_FALSE(!s.ok())) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceAdminImpl::SplitTablet(
    const tablet::SplitTabletRequestPB* req, SplitTabletResponsePB* resp, rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "SplitTablet", req, resp, &context)) {
    return;
  }
  if (PREDICT_FALSE(FLAGS_TEST_fail_tablet_split_probability > 0) &&
      RandomActWithProbability(FLAGS_TEST_fail_tablet_split_probability)) {
    return SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS(InvalidArgument,  // Use InvalidArgument to hit IsDefinitelyPermanentError().
            "Failing tablet split due to FLAGS_TEST_fail_tablet_split_probability"),
        TabletServerErrorPB::UNKNOWN_ERROR,
        &context);
  }
  PerformAtLeader(req, resp, &context, server_, "SplitTablet",
      [req, resp, &context, this](const LeaderTabletPeer& leader_tablet_peer) -> Status {
    const auto consensus_result = leader_tablet_peer.peer->GetConsensus();
    if (!consensus_result) {
      return consensus_result.status().CloneAndAddErrorCode(
          TabletServerError(TabletServerErrorPB::TABLET_NOT_RUNNING));
    }

    auto tablet_data_state = leader_tablet_peer.peer->data_state();
    if (tablet_data_state != tablet::TABLET_DATA_READY) {
      auto status = tablet_data_state == tablet::TABLET_DATA_SPLIT_COMPLETED
          ? STATUS_FORMAT(AlreadyPresent, "Tablet $0 is already split.", req->tablet_id())
          : STATUS_FORMAT(InvalidArgument, "Invalid tablet $0 data state: $1", req->tablet_id(),
                          tablet_data_state);
      return status.CloneAndAddErrorCode(
          TabletServerError(TabletServerErrorPB::TABLET_NOT_RUNNING));
    }

    auto operation = std::make_unique<tablet::SplitOperation>(
        leader_tablet_peer.tablet, server_->tablet_manager());
    *operation->AllocateRequest() = *req;
    operation->mutable_request()->dup_split_parent_leader_uuid(
        leader_tablet_peer.peer->permanent_uuid());

    operation->set_completion_callback(
        MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));

    leader_tablet_peer.peer->Submit(std::move(operation), leader_tablet_peer.leader_term);
    ts_split_op_added_->Increment();
    LOG(INFO) << leader_tablet_peer.peer->LogPrefix() << "RPC for split tablet successful. "
        << "Submitting request to " << leader_tablet_peer.peer->tablet_id()
        << " term " << leader_tablet_peer.leader_term;
    return Status::OK();
  });
}

void TabletServiceAdminImpl::CloneTablet(
    const CloneTabletRequestPB* req, CloneTabletResponsePB* resp, rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "CloneTablet", req, resp, &context)) {
    return;
  }
  PerformAtLeader(req, resp, &context, server_, "CloneTablet",
      [req, resp, &context, this](const LeaderTabletPeer& leader_tablet_peer) -> Status {
    LOG_WITH_FUNC(INFO) << "CloneTablet, req: " << AsString(*req);
    const auto consensus_result = leader_tablet_peer.peer->GetConsensus();
    if (!consensus_result) {
      return consensus_result.status().CloneAndAddErrorCode(
          TabletServerError(TabletServerErrorPB::TABLET_NOT_RUNNING));
    }

    auto operation = std::make_unique<tablet::CloneOperation>(
      leader_tablet_peer.tablet, server_->tablet_manager());
    operation->AllocateRequest()->CopyFrom(*req);
    operation->set_completion_callback(
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));

    leader_tablet_peer.peer->Submit(std::move(operation), leader_tablet_peer.leader_term);
    LOG(INFO) << leader_tablet_peer.peer->LogPrefix() << "RPC for clone tablet successful. "
        << "Submitted request to " << leader_tablet_peer.peer->tablet_id()
        << " in term " << leader_tablet_peer.leader_term;
    return Status::OK();
  });
}

Result<HostPort> TabletServiceAdminImpl::GetLocalPgHostPort() {
  HostPort local_pg_host_port;
  if (!FLAGS_TEST_mini_cluster_pg_host_port.empty()) {
    RETURN_NOT_OK(local_pg_host_port.ParseString(
        FLAGS_TEST_mini_cluster_pg_host_port, pgwrapper::PgProcessConf::kDefaultPort));
  } else {
    local_pg_host_port = server_->pgsql_proxy_bind_address();
  }
  std::string unix_domain_socket = PgDeriveSocketDir(local_pg_host_port);
  return HostPort(unix_domain_socket, local_pg_host_port.port());
}

void TabletServiceAdminImpl::ClonePgSchema(
    const ClonePgSchemaRequestPB* req, ClonePgSchemaResponsePB* resp, rpc::RpcContext context) {
  LOG_WITH_FUNC(INFO) << "req: " << AsString(*req);
  auto status = DoClonePgSchema(req, resp);
  LOG_WITH_FUNC(INFO) << "resp: " << AsString(*resp) << ", status: " << status;
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
  } else {
    context.RespondSuccess();
  }
}

Status TabletServiceAdminImpl::DoClonePgSchema(
    const ClonePgSchemaRequestPB* req, ClonePgSchemaResponsePB* resp) {
  AtomicFlagSleepMs(&FLAGS_TEST_clone_pg_schema_delay_ms);

  // Run ysql_dump to generate the schema of the clone database as of restore time.
  const std::string& target_db_name = req->target_db_name();

  auto local_hostport = VERIFY_RESULT(GetLocalPgHostPort());
  YsqlDumpRunner ysql_dump_runner =
      VERIFY_RESULT(YsqlDumpRunner::GetYsqlDumpRunner(local_hostport));
  std::string dump_output = VERIFY_RESULT(ysql_dump_runner.RunAndModifyForClone(
      req->source_db_name(), target_db_name, req->target_owner(), HybridTime(req->restore_ht())));
  VLOG(2) << "ysql_dump output: " << dump_output;

  // Execute the sql script to generate the PG database.
  YsqlshRunner ysqlsh_runner = VERIFY_RESULT(YsqlshRunner::GetYsqlshRunner(local_hostport));
  auto result =
      ysqlsh_runner.ExecuteSqlScript(dump_output, "clone_pg_schema" /* tmp_file_prefix */);
  if (!result.ok()) {
    LOG(INFO) << "Failed executing ysql_dump output: " << dump_output;
    return result.status();
  }

  LOG(INFO) << Format(
      "Clone Pg Schema Objects for source database: $0 to clone database: $1 done successfully",
      req->source_db_name(), target_db_name);
  return Status::OK();
}

void TabletServiceAdminImpl::EnableDbConns(
    const EnableDbConnsRequestPB* req, EnableDbConnsResponsePB* resp,
    rpc::RpcContext context) {
  auto status = DoEnableDbConns(req, resp);
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
  } else {
    context.RespondSuccess();
  }
}

Status TabletServiceAdminImpl::DoEnableDbConns(
    const EnableDbConnsRequestPB* req, EnableDbConnsResponsePB* resp) {
  SCHECK(!FLAGS_TEST_fail_enable_db_conns, InternalError, "Failing EnableDbConns for test");

  const std::string script = Format(
      "ALTER DATABASE $0 ALLOW_CONNECTIONS true",
      pgwrapper::PqEscapeIdentifier(req->target_db_name()));

  auto local_hostport = VERIFY_RESULT(GetLocalPgHostPort());
  YsqlshRunner ysqlsh_runner =
      VERIFY_RESULT(YsqlshRunner::GetYsqlshRunner(HostPort::FromPB(local_hostport)));
  RETURN_NOT_OK(ysqlsh_runner.ExecuteSqlScript(script, "enable_connections" /* tmp_file_prefix */));

  LOG(INFO) << Format(
      "Successfully enabled connections to clone target database $0", req->target_db_name());
  return Status::OK();
}

void TabletServiceAdminImpl::UpgradeYsql(
    const UpgradeYsqlRequestPB* req,
    UpgradeYsqlResponsePB* resp,
    rpc::RpcContext context) {
  if (!FLAGS_enable_ysql) {
    LOG(INFO) << "YSQL is not enabled. Skipping YSQL upgrade.";
    context.RespondSuccess();
    return;
  }

  LOG(INFO) << "Starting YSQL upgrade";

  pgwrapper::YsqlUpgradeHelper upgrade_helper(server_->pgsql_proxy_bind_address(),
                                              server_->GetSharedMemoryPostgresAuthKey(),
                                              FLAGS_heartbeat_interval_ms,
                                              req->use_single_connection());
  const auto status = upgrade_helper.Upgrade();
  if (!status.ok()) {
    LOG(INFO) << "YSQL upgrade failed: " << status;
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
    return;
  }

  LOG(INFO) << "YSQL upgrade done successfully";
  context.RespondSuccess();
}

void TabletServiceAdminImpl::WaitForYsqlBackendsCatalogVersion(
    const WaitForYsqlBackendsCatalogVersionRequestPB* req,
    WaitForYsqlBackendsCatalogVersionResponsePB* resp,
    rpc::RpcContext context) {
  VLOG_WITH_PREFIX(2) << "Received Wait for YSQL Backends Catalog Version RPC: "
                      << req->ShortDebugString();

  TEST_PAUSE_IF_FLAG(TEST_pause_wait_for_ysql_backends_catalog_version);

  const PgOid database_oid = req->database_oid();
  const uint64_t catalog_version = req->catalog_version();
  const int prev_num_lagging_backends = req->prev_num_lagging_backends();
  if (prev_num_lagging_backends == 0 || prev_num_lagging_backends < -1) {
    // Send back OPERATION_NOT_SUPPORTED to prevent further retry.
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_FORMAT(InvalidArgument,
                      "Unexpected prev_num_lagging_backends: $0",
                      prev_num_lagging_backends)
          .CloneAndAddErrorCode(TabletServerError(TabletServerErrorPB::OPERATION_NOT_SUPPORTED)),
        &context);
    return;
  }

  const CoarseTimePoint& deadline = context.GetClientDeadline();
  // Reserve 1s for responding back to master.
  const auto modified_deadline = deadline - 1s;

  // First, check tserver's catalog version.
  const std::string db_ver_tag = Format("[DB $0, V $1]", database_oid, catalog_version);
  uint64_t ts_catalog_version = 0;
  SCOPED_WAIT_STATUS(WaitForYSQLBackendsCatalogVersion);
  bool first_run = true;
  Status s = Wait(
      [catalog_version, database_oid, this, &ts_catalog_version, &first_run]() -> Result<bool> {
        server_->get_ysql_db_catalog_version(
            database_oid, &ts_catalog_version, nullptr /* last_breaking_catalog_version */);
        if (ts_catalog_version >= catalog_version) {
          return true;
        }
        if (first_run) {
          first_run = false;
          server_->heartbeater()->TriggerASAP();
        }
        return false;
      },
      modified_deadline,
      Format("Wait for tserver catalog version to reach $0", db_ver_tag));
  if (!s.ok()) {
    DCHECK(s.IsTimedOut());
    const std::string ts_db_ver_tag = Format("[DB $0, V $1]", database_oid, ts_catalog_version);
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_FORMAT(TryAgain,
                      "Tserver's catalog version is too old: $0 vs $1",
                      ts_db_ver_tag, db_ver_tag),
        &context);
    return;
  }

  // Second, check backends' catalog version.
  // Use template1 database because it is guaranteed to exist.
  // TODO(jason): use database related to database we are waiting on to not overload template1.
  // TODO(jason): come up with a more efficient connection reuse method for tserver-postgres
  // communication.  As of D19621, connections are spawned each request for YSQL upgrade, index
  // backfill, and this.  Creating the connection has a startup cost.
  auto res = server_->CreateInternalPGConn(
      "template1", kDefaultInternalPgUser, /*simple_query_protocol=*/false, modified_deadline);
  if (!res.ok()) {
    LOG_WITH_PREFIX_AND_FUNC(WARNING) << "failed to connect to local postgres: " << res.status();
    SetupErrorAndRespond(resp->mutable_error(), res.status(), &context);
    return;
  }
  pgwrapper::PGConn conn = std::move(*res);

  // Note: keep this query in sync with the errhint query (YbWaitForBackendsCatalogVersion at the
  // time of writing).
  //
  // TODO(Vaibhav): We will need to exempt the walreceiver and walwriter processes from this query
  // once we start supporting the pub-sub model in logical replication.
  //
  // TODO(jason): handle or create issue for catalog version being uint64 vs int64.
  const std::string num_lagging_backends_query = Format(
      "SELECT count(*) FROM pg_stat_activity WHERE"
      " backend_type != 'walsender' AND backend_type != 'yb-conn-mgr walsender'"
      " AND backend_type != 'yb auto analyze backend'"
      " AND backend_type != 'yb index backfill'"
      " AND backend_type != 'yb matview refresh'"
      " AND catalog_version < $0 AND datid = $1$2",
      catalog_version, database_oid,
      (req->has_requestor_pg_backend_pid() ?
       Format(" AND pid != $0", req->requestor_pg_backend_pid()) :
       ""));
  int num_lagging_backends = -1;
  const std::string description = Format("Wait for update to num lagging backends $0", db_ver_tag);
  s = Wait(
      [&]() -> Result<bool> {
        num_lagging_backends = narrow_cast<int>(VERIFY_RESULT(
            conn.FetchRow<pgwrapper::PGUint64>(num_lagging_backends_query)));
        if (num_lagging_backends != prev_num_lagging_backends) {
          SCHECK((prev_num_lagging_backends == -1 ||
                  prev_num_lagging_backends > num_lagging_backends),
                 InternalError,
                 Format("Unexpected prev_num_lagging_backends: $0, num_lagging_backends: $1",
                        prev_num_lagging_backends, num_lagging_backends));
          return true;
        }
        VLOG_WITH_PREFIX(4) << "Some backends are behind: " << num_lagging_backends;
        return false;
      },
      modified_deadline,
      description,
      // Start with a small delay even on retries (prev_num_lagging_backends != -1): a flat delay
      // would report backends catching up that much later, adding the same latency to DDLs waiting
      // on this.
      10ms /* initial_delay */,
      1.4 /* delay_multiplier */,
      5s /* max_delay */);

  if (s.IsTimedOut() && s.message().ToBuffer().find(description) != std::string::npos) {
    LOG_WITH_PREFIX(INFO) << "Deadline reached: still waiting on " << num_lagging_backends
                          << " backends " << db_ver_tag;
  } else if (!s.ok()) {
    LOG_WITH_PREFIX_AND_FUNC(WARNING) << "num lagging backends query failed: " << s;
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }
  DCHECK_GE(num_lagging_backends, 0);
  resp->set_num_lagging_backends(num_lagging_backends);
  context.RespondSuccess();
}

void TabletServiceAdminImpl::UpdateTransactionTablesVersion(
    const UpdateTransactionTablesVersionRequestPB* req,
    UpdateTransactionTablesVersionResponsePB* resp,
    rpc::RpcContext context) {
  LOG(INFO) << "Received update in transaction tables version to version " << req->version();

  auto context_ptr = std::make_shared<rpc::RpcContext>(std::move(context));
  auto callback = [resp, context_ptr](const Status& status) {
    if (!status.ok()) {
      LOG(WARNING) << status;
      SetupErrorAndRespond(resp->mutable_error(), status, context_ptr.get());
    } else {
      context_ptr->RespondSuccess();
    }
  };

  server_->TransactionManager().UpdateTransactionTablesVersion(req->version(), callback);
}

void TabletServiceAdminImpl::ApplyXClusterGuardedInfoIfNewer(
    const ApplyXClusterGuardedInfoIfNewerRequestPB* req,
    ApplyXClusterGuardedInfoIfNewerResponsePB* resp, rpc::RpcContext context) {
  VLOG(2) << "Received xCluster-guarded info with version "
          << req->xcluster_guarded_info().xcluster_guarded_info_version().ShortDebugString();
  server_->ApplyXClusterGuardedInfoIfNewer(req->xcluster_guarded_info());
  context.RespondSuccess();
}

void TabletServiceAdminImpl::GetPgSocketDir(
    const GetPgSocketDirRequestPB* req, GetPgSocketDirResponsePB* resp, rpc::RpcContext context) {
  auto result = GetLocalPgHostPort();
  if (!result.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), result.status(), &context);
    return;
  }
  HostPortToPB(*result, resp->mutable_pg_socket_dir());
  context.RespondSuccess();
}

bool EmptyWriteBatch(const docdb::KeyValueWriteBatchMsg& write_batch) {
  return write_batch.write_pairs().empty() && write_batch.apply_external_transactions().empty();
}

Status TabletServiceImpl::PerformWrite(
    const WriteRequestMsg* req, WriteResponseMsg* resp, rpc::RpcContext* context) {
  if (req->include_trace()) {
    context->EnsureTraceCreated();
  }
  ADOPT_TRACE(context->trace());
  TRACE("Start Write");
  TRACE_EVENT1("tserver", "TabletServiceImpl::Write", "tablet_id", std::string(req->tablet_id()));
  VLOG(2) << "Received Write RPC: " << AsString(*req);

  UpdateClock(*req, server_->Clock());
  TEST_SYNC_POINT_CALLBACK("TabletServiceImpl::PerformWrite", const_cast<WriteRequestMsg*>(req));

  auto tablet =
      VERIFY_RESULT(LookupLeaderTablet(server_->tablet_peer_lookup(), req->tablet_id(), resp));
  RETURN_NOT_OK(CheckWriteThrottling(req->rejection_score(), tablet.peer.get()));

  if (FLAGS_ysql_debug_log_write_requests) {
    WARN_NOT_OK(
        PrintYSQLWriteRequest(*req, *context, *tablet.tablet),
        "Failed to print YSQL write request");
  }

  if (tablet.tablet->metadata()->hidden()) {
    return STATUS(
        NotFound, "Tablet not found", req->tablet_id(),
        TabletServerError(TabletServerErrorPB::TABLET_NOT_FOUND));
  }

  CheckPersistenceStampPresent(*req);
  if (req->has_origination_info()) {
    RETURN_NOT_OK(CheckPersistenceTestRejection(req->origination_info()));
  }

  if (PREDICT_FALSE(req->has_write_batch() && !req->has_external_hybrid_time() &&
      (!req->write_batch().write_pairs().empty() || !req->write_batch().read_pairs().empty()))) {
    return STATUS(
        NotSupported, "Write Request contains write batch. This field should be "
        "used only for post-processed write requests during Raft replication.",
        TabletServerError(TabletServerErrorPB::INVALID_MUTATION));
  }

  bool has_operations = req->ql_write_batch_size() != 0 ||
                        req->redis_write_batch_size() != 0 ||
                        req->pgsql_write_batch_size() != 0 ||
                        req->pgsql_lock_batch_size() != 0 ||
                        (req->has_external_hybrid_time() && !EmptyWriteBatch(req->write_batch()));
  if (!has_operations && tablet.tablet->table_type() != TableType::REDIS_TABLE_TYPE) {
    // An empty request. This is fine, can just exit early with ok status instead of working hard.
    // This doesn't need to go to Raft log.
    FillTabletConsensusInfoIfRequestOpIdStale(tablet.peer, req, resp);
    MakeRpcOperationCompletionCallback(std::move(*context), resp, server_->Clock())(Status::OK());
    return Status::OK();
  }

  SCHECK(
      tablet.peer->HasSufficientDiskSpaceForWrite(), IOError,
      "Write to tablet $0 rejected. Node $1 has insufficient disk space", req->tablet_id(),
      tablet.peer->tablet_metadata()->fs_manager()->uuid());

  // For postgres requests:
  // 1. For non-system catalog tablets: check that the request has a catalog version higher
  //    than the breaking version.
  // 2. For system catalog writes: check the the request has the latest catalog version.
  const bool is_pgsql_user_table_write =
      (tablet.tablet->table_type() == TableType::PGSQL_TABLE_TYPE);
  // Postgres write requests to the system catalog tablet have the type YQL_TABLE_TYPE instead of
  // PGSQL_TABLE_TYPE, so we detect them via is_sys_catalog() instead.
  const bool is_sys_catalog_write = !is_pgsql_user_table_write && tablet.tablet->is_sys_catalog();

  const bool perform_catalog_version_check =
      is_pgsql_user_table_write ||
      (is_sys_catalog_write && FLAGS_yb_fail_catalog_write_on_catalog_version_mismatch);
  if (perform_catalog_version_check) {
    // We want to ensure that a DDL doesn't perform writes to the system catalog based off a stale
    // catalog cache to avoid issues such as #27597. So for system catalog writes we read the
    // authoritative catalog version (use_cache=false).
    CatalogVersionChecker catalog_version_checker(*server_, !is_sys_catalog_write /* use_cache */);
    for (const auto& pg_req : req->pgsql_write_batch()) {
      RETURN_NOT_OK(catalog_version_checker(
          pg_req, is_sys_catalog_write /* fail_for_non_breaking_version_change */));
    }
  }

  auto context_ptr = std::make_shared<RpcContext>(std::move(*context));

  auto leader_term = tablet.leader_term;
  if (req->has_leader_term() && req->leader_term() != OpId::kUnknownTerm) {
    leader_term = req->leader_term();
    if (leader_term != tablet.leader_term) {
      auto status =
          STATUS_FORMAT(InvalidArgument, "Tablet $0 Leader term changed", req->tablet_id());
      SetupErrorAndRespond(resp->mutable_error(), std::move(status), context_ptr.get());
      return Status::OK();
    }
  }

  auto query = std::make_unique<tablet::WriteQuery>(
      leader_term, context_ptr->GetClientDeadline(), tablet.peer.get(), tablet.tablet,
      context_ptr.get(), resp);
  query->set_client_request(*req);

  if (RandomActWithProbability(FLAGS_TEST_respond_write_failed_probability)) {
    LOG(INFO) << "Responding with a failure to " << req->ShortDebugString();
    tablet.peer->WriteAsync(std::move(query));
    auto status = STATUS(LeaderHasNoLease, "TEST: Random failure");
    SetupErrorAndRespond(resp->mutable_error(), std::move(status), context_ptr.get());
    return Status::OK();
  }

  if (RandomActWithProbability(FLAGS_TEST_respond_write_with_abort_probability)) {
    LOG(INFO) << "Responding with transaction aborted failure to " << req->ShortDebugString();
    SetupErrorAndRespond(resp->mutable_error(),
        CreateExpiredStatus("Transaction expired or aborted by a conflict"), context_ptr.get());
    return Status::OK();
  }

  FillTabletConsensusInfoIfRequestOpIdStale(tablet.peer, req, resp);
  query->set_callback(WriteQueryCompletionCallback(
      tablet.peer, context_ptr, resp, query.get(), server_->Clock(), req->include_trace(),
      req->has_leader_term()));

  query->AdjustYsqlQueryTransactionality(req->pgsql_write_batch_size());

  tablet.peer->WriteAsync(std::move(query));

  return Status::OK();
}

void TabletServiceImpl::Write(const WriteRequestMsg* req,
                              WriteResponseMsg* resp,
                              rpc::RpcContext context) {
  if (FLAGS_TEST_tserver_noop_read_write) {
    for ([[maybe_unused]] const auto& batch : req->ql_write_batch()) {
      resp->add_ql_response_batch();
    }
    context.RespondSuccess();
    return;
  }

  auto status = PerformWrite(req, resp, &context);
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), std::move(status), &context);
  }
}

void TabletServiceImpl::WaitForAsyncWrite(
    const WaitForAsyncWriteRequestPB* req, WaitForAsyncWriteResponsePB* resp,
    rpc::RpcContext context) {
  auto callback = [op_id = OpId::FromPB(req->op_id()), resp,
                   context_ptr = std::make_shared<rpc::RpcContext>(std::move(context))](
                      const Status& status) {
    if (!status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), status, context_ptr.get());
      return;
    }
    TEST_SYNC_POINT_CALLBACK(
        "TabletServiceImpl::WaitForAsyncWrite::Verified", const_cast<OpId*>(&op_id));
    context_ptr->RespondSuccess();
  };

  // Don't need to do a leader check here: a follower or post-split parent can still verify writes
  // from its log.
  auto tablet_result = LookupTabletPeer(server_->tablet_peer_lookup(), req->tablet_id());
  if (!tablet_result) {
    callback(tablet_result.status());
    return;
  }

  DEBUG_ONLY_TEST_SYNC_POINT("TabletServiceImpl::WaitForAsyncWrite::BeforeRegister");
  ASH_ENABLE_CONCURRENT_UPDATES();
  SET_WAIT_STATUS(Raft_WaitingForPipelinedReplication);
  tablet_result->tablet_peer->RegisterAsyncWriteCompletion(
      OpId::FromPB(req->op_id()), std::move(callback));
}

void TabletServiceImpl::Read(const ReadRequestMsg* req,
                             ReadResponseMsg* resp,
                             rpc::RpcContext context) {
#ifndef NDEBUG
  if (PREDICT_FALSE(FLAGS_TEST_wait_row_mark_exclusive_count > 0)) {
    for (const auto& pgsql_req : req->pgsql_batch()) {
      if (pgsql_req.has_row_mark_type() &&
          pgsql_req.row_mark_type() == RowMarkType::ROW_MARK_EXCLUSIVE) {
        static CountDownLatch row_mark_exclusive_latch(FLAGS_TEST_wait_row_mark_exclusive_count);
        row_mark_exclusive_latch.CountDown();
        row_mark_exclusive_latch.Wait();
        DEBUG_ONLY_TEST_SYNC_POINT("TabletServiceImpl::Read::RowMarkExclusive:1");
        DEBUG_ONLY_TEST_SYNC_POINT("TabletServiceImpl::Read::RowMarkExclusive:2");
        break;
      }
    }
  }
#endif // NDEBUG

  if (FLAGS_TEST_tserver_noop_read_write) {
    LOG(INFO) << "returning do tue no op read_write";
    context.RespondSuccess();
    return;
  }

  VLOG(5) << "Read req: " << req->ShortDebugString();
  PerformRead(server_, this, req, resp, std::move(context));
}

ConsensusServiceImpl::ConsensusServiceImpl(const scoped_refptr<MetricEntity>& metric_entity,
                                           TabletPeerLookupIf* tablet_manager)
    : ConsensusServiceIf(metric_entity),
      tablet_manager_(tablet_manager) {
}

ConsensusServiceImpl::~ConsensusServiceImpl() {
}

void ConsensusServiceImpl::CompleteUpdateConsensusResponse(
    std::shared_ptr<tablet::TabletPeer> tablet_peer,
    consensus::LWConsensusResponsePB* resp) {
  auto tablet = tablet_peer->shared_tablet_maybe_null();
  if (tablet) {
    resp->set_num_sst_files(tablet->GetCurrentVersionNumSSTFiles());
  }
  resp->set_propagated_hybrid_time(tablet_peer->clock().Now().ToUint64());
}

void ConsensusServiceImpl::MultiRaftUpdateConsensus(
      const consensus::MultiRaftConsensusRequestPB *req,
      consensus::MultiRaftConsensusResponsePB *resp,
      rpc::RpcContext context) {
    DVLOG(3) << "Received Batch Consensus Update RPC: " << req->ShortDebugString();
    // Effectively performs ConsensusServiceImpl::UpdateConsensus for
    // each ConsensusRequestPB in the batch but does not fail the entire
    // batch if a single request fails.
    for (int i = 0; i < req->consensus_request_size(); i++) {
      // Unfortunately, we have to use const_cast here,
      // because the protobuf-generated interface only gives us a const request
      // but we need to be able to move messages out of the request for efficiency.
      auto consensus_req = const_cast<ConsensusRequestPB*>(&req->consensus_request(i));
      auto consensus_resp = resp->add_consensus_response();;

      auto uuid_match_res = CheckUuidMatch(tablet_manager_, "UpdateConsensus", consensus_req,
                                           context.requestor_string());
      if (!uuid_match_res.ok()) {
        SetupError(consensus_resp->mutable_error(), uuid_match_res.status());
        continue;
      }

      auto peer_tablet_res = LookupTabletPeer(tablet_manager_, consensus_req->tablet_id());
      if (!peer_tablet_res.ok()) {
        SetupError(consensus_resp->mutable_error(), peer_tablet_res.status());
        continue;
      }
      auto tablet_peer = peer_tablet_res.get().tablet_peer;

      // Submit the update directly to the TabletPeer's Consensus instance.
      auto consensus_res = GetConsensus(tablet_peer);
      if (!consensus_res.ok()) {
        SetupError(consensus_resp->mutable_error(), consensus_res.status());
        continue;
      }
      auto consensus = *consensus_res;

      // TODO(lw_uc) effective update for multiraft.
      auto temp_resp = rpc::MakeSharedMessage<consensus::LWConsensusResponsePB>();
      Status s = consensus->Update(
         rpc::CopySharedMessage(*consensus_req),
         temp_resp.get(), context.GetClientDeadline());
      if (PREDICT_FALSE(!s.ok())) {
        // Clear the response first, since a partially-filled response could
        // result in confusing a caller, or in having missing required fields
        // in embedded optional messages.
        consensus_resp->Clear();
        SetupError(consensus_resp->mutable_error(), s);
        continue;
      }

      CompleteUpdateConsensusResponse(tablet_peer, temp_resp.get());
      temp_resp->ToGoogleProtobuf(consensus_resp);
    }
    context.RespondSuccess();
}

void ConsensusServiceImpl::UpdateConsensus(const consensus::LWConsensusRequestPB* req,
                                           consensus::LWConsensusResponsePB* resp,
                                           rpc::RpcContext context) {
  DVLOG(3) << "Received Consensus Update RPC: " << req->ShortDebugString();
  if (!CheckUuidMatchOrRespond(tablet_manager_, "UpdateConsensus", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  // Submit the update directly to the TabletPeer's Consensus instance.
  shared_ptr<Consensus> consensus;
  if (!GetConsensusOrRespond(tablet_peer, resp, &context, &consensus)) return;

  // Unfortunately, we have to use const_cast here, because the protobuf-generated interface only
  // gives us a const request, but we need to be able to move messages out of the request for
  // efficiency.
  Status s = consensus->Update(
      SharedField(context.shared_params(), const_cast<consensus::LWConsensusRequestPB*>(req)),
      resp, context.GetClientDeadline());
  if (PREDICT_FALSE(!s.ok())) {
    // Clear the response first, since a partially-filled response could
    // result in confusing a caller, or in having missing required fields
    // in embedded optional messages.
    resp->Clear();

    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }

  CompleteUpdateConsensusResponse(tablet_peer, resp);

  auto trace = Trace::CurrentTrace();
  if (trace && req->trace_requested()) {
    resp->dup_trace_buffer(trace->DumpToString(true));
  }
  context.RespondSuccess();
}

void ConsensusServiceImpl::RequestConsensusVote(const VoteRequestPB* req,
                                                VoteResponsePB* resp,
                                                rpc::RpcContext context) {
  DVLOG(3) << "Received Consensus Request Vote RPC: " << req->DebugString();
  if (!CheckUuidMatchOrRespond(tablet_manager_, "RequestConsensusVote", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  // Submit the vote request directly to the consensus instance.
  shared_ptr<Consensus> consensus;
  if (!GetConsensusOrRespond(tablet_peer, resp, &context, &consensus)) return;
  Status s = consensus->RequestVote(req, resp);
  RETURN_UNKNOWN_ERROR_IF_NOT_OK(s, resp, &context);
  context.RespondSuccess();
}

void ConsensusServiceImpl::ChangeConfig(const ChangeConfigRequestPB* req,
                                        ChangeConfigResponsePB* resp,
                                        RpcContext context) {
  VLOG(1) << "Received ChangeConfig RPC: " << req->ShortDebugString();
  // If the destination uuid is empty string, it means the client was retrying after a leader
  // stepdown and did not have a chance to update the uuid inside the request.
  // TODO: Note that this can be removed once Java YBClient will reset change config's uuid
  // correctly after leader step down.
  if (req->dest_uuid() != "" &&
      !CheckUuidMatchOrRespond(tablet_manager_, "ChangeConfig", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  shared_ptr<Consensus> consensus;
  if (!GetConsensusOrRespond(tablet_peer, resp, &context, &consensus)) return;
  std::optional<TabletServerErrorPB::Code> error_code;
  std::shared_ptr<RpcContext> context_ptr = std::make_shared<RpcContext>(std::move(context));
  Status s = consensus->ChangeConfig(*req, BindHandleResponse(resp, context_ptr), &error_code);
  VLOG(1) << "Sent ChangeConfig req " << req->ShortDebugString() << " to consensus layer.";
  if (PREDICT_FALSE(!s.ok())) {
    HandleErrorResponse(resp, context_ptr.get(), s, error_code);
    return;
  }
  // The success case is handled when the callback fires.
}

void ConsensusServiceImpl::UnsafeChangeConfig(const UnsafeChangeConfigRequestPB* req,
                                              UnsafeChangeConfigResponsePB* resp,
                                              RpcContext context) {
  VLOG(1) << "Received UnsafeChangeConfig RPC: " << req->ShortDebugString();
  if (!CheckUuidMatchOrRespond(tablet_manager_, "UnsafeChangeConfig", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  shared_ptr<Consensus> consensus;
  if (!GetConsensusOrRespond(tablet_peer, resp, &context, &consensus)) {
    return;
  }
  std::optional<TabletServerErrorPB::Code> error_code;
  const Status s = consensus->UnsafeChangeConfig(*req, &error_code);
  if (PREDICT_FALSE(!s.ok())) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    HandleErrorResponse(resp, &context, s, error_code);
    return;
  }
  context.RespondSuccess();
}

void ConsensusServiceImpl::GetNodeInstance(const GetNodeInstanceRequestPB* req,
                                           GetNodeInstanceResponsePB* resp,
                                           rpc::RpcContext context) {
  DVLOG(3) << "Received Get Node Instance RPC: " << req->DebugString();
  resp->mutable_node_instance()->CopyFrom(tablet_manager_->NodeInstance());
  auto status = tablet_manager_->GetRegistration(resp->mutable_registration());
  if (!status.ok()) {
    context.RespondFailure(status);
  } else {
    context.RespondSuccess();
  }
}

namespace {

class NODISCARD_CLASS RpcScope {
 public:
  template<class Req, class Resp>
  RpcScope(TabletPeerLookupIf* tablet_manager,
           const char* method_name,
           const Req* req,
           Resp* resp,
           rpc::RpcContext* context)
      : context_(context) {
    if (!CheckUuidMatchOrRespond(tablet_manager, method_name, req, resp, context)) {
      return;
    }
    auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
        tablet_manager, req->tablet_id(), resp, context));
    auto tablet_peer = peer_tablet.tablet_peer;

    if (!GetConsensusOrRespond(tablet_peer, resp, context, &consensus_)) {
      return;
    }
    responded_ = false;
  }

  ~RpcScope() {
    if (!responded_) {
      context_->RespondSuccess();
    }
  }

  template<class Resp>
  void CheckStatus(const Status& status, Resp* resp) {
    if (!status.ok()) {
      LOG(INFO) << "Status failed: " << status.ToString();
      SetupErrorAndRespond(resp->mutable_error(), status, context_);
      responded_ = true;
    }
  }

  Consensus* operator->() {
    return consensus_.get();
  }

  explicit operator bool() const {
    return !responded_;
  }

 private:
  rpc::RpcContext* context_;
  bool responded_ = true;
  shared_ptr<Consensus> consensus_;
};

} // namespace

void ConsensusServiceImpl::RunLeaderElection(const RunLeaderElectionRequestPB* req,
                                             RunLeaderElectionResponsePB* resp,
                                             rpc::RpcContext context) {
  VLOG(1) << "Received Run Leader Election RPC: " << req->DebugString();
  RpcScope scope(tablet_manager_, "RunLeaderElection", req, resp, &context);
  if (!scope) {
    return;
  }

  Status s = scope->StartElection(consensus::LeaderElectionData {
    .mode = consensus::ElectionMode::ELECT_EVEN_IF_LEADER_IS_ALIVE,
    .pending_commit = req->has_committed_index(),
    .must_be_committed_opid = OpId::FromPB(req->committed_index()),
    .originator_uuid = req->has_originator_uuid() ? req->originator_uuid() : std::string(),
    .suppress_vote_request = consensus::TEST_SuppressVoteRequest(req->suppress_vote_request()),
    .initial_election = req->initial_election() });
  scope.CheckStatus(s, resp);
}

void ConsensusServiceImpl::LeaderElectionLost(const consensus::LeaderElectionLostRequestPB *req,
                                              consensus::LeaderElectionLostResponsePB *resp,
                                              ::yb::rpc::RpcContext context) {
  LOG(INFO) << "LeaderElectionLost, req: " << req->ShortDebugString();
  RpcScope scope(tablet_manager_, "LeaderElectionLost", req, resp, &context);
  if (!scope) {
    return;
  }
  auto status = scope->ElectionLostByProtege(req->election_lost_by_uuid());
  scope.CheckStatus(status, resp);
  LOG(INFO) << "LeaderElectionLost, outcome: " << (scope ? "success" : "failure") << "req: "
            << req->ShortDebugString();
}

void ConsensusServiceImpl::LeaderStepDown(const LeaderStepDownRequestPB* req,
                                          LeaderStepDownResponsePB* resp,
                                          RpcContext context) {
  LOG(INFO) << "Received Leader stepdown RPC: " << req->ShortDebugString();

  if (PREDICT_FALSE(FLAGS_TEST_leader_stepdown_delay_ms > 0)) {
    LOG(INFO) << "Delaying leader stepdown for "
              << FLAGS_TEST_leader_stepdown_delay_ms << " ms.";
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_leader_stepdown_delay_ms));
  }

  RpcScope scope(tablet_manager_, "LeaderStepDown", req, resp, &context);
  if (!scope) {
    return;
  }
  Status s = scope->StepDown(req, resp);
  if (resp->has_error()) {
    LOG(INFO) << "Leader stepdown request " << req->ShortDebugString() << " failed. Resp code="
              << TabletServerErrorPB::Code_Name(resp->error().code());
  }
  scope.CheckStatus(s, resp);
}

void ConsensusServiceImpl::GetLastOpId(const consensus::GetLastOpIdRequestPB *req,
                                       consensus::GetLastOpIdResponsePB *resp,
                                       rpc::RpcContext context) {
  DVLOG(3) << "Received GetLastOpId RPC: " << req->DebugString();

  if (PREDICT_FALSE(req->opid_type() == consensus::UNKNOWN_OPID_TYPE)) {
    HandleErrorResponse(resp, &context,
                        STATUS(InvalidArgument, "Invalid opid_type specified to GetLastOpId()"));
    return;
  }

  if (!CheckUuidMatchOrRespond(tablet_manager_, "GetLastOpId", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  if (tablet_peer->state() != tablet::RUNNING) {
    SetupErrorAndRespond(resp->mutable_error(),
                         STATUS(ServiceUnavailable, "Tablet Peer not in RUNNING state"),
                         TabletServerErrorPB::TABLET_NOT_RUNNING, &context);
    return;
  }

  auto consensus = GetConsensusOrRespond(tablet_peer, resp, &context);
  if (!consensus) return;
  auto op_id = req->has_op_type()
      ? consensus->TEST_GetLastOpIdWithType(req->opid_type(), req->op_type())
      : consensus->GetLastOpId(req->opid_type());

  // RETURN_UNKNOWN_ERROR_IF_NOT_OK does not support Result, so have to add extra check here.
  if (!op_id.ok()) {
    RETURN_UNKNOWN_ERROR_IF_NOT_OK(op_id.status(), resp, &context);
  }
  op_id->ToPB(resp->mutable_opid());
  context.RespondSuccess();
}

void ConsensusServiceImpl::GetConsensusState(const consensus::GetConsensusStateRequestPB *req,
                                             consensus::GetConsensusStateResponsePB *resp,
                                             rpc::RpcContext context) {
  DVLOG(3) << "Received GetConsensusState RPC: " << req->DebugString();

  RpcScope scope(tablet_manager_, "GetConsensusState", req, resp, &context);
  if (!scope) {
    return;
  }
  ConsensusConfigType type = req->type();
  if (PREDICT_FALSE(type != CONSENSUS_CONFIG_ACTIVE && type != CONSENSUS_CONFIG_COMMITTED)) {
    HandleErrorResponse(resp, &context,
        STATUS(InvalidArgument, Format("Unsupported ConsensusConfigType $0 ($1)",
                                       ConsensusConfigType_Name(type), type)));
    return;
  }
  LeaderLeaseStatus leader_lease_status;
  *resp->mutable_cstate() = scope->ConsensusState(req->type(), &leader_lease_status);
  resp->set_leader_lease_status(leader_lease_status);
}

void ConsensusServiceImpl::StartRemoteBootstrap(const StartRemoteBootstrapRequestPB* req,
                                                StartRemoteBootstrapResponsePB* resp,
                                                rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(tablet_manager_, "StartRemoteBootstrap", req, resp, &context)) {
    return;
  }
  if (req->has_split_parent_tablet_id()
      && !PREDICT_FALSE(FLAGS_TEST_disable_post_split_tablet_rbs_check)) {
    // For any tablet that was the result of a split, the raft group leader will always send the
    // split_parent_tablet_id. However, our local tablet manager should only know about the parent
    // if it was part of the raft group which committed the split to the parent, and if the parent
    // tablet has yet to be deleted across the cluster.
    auto tablet_peer = tablet_manager_->GetServingTablet(req->split_parent_tablet_id());
    if (tablet_peer.ok()) {
      auto tablet = (**tablet_peer).shared_tablet_maybe_null();
      // If local parent tablet replica has been already split or remote bootstrapped from remote
      // replica that has been already split - allow RBS of child tablets.
      // In this case we can't rely on local parent tablet replica split to create child tablet
      // replicas on the current node, because local bootstrap is not replaying already applied
      // SPLIT_OP (it has op_id <= flushed_op_id).
      if (!tablet || tablet->metadata()->tablet_data_state() !=
                         tablet::TabletDataState::TABLET_DATA_SPLIT_COMPLETED) {
        YB_LOG_EVERY_N_SECS(WARNING, 30)
            << "Start remote bootstrap rejected: parent tablet not yet split.";
        SetupErrorAndRespond(
            resp->mutable_error(),
            STATUS(Incomplete, "Rejecting bootstrap request while parent tablet is present."),
            TabletServerErrorPB::TABLET_SPLIT_PARENT_STILL_LIVE,
            &context);
        return;
      }
    }
  }

  if (req->has_clone_source_seq_no() != req->has_clone_source_tablet_id()) {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_FORMAT(InvalidArgument, "Expected both or neither clone source field to be set."),
        TabletServerErrorPB::UNKNOWN_ERROR,
        &context);
    return;
  }
  if (req->has_clone_source_seq_no() && req->has_clone_source_tablet_id()) {
    auto tablet_peer = tablet_manager_->GetServingTablet(req->clone_source_tablet_id());
    if (tablet_peer.ok()) {
      auto tablet = (**tablet_peer).shared_tablet_maybe_null();
      if (tablet && !tablet->metadata()->HasAttemptedClone(req->clone_source_seq_no())) {
        SetupErrorAndRespond(
            resp->mutable_error(),
            STATUS(Incomplete, "Rejecting bootstrap request while clone source has not applied "
                "CLONE_OP."),
            TabletServerErrorPB::TABLET_SPLIT_PARENT_STILL_LIVE, // TODO(anubhav): use a new error
            &context);
        return;
      }
    }
  }

  const auto& wait_state = ash::WaitStateInfo::CurrentWaitState();
  if (wait_state) {
    wait_state->UpdateMetadata(
      {.root_request_id = Uuid::Generate(),
      .query_id = std::to_underlying(ash::FixedQueryId::kQueryIdForRemoteBootstrap)});
    wait_state->UpdateAuxInfo({.tablet_id = req->tablet_id(), .method = "RemoteBootstrap"});
  }

  Status s = tablet_manager_->StartRemoteBootstrap(*req);
  if (!s.ok()) {
    // Using Status::AlreadyPresent for a remote bootstrap operation that is already in progress.
    if (s.IsAlreadyPresent()) {
      YB_LOG_EVERY_N_SECS(WARNING, 30) << "Start remote bootstrap failed: " << s;
      SetupErrorAndRespond(resp->mutable_error(), s, TabletServerErrorPB::ALREADY_IN_PROGRESS,
                           &context);
      return;
    } else if (s.IsServiceUnavailable()) {
      YB_LOG_EVERY_N_SECS(WARNING, 5) << "Start remote bootstrap failed: " << s;
    } else {
      LOG(WARNING) << "Start remote bootstrap failed: " << s;
    }
  }

  RETURN_UNKNOWN_ERROR_IF_NOT_OK(s, resp, &context);
  context.RespondSuccess();
}

void TabletServiceImpl::NoOp(const NoOpRequestPB *req,
                             NoOpResponsePB *resp,
                             rpc::RpcContext context) {
  context.RespondSuccess();
}

void TabletServiceImpl::Publish(
    const PublishRequestPB* req, PublishResponsePB* resp, rpc::RpcContext context) {
  rpc::Publisher* publisher = server_->GetPublisher();
  resp->set_num_clients_forwarded_to(publisher ? (*publisher)(req->channel(), req->message()) : 0);
  context.RespondSuccess();
}

void TabletServiceImpl::ListTablets(const ListTabletsRequestPB* req,
                                    ListTabletsResponsePB* resp,
                                    rpc::RpcContext context) {
  TabletPeers peers = server_->tablet_manager()->GetTabletPeers(
      /*tablet_ptrs=*/ nullptr,
      UserTabletsOnly(req->include_user_tablets_only()));
  RepeatedPtrField<StatusAndSchemaPB>* peer_status = resp->mutable_status_and_schema();
  for (const TabletPeerPtr& peer : peers) {
    StatusAndSchemaPB* status = peer_status->Add();
    peer->GetTabletStatusPB(status->mutable_tablet_status());
    SchemaToPB(*peer->status_listener()->schema(), status->mutable_schema());
    peer->tablet_metadata()->partition_schema()->ToPB(status->mutable_partition_schema());
  }
  context.RespondSuccess();
}

void TabletServiceImpl::GetMasterAddresses(const GetMasterAddressesRequestPB* req,
                                           GetMasterAddressesResponsePB* resp,
                                           rpc::RpcContext context) {
  resp->set_master_addresses(server::MasterAddressesToString(
      *server_->tablet_manager()->server()->options().GetMasterAddresses()));
  context.RespondSuccess();
}

void TabletServiceImpl::GetLogLocation(
    const GetLogLocationRequestPB* req,
    GetLogLocationResponsePB* resp,
    rpc::RpcContext context) {
  resp->set_log_location(FLAGS_log_dir);
  context.RespondSuccess();
}

void TabletServiceImpl::ListTabletsForTabletServer(const ListTabletsForTabletServerRequestPB* req,
                                                   ListTabletsForTabletServerResponsePB* resp,
                                                   rpc::RpcContext context) {
  // Replicating logic from path-handlers.
  TabletPeers peers = server_->tablet_manager()->GetTabletPeers();
  for (const TabletPeerPtr& peer : peers) {
    TabletStatusPB status;
    peer->GetTabletStatusPB(&status);

    ListTabletsForTabletServerResponsePB::Entry* data_entry = resp->add_entries();
    data_entry->set_table_name(status.table_name());
    data_entry->set_tablet_id(status.tablet_id());

    auto consensus_result = peer->GetConsensus();
    data_entry->set_is_leader(
        consensus_result && consensus_result.get()->role() == PeerRole::LEADER);
    data_entry->set_has_leader_lease(
        consensus_result &&
        consensus_result.get()->GetLeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY);
    data_entry->set_state(status.state());

    auto tablet = peer->shared_tablet_maybe_null();
    uint64_t num_sst_files = tablet ? tablet->GetCurrentVersionNumSSTFiles() : 0;
    data_entry->set_num_sst_files(num_sst_files);

    uint64_t num_log_segments = peer->GetNumLogSegments();
    data_entry->set_num_log_segments(num_log_segments);

    auto num_memtables = tablet ? tablet->GetNumMemtables() : std::make_pair(0, 0);
    data_entry->set_num_memtables_intents(num_memtables.first);
    data_entry->set_num_memtables_regular(num_memtables.second);
  }

  context.RespondSuccess();
}

namespace {

Result<uint64_t> CalcChecksum(tablet::Tablet* tablet, CoarseTimePoint deadline) {
  auto scoped_read_operation = tablet->CreateScopedRWOperationNotBlockingRocksDbShutdownStart();
  RETURN_NOT_OK(scoped_read_operation);

  const shared_ptr<Schema> schema = tablet->metadata()->schema();
  dockv::ReaderProjection projection(*schema);
  auto iter = tablet->NewRowIterator(projection, {}, "", deadline);
  RETURN_NOT_OK(iter);

  qlexpr::QLTableRow value_map;
  ScanResultChecksummer collector;

  while (VERIFY_RESULT((**iter).FetchNext(&value_map))) {
    collector.HandleRow(*schema, value_map);
  }

  return collector.agg_checksum();
}

} // namespace

Result<uint64_t> TabletServiceImpl::DoChecksum(
    const ChecksumRequestPB* req, CoarseTimePoint deadline) {
  auto abstract_tablet = VERIFY_RESULT(GetTablet(
      server_->tablet_peer_lookup(), req->tablet_id(), /* tablet_peer = */ nullptr,
      req->consistency_level(), AllowSplitTablet::kTrue));
  return CalcChecksum(down_cast<tablet::Tablet*>(abstract_tablet.get()), deadline);
}

void TabletServiceImpl::Checksum(const ChecksumRequestPB* req,
                                 ChecksumResponsePB* resp,
                                 rpc::RpcContext context) {
  VLOG(1) << "Full request: " << req->DebugString();

  auto checksum = DoChecksum(req, context.GetClientDeadline());
  if (!checksum.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), checksum.status(), &context);
    return;
  }

  resp->set_checksum(*checksum);
  context.RespondSuccess();
}

void TabletServiceImpl::ImportData(const ImportDataRequestPB* req,
                                   ImportDataResponsePB* resp,
                                   rpc::RpcContext context) {
  auto peer = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context));

  auto status = peer.tablet->ImportData(req->source_dir());
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::GetTabletStatus(const GetTabletStatusRequestPB* req,
                                        GetTabletStatusResponsePB* resp,
                                        rpc::RpcContext context) {
  const Status s = server_->GetTabletStatus(req, resp);
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s,
                         s.IsNotFound() ? TabletServerErrorPB::TABLET_NOT_FOUND
                                        : TabletServerErrorPB::UNKNOWN_ERROR,
                         &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::IsTabletServerReady(const IsTabletServerReadyRequestPB* req,
                                            IsTabletServerReadyResponsePB* resp,
                                            rpc::RpcContext context) {
  Status s = server_->tablet_manager()->GetNumTabletsPendingBootstrap(resp);
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::GetSplitKey(
    const GetSplitKeyRequestPB* req, GetSplitKeyResponsePB* resp, RpcContext context) {
  TEST_PAUSE_IF_FLAG(TEST_pause_tserver_get_split_key);
  PerformAtLeader(req, resp, &context, server_, "GetSplitKey",
      [req, resp](const LeaderTabletPeer& leader_tablet_peer) -> Status {
        const auto& tablet = leader_tablet_peer.tablet;
        if (!req->is_manual_split() &&
            FLAGS_rocksdb_max_file_size_for_compaction > 0 &&
            tablet->schema()->table_properties().HasEffectiveDefaultTimeToLive()) {
          auto s = STATUS(NotSupported, "Tablet splitting not supported for TTL tables.");
          return s.CloneAndAddErrorCode(
              TabletServerError(TabletServerErrorPB::TABLET_SPLIT_DISABLED_TTL_EXPIRY));
        }
        if (tablet->MayHaveOrphanedPostSplitData()) {
          return STATUS(IllegalState, "Tablet has orphaned post-split data");
        }

        const auto split_factor =
            req->has_split_factor() ? req->split_factor() : kDefaultNumSplitParts;
        const auto split_keys = VERIFY_RESULT(tablet->GetSplitKeys(split_factor));
        const auto& encoded_keys = split_keys.encoded_keys;
        const auto& partition_keys = split_keys.partition_keys;

        resp->set_deprecated_split_encoded_key(encoded_keys.front());
        resp->set_deprecated_split_partition_key(partition_keys.front());
        resp->mutable_split_encoded_keys()->Add(encoded_keys.begin(), encoded_keys.end());
        resp->mutable_split_partition_keys()->Add(partition_keys.begin(), partition_keys.end());
        return Status::OK();
  });
}

void TabletServiceImpl::GetTserverCatalogVersionInfo(
    const GetTserverCatalogVersionInfoRequestPB* req,
    GetTserverCatalogVersionInfoResponsePB* resp,
    rpc::RpcContext context) {
  auto status = server_->get_ysql_db_oid_to_cat_version_info_map(*req, resp);
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::GetTserverCatalogMessageLists(
    const GetTserverCatalogMessageListsRequestPB* req,
    GetTserverCatalogMessageListsResponsePB* resp,
    rpc::RpcContext context) {
  auto status = server_->GetTserverCatalogMessageLists(*req, resp);
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::TriggerRelcacheInitConnection(
    const TriggerRelcacheInitConnectionRequestPB* req,
    TriggerRelcacheInitConnectionResponsePB* resp,
    rpc::RpcContext context) {
  auto shared_context = std::make_shared<rpc::RpcContext>(std::move(context));
  server_->TriggerRelcacheInitConnection(
      *req,
      [resp, shared_context](const Status& status) {
        if (!status.ok()) {
          SetupErrorAndRespond(resp->mutable_error(), status, shared_context.get());
          return;
        }
        shared_context->RespondSuccess();
      });
}

void TabletServiceImpl::ListMasterServers(const ListMasterServersRequestPB* req,
                                          ListMasterServersResponsePB* resp,
                                          rpc::RpcContext context) {
  const Status s = server_->tablet_manager()->server()->ListMasterServers(req, resp);
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s,
                         TabletServerErrorPB::UNKNOWN_ERROR,
                         &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::ClearUniverseUuid(const ClearUniverseUuidRequestPB* req,
                                          ClearUniverseUuidResponsePB* resp,
                                          rpc::RpcContext context) {
  const Status s = server_->tablet_manager()->server()->ClearUniverseUuid();
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::CheckTserverTabletHealth(const CheckTserverTabletHealthRequestPB* req,
                                                CheckTserverTabletHealthResponsePB* resp,
                                                rpc::RpcContext context) {
  TRACE("CheckTserverTabletHealth");
  for (auto& tablet_id : req->tablet_ids()) {
    auto res = LookupTabletPeer(server_->tablet_manager(), tablet_id);
    if (!res.ok()) {
      LOG_WITH_FUNC(INFO) << "Failed lookup for tablet " << tablet_id;
      continue;
    }

    auto consensus_result = GetConsensus(res->tablet_peer);
    if (!consensus_result.ok()) {
      LOG_WITH_FUNC(INFO) << "Could not find consensus for tablet " << tablet_id
                          << ": " << consensus_result.status();
      continue;
    }
    auto consensus = *consensus_result;

    auto* tablet_health = resp->add_tablet_healths();
    tablet_health->set_tablet_id(tablet_id);

    auto role = consensus->role();
    tablet_health->set_role(role);

    if (FLAGS_TEST_set_tablet_follower_lag_ms != 0) {
      tablet_health->set_follower_lag_ms(FLAGS_TEST_set_tablet_follower_lag_ms);
      continue;
    }
    if (role != PeerRole::LEADER) {
      tablet_health->set_follower_lag_ms(consensus->follower_lag_ms());
    }
  }
  TEST_PAUSE_IF_FLAG(TEST_pause_before_tablet_health_response);
  context.RespondSuccess();
}

void TabletServiceImpl::GetMetrics(const GetMetricsRequestPB* req,
                                   GetMetricsResponsePB* resp,
                                   rpc::RpcContext context) {
  auto result = server_->GetMetrics();
  if (!result.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), result.status(), &context);
    return;
  }
  vector<TserverMetricsInfoPB> metrics = result.get();
  *resp->mutable_metrics() = {metrics.begin(), metrics.end()};
  context.RespondSuccess();
}

void TabletServiceImpl::SetActiveTableIdsForMetrics(
    const SetActiveTableIdsForMetricsRequestPB* req, SetActiveTableIdsForMetricsResponsePB* resp,
    rpc::RpcContext context) {
  // Reported on every response, including rejections, so the caller always learns whether any
  // scrape can use the list it just sent.
  resp->set_filtering_enabled(FLAGS_enable_active_table_metrics_filtering);

  // DocDB table IDs are 32 hex characters, and colocation parent IDs are about twice that. The
  // bound keeps a caller from parking arbitrarily large strings in tablet server memory.
  constexpr size_t kMaxTableIdLength = 256;
  // Reserve no more than the limit allows, so an oversized request cannot force a large allocation
  // before the limit check below rejects it.
  std::unordered_set<std::string> table_ids;
  table_ids.reserve(std::min<size_t>(
      req->table_ids_size(),
      static_cast<size_t>(FLAGS_max_active_table_metrics_table_count) + 1));
  for (const auto& table_id : req->table_ids()) {
    if (table_id.empty() || table_id.size() > kMaxTableIdLength) {
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS_FORMAT(
              InvalidArgument, "table_ids must be between 1 and $0 bytes long, found $1 bytes",
              kMaxTableIdLength, table_id.size()),
          &context);
      return;
    }
    // Checked while copying, so an oversized request is rejected after at most one entry over the
    // limit instead of after the whole request has been copied. Duplicates do not count.
    if (table_ids.insert(table_id).second &&
        table_ids.size() > FLAGS_max_active_table_metrics_table_count) {
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS_FORMAT(
              InvalidArgument, "Active table list contains more than $0 tables",
              FLAGS_max_active_table_metrics_table_count),
          &context);
      return;
    }
  }

  const auto status =
      server_->SetActiveTableIdsForMetrics(std::move(table_ids), req->list_version());
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::PgRemoteExec(
    const PgRemoteExecRequestPB* req, PgRemoteExecResponsePB* resp,
    rpc::RpcContext context) {
  VLOG(1) << "received remote pg exec query: " << req->query();

  // TODO(#30396): Maintain a pool of connections instead of creating a new connection
  auto conn = server_->CreateInternalPGConn(
      req->database_name(), /* user */ "yb_global_views_user",
      /* simple_query_protocol */ false, context.GetClientDeadline(),
      pgwrapper::YbInternalConnKindWireName::kGlobalView);
  if (!conn.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), conn.status(), &context);
    return;
  }

  if (FLAGS_TEST_pause_remote_pg_query_execution_ms > 0) {
    SleepFor(FLAGS_TEST_pause_remote_pg_query_execution_ms * 1ms);
  }

  // c_str() pointers borrow from req's protobuf strings, valid for the
  // lifetime of this RPC handler.
  std::vector<const char*> params;
  params.reserve(req->params_size());
  for (const auto& p : req->params()) {
    if (p.has_value()) {
      params.push_back(p.value().c_str());
    } else {
      params.push_back(nullptr);
    }
  }
  // Postgres_fdw expects results in TEXT format
  auto result = conn->Fetch(req->query(), pgwrapper::PGResultFormat::kText, params);

  if (!result.ok()) {
    // TODO(#30482): Fetch the error status from PGresult
    auto msg = result.status().message().ToBuffer();
    if (msg.empty()) {
      LOG(DFATAL) << "PgRemoteExec failed with an empty error message. Status: " << result.status();
      msg = result.status().CodeAsString();
    }
    resp->set_error_message(std::move(msg));
    context.RespondSuccess();
    return;
  }

  auto* pg_result = result->get();
  const auto total_rows = PQntuples(pg_result);
  const auto num_cols = PQnfields(pg_result);
  resp->set_num_cols(num_cols);
  // 1 KB is kept aside for RPC headers
  const auto max_resp_size = FLAGS_rpc_max_message_size - 1_KB;
  auto& buffer = context.sidecars().Start();
  std::vector<std::optional<Slice>> cells(num_cols);
  int num_rows = 0;
  for (; num_rows < total_rows; ++num_rows) {
    size_t row_size = 0;
    for (int col = 0; col < num_cols; ++col) {
      // The sidecar keeps the NUL terminator libpq stores after each text
      // value, so the coordinator reads values as C strings in place.
      cells[col] = PQgetisnull(pg_result, num_rows, col)
          ? std::optional<Slice>()
          : std::optional<Slice>(Slice(
                PQgetvalue(pg_result, num_rows, col),
                PQgetlength(pg_result, num_rows, col) + 1));
      row_size += pggate::GvCellSize(cells[col]);
    }
    if (!pggate::EncodeGvRow(cells, row_size, &buffer, max_resp_size)) {
      break;
    }
  }
  if (num_rows < total_rows) {
    resp->set_reached_size_limit(true);
    VLOG(1) << "Reached RPC size limit (" << FLAGS_rpc_max_message_size
            << " bytes). Encoded " << num_rows << " out of " << total_rows << " rows";
  }
  resp->set_num_rows(num_rows);
  resp->set_rows_sidecar(narrow_cast<uint32_t>(context.sidecars().Complete()));
  context.RespondSuccess();
}

void TabletServiceImpl::GetObjectLockStatus(const GetObjectLockStatusRequestPB* req,
                                            GetObjectLockStatusResponsePB* resp,
                                            rpc::RpcContext context) {
  auto ts_local_lock_manager = server_->ts_local_lock_manager();
  if (!ts_local_lock_manager) {
    SetupErrorAndRespond(
        resp->mutable_error(), STATUS(IllegalState, "TSLocalLockManager not found."), &context);
  }
  ts_local_lock_manager->PopulateObjectLocks(resp->mutable_object_lock_infos());
  context.RespondSuccess();
}

void TabletServiceImpl::GetLockStatus(const GetLockStatusRequestPB* req,
                                      GetLockStatusResponsePB* resp,
                                      rpc::RpcContext context) {
  TRACE("GetLockStatus");

  // Only one among transactions_by_tablet and transaction_ids should be set.
  if (req->transactions_by_tablet().empty() == (req->transaction_ids_size() == 0)) {
    auto s = STATUS(
        IllegalState,
        "Request must specify either txns_by_tablet or txn_ids, and cannot specify both.");
    LOG(DFATAL) << s;
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }

  TabletPeers tablet_peers;
  std::map<TransactionId, SubtxnSet> limit_resp_to_txns;
  for (const auto& [tablet_id, _] : req->transactions_by_tablet()) {
    // GetLockStatusRequestPB may include tablets that aren't hosted at this tablet server.
    auto res = LookupTabletPeer(server_->tablet_manager(), tablet_id);
    if (!res.ok()) {
      continue;
    }
    tablet_peers.push_back(res->tablet_peer);
  }

  if (req->transaction_ids().size() > 0) {
    // If this request specifies transaction_ids, then we check every tablet leader at this tserver
    // TODO(pglocks): We should have involved tablet info in this case as well. See
    // https://github.com/yugabyte/yugabyte-db/issues/16913
    tablet_peers = server_->tablet_manager()->GetTabletPeers();
  }
  for (auto& txn_id : req->transaction_ids()) {
    auto id_or_status = FullyDecodeTransactionId(txn_id);
    if (!id_or_status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), id_or_status.status(), &context);
      return;
    }
    // TODO(pglocks): Include aborted_subtxn info here as well.
    limit_resp_to_txns.emplace(std::make_pair(*id_or_status, SubtxnSet()));
  }

  for (const auto& tablet_peer : tablet_peers) {
    auto leader_term = tablet_peer->LeaderTerm();
    if (leader_term != OpId::kUnknownTerm &&
        (tablet_peer->tablet_metadata()->table_type() == PGSQL_TABLE_TYPE ||
         tablet_peer->tablet_metadata()->table_name() == std::string(kPgAdvisoryLocksTableName))) {
      const auto& tablet_id = tablet_peer->tablet_id();
      auto* tablet_lock_info = resp->add_tablet_lock_infos();
      tablet_lock_info->set_is_advisory_lock_tablet(
          tablet_peer->tablet_metadata()->table_type() != PGSQL_TABLE_TYPE);
      auto tablet_ptr_res = tablet_peer->shared_tablet();
      if (!tablet_ptr_res.ok()) {
        resp->Clear();
        SetupErrorAndRespond(resp->mutable_error(), tablet_ptr_res.status(), &context);
        return;
      }
      auto tablet_ptr = *tablet_ptr_res;
      Status s = Status::OK();
      if (req->transactions_by_tablet().count(tablet_id) > 0) {
        std::map<TransactionId, SubtxnSet> transactions;
        for (auto& txn : req->transactions_by_tablet().at(tablet_id).transactions()) {
          auto& txn_id = txn.id();
          auto id_or_status = FullyDecodeTransactionId(txn_id);
          if (!id_or_status.ok()) {
            resp->Clear();
            SetupErrorAndRespond(resp->mutable_error(), id_or_status.status(), &context);
            return;
          }
          auto aborted_subtxns_or_status = SubtxnSet::FromPB(txn.aborted().set());
          if (!aborted_subtxns_or_status.ok()) {
            resp->Clear();
            SetupErrorAndRespond(
                resp->mutable_error(), aborted_subtxns_or_status.status(), &context);
            return;
          }
          transactions.emplace(std::make_pair(*id_or_status, *aborted_subtxns_or_status));
        }
        s = tablet_ptr->GetLockStatus(
            transactions, tablet_lock_info, req->max_single_shard_waiter_start_time_us(),
            req->max_txn_locks_per_tablet());
      } else {
        DCHECK(!limit_resp_to_txns.empty());
        s = tablet_ptr->GetLockStatus(
            limit_resp_to_txns, tablet_lock_info, req->max_single_shard_waiter_start_time_us(),
            req->max_txn_locks_per_tablet());
      }
      if (!s.ok()) {
        resp->Clear();
        SetupErrorAndRespond(resp->mutable_error(), s, &context);
        return;
      }
      tablet_lock_info->set_term(leader_term);
    }
  }
  context.RespondSuccess();
}

void TabletServiceImpl::CancelTransaction(
    const CancelTransactionRequestPB* req, CancelTransactionResponsePB* resp,
    rpc::RpcContext context) {
  TRACE("CancelTransaction");

  auto id_or_status = FullyDecodeTransactionId(req->transaction_id());
  if (!id_or_status.ok()) {
    return SetupErrorAndRespond(resp->mutable_error(), id_or_status.status(), &context);
  }
  const auto& txn_id = *id_or_status;

  TabletPeers status_tablet_peers;
  if (req->status_tablet_id().empty()) {
    status_tablet_peers = server_->tablet_manager()->GetStatusTabletPeers();
  } else {
    auto peer_or_status = LookupTabletPeerOrRespond(
        server_->tablet_manager(), req->status_tablet_id(), resp, &context);
    if (!peer_or_status.ok()) {
      return;
    }
    // Ensure that the given tablet is a status tablet and that the tablet peer is initialized.
    auto peer = peer_or_status->tablet_peer;
    const auto& tablet_ptr = peer->shared_tablet_maybe_null();
    if (!tablet_ptr || !tablet_ptr->transaction_coordinator()) {
      return SetupErrorAndRespond(resp->mutable_error(),
                                  STATUS_FORMAT(IllegalState,
                                                "Transaction Coordinator not found for tablet $0",
                                                req->status_tablet_id()),
                                  &context);
    }
    status_tablet_peers.push_back(std::move(peer));
  }

  std::vector<std::future<Result<TransactionStatusResult>>> txn_status_res_futures;
  for (const auto& tablet_peer : status_tablet_peers) {
    // If the peer is not the leader for the group,
    // 1. and the cancel request contains a status tablet, return a NOT_LEADER error.
    // 2. and the cancel request does not contain a status tablet id, skip the peer.
    //
    // We do not return an error in case of 2. as we need to broadcast the cancel request to all
    // status tablets. If we return an error instead, we might miss looking at the leader peer
    // of the transaction status tablet, and hence would not cancel the transaction.
    auto res = LeaderTerm(*tablet_peer);
    if (!res.ok()) {
      if (!req->status_tablet_id().empty()) {
        return SetupErrorAndRespond(resp->mutable_error(), res.status(), &context);
      }
      continue;
    }

    auto leader_term = *res;
    auto txn_found = false;
    auto tablet_ptr_res = tablet_peer->shared_tablet();
    if (!tablet_ptr_res) {
      return SetupErrorAndRespond(resp->mutable_error(), tablet_ptr_res.status(), &context);
    }
    auto tablet_ptr = *tablet_ptr_res;
    auto future = MakeFuture<Result<TransactionStatusResult>>(
        [txn_id, tablet_peer, leader_term, tablet_ptr, &txn_found](auto callback) {
      txn_found = tablet_ptr->transaction_coordinator()->CancelTransactionIfFound(
          txn_id,
          leader_term,
          [callback, tablet_peer] (Result<TransactionStatusResult> res) {
            // Note: If the txn is successfully canceled, we need not validate the peer's
            // leadership status as we see TransactionStatus::ABORTED only after the ABORT
            // operation is replicated across all involved tablets. So we can forward the
            // result immaterial of the peer's leadership status.
            callback(res);
          });
    });

    if (txn_found) {
      txn_status_res_futures.push_back(std::move(future));
    }
  }

  // Return if the transaction is not hosted on any of the status tablets of this TabletServer.
  if (txn_status_res_futures.empty()) {
    return SetupErrorAndRespond(resp->mutable_error(),
                                STATUS_FORMAT(NotFound,
                                              "Failed canceling transaction $0", txn_id.ToString()),
                                &context);
  }

  // There should be at most 1 status tablet hosting the transaction. In case of transaction
  // promotion, the transaction can be present at 2 status tablets until commit time.
  DCHECK_LE(txn_status_res_futures.size(), 2);

  for (auto& future : txn_status_res_futures) {
    // Errors and txn statuses other than ABORTED take precedence over TransactionStatus::ABORTED.
    // This needs to be done to correctly handle cancelation requests of promoted transactions.
    const auto& res = future.get();
    if (!res.ok() || res->status != TransactionStatus::ABORTED) {
      auto s = !res.ok() ? res.status() : STATUS_FORMAT(NotSupported,
                                                        "Cannot cancel transaction $0 in state $1",
                                                        txn_id.ToString(),
                                                        res->ToString());
      return SetupErrorAndRespond(resp->mutable_error(), s, &context);
    }
    DCHECK_EQ(res->status, TransactionStatus::ABORTED);
  }

  context.RespondSuccess();
}

void TabletServiceImpl::StartRemoteSnapshotTransfer(
    const StartRemoteSnapshotTransferRequestPB* req, StartRemoteSnapshotTransferResponsePB* resp,
    rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(
          server_->tablet_manager(), "StartRemoteSnapshotTransfer", req, resp, &context)) {
    return;
  }

  Status s = server_->tablet_manager()->StartRemoteSnapshotTransfer(*req);
  if (!s.ok()) {
    // Using Status::AlreadyPresent for a remote snapshot transfer operation that is already in
    // progress.
    if (s.IsAlreadyPresent()) {
      YB_LOG_EVERY_N_SECS(WARNING, 30) << "Start remote snapshot transfer failed: " << s;
      SetupErrorAndRespond(
          resp->mutable_error(), s, TabletServerErrorPB::ALREADY_IN_PROGRESS, &context);
      return;
    } else {
      LOG(WARNING) << "Start remote snapshot transfer failed: " << s;
    }
  }

  RETURN_UNKNOWN_ERROR_IF_NOT_OK(s, resp, &context);
  context.RespondSuccess();
}

void TabletServiceImpl::GetTabletKeyRanges(
    const GetTabletKeyRangesRequestPB* req, GetTabletKeyRangesResponsePB* resp,
    rpc::RpcContext context) {
  PerformAtLeader(
      req, resp, &context, server_, "GetTabletKeyRanges",
      [req, &context](const LeaderTabletPeer& leader_tablet_peer) -> Status {
        const auto& tablet = leader_tablet_peer.tablet;
        RETURN_NOT_OK(tablet->GetTabletKeyRanges(
            req->lower_bound_key(), req->upper_bound_key(), req->max_num_ranges(),
            req->range_size_bytes(),
            req->is_forward() ? tablet::Direction::kForward : tablet::Direction::kBackward,
            req->max_key_length(), &context.sidecars().Start()));
        return Status::OK();
      });
}

void TabletServiceImpl::ClearAllMetaCachesOnServer(
    const ClearAllMetaCachesOnServerRequestPB* req, ClearAllMetaCachesOnServerResponsePB* resp,
    rpc::RpcContext context) {
  server_->ClearAllMetaCachesOnServer();
  context.RespondSuccess();
}

void TabletServiceImpl::ClearMetacache(
    const ClearMetacacheRequestPB* req, ClearMetacacheResponsePB* resp, rpc::RpcContext context) {
  if (!req->has_namespace_id()) {
    SetupErrorAndRespond(
        resp->mutable_error(), STATUS(InvalidArgument, "namespace_id is not specified"), &context);
    return;
  }
  auto s = FLAGS_TEST_fail_clear_metacache
      ? STATUS(InternalError, "Failing ClearMetacache for test")
      : server_->ClearMetacache(req->namespace_id());
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
  } else {
    context.RespondSuccess();
  }
}

void TabletServiceImpl::ClearYCQLMetaDataCacheOnServer(
    const ClearYCQLMetaDataCacheOnServerRequestPB* req,
    ClearYCQLMetaDataCacheOnServerResponsePB* resp,
    rpc::RpcContext context) {
  auto s = server_->ClearYCQLMetaDataCache();
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
  } else {
    context.RespondSuccess();
  }
}

void TabletServiceImpl::AcquireObjectLocks(
    const AcquireObjectLockRequestPB* req, AcquireObjectLockResponsePB* resp,
    rpc::RpcContext context) {
  TRACE("Start AcquireObjectLocks");
  VLOG(2) << "Received AcquireObjectLocks RPC: " << req->DebugString();
  if (!FLAGS_enable_object_locking_for_table_locks) {
    LOG_WITH_FUNC(INFO)
        << "Flag enable_object_locking_for_table_locks disabled. Ignoring acquire request.";
    return context.RespondSuccess();
  }
  if (auto s = CheckLocalLeaseEpoch(GetRecipientLeaseEpoch(*req)); !s.ok()) {
    return SetupErrorAndRespond(
        resp->mutable_error(), s, TabletServerErrorPB::INVALID_YSQL_LEASE, &context);
  }
  auto ts_local_lock_manager = server_->ts_local_lock_manager();
  if (!ts_local_lock_manager) {
    VLOG_WITH_FUNC(1) << "TSLocalLockManager not found...";
    return SetupErrorAndRespond(
        resp->mutable_error(), STATUS(IllegalState, "TSLocalLockManager not found..."), &context);
  }
  const auto deadline = context.GetClientDeadline();
  ts_local_lock_manager->AcquireObjectLocksAsync(
      *req, deadline,
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));
}

void TabletServiceImpl::ReleaseObjectLocks(
    const ReleaseObjectLockRequestPB* req, ReleaseObjectLockResponsePB* resp,
    rpc::RpcContext context) {
  TRACE("Start ReleaseObjectLocks");
  VLOG(2) << "Received ReleaseObjectLocks RPC: " << req->DebugString();
  if (!FLAGS_enable_object_locking_for_table_locks) {
    LOG_WITH_FUNC(INFO)
        << "Flag enable_object_locking_for_table_locks disabled. Ignoring release request.";
    return context.RespondSuccess();
  }

  if (auto s = CheckLocalLeaseEpoch(GetRecipientLeaseEpoch(*req)); !s.ok()) {
    return SetupErrorAndRespond(
        resp->mutable_error(), s, TabletServerErrorPB::INVALID_YSQL_LEASE, &context);
  }
  auto ts_local_lock_manager = server_->ts_local_lock_manager();
  if (!ts_local_lock_manager) {
    VLOG_WITH_FUNC(1) << "TSLocalLockManager not found...";
    resp->set_propagated_hybrid_time(server_->Clock()->Now().ToUint64());
    SetupErrorAndRespond(
        resp->mutable_error(), STATUS(IllegalState, "TSLocalLockManager not found..."), &context);
    return;
  }
  auto res = ts_local_lock_manager->ReleaseObjectLocks(*req, context.GetClientDeadline());
  resp->set_propagated_hybrid_time(server_->Clock()->Now().ToUint64());
  if (!res.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), res.status(), &context);
  } else {
    context.RespondSuccess();
  }
}

void TabletServiceImpl::WaitForLockersMultiple(
    const WaitForLockersMultipleRequestPB* req, WaitForLockersMultipleResponsePB* resp,
    rpc::RpcContext context) {
  if (FLAGS_TEST_fail_wait_for_lockers) {
    LOG(INFO) << "Responding with a failure to " << req->ShortDebugString();
    // Send back OPERATION_NOT_SUPPORTED to prevent further retry.
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS(InternalError, "TEST_fail_wait_for_lockers set").CloneAndAddErrorCode(
          TabletServerError(TabletServerErrorPB::OPERATION_NOT_SUPPORTED)),
        &context);
    return;
  }

  TEST_PAUSE_IF_FLAG(TEST_pause_wait_for_lockers);
  TRACE("Start WaitForLockersMultiple");
  VLOG(2) << "Received WaitForLockersMultiple RPC: " << req->DebugString();
  if (!FLAGS_enable_object_locking_for_table_locks) {
    LOG_WITH_FUNC(INFO)
        << "Flag enable_object_locking_for_table_locks disabled. "
        << "Ignoring wait_for_lockers request.";
    return context.RespondSuccess();
  }
  if (auto s = CheckLocalLeaseEpoch(GetRecipientLeaseEpoch(*req)); !s.ok()) {
    return SetupErrorAndRespond(
        resp->mutable_error(), s, TabletServerErrorPB::INVALID_YSQL_LEASE, &context);
  }
  auto ts_local_lock_manager = server_->ts_local_lock_manager();
  if (!ts_local_lock_manager) {
    VLOG_WITH_FUNC(1) << "TSLocalLockManager not found...";
    return SetupErrorAndRespond(
        resp->mutable_error(), STATUS(IllegalState, "TSLocalLockManager not found..."), &context);
  }
  TransactionId background_txn_id = TransactionId::Nil();
  if (!req->background_transaction_id().empty()) {
    auto res = FullyDecodeTransactionId(req->background_transaction_id());
    if (!res.ok()) {
      return SetupErrorAndRespond(resp->mutable_error(), res.status(), &context);
    }
    background_txn_id = *res;
  }
  const auto deadline = context.GetClientDeadline();
  ts_local_lock_manager->WaitForLockersAsync(
      req->object_locks(), deadline,
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()),
      background_txn_id, req->background_transaction_status_tablet());
}

Result<GetYSQLLeaseInfoResponsePB> TabletServiceImpl::GetYSQLLeaseInfo(
    const GetYSQLLeaseInfoRequestPB& req, CoarseTimePoint deadline) {
  auto lease_info = VERIFY_RESULT(server_->GetYSQLLeaseInfo());
  GetYSQLLeaseInfoResponsePB resp;
  resp.set_is_live(lease_info.is_live);
  if (lease_info.is_live) {
    resp.set_lease_epoch(lease_info.lease_epoch);
  }
  return resp;
}

void TabletServiceImpl::AdminExecutePgsql(
    const AdminExecutePgsqlRequestPB* req, AdminExecutePgsqlResponsePB* resp,
    rpc::RpcContext context) {
  auto execute_pg_sql = [&req, &context, &server = server_]() -> Status {
    const auto& deadline = context.GetClientDeadline();
    auto pg_conn = VERIFY_RESULT(
        server->CreateInternalPGConn(req->database_name(), kDefaultInternalPgUser, false,
                                     deadline, req->yb_internal_conn_kind()));
    for (const auto& stmt : req->pgsql_statements()) {
      SCHECK_LT(
          CoarseMonoClock::Now(), deadline, TimedOut, "Timed out while executing Ysql statements");
      RETURN_NOT_OK(pg_conn.Execute(stmt));
    }
    return Status::OK();
  };

  auto status = execute_pg_sql();
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
  } else {
    context.RespondSuccess();
  }
}

void TabletServiceImpl::CollectYsqlCallHomeStats(
    const CollectYsqlCallHomeStatsRequestPB* req, CollectYsqlCallHomeStatsResponsePB* resp,
    rpc::RpcContext context) {
  auto result = CollectYsqlClusterStatsJson(server_);
  if (!result.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), result.status(), &context);
    return;
  }
  resp->set_json_stats(*result);
  context.RespondSuccess();
}

void TabletServiceImpl::GetLocalPgTxnSnapshot(
    const GetLocalPgTxnSnapshotRequestPB* req, GetLocalPgTxnSnapshotResponsePB* resp,
    rpc::RpcContext context) {
  const auto result = GetLocalPgTxnSnapshotImpl(*server_, req->snapshot_id());
  if (!result.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), result.status(), &context);
    return;
  }
  const auto& snapshot = *result;
  snapshot.ToPBNoReadTime(*resp->mutable_snapshot());
  snapshot.read_time.ToPB(resp->mutable_snapshot_read_time());
  context.RespondSuccess();
}

void TabletServiceAdminImpl::TestRetry(
    const TestRetryRequestPB* req, TestRetryResponsePB* resp, rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "TestRetry", req, resp, &context)) {
    return;
  }
  auto num_calls = TEST_num_test_retry_calls_.fetch_add(1) + 1;
  if (num_calls < req->num_retries()) {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_FORMAT(TryAgain, "Got $0 calls of $1", num_calls, req->num_retries()), &context);
  } else {
    context.RespondSuccess();
  }
}

void TabletServiceAdminImpl::GetActiveRbsInfo(
    const GetActiveRbsInfoRequestPB* req, GetActiveRbsInfoResponsePB* resp,
    rpc::RpcContext context) {
  for (const auto& peer : server_->tablet_manager()->GetTabletPeers()) {
    auto rbs_info = peer->status_listener()->GetRbsProgressInfo();
    if (!rbs_info) continue;
    auto* rbs_resp_info = resp->add_rbs_infos();
    rbs_resp_info->set_tablet_id(peer->tablet_id());
    rbs_resp_info->set_source_ts_uuid(rbs_info->source_ts_uuid);
    rbs_resp_info->set_sst_bytes_to_download(rbs_info->sst_bytes_to_download);
    rbs_resp_info->set_sst_bytes_downloaded(rbs_info->sst_bytes_downloaded);

    auto sst_end_time_micros = (rbs_info->sst_end_time_micros == 0) ?
                               GetCurrentTimeMicros() :
                               rbs_info->sst_end_time_micros;
    rbs_resp_info->set_sst_download_elapsed_sec(
        (sst_end_time_micros - rbs_info->sst_start_time_micros) / 1000000);
  }
  context.RespondSuccess();
}

void TabletServiceImpl::Shutdown() {
}

Result<VerifyVectorIndexesResponsePB> TabletServiceImpl::VerifyVectorIndexes(
    const VerifyVectorIndexesRequestPB& req, CoarseTimePoint deadline) {
  std::vector<tablet::TabletPeerPtr> tablet_peers;
  if (req.tablet_id().empty()) {
    SCHECK(req.index_table_ids().empty(), InvalidArgument,
           "Vector index ids are specified without a tablet id");
    tablet_peers = server_->tablet_manager()->GetTabletPeers();
  } else {
    tablet_peers.push_back(
        VERIFY_RESULT(LookupTabletPeer(server_->tablet_peer_lookup(), req.tablet_id()))
            .tablet_peer);
  }
  VerifyVectorIndexesResponsePB resp;
  for (const auto& peer : tablet_peers) {
    auto tablet = peer->shared_tablet_maybe_null();
    if (!tablet) {
      continue;
    }
    std::vector<TableId> index_table_ids(
        req.index_table_ids().begin(), req.index_table_ids().end());
    if (index_table_ids.empty()) {
      auto list = tablet->vector_indexes().List();
      if (!list) {
        continue;
      }
      for (const auto& vector_index : *list) {
        // A deactivated index cannot be checked, see CheckVectorIndex.
        if (!vector_index->IsDeactivated()) {
          index_table_ids.push_back(vector_index->table_id());
        }
      }
    }
    auto consensus = peer->GetConsensus();
    auto role = consensus.ok() ? (**consensus).role() : PeerRole::UNKNOWN_ROLE;
    for (const auto& index_table_id : index_table_ids) {
      auto& check = *resp.add_checks();
      check.set_tablet_id(peer->tablet_id());
      check.set_index_table_id(index_table_id);
      check.set_role(role);
      auto result = tablet->vector_indexes().Check(
          index_table_id, req.max_inconsistencies(), deadline);
      if (result.ok()) {
        *check.mutable_result() = std::move(*result);
      } else {
        StatusToPB(result.status(), check.mutable_status());
      }
    }
  }
  return resp;
}

Result<DumpTabletDataResponsePB> TabletServiceImpl::DumpTabletData(
    const DumpTabletDataRequestPB& req, CoarseTimePoint deadline) {
  LOG(INFO) << "DumpTabletData request: " << req.ShortDebugString();
  auto peer_tablet =
      VERIFY_RESULT(LookupTabletPeer(server_->tablet_peer_lookup(), req.tablet_id()));
  uint64_t read_ht = 0;
  if (req.has_read_ht()) {
    read_ht = req.read_ht();
  }
  // An absent max_wait_ms falls back to the server default, not to zero. Safe time trails the
  // present, so a caller asking for "now" is always slightly ahead of it.
  std::optional<MonoDelta> max_read_time_wait;
  if (req.has_max_wait_ms()) {
    max_read_time_wait = MonoDelta::FromMilliseconds(req.max_wait_ms());
  }

  auto peer_role = VERIFY_RESULT(peer_tablet.tablet_peer->GetConsensus())->role();

  std::unique_ptr<WritableFile> file;
  if (!req.dest_path().empty()) {
    RETURN_NOT_OK(Env::Default()->NewWritableFile(req.dest_path(), &file));
    RETURN_NOT_OK(file->Append(Format("Tablet ID: $0\n", req.tablet_id())));
    RETURN_NOT_OK(
        file->Append(Format("Peer UUID: $0\n", peer_tablet.tablet_peer->permanent_uuid())));
    RETURN_NOT_OK(file->Append(Format("Peer Role: $0\n", PeerRole_Name(peer_role))));
  }

  uint64_t row_count = 0;
  uint64_t xor_hash = 0;
  // Scope the hash by the requested table_id. A colocation parent table id (or an unset table_id)
  // hashes every table in the tablet; any other table id restricts the scan to that single table.
  TableId target_table_id;
  if (req.has_table_id() && !req.table_id().empty() && !IsColocationParentTableId(req.table_id())) {
    target_table_id = req.table_id();
  }
  Slice start_key = req.has_start_key() ? Slice(req.start_key()) : Slice();
  Slice end_key = req.has_end_key() ? Slice(req.end_key()) : Slice();
  const uint64_t max_rows = req.has_max_rows() ? req.max_rows() : 0;
  std::string next_key;
  RETURN_NOT_OK(
      tablet::DumpTabletData(
          *peer_tablet.tablet, server_->client_future(), file.get(), read_ht, max_read_time_wait,
          deadline, xor_hash, row_count, target_table_id, start_key, end_key, max_rows,
          &next_key));
  DumpTabletDataResponsePB resp;
  resp.set_row_count(row_count);
  resp.set_xor_hash(xor_hash);
  // Always sent, so a caller can tell "this server hashes the way I do" from "this server is too
  // old to say", which differ during a rolling upgrade. A test may alter it to impersonate such a
  // server; 0 means leave it unset, as that server would.
  const auto scheme_version_override = FLAGS_TEST_dump_tablet_data_hash_scheme_version;
  if (scheme_version_override < 0) {
    resp.set_hash_scheme_version(tablet::kTabletDataHashSchemeVersion);
  } else if (scheme_version_override > 0) {
    resp.set_hash_scheme_version(scheme_version_override);
  }
  if (!next_key.empty()) {
    resp.set_next_key(next_key);
  }

  if (file) {
    RETURN_NOT_OK(file->Append(Format("\nRow count: $0\n", row_count)));
    RETURN_NOT_OK(file->Append(Format("XOR hash: $0\n", xor_hash)));
    RETURN_NOT_OK(
        file->Append(Format("Hash scheme version: $0\n", tablet::kTabletDataHashSchemeVersion)));
    RETURN_NOT_OK(file->Close());
  }
  return resp;
}

Result<ConnectivityStateResponsePB> TabletServiceImpl::ConnectivityState(
    const ConnectivityStateRequestPB& req, CoarseTimePoint deadline) {
  return server_->ConnectivityState();
}

Status TabletServiceImpl::CheckLocalLeaseEpoch(std::optional<uint64_t> recipient_lease_epoch) {
  if (!recipient_lease_epoch.has_value()) {
    return Status::OK();
  }
  auto local_lease_info = VERIFY_RESULT(server_->GetYSQLLeaseInfo());
  if (local_lease_info.is_live && local_lease_info.lease_epoch == *recipient_lease_epoch) {
    return Status::OK();
  }
  auto msg = !local_lease_info.is_live
                 ? "TServer does not have a live lease"
                 : Format(
                       "Expected lease epoch in request $0 does not match TServer's local "
                       "lease epoch $1",
                       *recipient_lease_epoch, local_lease_info.lease_epoch);
  return STATUS(IllegalState, msg);
}

}  // namespace yb::tserver
