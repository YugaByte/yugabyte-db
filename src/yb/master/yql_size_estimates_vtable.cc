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
#include "yb/master/yql_size_estimates_vtable.h"

#include "yb/rpc/messenger.h"

#include "yb/util/net/dns_resolver.h"
#include "yb/util/yb_partition.h"

namespace yb {
namespace master {

YQLSizeEstimatesVTable::YQLSizeEstimatesVTable(const Master* const master)
    : YQLVirtualTable(master::kSystemSizeEstimatesTableName, master, CreateSchema()) {
}

Result<std::shared_ptr<QLRowBlock>> YQLSizeEstimatesVTable::RetrieveData(
    const QLReadRequestPB& request) const {
  auto vtable = std::make_shared<QLRowBlock>(schema_);
  std::vector<scoped_refptr<TableInfo> > tables;
  CatalogManager* catalog_manager = master_->catalog_manager();
  catalog_manager->GetAllTables(&tables, true);

  for (scoped_refptr<TableInfo> table : tables) {
    Schema schema;
    RETURN_NOT_OK(table->GetSchema(&schema));

    // Get namespace for table.
    NamespaceIdentifierPB nsId;
    nsId.set_id(table->namespace_id());
    scoped_refptr<NamespaceInfo> nsInfo;
    RETURN_NOT_OK(master_->catalog_manager()->FindNamespace(nsId, &nsInfo));

    // Hide non-YQL tables.
    if (table->GetTableType() != TableType::YQL_TABLE_TYPE) {
      continue;
    }

    // Get tablets for table.
    std::vector<scoped_refptr<TabletInfo> > tablets;
    table->GetAllTablets(&tablets);
    for (const scoped_refptr<TabletInfo>& tablet : tablets) {
      TabletLocationsPB tabletLocationsPB;
      Status s = catalog_manager->GetTabletLocations(tablet->id(), &tabletLocationsPB);
      // Skip not-found tablets: they might not be running yet or have been deleted.
      if (!s.ok()) {
        continue;
      }

      QLRow &row = vtable->Extend();
      RETURN_NOT_OK(SetColumnValue(kKeyspaceName, nsInfo->name(), &row));
      RETURN_NOT_OK(SetColumnValue(kTableName, table->name(), &row));

      const PartitionPB &partition = tabletLocationsPB.partition();
      uint16_t yb_start_hash = !partition.partition_key_start().empty() ?
          PartitionSchema::DecodeMultiColumnHashValue(partition.partition_key_start()) : 0;
      string cql_start_hash = std::to_string(YBPartition::YBToCqlHashCode(yb_start_hash));
      RETURN_NOT_OK(SetColumnValue(kRangeStart, cql_start_hash, &row));

      uint16_t yb_end_hash = !partition.partition_key_end().empty() ?
          PartitionSchema::DecodeMultiColumnHashValue(partition.partition_key_end()) : 0;
      string cql_end_hash = std::to_string(YBPartition::YBToCqlHashCode(yb_end_hash));
      RETURN_NOT_OK(SetColumnValue(kRangeEnd, cql_end_hash, &row));

      // TODO: These values should eventually be reasonable estimates.
      // For now using 0 as defaults which should mean that clients will use their own defaults
      // (i.e. minimums) for number of splits -- typically one split per YugaByte tablet.

      // The estimated average size in bytes of all data for each partition (i.e. hash) key.
      RETURN_NOT_OK(SetColumnValue(kMeanPartitionSize, 0, &row));
      // The estimated number of partition (i.e. hash) keys in this tablet.
      RETURN_NOT_OK(SetColumnValue(kPartitionsCount, 0, &row));
    }
  }

  return vtable;
}

Schema YQLSizeEstimatesVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn(kKeyspaceName, DataType::STRING));
  CHECK_OK(builder.AddKeyColumn(kTableName, DataType::STRING));
  CHECK_OK(builder.AddKeyColumn(kRangeStart, DataType::STRING));
  CHECK_OK(builder.AddKeyColumn(kRangeEnd, DataType::STRING));
  CHECK_OK(builder.AddColumn(kMeanPartitionSize, DataType::INT64));
  CHECK_OK(builder.AddColumn(kPartitionsCount, DataType::INT64));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
