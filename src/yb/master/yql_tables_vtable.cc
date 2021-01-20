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

#include "yb/master/yql_tables_vtable.h"

#include "yb/common/ql_value.h"
#include "yb/master/catalog_manager.h"

namespace yb {
namespace master {

YQLTablesVTable::YQLTablesVTable(const TableName& table_name,
                                 const NamespaceName& namespace_name,
                                 Master* const master)
    : YQLVirtualTable(table_name, namespace_name, master, CreateSchema()) {
}

Result<std::shared_ptr<QLRowBlock>> YQLTablesVTable::RetrieveData(
    const QLReadRequestPB& request) const {
  auto vtable = std::make_shared<QLRowBlock>(schema_);
  std::vector<scoped_refptr<TableInfo> > tables;
  master_->catalog_manager()->GetAllTables(&tables, true);
  for (scoped_refptr<TableInfo> table : tables) {

    // Skip non-YQL tables.
    if (!CatalogManager::IsYcqlTable(*table)) {
      continue;
    }

    // Skip index table.
    if (!table->indexed_table_id().empty()) {
      continue;
    }

    // Get namespace for table.
    NamespaceIdentifierPB nsId;
    nsId.set_id(table->namespace_id());
    scoped_refptr<NamespaceInfo> nsInfo;
    RETURN_NOT_OK(master_->catalog_manager()->FindNamespace(nsId, &nsInfo));

    // Create appropriate row for the table;
    QLRow& row = vtable->Extend();
    RETURN_NOT_OK(SetColumnValue(kKeyspaceName, nsInfo->name(), &row));
    RETURN_NOT_OK(SetColumnValue(kTableName, table->name(), &row));

    // Create appropriate flags entry.
    QLValuePB flags_elem;
    flags_elem.set_string_value("compound");
    QLValuePB flags_set;
    *flags_set.mutable_set_value()->add_elems() = flags_elem;
    RETURN_NOT_OK(SetColumnValue(kFlags, flags_set, &row));

    // Create appropriate table uuid entry.
    Uuid uuid;
    // Note: table id is in host byte order.
    RETURN_NOT_OK(uuid.FromHexString(table->id()));
    RETURN_NOT_OK(SetColumnValue(kId, uuid, &row));

    // Set the values for the table properties.
    Schema schema;
    RETURN_NOT_OK(table->GetSchema(&schema));

    // Adjusting precision, we use milliseconds internally, CQL uses seconds.
    // Sanity check, larger TTL values should be caught during analysis.
    DCHECK_LE(schema.table_properties().DefaultTimeToLive(),
              MonoTime::kMillisecondsPerSecond * std::numeric_limits<int32>::max());
    int32_t cql_ttl = static_cast<int32_t>(
        schema.table_properties().DefaultTimeToLive() / MonoTime::kMillisecondsPerSecond);
    RETURN_NOT_OK(SetColumnValue(kDefaultTimeToLive, cql_ttl, &row));

    QLValue txn;
    txn.set_map_value();
    txn.add_map_key()->set_string_value("enabled");
    txn.add_map_value()->set_string_value(schema.table_properties().is_transactional() ?
                                          "true" : "false");
    RETURN_NOT_OK(SetColumnValue(kTransactions, txn.value(), &row));
  }

  return vtable;
}

Schema YQLTablesVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn(kKeyspaceName, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddKeyColumn(kTableName, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kBloomFilterChance, QLType::Create(DataType::DOUBLE)));
  CHECK_OK(builder.AddColumn(kCaching, QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  CHECK_OK(builder.AddColumn(kCdc, QLType::Create(DataType::BOOL)));
  CHECK_OK(builder.AddColumn(kComment, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kCompaction,
                             QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  CHECK_OK(builder.AddColumn(kCompression,
                             QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  CHECK_OK(builder.AddColumn(kCrcCheck, QLType::Create(DataType::DOUBLE)));
  CHECK_OK(builder.AddColumn(kLocalReadRepair, QLType::Create(DataType::DOUBLE)));
  CHECK_OK(builder.AddColumn(kDefaultTimeToLive, QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn(kExtensions,
                             QLType::CreateTypeMap(DataType::STRING, DataType::BINARY)));
  CHECK_OK(builder.AddColumn(kFlags, QLType::CreateTypeSet(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kGcGraceSeconds, QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn(kId, QLType::Create(DataType::UUID)));
  CHECK_OK(builder.AddColumn(kMaxIndexInterval, QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn(kMemTableFlushPeriod, QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn(kMinIndexInterval, QLType::Create(DataType::INT32)));
  CHECK_OK(builder.AddColumn(kReadRepairChance, QLType::Create(DataType::DOUBLE)));
  CHECK_OK(builder.AddColumn(kSpeculativeRetry, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kTransactions,
                             QLType::CreateTypeMap(DataType::STRING, DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
