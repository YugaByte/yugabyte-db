// Copyright (c) YugabyteDB, Inc.
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

#include <algorithm>
#include <iterator>

#include <google/protobuf/repeated_field.h>

#include "yb/common/common_flags.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/pg_catversions.h"

#include "yb/consensus/metadata.pb.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/quorum_util.h"

#include "yb/master/async_rpc_tasks.h"
#include "yb/master/catalog_entity_info.pb.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/catalog_manager_util.h"
#include "yb/master/leader_epoch.h"
#include "yb/master/master.h"
#include "yb/master/master_heartbeat.pb.h"
#include "yb/master/master_heartbeat.service.h"
#include "yb/master/master_tablet_service.h"
#include "yb/master/master_service_base.h"
#include "yb/master/master_util.h"
#include "yb/master/scoped_leader_shared_lock-internal.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"
#include "yb/master/xcluster/xcluster_manager_if.h"
#include "yb/master/yql_partitions_vtable.h"
#include "yb/master/ysql/ysql_manager_if.h"

#include "yb/rpc/rpc_context.h"

#include "yb/tserver/service_util.h"

#include "yb/util/debug/trace_event.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"

DEFINE_UNKNOWN_int32(tablet_report_limit, 1000,
             "Max Number of tablets to report during a single heartbeat. "
             "If this is set to INT32_MAX, then heartbeat will report all dirty tablets.");
TAG_FLAG(tablet_report_limit, advanced);

DECLARE_bool(ysql_yb_enable_listen_notify);

DEFINE_test_flag(string, master_universe_uuid, "",
                 "When set, use this mocked uuid to compare against the universe_uuid "
                 "from the tserver request.");

DEFINE_RUNTIME_AUTO_bool(master_enable_universe_uuid_heartbeat_check, kLocalPersisted, false, true,
    "When true, enables a sanity check between masters and tservers to prevent tservers from "
    "mistakenly heartbeating to masters in different universes. Master leader will check the "
    "universe_uuid passed by tservers against with its own copy of universe_uuid "
    "and reject the request if there is a mismatch. Master sends its universe_uuid with the "
    "response.");
TAG_FLAG(master_enable_universe_uuid_heartbeat_check, advanced);

DEFINE_RUNTIME_AUTO_bool(use_tablet_report_pending_config_op_id, kLocalVolatile, false, true,
    "When true, allows master to rely on pending_config_op_id received within TServer->Master "
    "heartbeats.");

DEFINE_test_flag(bool, skip_processing_tablet_metadata, false,
                 "Whether to skip processing tablet metadata for TSHeartbeat.");

DEFINE_NON_RUNTIME_uint32(xcluster_guarded_lease_duration_ms, 2 * 60 * 1000,
    "Duration of xCluster-guarded information lease in milliseconds; not safe to lower.");
TAG_FLAG(xcluster_guarded_lease_duration_ms, advanced);

DEFINE_RUNTIME_int32(catalog_manager_report_batch_size, 1,
    "The max number of tablets evaluated in the heartbeat as a single SysCatalog update.");
TAG_FLAG(catalog_manager_report_batch_size, advanced);

DEFINE_RUNTIME_int32(master_ts_heartbeat_long_operation_warning_ms, 1000,
    "Log a warning (and dump the handler thread's stack) if processing a TSHeartbeat RPC on the "
    "master takes longer than this many ms.");

DEFINE_RUNTIME_bool(catalog_manager_wait_for_new_tablets_to_elect_leader, true,
    "Whether the catalog manager should wait for a newly created tablet to "
    "elect a leader before considering it successfully created. "
    "This is disabled in some tests where we explicitly manage leader "
    "election.");
TAG_FLAG(catalog_manager_wait_for_new_tablets_to_elect_leader, hidden);

DEFINE_RUNTIME_double(heartbeat_safe_deadline_ratio, .20,
    "When the heartbeat deadline has this percentage of time remaining, "
    "the master should halt tablet report processing so it can respond in time.");

DEFINE_RUNTIME_bool(master_enable_deletion_check_for_orphaned_tablets, true,
    "When set, this flag adds stricter protection around the deletion of orphaned tablets. When "
    "master leader is processing a tablet report and doesn't know about a tablet, explicitly "
    "check that the tablet has been deleted in the past. If it has, then issue a DeleteTablet "
    "to the tservers. Otherwise, it means that tserver has heartbeated to the wrong cluster, "
    "or there has been sys catalog corruption. In this case, log an error but don't actually "
    "delete any data.");

// Temporary.  Can be removed after long-run testing.
// TODO: how temporary is this?
DEFINE_RUNTIME_bool(master_ignore_stale_cstate, true,
    "Whether Master processes the raft config when the version is lower.");
TAG_FLAG(master_ignore_stale_cstate, hidden);

DEFINE_RUNTIME_bool(master_tombstone_evicted_tablet_replicas, true,
    "Whether the Master should tombstone (delete) tablet replicas that "
    "are no longer part of the latest reported raft config.");
TAG_FLAG(master_tombstone_evicted_tablet_replicas, hidden);

DEFINE_RUNTIME_uint32(maximum_tablet_leader_lease_expired_secs, 2 * 60,
    "If the leader lease in master's view has expired for this amount of seconds, "
    "treat the lease as expired for too long time.");

DEFINE_RUNTIME_bool(use_create_table_leader_hint, true,
    "Whether the Master should hint which replica for each tablet should "
    "be leader initially on tablet creation.");

DEFINE_RUNTIME_bool(send_leader_blacklisted_tservers_on_heartbeat, true,
    "When set, the master will send the list of leader-blacklisted tservers with no leaders "
    "on the heartbeat response.");
TAG_FLAG(send_leader_blacklisted_tservers_on_heartbeat, advanced);

DEFINE_test_flag(uint64, inject_latency_during_tablet_report_ms, 0,
                 "Number of milliseconds to sleep during the processing of a tablet batch.");

DEFINE_test_flag(bool, simulate_sys_catalog_data_loss, false,
    "On the heartbeat processing path, simulate a scenario where tablet metadata is missing due to "
    "a corruption. ");

DECLARE_bool(enable_db_history_retention_pins);
DECLARE_bool(enable_heartbeat_pg_catalog_versions_cache);
DECLARE_bool(enable_register_ts_from_raft);
DECLARE_bool(skip_fields_moved_to_xcluster_guarded_info);
DECLARE_bool(skip_tserver_version_checks);
DECLARE_int32(heartbeat_rpc_timeout_ms);

namespace yb::master {

namespace {

using std::string;

using namespace std::literals;
using namespace std::placeholders;

using consensus::ConsensusStatePB;
using consensus::PeerMemberType;
using tablet::RaftGroupStatePB;
using tablet::TABLET_DATA_DELETED;
using tablet::TABLET_DATA_TOMBSTONED;

using google::protobuf::RepeatedPtrField;

class MasterHeartbeatServiceImpl : public MasterServiceBase, public MasterHeartbeatIf {
 public:
  explicit MasterHeartbeatServiceImpl(Master* master)
      : MasterServiceBase(master), MasterHeartbeatIf(master->metric_entity()),
        master_(master), catalog_manager_(master->catalog_manager_impl()) {}

  static Status CheckUniverseUuidMatchFromTserver(
      const UniverseUuid& tserver_universe_uuid,
      const UniverseUuid& master_universe_uuid);

  void TSHeartbeat(const TSHeartbeatRequestPB* req,
                   TSHeartbeatResponsePB* resp,
                   rpc::RpcContext rpc) override;

 private:
  Master* master_;
  CatalogManager* catalog_manager_;

  struct ReportedTablet {
    TabletId tablet_id;
    TabletInfoPtr info;
    const ReportedTabletPB* report;
    std::map<TableId, scoped_refptr<TableInfo>> tables;

    std::string ToString() const {
      return YB_STRUCT_TO_STRING(tablet_id, info, report, tables);
    }
  };

  using ReportedTablets = std::vector<ReportedTablet>;

  Result<bool> ProcessTabletReport(
      const TSDescriptorPtr& ts_desc,
      const NodeInstancePB& ts_instance,
      const TabletReportPB* report_ptr,
      const LeaderEpoch& epoch,
      TabletReportUpdatesPB* report_update,
      rpc::RpcContext* rpc);

  Status ProcessTabletReportBatch(
      const TSDescriptorPtr& ts_desc,
      const NodeInstancePB& ts_instance,
      const TabletReportPB& report,
      ReportedTablets::iterator begin,
      ReportedTablets::iterator end,
      const LeaderEpoch& epoch,
      CoarseTimePoint deadline,
      TabletReportUpdatesPB* full_report_update,
      std::vector<RetryingTSRpcTaskWithTablePtr>* rpcs);

  void DeleteOrphanedTabletReplica(
      const TabletId& tablet_id, const LeaderEpoch& epoch, const TSDescriptorPtr& ts_desc);

  std::pair<MasterHeartbeatServiceImpl::ReportedTablets, std::vector<TabletId>>
      GetReportedAndOrphanedTablets(const RepeatedPtrField<ReportedTabletPB>& updated_tablets);

  bool ProcessCommittedConsensusState(
      const TSDescriptorPtr& ts_desc,
      bool is_incremental,
      const ReportedTabletPB& report,
      const LeaderEpoch& epoch,
      std::map<TableId, TableInfo::ReadLock>* table_read_locks,
      const TabletInfoPtr& tablet,
      const TabletInfo::WriteLock& tablet_lock,
      std::map<TableId, scoped_refptr<TableInfo>>* tables,
      std::vector<RetryingTSRpcTaskWithTablePtr>* rpcs);

  void UpdateTabletReplicasAfterConfigChange(
      const TabletInfoPtr& tablet,
      const std::string& sender_uuid,
      const ConsensusStatePB& consensus_state,
      const ReportedTabletPB& report,
      const LeaderEpoch& epoch);

  void UpdateTabletReplicaInLocalMemory(
      const TSDescriptorPtr& ts_desc,
      const ConsensusStatePB* consensus_state,
      const ReportedTabletPB& report,
      const TabletInfoPtr& tablet);

