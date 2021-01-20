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

#include "yb/cdc/cdc_service.h"

#include <shared_mutex>
#include <chrono>
#include <memory>

#include <boost/algorithm/string.hpp>

#include "yb/cdc/cdc_producer.h"
#include "yb/cdc/cdc_service.proxy.h"
#include "yb/cdc/cdc_rpc.h"

#include "yb/common/entity_ids.h"
#include "yb/common/ql_expr.h"
#include "yb/common/ql_value.h"
#include "yb/common/pg_system_attr.h"
#include "yb/common/wire_protocol.h"

#include "yb/consensus/raft_consensus.h"
#include "yb/consensus/replicate_msgs_holder.h"

#include "yb/client/meta_cache.h"
#include "yb/client/table.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_handle.h"
#include "yb/client/session.h"
#include "yb/client/yb_table_name.h"
#include "yb/client/yb_op.h"
#include "yb/gutil/strings/join.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/service_util.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/flag_tags.h"
#include "yb/util/scope_exit.h"
#include "yb/util/shared_lock.h"
#include "yb/yql/cql/ql/util/statement_result.h"

DEFINE_int32(cdc_read_rpc_timeout_ms, 30 * 1000,
             "Timeout used for CDC read rpc calls.  Reads normally occur cross-cluster.");
TAG_FLAG(cdc_read_rpc_timeout_ms, advanced);

DEFINE_int32(cdc_write_rpc_timeout_ms, 30 * 1000,
             "Timeout used for CDC write rpc calls.  Writes normally occur intra-cluster.");
TAG_FLAG(cdc_write_rpc_timeout_ms, advanced);

DEFINE_int32(cdc_ybclient_reactor_threads, 50,
             "The number of reactor threads to be used for processing ybclient "
             "requests for CDC.");
TAG_FLAG(cdc_ybclient_reactor_threads, advanced);

DEFINE_int32(cdc_state_checkpoint_update_interval_ms, 15 * 1000,
             "Rate at which CDC state's checkpoint is updated.");

DEFINE_string(certs_for_cdc_dir, "",
              "Directory that contains certificate authorities for CDC producer universes.");

DEFINE_int32(update_min_cdc_indices_interval_secs, 60,
             "How often to read cdc_state table to get the minimum applied index for each tablet "
             "across all streams. This information is used to correctly keep log files that "
             "contain unapplied entries. This is also the rate at which a tablet's minimum "
             "replicated index across all streams is sent to the other peers in the configuration. "
             "If flag enable_log_retention_by_op_idx is disabled, this flag has no effect.");

DEFINE_int32(update_metrics_interval_ms, 1000,
             "How often to update xDC cluster metrics.");

DEFINE_bool(enable_collect_cdc_metrics, false, "Enable collecting cdc metrics.");

DECLARE_bool(enable_log_retention_by_op_idx);

DECLARE_int32(cdc_checkpoint_opid_interval_ms);

METRIC_DEFINE_entity(cdc);

