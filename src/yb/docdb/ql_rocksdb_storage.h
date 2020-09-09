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

#ifndef YB_DOCDB_QL_ROCKSDB_STORAGE_H
#define YB_DOCDB_QL_ROCKSDB_STORAGE_H

#include <boost/optional.hpp>

#include "yb/rocksdb/db.h"
#include "yb/common/ql_rowwise_iterator_interface.h"
#include "yb/common/ql_storage_interface.h"

#include "yb/docdb/doc_key.h"

namespace yb {
namespace docdb {

// Implementation of YQLStorageIf with rocksdb as a backend. This is what all of our QL tables use.
class QLRocksDBStorage : public common::YQLStorageIf {
 public:
  explicit QLRocksDBStorage(const DocDB& doc_db);

  //------------------------------------------------------------------------------------------------
  // CQL Support.
  CHECKED_STATUS GetIterator(const QLReadRequestPB& request,
                             const Schema& projection,
                             const Schema& schema,
                             const TransactionOperationContextOpt& txn_op_context,
                             CoarseTimePoint deadline,
                             const ReadHybridTime& read_time,
                             const common::QLScanSpec& spec,
                             std::unique_ptr<common::YQLRowwiseIteratorIf> *iter) const override;

  CHECKED_STATUS BuildYQLScanSpec(
      const QLReadRequestPB& request,
      const ReadHybridTime& read_time,
      const Schema& schema,
      bool include_static_columns,
      const Schema& static_projection,
      std::unique_ptr<common::QLScanSpec>* spec,
      std::unique_ptr<common::QLScanSpec>* static_row_spec) const override;

  //------------------------------------------------------------------------------------------------
  // PGSQL Support.
  CHECKED_STATUS CreateIterator(const Schema& projection,
                                const Schema& schema,
                                const TransactionOperationContextOpt& txn_op_context,
                                CoarseTimePoint deadline,
                                const ReadHybridTime& read_time,
                                common::YQLRowwiseIteratorIf::UniPtr* iter) const override;

  CHECKED_STATUS InitIterator(common::YQLRowwiseIteratorIf* doc_iter,
                              const PgsqlReadRequestPB& request,
                              const Schema& schema,
                              const QLValuePB& ybctid) const override;

  CHECKED_STATUS GetIterator(const PgsqlReadRequestPB& request,
                             int64_t batch_arg_index,
                             const Schema& projection,
                             const Schema& schema,
                             const TransactionOperationContextOpt& txn_op_context,
                             CoarseTimePoint deadline,
                             const ReadHybridTime& read_time,
                             common::YQLRowwiseIteratorIf::UniPtr* iter) const override;

  CHECKED_STATUS GetIterator(const PgsqlReadRequestPB& request,
                             const Schema& projection,
                             const Schema& schema,
                             const TransactionOperationContextOpt& txn_op_context,
                             CoarseTimePoint deadline,
                             const ReadHybridTime& read_time,
                             const QLValuePB& ybctid,
                             common::YQLRowwiseIteratorIf::UniPtr* iter) const override;

 private:
  const DocDB doc_db_;
};

}  // namespace docdb
}  // namespace yb
#endif // YB_DOCDB_QL_ROCKSDB_STORAGE_H