  TabletReplica CreateNewReplicaForLocalMemory(
      const TSDescriptorPtr& ts_desc,
      const ConsensusStatePB* consensus_state,
      const ReportedTabletPB& report,
      const RaftGroupStatePB& state);

  bool ReplicaMapDiffersFromConsensusState(
      const TabletInfoPtr& tablet, const ConsensusStatePB& cstate);

  void ProcessTabletMetadata(
      const std::string& ts_uuid,
      const TabletDriveStorageMetadataPB& storage_metadata,
      const std::optional<TabletLeaderMetricsPB>& leader_metrics);

  void ProcessTabletReplicaFullCompactionStatus(
      const TabletServerId& ts_uuid, const FullCompactionStatusPB& full_compaction_status);

  // Returns Status::OK if the validation succeeded and processing of the request should continue,
  // otherwise non-OK if processing of the heartbeat should stop. Also sets rpc appropriately.
  Status ValidateTServerUniverseOrRespond(
      const TSHeartbeatRequestPB& req, TSHeartbeatResponsePB* resp, rpc::RpcContext* rpc);

  Result<TSDescriptorPtr> UpdateAndReturnTSDescriptorOrRespond(
      const LeaderEpoch& epoch, const TSHeartbeatRequestPB& req, TSHeartbeatResponsePB* resp,
      rpc::RpcContext* rpc);

  Result<TSDescriptorPtr> RegisterTServerOrRespond(
      const LeaderEpoch& epoch, const TSHeartbeatRequestPB& req, TSHeartbeatResponsePB* resp,
      rpc::RpcContext* rpc);

  Status FillHeartbeatResponseOrRespond(
      const TSHeartbeatRequestPB& req, TSHeartbeatResponsePB* resp, rpc::RpcContext* rpc);

  void PopulatePgCatalogVersionInfo(const TSHeartbeatRequestPB& req,
                                    TSHeartbeatResponsePB& resp);

  // Populate global cluster-wide database pins in the tserver heartbeat response.
  //
  // Since there can be other ongoing sessions that involves DBs on a tserver but the tserver
  // itself is unaware of, every global database pin need to be included in the response.
  void PopulateYsqlDbOldestPinnedReadTimes(TSHeartbeatResponsePB& resp);
};

Status MasterHeartbeatServiceImpl::CheckUniverseUuidMatchFromTserver(
    const UniverseUuid& tserver_universe_uuid,
    const UniverseUuid& master_universe_uuid) {
  if (!FLAGS_master_enable_universe_uuid_heartbeat_check) {
    return Status::OK();
  }

  // If the universe uuid on the master is empty, return an error. Eventually this field will be
  // set via a background thread.
  if (master_universe_uuid.IsNil()) {
    return STATUS(TryAgain, "universe_uuid is not yet set in the cluster config");
  }

  if (tserver_universe_uuid.IsNil()) {
    // The tserver must send universe_uuid with the request, ask it to retry.
    return STATUS(TryAgain, "universe_uuid needs to be set in the request");
  }

  if (tserver_universe_uuid != master_universe_uuid) {
    return STATUS(InvalidArgument,
        Format("Received wrong universe_uuid $0, expected $1",
            tserver_universe_uuid.ToString(), master_universe_uuid.ToString()));
  }
  return Status::OK();
}

void MasterHeartbeatServiceImpl::PopulatePgCatalogVersionInfo(
    const TSHeartbeatRequestPB& req,
    TSHeartbeatResponsePB& resp) {
  // Travels with the catalog version because it is consumed at the same moment: a backend about
  // to bump the version needs to know what its last bump is still costing the leader.
  resp.set_ysql_catalog_prefetch_load(GetYsqlCatalogPrefetchLoad());
  // When YSQL is disabled fall back to a single shared catalog version so that we still send
  // something back to legacy tservers.
  if (!FLAGS_enable_ysql) {
    uint64_t last_breaking_version = 0;
    uint64_t catalog_version = 0;
    auto s = catalog_manager_->GetYsqlCatalogVersion(
        &catalog_version, &last_breaking_version);
    if (s.ok()) {
      resp.set_ysql_catalog_version(catalog_version);
      resp.set_ysql_last_breaking_catalog_version(last_breaking_version);
      VLOG_IF(1, FLAGS_log_ysql_catalog_versions)
        << "responding (to ts " << req.common().ts_instance().permanent_uuid()
        << ") catalog version: " << catalog_version
        << ", breaking version: " << last_breaking_version;
    } else {
      LOG(WARNING) << "Could not get YSQL catalog version for heartbeat response: "
                    << s.ToUserMessage();
    }
    return;
  }

  DbOidToCatalogVersionMap versions;
  uint64_t fingerprint; // can only be used when versions is not empty.
  HybridTime read_ht;
  auto s = catalog_manager_->GetYsqlAllDBCatalogVersions(
      FLAGS_enable_heartbeat_pg_catalog_versions_cache /* use_cache */,
      &versions, &fingerprint, &read_ht);
  if (!s.ok() || versions.empty()) {
    LOG(WARNING) << "Could not get YSQL db catalog versions for heartbeat response: "
                 << s.ToUserMessage();
    return;
  }

  // Return versions back via heartbeat response if the tserver does not provide
  // a fingerprint or the tserver's fingerprint does not match the master's
  // fingerprint. The tserver does not provide a fingerprint when it has
  // not received any catalog versions yet after it starts.
  if (req.has_ysql_db_catalog_versions_fingerprint() &&
      req.ysql_db_catalog_versions_fingerprint() == fingerprint) {
    VLOG_IF(2, FLAGS_log_ysql_catalog_versions)
        << "Responding (to ts "
        << req.common().ts_instance().permanent_uuid()
        << ") without db catalog versions: fingerprints matched";
    return;
  }

  auto* const mutable_version_data = resp.mutable_db_catalog_version_data();
  if (read_ht.is_valid()) {
    mutable_version_data->set_catalog_versions_read_time(read_ht.ToUint64());
  }
  for (const auto& it : versions) {
    auto* const catalog_version = mutable_version_data->add_db_catalog_versions();
    catalog_version->set_db_oid(it.first);
    catalog_version->set_current_version(it.second.current_version);
    catalog_version->set_last_breaking_version(it.second.last_breaking_version);
  }

  if (FLAGS_ysql_yb_enable_invalidation_messages) {
    // We only read pg_yb_invalidation_messages when there is any catalog version
    // change. This reduces the number of reading pg_yb_invalidation_messages which
    // may be bulky if there are many databases and/or large invalidation messages.
    // The risk is that if this read fails, we end up only having catalog version
    // updates but not the associated invalidation messages updates which can cause
    // catalog cache refreshes on those nodes that have got the new catalog
    // version but missed the messages.
    auto messages = catalog_manager_->GetYsqlCatalogInvalationMessages(
        FLAGS_enable_heartbeat_pg_catalog_versions_cache /* use_cache */);
    if (messages.ok()) {
      // The table pg_yb_invalidation_messages is empty.
      VLOG_IF(2, messages->empty()) << "No YSQL invalidation messages for heartbeat response: "
                                    << ResultToStatus(messages);
      auto* const mutable_messages_data = resp.mutable_db_catalog_inval_messages_data();
      for (auto& [db_oid_version, message_list] : *messages) {
        auto* const inval_messages = mutable_messages_data->add_db_catalog_inval_messages();
        inval_messages->set_db_oid(db_oid_version.first);
        inval_messages->set_current_version(db_oid_version.second);
        if (message_list.has_value()) {
          inval_messages->set_message_list(std::move(*message_list));
        }
      }
    } else {
      const auto& s = messages.status();
      auto msg = Format("Could not get YSQL invalidation messages for heartbeat response: $0", s);
      if (s.IsNotFound()) {
        // During upgrade, the table pg_yb_invalidation_messages is created in the finalization
        // phase. We do not want to flood the log before that.
        YB_LOG_EVERY_N_SECS(WARNING, 60) << msg;
      } else {
        LOG(WARNING) << msg;
      }
    }
  }

  VLOG_IF(2, FLAGS_log_ysql_catalog_versions)
    << "responding (to ts "
    << req.common().ts_instance().permanent_uuid()
    << ") db catalog versions: "
    << resp.db_catalog_version_data().ShortDebugString()
    << ") db inval messages: "
    << tserver::CatalogInvalMessagesDataDebugString(resp);
}

void MasterHeartbeatServiceImpl::PopulateYsqlDbOldestPinnedReadTimes(TSHeartbeatResponsePB& resp) {
  if (!FLAGS_enable_db_history_retention_pins) {
    return;
  }
  auto cluster_pins = server_->ts_manager()->GetClusterYsqlDbPinsForPublishing(
      catalog_manager_->TimeSinceElectedLeader());
  resp.set_cluster_ysql_db_pins_ready(cluster_pins.ready);
  for (const auto& [db_oid, pin] : cluster_pins.pins) {
    (*resp.mutable_cluster_ysql_db_oldest_pinned_read_times())[db_oid]
      .set_db_level_oldest_read_time(pin.ToPB());
  }
}

void MasterHeartbeatServiceImpl::TSHeartbeat(
    const TSHeartbeatRequestPB* req,
    TSHeartbeatResponsePB* resp,
    rpc::RpcContext rpc) {
  LongOperationTracker long_operation_tracker(
      "TSHeartbeat", FLAGS_master_ts_heartbeat_long_operation_warning_ms * 1ms);

  consensus::ConsensusStatePB cpb;
  Status s = catalog_manager_->GetCurrentConfig(&cpb);
  if (!s.ok()) {
    // For now, we skip setting the config on errors (hopefully next heartbeat will work).
    // We could enhance to fail rpc, if there are too many error, on a case by case error basis.
    LOG(WARNING) << "Could not set master raft config : " << s.ToString();
  } else if (cpb.has_config()) {
    if (cpb.config().committed_op_index() > req->config_index()) {
      *resp->mutable_master_config() = std::move(cpb.config());
      LOG(INFO) << "Set config at index " << resp->master_config().committed_op_index()
                << " for ts uuid "
                << req->common().ts_instance().permanent_uuid();
    }
  } // Do nothing if config not ready.


  int64_t leader_term;
  {
    // If CatalogManager is not initialized don't even know whether or not we will
    // be a leader (so we can't tell whether or not we can accept tablet reports).
    SCOPED_LEADER_SHARED_LOCK(l, catalog_manager_);

    if (req->common().ts_instance().permanent_uuid().empty()) {
      // In FSManager, we have already added empty UUID protection so that TServer will
      // crash before even sending heartbeat to Master. Here is only for the case that
      // new updated Master might received empty UUID from old version of TServer that
      // doesn't have the crash code in FSManager.
      rpc.RespondFailure(STATUS(InvalidArgument, "Recevied Empty UUID from instance: ",
                                req->common().ts_instance().ShortDebugString()));
      return;
    }

    resp->set_leader_master(false);
    if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
      return;
    }

    leader_term = l.GetLeaderReadyTerm();
    resp->mutable_master_instance()->CopyFrom(server_->instance_pb());
    resp->set_leader_master(true);

    if (!ValidateTServerUniverseOrRespond(*req, resp, &rpc).ok()) {
      return;
    }

    if (!FillHeartbeatResponseOrRespond(*req, resp, &rpc).ok()) {
      return;
    }
    auto desc_result = UpdateAndReturnTSDescriptorOrRespond(l.epoch(), *req, resp, &rpc);
    if (!desc_result.ok()) {
      return;
    }
    TSDescriptorPtr& ts_desc = *desc_result;

    // Correctness requires that this occur after UpdateAndReturnTSDescriptorOrRespond.
    auto fill_status = catalog_manager_->GetXClusterManager()->FillXClusterGuardedInfo(
        leader_term, *resp->mutable_xcluster_guarded_info());
    if (!fill_status.ok()) {
      rpc.RespondFailure(fill_status.CloneAndPrepend("Failed to fill xCluster-guarded info"));
      return;
    }

    resp->set_tablet_report_limit(FLAGS_tablet_report_limit);

    resp->set_xcluster_guarded_lease_duration_ms(FLAGS_xcluster_guarded_lease_duration_ms);

    // Set the TServer metrics in TS Descriptor.
    if (req->has_metrics()) {
      ts_desc->UpdateMetrics(req->metrics());
    }

    auto process_tablet_report_result = ProcessTabletReport(
        ts_desc, req->common().ts_instance(),
        req->has_tablet_report() ? &req->tablet_report() : nullptr,
        l.epoch(),
        resp->mutable_tablet_report(), &rpc);
    if (!process_tablet_report_result.ok()) {
      rpc.RespondFailure(
          process_tablet_report_result.status().CloneAndPrepend("Failed to process tablet report"));
      return;
    }
    if (!*process_tablet_report_result) {
      resp->set_needs_full_tablet_report(true);
    }

    if (!req->has_tablet_report() || req->tablet_report().is_incremental()) {
      // Only process metadata if we have plenty of time to process the work (> 50% of
      // timeout).
      auto safe_time_left = CoarseMonoClock::Now() + (FLAGS_heartbeat_rpc_timeout_ms * 1ms / 2);
      if (rpc.GetClientDeadline() > safe_time_left &&
          PREDICT_TRUE(!ANNOTATE_UNPROTECTED_READ(FLAGS_TEST_skip_processing_tablet_metadata))) {
        std::unordered_map<TabletId, TabletLeaderMetricsPB> id_to_leader_metrics;
        for (auto& info : req->leader_info()) {
          id_to_leader_metrics[info.tablet_id()] = info;
        }
        for (const auto& metadata : req->storage_metadata()) {
          std::optional<TabletLeaderMetricsPB> leader_metrics;
          auto iter = id_to_leader_metrics.find(metadata.tablet_id());
          if (iter != id_to_leader_metrics.end()) {
            leader_metrics = iter->second;
          }
          ProcessTabletMetadata(ts_desc->id(), metadata, leader_metrics);
        }
      }

      for (const auto& consumer_replication_state : req->xcluster_consumer_replication_status()) {
        catalog_manager_->GetXClusterManager()->StoreConsumerReplicationStatus(
            consumer_replication_state);
      }

      // Only process the full compaction statuses if we have plenty of time to process the work (>
      // 50% of timeout).
      safe_time_left = CoarseMonoClock::Now() + (FLAGS_heartbeat_rpc_timeout_ms * 1ms / 2);
      if (rpc.GetClientDeadline() > safe_time_left) {
        for (const auto& full_compaction_status : req->full_compaction_statuses()) {
          ProcessTabletReplicaFullCompactionStatus(
              ts_desc->permanent_uuid(), full_compaction_status);
        }
      }
    }

    // Retrieve all the nodes known by the master.
    std::vector<std::shared_ptr<TSDescriptor>> descs;
    server_->ts_manager()->GetAllLiveDescriptors(&descs);
    for (const auto& desc : descs) {
      *resp->add_tservers() = desc->GetTSInformationPB();
    }

    auto cluster_config = server_->catalog_manager()->GetClusterConfig();
    if (cluster_config) {
      if (!FLAGS_skip_fields_moved_to_xcluster_guarded_info) {
        resp->set_deprecated_oid_cache_invalidations_count(
            cluster_config->oid_cache_invalidations_count());
      }

      uint32_t leader_drain_version = ts_desc->pending_leader_drain_notification();
      if (leader_drain_version && FLAGS_send_leader_blacklisted_tservers_on_heartbeat) {
        auto leader_blacklist = ToBlacklistSet(
            GetBlacklist(*cluster_config, /*blacklist_leader=*/ true));
        auto leaders_drained = !std::any_of(descs.begin(), descs.end(), [&](const auto& desc) {
          return IsBlacklisted(desc->GetRegistration(), leader_blacklist) &&
                 desc->leader_count() != 0;
        });
        if (leaders_drained) {
          for (const auto& desc : descs) {
            if (IsBlacklisted(desc->GetRegistration(), leader_blacklist)) {
              resp->add_leader_blacklisted_tservers_with_no_leaders(desc->permanent_uuid());
            }
          }
          ts_desc->exchg_pending_leader_drain_notification(leader_drain_version, 0);
        }
      }
    } else {
      LOG(WARNING) << "Could not get cluster config for heartbeat response: "
                   << cluster_config.status().ToUserMessage();
    }

    uint64_t transaction_tables_version = catalog_manager_->GetTransactionTablesVersion();
    resp->set_transaction_tables_version(transaction_tables_version);

    if (req->has_auto_flags_config_version() &&
        req->auto_flags_config_version() < server_->GetAutoFlagConfigVersion()) {
      *resp->mutable_auto_flags_config() = server_->GetAutoFlagsConfig();
    }
  }

