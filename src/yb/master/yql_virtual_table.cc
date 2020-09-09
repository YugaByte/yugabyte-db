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

#include "yb/master/yql_virtual_table.h"

#include "yb/common/ql_value.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/ts_manager.h"
#include "yb/master/yql_vtable_iterator.h"
#include "yb/util/shared_lock.h"

namespace yb {
namespace master {

YQLVirtualTable::YQLVirtualTable(const TableName& table_name,
                                 const Master* const master,
                                 const Schema& schema)
    : master_(master),
      table_name_(table_name),
      schema_(schema) {
}

CHECKED_STATUS YQLVirtualTable::GetIterator(
    const QLReadRequestPB& request,
    const Schema& projection,
    const Schema& schema,
    const TransactionOperationContextOpt& txn_op_context,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time,
    const common::QLScanSpec& spec,
    std::unique_ptr<common::YQLRowwiseIteratorIf>* iter)
    const {
  // Acquire shared lock on catalog manager to verify it is still the leader and metadata will
  // not change.
  CatalogManager::ScopedLeaderSharedLock l(master_->catalog_manager());
  RETURN_NOT_OK(l.first_failed_status());

  iter->reset(new YQLVTableIterator(
      VERIFY_RESULT(RetrieveData(request)), request.hashed_column_values()));
  return Status::OK();
}

CHECKED_STATUS YQLVirtualTable::BuildYQLScanSpec(
    const QLReadRequestPB& request,
    const ReadHybridTime& read_time,
    const Schema& schema,
    const bool include_static_columns,
    const Schema& static_projection,
    std::unique_ptr<common::QLScanSpec>* spec,
    std::unique_ptr<common::QLScanSpec>* static_row_spec) const {
  // There should be no static columns in system tables so we are not handling it.
  if (include_static_columns) {
    return STATUS(IllegalState, "system table contains no static columns");
  }
  spec->reset(new common::QLScanSpec(
      request.has_where_expr() ? &request.where_expr().condition() : nullptr,
      request.has_if_expr() ? &request.if_expr().condition() : nullptr,
      request.is_forward_scan()));
  return Status::OK();
}

void YQLVirtualTable::GetSortedLiveDescriptors(std::vector<std::shared_ptr<TSDescriptor>>* descs)
    const {
  master_->ts_manager()->GetAllLiveDescriptors(descs);
  std::sort(
      descs->begin(), descs->end(),
      [](const std::shared_ptr<TSDescriptor>& a, const std::shared_ptr<TSDescriptor>& b) -> bool {
        return a->permanent_uuid() < b->permanent_uuid();
      });
}

const std::string kSystemTablesReleaseVersion = "3.9-SNAPSHOT";
const std::string kSystemTablesReleaseVersionColumn = "release_version";

}  // namespace master
}  // namespace yb
