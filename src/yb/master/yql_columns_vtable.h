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

#ifndef YB_MASTER_YQL_COLUMNS_VTABLE_H
#define YB_MASTER_YQL_COLUMNS_VTABLE_H

#include "yb/master/master.h"
#include "yb/master/yql_virtual_table.h"

namespace yb {
namespace master {

// VTable implementation of system_schema.columns.
class YQLColumnsVTable : public YQLVirtualTable {
 public:
  explicit YQLColumnsVTable(const TableName& table_name,
                            const NamespaceName& namespace_name,
                            Master * const master);
  Result<std::shared_ptr<QLRowBlock>> RetrieveData(const QLReadRequestPB& request) const override;

 protected:
  Schema CreateSchema() const;
 private:
  CHECKED_STATUS PopulateColumnInformation(const Schema& schema,
                                           const std::string& keyspace_name,
                                           const std::string& table_name,
                                           const size_t col_idx,
                                           QLRow* const row) const;
  static constexpr const char* const kKeyspaceName = "keyspace_name";
  static constexpr const char* const kTableName = "table_name";
  static constexpr const char* const kColumnName = "column_name";
  static constexpr const char* const kClusteringOrder = "clustering_order";
  static constexpr const char* const kColumnNameBytes = "column_name_bytes";
  static constexpr const char* const kKind = "kind";
  static constexpr const char* const kPosition = "position";
  static constexpr const char* const kType = "type";
};

}  // namespace master
}  // namespace yb
#endif // YB_MASTER_YQL_COLUMNS_VTABLE_H
