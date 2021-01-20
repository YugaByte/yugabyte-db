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

#ifndef YB_MASTER_YQL_EMPTY_VTABLE_H
#define YB_MASTER_YQL_EMPTY_VTABLE_H

#include "yb/common/schema.h"
#include "yb/master/yql_virtual_table.h"

namespace yb {
namespace master {

// Generic virtual table which we currently use for system tables that are empty. Although we are
// not sure when the class will be deleted since currently it does not look like we need to populate
// some system tables.
class YQLEmptyVTable : public YQLVirtualTable {
 public:
  explicit YQLEmptyVTable(const TableName& table_name,
                          const NamespaceName& namespace_name,
                          const Master* const master,
                          const Schema& schema);
  Result<std::shared_ptr<QLRowBlock>> RetrieveData(const QLReadRequestPB& request) const override;
};

}  // namespace master
}  // namespace yb

#endif // YB_MASTER_YQL_EMPTY_VTABLE_H
