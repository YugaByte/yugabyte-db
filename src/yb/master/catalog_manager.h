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

#ifndef YB_MASTER_CATALOG_MANAGER_H
#define YB_MASTER_CATALOG_MANAGER_H

#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/optional/optional_fwd.hpp>
#include <boost/functional/hash.hpp>
#include <gtest/internal/gtest-internal.h>

#include "yb/common/entity_ids.h"
#include "yb/common/index.h"
#include "yb/common/partition.h"
#include "yb/common/transaction.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/thread_annotations.h"
#include "yb/master/async_rpc_tasks.h"
#include "yb/master/catalog_entity_info.h"
#include "yb/master/master_defaults.h"
#include "yb/master/permissions_manager.h"
#include "yb/master/sys_catalog_initialization.h"
#include "yb/master/scoped_leader_shared_lock.h"
#include "yb/master/system_tablet.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"
#include "yb/master/yql_virtual_table.h"
#include "yb/master/ysql_transaction_ddl.h"
#include "yb/rpc/rpc.h"
#include "yb/server/monitored_task.h"
#include "yb/tserver/tablet_peer_lookup.h"
#include "yb/util/cow_object.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/oid_generator.h"
#include "yb/util/promise.h"
#include "yb/util/random.h"
#include "yb/util/rw_mutex.h"
#include "yb/util/status.h"
#include "yb/util/test_macros.h"
#include "yb/util/version_tracker.h"

namespace yb {

class Schema;
class ThreadPool;

template<class T>
class AtomicGauge;

namespace pgwrapper {

#define CALL_GTEST_TEST_CLASS_NAME_(...) GTEST_TEST_CLASS_NAME_(__VA_ARGS__)
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBMarkDeleted));
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBUpdateSysTablet));
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBWithTables));
#undef CALL_GTEST_TEST_CLASS_NAME_

}

namespace tablet {

struct TableInfo;
enum RaftGroupStatePB;

}

namespace master {

class CatalogManagerBgTasks;
class ClusterLoadBalancer;
class Master;
class SysCatalogTable;
class TableInfo;
class TSDescriptor;
class ChangeEncryptionInfoRequestPB;
class ChangeEncryptionInfoResponsePB;
class TasksTracker;

struct DeferredAssignmentActions;

static const char* const kSecurityConfigType = "security-configuration";
static const char* const kYsqlCatalogConfigType = "ysql-catalog-configuration";
static const char* const kColocatedParentTableIdSuffix = ".colocated.parent.uuid";
static const char* const kColocatedParentTableNameSuffix = ".colocated.parent.tablename";
static const char* const kTablegroupParentTableIdSuffix = ".tablegroup.parent.uuid";
static const char* const kTablegroupParentTableNameSuffix = ".tablegroup.parent.tablename";

using PlacementId = std::string;

typedef unordered_map<TabletId, TabletServerId> TabletToTabletServerMap;

// Component within the catalog manager which tracks blacklist (decommission) operation
// related information.
class BlacklistState {
 public:
  BlacklistState() { Reset(); }
  ~BlacklistState() {}

  void Reset();

  std::string ToString();

  // Set of blacklisted servers host/ports. Protected by leader_lock_ in catalog manager.
  std::unordered_set<HostPort, HostPortHash> tservers_;
  // In-memory tracker for initial blacklist load.
  int64_t initial_load_;
};

// The component of the master which tracks the state and location
// of tables/tablets in the cluster.
//
// This is the master-side counterpart of TSTabletManager, which tracks
// the state of each tablet on a given tablet-server.
//
// Thread-safe.
class CatalogManager : public tserver::TabletPeerLookupIf {
  typedef std::unordered_map<NamespaceName, scoped_refptr<NamespaceInfo> > NamespaceInfoMap;

  class NamespaceNameMapper {
   public:
    NamespaceInfoMap& operator[](YQLDatabase db_type);
    const NamespaceInfoMap& operator[](YQLDatabase db_type) const;
    void clear();

   private:
    std::array<NamespaceInfoMap, 4> typed_maps_;
  };

 public:
  // Some code refers to ScopedLeaderSharedLock as CatalogManager::ScopedLeaderSharedLock.
  using ScopedLeaderSharedLock = ::yb::master::ScopedLeaderSharedLock;

  explicit CatalogManager(Master *master);
  virtual ~CatalogManager();

  CHECKED_STATUS Init();

  bool StartShutdown();
  void CompleteShutdown();
  CHECKED_STATUS CheckOnline() const;

  // Create Postgres sys catalog table.
  CHECKED_STATUS CreatePgsqlSysTable(const CreateTableRequestPB* req, CreateTableResponsePB* resp);

  CHECKED_STATUS ReplicatePgMetadataChange(const tserver::ChangeMetadataRequestPB* req);

  // Reserve Postgres oids for a Postgres database.
  CHECKED_STATUS ReservePgsqlOids(const ReservePgsqlOidsRequestPB* req,
                                  ReservePgsqlOidsResponsePB* resp,
                                  rpc::RpcContext* rpc);

  // Get the info (current only version) for the ysql system catalog.
  CHECKED_STATUS GetYsqlCatalogConfig(const GetYsqlCatalogConfigRequestPB* req,
                                      GetYsqlCatalogConfigResponsePB* resp,
                                      rpc::RpcContext* rpc);

  // Copy Postgres sys catalog tables into a new namespace.
  CHECKED_STATUS CopyPgsqlSysTables(const NamespaceId& namespace_id,
                                    const std::vector<scoped_refptr<TableInfo>>& tables);

  // Create a new Table with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS CreateTable(const CreateTableRequestPB* req,
                             CreateTableResponsePB* resp,
                             rpc::RpcContext* rpc);

  // Create the transaction status table if needed (i.e. if it does not exist already).
  //
  // This is called at the end of CreateTable if the table has transactions enabled.
  CHECKED_STATUS CreateTransactionsStatusTableIfNeeded(rpc::RpcContext *rpc);

  // Create the metrics snapshots table if needed (i.e. if it does not exist already).
  //
  // This is called at the end of CreateTable.
  CHECKED_STATUS CreateMetricsSnapshotsTableIfNeeded(rpc::RpcContext *rpc);

  // Get the information about an in-progress create operation.
  CHECKED_STATUS IsCreateTableDone(const IsCreateTableDoneRequestPB* req,
                                   IsCreateTableDoneResponsePB* resp);

  CHECKED_STATUS IsCreateTableInProgress(const TableId& table_id,
                                         CoarseTimePoint deadline,
                                         bool* create_in_progress);

  CHECKED_STATUS WaitForCreateTableToFinish(const TableId& table_id);

  // Check if the transaction status table creation is done.
  //
  // This is called at the end of IsCreateTableDone if the table has transactions enabled.
  CHECKED_STATUS IsTransactionStatusTableCreated(IsCreateTableDoneResponsePB* resp);

  // Check if the metrics snapshots table creation is done.
  //
  // This is called at the end of IsCreateTableDone.
  CHECKED_STATUS IsMetricsSnapshotsTableCreated(IsCreateTableDoneResponsePB* resp);

  // Called when transaction associated with table create finishes. Verifies postgres layer present.
  CHECKED_STATUS VerifyTablePgLayer(scoped_refptr<TableInfo> table, bool txn_query_succeeded);