namespace yb {
namespace cdc {

using namespace std::literals;

using rpc::RpcContext;
using tserver::TSTabletManager;
using client::internal::RemoteTabletServer;

constexpr int kMaxDurationForTabletLookup = 50;
constexpr char kDefaultMetricTableName[] = "DEFAULT_TABLE_NAME";
constexpr char kDefaultMetricTableId[] = "DEFAULT_TABLE_ID";
const client::YBTableName kCdcStateTableName(
    YQL_DATABASE_CQL, master::kSystemNamespaceName, master::kCdcStateTableName);

CDCServiceImpl::CDCServiceImpl(TSTabletManager* tablet_manager,
                               const scoped_refptr<MetricEntity>& metric_entity_server,
                               MetricRegistry* metric_registry)
    : CDCServiceIf(metric_entity_server),
      tablet_manager_(tablet_manager),
      metric_registry_(metric_registry),
      server_metrics_(std::make_shared<CDCServerMetrics>(metric_entity_server)) {
  const auto server = tablet_manager->server();
  async_client_init_.emplace(
      "cdc_client", FLAGS_cdc_ybclient_reactor_threads, FLAGS_cdc_read_rpc_timeout_ms / 1000,
      server->permanent_uuid(), &server->options(), server->metric_entity(), server->mem_tracker(),
      server->messenger());
  async_client_init_->Start();

  update_peers_and_metrics_thread_.reset(new std::thread(
      &CDCServiceImpl::UpdatePeersAndMetrics, this));
}

CDCServiceImpl::~CDCServiceImpl() {
  Shutdown();
}

namespace {
bool YsqlTableHasPrimaryKey(const client::YBSchema& schema) {
  for (const auto& col : schema.columns()) {
      if (col.order() == static_cast<int32_t>(PgSystemAttrNum::kYBRowId)) {
        // ybrowid column is added for tables that don't have user-specified primary key.
        return false;
    }
  }
  return true;
}

bool IsTabletPeerLeader(const std::shared_ptr<tablet::TabletPeer>& peer) {
  return peer->LeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY;
}
} // namespace

template <class ReqType, class RespType>
bool CDCServiceImpl::CheckOnline(const ReqType* req, RespType* resp, rpc::RpcContext* rpc) {
  TRACE("Received RPC $0: $1", rpc->ToString(), req->DebugString());
  if (PREDICT_FALSE(!tablet_manager_)) {
    SetupErrorAndRespond(resp->mutable_error(),
                         STATUS(ServiceUnavailable, "Tablet Server is not running"),
                         CDCErrorPB::NOT_RUNNING,
                         rpc);
    return false;
  }
  return true;
}

void CDCServiceImpl::CreateCDCStream(const CreateCDCStreamRequestPB* req,
                                     CreateCDCStreamResponsePB* resp,
                                     RpcContext context) {
  if (!CheckOnline(req, resp, &context)) {
    return;
  }

  RPC_CHECK_AND_RETURN_ERROR(req->has_table_id(),
                             STATUS(InvalidArgument, "Table ID is required to create CDC stream"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);

  std::shared_ptr<client::YBTable> table;
  Status s = async_client_init_->client()->OpenTable(req->table_id(), &table);
  RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::TABLE_NOT_FOUND, context);

  // We don't allow CDC on YEDIS and tables without a primary key.
  if (req->record_format() != CDCRecordFormat::WAL) {
    RPC_CHECK_NE_AND_RETURN_ERROR(table->table_type(), client::YBTableType::REDIS_TABLE_TYPE,
                                  STATUS(InvalidArgument, "Cannot setup CDC on YEDIS_TABLE"),
                                  resp->mutable_error(),
                                  CDCErrorPB::INVALID_REQUEST,
                                  context);

    // Check if YSQL table has a primary key. CQL tables always have a user specified primary key.
    RPC_CHECK_AND_RETURN_ERROR(
        table->table_type() != client::YBTableType::PGSQL_TABLE_TYPE ||
        YsqlTableHasPrimaryKey(table->schema()),
        STATUS(InvalidArgument, "Cannot setup CDC on table without primary key"),
        resp->mutable_error(),
        CDCErrorPB::INVALID_REQUEST,
        context);
  }

  std::unordered_map<std::string, std::string> options;
  options.reserve(2);
  options.emplace(kRecordType, CDCRecordType_Name(req->record_type()));
  options.emplace(kRecordFormat, CDCRecordFormat_Name(req->record_format()));

  auto result = async_client_init_->client()->CreateCDCStream(req->table_id(), options);
  RPC_CHECK_AND_RETURN_ERROR(result.ok(), result.status(), resp->mutable_error(),
                             CDCErrorPB::INTERNAL_ERROR, context);

  resp->set_stream_id(*result);

  // Add stream to cache.
  AddStreamMetadataToCache(*result, std::make_shared<StreamMetadata>(req->table_id(),
                                                                     req->record_type(),
                                                                     req->record_format()));
  context.RespondSuccess();
}

void CDCServiceImpl::DeleteCDCStream(const DeleteCDCStreamRequestPB* req,
                                     DeleteCDCStreamResponsePB* resp,
                                     RpcContext context) {
  if (!CheckOnline(req, resp, &context)) {
    return;
  }

  LOG(INFO) << "Received DeleteCDCStream request " << req->ShortDebugString();

  RPC_CHECK_AND_RETURN_ERROR(req->stream_id_size() > 0,
                             STATUS(InvalidArgument, "Stream ID is required to delete CDC stream"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);

  vector<CDCStreamId> streams(req->stream_id().begin(), req->stream_id().end());
  Status s = async_client_init_->client()->DeleteCDCStream(streams);
  RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, context);

  context.RespondSuccess();
}

void CDCServiceImpl::ListTablets(const ListTabletsRequestPB* req,
                                 ListTabletsResponsePB* resp,
                                 RpcContext context) {
  if (!CheckOnline(req, resp, &context)) {
    return;
  }

  RPC_CHECK_AND_RETURN_ERROR(req->has_stream_id(),
                             STATUS(InvalidArgument, "Stream ID is required to list tablets"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);

  auto tablets = GetTablets(req->stream_id());
  RPC_CHECK_AND_RETURN_ERROR(tablets.ok(), tablets.status(), resp->mutable_error(),
                             CDCErrorPB::INTERNAL_ERROR, context);

  if (!req->local_only()) {
    resp->mutable_tablets()->Reserve(tablets->size());
  }

  for (const auto& tablet : *tablets) {
    // Filter local tablets if needed.
    if (req->local_only()) {
      bool is_local = false;
      for (const auto& replica :  tablet.replicas()) {
        if (replica.ts_info().permanent_uuid() == tablet_manager_->server()->permanent_uuid()) {
          is_local = true;
          break;
        }
      }

      if (!is_local) {
        continue;
      }
    }

    auto res = resp->add_tablets();
    res->set_tablet_id(tablet.tablet_id());
    res->mutable_tservers()->Reserve(tablet.replicas_size());
    for (const auto& replica : tablet.replicas()) {
      auto tserver =  res->add_tservers();
      tserver->mutable_broadcast_addresses()->CopyFrom(replica.ts_info().broadcast_addresses());
      if (tserver->broadcast_addresses_size() == 0) {
        LOG(WARNING) << "No public broadcast addresses found for "
                     << replica.ts_info().permanent_uuid() << ".  Using private addresses instead.";
        tserver->mutable_broadcast_addresses()->CopyFrom(replica.ts_info().private_rpc_addresses());
      }
    }
  }

  context.RespondSuccess();
}

Result<google::protobuf::RepeatedPtrField<master::TabletLocationsPB>> CDCServiceImpl::GetTablets(
    const CDCStreamId& stream_id) {
  auto stream_metadata = VERIFY_RESULT(GetStream(stream_id));
  client::YBTableName table_name;
  table_name.set_table_id(stream_metadata->table_id);
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  RETURN_NOT_OK(async_client_init_->client()->GetTablets(table_name, 0, &tablets));
  return tablets;
}

void CDCServiceImpl::GetChanges(const GetChangesRequestPB* req,
                                GetChangesResponsePB* resp,
                                RpcContext context) {
  if (!CheckOnline(req, resp, &context)) {
    return;
  }

  YB_LOG_EVERY_N_SECS(INFO, 300) << "Received GetChanges request " << req->ShortDebugString();

  RPC_CHECK_AND_RETURN_ERROR(req->has_tablet_id(),
                             STATUS(InvalidArgument, "Tablet ID is required to get CDC changes"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);
  RPC_CHECK_AND_RETURN_ERROR(req->has_stream_id(),
                             STATUS(InvalidArgument, "Stream ID is required to get CDC changes"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);

  // Check that requested tablet_id is part of the CDC stream.
  ProducerTabletInfo producer_tablet = {"" /* UUID */, req->stream_id(), req->tablet_id()};
  Status s = CheckTabletValidForStream(producer_tablet);
  RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::INVALID_REQUEST, context);

  std::shared_ptr<tablet::TabletPeer> tablet_peer;
  s = tablet_manager_->GetTabletPeer(req->tablet_id(), &tablet_peer);
  auto original_leader_term = tablet_peer ? tablet_peer->LeaderTerm() : OpId::kUnknownTerm;

  // If we we can't serve this tablet...
  if (s.IsNotFound() || tablet_peer->LeaderStatus() != consensus::LeaderStatus::LEADER_AND_READY) {
    if (req->serve_as_proxy()) {
      // Forward GetChanges() to tablet leader. This commonly happens in Kubernetes setups.
      auto context_ptr = std::make_shared<RpcContext>(std::move(context));
      TabletLeaderGetChanges(req, resp, context_ptr, tablet_peer);
    // Otherwise, figure out the proper return code.
    } else if (s.IsNotFound()) {
      SetupErrorAndRespond(resp->mutable_error(), s, CDCErrorPB::TABLET_NOT_FOUND, &context);
    } else if (tablet_peer->LeaderStatus() == consensus::LeaderStatus::NOT_LEADER) {
      // TODO: we may be able to get some changes, even if we're not the leader.
      SetupErrorAndRespond(resp->mutable_error(),
          STATUS(NotFound, Format("Not leader for $0", req->tablet_id())),
          CDCErrorPB::TABLET_NOT_FOUND, &context);
    } else {
      SetupErrorAndRespond(resp->mutable_error(),
          STATUS(LeaderNotReadyToServe, "Not ready to serve"),
          CDCErrorPB::LEADER_NOT_READY, &context);
    }
    return;
  }

  auto session = async_client_init_->client()->NewSession();
  OpId op_id;

  if (req->has_from_checkpoint()) {
    op_id = OpId::FromPB(req->from_checkpoint().op_id());
  } else {
    auto result = GetLastCheckpoint(producer_tablet, session);
    RPC_CHECK_AND_RETURN_ERROR(result.ok(), result.status(), resp->mutable_error(),
                               CDCErrorPB::INTERNAL_ERROR, context);
    op_id = *result;
  }

  auto record = GetStream(req->stream_id());
  RPC_CHECK_AND_RETURN_ERROR(record.ok(), record.status(), resp->mutable_error(),
                             CDCErrorPB::INTERNAL_ERROR, context);

  int64_t last_readable_index;
  consensus::ReplicateMsgsHolder msgs_holder;
  MemTrackerPtr mem_tracker = GetMemTracker(tablet_peer, producer_tablet);

  // Read the latest changes from the Log.
  s = cdc::GetChanges(
      req->stream_id(), req->tablet_id(), op_id, *record->get(), tablet_peer, mem_tracker,
      &msgs_holder, resp, &last_readable_index);
  RPC_STATUS_RETURN_ERROR(
      s,
      resp->mutable_error(),
      s.IsNotFound() ? CDCErrorPB::CHECKPOINT_TOO_OLD : CDCErrorPB::UNKNOWN_ERROR,
      context);

  // Verify leadership was maintained for the duration of the GetChanges() read.
  s = tablet_manager_->GetTabletPeer(req->tablet_id(), &tablet_peer);
  if (s.IsNotFound() || tablet_peer->LeaderStatus() != consensus::LeaderStatus::LEADER_AND_READY ||
      tablet_peer->LeaderTerm() != original_leader_term) {
    SetupErrorAndRespond(resp->mutable_error(),
        STATUS(NotFound, Format("Not leader for $0", req->tablet_id())),
        CDCErrorPB::TABLET_NOT_FOUND, &context);
    return;
  }

  // Store information about the last server read & remote client ACK.
  uint64_t last_record_hybrid_time = resp->records_size() > 0 ?
      resp->records(resp->records_size() - 1).time() : 0;

  s = UpdateCheckpoint(producer_tablet, OpId::FromPB(resp->checkpoint().op_id()), op_id, session,
                       last_record_hybrid_time);
  RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, context);

  {
    std::shared_ptr<consensus::Consensus> shared_consensus = tablet_peer->shared_consensus();

    RPC_CHECK_NE_AND_RETURN_ERROR(shared_consensus, nullptr,
        STATUS_SUBSTITUTE(InternalError, "Failed to get tablet $0 peer consensus",
            req->tablet_id()),
        resp->mutable_error(),
        CDCErrorPB::INTERNAL_ERROR, context);

    shared_consensus->UpdateCDCConsumerOpId(GetMinSentCheckpointForTablet(req->tablet_id()));
  }

  // Update relevant GetChanges metrics before handing off the Response.
  auto tablet_metric = GetCDCTabletMetrics(producer_tablet, tablet_peer);
  if (tablet_metric) {
    auto lid = resp->checkpoint().op_id();
    tablet_metric->last_read_opid_term->set_value(lid.term());
    tablet_metric->last_read_opid_index->set_value(lid.index());
    tablet_metric->last_readable_opid_index->set_value(last_readable_index);
    tablet_metric->last_checkpoint_opid_index->set_value(op_id.index);
    if (resp->records_size() > 0) {
      auto& last_record = resp->records(resp->records_size()-1);
      tablet_metric->last_read_hybridtime->set_value(last_record.time());
      auto last_record_micros = HybridTime(last_record.time()).GetPhysicalValueMicros();
      tablet_metric->last_read_physicaltime->set_value(last_record_micros);
      // Only count bytes responded if we are including a response payload.
      tablet_metric->rpc_payload_bytes_responded->Increment(resp->ByteSize());
      // Get the physical time of the last committed record on producer.
      auto last_replicated_micros = GetLastReplicatedTime(tablet_peer);
      tablet_metric->async_replication_sent_lag_micros->set_value(
          last_replicated_micros - last_record_micros);
      auto& first_record = resp->records(0);
      auto first_record_micros = HybridTime(first_record.time()).GetPhysicalValueMicros();
      tablet_metric->last_checkpoint_physicaltime->set_value(first_record_micros);
      tablet_metric->async_replication_committed_lag_micros->set_value(
          last_replicated_micros - first_record_micros);
    } else {
      tablet_metric->rpc_heartbeats_responded->Increment();
      // If there are no more entries to be read, that means we're caught up.
      auto last_replicated_micros = GetLastReplicatedTime(tablet_peer);
      tablet_metric->last_read_physicaltime->set_value(last_replicated_micros);
      tablet_metric->last_checkpoint_physicaltime->set_value(last_replicated_micros);
      tablet_metric->async_replication_sent_lag_micros->set_value(0);
      tablet_metric->async_replication_committed_lag_micros->set_value(0);
    }
  }

  context.RespondSuccess();
}

void CDCServiceImpl::UpdatePeersCdcMinReplicatedIndex(const TabletId& tablet_id,
                                                      int64_t min_index) {
  std::vector<client::internal::RemoteTabletServer *> servers;
  auto s = GetTServers(tablet_id, &servers);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to get remote tablet servers for tablet id " << tablet_id;
  } else {
    for (const auto &server : servers) {
      if (server->IsLocal()) {
        // We modify our log directly. Avoid calling itself through the proxy.
        continue;
      }
      LOG(INFO) << "Modifying remote peer " << server->ToString();
      auto proxy = GetCDCServiceProxy(server);
      UpdateCdcReplicatedIndexRequestPB update_index_req;
      UpdateCdcReplicatedIndexResponsePB update_index_resp;
      update_index_req.set_tablet_id(tablet_id);
      update_index_req.set_replicated_index(min_index);
      rpc::RpcController rpc;
      rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_cdc_write_rpc_timeout_ms));
      WARN_NOT_OK(proxy->UpdateCdcReplicatedIndex(update_index_req, &update_index_resp, &rpc),
                  "UpdateCdcReplicatedIndex failed");
      // For now ignore the response.
    }
  }
}

