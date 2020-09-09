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

#include <gflags/gflags.h>

#include "yb/common/wire_protocol.h"
#include "yb/master/catalog_manager-internal.h"
#include "yb/master/flush_manager.h"
#include "yb/master/master_service_base-internal.h"
#include "yb/master/master.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"
#include "yb/master/encryption_manager.h"
#include "yb/server/webserver.h"
#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/flag_tags.h"
#include "yb/util/random_util.h"
#include "yb/util/shared_lock.h"

DEFINE_int64(tablet_split_size_threshold_bytes, 0,
             "Threshold on tablet size after which tablet should be split. Automated splitting is "
             "disabled if this value is set to 0");

DEFINE_int32(master_inject_latency_on_tablet_lookups_ms, 0,
             "Number of milliseconds that the master will sleep before responding to "
             "requests for tablet locations.");
TAG_FLAG(master_inject_latency_on_tablet_lookups_ms, unsafe);
TAG_FLAG(master_inject_latency_on_tablet_lookups_ms, hidden);

DEFINE_test_flag(bool, master_fail_transactional_tablet_lookups, false,
                 "Whether to fail all lookup requests to transactional table.");

DEFINE_double(master_slow_get_registration_probability, 0,
              "Probability of injecting delay in GetMasterRegistration.");

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
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());

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

  // TODO: KUDU-86 if something fails after this point the TS will not be able
  //       to register again.

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

  ts_desc->UpdateHeartbeatTime();
  ts_desc->set_num_live_replicas(req->num_live_tablets());
  ts_desc->set_leader_count(req->leader_count());

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
    // TODO(tsplit): for now we only do splitting in case there is no full tablet report to
    // minimize probability of TSHeartbeat RPC timeout and retry.
    // This will be improved to handle split retries appropriately and then we won't need that
    // check.
    for (const auto& tablet : req->tablets_for_split()) {
      LOG(INFO) << "Got tablet to split: " << AsString(tablet);
      const auto split_status = server_->catalog_manager()->SplitTablet(
          tablet.tablet_id(), tablet.split_encoded_key(), tablet.split_partition_key());
      if (!split_status.ok()) {
        LOG(WARNING) << split_status;
      }
    }
  }

  if (!ts_desc->has_tablet_report()) {
    resp->set_needs_full_tablet_report(true);
  }

  // Retrieve all the nodes known by the master.
  std::vector<std::shared_ptr<TSDescriptor>> descs;
  server_->ts_manager()->GetAllLiveDescriptors(&descs);
  for (const auto& desc : descs) {
    *resp->add_tservers() = *desc->GetTSInformationPB();
  }

  // Retrieve the ysql catalog schema version.
  uint64_t version = server_->catalog_manager()->GetYsqlCatalogVersion();
  resp->set_ysql_catalog_version(version);

  if (FLAGS_tablet_split_size_threshold_bytes > 0) {
    resp->set_tablet_split_size_threshold_bytes(FLAGS_tablet_split_size_threshold_bytes);
  }

  rpc.RespondSuccess();
}

void MasterServiceImpl::GetTabletLocations(const GetTabletLocationsRequestPB* req,
                                           GetTabletLocationsResponsePB* resp,
                                           RpcContext rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
    return;
  }

  if (PREDICT_FALSE(FLAGS_master_inject_latency_on_tablet_lookups_ms > 0)) {
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_master_inject_latency_on_tablet_lookups_ms));
  }
  if (PREDICT_FALSE(FLAGS_TEST_master_fail_transactional_tablet_lookups)) {
    std::vector<scoped_refptr<TableInfo>> tables;
    server_->catalog_manager()->GetAllTables(&tables);
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

  for (const TabletId& tablet_id : req->tablet_ids()) {
    // TODO: once we have catalog data. ACL checks would also go here, probably.
    TabletLocationsPB* locs_pb = resp->add_tablet_locations();
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

void MasterServiceImpl::CreateTable(const CreateTableRequestPB* req,
                                    CreateTableResponsePB* resp,
                                    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::CreateTable);
}

void MasterServiceImpl::IsCreateTableDone(const IsCreateTableDoneRequestPB* req,
                                          IsCreateTableDoneResponsePB* resp,
                                          RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsCreateTableDone);
}

void MasterServiceImpl::TruncateTable(const TruncateTableRequestPB* req,
                                      TruncateTableResponsePB* resp,
                                      RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::TruncateTable);
}

void MasterServiceImpl::IsTruncateTableDone(const IsTruncateTableDoneRequestPB* req,
                                            IsTruncateTableDoneResponsePB* resp,
                                            RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsTruncateTableDone);
}

void MasterServiceImpl::DeleteTable(const DeleteTableRequestPB* req,
                                    DeleteTableResponsePB* resp,
                                    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::DeleteTable);
}