  PopulateYsqlDbOldestPinnedReadTimes(*resp);

  PopulatePgCatalogVersionInfo(*req, *resp);

  {
    SCOPED_LEADER_SHARED_LOCK(lock, catalog_manager_, leader_term);

    resp->set_leader_master(false);
    if (!lock.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
      return;
    }
    resp->set_leader_master(true);
  }

  rpc.RespondSuccess();
}

std::pair<MasterHeartbeatServiceImpl::ReportedTablets, std::vector<TabletId>>
    MasterHeartbeatServiceImpl::GetReportedAndOrphanedTablets(
    const RepeatedPtrField<ReportedTabletPB>& updated_tablets) {
  MasterHeartbeatServiceImpl::ReportedTablets reported_tablets;
  std::vector<TabletId> orphaned_tablets;

  // Get tablet objects for all updated tablets.
  std::vector<TabletId> updated_tablet_ids;
  std::transform(
      updated_tablets.begin(), updated_tablets.end(),
      std::back_inserter(updated_tablet_ids), [](const auto& pb) { return pb.tablet_id();});
  auto updated_tablet_infos = catalog_manager_->GetTabletInfos(updated_tablet_ids);

  // Get table objects for all updated colocated tables.
  std::vector<TableId> updated_table_ids;
  for (int i = 0; i < updated_tablets.size(); ++i) {
    if (!updated_tablet_infos[i]) {
      continue;
    }
    // For colocated tablets, add ids for all the tables that need processing.
    for (const auto& [table_id, _] : updated_tablets[i].table_to_version()) {
      updated_table_ids.push_back(table_id);
    }
  }
  std::unordered_map<TableId, TableInfoPtr> updated_tables =
      catalog_manager_->GetTableInfos(updated_table_ids);

  for (int i = 0; i < updated_tablets.size(); ++i) {
    const auto& tablet = updated_tablet_infos[i];
    const auto& report = updated_tablets[i];

    if (!tablet || FLAGS_TEST_simulate_sys_catalog_data_loss) {
      orphaned_tablets.push_back(report.tablet_id());
      continue;
    }

    // 1b. Found the tablet, create information for updating local state.
    ReportedTablet reported_tablet {
      .tablet_id = report.tablet_id(),
      .info = tablet,
      .report = &report,
      .tables = {}
    };
    for (const auto& [table_id, version] : report.table_to_version()) {
      auto table = updated_tables.find(table_id);
      if (table->second == nullptr) {
        // TODO(Sanket): Do we need to suitably handle these orphaned tables?
        continue;
      }
      reported_tablet.tables[table_id] = table->second;
    }
    reported_tablets.push_back(std::move(reported_tablet));
  }
  return { reported_tablets, orphaned_tablets };
}

void MasterHeartbeatServiceImpl::DeleteOrphanedTabletReplica(
    const TabletId& tablet_id, const LeaderEpoch& epoch, const TSDescriptorPtr& ts_desc) {
  if (FLAGS_master_enable_deletion_check_for_orphaned_tablets &&
      !catalog_manager_->IsDeletedTabletLoadedFromSysCatalog(tablet_id)) {
    // See the comment in deleted_tablets_loaded_from_sys_catalog_ declaration for an
    // explanation of this logic.
    LOG(WARNING) << Format(
        "Skipping deletion of orphaned tablet $0, since master has never registered this "
        "tablet.", tablet_id);
    return;
  }

  // If a TS reported an unknown tablet, send a delete tablet rpc to the TS.
  LOG(INFO) << "Null tablet reported, possibly the TS was not around when the "
               "table was being deleted. Sending DeleteTablet RPC to this TS.";
  catalog_manager_->SendDeleteTabletRequest(
      tablet_id, tablet::TABLET_DATA_DELETED /* delete_type */,
      std::nullopt /* cas_config_opid_index_less_or_equal */, nullptr /* table */, ts_desc->id(),
      "Report from an orphaned tablet" /* reason */, epoch);
}

// Returns true if report was processed, or false if full report should be requested.
Result<bool> MasterHeartbeatServiceImpl::ProcessTabletReport(
    const TSDescriptorPtr& ts_desc,
    const NodeInstancePB& ts_instance,
    const TabletReportPB* report_ptr,
    const LeaderEpoch& epoch,
    TabletReportUpdatesPB* report_update,
    rpc::RpcContext* rpc) {
  if (!report_ptr) {
    return ts_desc->has_tablet_report();
  }
  const auto& report = *report_ptr;

  if (!ts_desc->PreCheckTabletReport(report)) {
    return false;
  }

  int num_tablets = report.updated_tablets_size();
  TRACE_EVENT2("master", "ProcessTabletReport",
              "requestor", rpc->requestor_string(),
              "num_tablets", num_tablets);

  VLOG(2) << "Received tablet report from " << rpc::RequestorString(rpc) << "("
          << ts_desc->permanent_uuid() << "): " << report.DebugString();

  // TODO: on a full tablet report, we may want to iterate over the tablets we think
  // the server should have, compare vs the ones being reported, and somehow mark
  // any that have been "lost" (eg somehow the tablet metadata got corrupted or something).
  auto [reported_tablets, orphaned_tablets] =
      GetReportedAndOrphanedTablets(report.updated_tablets());

  // Process any delete requests from orphaned tablets, identified above.
  for (const auto& tablet_id : orphaned_tablets) {
    // Every tablet in the report that is processed gets a heartbeat response entry.
    report_update->add_tablets()->set_tablet_id(tablet_id);
    DeleteOrphanedTabletReplica(tablet_id, epoch, ts_desc);
  }

  std::sort(reported_tablets.begin(), reported_tablets.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.tablet_id < rhs.tablet_id;
  });

