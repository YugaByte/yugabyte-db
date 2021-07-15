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

#include "yb/master/master_service.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/preprocessor/cat.hpp>

#include <gflags/gflags.h>

#include "yb/common/wire_protocol.h"

#include "yb/master/catalog_manager-internal.h"
#include "yb/master/encryption_manager.h"
#include "yb/master/flush_manager.h"
#include "yb/master/master.h"
#include "yb/master/master_service_base-internal.h"
#include "yb/master/master_service_base.h"
#include "yb/master/permissions_manager.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"

#include "yb/server/webserver.h"

#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/flag_tags.h"
#include "yb/util/random_util.h"
#include "yb/util/shared_lock.h"

DEFINE_int32(master_inject_latency_on_tablet_lookups_ms, 0,
             "Number of milliseconds that the master will sleep before responding to "
             "requests for tablet locations.");
TAG_FLAG(master_inject_latency_on_tablet_lookups_ms, unsafe);
TAG_FLAG(master_inject_latency_on_tablet_lookups_ms, hidden);

DEFINE_test_flag(bool, master_fail_transactional_tablet_lookups, false,
                 "Whether to fail all lookup requests to transactional table.");

DEFINE_double(master_slow_get_registration_probability, 0,
              "Probability of injecting delay in GetMasterRegistration.");

DEFINE_int32(tablet_report_limit, 1000,
             "Max Number of tablets to report during a single heartbeat. "
             "If this is set to INT32_MAX, then heartbeat will report all dirty tablets.");
TAG_FLAG(tablet_report_limit, advanced);

DECLARE_CAPABILITY(TabletReportLimit);
DECLARE_int32(heartbeat_rpc_timeout_ms);

using namespace std::literals;

namespace yb {
namespace master {

using std::string;
using std::vector;
using std::shared_ptr;
using std::ostringstream;
using consensus::RaftPeerPB;
using rpc::RpcContext;

static void SetupErrorAndRespond(MasterErrorPB* error,
                                 const Status& s,
                                 MasterErrorPB::Code code,
                                 RpcContext* rpc) {
  StatusToPB(s, error->mutable_status());
  error->set_code(code);
  rpc->RespondSuccess();
}

MasterServiceImpl::MasterServiceImpl(Master* server)
    : MasterServiceIf(server->metric_entity()),
      MasterServiceBase(server) {
}

void MasterServiceImpl::TSHeartbeat(const TSHeartbeatRequestPB* req,
                                    TSHeartbeatResponsePB* resp,
                                    RpcContext rpc) {
  LongOperationTracker long_operation_tracker("TSHeartbeat", 1s);

  // If CatalogManager is not initialized don't even know whether or not we will
  // be a leader (so we can't tell whether or not we can accept tablet reports).
  SCOPED_LEADER_SHARED_LOCK(l, server_->catalog_manager());

  consensus::ConsensusStatePB cpb;
  Status s = server_->catalog_manager()->GetCurrentConfig(&cpb);
  if (!s.ok()) {
    // For now, we skip setting the config on errors (hopefully next heartbeat will work).
    // We could enhance to fail rpc, if there are too many error, on a case by case error basis.
    LOG(WARNING) << "Could not set master raft config : " << s.ToString();
  } else if (cpb.has_config()) {
    if (cpb.config().opid_index() > req->config_index()) {
      *resp->mutable_master_config() = std::move(cpb.config());
      LOG(INFO) << "Set config at index " << resp->master_config().opid_index() << " for ts uuid "
                << req->common().ts_instance().permanent_uuid();
    }
  } // Do nothing if config not ready.

  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
    resp->set_leader_master(false);
    return;
  }

  resp->mutable_master_instance()->CopyFrom(server_->instance_pb());
  resp->set_leader_master(true);

