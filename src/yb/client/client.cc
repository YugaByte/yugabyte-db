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

#include "yb/client/client.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <limits>

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/client/client_utils.h"
#include "yb/client/meta_cache.h"
#include "yb/client/session.h"
#include "yb/client/table_alterer.h"
#include "yb/client/namespace_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/tablet_server.h"
#include "yb/client/yb_table_name.h"

#include "yb/common/common.pb.h"
#include "yb/common/entity_ids.h"
#include "yb/common/common_flags.h"
#include "yb/common/partition.h"
#include "yb/common/roles_permissions.h"
#include "yb/common/wire_protocol.h"

#include "yb/master/master.proxy.h"
#include "yb/master/master_defaults.h"
#include "yb/master/master_error.h"
#include "yb/master/master_util.h"
#include "yb/util/monotime.h"
#include "yb/yql/redis/redisserver/redis_constants.h"
#include "yb/yql/redis/redisserver/redis_parser.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/yb_rpc.h"
#include "yb/util/flag_tags.h"
#include "yb/util/init.h"
#include "yb/util/logging.h"
#include "yb/util/net/dns_resolver.h"
#include "yb/util/oid_generator.h"
#include "yb/util/scope_exit.h"
#include "yb/util/tsan_util.h"
#include "yb/util/crypt.h"

using yb::master::AlterTableRequestPB;
using yb::master::AlterTableRequestPB_Step;
using yb::master::AlterTableResponsePB;
using yb::master::CreateTableRequestPB;
using yb::master::CreateTableResponsePB;
using yb::master::DeleteTableRequestPB;
using yb::master::DeleteTableResponsePB;
using yb::master::CreateTablegroupRequestPB;
using yb::master::CreateTablegroupResponsePB;
using yb::master::DeleteTablegroupRequestPB;
using yb::master::DeleteTablegroupResponsePB;
using yb::master::ListTablegroupsRequestPB;
using yb::master::ListTablegroupsResponsePB;
using yb::master::GetNamespaceInfoRequestPB;
using yb::master::GetNamespaceInfoResponsePB;
using yb::master::GetTableSchemaRequestPB;
using yb::master::GetTableSchemaResponsePB;
using yb::master::GetColocatedTabletSchemaRequestPB;
using yb::master::GetColocatedTabletSchemaResponsePB;
using yb::master::GetTableLocationsRequestPB;
using yb::master::GetTableLocationsResponsePB;
using yb::master::GetTabletLocationsRequestPB;
using yb::master::GetTabletLocationsResponsePB;
using yb::master::IsLoadBalancedRequestPB;
using yb::master::IsLoadBalancedResponsePB;
using yb::master::IsLoadBalancerIdleRequestPB;
using yb::master::IsLoadBalancerIdleResponsePB;
using yb::master::ListMastersRequestPB;
using yb::master::ListMastersResponsePB;
using yb::master::ListTablesRequestPB;
using yb::master::ListTablesResponsePB;
using yb::master::ListTablesResponsePB_TableInfo;
using yb::master::ListTabletServersRequestPB;
using yb::master::ListTabletServersResponsePB;
using yb::master::ListTabletServersResponsePB_Entry;
using yb::master::CreateNamespaceRequestPB;
using yb::master::CreateNamespaceResponsePB;
using yb::master::AlterNamespaceRequestPB;
using yb::master::AlterNamespaceResponsePB;
using yb::master::DeleteNamespaceRequestPB;
using yb::master::DeleteNamespaceResponsePB;
using yb::master::ListNamespacesRequestPB;
using yb::master::ListNamespacesResponsePB;
using yb::master::ReservePgsqlOidsRequestPB;
using yb::master::ReservePgsqlOidsResponsePB;
using yb::master::GetYsqlCatalogConfigRequestPB;
using yb::master::GetYsqlCatalogConfigResponsePB;
using yb::master::CreateUDTypeRequestPB;
using yb::master::CreateUDTypeResponsePB;
using yb::master::AlterRoleRequestPB;
using yb::master::AlterRoleResponsePB;
using yb::master::CreateRoleRequestPB;
using yb::master::CreateRoleResponsePB;
using yb::master::DeleteUDTypeRequestPB;
using yb::master::DeleteUDTypeResponsePB;
using yb::master::DeleteRoleRequestPB;
using yb::master::DeleteRoleResponsePB;
using yb::master::DeleteTabletRequestPB;
using yb::master::DeleteTabletResponsePB;
using yb::master::GetPermissionsRequestPB;
using yb::master::GetPermissionsResponsePB;
using yb::master::GrantRevokeRoleRequestPB;
using yb::master::GrantRevokeRoleResponsePB;
using yb::master::ListUDTypesRequestPB;
using yb::master::ListUDTypesResponsePB;
using yb::master::GetUDTypeInfoRequestPB;
using yb::master::GetUDTypeInfoResponsePB;
using yb::master::GrantRevokePermissionResponsePB;
using yb::master::GrantRevokePermissionRequestPB;
using yb::master::MasterServiceProxy;
using yb::master::ReplicationInfoPB;
using yb::master::TabletLocationsPB;
using yb::master::RedisConfigSetRequestPB;
using yb::master::RedisConfigSetResponsePB;
using yb::master::RedisConfigGetRequestPB;
using yb::master::RedisConfigGetResponsePB;
using yb::master::CreateCDCStreamRequestPB;
using yb::master::CreateCDCStreamResponsePB;
using yb::master::DeleteCDCStreamRequestPB;
using yb::master::DeleteCDCStreamResponsePB;
using yb::master::GetCDCStreamRequestPB;
using yb::master::GetCDCStreamResponsePB;
using yb::master::ListCDCStreamsRequestPB;
using yb::master::ListCDCStreamsResponsePB;
using yb::rpc::Messenger;
using yb::rpc::MessengerBuilder;
using yb::rpc::RpcController;
using yb::tserver::NoOpRequestPB;
using yb::tserver::NoOpResponsePB;
using yb::util::kBcryptHashSize;
using std::set;
using std::string;
using std::vector;
using google::protobuf::RepeatedPtrField;

using namespace yb::size_literals;  // NOLINT.

DEFINE_bool(client_suppress_created_logs, false,
            "Suppress 'Created table ...' messages");
TAG_FLAG(client_suppress_created_logs, advanced);
TAG_FLAG(client_suppress_created_logs, hidden);

DEFINE_int32(backfill_index_client_rpc_timeout_ms, 60 * 60 * 1000, // 60 min.
             "Timeout for BackfillIndex RPCs from client to master.");
TAG_FLAG(backfill_index_client_rpc_timeout_ms, advanced);

DEFINE_test_flag(int32, yb_num_total_tablets, 0,
                 "The total number of tablets per table when a table is created.");

