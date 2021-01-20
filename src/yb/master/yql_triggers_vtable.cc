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
#include "yb/master/yql_triggers_vtable.h"

namespace yb {
namespace master {

YQLTriggersVTable::YQLTriggersVTable(const TableName& table_name,
                                     const NamespaceName& namespace_name,
                                     Master * const master)
    : YQLEmptyVTable(table_name, namespace_name, master, CreateSchema()) {
}

Schema YQLTriggersVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn("keyspace_name", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddKeyColumn("table_name", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddKeyColumn("trigger_name", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn("options",
                             QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