  // If the TS is registering, register in the TS manager.
  if (req->has_registration()) {
    Status s = server_->ts_manager()->RegisterTS(req->common().ts_instance(),
                                                 req->registration(),
                                                 server_->MakeCloudInfoPB(),
                                                 &server_->proxy_cache());
    if (!s.ok()) {
      LOG(WARNING) << "Unable to register tablet server (" << rpc.requestor_string() << "): "
                   << s.ToString();
      // TODO: add service-specific errors.
      rpc.RespondFailure(s);
      return;
    }
    SysClusterConfigEntryPB cluster_config;
    s = server_->catalog_manager()->GetClusterConfig(&cluster_config);
    if (!s.ok()) {
      LOG(WARNING) << "Unable to get cluster configuration: " << s.ToString();
      rpc.RespondFailure(s);
    }
    resp->set_cluster_uuid(cluster_config.cluster_uuid());
  }

  s = server_->catalog_manager()->FillHeartbeatResponse(req, resp);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to fill heartbeat response: " << s.ToString();
    rpc.RespondFailure(s);
  }

  // Look up the TS -- if it just registered above, it will be found here.
  // This allows the TS to register and tablet-report in the same RPC.
  TSDescriptorPtr ts_desc;
  s = server_->ts_manager()->LookupTS(req->common().ts_instance(), &ts_desc);
  if (s.IsNotFound()) {
    LOG(INFO) << "Got heartbeat from unknown tablet server { "
              << req->common().ts_instance().ShortDebugString()
              << " } as " << rpc.requestor_string()
              << "; Asking this server to re-register.";
    resp->set_needs_reregister(true);
    resp->set_needs_full_tablet_report(true);
    rpc.RespondSuccess();
    return;
  } else if (!s.ok()) {
    LOG(WARNING) << "Unable to look up tablet server for heartbeat request "
                 << req->DebugString() << " from " << rpc.requestor_string()
                 << "\nStatus: " << s.ToString();
    rpc.RespondFailure(s.CloneAndPrepend("Unable to lookup TS"));
    return;
  }

  ts_desc->UpdateHeartbeat(req);

  // Adjust the table report limit per heartbeat so this can be dynamically changed.
  if (ts_desc->HasCapability(CAPABILITY_TabletReportLimit)) {
    resp->set_tablet_report_limit(FLAGS_tablet_report_limit);
  }

  // Set the TServer metrics in TS Descriptor.
  if (req->has_metrics()) {
    ts_desc->UpdateMetrics(req->metrics());
  }

  if (req->has_tablet_report()) {
    s = server_->catalog_manager()->ProcessTabletReport(
      ts_desc.get(), req->tablet_report(), resp->mutable_tablet_report(), &rpc);
    if (!s.ok()) {
      rpc.RespondFailure(s.CloneAndPrepend("Failed to process tablet report"));
      return;
    }
  }

  if (!req->has_tablet_report() || req->tablet_report().is_incremental()) {
    // Only process split tablets if we have plenty of time to process the work (> 50% of timeout).
    auto safe_time_left = CoarseMonoClock::Now() + (FLAGS_heartbeat_rpc_timeout_ms * 1ms / 2);
    if (rpc.GetClientDeadline() > safe_time_left) {
      for (const auto& tablet : req->tablets_for_split()) {
        VLOG(1) << "Got tablet to split: " << AsString(tablet);
        const auto split_status = server_->catalog_manager()->SplitTablet(tablet.tablet_id());
        if (!split_status.ok()) {
          if (MasterError(split_status) == MasterErrorPB::REACHED_SPLIT_LIMIT) {
            YB_LOG_EVERY_N_SECS(WARNING, 60 * 60) << split_status;
          } else {
            LOG(WARNING) << split_status;
          }
        }
      }
    }

    safe_time_left = CoarseMonoClock::Now() + (FLAGS_heartbeat_rpc_timeout_ms * 1ms / 2);
    if (rpc.GetClientDeadline() > safe_time_left && req->has_tablet_path_info()) {
      server_->catalog_manager()->ProcessTabletPathInfo(
            ts_desc.get()->permanent_uuid(), req->tablet_path_info());
    }

    // Only set once. It may take multiple heartbeats to receive a full tablet report.
    if (!ts_desc->has_tablet_report()) {
      resp->set_needs_full_tablet_report(true);
    }
  }

