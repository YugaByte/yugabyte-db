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

#include "yb/master/master-test_base.h"

#include <memory>

#include <gtest/gtest.h>

#include "yb/common/partial_row.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/master-test-util.h"
#include "yb/master/master.h"
#include "yb/master/master.proxy.h"
#include "yb/master/mini_master.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"
#include "yb/rpc/messenger.h"
#include "yb/server/rpc_server.h"
#include "yb/server/server_base.proxy.h"
#include "yb/util/jsonreader.h"
#include "yb/util/status.h"
#include "yb/util/test_util.h"

DECLARE_bool(catalog_manager_check_ts_count_for_create_table);

namespace yb {
namespace master {

void MasterTestBase::SetUp() {
  YBTest::SetUp();

  // Set an RPC timeout for the controllers.
  controller_ = make_shared<RpcController>();
  controller_->set_timeout(MonoDelta::FromSeconds(10));

  // In this test, we create tables to test catalog manager behavior,
  // but we have no tablet servers. Typically this would be disallowed.
  FLAGS_catalog_manager_check_ts_count_for_create_table = false;

  // Start master with the create flag on.
  mini_master_.reset(new MiniMaster(Env::Default(), GetTestPath("Master"),
                                    AllocateFreePort(), AllocateFreePort(), 0));
  ASSERT_OK(mini_master_->Start());
  ASSERT_OK(mini_master_->master()->WaitUntilCatalogManagerIsLeaderAndReadyForTests());

  // Create a client proxy to it.
  client_messenger_ = ASSERT_RESULT(MessengerBuilder("Client").Build());
  rpc::ProxyCache proxy_cache(client_messenger_.get());
  proxy_.reset(new MasterServiceProxy(&proxy_cache, mini_master_->bound_rpc_addr()));

  // Create the default test namespace.
  CreateNamespaceResponsePB resp;
  ASSERT_OK(CreateNamespace(default_namespace_name, &resp));
  default_namespace_id = resp.id();
}

void MasterTestBase::TearDown() {
  client_messenger_->Shutdown();
  mini_master_->Shutdown();
  YBTest::TearDown();
}

Status MasterTestBase::CreateTable(const NamespaceName& namespace_name,
                                   const TableName& table_name,
                                   const Schema& schema,
                                   TableId *table_id /* = nullptr */) {
  CreateTableRequestPB req;
  return DoCreateTable(namespace_name, table_name, schema, &req, table_id);
}

Status MasterTestBase::CreatePgsqlTable(const NamespaceId& namespace_id,
                                        const TableName& table_name,
                                        const Schema& schema) {
  CreateTableRequestPB req, *request;
  request = &req;
  CreateTableResponsePB resp;

  request->set_table_type(TableType::PGSQL_TABLE_TYPE);
  request->set_name(table_name);
  SchemaToPB(schema, request->mutable_schema());

  if (!namespace_id.empty()) {
    request->mutable_namespace_()->set_id(namespace_id);
  }
  request->mutable_partition_schema()->set_hash_schema(PartitionSchemaPB::PGSQL_HASH_SCHEMA);
  request->mutable_schema()->mutable_table_properties()->set_num_tablets(8);

  // Dereferencing as the RPCs require const ref for request. Keeping request param as pointer
  // though, as that helps with readability and standardization.
  RETURN_NOT_OK(proxy_->CreateTable(*request, &resp, ResetAndGetController()));
  if (resp.has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp.error().status()));
  }
  return Status::OK();
}

Status MasterTestBase::CreateTablegroupTable(const NamespaceId& namespace_id,
                                             const TableName& table_name,
                                             const TablegroupId& tablegroup_id,
                                             const Schema& schema) {
  CreateTableRequestPB req, *request;
  request = &req;
  CreateTableResponsePB resp;

  request->set_table_type(TableType::PGSQL_TABLE_TYPE);
  request->set_name(table_name);
  request->set_colocated(true);
  request->set_tablegroup_id(tablegroup_id);
  SchemaToPB(schema, request->mutable_schema());

  if (!namespace_id.empty()) {
    request->mutable_namespace_()->set_id(namespace_id);
  }

  // Dereferencing as the RPCs require const ref for request. Keeping request param as pointer
  // though, as that helps with readability and standardization.
  RETURN_NOT_OK(proxy_->CreateTable(*request, &resp, ResetAndGetController()));
  if (resp.has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp.error().status()));
  }
  return Status::OK();
}

