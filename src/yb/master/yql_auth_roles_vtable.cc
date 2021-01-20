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
#include "yb/master/master_defaults.h"
#include "yb/master/yql_auth_roles_vtable.h"

namespace yb {
namespace master {

YQLAuthRolesVTable::YQLAuthRolesVTable(const TableName& table_name,
                                       const NamespaceName& namespace_name,
                                       Master * const master)
    : YQLVirtualTable(table_name, namespace_name, master, CreateSchema()) {
}

Result<std::shared_ptr<QLRowBlock>> YQLAuthRolesVTable::RetrieveData(
    const QLReadRequestPB& request) const {
  auto vtable = std::make_shared<QLRowBlock>(schema_);
  std::vector<scoped_refptr<RoleInfo>> roles;
  master_->catalog_manager()->permissions_manager()->GetAllRoles(&roles);
  for (const auto& role : roles) {
    auto l = role->LockForRead();
    const auto& pb = l->data().pb;
    QLRow& row = vtable->Extend();
    RETURN_NOT_OK(SetColumnValue(kRole, pb.role(), &row));
    RETURN_NOT_OK(SetColumnValue(kCanLogin, pb.can_login(), &row));
    RETURN_NOT_OK(SetColumnValue(kIsSuperuser, pb.is_superuser(), &row));

    QLValuePB members;
    QLSeqValuePB* list_value = members.mutable_list_value();

    for (const auto& member : pb.member_of()) {
      (*list_value->add_elems()).set_string_value(member);
    }
    RETURN_NOT_OK(SetColumnValue(kMemberOf, members, &row));
    if (pb.has_salted_hash()) {
        RETURN_NOT_OK(SetColumnValue(kSaltedHash, pb.salted_hash(), &row));
    }
  }

  return vtable;
}


Schema YQLAuthRolesVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn(kRole, DataType::STRING));
  CHECK_OK(builder.AddColumn(kCanLogin, QLType::Create(DataType::BOOL)));
  CHECK_OK(builder.AddColumn(kIsSuperuser, QLType::Create(DataType::BOOL)));
  CHECK_OK(builder.AddColumn(kMemberOf, QLType::CreateTypeList(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kSaltedHash, QLType::Create(DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