void MasterServiceImpl::IsDeleteTableDone(const IsDeleteTableDoneRequestPB* req,
                                          IsDeleteTableDoneResponsePB* resp,
                                          RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsDeleteTableDone);
}

void MasterServiceImpl::AlterTable(const AlterTableRequestPB* req,
                                   AlterTableResponsePB* resp,
                                   RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::AlterTable);
}

void MasterServiceImpl::IsAlterTableDone(const IsAlterTableDoneRequestPB* req,
                                         IsAlterTableDoneResponsePB* resp,
                                         RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsAlterTableDone);
}

void MasterServiceImpl::ListTables(const ListTablesRequestPB* req,
                                   ListTablesResponsePB* resp,
                                   RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::ListTables);
}

void MasterServiceImpl::GetTableLocations(const GetTableLocationsRequestPB* req,
                                          GetTableLocationsResponsePB* resp,
                                          RpcContext rpc) {
  HandleOnLeader(req, resp, &rpc, [&]() -> Status {
    if (PREDICT_FALSE(FLAGS_master_inject_latency_on_tablet_lookups_ms > 0)) {
      SleepFor(MonoDelta::FromMilliseconds(FLAGS_master_inject_latency_on_tablet_lookups_ms));
    }
    return server_->catalog_manager()->GetTableLocations(req, resp); });
}

void MasterServiceImpl::GetTableSchema(const GetTableSchemaRequestPB* req,
                                       GetTableSchemaResponsePB* resp,
                                       RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::GetTableSchema);
}

void MasterServiceImpl::CreateNamespace(const CreateNamespaceRequestPB* req,
                                        CreateNamespaceResponsePB* resp,
                                        RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::CreateNamespace);
}

void MasterServiceImpl::IsCreateNamespaceDone(const IsCreateNamespaceDoneRequestPB* req,
                                              IsCreateNamespaceDoneResponsePB* resp,
                                              rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsCreateNamespaceDone);
}

void MasterServiceImpl::DeleteNamespace(const DeleteNamespaceRequestPB* req,
                                        DeleteNamespaceResponsePB* resp,
                                        RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::DeleteNamespace);
}

void MasterServiceImpl::IsDeleteNamespaceDone(const IsDeleteNamespaceDoneRequestPB* req,
                                              IsDeleteNamespaceDoneResponsePB* resp,
                                              RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsDeleteNamespaceDone);
}

void MasterServiceImpl::AlterNamespace(const AlterNamespaceRequestPB* req,
                                       AlterNamespaceResponsePB* resp,
                                       RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::AlterNamespace);
}

void MasterServiceImpl::ListNamespaces(const ListNamespacesRequestPB* req,
                                       ListNamespacesResponsePB* resp,
                                       RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::ListNamespaces);
}

void MasterServiceImpl::GetNamespaceInfo(const GetNamespaceInfoRequestPB* req,
                                         GetNamespaceInfoResponsePB* resp,
                                         RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::GetNamespaceInfo);
}

void MasterServiceImpl::ReservePgsqlOids(const ReservePgsqlOidsRequestPB* req,
                                         ReservePgsqlOidsResponsePB* resp,
                                         rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::ReservePgsqlOids);
}

void MasterServiceImpl::GetYsqlCatalogConfig(const GetYsqlCatalogConfigRequestPB* req,
                                             GetYsqlCatalogConfigResponsePB* resp,
                                             rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::GetYsqlCatalogConfig);
}

// ------------------------------------------------------------------------------------------------
// Tablegroup
// ------------------------------------------------------------------------------------------------

void MasterServiceImpl::CreateTablegroup(const CreateTablegroupRequestPB* req,
                                         CreateTablegroupResponsePB* resp,
                                         rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::CreateTablegroup);
}

void MasterServiceImpl::DeleteTablegroup(const DeleteTablegroupRequestPB* req,
                                         DeleteTablegroupResponsePB* resp,
                                         rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::DeleteTablegroup);
}

void MasterServiceImpl::ListTablegroups(const ListTablegroupsRequestPB* req,
                                        ListTablegroupsResponsePB* resp,
                                        rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::ListTablegroups);
}

// ------------------------------------------------------------------------------------------------
// Permissions
// ------------------------------------------------------------------------------------------------

void MasterServiceImpl::CreateRole(const CreateRoleRequestPB* req,
                                   CreateRoleResponsePB* resp,
                                   rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &PermissionsManager::CreateRole);
}

void MasterServiceImpl::AlterRole(const AlterRoleRequestPB* req,
                                  AlterRoleResponsePB* resp,
                                  rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &PermissionsManager::AlterRole);
}

