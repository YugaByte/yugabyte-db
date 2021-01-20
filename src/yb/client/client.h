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
#ifndef YB_CLIENT_CLIENT_H_
#define YB_CLIENT_CLIENT_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <mutex>

#include <boost/function.hpp>
#include <boost/functional/hash/hash.hpp>

#include "yb/client/client_fwd.h"
#include "yb/client/schema.h"
#include "yb/common/common.pb.h"
#include "yb/common/transaction.h"
#include "yb/common/wire_protocol.h"

#ifdef YB_HEADERS_NO_STUBS
#include <gtest/gtest_prod.h>
#include "yb/common/clock.h"
#include "yb/common/entity_ids.h"
#include "yb/common/index.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/port.h"
#else
#include "yb/client/stubs.h"
#endif
#include "yb/client/permissions.h"
#include "yb/client/yb_table_name.h"
#include "yb/client/namespace_alterer.h"

#include "yb/common/partition.h"
#include "yb/common/roles_permissions.h"

#include "yb/master/master.pb.h"

#include "yb/rpc/rpc_fwd.h"

#include "yb/util/enums.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_fwd.h"
#include "yb/util/result.h"
#include "yb/util/status.h"
#include "yb/util/status_callback.h"
#include "yb/util/strongly_typed_bool.h"
#include "yb/util/threadpool.h"

template<class T> class scoped_refptr;

YB_DEFINE_ENUM(GrantRevokeStatementType, (GRANT)(REVOKE));
YB_STRONGLY_TYPED_BOOL(RequireTabletsRunning);

namespace yb {

class CloudInfoPB;
class MetricEntity;

namespace master {
class ReplicationInfoPB;
class TabletLocationsPB;
}

namespace tserver {
class LocalTabletServer;
class TabletServerServiceProxy;
}

namespace client {
namespace internal {
template <class Req, class Resp>
class ClientMasterRpc;
}

// This needs to be called by a client app before performing any operations that could result in
// logging.
void InitLogging();

//
// Installs a callback for internal client logging. It is invoked for a
// log event of any severity, across any YBClient instance.
//
// Only the first invocation has any effect; subsequent invocations are
// a no-op. The caller must ensure that 'cb' stays alive until
// UninstallLoggingCallback() is called.
//
// Before a callback is registered, all internal client log events are
// logged to stderr.
void InstallLoggingCallback(YBLoggingCallback* cb);

// Removes a callback installed via InstallLoggingCallback().
//
// Only the first invocation has any effect; subsequent invocations are
// a no-op.
//
// Should be called before unloading the client library.
void UninstallLoggingCallback();

// Set the logging verbosity of the client library. By default, this is 0. Logs become
// progressively more verbose as the level is increased. Empirically, the highest
// verbosity level used in YB is 6, which includes very fine-grained tracing
// information. Most useful logging is enabled at level 1 or 2, with the higher levels
// used only in rare circumstances.
//
// Logs are emitted to stderr, or to the configured log callback at SEVERITY_INFO.
//
// This may be called safely at any point during usage of the library.
void SetVerboseLogLevel(int level);

// The YB client library uses signals internally in some cases. By default, it uses
// SIGUSR2. If your application makes use of SIGUSR2, this advanced API can help
// workaround conflicts.
Status SetInternalSignalNumber(int signum);

using MasterAddressSource = std::function<std::vector<std::string>()>;

// Creates a new YBClient with the desired options.
//
// Note that YBClients are shared amongst multiple threads and, as such,
// are stored in shared pointers.
class YBClientBuilder {
 public:
  YBClientBuilder();
  ~YBClientBuilder();

  YBClientBuilder& clear_master_server_addrs();

  // Add RPC addresses of multiple masters.
  YBClientBuilder& master_server_addrs(const std::vector<std::string>& addrs);

  // Add a REST endpoint from which the address of the masters can be queried initially, and
  // refreshed in case of retries. Note that the endpoint mechanism overrides
  // both 'add_master_server_addr_file' and 'add_master_server_addr'.
  YBClientBuilder& add_master_server_endpoint(const std::string& endpoint);

  // Add an RPC address of a master. At least one master is required.
  YBClientBuilder& add_master_server_addr(const std::string& addr);

  // Don't override master addresses with external information from FLAGS_flagfile.
  YBClientBuilder& skip_master_flagfile(bool should_skip = true);