  // Calculate the deadline for this expensive loop coming up.
  CoarseTimePoint safe_deadline = rpc->GetClientDeadline() -
      static_cast<int64_t>(FLAGS_heartbeat_rpc_timeout_ms * FLAGS_heartbeat_safe_deadline_ratio) *
          1ms;

  // Process tablets by batches.
  for (auto tablet_iter = reported_tablets.begin(); tablet_iter != reported_tablets.end();) {
    auto batch_begin = tablet_iter;
    tablet_iter += std::min<size_t>(
        reported_tablets.end() - tablet_iter, FLAGS_catalog_manager_report_batch_size);

    // Keeps track of all RPCs that should be sent when we're done with a single batch.
    std::vector<RetryingTSRpcTaskWithTablePtr> rpcs;
    auto status = ProcessTabletReportBatch(
        ts_desc, ts_instance, report, batch_begin, tablet_iter, epoch, safe_deadline, report_update,
        &rpcs);
    if (!status.ok()) {
      for (auto& rpc : rpcs) {
        rpc->AbortAndReturnPrevState(status);
      }
      return status;
    }

    // 13. Send all queued RPCs.
    for (auto& rpc : rpcs) {
      DCHECK(rpc->table());
      rpc->table()->AddTask(rpc);
      WARN_NOT_OK(catalog_manager_->ScheduleTask(rpc),
                  Format("Failed to send $0", rpc->description()));
    }
    rpcs.clear();

    // 14. Check deadline. Need to exit before processing all batches if we're close to timing out.
    if (tablet_iter != reported_tablets.end()) {

      // [TESTING] Inject latency before processing a batch to test deadline.
      if (PREDICT_FALSE(FLAGS_TEST_inject_latency_during_tablet_report_ms > 0)) {
        LOG(INFO) << "Sleeping in CatalogManager::ProcessTabletReport for "
                  << FLAGS_TEST_inject_latency_during_tablet_report_ms << " ms";
        SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_inject_latency_during_tablet_report_ms));
      }

      // Return from here at configured safe heartbeat deadline to give the response packet time.
      if (safe_deadline < CoarseMonoClock::Now()) {
        LOG(INFO) << "Reached Heartbeat deadline. Returning early after processing "
                  << report_update->tablets_size() << " tablets";
        report_update->set_processing_truncated(true);
        break;
      }
    }
  } // Loop to process the next batch until fully iterated.

  if (!report.is_incremental()) {
    // A full report may take multiple heartbeats.
    // The TS communicates how much is left to process for the full report beyond this specific HB.
    bool completed_full_report =
        report.remaining_tablet_count() == 0 && !report_update->processing_truncated();
    if (report.updated_tablets_size() == 0) {
      LOG(INFO) << ts_desc->permanent_uuid() << " sent full tablet report with 0 tablets.";
    } else if (!ts_desc->has_tablet_report()) {
      LOG(INFO) << ts_desc->permanent_uuid()
                << (completed_full_report ? " finished" : " receiving") << " first full report: "
                << reported_tablets.size() << " tablets.";
    }
    if (completed_full_report) {
      ts_desc->CompleteFullReport(ts_instance);
    }
  }

  // 14. Queue background processing if we had updates.
  if (report.updated_tablets_size() > 0) {
    catalog_manager_->WakeBgTaskIfPendingUpdates();
  }

  return true;
}

int64_t GetCommittedConsensusStateOpIdIndex(const ReportedTabletPB& report) {
  if (!report.has_committed_consensus_state() ||
      !report.committed_consensus_state().config().has_committed_op_index()) {
    return consensus::kInvalidOpIdIndex;
  }

  return report.committed_consensus_state().config().committed_op_index();
}

Result<std::optional<OpId>> GetPendingConfigOpId(const ReportedTabletPB& report) {
  if (!report.has_pending_config_op_id()) {
    return std::nullopt;
  }
  const auto op_id = OpId::FromPB(report.pending_config_op_id());
  SCHECK_FORMAT(
      op_id.is_valid_not_empty(), InternalError, "Unexpected pending_config_op_id: $0",
      report.pending_config_op_id());
  return op_id;
}

// Returns true if a replica (not already tombstoned or deleted outright) should be deleted based
// on the comparison of its committed/pending Raft config state against the master's last-known
// committed state. Caller must additionally verify that the replica is not a member of the current
// Raft config before acting on a true result.
//
// When FLAGS_use_tablet_report_pending_config_op_id is disabled, or the report's
// pending_config_op_id is malformed, falls back to the legacy strict-less-than comparison for
// backward compatibility.
bool ShouldDeleteNonMember(
    const ReportedTabletPB& report,
    const ConsensusStatePB& prev_cstate,
    int64_t prev_opid_index,
    int64_t report_opid_index,
    const TSDescriptorPtr& ts_desc) {
  if (!FLAGS_master_tombstone_evicted_tablet_replicas ||
    report.tablet_data_state() == TABLET_DATA_TOMBSTONED ||
    report.tablet_data_state() == TABLET_DATA_DELETED) {
    return false;
  }

  if (!FLAGS_use_tablet_report_pending_config_op_id) {
    // For backward compatibility.
    return report_opid_index < prev_opid_index;
  }

  const auto pending_config_op_id_result = GetPendingConfigOpId(report);
  if (!pending_config_op_id_result.ok()) {
    YB_LOG_EVERY_N_SECS(WARNING, 5)
        << "Error getting pending_config_op_id from tablet " << report.tablet_id()
        << " report from " << ts_desc->permanent_uuid() << ": "
        << pending_config_op_id_result.status();
    // Fallback to pre-pending_config_op_id logic.
    return report_opid_index < prev_opid_index;
  }

  const auto pending_config_op_id = pending_config_op_id_result.get();
  const auto reported_config_may_still_be_pending =
      pending_config_op_id.has_value() &&
      pending_config_op_id->term >= prev_cstate.current_term();
  return report_opid_index <= prev_opid_index && !reported_config_may_still_be_pending;
}