void CDCServiceImpl::UpdateLagMetrics() {
  TabletCheckpoints tablet_checkpoints;
  {
    SharedLock<decltype(mutex_)> l(mutex_);
    tablet_checkpoints = tablet_checkpoints_;
  }

  client::TableHandle table;
  auto s = table.Open(kCdcStateTableName, async_client_init_->client());
  if (!s.ok()) {
    // It is possible that this runs before the cdc_state table is created. This is
    // ok. It just means that this is the first time the cluster starts.
    YB_LOG_EVERY_N_SECS(WARNING, 30) << "Unable to open table " << kCdcStateTableName.table_name()
                                     << " for metrics update.";
    return;
  }

  std::unordered_set<ProducerTabletInfo, ProducerTabletInfo::Hash> tablets_in_cdc_state_table;
  client::TableIteratorOptions options;
  options.columns = std::vector<string>{master::kCdcTabletId, master::kCdcStreamId};
  bool failed = false;
  options.error_handler = [&failed](const Status& status) {
    YB_LOG_EVERY_N_SECS(WARNING, 30) << "Scan of table " << kCdcStateTableName.table_name()
                                     << " failed: " << status << ". Could not update metrics.";
    failed = true;
  };
  // First go through tablets in the cdc_state table and update metrics for each one.
  for (const auto& row : client::TableRange(table, options)) {
    auto tablet_id = row.column(master::kCdcTabletIdIdx).string_value();
    auto stream_id = row.column(master::kCdcStreamIdIdx).string_value();
    std::shared_ptr<tablet::TabletPeer> tablet_peer;
    Status s = tablet_manager_->GetTabletPeer(tablet_id, &tablet_peer);
    if (s.IsNotFound()) {
      continue;
    }

    ProducerTabletInfo tablet_info = {"" /* universe_uuid */, stream_id, tablet_id};
    tablets_in_cdc_state_table.insert(tablet_info);
    auto tablet_metric = GetCDCTabletMetrics(tablet_info, tablet_peer);
    if (!tablet_metric) {
      continue;
    }
    if (tablet_peer->LeaderStatus() != consensus::LeaderStatus::LEADER_AND_READY) {
      // Set lag to 0 because we're not the leader for this tablet anymore, which means another peer
      // is responsible for tracking this tablet's lag.
      tablet_metric->async_replication_sent_lag_micros->set_value(0);
      tablet_metric->async_replication_committed_lag_micros->set_value(0);
    } else {
      // Get the physical time of the last committed record on producer.
      auto last_replicated_micros = GetLastReplicatedTime(tablet_peer);

      auto last_sent_micros = tablet_metric->last_read_physicaltime->value();
      auto last_committed_micros = tablet_metric->last_checkpoint_physicaltime->value();

      tablet_metric->async_replication_sent_lag_micros->set_value(
          last_replicated_micros - last_sent_micros);
      tablet_metric->async_replication_committed_lag_micros->set_value(
          last_replicated_micros - last_committed_micros);
    }
  }
  if (failed) {
    return;
  }

  // Now, go through tablets in tablet_checkpoints_ and set lag to 0 for all tablets we're no
  // longer replicating.
  for (auto it = tablet_checkpoints.begin(); it != tablet_checkpoints.end(); it++) {
    ProducerTabletInfo tablet_info = {"" /* universe_uuid */, it->stream_id(), it->tablet_id()};
    if (tablets_in_cdc_state_table.find(tablet_info) == tablets_in_cdc_state_table.end()) {
      // We're no longer replicating this tablet, so set lag to 0.
      std::shared_ptr<tablet::TabletPeer> tablet_peer;
      Status s = tablet_manager_->GetTabletPeer(it->tablet_id(), &tablet_peer);
      if (s.IsNotFound()) {
        continue;
      }
      auto tablet_metric = GetCDCTabletMetrics(it->producer_tablet_info, tablet_peer);
      if (!tablet_metric) {
        continue;
      }
      tablet_metric->async_replication_sent_lag_micros->set_value(0);
      tablet_metric->async_replication_committed_lag_micros->set_value(0);
    }
  }
}