  // The default timeout used for administrative operations (e.g. CreateTable,
  // AlterTable, ...). Optional.
  //
  // If not provided, defaults to 10s.
  YBClientBuilder& default_admin_operation_timeout(const MonoDelta& timeout);

  // The default timeout for individual RPCs. Optional.
  //
  // If not provided, defaults to 5s.
  YBClientBuilder& default_rpc_timeout(const MonoDelta& timeout);

  // Set the number of reactor threads that are used to send out the requests.
  // (defaults to the flag value yb_client_num_reactors : 16).
  YBClientBuilder& set_num_reactors(int32_t num_reactors);

  // Sets the cloud info for the client, indicating where the client is located.
  YBClientBuilder& set_cloud_info_pb(const CloudInfoPB& cloud_info_pb);

  // Sets metric entity to be used for emitting metrics. Optional.
  YBClientBuilder& set_metric_entity(const scoped_refptr<MetricEntity>& metric_entity);

  // Sets client name to be used for naming the client's messenger/reactors.
  YBClientBuilder& set_client_name(const std::string& name);

  // Sets the size of the threadpool for calling callbacks.
  YBClientBuilder& set_callback_threadpool_size(size_t size);

  YBClientBuilder& wait_for_leader_election_on_init(bool should_wait = true);

  // Sets skip master leader resolution.
  // Used in tests, when we do not have real master.
  YBClientBuilder& set_skip_master_leader_resolution(bool value);

  // Sets the tserver uuid for the client used by the CQL proxy. Intended only for use by CQL
  // proxy clients.
  YBClientBuilder& set_tserver_uuid(const TabletServerId& uuid);

  YBClientBuilder& set_parent_mem_tracker(const std::shared_ptr<MemTracker>& mem_tracker);

  YBClientBuilder& set_master_address_flag_name(const std::string& value);

  YBClientBuilder& AddMasterAddressSource(const MasterAddressSource& source);

  // Creates the client.
  // Will use specified messenger if not nullptr.
  // If messenger is nullptr - messenger will be created and owned by client. Client will shutdown
  // messenger on client shutdown.
  //
  // The return value may indicate an error in the create operation, or a
  // misuse of the builder; in the latter case, only the last error is
  // returned.
  Result<std::unique_ptr<YBClient>> Build(rpc::Messenger* messenger = nullptr);

  // Creates the client which gets the messenger ownership and shuts it down on client shutdown.
  Result<std::unique_ptr<YBClient>> Build(std::unique_ptr<rpc::Messenger>&& messenger);

 private:
  class Data;

  CHECKED_STATUS DoBuild(rpc::Messenger* messenger, std::unique_ptr<client::YBClient>* client);

  std::unique_ptr<Data> data_;

  DISALLOW_COPY_AND_ASSIGN(YBClientBuilder);
};

// The YBClient represents a connection to a cluster. From the user
// perspective, they should only need to create one of these in their
// application, likely a singleton -- but it's not a singleton in YB in any
// way. Different Client objects do not interact with each other -- no
// connection pooling, etc. Each YBClient instance is sandboxed with no
// global cross-client state.
//
// In the implementation, the client holds various pieces of common
// infrastructure which is not table-specific:
//
// - RPC messenger: reactor threads and RPC connections are pooled here
// - Authentication: the client is initialized with some credentials, and
//   all accesses through it share those credentials.
// - Caches: caches of table schemas, tablet locations, tablet server IP
//   addresses, etc are shared per-client.
//
// In order to actually access data on the cluster, callers must first
// create a YBSession object using NewSession(). A YBClient may
// have several associated sessions.
//
// TODO: Cluster administration functions are likely to be in this class
// as well.
//
// This class is thread-safe.
class YBClient {
 public:
  ~YBClient();

  std::unique_ptr<YBTableCreator> NewTableCreator();

  // set 'create_in_progress' to true if a CreateTable operation is in-progress.
  CHECKED_STATUS IsCreateTableInProgress(const YBTableName& table_name,
                                         bool *create_in_progress);

  // Wait for create table to finish.
  CHECKED_STATUS WaitForCreateTableToFinish(const YBTableName& table_name);
  CHECKED_STATUS WaitForCreateTableToFinish(const YBTableName& table_name,
                                            const CoarseTimePoint& deadline);

  CHECKED_STATUS WaitForCreateTableToFinish(const string& table_id);
  CHECKED_STATUS WaitForCreateTableToFinish(const string& table_id,
                                            const CoarseTimePoint& deadline);