namespace yb {
namespace client {

using internal::MetaCache;
using ql::ObjectType;
using std::shared_ptr;

#define CALL_SYNC_LEADER_MASTER_RPC(req, resp, method) \
  do { \
    auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout(); \
    CALL_SYNC_LEADER_MASTER_RPC_WITH_DEADLINE(req, resp, deadline, method); \
  } while(0);

#define CALL_SYNC_LEADER_MASTER_RPC_WITH_DEADLINE(req, resp, deadline, method) \
  do { \
    Status s = data_->SyncLeaderMasterRpc<BOOST_PP_CAT(method, RequestPB), \
                                          BOOST_PP_CAT(method, ResponsePB)>( \
        deadline, \
        req, \
        &resp, \
        nullptr, \
        BOOST_PP_STRINGIZE(method), \
        &MasterServiceProxy::method); \
    RETURN_NOT_OK(s); \
    if (resp.has_error()) { \
      return StatusFromPB(resp.error().status()); \
    } \
  } while(0);

// Adapts between the internal LogSeverity and the client's YBLogSeverity.
static void LoggingAdapterCB(YBLoggingCallback* user_cb,
                             LogSeverity severity,
                             const char* filename,
                             int line_number,
                             const struct ::tm* time,
                             const char* message,
                             size_t message_len) {
  YBLogSeverity client_severity;
  switch (severity) {
    case yb::SEVERITY_INFO:
      client_severity = SEVERITY_INFO;
      break;
    case yb::SEVERITY_WARNING:
      client_severity = SEVERITY_WARNING;
      break;
    case yb::SEVERITY_ERROR:
      client_severity = SEVERITY_ERROR;
      break;
    case yb::SEVERITY_FATAL:
      client_severity = SEVERITY_FATAL;
      break;
    default:
      LOG(FATAL) << "Unknown YB log severity: " << severity;
  }
  user_cb->Run(client_severity, filename, line_number, time,
               message, message_len);
}

void InitLogging() {
  InitGoogleLoggingSafeBasic("yb_client");
}

void InstallLoggingCallback(YBLoggingCallback* cb) {
  RegisterLoggingCallback(Bind(&LoggingAdapterCB, Unretained(cb)));
}

void UninstallLoggingCallback() {
  UnregisterLoggingCallback();
}

void SetVerboseLogLevel(int level) {
  FLAGS_v = level;
}

Status SetInternalSignalNumber(int signum) {
  return SetStackTraceSignal(signum);
}

YBClientBuilder::YBClientBuilder()
  : data_(new YBClientBuilder::Data()) {
}

YBClientBuilder::~YBClientBuilder() {
}

YBClientBuilder& YBClientBuilder::clear_master_server_addrs() {
  data_->master_server_addrs_.clear();
  return *this;
}

YBClientBuilder& YBClientBuilder::master_server_addrs(const vector<string>& addrs) {
  for (const string& addr : addrs) {
    data_->master_server_addrs_.push_back(addr);
  }
  return *this;
}

YBClientBuilder& YBClientBuilder::add_master_server_addr(const string& addr) {
  data_->master_server_addrs_.push_back(addr);
  return *this;
}

YBClientBuilder& YBClientBuilder::add_master_server_endpoint(const string& endpoint) {
  data_->master_server_endpoint_ = endpoint;
  return *this;
}

YBClientBuilder& YBClientBuilder::skip_master_flagfile(bool should_skip) {
  data_->skip_master_flagfile_ = should_skip;
  return *this;
}

YBClientBuilder& YBClientBuilder::wait_for_leader_election_on_init(bool should_wait) {
  data_->wait_for_leader_election_on_init_ = should_wait;
  return *this;
}

YBClientBuilder& YBClientBuilder::default_admin_operation_timeout(const MonoDelta& timeout) {
  data_->default_admin_operation_timeout_ = timeout;
  return *this;
}

YBClientBuilder& YBClientBuilder::default_rpc_timeout(const MonoDelta& timeout) {
  data_->default_rpc_timeout_ = timeout;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_num_reactors(int32_t num_reactors) {
  CHECK_GT(num_reactors, 0);
  data_->num_reactors_ = num_reactors;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_cloud_info_pb(const CloudInfoPB& cloud_info_pb) {
  data_->cloud_info_pb_ = cloud_info_pb;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_metric_entity(
    const scoped_refptr<MetricEntity>& metric_entity) {
  data_->metric_entity_ = metric_entity;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_client_name(const std::string& name) {
  data_->client_name_ = name;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_callback_threadpool_size(size_t size) {
  data_->threadpool_size_ = size;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_tserver_uuid(const TabletServerId& uuid) {
  data_->uuid_ = uuid;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_parent_mem_tracker(const MemTrackerPtr& mem_tracker) {
  data_->parent_mem_tracker_ = mem_tracker;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_master_address_flag_name(const std::string& value) {
  data_->master_address_flag_name_ = value;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_skip_master_leader_resolution(bool value) {
  data_->skip_master_leader_resolution_ = value;
  return *this;
}

YBClientBuilder& YBClientBuilder::AddMasterAddressSource(const MasterAddressSource& source) {
  data_->master_address_sources_.push_back(source);
  return *this;
}

Status YBClientBuilder::DoBuild(rpc::Messenger* messenger, std::unique_ptr<YBClient>* client) {
  RETURN_NOT_OK(CheckCPUFlags());

  std::unique_ptr<YBClient> c(new YBClient());

  // Init messenger.
  if (messenger) {
    c->data_->messenger_holder_ = nullptr;
    c->data_->messenger_ = messenger;
  } else {
    c->data_->messenger_holder_ = VERIFY_RESULT(client::CreateClientMessenger(
        data_->client_name_, data_->num_reactors_,
        data_->metric_entity_, data_->parent_mem_tracker_));
    c->data_->messenger_ = c->data_->messenger_holder_.get();
  }
  c->data_->proxy_cache_ = std::make_unique<rpc::ProxyCache>(c->data_->messenger_);
  c->data_->metric_entity_ = data_->metric_entity_;

  c->data_->master_address_flag_name_ = data_->master_address_flag_name_;
  c->data_->master_server_endpoint_ = data_->master_server_endpoint_;
  c->data_->master_address_sources_ = data_->master_address_sources_;
  c->data_->master_server_addrs_ = data_->master_server_addrs_;
  c->data_->skip_master_flagfile_ = data_->skip_master_flagfile_;
  c->data_->default_admin_operation_timeout_ = data_->default_admin_operation_timeout_;
  c->data_->default_rpc_timeout_ = data_->default_rpc_timeout_;
  c->data_->wait_for_leader_election_on_init_ = data_->wait_for_leader_election_on_init_;

  // Let's allow for plenty of time for discovering the master the first
  // time around.
  auto deadline = CoarseMonoClock::Now() + c->default_admin_operation_timeout();
  for (;;) {
    auto status = c->data_->SetMasterServerProxy(deadline,
            data_->skip_master_leader_resolution_,
            data_->wait_for_leader_election_on_init_);
    if (status.ok()) {
      break;
    }
    if (!status.IsNotFound() || CoarseMonoClock::Now() >= deadline) {
      RETURN_NOT_OK_PREPEND(status, "Could not locate the leader master")
    }
  }

  c->data_->meta_cache_.reset(new MetaCache(c.get()));

  // Init local host names used for locality decisions.
  RETURN_NOT_OK_PREPEND(c->data_->InitLocalHostNames(),
                        "Could not determine local host names");
  c->data_->cloud_info_pb_ = data_->cloud_info_pb_;
  c->data_->uuid_ = data_->uuid_;
  if (data_->threadpool_size_ > 0) {
    ThreadPoolBuilder tpb(data_->client_name_ + "_cb");
    tpb.set_max_threads(data_->threadpool_size_);
    std::unique_ptr<ThreadPool> tp;
    RETURN_NOT_OK_PREPEND(tpb.Build(&tp), "Could not create callback threadpool");
    c->data_->cb_threadpool_ = std::move(tp);
  }

  client->swap(c);
  return Status::OK();
}

Result<std::unique_ptr<YBClient>> YBClientBuilder::Build(rpc::Messenger* messenger) {
  std::unique_ptr<YBClient> client;
  RETURN_NOT_OK(DoBuild(messenger, &client));
  return client;
}

Result<std::unique_ptr<YBClient>> YBClientBuilder::Build(
    std::unique_ptr<rpc::Messenger>&& messenger) {
  std::unique_ptr<YBClient> client;
  auto ok = false;
  auto scope_exit = ScopeExit([&ok, &messenger] {
    if (!ok) {
      messenger->Shutdown();
    }
  });
  RETURN_NOT_OK(DoBuild(messenger.get(), &client));
  ok = true;
  client->data_->messenger_holder_ = std::move(messenger);
  return client;
}

YBClient::YBClient() : data_(new YBClient::Data()) {
  yb::InitCommonFlags();
}

YBClient::~YBClient() {
  Shutdown();
}

void YBClient::Shutdown() {
  data_->StartShutdown();
  if (data_->messenger_holder_) {
    data_->messenger_holder_->Shutdown();
  }
  if (data_->meta_cache_) {
    data_->meta_cache_->Shutdown();
  }
  if (data_->cb_threadpool_) {
    data_->cb_threadpool_->Shutdown();
  }
  data_->CompleteShutdown();
}

std::unique_ptr<YBTableCreator> YBClient::NewTableCreator() {
  return std::unique_ptr<YBTableCreator>(new YBTableCreator(this));
}

Status YBClient::IsCreateTableInProgress(const YBTableName& table_name,
                                         bool *create_in_progress) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->IsCreateTableInProgress(this, table_name, "" /* table_id */, deadline,
                                        create_in_progress);
}

Status YBClient::WaitForCreateTableToFinish(const YBTableName& table_name) {
  const auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return WaitForCreateTableToFinish(table_name, deadline);
}

Status YBClient::WaitForCreateTableToFinish(
    const YBTableName& table_name, const CoarseTimePoint& deadline) {
  return data_->WaitForCreateTableToFinish(this, table_name, "" /* table_id */, deadline);
}

Status YBClient::WaitForCreateTableToFinish(const string& table_id) {
  const auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return WaitForCreateTableToFinish(table_id, deadline);
}

Status YBClient::WaitForCreateTableToFinish(
    const string& table_id, const CoarseTimePoint& deadline) {
  const YBTableName empty_table_name;
  return data_->WaitForCreateTableToFinish(this, empty_table_name, table_id, deadline);
}

Status YBClient::TruncateTable(const string& table_id, bool wait) {
  return TruncateTables({table_id}, wait);
}

Status YBClient::TruncateTables(const vector<string>& table_ids, bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->TruncateTables(this, table_ids, deadline, wait);
}

Status YBClient::BackfillIndex(const TableId& table_id, bool wait) {
  auto deadline = (CoarseMonoClock::Now()
                   + MonoDelta::FromMilliseconds(FLAGS_backfill_index_client_rpc_timeout_ms));
  return data_->BackfillIndex(this, YBTableName(), table_id, deadline, wait);
}

Status YBClient::DeleteTable(const YBTableName& table_name, bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->DeleteTable(this,
                            table_name,
                            "" /* table_id */,
                            false /* is_index_table */,
                            deadline,
                            nullptr /* indexed_table_name */,
                            wait);
}

Status YBClient::DeleteTable(const string& table_id, bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->DeleteTable(this,
                            YBTableName(),
                            table_id,
                            false /* is_index_table */,
                            deadline,
                            nullptr /* indexed_table_name */,
                            wait);
}

Status YBClient::DeleteIndexTable(const YBTableName& table_name,
                                  YBTableName* indexed_table_name,
                                  bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->DeleteTable(this,
                            table_name,
                            "" /* table_id */,
                            true /* is_index_table */,
                            deadline,
                            indexed_table_name,
                            wait);
}

Status YBClient::DeleteIndexTable(const string& table_id,
                                  YBTableName* indexed_table_name,
                                  bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->DeleteTable(this,
                            YBTableName(),
                            table_id,
                            true /* is_index_table */,
                            deadline,
                            indexed_table_name,
                            wait);
}

Status YBClient::FlushTables(const std::vector<TableId>& table_ids,
                             bool add_indexes,
                             int timeout_secs,
                             bool is_compaction) {
  auto deadline = CoarseMonoClock::Now() + MonoDelta::FromSeconds(timeout_secs);
  return data_->FlushTables(this,
                            table_ids,
                            add_indexes,
                            deadline,
                            is_compaction);
}

Status YBClient::FlushTables(const std::vector<YBTableName>& table_names,
                             bool add_indexes,
                             int timeout_secs,
                             bool is_compaction) {
  auto deadline = CoarseMonoClock::Now() + MonoDelta::FromSeconds(timeout_secs);
  return data_->FlushTables(this,
                            table_names,
                            add_indexes,
                            deadline,
                            is_compaction);
}

std::unique_ptr<YBTableAlterer> YBClient::NewTableAlterer(const YBTableName& name) {
  return std::unique_ptr<YBTableAlterer>(new YBTableAlterer(this, name));
}

std::unique_ptr<YBTableAlterer> YBClient::NewTableAlterer(const string id) {
  return std::unique_ptr<YBTableAlterer>(new YBTableAlterer(this, id));
}

Status YBClient::IsAlterTableInProgress(const YBTableName& table_name,
                                        const string& table_id,
                                        bool *alter_in_progress) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->IsAlterTableInProgress(this, table_name, table_id, deadline, alter_in_progress);
}

Result<YBTableInfo> YBClient::GetYBTableInfo(const YBTableName& table_name) {
  YBTableInfo info;
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  RETURN_NOT_OK(data_->GetTableSchema(this, table_name, deadline, &info));
  return info;
}

Status YBClient::GetTableSchema(const YBTableName& table_name,
                                YBSchema* schema,
                                PartitionSchema* partition_schema) {
  Result<YBTableInfo> info = GetYBTableInfo(table_name);
  if (!info.ok()) {
    return info.status();
  }
  // Verify it is not an index table.
  if (info->index_info) {
    return STATUS(NotFound, "The table does not exist");
  }

  *schema = std::move(info->schema);
  *partition_schema = std::move(info->partition_schema);
  return Status::OK();
}

Status YBClient::GetTableSchemaById(const TableId& table_id, std::shared_ptr<YBTableInfo> info,
                                    StatusCallback callback) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->GetTableSchemaById(this, table_id, deadline, info, callback);
}

Status YBClient::GetColocatedTabletSchemaById(const TableId& parent_colocated_table_id,
                                              std::shared_ptr<std::vector<YBTableInfo>> info,
                                              StatusCallback callback) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->GetColocatedTabletSchemaById(this,
                                             parent_colocated_table_id,
                                             deadline,
                                             info,
                                             callback);
}

Result<IndexPermissions> YBClient::GetIndexPermissions(
    const TableId& table_id,
    const TableId& index_id) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->GetIndexPermissions(
      this,
      table_id,
      index_id,
      deadline);
}

Result<IndexPermissions> YBClient::GetIndexPermissions(
    const YBTableName& table_name,
    const YBTableName& index_name) {
  YBTableInfo table_info = VERIFY_RESULT(GetYBTableInfo(table_name));
  YBTableInfo index_info = VERIFY_RESULT(GetYBTableInfo(index_name));
  return GetIndexPermissions(table_info.table_id, index_info.table_id);
}

Result<IndexPermissions> YBClient::WaitUntilIndexPermissionsAtLeast(
    const TableId& table_id,
    const TableId& index_id,
    const IndexPermissions& target_index_permissions,
    const CoarseTimePoint deadline,
    const CoarseDuration max_wait) {
  return data_->WaitUntilIndexPermissionsAtLeast(
      this,
      table_id,
      index_id,
      target_index_permissions,
      deadline,
      max_wait);
}

Result<IndexPermissions> YBClient::WaitUntilIndexPermissionsAtLeast(
    const TableId& table_id,
    const TableId& index_id,
    const IndexPermissions& target_index_permissions,
    const CoarseDuration max_wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return WaitUntilIndexPermissionsAtLeast(
      table_id,
      index_id,
      target_index_permissions,
      deadline,
      max_wait);
}

Result<IndexPermissions> YBClient::WaitUntilIndexPermissionsAtLeast(
    const YBTableName& table_name,
    const YBTableName& index_name,
    const IndexPermissions& target_index_permissions,
    const CoarseTimePoint deadline,
    const CoarseDuration max_wait) {
  return data_->WaitUntilIndexPermissionsAtLeast(
      this,
      table_name,
      index_name,
      target_index_permissions,
      deadline,
      max_wait);
}

Result<IndexPermissions> YBClient::WaitUntilIndexPermissionsAtLeast(
    const YBTableName& table_name,
    const YBTableName& index_name,
    const IndexPermissions& target_index_permissions,
    const CoarseDuration max_wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return WaitUntilIndexPermissionsAtLeast(
      table_name,
      index_name,
      target_index_permissions,
      deadline,
      max_wait);
}

Status YBClient::AsyncUpdateIndexPermissions(const TableId& indexed_table_id) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  AlterTableRequestPB req;
  req.mutable_table()->set_table_id(indexed_table_id);
  req.set_force_send_alter_request(true);
  return data_->AlterTable(this, req, deadline);
}

Status YBClient::CreateNamespace(const std::string& namespace_name,
                                 const boost::optional<YQLDatabase>& database_type,
                                 const std::string& creator_role_name,
                                 const std::string& namespace_id,
                                 const std::string& source_namespace_id,
                                 const boost::optional<uint32_t>& next_pg_oid,
                                 const boost::optional<TransactionMetadata>& txn,
                                 const bool colocated) {
  CreateNamespaceRequestPB req;
  CreateNamespaceResponsePB resp;
  req.set_name(namespace_name);
  if (!creator_role_name.empty()) {
    req.set_creator_role_name(creator_role_name);
  }
  if (database_type) {
    req.set_database_type(*database_type);
  }
  if (!namespace_id.empty()) {
    req.set_namespace_id(namespace_id);
  }
  if (!source_namespace_id.empty()) {
    req.set_source_namespace_id(source_namespace_id);
  }
  if (next_pg_oid) {
    req.set_next_pg_oid(*next_pg_oid);
  }
  if (txn) {
    txn->ToPB(req.mutable_transaction());
  }
  req.set_colocated(colocated);
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  Status s = data_->SyncLeaderMasterRpc<CreateNamespaceRequestPB, CreateNamespaceResponsePB>(
        deadline, req, &resp, nullptr, "CreateNamespace", &MasterServiceProxy::CreateNamespace);
  if (resp.has_error()) {
    s = StatusFromPB(resp.error().status());
  }
  RETURN_NOT_OK(s);
  std::string cur_id = resp.has_id() ? resp.id() : namespace_id;

  // Verify that the namespace we found is running so that, once this request returns,
  // the client can send operations without receiving a "namespace not found" error.
  RETURN_NOT_OK(data_->WaitForCreateNamespaceToFinish(this, namespace_name, database_type, cur_id,
      CoarseMonoClock::Now() + default_admin_operation_timeout()));

  return Status::OK();
}

Status YBClient::CreateNamespaceIfNotExists(const std::string& namespace_name,
                                            const boost::optional<YQLDatabase>& database_type,
                                            const std::string& creator_role_name,
                                            const std::string& namespace_id,
                                            const std::string& source_namespace_id,
                                            const boost::optional<uint32_t>& next_pg_oid,
                                            const bool colocated) {
  Result<bool> namespace_exists = (!namespace_id.empty() ? NamespaceIdExists(namespace_id)
                                                         : NamespaceExists(namespace_name));
  if (VERIFY_RESULT(namespace_exists)) {
    // Verify that the namespace we found is running so that, once this request returns,
    // the client can send operations without receiving a "namespace not found" error.
    return data_->WaitForCreateNamespaceToFinish(this, namespace_name, database_type, namespace_id,
        CoarseMonoClock::Now() + default_admin_operation_timeout());
  }

  Status s = CreateNamespace(namespace_name, database_type, creator_role_name, namespace_id,
                             source_namespace_id, next_pg_oid, boost::none /* txn */, colocated);
  if (s.IsAlreadyPresent() && database_type && *database_type == YQLDatabase::YQL_DATABASE_CQL) {
    return Status::OK();
  }
  return s;
}

Status YBClient::IsCreateNamespaceInProgress(const std::string& namespace_name,
                                             const boost::optional<YQLDatabase>& database_type,
                                             const std::string& namespace_id,
                                             bool *create_in_progress) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->IsCreateNamespaceInProgress(this, namespace_name, database_type, namespace_id,
                                            deadline, create_in_progress);
}

Status YBClient::DeleteNamespace(const std::string& namespace_name,
                                 const boost::optional<YQLDatabase>& database_type,
                                 const std::string& namespace_id) {
  DeleteNamespaceRequestPB req;
  DeleteNamespaceResponsePB resp;
  req.mutable_namespace_()->set_name(namespace_name);
  if (!namespace_id.empty()) {
    req.mutable_namespace_()->set_id(namespace_id);
  }
  if (database_type) {
    req.set_database_type(*database_type);
    req.mutable_namespace_()->set_database_type(*database_type);
  }
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  Status s = data_->SyncLeaderMasterRpc<DeleteNamespaceRequestPB, DeleteNamespaceResponsePB>(
      deadline, req, &resp, nullptr, "DeleteNamespace", &MasterServiceProxy::DeleteNamespace);
  if (resp.has_error()) {
    s = StatusFromPB(resp.error().status());
  }
  RETURN_NOT_OK(s);

  // Verify that, once this request returns, the namespace has been successfully marked as deleted.
  RETURN_NOT_OK(data_->WaitForDeleteNamespaceToFinish(this, namespace_name, database_type,
      namespace_id, CoarseMonoClock::Now() + default_admin_operation_timeout()));

  return Status::OK();
}

Status YBClient::IsDeleteNamespaceInProgress(const std::string& namespace_name,
                                             const boost::optional<YQLDatabase>& database_type,
                                             const std::string& namespace_id,
                                             bool *delete_in_progress) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->IsDeleteNamespaceInProgress(this, namespace_name, database_type, namespace_id,
                                            deadline, delete_in_progress);
}

YBNamespaceAlterer* YBClient::NewNamespaceAlterer(
    const string& namespace_name, const std::string& namespace_id) {
  return new YBNamespaceAlterer(this, namespace_name, namespace_id);
}

Result<vector<master::NamespaceIdentifierPB>> YBClient::ListNamespaces(
    const boost::optional<YQLDatabase>& database_type) {
  ListNamespacesRequestPB req;
  ListNamespacesResponsePB resp;
  if (database_type) {
    req.set_database_type(*database_type);
  }
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListNamespaces);
  auto* namespaces = resp.mutable_namespaces();
  vector<master::NamespaceIdentifierPB> result;
  result.reserve(namespaces->size());
  for (auto& ns : *namespaces) {
    result.push_back(std::move(ns));
  }
  return result;
}

Status YBClient::GetNamespaceInfo(const std::string& namespace_id,
                                  const std::string& namespace_name,
                                  const boost::optional<YQLDatabase>& database_type,
                                  master::GetNamespaceInfoResponsePB* ret) {
  GetNamespaceInfoRequestPB req;
  GetNamespaceInfoResponsePB resp;

  if (!namespace_id.empty()) {
    req.mutable_namespace_()->set_id(namespace_id);
  }
  if (!namespace_name.empty()) {
    req.mutable_namespace_()->set_name(namespace_name);
  }
  if (database_type) {
    req.mutable_namespace_()->set_database_type(*database_type);
  }

  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetNamespaceInfo);
  ret->Swap(&resp);
  return Status::OK();
}

Status YBClient::ReservePgsqlOids(const std::string& namespace_id,
                                  const uint32_t next_oid, const uint32_t count,
                                  uint32_t* begin_oid, uint32_t* end_oid) {
  ReservePgsqlOidsRequestPB req;
  ReservePgsqlOidsResponsePB resp;
  req.set_namespace_id(namespace_id);
  req.set_next_oid(next_oid);
  req.set_count(count);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ReservePgsqlOids);
  *begin_oid = resp.begin_oid();
  *end_oid = resp.end_oid();
  return Status::OK();
}