MicrosTime CDCServiceImpl::GetLastReplicatedTime(
    const std::shared_ptr<tablet::TabletPeer>& tablet_peer) {
  yb::tablet::RemoveIntentsData data;
  tablet_peer->GetLastReplicatedData(&data);
  return data.log_ht.GetPhysicalValueMicros();
}

void CDCServiceImpl::UpdatePeersAndMetrics() {
  MonoTime time_since_update_peers = MonoTime::kUninitialized;
  MonoTime time_since_update_metrics = MonoTime::kUninitialized;

  // Returns false if the CDC service has been stopped.
  auto sleep_while_not_stopped = [this]() {
    int min_sleep_ms = std::min(100, GetAtomicFlag(&FLAGS_update_metrics_interval_ms));
    auto sleep_period = MonoDelta::FromMilliseconds(min_sleep_ms);
    SleepFor(sleep_period);
    if (cdc_service_stopped_.load(std::memory_order_acquire)) {
      return false;
    }
    return true;
  };

  do {
    if (ANNOTATE_UNPROTECTED_READ(FLAGS_enable_collect_cdc_metrics) &&
        (time_since_update_metrics == MonoTime::kUninitialized ||
         MonoTime::Now() - time_since_update_metrics >=
             MonoDelta::FromMilliseconds(GetAtomicFlag(&FLAGS_update_metrics_interval_ms)))) {
      UpdateLagMetrics();
      time_since_update_metrics = MonoTime::Now();
    }

    // If its not been 60s since the last peer update, continue.
    if (!FLAGS_enable_log_retention_by_op_idx ||
        (time_since_update_peers != MonoTime::kUninitialized &&
         MonoTime::Now() - time_since_update_peers <
             MonoDelta::FromSeconds(GetAtomicFlag(&FLAGS_update_min_cdc_indices_interval_secs)))) {
      continue;
    }

    time_since_update_peers = MonoTime::Now();
    LOG(INFO) << "Started to read minimum replicated indices for all tablets";

    client::TableHandle table;
    auto s = table.Open(kCdcStateTableName, async_client_init_->client());
    if (!s.ok()) {
      // It is possible that this runs before the cdc_state table is created. This is
      // ok. It just means that this is the first time the cluster starts.
      YB_LOG_EVERY_N_SECS(WARNING, 3600) << "Unable to open table "
                                         << kCdcStateTableName.table_name()
                                         << ". CDC min replicated indices won't be updated";
      continue;
    }

    int count = 0;
    std::unordered_map<std::string, int64_t> tablet_min_checkpoint_index;
    client::TableIteratorOptions options;
    bool failed = false;
    options.error_handler = [&failed](const Status& status) {
      LOG(WARNING) << "Scan of table " << kCdcStateTableName.table_name() << " failed: " << status;
      failed = true;
    };
    options.columns = std::vector<std::string>{master::kCdcTabletId, master::kCdcStreamId,
        master::kCdcCheckpoint, master::kCdcLastReplicationTime};
    for (const auto& row : client::TableRange(table, options)) {
      count++;
      auto tablet_id = row.column(master::kCdcTabletIdIdx).string_value();
      auto stream_id = row.column(master::kCdcStreamIdIdx).string_value();
      auto checkpoint = row.column(master::kCdcCheckpointIdx).string_value();
      std::string last_replicated_time_str;
      const auto& timestamp_ql_value = row.column(3);
      if (!timestamp_ql_value.IsNull()) {
        last_replicated_time_str = timestamp_ql_value.timestamp_value().ToFormattedString();
      }

      VLOG(1) << "stream_id: " << stream_id << ", tablet_id: " << tablet_id
              << ", checkpoint: " << checkpoint << ", last replicated time: "
              << last_replicated_time_str;

      auto result = OpId::FromString(checkpoint);
      if (!result.ok()) {
        LOG(WARNING) << "Read invalid op id " << row.column(1).string_value()
                     << " for tablet " << tablet_id;
        continue;
      }

      auto index = (*result).index;
      auto it = tablet_min_checkpoint_index.find(tablet_id);
      if (it == tablet_min_checkpoint_index.end()) {
        tablet_min_checkpoint_index[tablet_id] = index;
      } else {
        if (index < it->second) {
          it->second = index;
        }
      }
    }
    if (failed) {
      continue;
    }
    LOG(INFO) << "Read " << count << " records from " << kCdcStateTableName.table_name();

    VLOG(3) << "tablet_min_checkpoint_index size " << tablet_min_checkpoint_index.size();
    for (const auto &elem : tablet_min_checkpoint_index) {
      auto tablet_id = elem.first;
      std::shared_ptr<tablet::TabletPeer> tablet_peer;

      Status s = tablet_manager_->GetTabletPeer(tablet_id, &tablet_peer);
      if (s.IsNotFound()) {
        VLOG(2) << "Did not found tablet peer for tablet " << tablet_id;
        continue;
      } else if (!IsTabletPeerLeader(tablet_peer)) {
        VLOG(2) << "Tablet peer " << tablet_peer->permanent_uuid()
                << " is not the leader for tablet " << tablet_id;
        continue;
      } else if (!s.ok()) {
        LOG(WARNING) << "Error getting tablet_peer for tablet " << tablet_id << ": " << s;
        continue;
      }

      auto min_index = elem.second;
      s = tablet_peer->set_cdc_min_replicated_index(min_index);
      if (!s.ok()) {
        LOG(WARNING) << "Unable to set cdc min index for tablet peer "
                     << tablet_peer->permanent_uuid()
                     << " and tablet " << tablet_peer->tablet_id()
                     << ": " << s;
      }
      LOG(INFO) << "Updating followers for tablet " << tablet_id << " with index " << min_index;
      UpdatePeersCdcMinReplicatedIndex(tablet_id, min_index);
    }
    LOG(INFO) << "Done reading all the indices for all tablets and updating peers";
  } while (sleep_while_not_stopped());
}

Result<client::internal::RemoteTabletPtr> CDCServiceImpl::GetRemoteTablet(
    const TabletId& tablet_id) {
  std::promise<Result<client::internal::RemoteTabletPtr>> tablet_lookup_promise;
  auto future = tablet_lookup_promise.get_future();
  auto callback = [&tablet_lookup_promise](
      const Result<client::internal::RemoteTabletPtr>& result) {
    tablet_lookup_promise.set_value(result);
  };

  auto start = CoarseMonoClock::Now();
  async_client_init_->client()->LookupTabletById(
      tablet_id,
      // TODO(tsplit): decide whether we need to get info about stale table partitions here.
      /* table =*/ nullptr,
      CoarseMonoClock::Now() + MonoDelta::FromMilliseconds(FLAGS_cdc_read_rpc_timeout_ms),
      callback, client::UseCache::kFalse);
  future.wait();

  auto duration = CoarseMonoClock::Now() - start;
  if (duration > (kMaxDurationForTabletLookup * 1ms)) {
    LOG(WARNING) << "LookupTabletByKey took long time: " << duration << " ms";
  }

  auto remote_tablet = VERIFY_RESULT(future.get());
  return remote_tablet;
}

Result<RemoteTabletServer *> CDCServiceImpl::GetLeaderTServer(const TabletId& tablet_id) {
  auto result = VERIFY_RESULT(GetRemoteTablet(tablet_id));

  auto ts = result->LeaderTServer();
  if (ts == nullptr) {
    return STATUS(NotFound, "Tablet leader not found for tablet", tablet_id);
  }
  return ts;
}

Status CDCServiceImpl::GetTServers(const TabletId& tablet_id,
                                   std::vector<client::internal::RemoteTabletServer*>* servers) {
  auto result = VERIFY_RESULT(GetRemoteTablet(tablet_id));

  result->GetRemoteTabletServers(servers);
  return Status::OK();
}

