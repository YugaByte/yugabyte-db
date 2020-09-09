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

#ifndef YB_DOCDB_PGSQL_OPERATION_H
#define YB_DOCDB_PGSQL_OPERATION_H

#include "yb/common/ql_rowwise_iterator_interface.h"

#include "yb/docdb/doc_expr.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_operation.h"

namespace yb {

class IndexInfo;

namespace common {

class YQLStorageIf;

}

namespace docdb {

YB_STRONGLY_TYPED_BOOL(IsUpsert);

class PgsqlWriteOperation :
    public DocOperationBase<DocOperationType::PGSQL_WRITE_OPERATION, PgsqlWriteRequestPB>,
    public DocExprExecutor {
 public:
  PgsqlWriteOperation(const Schema& schema,
                      const TransactionOperationContextOpt& txn_op_context)
      : schema_(schema),
        txn_op_context_(txn_op_context) {
  }

  // Initialize PgsqlWriteOperation. Content of request will be swapped out by the constructor.
  CHECKED_STATUS Init(PgsqlWriteRequestPB* request, PgsqlResponsePB* response);
  bool RequireReadSnapshot() const override {
    // For YSQL the the standard operations (INSERT/UPDATE/DELETE) will read/check the primary key.
    // We use UPSERT stmt type for specific requests when we can guarantee we can skip the read.
    return request_.stmt_type() != PgsqlWriteRequestPB::PGSQL_UPSERT;
  }

  const PgsqlWriteRequestPB& request() const { return request_; }
  PgsqlResponsePB* response() const { return response_; }

  const faststring& result_buffer() const { return result_buffer_; }

  bool result_is_single_empty_row() const {
    return result_rows_ == 1 && result_buffer_.size() == sizeof(int64_t);
  }

  // Execute write.
  CHECKED_STATUS Apply(const DocOperationApplyData& data) override;

 private:
  void ClearResponse() override {
    if (response_) {
      response_->Clear();
    }
  }

  // Insert, update, delete, and colocated truncate operations.
  CHECKED_STATUS ApplyInsert(
      const DocOperationApplyData& data, IsUpsert is_upsert = IsUpsert::kFalse);
  CHECKED_STATUS ApplyUpdate(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyDelete(const DocOperationApplyData& data);
  CHECKED_STATUS ApplyTruncateColocated(const DocOperationApplyData& data);

  CHECKED_STATUS DeleteRow(const DocPath& row_path, DocWriteBatch* doc_write_batch,
                           const ReadHybridTime& read_ht, CoarseTimePoint deadline);

  // Reading current row before operating on it.
  CHECKED_STATUS ReadColumns(const DocOperationApplyData& data,
                             QLTableRow* table_row);

  CHECKED_STATUS PopulateResultSet(const QLTableRow& table_row);

  // Reading path to operate on.
  CHECKED_STATUS GetDocPaths(GetDocPathsMode mode,
                             DocPathsToLock *paths,
                             IsolationLevel *level) const override;

  //------------------------------------------------------------------------------------------------
  // Context.
  const Schema& schema_;
  const TransactionOperationContextOpt txn_op_context_;

  // Input arguments.
  PgsqlResponsePB* response_ = nullptr;

  // TODO(neil) Output arguments.
  // UPDATE, DELETE, INSERT operations should return total number of new or changed rows.

  // Doc key and encoded doc key for the primary key.
  boost::optional<DocKey> doc_key_;
  RefCntPrefix encoded_doc_key_;

  // Rows result requested.
  int64_t result_rows_ = 0;
  faststring result_buffer_;
};

class PgsqlReadOperation : public DocExprExecutor {
 public:
  // Construct and access methods.
  PgsqlReadOperation(const PgsqlReadRequestPB& request,
                     const TransactionOperationContextOpt& txn_op_context)
      : request_(request), txn_op_context_(txn_op_context) {
  }

  const PgsqlReadRequestPB& request() const { return request_; }
  PgsqlResponsePB& response() { return response_; }

  // Driver of the execution for READ operators for the given conditions in Protobuf request.
  // The protobuf request carries two different types of arguments.
  // - Scalar argument: The query condition is represented by one set of values. For example, each
  //   of the following scalar protobuf requests will carry one "ybctid" (ROWID).
  //     SELECT ... WHERE ybctid = y1;
  //     SELECT ... WHERE ybctid = y2;
  //     SELECT ... WHERE ybctid = y3;
  //
  // - Batch argument: The query condition is representd by many sets of values. For example, a
  //   batch protobuf will carry many ybctids.
  //     SELECT ... WHERE ybctid IN (y1, y2, y3)
  Result<size_t> Execute(const common::YQLStorageIf& ql_storage,
                         CoarseTimePoint deadline,
                         const ReadHybridTime& read_time,
                         const Schema& schema,
                         const Schema *index_schema,
                         faststring *result_buffer,
                         HybridTime *restart_read_ht);

  CHECKED_STATUS GetTupleId(QLValue *result) const override;

  CHECKED_STATUS GetIntents(const Schema& schema, KeyValueWriteBatchPB* out);

 private:
  // Execute a READ operator for a given scalar argument.
  Result<size_t> ExecuteScalar(const common::YQLStorageIf& ql_storage,
                               CoarseTimePoint deadline,
                               const ReadHybridTime& read_time,
                               const Schema& schema,
                               const Schema *index_schema,
                               int64_t batch_arg_index,
                               faststring *result_buffer,
                               HybridTime *restart_read_ht,
                               bool *has_paging_state);

  // Execute a READ operator for a given batch of arguments.
  // - Currently, batch argument is used for only ybctid, a virtual columns.
  // - We haven't used batch of actual columns yet (hash, range, regular).
  Result<size_t> ExecuteBatch(const common::YQLStorageIf& ql_storage,
                              CoarseTimePoint deadline,
                              const ReadHybridTime& read_time,
                              const Schema& schema,
                              const Schema *index_schema,
                              faststring *result_buffer,
                              HybridTime *restart_read_ht,
                              bool *has_paging_state);

  // Execute a READ operator for a given batch of ybctids.
  Result<size_t> ExecuteBatchYbctid(const common::YQLStorageIf& ql_storage,
                                    CoarseTimePoint deadline,
                                    const ReadHybridTime& read_time,
                                    const Schema& schema,
                                    faststring *result_buffer,
                                    HybridTime *restart_read_ht);

  CHECKED_STATUS GetPartitionIntent(
      const Schema& schema,
      const google::protobuf::RepeatedPtrField<PgsqlExpressionPB> &column_values,
      KeyValueWriteBatchPB* out);

  CHECKED_STATUS PopulateResultSet(const QLTableRow& table_row,
                                   faststring *result_buffer);

  CHECKED_STATUS EvalAggregate(const QLTableRow& table_row);

  CHECKED_STATUS PopulateAggregate(const QLTableRow& table_row,
                                   faststring *result_buffer);

  // Checks whether we have processed enough rows for a page and sets the appropriate paging
  // state in the response object.
  CHECKED_STATUS SetPagingStateIfNecessary(const common::YQLRowwiseIteratorIf* iter,
                                           size_t fetched_rows,
                                           const size_t row_count_limit,
                                           const bool scan_time_exceeded,
                                           const Schema* schema,
                                           int64_t batch_arg_index,
                                           bool *has_paging_state);

  //------------------------------------------------------------------------------------------------
  const PgsqlReadRequestPB& request_;
  const TransactionOperationContextOpt txn_op_context_;
  PgsqlResponsePB response_;
  common::YQLRowwiseIteratorIf::UniPtr table_iter_;
  common::YQLRowwiseIteratorIf::UniPtr index_iter_;
};

}  // namespace docdb
}  // namespace yb

#endif // YB_DOCDB_PGSQL_OPERATION_H