void MasterServiceImpl::DeleteRole(const DeleteRoleRequestPB* req,
                                   DeleteRoleResponsePB* resp,
                                   rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &PermissionsManager::DeleteRole);
}

void MasterServiceImpl::GrantRevokeRole(const GrantRevokeRoleRequestPB* req,
                                        GrantRevokeRoleResponsePB* resp,
                                        rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &PermissionsManager::GrantRevokeRole);
}

void MasterServiceImpl::GrantRevokePermission(const GrantRevokePermissionRequestPB* req,
                                              GrantRevokePermissionResponsePB* resp,
                                              rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &PermissionsManager::GrantRevokePermission);
}

void MasterServiceImpl::GetPermissions(const GetPermissionsRequestPB* req,
                                       GetPermissionsResponsePB* resp,
                                       rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &PermissionsManager::GetPermissions);
}

// ------------------------------------------------------------------------------------------------
// Redis
// ------------------------------------------------------------------------------------------------

void MasterServiceImpl::RedisConfigSet(
    const RedisConfigSetRequestPB* req, RedisConfigSetResponsePB* resp, rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::RedisConfigSet);
}

void MasterServiceImpl::RedisConfigGet(
    const RedisConfigGetRequestPB* req, RedisConfigGetResponsePB* resp, rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::RedisConfigGet);
}

// ------------------------------------------------------------------------------------------------
// YCQL user-defined types
// ------------------------------------------------------------------------------------------------

void MasterServiceImpl::CreateUDType(const CreateUDTypeRequestPB* req,
                                     CreateUDTypeResponsePB* resp,
                                     rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::CreateUDType);
}

void MasterServiceImpl::DeleteUDType(const DeleteUDTypeRequestPB* req,
                                     DeleteUDTypeResponsePB* resp,
                                     rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::DeleteUDType);
}

void MasterServiceImpl::ListUDTypes(const ListUDTypesRequestPB* req,
                                    ListUDTypesResponsePB* resp,
                                    rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::ListUDTypes);
}

void MasterServiceImpl::GetUDTypeInfo(const GetUDTypeInfoRequestPB* req,
                                      GetUDTypeInfoResponsePB* resp,
                                      rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::GetUDTypeInfo);
}

// ------------------------------------------------------------------------------------------------
// CDC Stream
// ------------------------------------------------------------------------------------------------

void MasterServiceImpl::CreateCDCStream(const CreateCDCStreamRequestPB* req,
                                        CreateCDCStreamResponsePB* resp,
                                        rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::CreateCDCStream);
}

void MasterServiceImpl::DeleteCDCStream(const DeleteCDCStreamRequestPB* req,
                                        DeleteCDCStreamResponsePB* resp,
                                        rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::DeleteCDCStream);
}

void MasterServiceImpl::ListCDCStreams(const ListCDCStreamsRequestPB* req,
                                       ListCDCStreamsResponsePB* resp,
                                       rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::ListCDCStreams);
}

void MasterServiceImpl::GetCDCStream(const GetCDCStreamRequestPB* req,
                                     GetCDCStreamResponsePB* resp,
                                     rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::GetCDCStream);
}

// ------------------------------------------------------------------------------------------------
// Miscellaneous
// ------------------------------------------------------------------------------------------------

void MasterServiceImpl::ListTabletServers(const ListTabletServersRequestPB* req,
                                          ListTabletServersResponsePB* resp,
                                          RpcContext rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
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
    entry->set_alive(TSManager::IsTSLive(desc));
    desc->GetMetrics(entry->mutable_metrics());
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
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
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
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
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
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
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

void MasterServiceImpl::SetPreferredZones(
    const SetPreferredZonesRequestPB* req, SetPreferredZonesResponsePB* resp,
    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::SetPreferredZones);
}

void MasterServiceImpl::GetMasterClusterConfig(
    const GetMasterClusterConfigRequestPB* req, GetMasterClusterConfigResponsePB* resp,
    RpcContext rpc) {
  // Explicit instantiation here because the handler method has a few overloadings.
  HandleIn<CatalogManager>(req, resp, &rpc, &CatalogManager::GetClusterConfig);
}

void MasterServiceImpl::ChangeMasterClusterConfig(
    const ChangeMasterClusterConfigRequestPB* req, ChangeMasterClusterConfigResponsePB* resp,
    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::SetClusterConfig);
}

void MasterServiceImpl::GetLoadMoveCompletion(
    const GetLoadMovePercentRequestPB* req, GetLoadMovePercentResponsePB* resp,
    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::GetLoadMoveCompletionPercent);
}

void MasterServiceImpl::GetLeaderBlacklistCompletion(
    const GetLeaderBlacklistPercentRequestPB* req, GetLoadMovePercentResponsePB* resp,
    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::GetLeaderBlacklistCompletionPercent);
}