  // Truncate the specified table.
  // Set 'wait' to true if the call must wait for the table to be fully truncated before returning.
  CHECKED_STATUS TruncateTable(const std::string& table_id, bool wait = true);
  CHECKED_STATUS TruncateTables(const std::vector<std::string>& table_ids, bool wait = true);

  // Backfill the specified index table.  This is only supported for YSQL at the moment.
  CHECKED_STATUS BackfillIndex(const TableId& table_id, bool wait = true);

  // Delete the specified table.
  // Set 'wait' to true if the call must wait for the table to be fully deleted before returning.
  CHECKED_STATUS DeleteTable(const YBTableName& table_name, bool wait = true);
  CHECKED_STATUS DeleteTable(const std::string& table_id, bool wait = true);

  // Delete the specified index table.
  // Set 'wait' to true if the call must wait for the table to be fully deleted before returning.
  CHECKED_STATUS DeleteIndexTable(const YBTableName& table_name,
                                  YBTableName* indexed_table_name = nullptr,
                                  bool wait = true);

  CHECKED_STATUS DeleteIndexTable(const std::string& table_id,
                                  YBTableName* indexed_table_name = nullptr,
                                  bool wait = true);

  // Flush or compact the specified tables.
  CHECKED_STATUS FlushTables(const std::vector<TableId>& table_ids,
                             bool add_indexes,
                             int timeout_secs,
                             bool is_compaction);
  CHECKED_STATUS FlushTables(const std::vector<YBTableName>& table_names,
                             bool add_indexes,
                             int timeout_secs,
                             bool is_compaction);

  std::unique_ptr<YBTableAlterer> NewTableAlterer(const YBTableName& table_name);
  std::unique_ptr<YBTableAlterer> NewTableAlterer(const string id);

  // Set 'alter_in_progress' to true if an AlterTable operation is in-progress.
  CHECKED_STATUS IsAlterTableInProgress(const YBTableName& table_name,
                                        const string& table_id,
                                        bool *alter_in_progress);

  CHECKED_STATUS GetTableSchema(const YBTableName& table_name,
                                YBSchema* schema,
                                PartitionSchema* partition_schema);
  Result<YBTableInfo> GetYBTableInfo(const YBTableName& table_name);

  CHECKED_STATUS GetTableSchemaById(const TableId& table_id, std::shared_ptr<YBTableInfo> info,
                                    StatusCallback callback);

  CHECKED_STATUS GetColocatedTabletSchemaById(const TableId& parent_colocated_table_id,
                                              std::shared_ptr<std::vector<YBTableInfo>> info,
                                              StatusCallback callback);

  Result<IndexPermissions> GetIndexPermissions(
      const TableId& table_id,
      const TableId& index_id);
  Result<IndexPermissions> GetIndexPermissions(
      const YBTableName& table_name,
      const YBTableName& index_name);
  Result<IndexPermissions> WaitUntilIndexPermissionsAtLeast(
      const TableId& table_id,
      const TableId& index_id,
      const IndexPermissions& target_index_permissions,
      const CoarseTimePoint deadline,
      const CoarseDuration max_wait = std::chrono::seconds(2));
  Result<IndexPermissions> WaitUntilIndexPermissionsAtLeast(
      const TableId& table_id,
      const TableId& index_id,
      const IndexPermissions& target_index_permissions,
      const CoarseDuration max_wait = std::chrono::seconds(2));
  Result<IndexPermissions> WaitUntilIndexPermissionsAtLeast(
      const YBTableName& table_name,
      const YBTableName& index_name,
      const IndexPermissions& target_index_permissions,
      const CoarseDuration max_wait = std::chrono::seconds(2));
  Result<IndexPermissions> WaitUntilIndexPermissionsAtLeast(
      const YBTableName& table_name,
      const YBTableName& index_name,
      const IndexPermissions& target_index_permissions,
      const CoarseTimePoint deadline,
      const CoarseDuration max_wait = std::chrono::seconds(2));

  // Trigger an async index permissions update after new YSQL index permissions are committed.
  Status AsyncUpdateIndexPermissions(const TableId& indexed_table_id);

  // Namespace related methods.