  // Retrieve all the nodes known by the master.
  std::vector<std::shared_ptr<TSDescriptor>> descs;
  server_->ts_manager()->GetAllLiveDescriptors(&descs);
  for (const auto& desc : descs) {
    *resp->add_tservers() = *desc->GetTSInformationPB();
  }

  // Retrieve the ysql catalog schema version.
  uint64_t last_breaking_version = 0;
  uint64_t catalog_version = 0;
  s = server_->catalog_manager()->GetYsqlCatalogVersion(&catalog_version,
                                                        &last_breaking_version);
  if (s.ok()) {
    resp->set_ysql_catalog_version(catalog_version);
    resp->set_ysql_last_breaking_catalog_version(last_breaking_version);
  } else {
    LOG(WARNING) << "Could not get YSQL catalog version for heartbeat response: "
                 << s.ToUserMessage();
  }

  rpc.RespondSuccess();
}

void MasterServiceImpl::GetTabletLocations(const GetTabletLocationsRequestPB* req,
                                           GetTabletLocationsResponsePB* resp,
                                           RpcContext rpc) {
  SCOPED_LEADER_SHARED_LOCK(l, server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
    return;
  }

  if (PREDICT_FALSE(FLAGS_master_inject_latency_on_tablet_lookups_ms > 0)) {
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_master_inject_latency_on_tablet_lookups_ms));
  }
  if (PREDICT_FALSE(FLAGS_TEST_master_fail_transactional_tablet_lookups)) {
    auto tables = server_->catalog_manager()->GetTables(GetTablesMode::kAll);
    const auto& tablet_id = req->tablet_ids(0);
    for (const auto& table : tables) {
      TabletInfos tablets;
      table->GetAllTablets(&tablets);
      for (const auto& tablet : tablets) {
        if (tablet->tablet_id() == tablet_id) {
          TableType table_type;
          {
            auto lock = table->LockForRead();
            table_type = table->metadata().state().table_type();
          }
          if (table_type == TableType::TRANSACTION_STATUS_TABLE_TYPE) {
            rpc.RespondFailure(STATUS(InvalidCommand, "TEST: Artificial failure"));
            return;
          }
          break;
        }
      }
    }
  }

  // For now all the tables in the cluster share the same replication information.
  int expected_live_replicas = 0;
  int expected_read_replicas = 0;
  server_->catalog_manager()->GetExpectedNumberOfReplicas(
      &expected_live_replicas, &expected_read_replicas);

  if (req->has_table_id()) {
    const auto table_info = server_->catalog_manager()->GetTableInfo(req->table_id());
    if (table_info) {
      const auto table_lock = table_info->LockForRead();
      resp->set_partition_list_version(table_lock->pb.partition_list_version());
    }
  }

  for (const TabletId& tablet_id : req->tablet_ids()) {
    // TODO: once we have catalog data. ACL checks would also go here, probably.
    TabletLocationsPB* locs_pb = resp->add_tablet_locations();
    locs_pb->set_expected_live_replicas(expected_live_replicas);
    locs_pb->set_expected_read_replicas(expected_read_replicas);
    Status s = server_->catalog_manager()->GetTabletLocations(tablet_id, locs_pb);
    if (!s.ok()) {
      resp->mutable_tablet_locations()->RemoveLast();

      GetTabletLocationsResponsePB::Error* err = resp->add_errors();
      err->set_tablet_id(tablet_id);
      StatusToPB(s, err->mutable_status());
    }
  }

  rpc.RespondSuccess();
}

#define COMMON_HANDLER_ARGS(class_name, method_name) \
    req, resp, &rpc, &class_name::method_name, __FILE__, __LINE__, __func__

#define HANDLE_ON_LEADER_IMPL(class_name, method_name, hold_leader_lock) \
    HandleIn<class_name>(COMMON_HANDLER_ARGS(class_name, method_name), (hold_leader_lock))

