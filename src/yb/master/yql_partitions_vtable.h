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

#ifndef YB_MASTER_YQL_PARTITIONS_VTABLE_H
#define YB_MASTER_YQL_PARTITIONS_VTABLE_H

#include "yb/master/master.h"
#include "yb/master/yql_virtual_table.h"

namespace yb {
namespace master {

// VTable implementation of system.partitions.
class YQLPartitionsVTable : public YQLVirtualTable {
 public:
  explicit YQLPartitionsVTable(const TableName& table_name,
                               const NamespaceName& namespace_name,
                               Master* const master);
  Result<std::shared_ptr<QLRowBlock>> RetrieveData(const QLReadRequestPB& request) const;
  Status GenerateAndCacheData() const;
 protected:
  Schema CreateSchema() const;

  mutable boost::shared_mutex mutex_;
  mutable std::shared_ptr<QLRowBlock> cache_;
  mutable int cached_tablets_version_ = -1;
  mutable int cached_tablet_locations_version_ = -1;
};

}  // namespace master
}  // namespace yb
#endif // YB_MASTER_YQL_PARTITIONS_VTABLE_H