Status MasterHeartbeatServiceImpl::ProcessTabletReportBatch(
    const TSDescriptorPtr& ts_desc,
    const NodeInstancePB& ts_instance,
    const TabletReportPB& full_report,
    ReportedTablets::iterator begin,
    ReportedTablets::iterator end,
    const LeaderEpoch& epoch,
    CoarseTimePoint deadline,
    TabletReportUpdatesPB* full_report_update,
    std::vector<RetryingTSRpcTaskWithTablePtr>* rpcs) {
  // First Pass. Iterate in TabletId Order to discover all Table locks we'll need.

  // Maps a table ID to its corresponding TableInfo.
  std::map<TableId, TableInfoPtr> table_info_map;

  std::map<TableId, TableInfo::ReadLock> table_read_locks;
  for (auto reported_tablet = begin; reported_tablet != end; ++reported_tablet) {
    auto table = reported_tablet->info->table();
    table_info_map[table->id()] = table;
    // Acquire locks for all colocated tables reported
    // in sorted order of table ids (We use a map).
    for (const auto& [table_id, table_info] : reported_tablet->tables) {
      table_info_map[table_info->id()] = table_info;
    }
  }

  // Need to acquire locks in Id order to prevent deadlock. Use timed acquisition bounded by the
  // heartbeat's RPC deadline: rather than block (and risk deadlock or long contention) we give up
  // and let the tserver retry the report on a later heartbeat.
  for (auto& [table_id, table] : table_info_map) {
    table_read_locks[table_id] = VERIFY_RESULT(table->TryLockForRead(deadline));
  }

  // Check whether this is the most recent report from this tserver before performing any
  // mutations. If not, we need to stop processing here to avoid overwriting the contents of the
  // more recent report. Concurrent reports for the same tablet are serialized by the per-tablet
  // write locks taken below.
  RETURN_NOT_OK(ts_desc->IsReportCurrent(ts_instance, full_report));

  std::map<TabletId, TabletInfo::WriteLock> tablet_write_locks;
  // Second Pass.
  // Process each tablet. The list is sorted by ID. This may not be in the order that the tablets
  // appear in 'full_report', but that has no bearing on correctness.
  std::vector<TabletInfoPtr> mutated_tablets;
  std::unordered_map<TableId, std::set<TabletId>> new_running_tablets;
  for (auto it = begin; it != end; ++it) {
    const auto& tablet_id = it->tablet_id;
    const TabletInfoPtr& tablet = it->info;
    const ReportedTabletPB& report = *it->report;
    const TableInfoPtr& table = tablet->table();

    // Prepare an heartbeat response entry for this tablet, now that we're going to process it.
    // Every tablet in the report that is processed gets one, even if there are no changes to it.
    ReportedTabletUpdatesPB* update = full_report_update->add_tablets();
    update->set_tablet_id(tablet_id);

    // Get tablet lock on demand.  This works in the batch case because the loop is ordered.
    // Timed acquisition bounded by the RPC deadline: give up (and let the tserver retry) rather
    // than block on a contended/deadlocking tablet write lock.
    tablet_write_locks[tablet_id] = VERIFY_RESULT(tablet->TryLockForWrite(deadline));
    auto& table_lock = table_read_locks[table->id()];
    auto& tablet_lock = tablet_write_locks[tablet_id];

    TRACE_EVENT1("master", "HandleReportedTablet", "tablet_id", report.tablet_id());
    RETURN_NOT_OK_PREPEND(catalog_manager_->CheckIsLeaderAndReady(),
        Format("This master is no longer the leader, unable to handle report for tablet $0",
            tablet_id));

    VLOG(3) << "tablet report: " << report.ShortDebugString()
            << " peer: " << ts_desc->permanent_uuid();

    // Delete the tablet if it (or its table) have been deleted.
    if (tablet_lock->is_deleted() ||
        table_lock->started_deleting()) {
      const string msg = tablet_lock->pb.state_msg();
      update->set_state_msg(msg);
      LOG(INFO) << "Got report from deleted tablet " << tablet->ToString()
                << " (" << msg << "): Sending delete request for this tablet";
      // TODO(unknown): Cancel tablet creation, instead of deleting, in cases
      // where that might be possible (tablet creation timeout & replacement).
      rpcs->push_back(catalog_manager_->MakeDeleteReplicaTask(
          ts_desc->permanent_uuid(), table, tablet_id, TABLET_DATA_DELETED,
          std::nullopt /* cas_config_opid_index_less_or_equal */, epoch, msg));
      continue;
    }

    if (!table_lock->is_running()) {
      const string msg = tablet_lock->pb.state_msg();
      LOG(INFO) << "Got report from tablet " << tablet->tablet_id() << " for non-running table "
                << table->ToString() << ": " << msg;
      update->set_state_msg(msg);
      continue;
    }

    // Tombstone a replica that is no longer part of the Raft config and should be deleted as
    // decided by ShouldDeleteNonMember.
    const ConsensusStatePB& prev_cstate = tablet_lock->pb.committed_consensus_state();
    const int64_t prev_opid_index = prev_cstate.config().committed_op_index();
    const int64_t report_opid_index = GetCommittedConsensusStateOpIdIndex(report);
    if (ShouldDeleteNonMember(report, prev_cstate, prev_opid_index, report_opid_index, ts_desc) &&
        !IsRaftConfigMember(ts_desc->permanent_uuid(), prev_cstate.config())) {
      const string delete_msg = (report_opid_index == consensus::kInvalidOpIdIndex) ?
          "Replica has no consensus available" :
          Format("Replica with old config index $0", report_opid_index);
      rpcs->push_back(catalog_manager_->MakeDeleteReplicaTask(
          ts_desc->permanent_uuid(), table, tablet_id, TABLET_DATA_TOMBSTONED,
          report_opid_index, epoch,
          Format("$0 (current committed config index is $1)", delete_msg, prev_opid_index)));
      continue;
    }

    // Skip a non-deleted tablet which reports an error.
    if (report.has_error()) {
      Status s = StatusFromPB(report.error());
      DCHECK(!s.ok());
      DCHECK_EQ(report.state(), tablet::FAILED);
      LOG(WARNING) << "Tablet " << tablet->ToString() << " has failed on TS "
                  << ts_desc->permanent_uuid() << ": " << s.ToString();
      continue;
    }

    // Hide the tablet if it (or its table) has been hidden and the tablet hasn't.
    if ((tablet_lock->is_hidden() ||
        table_lock->started_hiding()) &&
        report.has_is_hidden() &&
        !report.is_hidden()) {
      const string msg = tablet_lock->pb.state_msg();
      LOG(INFO) << "Got report from hidden tablet " << tablet->ToString()
                << " (" << msg << "): Sending hide request for this tablet";
      auto task = catalog_manager_->MakeDeleteReplicaTask(
          ts_desc->permanent_uuid(), table, tablet_id, TABLET_DATA_DELETED,
          std::nullopt /* cas_config_opid_index_less_or_equal */, epoch, msg);
      task->set_hide_only(true);
      rpcs->push_back(task);
    }

    // Process the report's consensus state.
    // The report will not have a committed_consensus_state if it is in the
    // middle of starting up, such as during tablet bootstrap.
    // If we received an incremental report, and the tablet is starting up, we will update the
    // replica so that the balancer knows how many tablets are in the middle of remote bootstrap.
    if (report.has_committed_consensus_state()) {
      const bool tablet_was_running = tablet_lock->is_running();
      if (ProcessCommittedConsensusState(
              ts_desc, full_report.is_incremental(), report, epoch, &table_read_locks, tablet,
              tablet_lock, &it->tables, rpcs)) {
        // If the tablet was mutated, add it to the tablets to be re-persisted.
        //
        // Done here and not on a per-mutation basis to avoid duplicate entries.
        mutated_tablets.push_back(tablet);
        if (!tablet_was_running && tablet_lock->is_running()) {
          new_running_tablets[table->id()].insert(tablet->id());
        }
      }
    } else if (full_report.is_incremental() &&
        (report.state() == tablet::NOT_STARTED || report.state() == tablet::BOOTSTRAPPING)) {
      // When a tablet server is restarted, it sends a full tablet report with all of its tablets
      // in the NOT_STARTED state, so this would make the cluster balancer think that all the
      // tablets are being remote bootstrapped at once, so only process incremental reports here.
      UpdateTabletReplicaInLocalMemory(ts_desc, nullptr /* consensus */, report, tablet);
    }
  } // Finished one round of batch processing.

  // Write all tablet mutations to the catalog table.
  //
  // SysCatalogTable::Write will short-circuit the case where the data has not
  // in fact changed since the previous version and avoid any unnecessary mutations.
  if (!mutated_tablets.empty()) {
    Status s = catalog_manager_->sys_catalog()->Upsert(epoch, mutated_tablets);
    if (!s.ok()) {
      LOG(WARNING) << "Error updating tablets: " << s;
      return s;
    }
  }

  // Filter the mutated tablets to find which tablets were modified. Need to actually commit the
  // state of the tablets before updating the system.partitions table, so get this first.
  auto yql_partitions_mutated_tablets = YQLPartitionsVTable::FilterRelevantTablets(mutated_tablets);

  // Publish the in-memory tablet mutations and release the locks.
  for (auto& l : tablet_write_locks) {
    l.second.Commit();
  }
  tablet_write_locks.clear();

  // Unlock the tables; we no longer need to access their state.
  table_read_locks.clear();

  // Update the table state if all its tablets are now running.
  for (auto& [table_id, tablets] : new_running_tablets) {
    const auto& table_info = table_info_map[table_id];
    catalog_manager_->SchedulePostTabletCreationTasks(table_info, epoch, tablets);

    // If this is a transaction status tablet, we need to bump the transaction table versions so
    // that tservers update their cache of usable status tablets to include this tablet.
    // We do one incrment per status tablet here for easier testing even though one increment total
    // is sufficient. Transaction status creations are rare, so this should not be an issue.
    if (table_info->GetTableType() == TRANSACTION_STATUS_TABLE_TYPE) {
      WARN_NOT_OK(
          catalog_manager_->IncrementTransactionTablesVersion(),
          "Failed to increment transaction status version, transaction status tablet may not be "
          "usable until next increment");
    }
  }

  // Update the relevant tablet entries in system.partitions.
  if (!yql_partitions_mutated_tablets.empty()) {
    Status s = catalog_manager_->GetYqlPartitionsVtable()
        .ProcessMutatedTablets(yql_partitions_mutated_tablets, tablet_write_locks);
  }

  // Third Pass. Process all tablet schema version changes.
  // (This is separate from tablet state mutations because only table on-disk state is changed.)
  for (auto it = begin; it != end; ++it) {
    const ReportedTabletPB& report = *it->report;
    if (!report.has_schema_version()) {
      continue;
    }
    const TabletInfoPtr& tablet = it->info;
    auto leader = tablet->GetLeader();
    if (leader.ok() && leader.get()->permanent_uuid() == ts_desc->permanent_uuid()) {
      RETURN_NOT_OK(catalog_manager_->HandleTabletSchemaVersionReport(
          tablet.get(), report.schema_version(), epoch));
    }
  }

  return Status::OK();
}