Status MasterTestBase::DoCreateTable(const NamespaceName& namespace_name,
                                     const TableName& table_name,
                                     const Schema& schema,
                                     CreateTableRequestPB* request,
                                     TableId *table_id /* = nullptr */) {
  CreateTableResponsePB resp;

  request->set_name(table_name);
  SchemaToPB(schema, request->mutable_schema());

  if (!namespace_name.empty()) {
    request->mutable_namespace_()->set_name(namespace_name);
  }
  request->mutable_partition_schema()->set_hash_schema(PartitionSchemaPB::MULTI_COLUMN_HASH_SCHEMA);
  request->mutable_schema()->mutable_table_properties()->set_num_tablets(8);

  // Dereferencing as the RPCs require const ref for request. Keeping request param as pointer
  // though, as that helps with readability and standardization.
  RETURN_NOT_OK(proxy_->CreateTable(*request, &resp, ResetAndGetController()));
  if (table_id) {
    *table_id = resp.table_id();
  }
  if (resp.has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp.error().status()));
  }

  return Status::OK();
}

void MasterTestBase::DoListTables(const ListTablesRequestPB& req, ListTablesResponsePB* resp) {
  ASSERT_OK(proxy_->ListTables(req, resp, ResetAndGetController()));
  SCOPED_TRACE(resp->DebugString());
  ASSERT_FALSE(resp->has_error());
}

void MasterTestBase::DoListAllTables(ListTablesResponsePB* resp,
                                     const NamespaceName& namespace_name /*= ""*/) {
  ListTablesRequestPB req;

  if (!namespace_name.empty()) {
    req.mutable_namespace_()->set_name(namespace_name);
  }

  DoListTables(req, resp);
}

Status MasterTestBase::DeleteTable(const NamespaceName& namespace_name,
                                   const TableName& table_name,
                                   TableId* table_id /* = nullptr */) {
  DeleteTableRequestPB req;
  DeleteTableResponsePB resp;
  req.mutable_table()->set_table_name(table_name);

  if (!namespace_name.empty()) {
    req.mutable_table()->mutable_namespace_()->set_name(namespace_name);
  }

  RETURN_NOT_OK(proxy_->DeleteTable(req, &resp, ResetAndGetController()));
  SCOPED_TRACE(resp.DebugString());
  if (table_id) {
    *table_id = resp.table_id();
  }

  if (resp.has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp.error().status()));
  }
  return Status::OK();
}

Status MasterTestBase::CreateTablegroup(const TablegroupId& tablegroup_id,
                                        const NamespaceId& namespace_id,
                                        const NamespaceName& namespace_name) {
  CreateTablegroupRequestPB req, *request;
  request = &req;
  CreateTablegroupResponsePB resp;

  request->set_id(tablegroup_id);
  request->set_namespace_id(namespace_id);
  request->set_namespace_name(namespace_name);

  // Dereferencing as the RPCs require const ref for request. Keeping request param as pointer
  // though, as that helps with readability and standardization.
  RETURN_NOT_OK(proxy_->CreateTablegroup(*request, &resp, ResetAndGetController()));
  if (resp.has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp.error().status()));
  }
  return Status::OK();
}

Status MasterTestBase::DeleteTablegroup(const TablegroupId& tablegroup_id,
                                        const NamespaceId& namespace_id) {
  DeleteTablegroupRequestPB req;
  DeleteTablegroupResponsePB resp;
  req.set_id(tablegroup_id);
  req.set_namespace_id(namespace_id);

  RETURN_NOT_OK(proxy_->DeleteTablegroup(req, &resp, ResetAndGetController()));
  if (resp.has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp.error().status()));
  }
  return Status::OK();
}

void MasterTestBase::DoListTablegroups(const ListTablegroupsRequestPB& req,
                                       ListTablegroupsResponsePB* resp) {
  ASSERT_OK(proxy_->ListTablegroups(req, resp, ResetAndGetController()));
  SCOPED_TRACE(resp->DebugString());
  ASSERT_FALSE(resp->has_error());
}

void MasterTestBase::DoListAllNamespaces(ListNamespacesResponsePB* resp) {
  DoListAllNamespaces(boost::none, resp);
}

void MasterTestBase::DoListAllNamespaces(const boost::optional<YQLDatabase>& database_type,
                                         ListNamespacesResponsePB* resp) {
  ListNamespacesRequestPB req;
  if (database_type) {
    req.set_database_type(*database_type);
  }

  ASSERT_OK(proxy_->ListNamespaces(req, resp, ResetAndGetController()));
  SCOPED_TRACE(resp->DebugString());
  ASSERT_FALSE(resp->has_error());
}

Status MasterTestBase::CreateNamespace(const NamespaceName& ns_name,
                                       CreateNamespaceResponsePB* resp) {
  return CreateNamespace(ns_name, boost::none, resp);
}

Status MasterTestBase::CreateNamespace(const NamespaceName& ns_name,
                                       const boost::optional<YQLDatabase>& database_type,
                                       CreateNamespaceResponsePB* resp) {
  RETURN_NOT_OK(CreateNamespaceAsync(ns_name, database_type, resp));
  return CreateNamespaceWait(resp->id(), database_type);
}

