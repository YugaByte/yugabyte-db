//--------------------------------------------------------------------------------------------------
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
//
//
// Treenode definitions for GRANT statements.
//--------------------------------------------------------------------------------------------------

#include "yb/common/redis_constants_common.h"
#include "yb/yql/cql/ql/ptree/pt_grant_revoke.h"
#include "yb/yql/cql/ql/ptree/sem_context.h"
#include "yb/gutil/strings/substitute.h"

DECLARE_bool(use_cassandra_authentication);

namespace yb {
namespace ql {

using std::shared_ptr;
using std::to_string;
using strings::Substitute;

//--------------------------------------------------------------------------------------------------
// GRANT Role Statement.

PTGrantRevokeRole::PTGrantRevokeRole(MemoryContext* memctx,
                         YBLocation::SharedPtr loc,
                         GrantRevokeStatementType statement_type,
                         const MCSharedPtr<MCString>& granted_role_name,
                         const MCSharedPtr<MCString>& recipient_role_name)
    : TreeNode(memctx, loc),
      statement_type_(statement_type),
      granted_role_name_(granted_role_name),
      recipient_role_name_(recipient_role_name) {}

PTGrantRevokeRole::~PTGrantRevokeRole() {
}

Status PTGrantRevokeRole::Analyze(SemContext* sem_context) {
  if (FLAGS_use_cassandra_authentication) {
    RETURN_NOT_OK(sem_context->CheckHasRolePermission(loc(), PermissionType::AUTHORIZE_PERMISSION,
        recipient_role_name_->data()));
    RETURN_NOT_OK(sem_context->CheckHasRolePermission(loc(), PermissionType::AUTHORIZE_PERMISSION,
        granted_role_name_->data()));
  }
  return Status::OK();
}

void PTGrantRevokeRole::PrintSemanticAnalysisResult(SemContext* sem_context) {
  MCString sem_output("\tGRANT Role ", sem_context->PTempMem());
  sem_output = sem_output + granted_role_name().c_str() +  " TO  " + recipient_role_name().c_str();
  VLOG(3) << "SEMANTIC ANALYSIS RESULT (" << *loc_ << "):\n" << sem_output;
}


//--------------------------------------------------------------------------------------------------
// GRANT Permission Statement.

// TODO (Bristy) : Move this into commn/util.
const std::map<std::string, PermissionType >  PTGrantRevokePermission::kPermissionMap = {
    {"all", PermissionType::ALL_PERMISSION },
    {"alter", PermissionType::ALTER_PERMISSION},
    {"create", PermissionType::CREATE_PERMISSION},
    {"drop", PermissionType::DROP_PERMISSION },
    {"select", PermissionType::SELECT_PERMISSION},
    {"modify", PermissionType::MODIFY_PERMISSION},
    {"authorize", PermissionType::AUTHORIZE_PERMISSION},
    {"describe", PermissionType::DESCRIBE_PERMISSION}
};

//--------------------------------------------------------------------------------------------------

PTGrantRevokePermission::PTGrantRevokePermission(MemoryContext* memctx,
                                                 YBLocation::SharedPtr loc,
                                                 GrantRevokeStatementType statement_type,
                                                 const MCSharedPtr<MCString>& permission_name,
                                                 const ResourceType& resource_type,
                                                 const PTQualifiedName::SharedPtr& resource_name,
                                                 const PTQualifiedName::SharedPtr& role_name)
    : TreeNode(memctx, loc),
      statement_type_(statement_type),
      permission_name_(permission_name),
      complete_resource_name_(resource_name),
      role_name_(role_name),
      resource_type_(resource_type) {
}

PTGrantRevokePermission::~PTGrantRevokePermission() {
}

Status PTGrantRevokePermission::Analyze(SemContext* sem_context) {
  SemState sem_state(sem_context);
  RETURN_NOT_AUTH_ENABLED(sem_context);

  // We check for the existence of the resource in the catalog manager as
  // this should be a rare occurence.
  // TODO (Bristy): Should we use a cache for these checks?

  auto iterator = kPermissionMap.find(string(permission_name_->c_str()));
  if (iterator == kPermissionMap.end()) {
    return sem_context->Error(this, Substitute("Unknown Permission '$0'",
                                               permission_name_->c_str()).c_str(),
                              ErrorCode::SYNTAX_ERROR);
  }

  permission_ = iterator->second;

  // Check that the permission being granted is supported by the resource.
  // This check should be done before anything else.
  if (permission_ != PermissionType::ALL_PERMISSION &&
      !valid_permission_for_resource(permission_, resource_type_)) {
    // Match apache cassandra's error message.
    return sem_context->Error(loc(),
        "Resource type DataResource does not support any of the requested permissions",
        ErrorCode::SYNTAX_ERROR);
  }

  // Processing the role name.
  RETURN_NOT_OK(role_name_->AnalyzeName(sem_context, OBJECT_ROLE));
  switch (resource_type_) {
    case ResourceType::KEYSPACE: {
      if (complete_resource_name_->QLName() == common::kRedisKeyspaceName) {
        return sem_context->Error(loc(),
                                  strings::Substitute("$0 is a reserved keyspace name",
                                                      common::kRedisKeyspaceName).c_str(),
                                  ErrorCode::INVALID_ARGUMENTS);
      }
      RETURN_NOT_OK(sem_context->CheckHasKeyspacePermission(loc(),
          PermissionType::AUTHORIZE_PERMISSION,
          complete_resource_name_->ToTableName().namespace_name()));
      break;
    }
    case ResourceType::TABLE: {
      RETURN_NOT_OK(complete_resource_name_->AnalyzeName(sem_context, OBJECT_TABLE));
      RETURN_NOT_OK(sem_context->CheckHasTablePermission(loc(), AUTHORIZE_PERMISSION,
          complete_resource_name_->ToTableName()));
      break;
    }
    case ResourceType::ROLE: {
      RETURN_NOT_OK(complete_resource_name_->AnalyzeName(sem_context, OBJECT_ROLE));
      RETURN_NOT_OK(sem_context->CheckHasRolePermission(loc(), AUTHORIZE_PERMISSION,
          complete_resource_name_->QLName()));
      break;
    }
    case ResourceType::ALL_KEYSPACES: {
      RETURN_NOT_OK(sem_context->CheckHasAllKeyspacesPermission(loc(), AUTHORIZE_PERMISSION));
      break;
    }
    case ResourceType::ALL_ROLES:
      RETURN_NOT_OK(sem_context->CheckHasAllRolesPermission(loc(), AUTHORIZE_PERMISSION));
      break;
  }

  PrintSemanticAnalysisResult(sem_context);
  return Status::OK();
}


void PTGrantRevokePermission::PrintSemanticAnalysisResult(SemContext* sem_context) {
  MCString sem_output("\tGrant Permission ", sem_context->PTempMem());
  sem_output =  sem_output + " Permission : " + permission_name_->c_str();
  sem_output =  sem_output + " Resource : " + canonical_resource().c_str();
  sem_output =  sem_output + " Role : " + role_name()->QLName().c_str();
  VLOG(3) << "SEMANTIC ANALYSIS RESULT (" << *loc_ << "):\n" << sem_output;
}

}  // namespace ql
}  // namespace yb