  // Create a new namespace with the given name.
  // TODO(neil) When database_type is undefined, backend will not check error on database type.
  // Except for testing we should use proper database_types for all creations.
  CHECKED_STATUS CreateNamespace(const std::string& namespace_name,
                                 const boost::optional<YQLDatabase>& database_type = boost::none,
                                 const std::string& creator_role_name = "",
                                 const std::string& namespace_id = "",
                                 const std::string& source_namespace_id = "",
                                 const boost::optional<uint32_t>& next_pg_oid = boost::none,
                                 const boost::optional<TransactionMetadata>& txn = boost::none,
                                 const bool colocated = false);

  // It calls CreateNamespace(), but before it checks that the namespace has NOT been yet
  // created. So, it prevents error 'namespace already exists'.
  // TODO(neil) When database_type is undefined, backend will not check error on database type.
  // Except for testing we should use proper database_types for all creations.
  CHECKED_STATUS CreateNamespaceIfNotExists(const std::string& namespace_name,
                                            const boost::optional<YQLDatabase>& database_type =
                                            boost::none,
                                            const std::string& creator_role_name = "",
                                            const std::string& namespace_id = "",
                                            const std::string& source_namespace_id = "",
                                            const boost::optional<uint32_t>& next_pg_oid =
                                            boost::none,
                                            const bool colocated = false);

  // Set 'create_in_progress' to true if a CreateNamespace operation is in-progress.
  CHECKED_STATUS IsCreateNamespaceInProgress(const std::string& namespace_name,
                                             const boost::optional<YQLDatabase>& database_type,
                                             const std::string& namespace_id,
                                             bool *create_in_progress);

  // Delete namespace with the given name.
  CHECKED_STATUS DeleteNamespace(const std::string& namespace_name,
                                 const boost::optional<YQLDatabase>& database_type = boost::none,
                                 const std::string& namespace_id = "");

  // Set 'delete_in_progress' to true if a DeleteNamespace operation is in-progress.
  CHECKED_STATUS IsDeleteNamespaceInProgress(const std::string& namespace_name,
                                             const boost::optional<YQLDatabase>& database_type,
                                             const std::string& namespace_id,
                                             bool *delete_in_progress);

  YBNamespaceAlterer* NewNamespaceAlterer(const string& namespace_name,
                                          const std::string& namespace_id);

  // For Postgres: reserve oids for a Postgres database.
  CHECKED_STATUS ReservePgsqlOids(const std::string& namespace_id,
                                  uint32_t next_oid, uint32_t count,
                                  uint32_t* begin_oid, uint32_t* end_oid);

  CHECKED_STATUS GetYsqlCatalogMasterVersion(uint64_t *ysql_catalog_version);

  // Grant permission with given arguments.
  CHECKED_STATUS GrantRevokePermission(GrantRevokeStatementType statement_type,
                                       const PermissionType& permission,
                                       const ResourceType& resource_type,
                                       const std::string& canonical_resource,
                                       const char* resource_name,
                                       const char* namespace_name,
                                       const std::string& role_name);

  // List all namespace identifiers.
  Result<vector<master::NamespaceIdentifierPB>> ListNamespaces() {
    return ListNamespaces(boost::none);
  }

  Result<vector<master::NamespaceIdentifierPB>> ListNamespaces(
      const boost::optional<YQLDatabase>& database_type);

  // Get namespace information.
  CHECKED_STATUS GetNamespaceInfo(const std::string& namespace_id,
                                  const std::string& namespace_name,
                                  const boost::optional<YQLDatabase>& database_type,
                                  master::GetNamespaceInfoResponsePB* ret);

  // Check if the namespace given by 'namespace_name' or 'namespace_id' exists.
  // Result value is set only on success.
  Result<bool> NamespaceExists(const std::string& namespace_name,
                               const boost::optional<YQLDatabase>& database_type = boost::none);
  Result<bool> NamespaceIdExists(const std::string& namespace_id,
                                 const boost::optional<YQLDatabase>& database_type = boost::none);

  // Create a new tablegroup.
  CHECKED_STATUS CreateTablegroup(const std::string& namespace_name,
                                  const std::string& namespace_id,
                                  const std::string& tablegroup_id);

  // Delete a tablegroup.
  CHECKED_STATUS DeleteTablegroup(const std::string& namespace_id,
                                  const std::string& tablegroup_id);

  // Check if the tablegroup given by 'tablegroup_id' exists.
  // Result value is set only on success.
  Result<bool> TablegroupExists(const std::string& namespace_name,
                                const std::string& tablegroup_id);
  Result<vector<master::TablegroupIdentifierPB>> ListTablegroups(const std::string& namespace_name);