std::shared_ptr<CDCServiceProxy> CDCServiceImpl::GetCDCServiceProxy(RemoteTabletServer* ts) {
  auto hostport = HostPortFromPB(DesiredHostPort(
      ts->public_rpc_hostports(), ts->private_rpc_hostports(), ts->cloud_info(),
      async_client_init_->client()->cloud_info()));
  DCHECK(!hostport.host().empty());

  {
    SharedLock<decltype(mutex_)> l(mutex_);
    auto it = cdc_service_map_.find(hostport);
    if (it != cdc_service_map_.end()) {
      return it->second;
    }
  }

  auto cdc_service = std::make_shared<CDCServiceProxy>(&async_client_init_->client()->proxy_cache(),
                                                       hostport);
  {
    std::lock_guard<decltype(mutex_)> l(mutex_);
    cdc_service_map_.emplace(hostport, cdc_service);
  }
  return cdc_service;
}

void CDCServiceImpl::TabletLeaderGetChanges(const GetChangesRequestPB* req,
                                            GetChangesResponsePB* resp,
                                            std::shared_ptr<RpcContext> context,
                                            std::shared_ptr<tablet::TabletPeer> peer) {
  auto rpc_handle = rpcs_.Prepare();
  RPC_CHECK_AND_RETURN_ERROR(rpc_handle != rpcs_.InvalidHandle(),
      STATUS(Aborted,
          Format("Could not create valid handle for GetChangesCDCRpc: tablet=$0, peer=$1",
                 req->tablet_id(),
                 peer->permanent_uuid())),
      resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, *context.get());

  // Increment Proxy Metric.
  server_metrics_->cdc_rpc_proxy_count->Increment();

  // Forward this Request Info to the proper TabletServer.
  GetChangesRequestPB new_req;
  new_req.CopyFrom(*req);
  new_req.set_serve_as_proxy(false);
  CoarseTimePoint deadline = context->GetClientDeadline();
  if (deadline == CoarseTimePoint::max()) { // Not specified by user.
    deadline = CoarseMonoClock::now() + async_client_init_->client()->default_rpc_timeout();
  }
  *rpc_handle = CreateGetChangesCDCRpc(
      deadline,
      nullptr, /* RemoteTablet: will get this from 'new_req' */
      async_client_init_->client(),
      &new_req,
      [=] (Status status, GetChangesResponsePB&& new_resp) {
        auto retained = rpcs_.Unregister(rpc_handle);
        *resp = std::move(new_resp);
        RPC_STATUS_RETURN_ERROR(status, resp->mutable_error(), resp->error().code(),
                                *context.get());
        context->RespondSuccess();
      });
  (**rpc_handle).SendRpc();
}

void CDCServiceImpl::TabletLeaderGetCheckpoint(const GetCheckpointRequestPB* req,
                                               GetCheckpointResponsePB* resp,
                                               RpcContext* context,
                                               const std::shared_ptr<tablet::TabletPeer>& peer) {
  auto result = GetLeaderTServer(req->tablet_id());
  RPC_CHECK_AND_RETURN_ERROR(result.ok(), result.status(), resp->mutable_error(),
                             CDCErrorPB::TABLET_NOT_FOUND, *context);

  auto ts_leader = *result;
  // Check that tablet leader identified by master is not current tablet peer.
  // This can happen during tablet rebalance if master and tserver have different views of
  // leader. We need to avoid self-looping in this case.
  if (peer) {
    RPC_CHECK_NE_AND_RETURN_ERROR(ts_leader->permanent_uuid(), peer->permanent_uuid(),
                                  STATUS(IllegalState,
                                         Format("Tablet leader changed: leader=$0, peer=$1",
                                                ts_leader->permanent_uuid(),
                                                peer->permanent_uuid())),
                                  resp->mutable_error(), CDCErrorPB::NOT_LEADER, *context);
  }

  auto cdc_proxy = GetCDCServiceProxy(ts_leader);
  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_cdc_read_rpc_timeout_ms));
  // TODO(NIC): Change to GetCheckpointAsync like CDCPoller::DoPoll.
  auto status = cdc_proxy->GetCheckpoint(*req, resp, &rpc);
  RPC_STATUS_RETURN_ERROR(status, resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, *context);
  context->RespondSuccess();
}

void CDCServiceImpl::GetCheckpoint(const GetCheckpointRequestPB* req,
                                   GetCheckpointResponsePB* resp,
                                   RpcContext context) {
  if (!CheckOnline(req, resp, &context)) {
    return;
  }

  RPC_CHECK_AND_RETURN_ERROR(req->has_tablet_id(),
                             STATUS(InvalidArgument, "Tablet ID is required to get CDC checkpoint"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);
  RPC_CHECK_AND_RETURN_ERROR(req->has_stream_id(),
                             STATUS(InvalidArgument, "Stream ID is required to get CDC checkpoint"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);

  std::shared_ptr<tablet::TabletPeer> tablet_peer;
  Status s = tablet_manager_->GetTabletPeer(req->tablet_id(), &tablet_peer);

  if (s.IsNotFound() || !IsTabletPeerLeader(tablet_peer)) {
    // Forward GetChanges() to tablet leader. This happens often in Kubernetes setups.
    TabletLeaderGetCheckpoint(req, resp, &context, tablet_peer);
    return;
  }

  // Check that requested tablet_id is part of the CDC stream.
  ProducerTabletInfo producer_tablet = {"" /* UUID */, req->stream_id(), req->tablet_id()};
  s = CheckTabletValidForStream(producer_tablet);
  RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::INVALID_REQUEST, context);

  auto session = async_client_init_->client()->NewSession();

  auto result = GetLastCheckpoint(producer_tablet, session);
  RPC_CHECK_AND_RETURN_ERROR(result.ok(), result.status(), resp->mutable_error(),
                             CDCErrorPB::INTERNAL_ERROR, context);

  result->ToPB(resp->mutable_checkpoint()->mutable_op_id());
  context.RespondSuccess();
}

void CDCServiceImpl::UpdateCdcReplicatedIndex(const UpdateCdcReplicatedIndexRequestPB* req,
                                              UpdateCdcReplicatedIndexResponsePB* resp,
                                              rpc::RpcContext context) {
  if (!CheckOnline(req, resp, &context)) {
    return;
  }

  RPC_CHECK_AND_RETURN_ERROR(req->has_tablet_id(),
                             STATUS(InvalidArgument,
                                    "Tablet ID is required to set the log replicated index"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);

  RPC_CHECK_AND_RETURN_ERROR(req->has_replicated_index(),
                             STATUS(InvalidArgument,
                                    "Replicated index is required to set the log replicated index"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);

  std::shared_ptr<tablet::TabletPeer> tablet_peer;
  RPC_STATUS_RETURN_ERROR(tablet_manager_->GetTabletPeer(req->tablet_id(), &tablet_peer),
                          resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, context);

  RPC_CHECK_AND_RETURN_ERROR(tablet_peer->log_available(),
                             STATUS(TryAgain, "Tablet peer is not ready to set its log cdc index"),
                             resp->mutable_error(),
                             CDCErrorPB::INTERNAL_ERROR,
                             context);

  RPC_STATUS_RETURN_ERROR(tablet_peer->set_cdc_min_replicated_index(req->replicated_index()),
                          resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, context);

  context.RespondSuccess();
}

  Result<OpId> CDCServiceImpl::TabletLeaderLatestEntryOpId(const TabletId& tablet_id) {
    auto ts_leader = VERIFY_RESULT(GetLeaderTServer(tablet_id));

    auto cdc_proxy = GetCDCServiceProxy(ts_leader);
    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_cdc_read_rpc_timeout_ms));
    GetLatestEntryOpIdRequestPB req;
    GetLatestEntryOpIdResponsePB resp;
    req.set_tablet_id(tablet_id);
    auto status = cdc_proxy->GetLatestEntryOpId(req, &resp, &rpc);
    if (!status.ok()) {
      // If we failed to get the latest entry op id, we try other tservers. The leader is guaranteed
      // to have the most up-to-date information, but for our purposes, it's ok to be slightly
      // behind.
      std::vector<client::internal::RemoteTabletServer *> servers;
      auto s = GetTServers(tablet_id, &servers);
      for (const auto& server : servers) {
        // We don't want to try the leader again.
        if (server->permanent_uuid() == ts_leader->permanent_uuid()) {
          continue;
        }
        auto follower_cdc_proxy = GetCDCServiceProxy(server);
        status = follower_cdc_proxy->GetLatestEntryOpId(req, &resp, &rpc);
        if (status.ok()) {
          return OpId::FromPB(resp.op_id());
        }
      }
      DCHECK(!status.ok());
      return status;
    }
    return OpId::FromPB(resp.op_id());
  }

void CDCServiceImpl::GetLatestEntryOpId(const GetLatestEntryOpIdRequestPB* req,
                                        GetLatestEntryOpIdResponsePB* resp,
                                        rpc::RpcContext context) {
  std::shared_ptr<tablet::TabletPeer> tablet_peer;
  Status s = tablet_manager_->GetTabletPeer(req->tablet_id(), &tablet_peer);
  RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, context);

  if (!tablet_peer->log_available()) {
    const string err_message = strings::Substitute("Unable to get the latest entry op id from "
        "peer $0 and tablet $1 because its log object hasn't been initialized",
        tablet_peer->permanent_uuid(), tablet_peer->tablet_id());
    LOG(WARNING) << err_message;
    SetupErrorAndRespond(resp->mutable_error(),
                         STATUS(ServiceUnavailable, err_message),
                         CDCErrorPB::INTERNAL_ERROR,
                         &context);
    return;
  }
  OpId op_id = tablet_peer->log()->GetLatestEntryOpId();
  op_id.ToPB(resp->mutable_op_id());
  context.RespondSuccess();
}

