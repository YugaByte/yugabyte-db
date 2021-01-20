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

#include "yb/master/master_defaults.h"
#include "yb/master/yql_types_vtable.h"
#include "yb/master/catalog_manager.h"

namespace yb {
namespace master {

QLTypesVTable::QLTypesVTable(const TableName& table_name,
                             const NamespaceName& namespace_name,
                             Master* const master)
    : YQLVirtualTable(table_name, namespace_name, master, CreateSchema()) {
}

Result<std::shared_ptr<QLRowBlock>> QLTypesVTable::RetrieveData(
    const QLReadRequestPB& request) const {
  auto vtable = std::make_shared<QLRowBlock>(schema_);
  std::vector<scoped_refptr<UDTypeInfo> > types;
  master_->catalog_manager()->GetAllUDTypes(&types);

  for (scoped_refptr<UDTypeInfo> type : types) {
    // Get namespace for table.
    NamespaceIdentifierPB nsId;
    nsId.set_id(type->namespace_id());
    scoped_refptr<NamespaceInfo> nsInfo;
    RETURN_NOT_OK(master_->catalog_manager()->FindNamespace(nsId, &nsInfo));

    // Create appropriate row for the table;
    QLRow& row = vtable->Extend();
    RETURN_NOT_OK(SetColumnValue(kKeyspaceName, nsInfo->name(), &row));
    RETURN_NOT_OK(SetColumnValue(kTypeName, type->name(), &row));

    // Create appropriate field_names entry.
    QLValuePB field_names;
    QLSeqValuePB *list_value = field_names.mutable_list_value();
    for (int i = 0; i < type->field_names_size(); i++) {
      QLValuePB field_name;
      field_name.set_string_value(type->field_names(i));
      *list_value->add_elems() = field_name;
    }
    RETURN_NOT_OK(SetColumnValue(kFieldNames, field_names, &row));

    // Create appropriate field_types entry.
    QLValuePB field_types;
    list_value = field_types.mutable_list_value();
    for (int i = 0; i < type->field_types_size(); i++) {
      QLValuePB field_type;
      const string& field_type_name = QLType::FromQLTypePB(type->field_types(i))->ToString();
      field_type.set_string_value(field_type_name);
      *list_value->add_elems() = field_type;
    }
    RETURN_NOT_OK(SetColumnValue(kFieldTypes, field_types, &row));
  }

  return vtable;
}

Schema QLTypesVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn("keyspace_name", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddKeyColumn("type_name", QLType::Create(DataType::STRING)));
  // TODO: field_names should be a frozen list.
  CHECK_OK(builder.AddColumn("field_names", QLType::CreateTypeList(DataType::STRING)));
  // TODO: field_types should be a frozen list.
  CHECK_OK(builder.AddColumn("field_types", QLType::CreateTypeList(DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