bool MasterHeartbeatServiceImpl::ProcessCommittedConsensusState(
    const TSDescriptorPtr& ts_desc,
    bool is_incremental,
    const ReportedTabletPB& report,
    const LeaderEpoch& epoch,
    std::map<TableId, TableInfo::ReadLock>* table_read_locks,
    const TabletInfoPtr& tablet,
    const TabletInfo::WriteLock& tablet_lock,
    std::map<TableId, scoped_refptr<TableInfo>>* tables,
    std::vector<RetryingTSRpcTaskWithTablePtr>* rpcs) {
  const ConsensusStatePB& prev_cstate = tablet_lock->pb.committed_consensus_state();
  ConsensusStatePB cstate = report.committed_consensus_state();
  bool tablet_was_mutated = false;

  // 6a. The master only processes reports for replicas with committed
  // consensus configurations since it needs the committed index to only
  // cache the most up-to-date config. Since it's possible for TOMBSTONED
  // replicas with no ConsensusMetadata on disk to be reported as having no
  // committed config opid_index, we skip over those replicas.
  if (!cstate.config().has_committed_op_index()) {
    LOG(WARNING) << "Missing opid_index in reported config: " << report.ShortDebugString();
    return false;
  }
  if (PREDICT_TRUE(FLAGS_master_ignore_stale_cstate) &&
        (cstate.current_term() < prev_cstate.current_term() ||
        GetCommittedConsensusStateOpIdIndex(report) < prev_cstate.config().committed_op_index())) {
    LOG(WARNING) << "Stale heartbeat for Tablet " << tablet->ToString()
                << " on TS " << ts_desc->permanent_uuid()
                << " cstate=" << cstate.ShortDebugString()
                << ", prev_cstate=" << prev_cstate.ShortDebugString();
    return false;
  }

  // We do not expect reports from a TS for a config that it is not part of. This can happen if a
  // TS is removed from the config while it is remote bootstrapping. In this case, we must ignore
  // the heartbeats to avoid incorrectly adding this TS to the config in
  // UpdateTabletReplicaInLocalMemory.
  auto found_ts_in_config = std::ranges::any_of(
      cstate.config().peers(),
      [&ts_desc](const auto& peer) { return peer.permanent_uuid() == ts_desc->permanent_uuid(); });
  if (!found_ts_in_config) {
    LOG(WARNING) << Format("Ignoring heartbeat from tablet server that is not part of reported "
        "consensus config. ts_desc: $0, cstate: $1.", *ts_desc, cstate);
    return false;
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
  if (!tablet_lock->is_running() &&
      report.state() == tablet::RUNNING &&
        (cstate.has_leader_uuid() ||
        !FLAGS_catalog_manager_wait_for_new_tablets_to_elect_leader)) {
    DCHECK_EQ(SysTabletsEntryPB::CREATING, tablet_lock->pb.state())
        << "Tablet in unexpected state: " << tablet->ToString()
        << ": " << tablet_lock->pb.ShortDebugString();
    VLOG(1) << "Tablet " << tablet->ToString() << " is now online";
    tablet_lock.mutable_data()->set_state(SysTabletsEntryPB::RUNNING,
        "Tablet reported with an active leader");
    tablet_was_mutated = true;
  }

  // 6d. Update the consensus state if:
  // - A config change operation was committed (reflected by a change to
  //   the committed config's opid_index).
  // - The new cstate has a leader, and either the old cstate didn't, or
  //   there was a term change.
  if (cstate.config().committed_op_index() > prev_cstate.config().committed_op_index() ||
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
        string msg = Format("Previously reported cstate for tablet $0 gave "
                            "a different leader for term $1 than the current cstate. "
                            "Previous cstate: $2. Current cstate: $3.",
                            tablet->ToString(), cstate.current_term(),
                            prev_cstate.ShortDebugString(), cstate.ShortDebugString());
        LOG(DFATAL) << msg;
        return false;
      }
    }

    // 6d(ii). Delete any replicas from the previous config that are not in the new one.
    if (FLAGS_master_tombstone_evicted_tablet_replicas) {
      std::unordered_set<string> current_member_uuids;
      for (const consensus::RaftPeerPB &peer : cstate.config().peers()) {
        InsertOrDie(&current_member_uuids, peer.permanent_uuid());
      }
      for (const consensus::RaftPeerPB &prev_peer : prev_cstate.config().peers()) {
        const string& peer_uuid = prev_peer.permanent_uuid();
        if (!ContainsKey(current_member_uuids, peer_uuid)) {
          // Don't delete a tablet server that hasn't reported in yet (Bootstrapping).
          if (!master_->ts_manager()->LookupTSByUUID(peer_uuid).ok()) {
            continue;
          }
          // Otherwise, the TabletServer needs to remove this peer.
          rpcs->push_back(catalog_manager_->MakeDeleteReplicaTask(
              peer_uuid, tablet->table(), tablet->tablet_id(), TABLET_DATA_TOMBSTONED,
              prev_cstate.config().committed_op_index(), epoch,
              Format("TS $0 not found in new config with opid_index $1",
                    peer_uuid, cstate.config().committed_op_index())));
        }
      }
    }
    // 6d(iii). Update the in-memory ReplicaLocations for this tablet using the new config.
    VLOG(2) << "Updating replicas for tablet " << tablet->tablet_id()
          << " using config reported by " << ts_desc->permanent_uuid()
          << " to that committed in log index " << cstate.config().committed_op_index()
          << " with leader state from term " << cstate.current_term();
    UpdateTabletReplicasAfterConfigChange(tablet, ts_desc->permanent_uuid(), cstate, report, epoch);

    // 6d(iv). Update the consensus state. Don't use 'prev_cstate' after this.
    LOG(INFO) << "Tablet: " << tablet->tablet_id() << " reported consensus state change."
              << " New consensus state: " << cstate.ShortDebugString()
              << " from " << ts_desc->permanent_uuid();
    *tablet_lock.mutable_data()->pb.mutable_committed_consensus_state() = cstate;
    tablet_was_mutated = true;
  } else {
    // Report opid_index is equal to the previous opid_index. If some
    // replica is reporting the same consensus configuration we already know about, but we
    // haven't yet heard from all the tservers in the config, update the in-memory
    // ReplicaLocations.
    LOG(INFO) << "Tablet server " << ts_desc->permanent_uuid() << " sent "
              << (is_incremental ? "incremental" : "full tablet")
              << " report for " << tablet->tablet_id()
              << ", prev state op id: " << prev_cstate.config().committed_op_index()
              << ", prev state term: " << prev_cstate.current_term()
              << ", prev state has_leader_uuid: " << prev_cstate.has_leader_uuid()
              << ". Consensus state: " << cstate.ShortDebugString();
    if (FLAGS_enable_register_ts_from_raft &&
        ReplicaMapDiffersFromConsensusState(tablet, cstate)) {
      LOG(INFO) << Format("Tablet replica map differs from reported consensus state. Replica map: "
          "$0. Reported consensus state: $1.", *tablet->GetReplicaLocations(),
          cstate.ShortDebugString());
      UpdateTabletReplicasAfterConfigChange(
          tablet, ts_desc->permanent_uuid(), cstate, report, epoch);
    } else {
      UpdateTabletReplicaInLocalMemory(ts_desc, &cstate, report, tablet);
    }
  }

  if (FLAGS_use_create_table_leader_hint && !cstate.has_leader_uuid()) {
    catalog_manager_->StartElectionIfReady(cstate, epoch, tablet);
  }

  if (tablet_lock->pb.split_tablet_ids_size() > 0 && tablet_lock->is_hidden()) {
    VLOG(1) << "Skipping AlterTable for hidden already-split tablet " << tablet->ToString();
    return tablet_was_mutated;
  }

  // 7. Send an AlterSchema RPC if the tablet has an old schema version.
  if (table_read_locks->count(tablet->table()->id())) {
    const TableInfo::ReadLock& table_lock = (*table_read_locks)[tablet->table()->id()];
    if (report.has_schema_version() &&
        report.schema_version() != table_lock->pb.version()) {
      if (report.schema_version() > table_lock->pb.version()) {
        LOG(WARNING) << "TS " << ts_desc->permanent_uuid()
                     << " has reported a schema version greater than the current one "
                     << " for tablet " << tablet->ToString()
                     << ". Expected version " << table_lock->pb.version()
                     << " got " << report.schema_version()
                     << " (corruption)";
      } else {
        // TODO: For Alter (rolling apply to tablets), this is an expected transitory state.
        LOG(INFO) << "TS " << ts_desc->permanent_uuid()
                  << " does not have the latest schema for tablet " << tablet->ToString()
                  << ". Expected version " << table_lock->pb.version()
                  << " got " << report.schema_version();
      }
      // All metadata related changes for the tablet is passed as part of RESTORE_ON_TABLET rpcs
      // and we should not trigger anything else during restore so as to not race schema versions.
      // TODO(Sanket): What if restore is stuck then this block is muted forever.
      auto restore_result = catalog_manager_->IsTableUndergoingPitrRestore(*tablet->table());
      LOG_IF(DFATAL, !restore_result.ok())
          << "Failed to determine if table has PITR restore in progress";
      if (!restore_result.ok() || !*restore_result) {
        // It's possible that the tablet being reported is a laggy replica, and in fact
        // the leader has already received an AlterTable RPC. That's OK, though --
        // it'll safely ignore it if we send another.
        TransactionId txn_id = TransactionId::Nil();
        if (table_lock->pb.has_transaction() &&
            table_lock->pb.transaction().has_transaction_id()) {
          LOG(INFO) << "Parsing transaction ID for tablet ID " << tablet->tablet_id();
          auto txn_id_res = FullyDecodeTransactionId(
              table_lock->pb.transaction().transaction_id());
          if (!txn_id_res.ok()) {
            LOG(WARNING) << "Parsing transaction ID failed for tablet ID " << tablet->tablet_id();
            return false;
          }
          txn_id = txn_id_res.get();
        }
        LOG(INFO) << "Triggering AlterTable with transaction ID " << txn_id
                  << " due to heartbeat delay for tablet ID " << tablet->tablet_id();
        rpcs->push_back(std::make_shared<AsyncAlterTable>(
            master_, catalog_manager_->AsyncTaskPool(), tablet, tablet->table(), txn_id, epoch));
      }
    }
  }

  // Send AlterSchema RPC for colocated tables of this tablet if they are outdated.
  for (const auto& id_to_version : report.table_to_version()) {
    // Skip Primary table.
    if (tablet->table()->id() == id_to_version.first) {
      continue;
    }
    if (tables->count(id_to_version.first)) {
      const auto& table_lock = (*table_read_locks)[id_to_version.first];
      // Ignore if same version.
      if (table_lock->pb.version() == id_to_version.second) {
        continue;
      }
      if (id_to_version.second > table_lock->pb.version()) {
        LOG(WARNING) << "TS " << ts_desc->permanent_uuid()
                     << " has reported a schema version greater than the current one "
                     << " for table " << id_to_version.first
                     << ". Expected version " << table_lock->pb.version()
                     << " got " << id_to_version.second
                     << " (corruption)";
      } else {
        LOG(INFO) << "TS " << ts_desc->permanent_uuid()
                  << " does not have the latest schema for table " << id_to_version.first
                  << ". Expected version " << table_lock->pb.version()
                  << " got " << id_to_version.second;
      }
      // All metadata related changes for the tablet is passed as part of RESTORE_ON_TABLET rpcs
      // and we should not trigger anything else during restore so as to not race schema versions.
      // TODO(Sanket): What if restore is stuck then this block is muted forever.
      auto restore_result =
          catalog_manager_->IsTableUndergoingPitrRestore(*(*tables)[id_to_version.first]);
      LOG_IF(DFATAL, !restore_result.ok())
          << "Failed to determine if table has PITR restore in progress";
      if (!restore_result.ok() || !*restore_result) {
        LOG(INFO) << "Triggering AlterTable for table id " << id_to_version.first
                  << " due to heartbeat delay for tablet ID " << tablet->tablet_id();
        rpcs->push_back(std::make_shared<AsyncAlterTable>(
            master_, catalog_manager_->AsyncTaskPool(), tablet, (*tables)[id_to_version.first],
            TransactionId::Nil(), epoch));
      }
    }
  }

  return tablet_was_mutated;
}