void CDCServiceImpl::BootstrapProducer(const BootstrapProducerRequestPB* req,
                                       BootstrapProducerResponsePB* resp,
                                       rpc::RpcContext context) {
  LOG(INFO) << "Received BootstrapProducer request " << req->ShortDebugString();
  RPC_CHECK_AND_RETURN_ERROR(req->table_ids().size() > 0,
                             STATUS(InvalidArgument, "Table ID is required to create CDC stream"),
                             resp->mutable_error(),
                             CDCErrorPB::INVALID_REQUEST,
                             context);

  client::TableHandle cdc_state_table;

  std::vector<client::YBOperationPtr> ops;
  auto session = async_client_init_->client()->NewSession();

  // Used to delete streams in case of failure.
  std::vector<CDCStreamId> created_cdc_streams;
  auto scope_exit = ScopeExit([this, &created_cdc_streams] {
    if (!created_cdc_streams.empty()) {
      Status s = async_client_init_->client()->DeleteCDCStream(created_cdc_streams);
      if (!s.ok()) {
        LOG(WARNING) << "Unable to delete streams " << JoinCSVLine(created_cdc_streams)
                     << ": " << s;
      }
    }
  });

  std::vector<CDCStreamId> bootstrap_ids;

  for (const auto& table_id : req->table_ids()) {
    std::shared_ptr<client::YBTable> table;
    Status s = async_client_init_->client()->OpenTable(table_id, &table);
    RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::TABLE_NOT_FOUND, context);

    // Generate a bootstrap id by calling CreateCDCStream, and also setup the stream in the master.
    // If the consumer's master sends a CreateCDCStream with a bootstrap id, the producer's master
    // will verify that the stream id exists and return success if it does since everything else
    // has already been done by this call.
    std::unordered_map<std::string, std::string> options;
    options.reserve(2);
    options.emplace(cdc::kRecordType, CDCRecordType_Name(cdc::CDCRecordType::CHANGE));
    options.emplace(cdc::kRecordFormat, CDCRecordFormat_Name(cdc::CDCRecordFormat::WAL));

    auto result = async_client_init_->client()->CreateCDCStream(table_id, options);
    RPC_CHECK_AND_RETURN_ERROR(result.ok(), result.status(), resp->mutable_error(),
                               CDCErrorPB::INTERNAL_ERROR, context);
    const std::string& bootstrap_id = *result;
    created_cdc_streams.push_back(bootstrap_id);

    if (!cdc_state_table.table()) {
      Status s = cdc_state_table.Open(kCdcStateTableName, async_client_init_->client());
      RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, context);
    }

    google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
    s = async_client_init_->client()->GetTabletsFromTableId(table_id, 0, &tablets);
    RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::TABLE_NOT_FOUND, context);

    // For each tablet, create a row in cdc_state table containing the generated bootstrap id, and
    // the latest op id in the logs.
    for (const auto &tablet : tablets) {
      std::shared_ptr<tablet::TabletPeer> tablet_peer;
      OpId op_id;

      s = tablet_manager_->GetTabletPeer(tablet.tablet_id(), &tablet_peer);
      if (!s.ok()) {
        auto result = TabletLeaderLatestEntryOpId(tablet.tablet_id());
        RPC_CHECK_AND_RETURN_ERROR(result.ok(), result.status(), resp->mutable_error(),
            CDCErrorPB::INTERNAL_ERROR, context);
        op_id = *result;
      } else {
        if (!tablet_peer->log_available()) {
          const string err_message = strings::Substitute("Unable to get the latest entry op id "
              "from peer $0 and tablet $1 because its log object hasn't been initialized",
              tablet_peer->permanent_uuid(), tablet_peer->tablet_id());
          LOG(WARNING) << err_message;
          SetupErrorAndRespond(resp->mutable_error(),
                               STATUS(ServiceUnavailable, err_message),
                               CDCErrorPB::INTERNAL_ERROR,
                               &context);
          return;
        }
        op_id = tablet_peer->log()->GetLatestEntryOpId();
      }

      const auto op = cdc_state_table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
      auto *const write_req = op->mutable_request();

      QLAddStringHashValue(write_req, tablet.tablet_id());
      QLAddStringRangeValue(write_req, bootstrap_id);
      cdc_state_table.AddStringColumnValue(write_req, master::kCdcCheckpoint, op_id.ToString());
      ops.push_back(std::move(op));

      {
        ProducerTabletInfo producer_tablet{
          "" /* Universe UUID */, bootstrap_id, tablet.tablet_id()};
        auto now = CoarseMonoClock::Now();
        TabletCheckpoint sent_checkpoint({op_id, now});
        TabletCheckpoint commit_checkpoint({op_id, now});
        std::lock_guard<decltype(mutex_)> l(mutex_);
        tablet_checkpoints_.emplace(producer_tablet, commit_checkpoint, sent_checkpoint);
      }
    }
    bootstrap_ids.push_back(std::move(bootstrap_id));
  }
  Status s = session->ApplyAndFlush(ops);
  RPC_STATUS_RETURN_ERROR(s, resp->mutable_error(), CDCErrorPB::INTERNAL_ERROR, context);

  for (const auto& bootstrap_id : bootstrap_ids) {
    resp->add_cdc_bootstrap_ids(bootstrap_id);
  }
  // Clear this vector so no streams are deleted by scope_exit since we succeeded.
  created_cdc_streams.clear();
  context.RespondSuccess();
}

void CDCServiceImpl::Shutdown() {
  if (async_client_init_) {
    async_client_init_->Shutdown();
    rpcs_.Shutdown();
    if (update_peers_and_metrics_thread_) {
      cdc_service_stopped_.store(true, std::memory_order_release);
      update_peers_and_metrics_thread_->join();
    }
    async_client_init_ = boost::none;
  }
}