  // Authentication and Authorization
  // Create a new role.
  CHECKED_STATUS CreateRole(const RoleName& role_name,
                            const std::string& salted_hash,
                            const bool login, const bool superuser,
                            const RoleName& creator_role_name);

  // Alter an existing role.
  CHECKED_STATUS AlterRole(const RoleName& role_name,
                           const boost::optional<std::string>& salted_hash,
                           const boost::optional<bool> login,
                           const boost::optional<bool> superuser,
                           const RoleName& current_role_name);

  // Delete a role.
  CHECKED_STATUS DeleteRole(const std::string& role_name, const std::string& current_role_name);

  CHECKED_STATUS SetRedisPasswords(const vector<string>& passwords);
  // Fetches the password from the local cache, or from the master if the local cached value
  // is too old.
  CHECKED_STATUS GetRedisPasswords(vector<string>* passwords);

  CHECKED_STATUS SetRedisConfig(const string& key, const vector<string>& values);
  CHECKED_STATUS GetRedisConfig(const string& key, vector<string>* values);

  // Grants a role to another role, or revokes a role from another role.
  CHECKED_STATUS GrantRevokeRole(GrantRevokeStatementType statement_type,
                                 const std::string& granted_role_name,
                                 const std::string& recipient_role_name);

  // Get all the roles' permissions from the master only if the master's permissions version is
  // greater than permissions_cache->version().s
  CHECKED_STATUS GetPermissions(client::internal::PermissionsCache* permissions_cache);

  // (User-defined) type related methods.

  // Create a new (user-defined) type.
  CHECKED_STATUS CreateUDType(const std::string &namespace_name,
                              const std::string &type_name,
                              const std::vector<std::string> &field_names,
                              const std::vector<std::shared_ptr<QLType>> &field_types);

  // Delete a (user-defined) type by name.
  CHECKED_STATUS DeleteUDType(const std::string &namespace_name, const std::string &type_name);

  // Retrieve a (user-defined) type by name.
  CHECKED_STATUS GetUDType(const std::string &namespace_name,
                           const std::string &type_name,
                           std::shared_ptr<QLType> *ql_type);

  // CDC Stream related methods.

  // Create a new CDC stream.
  Result<CDCStreamId> CreateCDCStream(const TableId& table_id,
                                      const std::unordered_map<std::string, std::string>& options);

  void CreateCDCStream(const TableId& table_id,
                       const std::unordered_map<std::string, std::string>& options,
                       CreateCDCStreamCallback callback);

  // Delete multiple CDC streams.
  CHECKED_STATUS DeleteCDCStream(const vector<CDCStreamId>& streams);

  // Delete a CDC stream.
  CHECKED_STATUS DeleteCDCStream(const CDCStreamId& stream_id);

  void DeleteCDCStream(const CDCStreamId& stream_id, StatusCallback callback);

  // Retrieve a CDC stream.
  CHECKED_STATUS GetCDCStream(const CDCStreamId &stream_id,
                              TableId* table_id,
                              std::unordered_map<std::string, std::string>* options);

  void GetCDCStream(const CDCStreamId& stream_id,
                    std::shared_ptr<TableId> table_id,
                    std::shared_ptr<std::unordered_map<std::string, std::string>> options,
                    StdStatusCallback callback);

  void DeleteTablet(const TabletId& tablet_id, StdStatusCallback callback);

  // Find the number of tservers. This function should not be called frequently for reading or
  // writing actual data. Currently, it is called only for SQL DDL statements.
  // If primary_only is set to true, we expect the primary/sync cluster tserver count only.
  // If use_cache is set to true, we return old value.
  CHECKED_STATUS TabletServerCount(int *tserver_count, bool primary_only = false,
      bool use_cache = false);

  CHECKED_STATUS ListTabletServers(std::vector<std::unique_ptr<YBTabletServer>>* tablet_servers);

  // Sets local tserver and its proxy.
  void SetLocalTabletServer(const std::string& ts_uuid,
                            const std::shared_ptr<tserver::TabletServerServiceProxy>& proxy,
                            const tserver::LocalTabletServer* local_tserver);

  // List only those tables whose names pass a substring match on 'filter'.
  //
  // 'tables' is appended to only on success.
  Result<std::vector<YBTableName>> ListTables(
      const std::string& filter = "",
      bool exclude_ysql = false);