void MasterServiceImpl::IsMasterLeaderServiceReady(
    const IsMasterLeaderReadyRequestPB* req, IsMasterLeaderReadyResponsePB* resp,
    RpcContext rpc) {
  CatalogManager::ScopedLeaderSharedLock l(server_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespond(resp, &rpc)) {
    return;
  }

  rpc.RespondSuccess();
}

void MasterServiceImpl::IsLoadBalanced(
    const IsLoadBalancedRequestPB* req, IsLoadBalancedResponsePB* resp,
    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsLoadBalanced);
}

void MasterServiceImpl::IsLoadBalancerIdle(
    const IsLoadBalancerIdleRequestPB* req, IsLoadBalancerIdleResponsePB* resp,
    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsLoadBalancerIdle);
}

void MasterServiceImpl::AreLeadersOnPreferredOnly(
    const AreLeadersOnPreferredOnlyRequestPB* req, AreLeadersOnPreferredOnlyResponsePB* resp,
    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::AreLeadersOnPreferredOnly);
}

void MasterServiceImpl::FlushTables(const FlushTablesRequestPB* req,
                                    FlushTablesResponsePB* resp,
                                    RpcContext rpc) {
  HandleIn(req, resp, &rpc, &FlushManager::FlushTables);
}

void MasterServiceImpl::IsFlushTablesDone(const IsFlushTablesDoneRequestPB* req,
                                          IsFlushTablesDoneResponsePB* resp,
                                          RpcContext rpc) {
  HandleIn(req, resp, &rpc, &FlushManager::IsFlushTablesDone);
}

void MasterServiceImpl::IsInitDbDone(const IsInitDbDoneRequestPB* req,
                                     IsInitDbDoneResponsePB* resp,
                                     RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::IsInitDbDone, HoldCatalogLock::kFalse);
}

void MasterServiceImpl::ChangeEncryptionInfo(const ChangeEncryptionInfoRequestPB* req,
                                             ChangeEncryptionInfoResponsePB* resp,
                                             rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::ChangeEncryptionInfo);
}

void MasterServiceImpl::IsEncryptionEnabled(const IsEncryptionEnabledRequestPB* req,
                                            IsEncryptionEnabledResponsePB* resp,
                                            rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::IsEncryptionEnabled);
}

void MasterServiceImpl::GetUniverseKeyRegistry(const GetUniverseKeyRegistryRequestPB* req,
                                               GetUniverseKeyRegistryResponsePB* resp,
                                               rpc::RpcContext rpc) {
  HandleOnAllMasters(req, resp, &rpc, &EncryptionManager::GetUniverseKeyRegistry);
}

void MasterServiceImpl::AddUniverseKeys(const AddUniverseKeysRequestPB* req,
                                        AddUniverseKeysResponsePB* resp,
                                        rpc::RpcContext rpc) {
  HandleOnAllMasters(req, resp, &rpc, &EncryptionManager::AddUniverseKeys);
}

void MasterServiceImpl::HasUniverseKeyInMemory(const HasUniverseKeyInMemoryRequestPB* req,
                                               HasUniverseKeyInMemoryResponsePB* resp,
                                               rpc::RpcContext rpc) {
  HandleOnAllMasters(req, resp, &rpc, &EncryptionManager::HasUniverseKeyInMemory);
}

void MasterServiceImpl::SetupUniverseReplication(const SetupUniverseReplicationRequestPB* req,
                                                 SetupUniverseReplicationResponsePB* resp,
                                                 rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::SetupUniverseReplication);
}

void MasterServiceImpl::DeleteUniverseReplication(const DeleteUniverseReplicationRequestPB* req,
                                                  DeleteUniverseReplicationResponsePB* resp,
                                                  rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::DeleteUniverseReplication);
}

void MasterServiceImpl::AlterUniverseReplication(const AlterUniverseReplicationRequestPB* req,
                                                  AlterUniverseReplicationResponsePB* resp,
                                                  rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::AlterUniverseReplication);
}

void MasterServiceImpl::SetUniverseReplicationEnabled(
                          const SetUniverseReplicationEnabledRequestPB* req,
                          SetUniverseReplicationEnabledResponsePB* resp,
                          rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::SetUniverseReplicationEnabled);
}

void MasterServiceImpl::GetUniverseReplication(const GetUniverseReplicationRequestPB* req,
                                               GetUniverseReplicationResponsePB* resp,
                                               rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &enterprise::CatalogManager::GetUniverseReplication);
}

void MasterServiceImpl::SplitTablet(
    const SplitTabletRequestPB* req, SplitTabletResponsePB* resp, rpc::RpcContext rpc) {
  HandleIn(req, resp, &rpc, &CatalogManager::SplitTablet);
}

} // namespace master
} // namespace yb