Result<OpId> CDCServiceImpl::GetLastCheckpoint(
    const ProducerTabletInfo& producer_tablet,
    const std::shared_ptr<client::YBSession>& session) {
  {
    SharedLock<decltype(mutex_)> l(mutex_);
    auto it = tablet_checkpoints_.find(producer_tablet);
    if (it != tablet_checkpoints_.end()) {
      // Use checkpoint from cache only if it is current.
      if (it->cdc_state_checkpoint.op_id.index > 0 &&
          CoarseMonoClock::Now() - it->cdc_state_checkpoint.last_update_time <=
              (FLAGS_cdc_state_checkpoint_update_interval_ms * 1ms)) {
        return it->cdc_state_checkpoint.op_id;
      }
    }
  }

  client::TableHandle table;
  RETURN_NOT_OK(table.Open(kCdcStateTableName, async_client_init_->client()));

  const auto op = table.NewReadOp();
  auto* const req = op->mutable_request();
  DCHECK(!producer_tablet.stream_id.empty() && !producer_tablet.tablet_id.empty());
  QLAddStringHashValue(req, producer_tablet.tablet_id);

  auto cond = req->mutable_where_expr()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddStringCondition(cond, Schema::first_column_id() + master::kCdcStreamIdIdx,
      QL_OP_EQUAL, producer_tablet.stream_id);
  req->mutable_column_refs()->add_ids(Schema::first_column_id() + master::kCdcTabletIdIdx);
  req->mutable_column_refs()->add_ids(Schema::first_column_id() + master::kCdcStreamIdIdx);
  table.AddColumns({master::kCdcCheckpoint}, req);

  RETURN_NOT_OK(session->ReadSync(op));
  auto row_block = ql::RowsResult(op.get()).GetRowBlock();
  if (row_block->row_count() == 0) {
    return OpId(0, 0);
  }

  DCHECK_EQ(row_block->row_count(), 1);
  DCHECK_EQ(row_block->row(0).column(0).type(), InternalType::kStringValue);

  return OpId::FromString(row_block->row(0).column(0).string_value());
}

Status CDCServiceImpl::UpdateCheckpoint(const ProducerTabletInfo& producer_tablet,
                                        const OpId& sent_op_id,
                                        const OpId& commit_op_id,
                                        const std::shared_ptr<client::YBSession>& session,
                                        uint64_t last_record_hybrid_time) {
  bool update_cdc_state = true;
  auto now = CoarseMonoClock::Now();
  TabletCheckpoint sent_checkpoint({sent_op_id, now});
  TabletCheckpoint commit_checkpoint({commit_op_id, now});

  {
    std::lock_guard<decltype(mutex_)> l(mutex_);
    auto it = tablet_checkpoints_.find(producer_tablet);
    if (it != tablet_checkpoints_.end()) {
      it->sent_checkpoint = sent_checkpoint;

      if (commit_op_id.index > 0) {
        it->cdc_state_checkpoint.op_id = commit_op_id;
      }

      // Check if we need to update cdc_state table.
      if (now - it->cdc_state_checkpoint.last_update_time <=
          (FLAGS_cdc_state_checkpoint_update_interval_ms * 1ms)) {
        update_cdc_state = false;
      } else {
        it->cdc_state_checkpoint.last_update_time = now;
      }
    } else {
      tablet_checkpoints_.emplace(producer_tablet, commit_checkpoint, sent_checkpoint);
    }
  }

  if (update_cdc_state) {
    client::TableHandle table;
    RETURN_NOT_OK(table.Open(kCdcStateTableName, async_client_init_->client()));
    const auto op = table.NewUpdateOp();
    auto* const req = op->mutable_request();
    DCHECK(!producer_tablet.stream_id.empty() && !producer_tablet.tablet_id.empty());
    QLAddStringHashValue(req, producer_tablet.tablet_id);
    QLAddStringRangeValue(req, producer_tablet.stream_id);
    table.AddStringColumnValue(req, master::kCdcCheckpoint, commit_op_id.ToString());
    // If we have a last record hybrid time, use that for physical time. If not, it means we're
    // caught up, so the current time.
    uint64_t last_replication_time_micros = last_record_hybrid_time != 0 ?
        HybridTime(last_record_hybrid_time).GetPhysicalValueMicros() : GetCurrentTimeMicros();
    table.AddTimestampColumnValue(
        req, master::kCdcLastReplicationTime,
        last_replication_time_micros);
    RETURN_NOT_OK(session->ApplyAndFlush(op));
  }

  return Status::OK();
}

OpId CDCServiceImpl::GetMinSentCheckpointForTablet(const std::string& tablet_id) {
  OpId min_op_id = OpId::Max();
  auto now = CoarseMonoClock::Now();

  SharedLock<rw_spinlock> l(mutex_);
  auto it_range = tablet_checkpoints_.get<TabletTag>().equal_range(tablet_id);
  if (it_range.first == it_range.second) {
    LOG(WARNING) << "Tablet ID not found in stream_tablets map: " << tablet_id;
    return min_op_id;
  }

  auto cdc_checkpoint_opid_interval = FLAGS_cdc_checkpoint_opid_interval_ms * 1ms;
  for (auto it = it_range.first; it != it_range.second; ++it) {
    // We don't want to include streams that are not being actively polled.
    // So, if the stream has not been polled in the last x seconds,
    // then we ignore that stream while calculating min op ID.
    if (now - it->sent_checkpoint.last_update_time <= cdc_checkpoint_opid_interval &&
        it->sent_checkpoint.op_id.index < min_op_id.index) {
      min_op_id = it->sent_checkpoint.op_id;
    }
  }
  return min_op_id;
}

std::shared_ptr<CDCTabletMetrics> CDCServiceImpl::GetCDCTabletMetrics(
    const ProducerTabletInfo& producer,
    std::shared_ptr<tablet::TabletPeer> tablet_peer) {
  // 'nullptr' not recommended: using for tests.
  if (tablet_peer == nullptr) {
    auto status = tablet_manager_->GetTabletPeer(producer.tablet_id, &tablet_peer);
    if (!status.ok() || tablet_peer == nullptr) return nullptr;
  }

  auto tablet = tablet_peer->shared_tablet();
  if (tablet == nullptr) return nullptr;

  std::string key = "CDCMetrics::" + producer.stream_id;
  std::shared_ptr<void> metrics_raw = tablet->GetAdditionalMetadata(key);
  if (metrics_raw == nullptr) {
    //  Create a new METRIC_ENTITY_cdc here.
    MetricEntity::AttributeMap attrs;
    {
      SharedLock<rw_spinlock> l(mutex_);
      auto it = stream_metadata_.find(producer.stream_id);
      attrs["table_id"] = it != stream_metadata_.end() ?
          it->second->table_id : kDefaultMetricTableId;
      // Todo(Rahul): Right now, we don't easily expose table name from the producer.
      // Populate this table name when we expose per table stats.
      attrs["table_name"] = kDefaultMetricTableName;
      attrs["stream_id"] = producer.stream_id;
    }
    auto entity = METRIC_ENTITY_cdc.Instantiate(metric_registry_,
        std::to_string(ProducerTabletInfo::Hash {}(producer)), attrs);
    metrics_raw = std::make_shared<CDCTabletMetrics>(entity);
    // Adding the new metric to the tablet so it maintains the same lifetime scope.
    tablet->AddAdditionalMetadata(key, metrics_raw);
  }

  return std::static_pointer_cast<CDCTabletMetrics>(metrics_raw);
}