Status MasterTestBase::CreateNamespaceAsync(const NamespaceName& ns_name,
                                            const boost::optional<YQLDatabase>& database_type,
                                            CreateNamespaceResponsePB* resp) {
  CreateNamespaceRequestPB req;
  req.set_name(ns_name);
  if (database_type) {
    req.set_database_type(*database_type);
  }

  RETURN_NOT_OK(proxy_->CreateNamespace(req, resp, ResetAndGetController()));
  if (resp->has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp->error().status()));
  }
  return Status::OK();
}

Status MasterTestBase::CreateNamespaceWait(const NamespaceId& ns_id,
                                           const boost::optional<YQLDatabase>& database_type) {
  Status status = Status::OK();

  IsCreateNamespaceDoneRequestPB is_req;
  is_req.mutable_namespace_()->set_id(ns_id);
  if (database_type) {
    is_req.mutable_namespace_()->set_database_type(*database_type);
  }

  AssertLoggedWaitFor([&]() -> Result<bool> {
    IsCreateNamespaceDoneResponsePB is_resp;
    status = proxy_->IsCreateNamespaceDone(is_req, &is_resp, ResetAndGetController());
    WARN_NOT_OK(status, "IsCreateNamespaceDone returned unexpected error");
    if (status.ok()) {
      if (is_resp.has_done() && is_resp.done()) {
        if (is_resp.has_error()) {
          status = StatusFromPB(is_resp.error().status());
        }
        return true;
      }
    }
    return false;
  }, MonoDelta::FromSeconds(60), "Wait for create namespace to finish async setup tasks.");

  return status;
}

Status MasterTestBase::AlterNamespace(const NamespaceName& ns_name,
                                      const NamespaceId& ns_id,
                                      const boost::optional<YQLDatabase>& database_type,
                                      const std::string& new_name,
                                      AlterNamespaceResponsePB* resp) {
  AlterNamespaceRequestPB req;
  req.mutable_namespace_()->set_id(ns_id);
  req.mutable_namespace_()->set_name(ns_name);
  if (database_type) {
    req.mutable_namespace_()->set_database_type(*database_type);
  }
  req.set_new_name(new_name);

  RETURN_NOT_OK(proxy_->AlterNamespace(req, resp, ResetAndGetController()));
  if (resp->has_error()) {
    RETURN_NOT_OK(StatusFromPB(resp->error().status()));
  }
  return Status::OK();
}

// PGSQL Namespaces are deleted asynchronously since they may delete a large number of tables.
// CQL Namespaces don't need to call this function and return success if present.
Status MasterTestBase::DeleteNamespaceWait(IsDeleteNamespaceDoneRequestPB const& del_req) {
  Status status = Status::OK();
  AssertLoggedWaitFor([&]() -> Result<bool> {
    IsDeleteNamespaceDoneResponsePB del_resp;
    status = proxy_->IsDeleteNamespaceDone(del_req, &del_resp, ResetAndGetController());
    if (!status.ok()) {
      WARN_NOT_OK(status, "IsDeleteNamespaceDone returned unexpected error");
      return true;
    }
    if (del_resp.has_done() && del_resp.done()) {
      if (del_resp.has_error()) {
        status = StatusFromPB(del_resp.error().status());
      }
      return true;
    }
    return false;
  }, MonoDelta::FromSeconds(10), "Wait for delete namespace to finish async cleanup tasks.");

  return status;
}

Status MasterTestBase::DeleteTableSync(const NamespaceName& ns_name, const TableName& table_name,
                                       TableId* table_id) {
  RETURN_NOT_OK(DeleteTable(ns_name, table_name, table_id));

  IsDeleteTableDoneRequestPB done_req;
  done_req.set_table_id(*table_id);
  IsDeleteTableDoneResponsePB done_resp;
  bool delete_done = false;

  for (int num_retries = 0; num_retries < 30; ++num_retries) {
    RETURN_NOT_OK(proxy_->IsDeleteTableDone(done_req, &done_resp, ResetAndGetController()));
    if (!done_resp.has_done()) {
      return STATUS_FORMAT(
          IllegalState, "Expected IsDeleteTableDone response to set value for done ($0.$1)",
          ns_name, table_name);
    }
    if (done_resp.done()) {
      LOG(INFO) << "Done on retry " << num_retries;
      delete_done = true;
      break;
    }

    SleepFor(MonoDelta::FromMilliseconds(10 * num_retries)); // sleep a bit more with each attempt.
  }

  if (!delete_done) {
    return STATUS_FORMAT(IllegalState, "Delete Table did not complete ($0.$1)",
                         ns_name, table_name);
  }
  return Status::OK();
}

} // namespace master
} // namespace yb
