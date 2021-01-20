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

#include "yb/common/redis_constants_common.h"
#include "yb/common/ql_value.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master_defaults.h"
#include "yb/master/yql_keyspaces_vtable.h"

namespace yb {
namespace master {

YQLKeyspacesVTable::YQLKeyspacesVTable(const TableName& table_name,
                                       const NamespaceName& namespace_name,
                                       Master * const master)
    : YQLVirtualTable(table_name, namespace_name, master, CreateSchema()) {
}

Result<std::shared_ptr<QLRowBlock>> YQLKeyspacesVTable::RetrieveData(
    const QLReadRequestPB& request) const {
  auto vtable = std::make_shared<QLRowBlock>(schema_);
  std::vector<scoped_refptr<NamespaceInfo> > namespaces;
  master_->catalog_manager()->GetAllNamespaces(&namespaces, true);
  for (scoped_refptr<NamespaceInfo> ns : namespaces) {
    // Skip non-YQL namespace.
    if (!CatalogManager::IsYcqlNamespace(*ns)) {
      continue;
    }

    QLRow& row = vtable->Extend();
    RETURN_NOT_OK(SetColumnValue(kKeyspaceName, ns->name(), &row));
    RETURN_NOT_OK(SetColumnValue(kDurableWrites, true, &row));

    int repl_factor;
    RETURN_NOT_OK(master_->catalog_manager()->GetReplicationFactor(ns->name(), &repl_factor));
    RETURN_NOT_OK(SetColumnValue(kReplication, util::GetReplicationValue(repl_factor), &row));
  }

  return vtable;
}

Schema YQLKeyspacesVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn(kKeyspaceName, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kDurableWrites, QLType::Create(DataType::BOOL)));
  // TODO: replication needs to be a frozen map.
  CHECK_OK(builder.AddColumn(kReplication,
                             QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
