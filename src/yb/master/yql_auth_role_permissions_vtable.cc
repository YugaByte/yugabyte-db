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

#include "yb/master/catalog_manager.h"
#include "yb/master/permissions_manager.h"
#include "yb/master/master_defaults.h"
#include "yb/master/yql_auth_role_permissions_vtable.h"
#include "yb/common/common.pb.h"
#include "yb/common/roles_permissions.h"
#include "yb/gutil/strings/substitute.h"

namespace yb {
namespace master {

YQLAuthRolePermissionsVTable::YQLAuthRolePermissionsVTable(const TableName& table_name,
                                                           const NamespaceName& namespace_name,
                                                           Master* const master)
    : YQLVirtualTable(table_name, namespace_name, master, CreateSchema()) {
}

Result<std::shared_ptr<QLRowBlock>> YQLAuthRolePermissionsVTable::RetrieveData(
    const QLReadRequestPB& request) const {
  auto vtable = std::make_shared<QLRowBlock>(schema_);
  std::vector<scoped_refptr<RoleInfo>> roles;
  master_->catalog_manager()->permissions_manager()->GetAllRoles(&roles);
  for (const auto& rp : roles) {
    auto l = rp->LockForRead();
    const auto& pb = l->data().pb;
    for (const auto& resource : pb.resources()) {
      QLRow& row = vtable->Extend();
      RETURN_NOT_OK(SetColumnValue(kRole, pb.role(), &row));
      RETURN_NOT_OK(SetColumnValue(kResource, resource.canonical_resource(), &row));

      QLValuePB permissions;
      QLSeqValuePB* list_value = permissions.mutable_list_value();

      for (int j = 0; j < resource.permissions_size(); j++) {
        const auto& permission = resource.permissions(j);
        string permission_name  = PermissionName(permission);
        if (permission_name.empty()) {
          return STATUS(InvalidArgument,
                        strings::Substitute("Unknown Permission $0",
                                            PermissionType_Name(permission)));
        } else {
          (*list_value->add_elems()).set_string_value(permission_name);
        }
      }
      RETURN_NOT_OK(SetColumnValue(kPermissions, permissions, &row));

    }
  }

  return vtable;
}


Schema YQLAuthRolePermissionsVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn(kRole, DataType::STRING));
  CHECK_OK(builder.AddColumn(kResource, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kPermissions, QLType::CreateTypeList(DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