void MasterHeartbeatServiceImpl::UpdateTabletReplicasAfterConfigChange(
    const TabletInfoPtr& tablet,
    const std::string& sender_uuid,
    const ConsensusStatePB& consensus_state,
    const ReportedTabletPB& report,
    const LeaderEpoch& epoch) {
  auto replica_locations = std::make_shared<TabletReplicaMap>();
  auto prev_rl = tablet->GetReplicaLocations();

  for (const consensus::RaftPeerPB& peer : consensus_state.config().peers()) {
    if (!peer.has_permanent_uuid()) {
      LOG(WARNING) << "Missing UUID for peer" << peer.ShortDebugString();
      continue;
    }
    auto ts_desc_result = master_->ts_manager()->LookupTSByUUID(peer.permanent_uuid());
    if (!ts_desc_result.ok()) {
      if (!FLAGS_enable_register_ts_from_raft) {
        LOG(WARNING) << "Tablet server has never reported in. "
                    << "Not including in replica locations map yet. Peer: "
                    << peer.ShortDebugString()
                    << "; Tablet: " << tablet->ToString();
        continue;
      }

      LOG(INFO) << "Tablet server has never reported in. Registering the ts using "
                << "the raft config. Peer: " << peer.ShortDebugString()
                << "; Tablet: " << tablet->ToString();
      Status s = catalog_manager_->RegisterTsFromRaftConfig(peer, epoch);
      if (!s.ok()) {
        LOG(WARNING) << "Could not register ts from raft config: " << s
                    << " Skip updating the replica map.";
        continue;
      }

      // Guaranteed to find the ts since we just registered.
      ts_desc_result = master_->ts_manager()->LookupTSByUUID(peer.permanent_uuid());
      if (!ts_desc_result.ok()) {
        LOG(WARNING) << "Could not find ts with uuid " << peer.permanent_uuid()
                    << " after registering from raft config. Skip updating the replica"
                    << " map.";
        continue;
      }
    }
    const auto& ts_desc = *ts_desc_result;

    const TabletReplica* existing_replica = FindOrNull(*prev_rl, ts_desc->permanent_uuid());
    // The RaftGroupStatePB in the report is only applicable to the replica that is owned by the
    // sender. Initialize the other replicas with an unknown state.
    const RaftGroupStatePB replica_state =
        (sender_uuid == ts_desc->permanent_uuid()) ? report.state() : RaftGroupStatePB::UNKNOWN;

    auto replica = CreateNewReplicaForLocalMemory(ts_desc, &consensus_state, report, replica_state);
    if (existing_replica) {
      replica.UpdateDriveInfo(existing_replica->drive_info);
      replica.UpdateLeaderLeaseInfo(existing_replica->leader_lease_info);
    }
    auto result = replica_locations.get()->insert({ts_desc->id(), replica});
    LOG_IF(FATAL, !result.second) << "duplicate uuid: " << ts_desc->id();
  }

  // Update the local tablet replica set.
  catalog_manager_->SetTabletReplicaLocations(tablet, replica_locations);
}

void MasterHeartbeatServiceImpl::UpdateTabletReplicaInLocalMemory(
    const TSDescriptorPtr& ts_desc,
    const ConsensusStatePB* consensus_state,
    const ReportedTabletPB& report,
    const TabletInfoPtr& tablet) {
  auto replica = CreateNewReplicaForLocalMemory(ts_desc, consensus_state, report, report.state());
  catalog_manager_->UpdateTabletReplicaLocations(tablet, ts_desc->id(), replica);
}

TabletReplica MasterHeartbeatServiceImpl::CreateNewReplicaForLocalMemory(
    const TSDescriptorPtr& ts_desc,
    const ConsensusStatePB* consensus_state,
    const ReportedTabletPB& report,
    const RaftGroupStatePB& state) {
  TabletReplica replica;
  // Tablets in state NOT_STARTED or BOOTSTRAPPING don't have a consensus.
  if (consensus_state == nullptr) {
    replica.role = PeerRole::NON_PARTICIPANT;
    replica.member_type = PeerMemberType::UNKNOWN_MEMBER_TYPE;
  } else {
    CHECK(consensus_state != nullptr) << "No cstate: " << ts_desc->permanent_uuid()
                                      << " - " << state;
    replica.role = GetConsensusRole(ts_desc->permanent_uuid(), *consensus_state);
    replica.member_type = GetConsensusMemberType(ts_desc->permanent_uuid(), *consensus_state);
  }
  if (report.has_should_disable_lb_move()) {
    replica.should_disable_lb_move = report.should_disable_lb_move();
  }
  if (report.has_fs_data_dir()) {
    replica.fs_data_dir = report.fs_data_dir();
  }
  replica.state = state;
  replica.ts_desc = ts_desc;
  if (!ts_desc->registered_through_heartbeat()) {
    auto last_heartbeat = ts_desc->LastHeartbeatTime();
    replica.time_updated = last_heartbeat ? last_heartbeat : MonoTime::kMin;
  }
  return replica;
}

bool MasterHeartbeatServiceImpl::ReplicaMapDiffersFromConsensusState(
    const TabletInfoPtr& tablet, const ConsensusStatePB& cstate) {
  auto locs = tablet->GetReplicaLocations();
  if (locs->size() != implicit_cast<size_t>(cstate.config().peers_size())) {
    return true;
  }
  for (auto iter = cstate.config().peers().begin(); iter != cstate.config().peers().end(); iter++) {
    if (locs->find(iter->permanent_uuid()) == locs->end()) {
      return true;
    }
  }
  return false;
}

// Return true if the received ht_lease_exp from the leader peer has been expired for too
// long time when the ts metrics heartbeat reaches the master.
bool IsHtLeaseExpiredForTooLong(MicrosTime now, MicrosTime ht_lease_exp) {
  const auto now_usec = boost::posix_time::microseconds(now);
  const auto ht_lease_exp_usec = boost::posix_time::microseconds(ht_lease_exp);
  return (now_usec - ht_lease_exp_usec).total_seconds() >
      FLAGS_maximum_tablet_leader_lease_expired_secs;
}

void MasterHeartbeatServiceImpl::ProcessTabletMetadata(
    const std::string& ts_uuid,
    const TabletDriveStorageMetadataPB& storage_metadata,
    const std::optional<TabletLeaderMetricsPB>& leader_metrics) {
  const string& tablet_id = storage_metadata.tablet_id();
  auto tablet_result = catalog_manager_->GetTabletInfo(tablet_id);
  if (!tablet_result) {
    VLOG(1) << Format("Tablet $0 not found", tablet_id);
    return;
  }
  auto& tablet = *tablet_result;
  MicrosTime new_ht_lease_exp = 0;
  uint64 new_heartbeats_without_leader_lease = 0;
  consensus::LeaderLeaseStatus leader_lease_status =
      consensus::LeaderLeaseStatus::NO_MAJORITY_REPLICATED_LEASE;
  bool leader_lease_info_initialized = false;
  if (leader_metrics.has_value()) {
    auto existing_leader_lease_info = tablet->GetLeaderLeaseInfoIfLeader(ts_uuid);
    // If the peer is the current leader, update the counter to track heartbeats that
    // the tablet doesn't have a valid lease.
    if (existing_leader_lease_info) {
      const auto& leader_info = *leader_metrics;
      leader_lease_status = leader_info.leader_lease_status();
      leader_lease_info_initialized = true;
      if (leader_info.leader_lease_status() == consensus::LeaderLeaseStatus::HAS_LEASE) {
        auto ht_lease_exp = leader_info.ht_lease_expiration();
        auto current_ht_lease_exp = existing_leader_lease_info->ht_lease_expiration;
        // If the reported ht lease from the leader is expired for more than
        // FLAGS_maximum_tablet_leader_lease_expired_secs, the leader shouldn't be treated
        // as a valid leader.
        // If the tablet has been changed from RF1 to any RF>1, the newly reported ht_lease_exp
        // is always less than that reported when it was RF1 (kInfiniteHybridTimeLeaseExpiration).
        // We should also treat such ht_lease_exp as valid.
        // See https://github.com/yugabyte/yugabyte-db/issues/24575.
        if ((ht_lease_exp >= current_ht_lease_exp ||
                 current_ht_lease_exp == consensus::kInfiniteHybridTimeLeaseExpiration) &&
            !IsHtLeaseExpiredForTooLong(
                master_->clock()->Now().GetPhysicalValueMicros(), ht_lease_exp)) {
          new_ht_lease_exp = ht_lease_exp;
          tablet->UpdateLastTimeWithValidLeader();
        }
      } else {
        new_heartbeats_without_leader_lease =
            existing_leader_lease_info->heartbeats_without_leader_lease + 1;
      }
    }
  }
  TabletLeaderLeaseInfo leader_lease_info{
        leader_lease_info_initialized,
        leader_lease_status,
        new_ht_lease_exp,
        new_heartbeats_without_leader_lease};
  auto drive_info = TabletReplicaDriveInfo {
    .sst_files_size = storage_metadata.sst_file_size(),
    .wal_files_size = storage_metadata.wal_file_size(),
    .uncompressed_sst_file_size = storage_metadata.uncompressed_sst_file_size(),
    .may_have_orphaned_post_split_data = storage_metadata.may_have_orphaned_post_split_data(),
    .total_size = storage_metadata.total_size(),
    .vector_index_size = storage_metadata.vector_index_size(),
    .has_active_vector_index_backfill = storage_metadata.has_active_vector_index_backfill(),
  };
  tablet->UpdateReplicaInfo(ts_uuid, drive_info, leader_lease_info);
}