  // Truncate the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS TruncateTable(const TruncateTableRequestPB* req,
                               TruncateTableResponsePB* resp,
                               rpc::RpcContext* rpc);

  // Get the information about an in-progress truncate operation.
  CHECKED_STATUS IsTruncateTableDone(const IsTruncateTableDoneRequestPB* req,
                                     IsTruncateTableDoneResponsePB* resp);

  // Backfill the specified index.  Currently only supported for YSQL.  YCQL does not need this as
  // master automatically runs backfill according to the DocDB permissions.
  CHECKED_STATUS BackfillIndex(
      const BackfillIndexRequestPB* req,
      BackfillIndexResponsePB* resp,
      rpc::RpcContext* rpc);

  // Delete the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS DeleteTable(const DeleteTableRequestPB* req,
                             DeleteTableResponsePB* resp,
                             rpc::RpcContext* rpc);
  CHECKED_STATUS DeleteTableInternal(
      const DeleteTableRequestPB* req, DeleteTableResponsePB* resp, rpc::RpcContext* rpc);

  // Get the information about an in-progress delete operation.
  CHECKED_STATUS IsDeleteTableDone(const IsDeleteTableDoneRequestPB* req,
                                   IsDeleteTableDoneResponsePB* resp);

  // Alter the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS AlterTable(const AlterTableRequestPB* req,
                            AlterTableResponsePB* resp,
                            rpc::RpcContext* rpc);

  // Get the information about an in-progress alter operation.
  CHECKED_STATUS IsAlterTableDone(const IsAlterTableDoneRequestPB* req,
                                  IsAlterTableDoneResponsePB* resp);

  // Get the information about the specified table.
  CHECKED_STATUS GetTableSchema(const GetTableSchemaRequestPB* req,
                                GetTableSchemaResponsePB* resp);

  // Get the information about the specified colocated databsae.
  CHECKED_STATUS GetColocatedTabletSchema(const GetColocatedTabletSchemaRequestPB* req,
                                          GetColocatedTabletSchemaResponsePB* resp);

  // List all the running tables.
  CHECKED_STATUS ListTables(const ListTablesRequestPB* req,
                            ListTablesResponsePB* resp);

  CHECKED_STATUS GetTableLocations(const GetTableLocationsRequestPB* req,
                                   GetTableLocationsResponsePB* resp);

  // Lookup tablet by ID, then call GetTabletLocations below.
  CHECKED_STATUS GetTabletLocations(const TabletId& tablet_id, TabletLocationsPB* locs_pb);

  // Look up the locations of the given tablet. The locations
  // vector is overwritten (not appended to).
  // If the tablet is not found, returns Status::NotFound.
  // If the tablet is not running, returns Status::ServiceUnavailable.
  // Otherwise, returns Status::OK and puts the result in 'locs_pb'.
  // This only returns tablets which are in RUNNING state.
  CHECKED_STATUS GetTabletLocations(scoped_refptr<TabletInfo> tablet_info,
                                    TabletLocationsPB* locs_pb);

  // Returns the system tablet in catalog manager by the id.
  Result<std::shared_ptr<tablet::AbstractTablet>> GetSystemTablet(const TabletId& id);

  // Handle a tablet report from the given tablet server.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS ProcessTabletReport(TSDescriptor* ts_desc,
                                     const TabletReportPB& report,
                                     TabletReportUpdatesPB *report_update,
                                     rpc::RpcContext* rpc);

  // Create a new Namespace with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS CreateNamespace(const CreateNamespaceRequestPB* req,
                                 CreateNamespaceResponsePB* resp,
                                 rpc::RpcContext* rpc);
  // Get the information about an in-progress create operation.
  CHECKED_STATUS IsCreateNamespaceDone(const IsCreateNamespaceDoneRequestPB* req,
                                       IsCreateNamespaceDoneResponsePB* resp);

  // Delete the specified Namespace.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS DeleteNamespace(const DeleteNamespaceRequestPB* req,
                                 DeleteNamespaceResponsePB* resp,
                                 rpc::RpcContext* rpc);
  // Get the information about an in-progress delete operation.
  CHECKED_STATUS IsDeleteNamespaceDone(const IsDeleteNamespaceDoneRequestPB* req,
                                       IsDeleteNamespaceDoneResponsePB* resp);

  // Alter the specified Namespace.
  CHECKED_STATUS AlterNamespace(const AlterNamespaceRequestPB* req,
                                AlterNamespaceResponsePB* resp,
                                rpc::RpcContext* rpc);

  // User API to Delete YSQL database tables.
  CHECKED_STATUS DeleteYsqlDatabase(const DeleteNamespaceRequestPB* req,
                                    DeleteNamespaceResponsePB* resp,
                                    rpc::RpcContext* rpc);

  // Work to delete YSQL database tables, handled asynchronously from the User API call.
  void DeleteYsqlDatabaseAsync(scoped_refptr<NamespaceInfo> database);

  // Delete all tables in YSQL database.
  CHECKED_STATUS DeleteYsqlDBTables(const scoped_refptr<NamespaceInfo>& database);

  // List all the current namespaces.
  CHECKED_STATUS ListNamespaces(const ListNamespacesRequestPB* req,
                                ListNamespacesResponsePB* resp);

  // Get information about a namespace.
  CHECKED_STATUS GetNamespaceInfo(const GetNamespaceInfoRequestPB* req,
                                  GetNamespaceInfoResponsePB* resp,
                                  rpc::RpcContext* rpc);

  // Set Redis Config
  CHECKED_STATUS RedisConfigSet(const RedisConfigSetRequestPB* req,
                                RedisConfigSetResponsePB* resp,
                                rpc::RpcContext* rpc);

  // Get Redis Config
  CHECKED_STATUS RedisConfigGet(const RedisConfigGetRequestPB* req,
                                RedisConfigGetResponsePB* resp,
                                rpc::RpcContext* rpc);

  CHECKED_STATUS CreateTablegroup(const CreateTablegroupRequestPB* req,
                                  CreateTablegroupResponsePB* resp,
                                  rpc::RpcContext* rpc);

  CHECKED_STATUS DeleteTablegroup(const DeleteTablegroupRequestPB* req,
                                  DeleteTablegroupResponsePB* resp,
                                  rpc::RpcContext* rpc);

  // List all the current tablegroups for a namespace.
  CHECKED_STATUS ListTablegroups(const ListTablegroupsRequestPB* req,
                                 ListTablegroupsResponsePB* resp,
                                 rpc::RpcContext* rpc);

  bool HasTablegroups();

  // Create a new User-Defined Type with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS CreateUDType(const CreateUDTypeRequestPB* req,
                              CreateUDTypeResponsePB* resp,
                              rpc::RpcContext* rpc);

  // Delete the specified UDType.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS DeleteUDType(const DeleteUDTypeRequestPB* req,
                              DeleteUDTypeResponsePB* resp,
                              rpc::RpcContext* rpc);

  // List all user defined types in given namespaces.
  CHECKED_STATUS ListUDTypes(const ListUDTypesRequestPB* req,
                             ListUDTypesResponsePB* resp);

  // Get the info (id, name, namespace, fields names, field types) of a (user-defined) type.
  CHECKED_STATUS GetUDTypeInfo(const GetUDTypeInfoRequestPB* req,
                               GetUDTypeInfoResponsePB* resp,
                               rpc::RpcContext* rpc);

  // Delete CDC streams for a table.
  virtual CHECKED_STATUS DeleteCDCStreamsForTable(const TableId& table_id);
  virtual CHECKED_STATUS DeleteCDCStreamsForTables(const vector<TableId>& table_ids);

  virtual CHECKED_STATUS ChangeEncryptionInfo(const ChangeEncryptionInfoRequestPB* req,
                                              ChangeEncryptionInfoResponsePB* resp);

  Result<uint64_t> IncrementYsqlCatalogVersion();

  // Records the fact that initdb has succesfully completed.
  CHECKED_STATUS InitDbFinished(Status initdb_status, int64_t term);

  // Check if the initdb operation has been completed. This is intended for use by whoever wants
  // to wait for the cluster to be fully initialized, e.g. minicluster, YugaWare, etc.
  CHECKED_STATUS IsInitDbDone(const IsInitDbDoneRequestPB* req, IsInitDbDoneResponsePB* resp);

  CHECKED_STATUS GetYsqlCatalogVersion(uint64_t* catalog_version, uint64_t* last_breaking_version);

  virtual CHECKED_STATUS FillHeartbeatResponse(const TSHeartbeatRequestPB* req,
                                               TSHeartbeatResponsePB* resp);

  SysCatalogTable* sys_catalog() { return sys_catalog_.get(); }

  // Dump all of the current state about tables and tablets to the
  // given output stream. This is verbose, meant for debugging.
  virtual void DumpState(std::ostream* out, bool on_disk_dump = false) const;

  void SetLoadBalancerEnabled(bool is_enabled);

  bool IsLoadBalancerEnabled();

  // Return the table info for the table with the specified UUID, if it exists.
  scoped_refptr<TableInfo> GetTableInfo(const TableId& table_id);
  scoped_refptr<TableInfo> GetTableInfoUnlocked(const TableId& table_id) REQUIRES_SHARED(lock_);

  // Get Table info given namespace id and table name.
  scoped_refptr<TableInfo> GetTableInfoFromNamespaceNameAndTableName(
      YQLDatabase db_type, const NamespaceName& namespace_name, const TableName& table_name);

  // Return all the available TableInfo. The flag 'includeOnlyRunningTables' determines whether
  // to retrieve all Tables irrespective of their state or just 'RUNNING' tables.
  // To retrieve all live tables in the system, you should set this flag to true.
  void GetAllTables(std::vector<scoped_refptr<TableInfo> > *tables,
                    bool includeOnlyRunningTables = false);

  // Return all the available NamespaceInfo. The flag 'includeOnlyRunningNamespaces' determines
  // whether to retrieve all Namespaces irrespective of their state or just 'RUNNING' namespaces.
  // To retrieve all live tables in the system, you should set this flag to true.
  void GetAllNamespaces(std::vector<scoped_refptr<NamespaceInfo> >* namespaces,
                        bool includeOnlyRunningNamespaces = false);

  // Return all the available (user-defined) types.
  void GetAllUDTypes(std::vector<scoped_refptr<UDTypeInfo> >* types);

  // Return the recent tasks.
  std::vector<std::shared_ptr<MonitoredTask>> GetRecentTasks();

  // Return the recent user-initiated jobs.
  std::vector<std::shared_ptr<MonitoredTask>> GetRecentJobs();

  NamespaceName GetNamespaceNameUnlocked(const NamespaceId& id) const REQUIRES_SHARED(lock_);
  NamespaceName GetNamespaceName(const NamespaceId& id) const;

  NamespaceName GetNamespaceNameUnlocked(const scoped_refptr<TableInfo>& table) const
      REQUIRES_SHARED(lock_);
  NamespaceName GetNamespaceName(const scoped_refptr<TableInfo>& table) const;

  // Is the table a system table?
  bool IsSystemTable(const TableInfo& table) const;

  // Is the table a user created table?
  bool IsUserTable(const TableInfo& table) const;
  bool IsUserTableUnlocked(const TableInfo& table) const REQUIRES_SHARED(lock_);

  // Is the table a user created index?
  bool IsUserIndex(const TableInfo& table) const;
  bool IsUserIndexUnlocked(const TableInfo& table) const REQUIRES_SHARED(lock_);

  // Is the table a special sequences system table?
  bool IsSequencesSystemTable(const TableInfo& table) const;

  // Is the table id from a table created for colocated database?
  bool IsColocatedParentTableId(const TableId& table_id) const;

  // Is the table a table created for colocated database?
  bool IsColocatedParentTable(const TableInfo& table) const;

  // Is the table a table created for a tablegroup?
  bool IsTablegroupParentTable(const TableInfo& table) const;

  // Is the table a table created in a colocated database?
  bool IsColocatedUserTable(const TableInfo& table) const;

  // Is the table created by user?
  // Note that table can be regular table or index in this case.
  bool IsUserCreatedTable(const TableInfo& table) const;
  bool IsUserCreatedTableUnlocked(const TableInfo& table) const REQUIRES_SHARED(lock_);

  // Let the catalog manager know that we have received a response for a delete tablet request,
  // and that we either deleted the tablet successfully, or we received a fatal error.
  //
  // Async tasks should call this when they finish. The last such tablet peer notification will
  // trigger trying to transition the table from DELETING to DELETED state.
  void NotifyTabletDeleteFinished(
      const TabletServerId& tserver_uuid, const TableId& table_id, scoped_refptr<TableInfo> table);

  // For a DeleteTable, we first mark tables as DELETING then move them to DELETED once all
  // outstanding tasks are complete and the TS side tablets are deleted.
  // For system tables or colocated tables, we just need outstanding tasks to be done.
  //
  // If all conditions are met, returns a lock in WRITE mode on this table, else nullptr.
  std::unique_ptr<TableInfo::lock_type> MaybeTransitionTableToDeleted(
      scoped_refptr<TableInfo> table);

  // Used by ConsensusService to retrieve the TabletPeer for a system
  // table specified by 'tablet_id'.
  //
  // See also: TabletPeerLookupIf, ConsensusServiceImpl.
  CHECKED_STATUS GetTabletPeer(
      const TabletId& tablet_id,
      std::shared_ptr<tablet::TabletPeer>* tablet_peer) const override;

  const NodeInstancePB& NodeInstance() const override;

  CHECKED_STATUS GetRegistration(ServerRegistrationPB* reg) const override;

  bool IsInitialized() const;

  virtual CHECKED_STATUS StartRemoteBootstrap(const consensus::StartRemoteBootstrapRequestPB& req)
      override;

  int GetNumReplicasFromPlacementInfo(const PlacementInfoPB& placement_info);

  // Loops through the table's placement infos to make sure the overall replication info is valid.
  virtual CHECKED_STATUS CheckValidReplicationInfo(const ReplicationInfoPB& replication_info,
                                                   const TSDescriptorVector& all_ts_descs,
                                                   const vector<Partition>& partitions,
                                                   CreateTableResponsePB* resp);

  // Makes sure the available ts_descs in a placement can accomodate the placement config.
  CHECKED_STATUS CheckValidPlacementInfo(const PlacementInfoPB& placement_info,
                                         const TSDescriptorVector& ts_descs,
                                         const vector<Partition>& partitions,
                                         CreateTableResponsePB* resp);

  // Loops through the table's placement infos and populates the corresponding config from
  // each placement.
  virtual CHECKED_STATUS HandlePlacementUsingReplicationInfo(
      const ReplicationInfoPB& replication_info,
      const TSDescriptorVector& all_ts_descs,
      consensus::RaftConfigPB* config);

  // Handles the config creation for a given placement.
  CHECKED_STATUS HandlePlacementUsingPlacementInfo(const PlacementInfoPB& placement_info,
                                                   const TSDescriptorVector& ts_descs,
                                                   consensus::RaftPeerPB::MemberType member_type,
                                                   consensus::RaftConfigPB* config);

    // Set the current committed config.
  CHECKED_STATUS GetCurrentConfig(consensus::ConsensusStatePB *cpb) const;

  // Return OK if this CatalogManager is a leader in a consensus configuration and if
  // the required leader state (metadata for tables and tablets) has
  // been successfully loaded into memory. CatalogManager must be
  // initialized before calling this method.
  CHECKED_STATUS CheckIsLeaderAndReady() const;

  // Returns this CatalogManager's role in a consensus configuration. CatalogManager
  // must be initialized before calling this method.
  consensus::RaftPeerPB::Role Role() const;

  CHECKED_STATUS PeerStateDump(const vector<consensus::RaftPeerPB>& masters_raft,
                               const DumpMasterStateRequestPB* req,
                               DumpMasterStateResponsePB* resp);

  // If we get removed from an existing cluster, leader might ask us to detach ourselves from the
  // cluster. So we enter a shell mode equivalent state, with no bg tasks and no tablet peer
  // nor consensus.
  CHECKED_STATUS GoIntoShellMode();

  // Setters and getters for the cluster config item.
  //
  // To change the cluster config, a client would need to do a client-side read-modify-write by
  // issuing a get for the latest config, obtaining the current valid config (together with its
  // respective version number), modify the values it wants of said config and issuing a write
  // afterwards, without changing the version number. In case the version number does not match
  // on the server, the change will fail and the client will have to retry the get, as someone
  // must have updated the config in the meantime.
  CHECKED_STATUS GetClusterConfig(GetMasterClusterConfigResponsePB* resp);
  CHECKED_STATUS GetClusterConfig(SysClusterConfigEntryPB* config);
  CHECKED_STATUS SetClusterConfig(
      const ChangeMasterClusterConfigRequestPB* req, ChangeMasterClusterConfigResponsePB* resp);

  CHECKED_STATUS SetPreferredZones(
      const SetPreferredZonesRequestPB* req, SetPreferredZonesResponsePB* resp);

  CHECKED_STATUS GetReplicationFactor(int* num_replicas);
  CHECKED_STATUS GetReplicationFactor(NamespaceName namespace_name, int* num_replicas) {
    // TODO ENG-282 We currently don't support per-namespace replication factor.
    return GetReplicationFactor(num_replicas);
  }
  CHECKED_STATUS GetReplicationFactorForTablet(const scoped_refptr<TabletInfo>& tablet,
      int* num_replicas);

  void GetExpectedNumberOfReplicas(int* num_live_replicas, int* num_read_replicas);

  // Get the percentage of tablets that have been moved off of the black-listed tablet servers.
  CHECKED_STATUS GetLoadMoveCompletionPercent(GetLoadMovePercentResponsePB* resp);

  // Get the percentage of leaders that have been moved off of the leader black-listed tablet
  // servers.
  CHECKED_STATUS GetLeaderBlacklistCompletionPercent(GetLoadMovePercentResponsePB* resp);

  // Get the percentage of leaders/tablets that have been moved off of the (leader) black-listed
  // tablet servers.
  CHECKED_STATUS GetLoadMoveCompletionPercent(GetLoadMovePercentResponsePB* resp,
      bool blacklist_leader) EXCLUDES(blacklist_lock_);

  // API to check if all the live tservers have similar tablet workload.
  CHECKED_STATUS IsLoadBalanced(const IsLoadBalancedRequestPB* req,
                                IsLoadBalancedResponsePB* resp);

  CHECKED_STATUS IsLoadBalancerIdle(const IsLoadBalancerIdleRequestPB* req,
                                    IsLoadBalancerIdleResponsePB* resp);

  // API to check that all tservers that shouldn't have leader load do not.
  CHECKED_STATUS AreLeadersOnPreferredOnly(const AreLeadersOnPreferredOnlyRequestPB* req,
                                           AreLeadersOnPreferredOnlyResponsePB* resp);

  // Return the placement uuid of the primary cluster containing this master.
  string placement_uuid() const;

  // Clears out the existing metadata ('table_names_map_', 'table_ids_map_',
  // and 'tablet_map_'), loads tables metadata into memory and if successful
  // loads the tablets metadata.
  CHECKED_STATUS VisitSysCatalog(int64_t term);
  virtual CHECKED_STATUS RunLoaders(int64_t term) REQUIRES(lock_);

  // Waits for the worker queue to finish processing, returns OK if worker queue is idle before
  // the provided timeout, TimedOut Status otherwise.
  CHECKED_STATUS WaitForWorkerPoolTests(
      const MonoDelta& timeout = MonoDelta::FromSeconds(10)) const;

  // Returns whether the namespace is a YCQL namespace.
  static bool IsYcqlNamespace(const NamespaceInfo& ns);

  // Returns whether the table is a YCQL table.
  static bool IsYcqlTable(const TableInfo& table);

  CHECKED_STATUS FindNamespaceUnlocked(const NamespaceIdentifierPB& ns_identifier,
                                       scoped_refptr<NamespaceInfo>* ns_info) const
      REQUIRES_SHARED(lock_);

  CHECKED_STATUS FindNamespace(const NamespaceIdentifierPB& ns_identifier,
                               scoped_refptr<NamespaceInfo>* ns_info) const;

  CHECKED_STATUS FindTable(const TableIdentifierPB& table_identifier,
                           scoped_refptr<TableInfo>* table_info);

  Result<TableDescription> DescribeTable(const TableIdentifierPB& table_identifier);

  void AssertLeaderLockAcquiredForReading() const {
    leader_lock_.AssertAcquiredForReading();
  }

  std::string GenerateId(boost::optional<const SysRowEntry::Type> entity_type = boost::none);
  std::string GenerateIdUnlocked(boost::optional<const SysRowEntry::Type> entity_type = boost::none)
      REQUIRES_SHARED(lock_);

  ThreadPool* AsyncTaskPool() { return async_task_pool_.get(); }

  PermissionsManager* permissions_manager() {
    return permissions_manager_.get();
  }

  uintptr_t tablets_version() const {
    std::lock_guard<LockType> l_map(lock_);
    return tablet_map_.Version() + table_ids_map_.Version();
  }

  uintptr_t tablet_locations_version() const {
    return tablet_locations_version_.load(std::memory_order_acquire);
  }

  EncryptionManager& encryption_manager() {
    return *encryption_manager_;
  }

  CHECKED_STATUS SplitTablet(
      const TabletId& tablet_id, const std::string& split_encoded_key,
      const std::string& split_partition_key);

  // Splits tablet specified in the request using middle of the partition as a split point.
  CHECKED_STATUS SplitTablet(
      const SplitTabletRequestPB* req, SplitTabletResponsePB* resp, rpc::RpcContext* rpc);

  CHECKED_STATUS DeleteTablet(
      const DeleteTabletRequestPB* req, DeleteTabletResponsePB* resp, rpc::RpcContext* rpc);

  // Test wrapper around protected DoSplitTablet method.
  CHECKED_STATUS TEST_SplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info, docdb::DocKeyHash split_hash_code);

  // Schedule a task to run on the async task thread pool.
  CHECKED_STATUS ScheduleTask(std::shared_ptr<RetryingTSRpcTask> task);

  // Time since this peer became master leader. Caller should verify that it is leader before.
  MonoDelta TimeSinceElectedLeader();

  Result<std::vector<TableDescription>> CollectTables(
      const google::protobuf::RepeatedPtrField<TableIdentifierPB>& tables,
      bool add_indexes,
      bool include_parent_colocated_table = false);

 protected:
  // TODO Get rid of these friend classes and introduce formal interface.
  friend class TableLoader;
  friend class TabletLoader;
  friend class NamespaceLoader;
  friend class UDTypeLoader;
  friend class ClusterConfigLoader;
  friend class RoleLoader;
  friend class RedisConfigLoader;
  friend class SysConfigLoader;
  friend class ::yb::master::ScopedLeaderSharedLock;
  friend class PermissionsManager;
  friend class MultiStageAlterTable;
  friend class BackfillTable;
  friend class BackfillTablet;