OpId CDCServiceImpl::GetMinAppliedCheckpointForTablet(
    const std::string& tablet_id,
    const std::shared_ptr<client::YBSession>& session) {

  OpId min_op_id = OpId::Max();
  bool min_op_id_updated = false;

  {
    SharedLock<rw_spinlock> l(mutex_);
    // right => multimap where keys are tablet_ids and values are stream_ids.
    // left => multimap where keys are stream_ids and values are tablet_ids.
    auto it_range = tablet_checkpoints_.get<TabletTag>().equal_range(tablet_id);
    if (it_range.first != it_range.second) {
      // Iterate over all the streams for this tablet.
      for (auto it = it_range.first; it != it_range.second; ++it) {
        if (it->cdc_state_checkpoint.op_id.index < min_op_id.index) {
          min_op_id = it->cdc_state_checkpoint.op_id;
          min_op_id_updated = true;
        }
      }
    } else {
      VLOG(2) << "Didn't find any streams for tablet " << tablet_id;
    }
  }
  if (min_op_id_updated) {
    return min_op_id;
  }

  LOG(INFO) << "Unable to find checkpoint for tablet " << tablet_id << " in the cache";
  min_op_id = OpId();

  // We didn't find any streams for this tablet in the cache.
  // Let's read the cdc_state table and save this information in the cache so that it can be used
  // next time.
  client::TableHandle table;
  auto s = table.Open(kCdcStateTableName, async_client_init_->client());
  if (!s.ok()) {
    YB_LOG_EVERY_N(WARNING, 30) << "Unable to open table " << kCdcStateTableName.table_name();
    // Return consensus::MinimumOpId()
    return min_op_id;
  }

  const auto op = table.NewReadOp();
  auto* const req = op->mutable_request();
  QLAddStringHashValue(req, tablet_id);
  table.AddColumns({master::kCdcCheckpoint, master::kCdcStreamId}, req);
  if (!session->ApplyAndFlush(op).ok()) {
    YB_LOG_EVERY_N(WARNING, 30) << "Unable to read table " << kCdcStateTableName.table_name();
    // Return consensus::MinimumOpId()
    return min_op_id;
  }

  auto row_block = ql::RowsResult(op.get()).GetRowBlock();
  if (row_block->row_count() == 0) {
    YB_LOG_EVERY_N(WARNING, 30) << "Unable to find any cdc record for tablet " << tablet_id
                                 << " in table " << kCdcStateTableName.table_name();
    // Return consensus::MinimumOpId()
    return min_op_id;
  }

  DCHECK_EQ(row_block->row(0).column(0).type(), InternalType::kStringValue);

  auto min_index = consensus::MaximumOpId().index();
  for (const auto& row : row_block->rows()) {
    std::string stream_id = row.column(1).string_value();
    auto result = OpId::FromString(row.column(0).string_value());
    if (!result.ok()) {
      LOG(WARNING) << "Invalid checkpoint " << row.column(0).string_value()
                   << " for tablet " << tablet_id << " and stream " << stream_id;
      continue;
    }

    auto index = (*result).index;
    auto term = (*result).term;

    if (index < min_index) {
      min_op_id.term = term;
      min_op_id.index = index;
    }

    // If the checkpoints cache hasn't been updated yet, update it so we don't have to read
    // the table next time we get a request for this tablet.
    std::lock_guard<rw_spinlock> l(mutex_);
    ProducerTabletInfo producer_tablet{"" /*UUID*/, stream_id, tablet_id};
    auto cached_checkpoint = tablet_checkpoints_.find(producer_tablet);
    if (cached_checkpoint == tablet_checkpoints_.end()) {
      std::chrono::time_point<CoarseMonoClock> min_clock =
          CoarseMonoClock::time_point(CoarseMonoClock::duration(0));
      OpId checkpoint_op_id(term, index);
      TabletCheckpoint commit_checkpoint({checkpoint_op_id, min_clock});
      tablet_checkpoints_.emplace(producer_tablet, commit_checkpoint, commit_checkpoint);
    }
  }

  return min_op_id;
}

Result<std::shared_ptr<StreamMetadata>> CDCServiceImpl::GetStream(const std::string& stream_id) {
  auto stream = GetStreamMetadataFromCache(stream_id);
  if (stream != nullptr) {
    return stream;
  }

  // Look up stream in sys catalog.
  TableId table_id;
  std::unordered_map<std::string, std::string> options;
  RETURN_NOT_OK(async_client_init_->client()->GetCDCStream(stream_id, &table_id, &options));

  auto stream_metadata = std::make_shared<StreamMetadata>();;
  stream_metadata->table_id = table_id;
  for (const auto& option : options) {
    if (option.first == kRecordType) {
      SCHECK(CDCRecordType_Parse(option.second, &stream_metadata->record_type),
             IllegalState, "CDC record type parsing error");
    } else if (option.first == kRecordFormat) {
      SCHECK(CDCRecordFormat_Parse(option.second, &stream_metadata->record_format),
             IllegalState, "CDC record format parsing error");
    } else {
      LOG(WARNING) << "Unsupported CDC option: " << option.first;
    }
  }

  AddStreamMetadataToCache(stream_id, stream_metadata);
  return stream_metadata;
}

void CDCServiceImpl::AddStreamMetadataToCache(const std::string& stream_id,
                                              const std::shared_ptr<StreamMetadata>& metadata) {
  std::lock_guard<decltype(mutex_)> l(mutex_);
  stream_metadata_.emplace(stream_id, metadata);
}

std::shared_ptr<StreamMetadata> CDCServiceImpl::GetStreamMetadataFromCache(
    const std::string& stream_id) {
  SharedLock<decltype(mutex_)> l(mutex_);
  auto it = stream_metadata_.find(stream_id);
  if (it != stream_metadata_.end()) {
    return it->second;
  } else {
    return nullptr;
  }
}

MemTrackerPtr CDCServiceImpl::GetMemTracker(
    const std::shared_ptr<tablet::TabletPeer>& tablet_peer,
    const ProducerTabletInfo& producer_info) {
  SharedLock<rw_spinlock> l(mutex_);
  auto it = tablet_checkpoints_.find(producer_info);
  if (it == tablet_checkpoints_.end()) {
    return nullptr;
  }
  if (!it->mem_tracker) {
    auto cdc_mem_tracker = MemTracker::FindOrCreateTracker(
        "CDC", tablet_peer->tablet()->mem_tracker());
    const_cast<MemTrackerPtr&>(it->mem_tracker) = MemTracker::FindOrCreateTracker(
        producer_info.stream_id, cdc_mem_tracker);
  }
  return it->mem_tracker;
}

Status CDCServiceImpl::CheckTabletValidForStream(const ProducerTabletInfo& info) {
  {
    SharedLock<rw_spinlock> l(mutex_);
    if (tablet_checkpoints_.count(info) != 0) {
      return Status::OK();
    }
    const auto& stream_index = tablet_checkpoints_.get<StreamTag>();
    if (stream_index.find(info.stream_id) != stream_index.end()) {
      // Did not find matching tablet ID.
      return STATUS_FORMAT(InvalidArgument, "Tablet ID $0 is not part of stream ID $1",
                           info.tablet_id, info.stream_id);
    }
  }

  // If we don't recognize the stream_id, populate our full tablet list for this stream.
  auto tablets = VERIFY_RESULT(GetTablets(info.stream_id));
  bool found = false;
  {
    std::lock_guard<rw_spinlock> l(mutex_);
    for (const auto &tablet : tablets) {
      // Add every tablet in the stream.
      ProducerTabletInfo producer_info{info.universe_uuid, info.stream_id, tablet.tablet_id()};
      tablet_checkpoints_.emplace(producer_info, TabletCheckpoint(), TabletCheckpoint());
      // If this is the tablet that the user requested.
      if (tablet.tablet_id() == info.tablet_id) {
        found = true;
      }
    }
  }
  return found ? Status::OK()
               : STATUS_FORMAT(InvalidArgument, "Tablet ID $0 is not part of stream ID $1",
                               info.tablet_id, info.stream_id);
}

}  // namespace cdc
}  // namespace yb