#define HANDLE_ON_LEADER_WITH_LOCK(class_name, method_name) \
    HANDLE_ON_LEADER_IMPL(class_name, method_name, HoldCatalogLock::kTrue)

#define HANDLE_ON_LEADER_WITHOUT_LOCK(class_name, method_name) \
    HANDLE_ON_LEADER_IMPL(class_name, method_name, HoldCatalogLock::kFalse)

#define HANDLE_ON_ALL_MASTERS(class_name, method_name) \
    HandleOnAllMasters<class_name>(COMMON_HANDLER_ARGS(class_name, method_name))

#define MASTER_SERVICE_IMPL_ON_LEADER_WITH_LOCK(r, class_name, method_name) \
  void MasterServiceImpl::method_name( \
      const BOOST_PP_CAT(method_name, RequestPB)* req, \
      BOOST_PP_CAT(method_name, ResponsePB)* resp, \
      RpcContext rpc) { \
    HANDLE_ON_LEADER_WITH_LOCK(class_name, method_name); \
  }

#define MASTER_SERVICE_IMPL_ON_ALL_MASTERS(r, class_name, method_name) \
  void MasterServiceImpl::method_name( \
      const BOOST_PP_CAT(method_name, RequestPB)* req, \
      BOOST_PP_CAT(method_name, ResponsePB)* resp, \
      RpcContext rpc) { \
    HANDLE_ON_ALL_MASTERS(class_name, method_name); \
  }

BOOST_PP_SEQ_FOR_EACH(
    MASTER_SERVICE_IMPL_ON_LEADER_WITH_LOCK, CatalogManager,
    (CreateTable)
    (IsCreateTableDone)
    (AnalyzeTable)
    (TruncateTable)
    (IsTruncateTableDone)
    (BackfillIndex)
    (LaunchBackfillIndexForTable)
    (GetBackfillJobs)
    (DeleteTable)
    (IsDeleteTableDone)
    (AlterTable)
    (IsAlterTableDone)
    (ListTables)
    (GetTableSchema)
    (GetColocatedTabletSchema)
    (CreateNamespace)
    (IsCreateNamespaceDone)
    (DeleteNamespace)
    (IsDeleteNamespaceDone)
    (AlterNamespace)
    (ListNamespaces)
    (GetNamespaceInfo)
    (ReservePgsqlOids)
    (GetYsqlCatalogConfig)
    (CreateTablegroup)
    (DeleteTablegroup)
    (ListTablegroups)
    (RedisConfigSet)
    (RedisConfigGet)
    (CreateUDType)
    (DeleteUDType)
    (ListUDTypes)
    (GetUDTypeInfo)
    (SetPreferredZones)
    (IsLoadBalanced)
    (IsLoadBalancerIdle)
    (AreLeadersOnPreferredOnly)
    (SplitTablet)
    (DeleteTablet)
    (DdlLog)
    (FlushSysCatalog)
)


void MasterServiceImpl::GetTableLocations(const GetTableLocationsRequestPB* req,
                                          GetTableLocationsResponsePB* resp,
                                          RpcContext rpc) {
  // We can't use the HANDLE_ON_LEADER_WITH_LOCK macro here because we have to inject latency
  // before acquiring the leader lock.
  HandleOnLeader(req, resp, &rpc, [&]() -> Status {
    if (PREDICT_FALSE(FLAGS_master_inject_latency_on_tablet_lookups_ms > 0)) {
      SleepFor(MonoDelta::FromMilliseconds(FLAGS_master_inject_latency_on_tablet_lookups_ms));
    }
    return server_->catalog_manager()->GetTableLocations(req, resp);
  }, __FILE__, __LINE__, __func__, HoldCatalogLock::kTrue);
}

// ------------------------------------------------------------------------------------------------
// Permissions
// ------------------------------------------------------------------------------------------------