Status YBClient::GetYsqlCatalogMasterVersion(uint64_t *ysql_catalog_version) {
  GetYsqlCatalogConfigRequestPB req;
  GetYsqlCatalogConfigResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetYsqlCatalogConfig);
  *ysql_catalog_version = resp.version();
  return Status::OK();
}

Status YBClient::GrantRevokePermission(GrantRevokeStatementType statement_type,
                                       const PermissionType& permission,
                                       const ResourceType& resource_type,
                                       const std::string& canonical_resource,
                                       const char* resource_name,
                                       const char* namespace_name,
                                       const std::string& role_name) {
  // Setting up request.
  GrantRevokePermissionRequestPB req;
  req.set_role_name(role_name);
  req.set_canonical_resource(canonical_resource);
  if (resource_name != nullptr) {
    req.set_resource_name(resource_name);
  }
  if (namespace_name != nullptr) {
    req.mutable_namespace_()->set_name(namespace_name);
  }
  req.set_resource_type(resource_type);
  req.set_permission(permission);

  req.set_revoke(statement_type == GrantRevokeStatementType::REVOKE);

  GrantRevokePermissionResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GrantRevokePermission);
  return Status::OK();
}

Result<bool> YBClient::NamespaceExists(const std::string& namespace_name,
                                       const boost::optional<YQLDatabase>& database_type) {
  for (const auto& ns : VERIFY_RESULT(ListNamespaces(database_type))) {
    if (ns.name() == namespace_name) {
      return true;
    }
  }
  return false;
}

