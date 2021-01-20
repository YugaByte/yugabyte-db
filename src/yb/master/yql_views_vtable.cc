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
#include "yb/master/yql_views_vtable.h"

namespace yb {
namespace master {

YQLViewsVTable::YQLViewsVTable(const TableName& table_name,
                               const NamespaceName& namespace_name,
                               Master * const master)
    : YQLEmptyVTable(table_name, namespace_name, master, CreateSchema()) {
}

Schema YQLViewsVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn("keyspace_name", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddKeyColumn("view_name", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn("base_table_id", QLType::Create(DataType::UUID)));
  CHECK_OK(builder.AddColumn("base_table_name", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn("bloom_filter_fp_chance", QLType::Create(DataType::DOUBLE)));
  // TODO: caching needs to be a frozen map.
  CHECK_OK(builder.AddColumn("caching",
                             QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  CHECK_OK(builder.AddColumn("cdc", QLType::Create(DataType::BOOL)));
  CHECK_OK(builder.AddColumn("comment", QLType::Create(DataType::STRING)));
  // TODO: compaction needs to be a frozen map.
  CHECK_OK(builder.AddColumn("compaction",
                             QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  // TODO: compression needs to be a frozen map.
  CHECK_OK(builder.AddColumn("compression",
                             QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  CHECK_OK(builder.AddColumn("crc_check_chance", QLType::Create(DataType::DOUBLE)));
  CHECK_OK(builder.AddColumn("dclocal_read_repair_chance", QLType::Create(DataType::DOUBLE)));
  CHECK_OK(builder.AddColumn("default_time_to_live", QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn("extensions",
                             QLType::CreateTypeMap(DataType::STRING, DataType::BINARY)));
  CHECK_OK(builder.AddColumn("gc_grace_seconds", QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn("id", QLType::Create(DataType::UUID)));
  CHECK_OK(builder.AddColumn("include_all_columns", QLType::Create(DataType::BOOL)));
  CHECK_OK(builder.AddColumn("max_index_interval", QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn("memtable_flush_period_in_ms", QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn("min_index_interval", QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn("read_repair_chance", QLType::Create(DataType::DOUBLE)));
  CHECK_OK(builder.AddColumn("speculative_retry", QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn("where_clause", QLType::Create(DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