BOOST_PP_SEQ_FOR_EACH(
    MASTER_SERVICE_IMPL_ON_LEADER_WITH_LOCK,
    PermissionsManager,
    (CreateRole)
    (AlterRole)
    (DeleteRole)
    (GrantRevokeRole)
    (GrantRevokePermission)
    (GetPermissions));

// ------------------------------------------------------------------------------------------------
// CDC Stream
// ------------------------------------------------------------------------------------------------

BOOST_PP_SEQ_FOR_EACH(
    MASTER_SERVICE_IMPL_ON_LEADER_WITH_LOCK,
    enterprise::CatalogManager,
    (CreateCDCStream)
    (DeleteCDCStream)
    (ListCDCStreams)
    (GetCDCStream));

// ------------------------------------------------------------------------------------------------
// Miscellaneous
// ------------------------------------------------------------------------------------------------

void MasterServiceImpl::ListTabletServers(const ListTabletServersRequestPB* req,
                                          ListTabletServersResponsePB* resp,
                                          RpcContext rpc) {
  SCOPED_LEADER_SHARED_LOCK(l, server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
    return;
  }

  std::vector<std::shared_ptr<TSDescriptor> > descs;
  if (!req->primary_only()) {
    server_->ts_manager()->GetAllDescriptors(&descs);
  } else {
    server_->ts_manager()->GetAllLiveDescriptorsInCluster(
        &descs,
        server_->catalog_manager()->placement_uuid());
  }

  for (const std::shared_ptr<TSDescriptor>& desc : descs) {
    ListTabletServersResponsePB::Entry* entry = resp->add_servers();
    auto ts_info = *desc->GetTSInformationPB();
    *entry->mutable_instance_id() = std::move(*ts_info.mutable_tserver_instance());
    *entry->mutable_registration() = std::move(*ts_info.mutable_registration());
    entry->set_millis_since_heartbeat(desc->TimeSinceHeartbeat().ToMilliseconds());
    entry->set_alive(desc->IsLive());
    desc->GetMetrics(entry->mutable_metrics());
  }
  rpc.RespondSuccess();
}

void MasterServiceImpl::ListLiveTabletServers(const ListLiveTabletServersRequestPB* req,
                                              ListLiveTabletServersResponsePB* resp,
                                              RpcContext rpc) {
  SCOPED_LEADER_SHARED_LOCK(l, server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
    return;
  }
  string placement_uuid = server_->catalog_manager()->placement_uuid();

  vector<std::shared_ptr<TSDescriptor> > descs;
  server_->ts_manager()->GetAllLiveDescriptors(&descs);

  for (const std::shared_ptr<TSDescriptor>& desc : descs) {
    ListLiveTabletServersResponsePB::Entry* entry = resp->add_servers();
    auto ts_info = *desc->GetTSInformationPB();
    *entry->mutable_instance_id() = std::move(*ts_info.mutable_tserver_instance());
    *entry->mutable_registration() = std::move(*ts_info.mutable_registration());
    bool isPrimary = server_->ts_manager()->IsTsInCluster(desc, placement_uuid);
    entry->set_isfromreadreplica(!isPrimary);
  }
  rpc.RespondSuccess();
}

void MasterServiceImpl::ListMasters(
    const ListMastersRequestPB* req,
    ListMastersResponsePB* resp,
    RpcContext rpc) {
  std::vector<ServerEntryPB> masters;
  Status s = server_->ListMasters(&masters);
  if (s.ok()) {
    for (const ServerEntryPB& master : masters) {
      resp->add_masters()->CopyFrom(master);
    }
    rpc.RespondSuccess();
  } else {
    SetupErrorAndRespond(resp->mutable_error(), s, MasterErrorPB_Code_UNKNOWN_ERROR, &rpc);
  }
}