Result<bool> YBClient::NamespaceIdExists(const std::string& namespace_id,
                                         const boost::optional<YQLDatabase>& database_type) {
  for (const auto& ns : VERIFY_RESULT(ListNamespaces(database_type))) {
    if (ns.id() == namespace_id) {
      return true;
    }
  }
  return false;
}

Status YBClient::CreateTablegroup(const std::string& namespace_name,
                                  const std::string& namespace_id,
                                  const std::string& tablegroup_id) {
  CreateTablegroupRequestPB req;
  CreateTablegroupResponsePB resp;
  req.set_id(tablegroup_id);
  req.set_namespace_id(namespace_id);
  req.set_namespace_name(namespace_name);

  int attempts = 0;
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();

  Status s = data_->SyncLeaderMasterRpc<CreateTablegroupRequestPB, CreateTablegroupResponsePB>(
      deadline, req, &resp, &attempts, "CreateTablegroup", &MasterServiceProxy::CreateTablegroup);

  // This case should not happen but need to validate contents since fields are optional in PB.
  if (!resp.has_parent_table_id() || !resp.has_parent_table_name()) {
    return STATUS(NotFound, "Parent table information not found in CREATE TABLEGROUP response.");
  }

  const YBTableName table_name(YQL_DATABASE_PGSQL, namespace_name, resp.parent_table_name());

  // Handle special cases based on resp.error().
  if (resp.has_error()) {
    LOG_IF(DFATAL, s.ok()) << "Expecting error status if response has error: " <<
        resp.error().code() << " Status: " << resp.error().status().ShortDebugString();

    if (resp.error().code() == master::MasterErrorPB::OBJECT_ALREADY_PRESENT && attempts > 1) {
      // If the table already exists and the number of attempts is >
      // 1, then it means we may have succeeded in creating the
      // table, but client didn't receive the successful
      // response (e.g., due to failure before the successful
      // response could be sent back, or due to a I/O pause or a
      // network blip leading to a timeout, etc...)
      YBTableInfo info;

      // A fix for https://yugabyte.atlassian.net/browse/ENG-529:
      // If we've been retrying table creation, and the table is now in the process is being
      // created, we can sometimes see an empty schema. Wait until the table is fully created
      // before we compare the schema.
      RETURN_NOT_OK_PREPEND(
          data_->WaitForCreateTableToFinish(this, table_name, resp.parent_table_id(), deadline),
          strings::Substitute("Failed waiting for table $0 to finish being created",
                              table_name.ToString()));

      RETURN_NOT_OK_PREPEND(
          data_->GetTableSchema(this, table_name, deadline, &info),
          strings::Substitute("Unable to check the schema of table $0", table_name.ToString()));

      YBSchemaBuilder schemaBuilder;
      schemaBuilder.AddColumn("parent_column")->Type(BINARY)->PrimaryKey()->NotNull();
      YBSchema ybschema;
      CHECK_OK(schemaBuilder.Build(&ybschema));

      if (!ybschema.Equals(info.schema)) {
         string msg = Format("Table $0 already exists with a different "
                             "schema. Requested schema was: $1, actual schema is: $2",
                             table_name,
                             internal::GetSchema(ybschema),
                             internal::GetSchema(info.schema));
        LOG(ERROR) << msg;
        return STATUS(AlreadyPresent, msg);
      }

      return Status::OK();
    }

    return StatusFromPB(resp.error().status());
  }

  // Wait for create table to finish.
  RETURN_NOT_OK_PREPEND(
      data_->WaitForCreateTableToFinish(this, table_name, resp.parent_table_id(), deadline),
      strings::Substitute("Failed waiting for parent table $0 to finish being created",
                          table_name.ToString()));

  return Status::OK();
}

