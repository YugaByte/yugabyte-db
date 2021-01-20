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
#include "yb/master/yql_aggregates_vtable.h"

namespace yb {
namespace master {

YQLAggregatesVTable::YQLAggregatesVTable(const TableName& table_name,
                                         const NamespaceName& namespace_name,
                                         Master* const master)
    : YQLEmptyVTable(table_name, namespace_name, master, CreateSchema()) {
}

Schema YQLAggregatesVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn("keyspace_name", DataType::STRING));
  CHECK_OK(builder.AddKeyColumn("aggregate_name", DataType::STRING));
  // TODO: argument_types should be part of the primary key, but since we don't support the CQL
  // 'frozen' type, we can't have collections in our primary key.
  CHECK_OK(builder.AddColumn("argument_types", QLType::CreateTypeList(DataType::STRING)));
  CHECK_OK(builder.AddColumn("final_func", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn("initcond", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn("state_func", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn("state_type", QLType::Create(DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