void MasterServiceImpl::ListMasterRaftPeers(
    const ListMasterRaftPeersRequestPB* req,
    ListMasterRaftPeersResponsePB* resp,
    RpcContext rpc) {
  std::vector<RaftPeerPB> masters;
  Status s = server_->ListRaftConfigMasters(&masters);
  if (s.ok()) {
    for (const RaftPeerPB& master : masters) {
      resp->add_masters()->CopyFrom(master);
    }
    rpc.RespondSuccess();
  } else {
    SetupErrorAndRespond(resp->mutable_error(), s, MasterErrorPB_Code_UNKNOWN_ERROR, &rpc);
  }
}

void MasterServiceImpl::GetMasterRegistration(const GetMasterRegistrationRequestPB* req,
                                              GetMasterRegistrationResponsePB* resp,
                                              RpcContext rpc) {
  // instance_id must always be set in order for status pages to be useful.
  if (RandomActWithProbability(FLAGS_master_slow_get_registration_probability)) {
    std::this_thread::sleep_for(20s);
  }
  resp->mutable_instance_id()->CopyFrom(server_->instance_pb());
  SCOPED_LEADER_SHARED_LOCK(l, server_->catalog_manager());
  if (!l.CheckIsInitializedOrRespond(resp, &rpc)) {
    return;
  }
  Status s = server_->GetMasterRegistration(resp->mutable_registration());
  CheckRespErrorOrSetUnknown(s, resp);
  auto role = server_->catalog_manager()->Role();
  if (role == RaftPeerPB::LEADER) {
    if (!l.leader_status().ok()) {
      YB_LOG_EVERY_N_SECS(INFO, 1)
          << "Patching role from leader to follower because of: " << l.leader_status()
          << THROTTLE_MSG;
      role = RaftPeerPB::FOLLOWER;
    }
  }
  resp->set_role(role);
  rpc.RespondSuccess();
}

void MasterServiceImpl::DumpState(
    const DumpMasterStateRequestPB* req,
    DumpMasterStateResponsePB* resp,
    RpcContext rpc) {
  SCOPED_LEADER_SHARED_LOCK(l, server_->catalog_manager());
  if (!l.CheckIsInitializedOrRespond(resp, &rpc)) {
    return;
  }

  const string role = (req->has_peers_also() && req->peers_also() ? "Leader" : "Follower");
  const string title = role + " Master " + server_->instance_pb().permanent_uuid();

  if (req->return_dump_as_string()) {
    ostringstream ss;
    server_->catalog_manager()->DumpState(&ss, req->on_disk());
    resp->set_dump(title + ":\n" + ss.str());
  } else {
    LOG(INFO) << title;
    server_->catalog_manager()->DumpState(&LOG(INFO), req->on_disk());
  }

  if (req->has_peers_also() && req->peers_also()) {
    std::vector<RaftPeerPB> masters_raft;
    Status s = server_->ListRaftConfigMasters(&masters_raft);
    CheckRespErrorOrSetUnknown(s, resp);

    if (!s.ok())
      return;

    LOG(INFO) << "Sending dump command to " << masters_raft.size()-1 << " peers.";

    // Remove our entry before broadcasting to all peers.
    std::vector<RaftPeerPB>::iterator it;
    bool found = false;
    for (it = masters_raft.begin(); it != masters_raft.end(); it++) {
      RaftPeerPB peerPB = *it;
      if (server_->instance_pb().permanent_uuid() == peerPB.permanent_uuid()) {
        found = true;
        break;
      }
    }

    if (!found) {
      CHECK(found) << "Did not find leader in Raft config";
    }

    masters_raft.erase(it);

    s = server_->catalog_manager()->PeerStateDump(masters_raft, req, resp);
    CheckRespErrorOrSetUnknown(s, resp);
  }

  rpc.RespondSuccess();
}

void MasterServiceImpl::RemovedMasterUpdate(const RemovedMasterUpdateRequestPB* req,
                                            RemovedMasterUpdateResponsePB* resp,
                                            RpcContext rpc) {
  SCOPED_LEADER_SHARED_LOCK(l, server_->catalog_manager());
  if (!l.CheckIsInitializedOrRespond(resp, &rpc)) {
    return;
  }

  Status s = server_->GoIntoShellMode();
  CheckRespErrorOrSetUnknown(s, resp);
  rpc.RespondSuccess();
}