Status YBClient::DeleteTablegroup(const std::string& namespace_id,
                                  const std::string& tablegroup_id) {
  DeleteTablegroupRequestPB req;
  DeleteTablegroupResponsePB resp;
  req.set_id(tablegroup_id);
  req.set_namespace_id(namespace_id);

  int attempts = 0;
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();

  Status s = data_->SyncLeaderMasterRpc<DeleteTablegroupRequestPB, DeleteTablegroupResponsePB>(
      deadline, req, &resp, &attempts, "DeleteTablegroup", &MasterServiceProxy::DeleteTablegroup);

  // This case should not happen but need to validate contents since fields are optional in PB.
  if (!resp.has_parent_table_id()) {
    return STATUS(NotFound, "Parent table information not found in DELETE TABLEGROUP response.");
  }

  // Handle special cases based on resp.error().
  if (resp.has_error()) {
    LOG_IF(DFATAL, s.ok()) << "Expecting error status if response has error: " <<
        resp.error().code() << " Status: " << resp.error().status().ShortDebugString();

    if (resp.error().code() == master::MasterErrorPB::OBJECT_NOT_FOUND && attempts > 1) {
      // A prior attempt to delete the table has succeeded, but
      // appeared as a failure to the client due to, e.g., an I/O or
      // network issue.
      LOG(INFO) << "Parent table for tablegroup with ID " << tablegroup_id << " already deleted.";
      return Status::OK();
    } else {
      return StatusFromPB(resp.error().status());
    }
  } else {
    // Check the status only if the response has no error.
    RETURN_NOT_OK(s);
  }

  // Spin until the table is deleted. Currently only waits till the table reaches DELETING state
  // See github issue #5290
  RETURN_NOT_OK_PREPEND(data_->WaitForDeleteTableToFinish(this,
                                                          resp.parent_table_id(),
                                                          deadline),
      strings::Substitute("Failed waiting for parent table with id $0 to finish being deleted",
                          resp.parent_table_id()));

  LOG(INFO) << "Deleted parent table for tablegroup with ID " << tablegroup_id;
  return Status::OK();
}

Result<vector<master::TablegroupIdentifierPB>>
YBClient::ListTablegroups(const std::string& namespace_name) {
  GetNamespaceInfoResponsePB ret;
  Status s = GetNamespaceInfo("", namespace_name, YQL_DATABASE_PGSQL, &ret);
  if (!s.ok()) {
    return s;
  }

  ListTablegroupsRequestPB req;
  ListTablegroupsResponsePB resp;

  req.set_namespace_id(ret.namespace_().id());
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListTablegroups);
  auto* tablegroups = resp.mutable_tablegroups();
  vector<master::TablegroupIdentifierPB> result;
  result.reserve(tablegroups->size());
  for (auto& tg : *tablegroups) {
    result.push_back(std::move(tg));
  }
  return result;
}

Result<bool> YBClient::TablegroupExists(const std::string& namespace_name,
                                        const std::string& tablegroup_id) {

  for (const auto& tg : VERIFY_RESULT(ListTablegroups(namespace_name))) {
    if (tg.id().compare(tablegroup_id) == 0) {
      return true;
    }
  }
  return false;
}

Status YBClient::GetUDType(const std::string& namespace_name,
                           const std::string& type_name,
                           std::shared_ptr<QLType>* ql_type) {
  // Setting up request.
  GetUDTypeInfoRequestPB req;
  req.mutable_type()->mutable_namespace_()->set_name(namespace_name);
  req.mutable_type()->set_type_name(type_name);

  // Sending request.
  GetUDTypeInfoResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetUDTypeInfo);

  // Filling in return values.
  std::vector<string> field_names;
  for (const auto& field_name : resp.udtype().field_names()) {
    field_names.push_back(field_name);
  }

  std::vector<shared_ptr<QLType>> field_types;
  for (const auto& field_type : resp.udtype().field_types()) {
    field_types.push_back(QLType::FromQLTypePB(field_type));
  }

  (*ql_type)->SetUDTypeFields(resp.udtype().id(), field_names, field_types);

  return Status::OK();
}

Status YBClient::CreateRole(const RoleName& role_name,
                            const std::string& salted_hash,
                            const bool login, const bool superuser,
                            const RoleName& creator_role_name) {

  // Setting up request.
  CreateRoleRequestPB req;
  req.set_salted_hash(salted_hash);
  req.set_name(role_name);
  req.set_login(login);
  req.set_superuser(superuser);

  if (!creator_role_name.empty()) {
    req.set_creator_role_name(creator_role_name);
  }

  CreateRoleResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, CreateRole);
  return Status::OK();
}

Status YBClient::AlterRole(const RoleName& role_name,
                           const boost::optional<std::string>& salted_hash,
                           const boost::optional<bool> login,
                           const boost::optional<bool> superuser,
                           const RoleName& current_role_name) {
  // Setting up request.
  AlterRoleRequestPB req;
  req.set_name(role_name);
  if (salted_hash) {
    req.set_salted_hash(*salted_hash);
  }
  if (login) {
    req.set_login(*login);
  }
  if (superuser) {
    req.set_superuser(*superuser);
  }
  req.set_current_role(current_role_name);

  AlterRoleResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, AlterRole);
  return Status::OK();
}

