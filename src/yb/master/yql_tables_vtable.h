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

#ifndef YB_MASTER_YQL_TABLES_VTABLE_H
#define YB_MASTER_YQL_TABLES_VTABLE_H

#include "yb/master/master.h"
#include "yb/master/yql_virtual_table.h"

namespace yb {
namespace master {

// VTable implementation of system_schema.tables.
class YQLTablesVTable : public YQLVirtualTable {
 public:
  explicit YQLTablesVTable(const TableName& table_name,
                           const NamespaceName& namespace_name,
                           Master * const master);
  Result<std::shared_ptr<QLRowBlock>> RetrieveData(const QLReadRequestPB& request) const override;
 protected:
  Schema CreateSchema() const;
 private:
  static constexpr const char* const kKeyspaceName = "keyspace_name";
  static constexpr const char* const kTableName = "table_name";
  static constexpr const char* const kBloomFilterChance = "bloom_filter_fp_chance";
  static constexpr const char* const kCaching = "caching";
  static constexpr const char* const kCdc = "cdc";
  static constexpr const char* const kComment = "comment";
  static constexpr const char* const kCompaction = "compaction";
  static constexpr const char* const kCompression = "compression";
  static constexpr const char* const kCrcCheck = "crc_check_chance";
  static constexpr const char* const kLocalReadRepair = "dclocal_read_repair_chance";
  static constexpr const char* const kDefaultTimeToLive = "default_time_to_live";
  static constexpr const char* const kExtensions = "extensions";
  static constexpr const char* const kFlags = "flags";
  static constexpr const char* const kGcGraceSeconds = "gc_grace_seconds";
  static constexpr const char* const kId = "id";
  static constexpr const char* const kMaxIndexInterval = "max_index_interval";
  static constexpr const char* const kMemTableFlushPeriod = "memtable_flush_period_in_ms";
  static constexpr const char* const kMinIndexInterval = "min_index_interval";
  static constexpr const char* const kReadRepairChance = "read_repair_chance";
  static constexpr const char* const kSpeculativeRetry = "speculative_retry";
  static constexpr const char* const kTransactions = "transactions";
};

}  // namespace master
}  // namespace yb
#endif // YB_MASTER_YQL_TABLES_VTABLE_H