  // List all running tablets' uuids for this table.
  // 'tablets' is appended to only on success.
  CHECKED_STATUS GetTablets(
      const YBTableName& table_name,
      const int32_t max_tablets,
      std::vector<TabletId>* tablet_uuids,
      std::vector<std::string>* ranges,
      std::vector<master::TabletLocationsPB>* locations = nullptr,
      RequireTabletsRunning require_tablets_running = RequireTabletsRunning::kFalse);

  CHECKED_STATUS GetTabletsAndUpdateCache(
      const YBTableName& table_name,
      const int32_t max_tablets,
      std::vector<TabletId>* tablet_uuids,
      std::vector<std::string>* ranges,
      std::vector<master::TabletLocationsPB>* locations);

  Status GetTabletsFromTableId(
      const std::string& table_id, const int32_t max_tablets,
      google::protobuf::RepeatedPtrField<master::TabletLocationsPB>* tablets);

  CHECKED_STATUS GetTablets(
      const YBTableName& table_name,
      const int32_t max_tablets,
      google::protobuf::RepeatedPtrField<master::TabletLocationsPB>* tablets,
      RequireTabletsRunning require_tablets_running = RequireTabletsRunning::kFalse);

  CHECKED_STATUS GetTabletLocation(const TabletId& tablet_id,
                                   master::TabletLocationsPB* tablet_location);

  // Get the list of master uuids. Can be enhanced later to also return port/host info.
  CHECKED_STATUS ListMasters(
    CoarseTimePoint deadline,
    std::vector<std::string>* master_uuids);

  // Check if the table given by 'table_name' exists.
  // Result value is set only on success.
  Result<bool> TableExists(const YBTableName& table_name);

  Result<bool> IsLoadBalanced(uint32_t num_servers);
  Result<bool> IsLoadBalancerIdle();

  // Open the table with the given name or id. This will do an RPC to ensure that
  // the table exists and look up its schema.
  //
  // TODO: should we offer an async version of this as well?
  // TODO: probably should have a configurable timeout in YBClientBuilder?
  CHECKED_STATUS OpenTable(const YBTableName& table_name, std::shared_ptr<YBTable>* table);
  CHECKED_STATUS OpenTable(const TableId& table_id, std::shared_ptr<YBTable>* table);

  Result<YBTablePtr> OpenTable(const TableId& table_id) {
    YBTablePtr result;
    RETURN_NOT_OK(OpenTable(table_id, &result));
    return result;
  }

  Result<YBTablePtr> OpenTable(const YBTableName& name) {
    YBTablePtr result;
    RETURN_NOT_OK(OpenTable(name, &result));
    return result;
  }

  // Create a new session for interacting with the cluster.
  // User is responsible for destroying the session object.
  // This is a fully local operation (no RPCs or blocking).
  std::shared_ptr<YBSession> NewSession();

  // Return the socket address of the master leader for this client.
  HostPort GetMasterLeaderAddress();

  // Caller knows that the existing leader might have died or stepped down, so it can use this API
  // to reset the client state to point to new master leader.
  Result<HostPort> RefreshMasterLeaderAddress();

  // Once a config change is completed to add/remove a master, update the client to add/remove it
  // from its own master address list.
  CHECKED_STATUS AddMasterToClient(const HostPort& add);
  CHECKED_STATUS RemoveMasterFromClient(const HostPort& remove);
  CHECKED_STATUS SetMasterAddresses(const std::string& addrs);

  // Policy with which to choose amongst multiple replicas.
  enum ReplicaSelection {
    // Select the LEADER replica.
    LEADER_ONLY,

    // Select the closest replica to the client, or a random one if all
    // replicas are equidistant.
    CLOSEST_REPLICA,

    // Select the first replica in the list.
    FIRST_REPLICA
  };

  bool IsMultiMaster() const;

  // Get the number of tablets to be created for a new user table.
  // This will be based on --num_shards_per_tserver or --ysql_num_shards_per_tserver
  // and number of tservers.
  Result<int> NumTabletsForUserTable(TableType table_type);

  void TEST_set_admin_operation_timeout(const MonoDelta& timeout);

  const MonoDelta& default_admin_operation_timeout() const;
  const MonoDelta& default_rpc_timeout() const;

  // Value for the latest observed hybrid_time when none has been observed or set.
  static const uint64_t kNoHybridTime;