Status YBClient::DeleteRole(const std::string& role_name,
                            const std::string& current_role_name) {
  // Setting up request.
  DeleteRoleRequestPB req;
  req.set_name(role_name);
  req.set_current_role(current_role_name);

  DeleteRoleResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, DeleteRole);
  return Status::OK();
}

static const string kRequirePass = "requirepass";
Status YBClient::SetRedisPasswords(const std::vector<string>& passwords) {
  // TODO: Store hash instead of the password?
  return SetRedisConfig(kRequirePass, passwords);
}

Status YBClient::GetRedisPasswords(vector<string>* passwords) {
  Status s = GetRedisConfig(kRequirePass, passwords);
  if (s.IsNotFound()) {
    // If the redis config has no kRequirePass key.
    passwords->clear();
    s = Status::OK();
  }
  return s;
}

Status YBClient::SetRedisConfig(const string& key, const vector<string>& values) {
  // Setting up request.
  RedisConfigSetRequestPB req;
  req.set_keyword(key);
  for (const auto& value : values) {
    req.add_args(value);
  }
  RedisConfigSetResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, RedisConfigSet);
  return Status::OK();
}

Status YBClient::GetRedisConfig(const string& key, vector<string>* values) {
  // Setting up request.
  RedisConfigGetRequestPB req;
  RedisConfigGetResponsePB resp;
  req.set_keyword(key);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, RedisConfigGet);
  values->clear();
  for (const auto& arg : resp.args())
    values->push_back(arg);
  return Status::OK();
}

Status YBClient::GrantRevokeRole(GrantRevokeStatementType statement_type,
                                 const std::string& granted_role_name,
                                 const std::string& recipient_role_name) {
  // Setting up request.
  GrantRevokeRoleRequestPB req;
  req.set_revoke(statement_type == GrantRevokeStatementType::REVOKE);
  req.set_granted_role(granted_role_name);
  req.set_recipient_role(recipient_role_name);

  GrantRevokeRoleResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GrantRevokeRole);
  return Status::OK();
}

Status YBClient::GetPermissions(client::internal::PermissionsCache* permissions_cache) {
  if (!permissions_cache) {
    DFATAL_OR_RETURN_NOT_OK(STATUS(InvalidArgument, "Invalid null permissions_cache"));
  }

  boost::optional<uint64_t> version = permissions_cache->version();

  // Setting up request.
  GetPermissionsRequestPB req;
  if (version) {
    req.set_if_version_greater_than(*version);
  }

  GetPermissionsResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetPermissions);

  VLOG(1) << "Got permissions cache: " << resp.ShortDebugString();

  // The first request is a special case. We always replace the cache since we don't have anything.
  if (!version) {
    // We should at least receive cassandra's permissions.
    if (resp.role_permissions_size() == 0) {
      DFATAL_OR_RETURN_NOT_OK(
          STATUS(IllegalState, "Received invalid empty permissions cache from master"));

    }
  } else if (resp.version() == *version) {
      // No roles should have been received if both versions match.
      if (resp.role_permissions_size() != 0) {
        DFATAL_OR_RETURN_NOT_OK(STATUS(IllegalState,
            "Received permissions cache when none was expected because the master's "
            "permissions versions is equal to the client's version"));
      }
      // Nothing to update.
      return Status::OK();
  } else if (resp.version() < *version) {
    // If the versions don't match, then the master's version has to be greater than ours.
    DFATAL_OR_RETURN_NOT_OK(STATUS_SUBSTITUTE(IllegalState,
        "Client's permissions version $0 can't be greater than the master's permissions version $1",
        *version, resp.version()));
  }

  permissions_cache->UpdateRolesPermissions(resp);
  return Status::OK();
}

Status YBClient::CreateUDType(const std::string& namespace_name,
                              const std::string& type_name,
                              const std::vector<std::string>& field_names,
                              const std::vector<std::shared_ptr<QLType>>& field_types) {
  // Setting up request.
  CreateUDTypeRequestPB req;
  req.mutable_namespace_()->set_name(namespace_name);
  req.set_name(type_name);
  for (const string& field_name : field_names) {
    req.add_field_names(field_name);
  }
  for (const std::shared_ptr<QLType>& field_type : field_types) {
    field_type->ToQLTypePB(req.add_field_types());
  }

  CreateUDTypeResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, CreateUDType);
  return Status::OK();
}

Status YBClient::DeleteUDType(const std::string& namespace_name,
                              const std::string& type_name) {
  // Setting up request.
  DeleteUDTypeRequestPB req;
  req.mutable_type()->mutable_namespace_()->set_name(namespace_name);
  req.mutable_type()->set_type_name(type_name);

  DeleteUDTypeResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, DeleteUDType);
  return Status::OK();
}

Result<CDCStreamId> YBClient::CreateCDCStream(
    const TableId& table_id,
    const std::unordered_map<std::string, std::string>& options) {
  // Setting up request.
  CreateCDCStreamRequestPB req;
  req.set_table_id(table_id);
  req.mutable_options()->Reserve(options.size());
  for (const auto& option : options) {
    auto new_option = req.add_options();
    new_option->set_key(option.first);
    new_option->set_value(option.second);
  }

  CreateCDCStreamResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, CreateCDCStream);
  return resp.stream_id();
}

void YBClient::CreateCDCStream(const TableId& table_id,
                               const std::unordered_map<std::string, std::string>& options,
                               CreateCDCStreamCallback callback) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  data_->CreateCDCStream(this, table_id, options, deadline, callback);
}

Status YBClient::GetCDCStream(const CDCStreamId& stream_id,
                              TableId* table_id,
                              std::unordered_map<std::string, std::string>* options) {
  // Setting up request.
  GetCDCStreamRequestPB req;
  req.set_stream_id(stream_id);

  // Sending request.
  GetCDCStreamResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetCDCStream);

  // Filling in return values.
  *table_id = resp.stream().table_id();

  options->clear();
  options->reserve(resp.stream().options_size());
  for (const auto& option : resp.stream().options()) {
    options->emplace(option.key(), option.value());
  }

  return Status::OK();
}

void YBClient::GetCDCStream(const CDCStreamId& stream_id,
                            std::shared_ptr<TableId> table_id,
                            std::shared_ptr<std::unordered_map<std::string, std::string>> options,
                            StdStatusCallback callback) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  data_->GetCDCStream(this, stream_id, table_id, options, deadline, callback);
}

Status YBClient::DeleteCDCStream(const vector<CDCStreamId>& streams) {
  if (streams.empty()) {
    return STATUS(InvalidArgument, "At least one stream id should be provided");
  }

  // Setting up request.
  DeleteCDCStreamRequestPB req;
  req.mutable_stream_id()->Reserve(streams.size());
  for (const auto& stream : streams) {
    req.add_stream_id(stream);
  }

  DeleteCDCStreamResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, DeleteCDCStream);
  return Status::OK();
}

Status YBClient::DeleteCDCStream(const CDCStreamId& stream_id) {
  // Setting up request.
  DeleteCDCStreamRequestPB req;
  req.add_stream_id(stream_id);

  DeleteCDCStreamResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, DeleteCDCStream);
  return Status::OK();
}

void YBClient::DeleteCDCStream(const CDCStreamId& stream_id, StatusCallback callback) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  data_->DeleteCDCStream(this, stream_id, deadline, callback);
}

void YBClient::DeleteTablet(const TabletId& tablet_id, StdStatusCallback callback) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  data_->DeleteTablet(this, tablet_id, deadline, callback);
}

Status YBClient::TabletServerCount(int *tserver_count, bool primary_only, bool use_cache) {
  int tserver_count_cached = data_->tserver_count_cached_.load(std::memory_order_acquire);
  if (use_cache && tserver_count_cached > 0) {
    *tserver_count = tserver_count_cached;
    return Status::OK();
  }

  ListTabletServersRequestPB req;
  ListTabletServersResponsePB resp;
  req.set_primary_only(primary_only);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListTabletServers);
  data_->tserver_count_cached_.store(resp.servers_size(), std::memory_order_release);
  *tserver_count = resp.servers_size();
  return Status::OK();
}