void MasterServiceImpl::ChangeLoadBalancerState(
    const ChangeLoadBalancerStateRequestPB* req, ChangeLoadBalancerStateResponsePB* resp,
    RpcContext rpc) {
  // This should work on both followers and leaders, in order to cover leader failover!
  if (req->has_is_enabled()) {
    LOG(INFO) << "Changing balancer state to " << req->is_enabled();
    server_->catalog_manager()->SetLoadBalancerEnabled(req->is_enabled());
  }

  rpc.RespondSuccess();
}

void MasterServiceImpl::GetLoadBalancerState(
    const GetLoadBalancerStateRequestPB* req, GetLoadBalancerStateResponsePB* resp,
    RpcContext rpc) {
  resp->set_is_enabled(server_->catalog_manager()->IsLoadBalancerEnabled());
  rpc.RespondSuccess();
}

void MasterServiceImpl::GetMasterClusterConfig(
    const GetMasterClusterConfigRequestPB* req, GetMasterClusterConfigResponsePB* resp,
    RpcContext rpc) {
  HANDLE_ON_LEADER_WITH_LOCK(CatalogManager, GetClusterConfig);
}

void MasterServiceImpl::ChangeMasterClusterConfig(
    const ChangeMasterClusterConfigRequestPB* req, ChangeMasterClusterConfigResponsePB* resp,
    RpcContext rpc) {
  HANDLE_ON_LEADER_WITH_LOCK(CatalogManager, SetClusterConfig);
}

void MasterServiceImpl::GetLoadMoveCompletion(
    const GetLoadMovePercentRequestPB* req, GetLoadMovePercentResponsePB* resp,
    RpcContext rpc) {
  HANDLE_ON_LEADER_WITH_LOCK(CatalogManager, GetLoadMoveCompletionPercent);
}

void MasterServiceImpl::GetLeaderBlacklistCompletion(
    const GetLeaderBlacklistPercentRequestPB* req, GetLoadMovePercentResponsePB* resp,
    RpcContext rpc) {
  HANDLE_ON_LEADER_WITH_LOCK(CatalogManager, GetLeaderBlacklistCompletionPercent);
}

void MasterServiceImpl::IsMasterLeaderServiceReady(
    const IsMasterLeaderReadyRequestPB* req, IsMasterLeaderReadyResponsePB* resp,
    RpcContext rpc) {
  SCOPED_LEADER_SHARED_LOCK(l, server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
    return;
  }

  rpc.RespondSuccess();
}

BOOST_PP_SEQ_FOR_EACH(
    MASTER_SERVICE_IMPL_ON_LEADER_WITH_LOCK, FlushManager,
    (FlushTables)
    (IsFlushTablesDone));

void MasterServiceImpl::IsInitDbDone(const IsInitDbDoneRequestPB* req,
                                     IsInitDbDoneResponsePB* resp,
                                     RpcContext rpc) {
  HANDLE_ON_LEADER_WITHOUT_LOCK(CatalogManager, IsInitDbDone);
}

BOOST_PP_SEQ_FOR_EACH(
    MASTER_SERVICE_IMPL_ON_ALL_MASTERS, EncryptionManager,
    (GetUniverseKeyRegistry)
    (AddUniverseKeys)
    (HasUniverseKeyInMemory));

BOOST_PP_SEQ_FOR_EACH(
    MASTER_SERVICE_IMPL_ON_LEADER_WITH_LOCK, enterprise::CatalogManager,
    (SetupUniverseReplication)
    (DeleteUniverseReplication)
    (AlterUniverseReplication)
    (SetUniverseReplicationEnabled)
    (GetUniverseReplication)
    (IsSetupUniverseReplicationDone)
    (ChangeEncryptionInfo)
    (IsEncryptionEnabled));

} // namespace master
} // namespace yb
