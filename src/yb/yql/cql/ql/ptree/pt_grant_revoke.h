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
// Tree node definitions for GRANT statement.
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_CQL_QL_PTREE_PT_GRANT_REVOKE_H_
#define YB_YQL_CQL_QL_PTREE_PT_GRANT_REVOKE_H_

#include "yb/common/roles_permissions.h"
#include "yb/common/schema.h"
#include "yb/common/common.pb.h"
#include "yb/master/master.pb.h"
#include "yb/util/enums.h"
#include "yb/yql/cql/ql/ptree/list_node.h"
#include "yb/yql/cql/ql/ptree/tree_node.h"
#include "yb/yql/cql/ql/ptree/pt_table_property.h"
#include "yb/yql/cql/ql/ptree/pt_type.h"
#include "yb/yql/cql/ql/ptree/pt_name.h"
#include "yb/yql/cql/ql/ptree/pt_update.h"

namespace yb {
namespace ql {

//--------------------------------------------------------------------------------------------------
// GRANT Role Statement.

class PTGrantRevokeRole : public TreeNode {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTGrantRevokeRole> SharedPtr;
  typedef MCSharedPtr<const PTGrantRevokeRole> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTGrantRevokeRole(MemoryContext* memctx, YBLocation::SharedPtr loc,
              GrantRevokeStatementType statement_type,
              const MCSharedPtr<MCString>& granted_role_name,
              const MCSharedPtr<MCString>& recipient_role_name);
  virtual ~PTGrantRevokeRole();

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTGrantRevokeRole;
  }

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTGrantRevokeRole::SharedPtr MakeShared(MemoryContext* memctx, TypeArgs&&... args) {
    return MCMakeShared<PTGrantRevokeRole>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  virtual CHECKED_STATUS Analyze(SemContext* sem_context) override;

  void PrintSemanticAnalysisResult(SemContext *sem_context);

  // Name of role that is being granted.
  std::string granted_role_name() const {
    return granted_role_name_->c_str();
  }

  // Name of role that is receiving the grant statement.
  std::string recipient_role_name() const {
    return recipient_role_name_->c_str();
  }

  // Type of statement: GRANT or REVOKE.
  GrantRevokeStatementType statement_type() const {
    return statement_type_;
  }

 private:
  const GrantRevokeStatementType statement_type_;
  const MCSharedPtr<MCString> granted_role_name_;
  const MCSharedPtr<MCString> recipient_role_name_;
};

//--------------------------------------------------------------------------------------------------
// GRANT Permission Statement.

class PTGrantRevokePermission : public TreeNode {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTGrantRevokePermission> SharedPtr;
  typedef MCSharedPtr<const PTGrantRevokePermission> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTGrantRevokePermission(MemoryContext* memctx, YBLocation::SharedPtr loc,
                          GrantRevokeStatementType statement_type,
                          const MCSharedPtr<MCString>& permission_name,
                          const ResourceType& resource_type,
                          const PTQualifiedName::SharedPtr& resource_name,
                          const PTQualifiedName::SharedPtr& role_name);
  virtual ~PTGrantRevokePermission();

  static const std::map<std::string, PermissionType> kPermissionMap;

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTGrantRevokePermission;
  }

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTGrantRevokePermission::SharedPtr MakeShared(MemoryContext* memctx,
                                                              TypeArgs&&... args) {
    return MCMakeShared<PTGrantRevokePermission>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;
  void PrintSemanticAnalysisResult(SemContext *sem_context);

  // Type of statement: GRANT or REVOKE.
  GrantRevokeStatementType statement_type() const {
    return statement_type_;
  }

  // Name of Role the permission is being granted to.
  const PTQualifiedName::SharedPtr role_name() const {
    return role_name_;
  }

  const ResourceType resource_type() const {
    return resource_type_;
  }

  // Name of resource, e.g. role_eng, table_salaries, keyspace_qa.
  const char* resource_name() const {
    if (resource_type_ == ResourceType::ALL_ROLES ||
        resource_type_ == ResourceType::ALL_KEYSPACES) {
      return nullptr;
    }
    return complete_resource_name_->last_name().c_str();
  }

  // Keyspace name for tables and keyspaces.
  const char* namespace_name() const {
    if (resource_type_ == ResourceType::TABLE || resource_type_ == ResourceType::KEYSPACE) {
      return complete_resource_name_->first_name().c_str();
    }
    return nullptr;
  }

  const PermissionType permission() const {
    return permission_;
  }

  const string canonical_resource() const  {
    std::string prefix = "";
    std::string suffix = "";
    if (complete_resource_name_ != nullptr) {
      if (resource_type_ == ResourceType::TABLE) {
        suffix = "/" + string(namespace_name());
      }
      suffix = suffix + "/" + string(resource_name());
    }

    if (resource_type_ == ResourceType::ALL_ROLES || resource_type_ == ResourceType::ROLE) {
      prefix = kRolesRoleResource;
    } else {
      prefix = kRolesDataResource;
    }
    std::string full_name = prefix + suffix;
    return full_name;
  }

 protected:
  const GrantRevokeStatementType statement_type_;
  // Just need an arbitrary default value here.
  PermissionType permission_ = PermissionType::ALL_PERMISSION;
  const MCSharedPtr<MCString> permission_name_;
  // complete_resource_name_ is nullptr for ALL KEYSPACES and ALL ROLES.
  const PTQualifiedName::SharedPtr complete_resource_name_;
  const PTQualifiedName::SharedPtr role_name_;
  const ResourceType resource_type_;
};

}  // namespace ql
}  // namespace yb

#endif  // YB_YQL_CQL_QL_PTREE_PT_GRANT_REVOKE_H_