Status YBClient::ListTabletServers(vector<std::unique_ptr<YBTabletServer>>* tablet_servers) {
  ListTabletServersRequestPB req;
  ListTabletServersResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListTabletServers);
  for (int i = 0; i < resp.servers_size(); i++) {
    const ListTabletServersResponsePB_Entry& e = resp.servers(i);
    auto ts = std::make_unique<YBTabletServer>(
        e.instance_id().permanent_uuid(),
        DesiredHostPort(e.registration().common(), data_->cloud_info_pb_).host(),
        e.registration().common().placement_uuid());
    tablet_servers->push_back(std::move(ts));
  }
  return Status::OK();
}

void YBClient::SetLocalTabletServer(const string& ts_uuid,
                                    const shared_ptr<tserver::TabletServerServiceProxy>& proxy,
                                    const tserver::LocalTabletServer* local_tserver) {
  data_->meta_cache_->SetLocalTabletServer(ts_uuid, proxy, local_tserver);
}

Result<bool> YBClient::IsLoadBalanced(uint32_t num_servers) {
  IsLoadBalancedRequestPB req;
  IsLoadBalancedResponsePB resp;

  req.set_expected_num_servers(num_servers);
  // Cannot use CALL_SYNC_LEADER_MASTER_RPC directly since this is substituted with RETURN_NOT_OK
  // and we want to capture the status to check if load is balanced.
  Status s = [&, this]() -> Status {
    CALL_SYNC_LEADER_MASTER_RPC(req, resp, IsLoadBalanced);
    return Status::OK();
  }();
  return s.ok();
}

Result<bool> YBClient::IsLoadBalancerIdle() {
  IsLoadBalancerIdleRequestPB req;
  IsLoadBalancerIdleResponsePB resp;

  Status s = [&]() -> Status {
    CALL_SYNC_LEADER_MASTER_RPC(req, resp, IsLoadBalancerIdle);
    return Status::OK();
  }();

  if (s.ok()) {
    return true;
  } else if (master::MasterError(s) == master::MasterErrorPB::LOAD_BALANCER_RECENTLY_ACTIVE) {
    return false;
  } else {
    return s;
  }
}

Status YBClient::GetTabletsFromTableId(const string& table_id,
                                       const int32_t max_tablets,
                                       RepeatedPtrField<TabletLocationsPB>* tablets) {
  GetTableLocationsRequestPB req;
  GetTableLocationsResponsePB resp;
  req.mutable_table()->set_table_id(table_id);

  if (max_tablets == 0) {
    req.set_max_returned_locations(std::numeric_limits<int32_t>::max());
  } else if (max_tablets > 0) {
    req.set_max_returned_locations(max_tablets);
  }
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetTableLocations);
  *tablets = resp.tablet_locations();
  return Status::OK();
}

Status YBClient::GetTablets(const YBTableName& table_name,
                            const int32_t max_tablets,
                            RepeatedPtrField<TabletLocationsPB>* tablets,
                            const RequireTabletsRunning require_tablets_running) {
  GetTableLocationsRequestPB req;
  GetTableLocationsResponsePB resp;
  if (table_name.has_table()) {
    table_name.SetIntoTableIdentifierPB(req.mutable_table());
  } else if (table_name.has_table_id()) {
    req.mutable_table()->set_table_id(table_name.table_id());
  }

  if (max_tablets == 0) {
    req.set_max_returned_locations(std::numeric_limits<int32_t>::max());
  } else if (max_tablets > 0) {
    req.set_max_returned_locations(max_tablets);
  }
  req.set_require_tablets_running(require_tablets_running);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetTableLocations);
  *tablets = resp.tablet_locations();
  return Status::OK();
}

Status YBClient::GetTabletLocation(const TabletId& tablet_id,
                                   master::TabletLocationsPB* tablet_location) {
  GetTabletLocationsRequestPB req;
  GetTabletLocationsResponsePB resp;
  req.add_tablet_ids(tablet_id);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetTabletLocations);

  if (resp.tablet_locations_size() != 1) {
    return STATUS_SUBSTITUTE(IllegalState, "Expected single tablet for $0, received $1",
                             tablet_id, resp.tablet_locations_size());
  }

  *tablet_location = resp.tablet_locations(0);
  return Status::OK();
}

namespace {

void FillFromRepeatedTabletLocations(
    const RepeatedPtrField<TabletLocationsPB>& tablets,
    vector<TabletId>* tablet_uuids,
    vector<string>* ranges,
    std::vector<master::TabletLocationsPB>* locations) {
  tablet_uuids->reserve(tablets.size());
  if (ranges != nullptr) {
    ranges->reserve(tablets.size());
  }
  for (const TabletLocationsPB& tablet : tablets) {
    if (locations) {
      locations->push_back(tablet);
    }
    tablet_uuids->push_back(tablet.tablet_id());
    if (ranges != nullptr) {
      const PartitionPB& partition = tablet.partition();
      ranges->push_back(partition.ShortDebugString());
    }
  }
}

} // namespace

Status YBClient::GetTablets(const YBTableName& table_name,
                            const int32_t max_tablets,
                            vector<TabletId>* tablet_uuids,
                            vector<string>* ranges,
                            std::vector<master::TabletLocationsPB>* locations,
                            const RequireTabletsRunning require_tablets_running) {
  RepeatedPtrField<TabletLocationsPB> tablets;
  RETURN_NOT_OK(GetTablets(table_name, max_tablets, &tablets, require_tablets_running));
  FillFromRepeatedTabletLocations(tablets, tablet_uuids, ranges, locations);
  return Status::OK();
}

Status YBClient::GetTabletsAndUpdateCache(
    const YBTableName& table_name,
    const int32_t max_tablets,
    vector<TabletId>* tablet_uuids,
    vector<string>* ranges,
    std::vector<master::TabletLocationsPB>* locations) {
  RepeatedPtrField<TabletLocationsPB> tablets;
  RETURN_NOT_OK(GetTablets(table_name, max_tablets, &tablets, RequireTabletsRunning::kFalse));
  FillFromRepeatedTabletLocations(tablets, tablet_uuids, ranges, locations);

  RETURN_NOT_OK(data_->meta_cache_->ProcessTabletLocations(
      tablets, /* partition_group_start= */ nullptr, /* lookup_rpc= */ nullptr));

  return Status::OK();
}

rpc::Messenger* YBClient::messenger() const {
  return data_->messenger_;
}

const scoped_refptr<MetricEntity>& YBClient::metric_entity() const {
  return data_->metric_entity_;
}

rpc::ProxyCache& YBClient::proxy_cache() const {
  return *data_->proxy_cache_;
}

ThreadPool *YBClient::callback_threadpool() {
  return data_->cb_threadpool_.get();
}

const std::string& YBClient::proxy_uuid() const {
  return data_->uuid_;
}

const ClientId& YBClient::id() const {
  return data_->id_;
}

const CloudInfoPB& YBClient::cloud_info() const {
  return data_->cloud_info_pb_;
}

std::pair<RetryableRequestId, RetryableRequestId> YBClient::NextRequestIdAndMinRunningRequestId(
    const TabletId& tablet_id) {
  std::lock_guard<simple_spinlock> lock(data_->tablet_requests_mutex_);
  auto& tablet = data_->tablet_requests_[tablet_id];
  if (tablet.request_id_seq == kInitializeFromMinRunning) {
    return std::make_pair(kInitializeFromMinRunning, kInitializeFromMinRunning);
  }
  auto id = tablet.request_id_seq++;
  tablet.running_requests.insert(id);
  return std::make_pair(id, *tablet.running_requests.begin());
}

void YBClient::RequestFinished(const TabletId& tablet_id, RetryableRequestId request_id) {
  if (request_id == kInitializeFromMinRunning) {
    return;
  }
  std::lock_guard<simple_spinlock> lock(data_->tablet_requests_mutex_);
  auto& tablet = data_->tablet_requests_[tablet_id];
  auto it = tablet.running_requests.find(request_id);
  if (it != tablet.running_requests.end()) {
    tablet.running_requests.erase(it);
  } else {
    LOG(DFATAL) << "RequestFinished called for an unknown request: "
                << tablet_id << ", " << request_id;
  }
}