void MasterHeartbeatServiceImpl::ProcessTabletReplicaFullCompactionStatus(
    const TabletServerId& ts_uuid, const FullCompactionStatusPB& full_compaction_status) {
  if (!full_compaction_status.has_tablet_id()) {
    VLOG(1) << "Tablet id not found";
    return;
  }

  const TabletId& tablet_id = full_compaction_status.tablet_id();

  if (!full_compaction_status.has_full_compaction_state()) {
    LOG(WARNING) << Format(
        "Full compaction status not reported for tablet $0 on tserver $1", tablet_id, ts_uuid);
    return;
  }

  if (!full_compaction_status.has_last_full_compaction_time()) {
    LOG(WARNING) << Format(
        "Last full compaction time not reported for tablet $0 on tserver $1", tablet_id, ts_uuid);
    return;
  }

  const auto result = catalog_manager_->GetTabletInfo(tablet_id);
  if (!result.ok()) {
    LOG_WITH_FUNC(WARNING) << result;
    return;
  }

  (*result)->UpdateReplicaFullCompactionStatus(
      ts_uuid, FullCompactionStatus{
                   full_compaction_status.full_compaction_state(),
                   HybridTime(full_compaction_status.last_full_compaction_time())});
}

Status MasterHeartbeatServiceImpl::FillHeartbeatResponseOrRespond(
    const TSHeartbeatRequestPB& req, TSHeartbeatResponsePB* resp, rpc::RpcContext* rpc) {
  auto s = catalog_manager_->FillHeartbeatResponse(req, resp);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to fill heartbeat response: " << s.ToString();
    rpc->RespondFailure(s);
    return s;
  }
  return Status::OK();
}

Status MasterHeartbeatServiceImpl::ValidateTServerUniverseOrRespond(
    const TSHeartbeatRequestPB& req, TSHeartbeatResponsePB* resp, rpc::RpcContext* rpc) {
  // At the time of this check, we need to know that we're the master leader to access the
  // cluster config.
  auto cluster_config_result = catalog_manager_->GetClusterConfig();
  if (!cluster_config_result.ok()) {
    auto& s = cluster_config_result.status();
    LOG(WARNING) << "Unable to get cluster configuration: " << s.ToString();
    rpc->RespondFailure(s);
    return s;
  }
  const auto& cluster_config = *cluster_config_result;

  auto tserver_universe_uuid_res = UniverseUuid::FromString(req.universe_uuid());
  if (!tserver_universe_uuid_res) {
    LOG(WARNING) << "Could not decode request universe_uuid: "
                 << tserver_universe_uuid_res.status().ToString();
    rpc->RespondFailure(tserver_universe_uuid_res.status());
    return tserver_universe_uuid_res.status();
  }
  auto tserver_universe_uuid = *tserver_universe_uuid_res;

  // Read a locked copy: tests may change this string flag concurrently.
  std::string test_master_universe_uuid;
  CHECK(google::GetCommandLineOption("TEST_master_universe_uuid", &test_master_universe_uuid));
  auto master_universe_uuid_res = UniverseUuid::FromString(
      test_master_universe_uuid.empty() ? cluster_config.universe_uuid()
                                        : test_master_universe_uuid);
  if (!master_universe_uuid_res) {
    LOG(WARNING) << "Could not decode cluster config universe_uuid: "
                 << master_universe_uuid_res.status().ToString();
    rpc->RespondFailure(master_universe_uuid_res.status());
    return master_universe_uuid_res.status();
  }
  auto master_universe_uuid = *master_universe_uuid_res;

  auto s = CheckUniverseUuidMatchFromTserver(tserver_universe_uuid, master_universe_uuid);

  if (!s.ok()) {
    LOG(WARNING) << "Failed CheckUniverseUuidMatchFromTserver check: " << s.ToString();
    if (master_universe_uuid.IsNil()) {
      auto* error = resp->mutable_error();
      error->set_code(MasterErrorPB::INVALID_CLUSTER_CONFIG);
      StatusToPB(s, error->mutable_status());
      rpc->RespondSuccess();
      return s;
    }

    if (tserver_universe_uuid.IsNil()) {
      resp->set_universe_uuid((*master_universe_uuid_res).ToString());
      auto* error = resp->mutable_error();
      error->set_code(MasterErrorPB::INVALID_REQUEST);
      StatusToPB(s, error->mutable_status());
      rpc->RespondSuccess();
      return s;
    }

    rpc->RespondFailure(s);
    return s;
  }
  if (req.has_registration()) {
    resp->set_cluster_uuid(cluster_config.cluster_uuid());
  }

  return Status::OK();
}

Result<TSDescriptorPtr>
MasterHeartbeatServiceImpl::RegisterTServerOrRespond(
    const LeaderEpoch& epoch, const TSHeartbeatRequestPB& req, TSHeartbeatResponsePB* resp,
    rpc::RpcContext* rpc) {
  if (!FLAGS_skip_tserver_version_checks && req.registration().has_version_info()) {
    const auto& registration = req.registration();
    auto status = server_->ysql_manager().ValidateTServerVersion(registration.version_info());
    if (!status.ok()) {
      LOG(WARNING) << "yb-tserver " << registration.common().ShortDebugString()
                   << " running invalid version: "
                   << registration.version_info().ShortDebugString();
      resp->set_is_fatal_error(true);
      auto* error = resp->mutable_error();
      error->set_code(MasterErrorPB::INTERNAL_ERROR);
      StatusToPB(status, error->mutable_status());
      rpc->RespondSuccess();
      return status;
    }
  }

  const auto& ts_uuid = req.common().ts_instance().permanent_uuid();
  auto old_desc = server_->ts_manager()->LookupTSByUUID(ts_uuid);
  int64_t old_seqno = old_desc.ok() ? (*old_desc)->latest_seqno() : -1;

  auto desc_result = server_->ts_manager()->RegisterFromHeartbeat(
      req, epoch, server_->MakeCloudInfoPB(), &server_->proxy_cache());
  if (desc_result.ok()) {
    LOG(INFO) << "Registering " << req.common().ts_instance().ShortDebugString();

    if (FLAGS_ysql_yb_enable_listen_notify &&
        old_seqno >= 0 &&
        req.common().ts_instance().instance_seqno() > old_seqno) {
      auto s = catalog_manager_->DeleteNotificationsReplicationSlot(
          ts_uuid, req.common().ts_instance().start_time_us());
      WARN_NOT_OK(s, Format(
          "Failed to delete notifications replication slot for restarted tserver $0", ts_uuid));
    }

    return desc_result;
  }
  auto& status = desc_result.status();
  if (status.IsAlreadyPresent()) {
    // AlreadyPresent indicates a host port collision. This tserver shouldn't be heartbeating
    // anymore, but in any case ask it to re-register to preserve existing semantics.
    LOG(INFO) << Format(
        "Got heartbeat from unknown tablet server { $0 } as $1; Asking this server to "
        "re-register. Status from ts lookup: $2",
        req.common().ts_instance().ShortDebugString(), rpc->requestor_string(), status);
    resp->set_needs_reregister(true);
    resp->set_needs_full_tablet_report(true);
    rpc->RespondSuccess();
    return desc_result;
  }
  LOG(WARNING) << Format(
      "Failed to register tablet server { $0 } as $1; Status was: $2",
      req.common().ts_instance().ShortDebugString(), rpc->requestor_string(), status);
  rpc->RespondFailure(status);
  return desc_result;
}

Result<TSDescriptorPtr> MasterHeartbeatServiceImpl::UpdateAndReturnTSDescriptorOrRespond(
    const LeaderEpoch& epoch, const TSHeartbeatRequestPB& req, TSHeartbeatResponsePB* resp,
    rpc::RpcContext* rpc) {
  if (req.has_registration()) {
    return RegisterTServerOrRespond(epoch, req, resp, rpc);
  }
  auto desc_result = server_->ts_manager()->LookupAndUpdateTSFromHeartbeat(req, epoch);
  if (desc_result.ok()) {
    return desc_result;
  }
  // todo(zdrudi): be more precise about the error code here.
  if (desc_result.status().IsNotFound()) {
    LOG(INFO) << Format(
        "Failed to lookup tablet server { $0 } as $1; Asking this server to re-register. Status "
        "from ts lookup: $2",
        req.common().ts_instance().ShortDebugString(), rpc->requestor_string(),
        desc_result.status());
    resp->set_needs_reregister(true);
    resp->set_needs_full_tablet_report(true);
    rpc->RespondSuccess();
    return desc_result;
  }
  // Under some circumstances TS lookups from a heartbeat may trigger sys catalog writes which can
  // fail. Distinguish these failures from lookup failures using a different log message and failing
  // the rpc.
  LOG(WARNING) << Format(
      "Failed to lookup tablet server { $0 } as $1; Status was: $2",
      req.common().ts_instance().ShortDebugString(), rpc->requestor_string(), desc_result.status());
  rpc->RespondFailure(desc_result.status());
  return desc_result;
}

} // namespace

std::unique_ptr<rpc::ServiceIf> MakeMasterHeartbeatService(Master* master) {
  return std::make_unique<MasterHeartbeatServiceImpl>(master);
}

} // namespace yb::master