  // Returns highest hybrid_time observed by the client.
  // The latest observed hybrid_time can be used to start a snapshot scan on a
  // table which is guaranteed to contain all data written or previously read by
  // this client.
  uint64_t GetLatestObservedHybridTime() const;

  // Sets the latest observed hybrid_time, encoded in the HybridTime format.
  // This is only useful when forwarding hybrid_times between clients to enforce
  // external consistency when using YBSession::CLIENT_PROPAGATED external consistency
  // mode.
  // To use this the user must obtain the HybridTime encoded hybrid_time from the first
  // client with YBClient::GetLatestObservedHybridTime() and the set it in the new
  // client with this method.
  void SetLatestObservedHybridTime(uint64_t ht_hybrid_time);

  // Given a host and port for a master, get the uuid of that process.
  CHECKED_STATUS GetMasterUUID(const std::string& host, int16_t port, std::string* uuid);

  CHECKED_STATUS SetReplicationInfo(const master::ReplicationInfoPB& replication_info);

  void LookupTabletByKey(const std::shared_ptr<const YBTable>& table,
                         const std::string& partition_key,
                         CoarseTimePoint deadline,
                         LookupTabletCallback callback);

  void LookupTabletById(const std::string& tablet_id,
                        const std::shared_ptr<const YBTable>& table,
                        CoarseTimePoint deadline,
                        LookupTabletCallback callback,
                        UseCache use_cache);

  void LookupAllTablets(const std::shared_ptr<const YBTable>& table,
                        CoarseTimePoint deadline,
                        LookupTabletRangeCallback callback);

  std::future<Result<std::vector<internal::RemoteTabletPtr>>> LookupAllTabletsFuture(
      const std::shared_ptr<const YBTable>& table,
      CoarseTimePoint deadline);

  rpc::Messenger* messenger() const;

  const scoped_refptr<MetricEntity>& metric_entity() const;

  rpc::ProxyCache& proxy_cache() const;

  const std::string& proxy_uuid() const;

  // Id of this client instance.
  const ClientId& id() const;

  const CloudInfoPB& cloud_info() const;

  std::pair<RetryableRequestId, RetryableRequestId> NextRequestIdAndMinRunningRequestId(
      const TabletId& tablet_id);
  void RequestFinished(const TabletId& tablet_id, RetryableRequestId request_id);

  void MaybeUpdateMinRunningRequestId(
      const TabletId& tablet_id, RetryableRequestId min_running_request_id);

  void Shutdown();

 private:
  class Data;

  friend class YBClientBuilder;
  friend class YBNoOp;
  friend class YBTable;
  friend class YBTableAlterer;
  friend class YBNamespaceAlterer;
  friend class YBTableCreator;
  friend class internal::Batcher;
  friend class internal::GetTableSchemaRpc;
  friend class internal::GetColocatedTabletSchemaRpc;
  friend class internal::LookupRpc;
  friend class internal::MetaCache;
  friend class internal::RemoteTablet;
  friend class internal::RemoteTabletServer;
  friend class internal::AsyncRpc;
  friend class internal::TabletInvoker;
  template <class Req, class Resp>
  friend class internal::ClientMasterRpc;
  friend class PlacementInfoTest;

  FRIEND_TEST(ClientTest, TestGetTabletServerBlacklist);
  FRIEND_TEST(ClientTest, TestMasterDown);
  FRIEND_TEST(ClientTest, TestMasterLookupPermits);
  FRIEND_TEST(ClientTest, TestReplicatedTabletWritesAndAltersWithLeaderElection);
  FRIEND_TEST(ClientTest, TestScanFaultTolerance);
  FRIEND_TEST(ClientTest, TestScanTimeout);
  FRIEND_TEST(ClientTest, TestWriteWithDeadMaster);
  FRIEND_TEST(MasterFailoverTest, DISABLED_TestPauseAfterCreateTableIssued);
  FRIEND_TEST(MasterFailoverTestIndexCreation, TestPauseAfterCreateIndexIssued);

  friend std::future<Result<internal::RemoteTabletPtr>> LookupFirstTabletFuture(
      const std::shared_ptr<const YBTable>& table);

  YBClient();

  ThreadPool* callback_threadpool();

  std::unique_ptr<Data> data_;

  DISALLOW_COPY_AND_ASSIGN(YBClient);
};

}  // namespace client
}  // namespace yb
#endif  // YB_CLIENT_CLIENT_H_