void YBClient::MaybeUpdateMinRunningRequestId(
    const TabletId& tablet_id, RetryableRequestId min_running_request_id) {
  std::lock_guard<simple_spinlock> lock(data_->tablet_requests_mutex_);
  auto& tablet = data_->tablet_requests_[tablet_id];
  if (tablet.request_id_seq == kInitializeFromMinRunning) {
    tablet.request_id_seq = min_running_request_id + (1 << 24);
    VLOG(1) << "Set request_id_seq for tablet " << tablet_id << " to " << tablet.request_id_seq;
  }
}

void YBClient::LookupTabletByKey(const std::shared_ptr<const YBTable>& table,
                                 const std::string& partition_key,
                                 CoarseTimePoint deadline,
                                 LookupTabletCallback callback) {
  data_->meta_cache_->LookupTabletByKey(table, partition_key, deadline, std::move(callback));
}

void YBClient::LookupTabletById(const std::string& tablet_id,
                                const std::shared_ptr<const YBTable>& table,
                                CoarseTimePoint deadline,
                                LookupTabletCallback callback,
                                UseCache use_cache) {
  data_->meta_cache_->LookupTabletById(
      tablet_id, table, deadline, std::move(callback), use_cache);
}

void YBClient::LookupAllTablets(const std::shared_ptr<const YBTable>& table,
                                CoarseTimePoint deadline,
                                LookupTabletRangeCallback callback) {
  data_->meta_cache_->LookupAllTablets(table, deadline, std::move(callback));
}

std::future<Result<std::vector<internal::RemoteTabletPtr>>> YBClient::LookupAllTabletsFuture(
    const std::shared_ptr<const YBTable>& table,
    CoarseTimePoint deadline) {
  return MakeFuture<Result<std::vector<internal::RemoteTabletPtr>>>([&](auto callback) {
    this->LookupAllTablets(table, deadline, std::move(callback));
  });
}

HostPort YBClient::GetMasterLeaderAddress() {
  return data_->leader_master_hostport();
}

Status YBClient::ListMasters(CoarseTimePoint deadline, std::vector<std::string>* master_uuids) {
  ListMastersRequestPB req;
  ListMastersResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC_WITH_DEADLINE(req, resp, deadline, ListMasters);

  master_uuids->clear();
  for (const ServerEntryPB& master : resp.masters()) {
    if (master.has_error()) {
      LOG(ERROR) << "Master " << master.ShortDebugString() << " hit error "
        << master.error().ShortDebugString();
      return StatusFromPB(master.error());
    }
    master_uuids->push_back(master.instance_id().permanent_uuid());
  }
  return Status::OK();
}

Result<HostPort> YBClient::RefreshMasterLeaderAddress() {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  RETURN_NOT_OK(data_->SetMasterServerProxy(deadline));

  return GetMasterLeaderAddress();
}

Status YBClient::RemoveMasterFromClient(const HostPort& remove) {
  return data_->RemoveMasterAddress(remove);
}

Status YBClient::AddMasterToClient(const HostPort& add) {
  return data_->AddMasterAddress(add);
}

Status YBClient::SetMasterAddresses(const std::string& addrs) {
  return data_->SetMasterAddresses(addrs);
}

Status YBClient::GetMasterUUID(const string& host,
                               int16_t port,
                               string* uuid) {
  HostPort hp(host, port);
  ServerEntryPB server;
  RETURN_NOT_OK(master::GetMasterEntryForHosts(
      data_->proxy_cache_.get(), {hp}, default_rpc_timeout(), &server));

  if (server.has_error()) {
    return STATUS_FORMAT(
      RuntimeError,
      "Error while getting uuid of $0.",
      HostPortToString(host, port));
  }

  *uuid = server.instance_id().permanent_uuid();

  return Status::OK();
}

Status YBClient::SetReplicationInfo(const ReplicationInfoPB& replication_info) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->SetReplicationInfo(this, replication_info, deadline);
}

Result<std::vector<YBTableName>> YBClient::ListTables(const std::string& filter,
                                                      bool exclude_ysql) {
  ListTablesRequestPB req;
  ListTablesResponsePB resp;

  if (!filter.empty()) {
    req.set_name_filter(filter);
  }
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListTables);
  std::vector<YBTableName> result;
  result.reserve(resp.tables_size());
  for (int i = 0; i < resp.tables_size(); i++) {
    const ListTablesResponsePB_TableInfo& table_info = resp.tables(i);
    DCHECK(table_info.has_namespace_());
    DCHECK(table_info.namespace_().has_name());
    DCHECK(table_info.namespace_().has_id());
    if (exclude_ysql && table_info.table_type() == TableType::PGSQL_TABLE_TYPE) {
      continue;
    }
    result.emplace_back(master::GetDatabaseTypeForTable(table_info.table_type()),
                        table_info.namespace_().id(),
                        table_info.namespace_().name(),
                        table_info.id(),
                        table_info.name(),
                        table_info.relation_type());
  }
  return result;
}

Result<bool> YBClient::TableExists(const YBTableName& table_name) {
  for (const YBTableName& table : VERIFY_RESULT(ListTables(table_name.table_name()))) {
    if (table == table_name) {
      return true;
    }
  }
  return false;
}

Status YBClient::OpenTable(const YBTableName& table_name, shared_ptr<YBTable>* table) {
  YBTableInfo info;
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  RETURN_NOT_OK(data_->GetTableSchema(this, table_name, deadline, &info));

  // In the future, probably will look up the table in some map to reuse YBTable
  // instances.
  std::shared_ptr<YBTable> ret(new YBTable(this, info));
  RETURN_NOT_OK(ret->Open());
  table->swap(ret);
  return Status::OK();
}

Status YBClient::OpenTable(const TableId& table_id, shared_ptr<YBTable>* table) {
  YBTableInfo info;
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  RETURN_NOT_OK(data_->GetTableSchema(this, table_id, deadline, &info));

  // In the future, probably will look up the table in some map to reuse YBTable
  // instances.
  std::shared_ptr<YBTable> ret(new YBTable(this, info));
  RETURN_NOT_OK(ret->Open());
  table->swap(ret);
  return Status::OK();
}

shared_ptr<YBSession> YBClient::NewSession() {
  return std::make_shared<YBSession>(this);
}

bool YBClient::IsMultiMaster() const {
  return data_->IsMultiMaster();
}

Result<int> YBClient::NumTabletsForUserTable(TableType table_type) {
  if (FLAGS_TEST_yb_num_total_tablets > 0) {
    VLOG(1) << "num_tablets=" << FLAGS_TEST_yb_num_total_tablets
            << ": --TEST_yb_num_total_tablets is specified.";
    return FLAGS_TEST_yb_num_total_tablets;
  } else {
    int tserver_count = 0;
    RETURN_NOT_OK(TabletServerCount(&tserver_count, true /* primary_only */));
    int num_tablets = 0;
    if (table_type == TableType::PGSQL_TABLE_TYPE) {
      num_tablets = tserver_count * FLAGS_ysql_num_shards_per_tserver;
      VLOG(1) << "num_tablets = " << num_tablets << ": "
              << "calculated as tserver_count * FLAGS_ysql_num_shards_per_tserver ("
              << tserver_count << " * " << FLAGS_ysql_num_shards_per_tserver << ")";
    } else {
      num_tablets = tserver_count * FLAGS_yb_num_shards_per_tserver;
      VLOG(1) << "num_tablets = " << num_tablets << ": "
              << "calculated as tserver_count * FLAGS_yb_num_shards_per_tserver ("
              << tserver_count << " * " << FLAGS_yb_num_shards_per_tserver << ")";
    }
    return num_tablets;
  }
}

void YBClient::TEST_set_admin_operation_timeout(const MonoDelta& timeout) {
  data_->default_admin_operation_timeout_ = timeout;
}

const MonoDelta& YBClient::default_admin_operation_timeout() const {
  return data_->default_admin_operation_timeout_;
}

const MonoDelta& YBClient::default_rpc_timeout() const {
  return data_->default_rpc_timeout_;
}

const uint64_t YBClient::kNoHybridTime = 0;

uint64_t YBClient::GetLatestObservedHybridTime() const {
  return data_->GetLatestObservedHybridTime();
}

void YBClient::SetLatestObservedHybridTime(uint64_t ht_hybrid_time) {
  data_->UpdateLatestObservedHybridTime(ht_hybrid_time);
}

}  // namespace client
}  // namespace yb
