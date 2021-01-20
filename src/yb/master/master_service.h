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
#ifndef YB_MASTER_MASTER_SERVICE_H
#define YB_MASTER_MASTER_SERVICE_H

#include "yb/master/master.service.h"
#include "yb/master/master_service_base.h"

namespace yb {
namespace master {

class TSDescriptor;

// Implementation of the master service. See master.proto for docs
// on each RPC.
class MasterServiceImpl : public MasterServiceIf,
                          public MasterServiceBase {
 public:
  explicit MasterServiceImpl(Master* server);
  MasterServiceImpl(const MasterServiceImpl&) = delete;
  void operator=(const MasterServiceImpl&) = delete;

  void TSHeartbeat(const TSHeartbeatRequestPB* req,
                   TSHeartbeatResponsePB* resp,
                   rpc::RpcContext rpc) override;

  void GetTabletLocations(const GetTabletLocationsRequestPB* req,
                          GetTabletLocationsResponsePB* resp,
                          rpc::RpcContext rpc) override;

  void CreateTable(const CreateTableRequestPB* req,
                   CreateTableResponsePB* resp,
                   rpc::RpcContext rpc) override;
  void IsCreateTableDone(const IsCreateTableDoneRequestPB* req,
                         IsCreateTableDoneResponsePB* resp,
                         rpc::RpcContext rpc) override;
  void TruncateTable(const TruncateTableRequestPB* req,
                     TruncateTableResponsePB* resp,
                     rpc::RpcContext rpc) override;
  void IsTruncateTableDone(const IsTruncateTableDoneRequestPB* req,
                           IsTruncateTableDoneResponsePB* resp,
                           rpc::RpcContext rpc) override;
  void BackfillIndex(const BackfillIndexRequestPB* req,
                     BackfillIndexResponsePB* resp,
                     rpc::RpcContext rpc) override;
  void DeleteTable(const DeleteTableRequestPB* req,
                   DeleteTableResponsePB* resp,
                   rpc::RpcContext rpc) override;
  void IsDeleteTableDone(const IsDeleteTableDoneRequestPB* req,
                         IsDeleteTableDoneResponsePB* resp,
                         rpc::RpcContext rpc) override;
  void AlterTable(const AlterTableRequestPB* req,
                  AlterTableResponsePB* resp,
                  rpc::RpcContext rpc) override;
  void IsAlterTableDone(const IsAlterTableDoneRequestPB* req,
                        IsAlterTableDoneResponsePB* resp,
                        rpc::RpcContext rpc) override;
  void ListTables(const ListTablesRequestPB* req,
                  ListTablesResponsePB* resp,
                  rpc::RpcContext rpc) override;
  void GetTableLocations(const GetTableLocationsRequestPB* req,
                         GetTableLocationsResponsePB* resp,
                         rpc::RpcContext rpc) override;
  void GetTableSchema(const GetTableSchemaRequestPB* req,
                      GetTableSchemaResponsePB* resp,
                      rpc::RpcContext rpc) override;
  void GetColocatedTabletSchema(const GetColocatedTabletSchemaRequestPB* req,
                                GetColocatedTabletSchemaResponsePB* resp,
                                rpc::RpcContext rpc) override;
  void ListTabletServers(const ListTabletServersRequestPB* req,
                         ListTabletServersResponsePB* resp,
                         rpc::RpcContext rpc) override;

  void CreateNamespace(const CreateNamespaceRequestPB* req,
                       CreateNamespaceResponsePB* resp,
                       rpc::RpcContext rpc) override;
  void IsCreateNamespaceDone(const IsCreateNamespaceDoneRequestPB* req,
                             IsCreateNamespaceDoneResponsePB* resp,
                             rpc::RpcContext rpc) override;
  void DeleteNamespace(const DeleteNamespaceRequestPB* req,
                       DeleteNamespaceResponsePB* resp,
                       rpc::RpcContext rpc) override;
  void IsDeleteNamespaceDone(const IsDeleteNamespaceDoneRequestPB* req,
                             IsDeleteNamespaceDoneResponsePB* resp,
                             rpc::RpcContext rpc) override;
  void AlterNamespace(const AlterNamespaceRequestPB* req,
                      AlterNamespaceResponsePB* resp,
                      rpc::RpcContext rpc) override;
  void ListNamespaces(const ListNamespacesRequestPB* req,
                      ListNamespacesResponsePB* resp,
                      rpc::RpcContext rpc) override;
  void GetNamespaceInfo(const GetNamespaceInfoRequestPB* req,
                        GetNamespaceInfoResponsePB* resp,
                        rpc::RpcContext rpc) override;

  void ReservePgsqlOids(const ReservePgsqlOidsRequestPB* req,
                        ReservePgsqlOidsResponsePB* resp,
                        rpc::RpcContext rpc) override;

  void GetYsqlCatalogConfig(const GetYsqlCatalogConfigRequestPB* req,
                            GetYsqlCatalogConfigResponsePB* resp,
                            rpc::RpcContext rpc) override;

  void CreateTablegroup(const CreateTablegroupRequestPB* req,
                        CreateTablegroupResponsePB* resp,
                        rpc::RpcContext rpc) override;

  void DeleteTablegroup(const DeleteTablegroupRequestPB* req,
                        DeleteTablegroupResponsePB* resp,
                        rpc::RpcContext rpc) override;

  void ListTablegroups(const ListTablegroupsRequestPB* req,
                       ListTablegroupsResponsePB* resp,
                       rpc::RpcContext rpc) override;

  void CreateRole(const CreateRoleRequestPB* req,
                  CreateRoleResponsePB* resp,
                  rpc::RpcContext rpc) override;
  void AlterRole(const AlterRoleRequestPB* req,
                 AlterRoleResponsePB* resp,
                 rpc::RpcContext rpc) override;
  void DeleteRole(const DeleteRoleRequestPB* req,
                  DeleteRoleResponsePB* resp,
                  rpc::RpcContext rpc) override;
  void GrantRevokeRole(const GrantRevokeRoleRequestPB* req,
                       GrantRevokeRoleResponsePB* resp,
                       rpc::RpcContext rpc) override;
  void GrantRevokePermission(const GrantRevokePermissionRequestPB* req,
                             GrantRevokePermissionResponsePB* resp,
                             rpc::RpcContext rpc) override;
  void GetPermissions(const GetPermissionsRequestPB* req,
                      GetPermissionsResponsePB* resp,
                      rpc::RpcContext rpc) override;

  void RedisConfigSet(const RedisConfigSetRequestPB* req,
                      RedisConfigSetResponsePB* resp,
                      rpc::RpcContext rpc) override;
  void RedisConfigGet(const RedisConfigGetRequestPB* req,
                      RedisConfigGetResponsePB* resp,
                      rpc::RpcContext rpc) override;

  void CreateUDType(const CreateUDTypeRequestPB* req,
                    CreateUDTypeResponsePB* resp,
                    rpc::RpcContext rpc) override;
  void DeleteUDType(const DeleteUDTypeRequestPB* req,
                    DeleteUDTypeResponsePB* resp,
                    rpc::RpcContext rpc) override;
  void ListUDTypes(const ListUDTypesRequestPB* req,
                   ListUDTypesResponsePB* resp,
                   rpc::RpcContext rpc) override;
  void GetUDTypeInfo(const GetUDTypeInfoRequestPB* req,
                     GetUDTypeInfoResponsePB* resp,
                     rpc::RpcContext rpc) override;

  void CreateCDCStream(const CreateCDCStreamRequestPB* req,
                       CreateCDCStreamResponsePB* resp,
                       rpc::RpcContext rpc) override;
  void DeleteCDCStream(const DeleteCDCStreamRequestPB* req,
                       DeleteCDCStreamResponsePB* resp,
                       rpc::RpcContext rpc) override;
  void ListCDCStreams(const ListCDCStreamsRequestPB* req,
                      ListCDCStreamsResponsePB* resp,
                      rpc::RpcContext rpc) override;
  void GetCDCStream(const GetCDCStreamRequestPB* req,
                    GetCDCStreamResponsePB* resp,
                    rpc::RpcContext rpc) override;

  void ListMasters(const ListMastersRequestPB* req,
                   ListMastersResponsePB* resp,
                   rpc::RpcContext rpc) override;

  void ListMasterRaftPeers(const ListMasterRaftPeersRequestPB* req,
                           ListMasterRaftPeersResponsePB* resp,
                           rpc::RpcContext rpc) override;

  void GetMasterRegistration(const GetMasterRegistrationRequestPB* req,
                             GetMasterRegistrationResponsePB* resp,
                             rpc::RpcContext rpc) override;

  void DumpState(const DumpMasterStateRequestPB* req,
                 DumpMasterStateResponsePB* resp,
                 rpc::RpcContext rpc) override;

  void ChangeLoadBalancerState(const ChangeLoadBalancerStateRequestPB* req,
                               ChangeLoadBalancerStateResponsePB* resp,
                               rpc::RpcContext rpc) override;

  void GetLoadBalancerState(const GetLoadBalancerStateRequestPB* req,
                            GetLoadBalancerStateResponsePB* resp,
                            rpc::RpcContext rpc) override;

  void RemovedMasterUpdate(const RemovedMasterUpdateRequestPB* req,
                           RemovedMasterUpdateResponsePB* resp,
                           rpc::RpcContext rpc) override;

  void SetPreferredZones(const SetPreferredZonesRequestPB* req,
                         SetPreferredZonesResponsePB* resp,
                         rpc::RpcContext rpc) override;

  void GetMasterClusterConfig(const GetMasterClusterConfigRequestPB* req,
                              GetMasterClusterConfigResponsePB* resp,
                              rpc::RpcContext rpc) override;

  void ChangeMasterClusterConfig(const ChangeMasterClusterConfigRequestPB* req,
                                 ChangeMasterClusterConfigResponsePB* resp,
                                 rpc::RpcContext rpc) override;

  void GetLoadMoveCompletion(const GetLoadMovePercentRequestPB* req,
                             GetLoadMovePercentResponsePB* resp,
                             rpc::RpcContext rpc) override;

  void GetLeaderBlacklistCompletion(const GetLeaderBlacklistPercentRequestPB* req,
                             GetLoadMovePercentResponsePB* resp,
                             rpc::RpcContext rpc) override;

  void IsMasterLeaderServiceReady(const IsMasterLeaderReadyRequestPB* req,
                                  IsMasterLeaderReadyResponsePB* resp,
                                  rpc::RpcContext rpc) override;

  void IsLoadBalanced(const IsLoadBalancedRequestPB* req,
                      IsLoadBalancedResponsePB* resp,
                      rpc::RpcContext rpc) override;

  void IsLoadBalancerIdle(const IsLoadBalancerIdleRequestPB* req,
                          IsLoadBalancerIdleResponsePB* resp,
                          rpc::RpcContext rpc) override;

  void AreLeadersOnPreferredOnly(const AreLeadersOnPreferredOnlyRequestPB* req,
                                 AreLeadersOnPreferredOnlyResponsePB* resp,
                                 rpc::RpcContext rpc) override;

  void FlushTables(const FlushTablesRequestPB* req,
                   FlushTablesResponsePB* resp,
                   rpc::RpcContext rpc) override;

  void IsFlushTablesDone(const IsFlushTablesDoneRequestPB* req,
                         IsFlushTablesDoneResponsePB* resp,
                         rpc::RpcContext rpc) override;

  void IsInitDbDone(const IsInitDbDoneRequestPB* req,
                    IsInitDbDoneResponsePB* resp,
                    rpc::RpcContext rpc) override;

  void ChangeEncryptionInfo(const ChangeEncryptionInfoRequestPB* req,
                            ChangeEncryptionInfoResponsePB* resp,
                            rpc::RpcContext rpc) override;

  void IsEncryptionEnabled(const IsEncryptionEnabledRequestPB* req,
                           IsEncryptionEnabledResponsePB* resp,
                           rpc::RpcContext rpc) override;

  void GetUniverseKeyRegistry(const GetUniverseKeyRegistryRequestPB* req,
                              GetUniverseKeyRegistryResponsePB* resp,
                              rpc::RpcContext rpc) override;

  void AddUniverseKeys(const AddUniverseKeysRequestPB* req,
                       AddUniverseKeysResponsePB* resp,
                       rpc::RpcContext rpc) override;

  void HasUniverseKeyInMemory(const HasUniverseKeyInMemoryRequestPB* req,
                              HasUniverseKeyInMemoryResponsePB* resp,
                              rpc::RpcContext rpc) override;

  void SetupUniverseReplication(const SetupUniverseReplicationRequestPB* req,
                                SetupUniverseReplicationResponsePB* resp,
                                rpc::RpcContext rpc) override;

  void DeleteUniverseReplication(const DeleteUniverseReplicationRequestPB* req,
                                 DeleteUniverseReplicationResponsePB* resp,
                                 rpc::RpcContext rpc) override;

  void AlterUniverseReplication(const AlterUniverseReplicationRequestPB* req,
                                AlterUniverseReplicationResponsePB* resp,
                                rpc::RpcContext rpc) override;

  void SetUniverseReplicationEnabled(const SetUniverseReplicationEnabledRequestPB* req,
                                     SetUniverseReplicationEnabledResponsePB* resp,
                                     rpc::RpcContext rpc) override;

  void GetUniverseReplication(const GetUniverseReplicationRequestPB* req,
                              GetUniverseReplicationResponsePB* resp,
                              rpc::RpcContext rpc) override;

  void SplitTablet(
      const SplitTabletRequestPB* req, SplitTabletResponsePB* resp, rpc::RpcContext rpc) override;

  void DeleteTablet(
      const DeleteTabletRequestPB* req, DeleteTabletResponsePB* resp, rpc::RpcContext rpc) override;

 private:
};

} // namespace master
} // namespace yb

#endif // YB_MASTER_MASTER_SERVICE_H