#define CALL_FRIEND_TEST(...) FRIEND_TEST(__VA_ARGS__)
  CALL_FRIEND_TEST(pgwrapper::PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBMarkDeleted));
  CALL_FRIEND_TEST(pgwrapper::PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBUpdateSysTablet));
  CALL_FRIEND_TEST(pgwrapper::PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBWithTables));
#undef CALL_FRIEND_TEST
  FRIEND_TEST(SysCatalogTest, TestCatalogManagerTasksTracker);
  FRIEND_TEST(SysCatalogTest, TestPrepareDefaultClusterConfig);
  FRIEND_TEST(SysCatalogTest, TestSysCatalogTablesOperations);
  FRIEND_TEST(SysCatalogTest, TestSysCatalogTabletsOperations);
  FRIEND_TEST(SysCatalogTest, TestTableInfoCommit);

  FRIEND_TEST(MasterTest, TestTabletsDeletedWhenTableInDeletingState);

  // Called by SysCatalog::SysCatalogStateChanged when this node
  // becomes the leader of a consensus configuration.
  //
  // Executes LoadSysCatalogDataTask below and marks the current time as time since leader.
  CHECKED_STATUS ElectedAsLeaderCb();

  // Loops and sleeps until one of the following conditions occurs:
  // 1. The current node is the leader master in the current term
  //    and at least one op from the current term is committed. Returns OK.
  // 2. The current node is not the leader master.
  //    Returns IllegalState.
  // 3. The provided timeout expires. Returns TimedOut.
  //
  // This method is intended to ensure that all operations replicated by
  // previous masters are committed and visible to the local node before
  // reading that data, to ensure consistency across failovers.
  CHECKED_STATUS WaitUntilCaughtUpAsLeader(const MonoDelta& timeout);

  // This method is submitted to 'leader_initialization_pool_' by
  // ElectedAsLeaderCb above. It:
  // 1) Acquired 'lock_'
  // 2) Runs the various Visitors defined below
  // 3) Releases 'lock_' and if successful, updates 'leader_ready_term_'
  // to true (under state_lock_).
  void LoadSysCatalogDataTask();

  // Generated the default entry for the cluster config, that is written into sys_catalog on very
  // first leader election of the cluster.
  //
  // Sets the version field of the SysClusterConfigEntryPB to 0.
  CHECKED_STATUS PrepareDefaultClusterConfig(int64_t term) REQUIRES(lock_);

  // Sets up various system configs.
  CHECKED_STATUS PrepareDefaultSysConfig(int64_t term) REQUIRES(lock_);

  // Starts an asynchronous run of initdb. Errors are handled in the callback. Returns true
  // if started running initdb, false if decided that it is not needed.
  bool StartRunningInitDbIfNeeded(int64_t term) REQUIRES_SHARED(lock_);

  CHECKED_STATUS PrepareDefaultNamespaces(int64_t term) REQUIRES(lock_);

  CHECKED_STATUS PrepareSystemTables(int64_t term) REQUIRES(lock_);

  CHECKED_STATUS PrepareSysCatalogTable(int64_t term) REQUIRES(lock_);

  template <class T>
  CHECKED_STATUS PrepareSystemTableTemplate(const TableName& table_name,
                                            const NamespaceName& namespace_name,
                                            const NamespaceId& namespace_id,
                                            int64_t term) REQUIRES(lock_);

  CHECKED_STATUS PrepareSystemTable(const TableName& table_name,
                                    const NamespaceName& namespace_name,
                                    const NamespaceId& namespace_id,
                                    const Schema& schema,
                                    int64_t term,
                                    YQLVirtualTable* vtable) REQUIRES(lock_);

  CHECKED_STATUS PrepareNamespace(YQLDatabase db_type,
                                  const NamespaceName& name,
                                  const NamespaceId& id,
                                  int64_t term) REQUIRES(lock_);

  void ProcessPendingNamespace(NamespaceId id,
                               std::vector<scoped_refptr<TableInfo>> template_tables,
                               TransactionMetadata txn);

  // Called when transaction associated with NS create finishes. Verifies postgres layer present.
  CHECKED_STATUS VerifyNamespacePgLayer(scoped_refptr<NamespaceInfo> ns, bool txn_query_succeeded);

  CHECKED_STATUS ConsensusStateToTabletLocations(const consensus::ConsensusStatePB& cstate,
                                                 TabletLocationsPB* locs_pb);

  // Creates the table and associated tablet objects in-memory and updates the appropriate
  // catalog manager maps.
  CHECKED_STATUS CreateTableInMemory(const CreateTableRequestPB& req,
                                     const Schema& schema,
                                     const PartitionSchema& partition_schema,
                                     const bool create_tablets,
                                     const NamespaceId& namespace_id,
                                     const NamespaceName& namespace_name,
                                     const vector<Partition>& partitions,
                                     IndexInfoPB* index_info,
                                     vector<TabletInfo*>* tablets,
                                     CreateTableResponsePB* resp,
                                     scoped_refptr<TableInfo>* table) REQUIRES(lock_);
  CHECKED_STATUS CreateTabletsFromTable(const vector<Partition>& partitions,
                                        const scoped_refptr<TableInfo>& table,
                                        std::vector<TabletInfo*>* tablets) REQUIRES(lock_);

  // Helper for creating copartitioned table.
  CHECKED_STATUS CreateCopartitionedTable(const CreateTableRequestPB& req,
                                          CreateTableResponsePB* resp,
                                          rpc::RpcContext* rpc,
                                          Schema schema,
                                          scoped_refptr<NamespaceInfo> ns);

  // Check that local host is present in master addresses for normal master process start.
  // On error, it could imply that master_addresses is incorrectly set for shell master startup
  // or that this master host info was missed in the master addresses and it should be
  // participating in the very first quorum setup.
  CHECKED_STATUS CheckLocalHostInMasterAddresses();

  // Helper for initializing 'sys_catalog_'. After calling this
  // method, the caller should call WaitUntilRunning() on sys_catalog_
  // WITHOUT holding 'lock_' to wait for consensus to start for
  // sys_catalog_.
  //
  // This method is thread-safe.
  CHECKED_STATUS InitSysCatalogAsync();

  // Helper for creating the initial TableInfo state
  // Leaves the table "write locked" with the new info in the
  // "dirty" state field.
  scoped_refptr<TableInfo> CreateTableInfo(const CreateTableRequestPB& req,
                                           const Schema& schema,
                                           const PartitionSchema& partition_schema,
                                           const NamespaceId& namespace_id,
                                           const NamespaceName& namespace_name,
                                           IndexInfoPB* index_info) REQUIRES(lock_);

  // Helper for creating the initial TabletInfo state.
  // Leaves the tablet "write locked" with the new info in the
  // "dirty" state field.
  TabletInfo *CreateTabletInfo(TableInfo* table,
                               const PartitionPB& partition) REQUIRES(lock_);

  // Remove the specified entries from the protobuf field table_ids of a TabletInfo.
  Status RemoveTableIdsFromTabletInfo(
      TabletInfoPtr tablet_info, unordered_set<TableId> tables_to_remove);

  // Add index info to the indexed table.
  CHECKED_STATUS AddIndexInfoToTable(const scoped_refptr<TableInfo>& indexed_table,
                                     const IndexInfoPB& index_info,
                                     CreateTableResponsePB* resp);

  // Delete index info from the indexed table.
  CHECKED_STATUS MarkIndexInfoFromTableForDeletion(
      const TableId& indexed_table_id, const TableId& index_table_id, bool multi_stage,
      DeleteTableResponsePB* resp);

  // Delete index info from the indexed table.
  CHECKED_STATUS DeleteIndexInfoFromTable(
      const TableId& indexed_table_id, const TableId& index_table_id);

  // Builds the TabletLocationsPB for a tablet based on the provided TabletInfo.
  // Populates locs_pb and returns true on success.
  // Returns Status::ServiceUnavailable if tablet is not running.
  CHECKED_STATUS BuildLocationsForTablet(const scoped_refptr<TabletInfo>& tablet,
                                         TabletLocationsPB* locs_pb);

  // Check whether the tservers in the current replica map differs from those in the cstate when
  // processing a tablet report. Ignore the roles reported by the cstate, just compare the
  // tservers.
  bool ReplicaMapDiffersFromConsensusState(const scoped_refptr<TabletInfo>& tablet,
                                           const consensus::ConsensusStatePB& consensus_state);

  void ReconcileTabletReplicasInLocalMemoryWithReport(
      const scoped_refptr<TabletInfo>& tablet,
      const std::string& sender_uuid,
      const consensus::ConsensusStatePB& consensus_state,
      const ReportedTabletPB& report);

  // Register a tablet server whenever it heartbeats with a consensus configuration. This is
  // needed because we have logic in the Master that states that if a tablet
  // server that is part of a consensus configuration has not heartbeated to the Master yet, we
  // leave it out of the consensus configuration reported to clients.
  // TODO: See if we can remove this logic, as it seems confusing.
  void UpdateTabletReplicaInLocalMemory(TSDescriptor* ts_desc,
                                        const consensus::ConsensusStatePB* consensus_state,
                                        const ReportedTabletPB& report,
                                        const scoped_refptr<TabletInfo>& tablet_to_update);

  static void CreateNewReplicaForLocalMemory(TSDescriptor* ts_desc,
                                             const consensus::ConsensusStatePB* consensus_state,
                                             const ReportedTabletPB& report,
                                             TabletReplica* new_replica);

  // Extract the set of tablets that can be deleted and the set of tablets
  // that must be processed because not running yet.
  void ExtractTabletsToProcess(TabletInfos *tablets_to_delete,
                               TabletInfos *tablets_to_process);

  // Determine whether any tables are in the DELETING state.
  bool AreTablesDeleting();

  // Task that takes care of the tablet assignments/creations.
  // Loops through the "not created" tablets and sends a CreateTablet() request.
  CHECKED_STATUS ProcessPendingAssignments(const TabletInfos& tablets);

  // Given 'two_choices', which should be a vector of exactly two elements, select which
  // one is the better choice for a new replica.
  std::shared_ptr<TSDescriptor> PickBetterReplicaLocation(const TSDescriptorVector& two_choices);

  // Select a tablet server from 'ts_descs' on which to place a new replica.
  // Any tablet servers in 'excluded' are not considered.
  // REQUIRES: 'ts_descs' must include at least one non-excluded server.
  std::shared_ptr<TSDescriptor> SelectReplica(
      const TSDescriptorVector& ts_descs,
      const std::set<std::shared_ptr<TSDescriptor>>& excluded);

  // Select N Replicas from online tablet servers (as specified by
  // 'ts_descs') for the specified tablet and populate the consensus configuration
  // object. If 'ts_descs' does not specify enough online tablet
  // servers to select the N replicas, return Status::InvalidArgument.
  //
  // This method is called by "ProcessPendingAssignments()".
  CHECKED_STATUS SelectReplicasForTablet(const TSDescriptorVector& ts_descs, TabletInfo* tablet);

  // Select N Replicas from the online tablet servers that have been chosen to respect the
  // placement information provided. Populate the consensus configuration object with choices and
  // also update the set of selected tablet servers, to not place several replicas on the same TS.
  // member_type indicated what type of replica to select for.
  //
  // This method is called by "SelectReplicasForTablet".
  void SelectReplicas(
      const TSDescriptorVector& ts_descs,
      int nreplicas, consensus::RaftConfigPB* config,
      std::set<std::shared_ptr<TSDescriptor>>* already_selected_ts,
      consensus::RaftPeerPB::MemberType member_type);

  void HandleAssignPreparingTablet(TabletInfo* tablet,
                                   DeferredAssignmentActions* deferred);

  // Assign tablets and send CreateTablet RPCs to tablet servers.
  // The out param 'new_tablets' should have any newly-created TabletInfo
  // objects appended to it.
  void HandleAssignCreatingTablet(TabletInfo* tablet,
                                  DeferredAssignmentActions* deferred,
                                  TabletInfos* new_tablets);

  CHECKED_STATUS HandleTabletSchemaVersionReport(TabletInfo *tablet, uint32_t version,
                                                 const scoped_refptr<TableInfo>& table = nullptr);

  // Send the create tablet requests to the selected peers of the consensus configurations.
  // The creation is async, and at the moment there is no error checking on the
  // caller side. We rely on the assignment timeout. If we don't see the tablet
  // after the timeout, we regenerate a new one and proceed with a new
  // assignment/creation.
  //
  // This method is part of the "ProcessPendingAssignments()"
  //
  // This must be called after persisting the tablet state as
  // CREATING to ensure coherent state after Master failover.
  void SendCreateTabletRequests(const std::vector<TabletInfo*>& tablets);

  // Send the "alter table request" to all tablets of the specified table.
  //
  // Also, initiates the required AlterTable requests to backfill the Index.
  // Initially the index is set to be in a INDEX_PERM_DELETE_ONLY state, then
  // updated to INDEX_PERM_WRITE_AND_DELETE state; followed by backfilling. Once
  // all the tablets have completed backfilling, the index will be updated
  // to be in INDEX_PERM_READ_WRITE_AND_DELETE state.
  void SendAlterTableRequest(const scoped_refptr<TableInfo>& table,
                             const AlterTableRequestPB* req = nullptr);

  // Start the background task to send the CopartitionTable() RPC to the leader for this
  // tablet.
  void SendCopartitionTabletRequest(const scoped_refptr<TabletInfo>& tablet,
                                    const scoped_refptr<TableInfo>& table);

  // Starts the background task to send the SplitTablet RPC to the leader for the specified tablet.
  void SendSplitTabletRequest(
      const scoped_refptr<TabletInfo>& tablet, std::array<TabletId, kNumSplitParts> new_tablet_ids,
      const std::string& split_encoded_key, const std::string& split_partition_key);

  // Send the "truncate table request" to all tablets of the specified table.
  void SendTruncateTableRequest(const scoped_refptr<TableInfo>& table);

  // Start the background task to send the TruncateTable() RPC to the leader for this tablet.
  void SendTruncateTabletRequest(const scoped_refptr<TabletInfo>& tablet);

  // Truncate the specified table/index.
  CHECKED_STATUS TruncateTable(const TableId& table_id,
                               TruncateTableResponsePB* resp,
                               rpc::RpcContext* rpc);

  // Delete the specified table in memory. The TableInfo, DeletedTableInfo and lock of the deleted
  // table are appended to the lists. The caller will be responsible for committing the change and
  // deleting the actual table and tablets.
  CHECKED_STATUS DeleteTableInMemory(const TableIdentifierPB& table_identifier,
                                     bool is_index_table,
                                     bool update_indexed_table,
                                     std::vector<scoped_refptr<TableInfo>>* tables,
                                     std::vector<std::unique_ptr<TableInfo::lock_type>>* table_lcks,
                                     DeleteTableResponsePB* resp,
                                     rpc::RpcContext* rpc);

  // Request tablet servers to delete all replicas of the tablet.
  void DeleteTabletReplicas(TabletInfo* tablet, const std::string& msg);

  // Returns error if and only if it is forbidden to both:
  // 1) Delete single tablet from table.
  // 2) Delete the whole table.
  // This is used for pre-checks in both `DeleteTablet` and `DeleteTabletsAndSendRequests`.
  CHECKED_STATUS CheckIfForbiddenToDeleteTabletOf(const scoped_refptr<TableInfo>& table);

  // Marks each of the tablets in the given table as deleted and triggers requests to the tablet
  // servers to delete them. The table parameter is expected to be given "write locked".
  void DeleteTabletsAndSendRequests(const scoped_refptr<TableInfo>& table);

  // Marks tablet as deleted and triggers requests to the tablet servers to delete them.
  void DeleteTabletAndSendRequests(
      const scoped_refptr<TabletInfo>& tablet, const std::string& deletion_msg);

  // Send the "delete tablet request" to the specified TS/tablet.
  // The specified 'reason' will be logged on the TS.
  void SendDeleteTabletRequest(const TabletId& tablet_id,
                               tablet::TabletDataState delete_type,
                               const boost::optional<int64_t>& cas_config_opid_index_less_or_equal,
                               const scoped_refptr<TableInfo>& table,
                               TSDescriptor* ts_desc,
                               const std::string& reason);

  // Start a task to request the specified tablet leader to step down and optionally to remove
  // the server that is over-replicated. A new tablet server can be specified to start an election
  // immediately to become the new leader. If new_leader_ts_uuid is empty, the election will be run
  // following the protocol's default mechanism.
  void SendLeaderStepDownRequest(
      const scoped_refptr<TabletInfo>& tablet, const consensus::ConsensusStatePB& cstate,
      const string& change_config_ts_uuid, bool should_remove,
      const string& new_leader_ts_uuid = "");

  // Start a task to change the config to remove a certain voter because the specified tablet is
  // over-replicated.
  void SendRemoveServerRequest(
      const scoped_refptr<TabletInfo>& tablet, const consensus::ConsensusStatePB& cstate,
      const string& change_config_ts_uuid);

  // Start a task to change the config to add an additional voter because the
  // specified tablet is under-replicated.
  void SendAddServerRequest(
      const scoped_refptr<TabletInfo>& tablet, consensus::RaftPeerPB::MemberType member_type,
      const consensus::ConsensusStatePB& cstate, const string& change_config_ts_uuid);

  void GetPendingServerTasksUnlocked(const TableId &table_uuid,
                                     TabletToTabletServerMap *add_replica_tasks_map,
                                     TabletToTabletServerMap *remove_replica_tasks_map,
                                     TabletToTabletServerMap *stepdown_leader_tasks)
      REQUIRES_SHARED(lock_);

  // Abort creation of 'table': abort all mutation for TabletInfo and
  // TableInfo objects (releasing all COW locks), abort all pending
  // tasks associated with the table, and erase any state related to
  // the table we failed to create from the in-memory maps
  // ('table_names_map_', 'table_ids_map_', 'tablet_map_' below).
  CHECKED_STATUS AbortTableCreation(TableInfo* table,
                                    const std::vector<TabletInfo*>& tablets,
                                    const Status& s,
                                    CreateTableResponsePB* resp);

  // Returns 'table_replication_info' itself if set. Otherwise returns the cluster level
  // replication info.
  Result<ReplicationInfoPB> ResolveReplicationInfo(const ReplicationInfoPB& table_replication_info);

  // Returns whether 'replication_info' has any relevant fields set.
  bool IsReplicationInfoSet(const ReplicationInfoPB& replication_info);

  // Validates that 'replication_info' for a table has supported fields set.
  CHECKED_STATUS ValidateTableReplicationInfo(const ReplicationInfoPB& replication_info);

  // Report metrics.
  void ReportMetrics();

  // Conventional "T xxx P yyy: " prefix for logging.
  std::string LogPrefix() const;

  // Aborts all tasks belonging to 'tables' and waits for them to finish.
  void AbortAndWaitForAllTasks(const std::vector<scoped_refptr<TableInfo>>& tables);

  // Can be used to create background_tasks_ field for this master.
  // Used on normal master startup or when master comes out of the shell mode.
  CHECKED_STATUS EnableBgTasks();

  // Helper function for RebuildYQLSystemPartitions to get the system.partitions tablet.
  Status GetYQLPartitionsVTable(std::shared_ptr<SystemTablet>* tablet);
  // Background task for automatically rebuilding system.partitions every
  // partitions_vtable_cache_refresh_secs seconds.
  void RebuildYQLSystemPartitions();

  // Set the current list of black listed nodes, which is used to track the load movement off of
  // these nodes. Also sets the initial load (which is the number of tablets on these nodes)
  // when the blacklist load removal operation was started. It permits overwrite semantics
  // for the blacklist.
  CHECKED_STATUS SetBlackList(const BlacklistPB& blacklist) REQUIRES(blacklist_lock_);
  CHECKED_STATUS SetLeaderBlacklist(const BlacklistPB& leader_blacklist) REQUIRES(blacklist_lock_);

  // Registers new split tablet with `partition` for the same table as `source_tablet_info` tablet.
  // Does not change any other tablets and their partitions.
  // Returns TabletInfo for registered tablet.
  Result<TabletInfo*> RegisterNewTabletForSplit(
      TabletInfo* source_tablet_info, const PartitionPB& partition,
      TableInfo::lock_type* table_write_lock);

  Result<scoped_refptr<TabletInfo>> GetTabletInfo(const TabletId& tablet_id);

  CHECKED_STATUS DoSplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info, const std::string& split_encoded_key,
      const std::string& split_partition_key);

  // Splits tablet using specified split_hash_code as a split point.
  CHECKED_STATUS DoSplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info, docdb::DocKeyHash split_hash_code);

  // Calculate the total number of replicas which are being handled by servers in state.
  int64_t GetNumRelevantReplicas(const BlacklistState& state, bool leaders_only);

  int64_t leader_ready_term() EXCLUDES(state_lock_) {
    std::lock_guard<simple_spinlock> l(state_lock_);
    return leader_ready_term_;
  }

  // Delete tables from internal map by id, if it has no more active tasks and tablets.
  // This function should only be called from the bg_tasks thread, in a single threaded fashion!
  void CleanUpDeletedTables();

  // Called when a new table id is added to table_ids_map_.
  void HandleNewTableId(const TableId& id);

  // Creates a new TableInfo object.
  scoped_refptr<TableInfo> NewTableInfo(TableId id);

  // Register the tablet server with the ts manager using the Raft config. This is called for
  // servers that are part of the Raft config but haven't registered as yet.
  CHECKED_STATUS RegisterTsFromRaftConfig(const consensus::RaftPeerPB& peer);

  template <class Loader>
  CHECKED_STATUS Load(const std::string& title, const int64_t term);

  virtual void Started() {}

  // ----------------------------------------------------------------------------------------------
  // Private member fields
  // ----------------------------------------------------------------------------------------------

  // TODO: the maps are a little wasteful of RAM, since the TableInfo/TabletInfo
  // objects have a copy of the string key. But STL doesn't make it
  // easy to make a "gettable set".

  // Lock protecting the various in memory storage structures.
  typedef rw_spinlock LockType;
  mutable LockType lock_;

  // Note: Namespaces and tables for YSQL databases are identified by their ids only and therefore
  // are not saved in the name maps below.

  // Table map: table-id -> TableInfo
  VersionTracker<TableInfoMap> table_ids_map_ GUARDED_BY(lock_);

  // Table map: [namespace-id, table-name] -> TableInfo
  // Don't have to use VersionTracker for it, since table_ids_map_ already updated at the same time.
  TableInfoByNameMap table_names_map_ GUARDED_BY(lock_);

  // Tablet maps: tablet-id -> TabletInfo
  VersionTracker<TabletInfoMap> tablet_map_ GUARDED_BY(lock_);

  // Namespace maps: namespace-id -> NamespaceInfo and namespace-name -> NamespaceInfo
  NamespaceInfoMap namespace_ids_map_ GUARDED_BY(lock_);
  NamespaceNameMapper namespace_names_mapper_ GUARDED_BY(lock_);

  // User-Defined type maps: udtype-id -> UDTypeInfo and udtype-name -> UDTypeInfo
  UDTypeInfoMap udtype_ids_map_ GUARDED_BY(lock_);
  UDTypeInfoByNameMap udtype_names_map_ GUARDED_BY(lock_);

  // RedisConfig map: RedisConfigKey -> RedisConfigInfo
  typedef std::unordered_map<RedisConfigKey, scoped_refptr<RedisConfigInfo>> RedisConfigInfoMap;
  RedisConfigInfoMap redis_config_map_ GUARDED_BY(lock_);

  // Config information.
  scoped_refptr<ClusterConfigInfo> cluster_config_ = nullptr; // No GUARD, only write on Load.

  // YSQL Catalog information.
  scoped_refptr<SysConfigInfo> ysql_catalog_config_ = nullptr; // No GUARD, only write on Load.

  Master *master_;
  Atomic32 closing_;
  ObjectIdGenerator oid_generator_;

  // Random number generator used for selecting replica locations.
  ThreadSafeRandom rng_;

  gscoped_ptr<SysCatalogTable> sys_catalog_;

  // Mutex to avoid concurrent remote bootstrap sessions.
  std::mutex remote_bootstrap_mtx_;

  // Set to true if this master has received at least the superblock from a remote master.
  bool tablet_exists_;

  // Background thread, used to execute the catalog manager tasks
  // like the assignment and cleaner.
  friend class CatalogManagerBgTasks;
  gscoped_ptr<CatalogManagerBgTasks> background_tasks_;

  // Background threadpool, newer features use this (instead of the Background thread)
  // to execute time-lenient catalog manager tasks.
  std::unique_ptr<yb::ThreadPool> background_tasks_thread_pool_;

  mutable LockType blacklist_lock_;

  // Track all information related to the black list operations.
  BlacklistState blacklistState GUARDED_BY(blacklist_lock_);

  // Track all information related to the leader black list operations.
  BlacklistState leaderBlacklistState GUARDED_BY(blacklist_lock_);

  // TODO: convert this to YB_DEFINE_ENUM for automatic pretty-printing.
  enum State {
    kConstructed,
    kStarting,
    kRunning,
    kClosing
  };

  // Lock protecting state_, leader_ready_term_
  mutable simple_spinlock state_lock_;
  State state_ GUARDED_BY(state_lock_);

  // Used to defer Master<->TabletServer work from reactor threads onto a thread where
  // blocking behavior is permissible.
  //
  // NOTE: Presently, this thread pool must contain only a single
  // thread (to correctly serialize invocations of ElectedAsLeaderCb
  // upon closely timed consecutive elections).
  gscoped_ptr<ThreadPool> leader_initialization_pool_;

  // Thread pool to do the async RPC task work.
  gscoped_ptr<ThreadPool> async_task_pool_;

  // This field is updated when a node becomes leader master,
  // waits for all outstanding uncommitted metadata (table and tablet metadata)
  // in the sys catalog to commit, and then reads that metadata into in-memory
  // data structures. This is used to "fence" client and tablet server requests
  // that depend on the in-memory state until this master can respond
  // correctly.
  int64_t leader_ready_term_ GUARDED_BY(state_lock_);

  // Lock used to fence operations and leader elections. All logical operations
  // (i.e. create table, alter table, etc.) should acquire this lock for
  // reading. Following an election where this master is elected leader, it
  // should acquire this lock for writing before reloading the metadata.
  //
  // Readers should not acquire this lock directly; use ScopedLeadershipLock
  // instead.
  //
  // Always acquire this lock before state_lock_.
  RWMutex leader_lock_;

  // Async operations are accessing some private methods
  // (TODO: this stuff should be deferred and done in the background thread)
  friend class AsyncAlterTable;

  // Number of live tservers metric.
  scoped_refptr<AtomicGauge<uint32_t>> metric_num_tablet_servers_live_;

  // Number of dead tservers metric.
  scoped_refptr<AtomicGauge<uint32_t>> metric_num_tablet_servers_dead_;

  friend class ClusterLoadBalancer;

  // Policy for load balancing tablets on tablet servers.
  std::unique_ptr<ClusterLoadBalancer> load_balance_policy_;

  // Tablet peer for the sys catalog tablet's peer.
  const std::shared_ptr<tablet::TabletPeer> tablet_peer() const;

  // Use the Raft config that has been bootstrapped to update the in-memory state of master options
  // and also the on-disk state of the consensus meta object.
  CHECKED_STATUS UpdateMastersListInMemoryAndDisk();

  // Tablets of system tables on the master indexed by the tablet id.
  std::unordered_map<std::string, std::shared_ptr<tablet::AbstractTablet>> system_tablets_;

  // Tablet of colocated namespaces indexed by the namespace id.
  std::unordered_map<NamespaceId, scoped_refptr<TabletInfo>> colocated_tablet_ids_map_
      GUARDED_BY(lock_);

  typedef std::unordered_map<TablegroupId, scoped_refptr<TabletInfo>> TablegroupTabletMap;

  std::unordered_map<NamespaceId, TablegroupTabletMap> tablegroup_tablet_ids_map_
      GUARDED_BY(lock_);

  std::unordered_map<TablegroupId, scoped_refptr<TablegroupInfo>> tablegroup_ids_map_
      GUARDED_BY(lock_);

  boost::optional<std::future<Status>> initdb_future_;
  boost::optional<InitialSysCatalogSnapshotWriter> initial_snapshot_writer_;

  std::unique_ptr<PermissionsManager> permissions_manager_;

  // This is used for tracking that initdb has started running previously.
  std::atomic<bool> pg_proc_exists_{false};

  // Tracks most recent async tasks.
  scoped_refptr<TasksTracker> tasks_tracker_;

  // Tracks most recent user initiated jobs.
  scoped_refptr<TasksTracker> jobs_tracker_;

  std::unique_ptr<EncryptionManager> encryption_manager_;

  // A pointer to the system.partitions tablet for the RebuildYQLSystemPartitions bg task.
  std::shared_ptr<SystemTablet> system_partitions_tablet_ = nullptr;

  // Handles querying and processing YSQL DDL Transactions as a catalog manager background task.
  YsqlTransactionDdl ysql_transaction_;

  MonoTime time_elected_leader_;

  void StartElectionIfReady(
      const consensus::ConsensusStatePB& cstate, TabletInfo* tablet);

 private:
  virtual bool CDCStreamExistsUnlocked(const CDCStreamId& id) REQUIRES_SHARED(lock_);

  // Should be bumped up when tablet locations are changed.
  std::atomic<uintptr_t> tablet_locations_version_{0};

  rpc::ScheduledTaskTracker refresh_yql_partitions_task_;

  DISALLOW_COPY_AND_ASSIGN(CatalogManager);
};

}  // namespace master
}  // namespace yb

#endif // YB_MASTER_CATALOG_MANAGER_H
