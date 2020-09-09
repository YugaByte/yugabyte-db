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

#include "yb/docdb/pgsql_operation.h"

#include <boost/optional/optional_io.hpp>

#include "yb/common/partition.h"
#include "yb/common/ql_storage_interface.h"
#include "yb/common/ql_value.h"
#include "yb/common/pg_system_attr.h"

#include "yb/docdb/doc_pgsql_scanspec.h"
#include "yb/docdb/doc_rowwise_iterator.h"
#include "yb/docdb/primitive_value_util.h"

#include "yb/util/flag_tags.h"
#include "yb/util/scope_exit.h"
#include "yb/util/trace.h"

#include "yb/yql/pggate/util/pg_doc_data.h"

DECLARE_bool(trace_docdb_calls);
DECLARE_bool(ysql_disable_index_backfill);

DEFINE_double(ysql_scan_timeout_multiplier, 0.5,
              "YSQL read scan timeout multipler of retryable_rpc_single_call_timeout_ms.");

DEFINE_test_flag(int32, slowdown_pgsql_aggregate_read_ms, 0,
                 "If set > 0, slows down the response to pgsql aggregate read by this amount.");

namespace yb {
namespace docdb {

namespace {

CHECKED_STATUS CreateProjection(const Schema& schema,
                                const PgsqlColumnRefsPB& column_refs,
                                Schema* projection) {
  // Create projection of non-primary key columns. Primary key columns are implicitly read by DocDB.
  // It will also sort the columns before scanning.
  vector<ColumnId> column_ids;
  column_ids.reserve(column_refs.ids_size());
  for (int32_t id : column_refs.ids()) {
    const ColumnId column_id(id);
    if (!schema.is_key_column(column_id)) {
      column_ids.emplace_back(column_id);
    }
  }
  return schema.CreateProjectionByIdsIgnoreMissing(column_ids, projection);
}

} // namespace

//--------------------------------------------------------------------------------------------------

Status PgsqlWriteOperation::Init(PgsqlWriteRequestPB* request, PgsqlResponsePB* response) {
  // Initialize operation inputs.
  request_.Swap(request);
  response_ = response;

  // Init DocDB key using either ybctid or partition and range values.
  if (request_.has_ybctid_column_value()) {
    const string& ybctid = request_.ybctid_column_value().value().binary_value();
    SCHECK(!ybctid.empty(), InternalError, "empty ybctid");
    doc_key_.emplace(schema_);
    RETURN_NOT_OK(doc_key_->DecodeFrom(ybctid));
  } else {
    vector<PrimitiveValue> hashed_components;
    vector<PrimitiveValue> range_components;
    RETURN_NOT_OK(InitKeyColumnPrimitiveValues(request_.partition_column_values(),
                                               schema_,
                                               0,
                                               &hashed_components));
    RETURN_NOT_OK(InitKeyColumnPrimitiveValues(request_.range_column_values(),
                                               schema_,
                                               schema_.num_hash_key_columns(),
                                               &range_components));
    if (hashed_components.empty()) {
      doc_key_.emplace(schema_, range_components);
    } else {
      doc_key_.emplace(schema_, request_.hash_code(), hashed_components, range_components);
    }
  }
  encoded_doc_key_ = doc_key_->EncodeAsRefCntPrefix();

  return Status::OK();
}

Status PgsqlWriteOperation::Apply(const DocOperationApplyData& data) {
  VLOG(4) << "Write, read time: " << data.read_time << ", txn: " << txn_op_context_;

  auto scope_exit = ScopeExit([this] {
    if (!result_buffer_.empty()) {
      NetworkByteOrder::Store64(result_buffer_.data(), result_rows_);
    }
  });

  switch (request_.stmt_type()) {
    case PgsqlWriteRequestPB::PGSQL_INSERT:
      return ApplyInsert(data, IsUpsert::kFalse);

    case PgsqlWriteRequestPB::PGSQL_UPDATE:
      return ApplyUpdate(data);

    case PgsqlWriteRequestPB::PGSQL_DELETE:
      return ApplyDelete(data);

    case PgsqlWriteRequestPB::PGSQL_UPSERT: {
      // Upserts should not have column refs (i.e. require read).
      DSCHECK(!request_.has_column_refs() || request_.column_refs().ids().empty(),
              IllegalState,
              "Upsert operation should not have column references");
      return ApplyInsert(data, IsUpsert::kTrue);
    }

    case PgsqlWriteRequestPB::PGSQL_TRUNCATE_COLOCATED:
      return ApplyTruncateColocated(data);
  }
  return Status::OK();
}

Status PgsqlWriteOperation::ApplyInsert(const DocOperationApplyData& data, IsUpsert is_upsert) {
  QLTableRow table_row;
  if (!is_upsert) {
    RETURN_NOT_OK(ReadColumns(data, &table_row));
    if (!table_row.IsEmpty()) {
      VLOG(4) << "Duplicate row: " << table_row.ToString();
      // Primary key or unique index value found.
      response_->set_status(PgsqlResponsePB::PGSQL_STATUS_DUPLICATE_KEY_ERROR);
      response_->set_error_message("Duplicate key found in primary key or unique index");
      return Status::OK();
    }
  }

  // Add the liveness column.
  static const PrimitiveValue kLivenessColumnId =
      PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn);

  RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
      DocPath(encoded_doc_key_.as_slice(), kLivenessColumnId),
      Value(PrimitiveValue()),
      data.read_time, data.deadline, request_.stmt_id()));

  for (const auto& column_value : request_.column_values()) {
    // Get the column.
    if (!column_value.has_column_id()) {
      return STATUS(InternalError, "column id missing", column_value.DebugString());
    }
    const ColumnId column_id(column_value.column_id());
    const ColumnSchema& column = VERIFY_RESULT(schema_.column_by_id(column_id));

    // Check column-write operator.
    CHECK(GetTSWriteInstruction(column_value.expr()) == bfpg::TSOpcode::kScalarInsert)
      << "Illegal write instruction";

    // Evaluate column value.
    QLExprResult expr_result;
    RETURN_NOT_OK(EvalExpr(column_value.expr(), table_row, expr_result.Writer()));
    const SubDocument sub_doc =
        SubDocument::FromQLValuePB(expr_result.Value(), column.sorting_type());

    // Inserting into specified column.
    DocPath sub_path(encoded_doc_key_.as_slice(), PrimitiveValue(column_id));
    RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
        sub_path, sub_doc, data.read_time, data.deadline, request_.stmt_id()));
  }

  RETURN_NOT_OK(PopulateResultSet(table_row));

  response_->set_status(PgsqlResponsePB::PGSQL_STATUS_OK);
  return Status::OK();
}

Status PgsqlWriteOperation::ApplyUpdate(const DocOperationApplyData& data) {
  QLTableRow table_row;
  RETURN_NOT_OK(ReadColumns(data, &table_row));
  if (table_row.IsEmpty()) {
    // Row not found.
    response_->set_skipped(true);
    return Status::OK();
  }

  // skipped is set to false if this operation produces some data to write.
  bool skipped = true;

  if (request_.has_ybctid_column_value()) {
    for (const auto& column_value : request_.column_new_values()) {
      // Get the column.
      if (!column_value.has_column_id()) {
        return STATUS(InternalError, "column id missing", column_value.DebugString());
      }
      const ColumnId column_id(column_value.column_id());
      const ColumnSchema& column = VERIFY_RESULT(schema_.column_by_id(column_id));

      // Check column-write operator.
      SCHECK(GetTSWriteInstruction(column_value.expr()) == bfpg::TSOpcode::kScalarInsert ||
             GetTSWriteInstruction(column_value.expr()) == bfpg::TSOpcode::kPgEvalExprCall,
             InternalError,
             "Unsupported DocDB Expression");

      // Evaluate column value.
      QLExprResult expr_result;
      RETURN_NOT_OK(EvalExpr(column_value.expr(), table_row, expr_result.Writer(), &schema_));

      // Inserting into specified column.
      const SubDocument sub_doc =
          SubDocument::FromQLValuePB(expr_result.Value(), column.sorting_type());

      DocPath sub_path(encoded_doc_key_.as_slice(), PrimitiveValue(column_id));
      RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
          sub_path, sub_doc, data.read_time, data.deadline, request_.stmt_id()));
      skipped = false;
    }
  } else {
    // This UPDATE is calling PGGATE directly without going thru PostgreSQL layer.
    // Keep it here as we might need it.

    // Very limited support for where expressions. Only used for updates to the sequences data
    // table.
    bool is_match = true;
    if (request_.has_where_expr()) {
      QLExprResult match;
      RETURN_NOT_OK(EvalExpr(request_.where_expr(), table_row, match.Writer()));
      is_match = match.Value().bool_value();
    }

    if (is_match) {
      for (const auto &column_value : request_.column_new_values()) {
        // Get the column.
        if (!column_value.has_column_id()) {
          return STATUS(InternalError, "column id missing", column_value.DebugString());
        }
        const ColumnId column_id(column_value.column_id());
        const ColumnSchema& column = VERIFY_RESULT(schema_.column_by_id(column_id));

        // Check column-write operator.
        CHECK(GetTSWriteInstruction(column_value.expr()) == bfpg::TSOpcode::kScalarInsert)
        << "Illegal write instruction";

        // Evaluate column value.
        QLExprResult expr_result;
        RETURN_NOT_OK(EvalExpr(column_value.expr(), table_row, expr_result.Writer()));

        const SubDocument sub_doc =
            SubDocument::FromQLValuePB(expr_result.Value(), column.sorting_type());

        // Inserting into specified column.
        DocPath sub_path(encoded_doc_key_.as_slice(), PrimitiveValue(column_id));
        RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
            sub_path, sub_doc, data.read_time, data.deadline, request_.stmt_id()));
        skipped = false;
      }
    }
  }

  // Returning the values before the update.
  RETURN_NOT_OK(PopulateResultSet(table_row));

  if (skipped) {
    response_->set_skipped(true);
  }
  response_->set_rows_affected_count(1);
  response_->set_status(PgsqlResponsePB::PGSQL_STATUS_OK);
  return Status::OK();
}

Status PgsqlWriteOperation::ApplyDelete(const DocOperationApplyData& data) {
  int num_deleted = 1;
  QLTableRow table_row;
  RETURN_NOT_OK(ReadColumns(data, &table_row));
  if (table_row.IsEmpty()) {
    // Row not found.
    response_->set_skipped(true);
    // Return early unless we still want to apply the delete for backfill purposes.  Deletes to
    // nonexistent rows are expected to get written to the index when the index has the delete
    // permission during an online schema migration.
    // TODO(jason): apply deletes only when this is an index table going through a schema migration,
    // not just when backfill is enabled.
    if (FLAGS_ysql_disable_index_backfill) {
      return Status::OK();
    } else {
      num_deleted = 0;
    }
  }

  // TODO(neil) Add support for WHERE clause.
  CHECK(request_.column_values_size() == 0) << "WHERE clause condition is not yet fully supported";

  // Otherwise, delete the referenced row (all columns).
  RETURN_NOT_OK(data.doc_write_batch->DeleteSubDoc(DocPath(
      encoded_doc_key_.as_slice()), data.read_time, data.deadline));

  RETURN_NOT_OK(PopulateResultSet(table_row));

  response_->set_rows_affected_count(num_deleted);
  response_->set_status(PgsqlResponsePB::PGSQL_STATUS_OK);
  return Status::OK();
}

Status PgsqlWriteOperation::ApplyTruncateColocated(const DocOperationApplyData& data) {
  RETURN_NOT_OK(data.doc_write_batch->DeleteSubDoc(DocPath(
      encoded_doc_key_.as_slice()), data.read_time, data.deadline));
  response_->set_status(PgsqlResponsePB::PGSQL_STATUS_OK);
  return Status::OK();
}

Status PgsqlWriteOperation::ReadColumns(const DocOperationApplyData& data,
                                        QLTableRow* table_row) {
  // Filter the columns using primary key.
  if (doc_key_) {
    Schema projection;
    RETURN_NOT_OK(CreateProjection(schema_, request_.column_refs(), &projection));
    DocPgsqlScanSpec spec(projection, request_.stmt_id(), *doc_key_);
    DocRowwiseIterator iterator(projection,
                                schema_,
                                txn_op_context_,
                                data.doc_write_batch->doc_db(),
                                data.deadline,
                                data.read_time);
    RETURN_NOT_OK(iterator.Init(spec));
    if (VERIFY_RESULT(iterator.HasNext())) {
      RETURN_NOT_OK(iterator.NextRow(table_row));
    } else {
      table_row->Clear();
    }
    data.restart_read_ht->MakeAtLeast(iterator.RestartReadHt());
  }

  return Status::OK();
}

Status PgsqlWriteOperation::PopulateResultSet(const QLTableRow& table_row) {
  if (result_buffer_.empty()) {
    // Reserve space for num rows.
    pggate::PgWire::WriteInt64(0, &result_buffer_);
  }
  ++result_rows_;
  int rscol_index = 0;
  for (const PgsqlExpressionPB& expr : request_.targets()) {
    if (expr.has_column_id()) {
      QLExprResult value;
      if (expr.column_id() == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
        // Strip cotable id / pgtable id from the serialized DocKey before returning it as ybctid.
        Slice tuple_id = encoded_doc_key_.as_slice();
        if (tuple_id.starts_with(ValueTypeAsChar::kTableId)) {
          tuple_id.remove_prefix(1 + kUuidSize);
        } else if (tuple_id.starts_with(ValueTypeAsChar::kPgTableOid)) {
          tuple_id.remove_prefix(1 + sizeof(PgTableOid));
        }
        value.Writer().NewValue().set_binary_value(tuple_id.data(), tuple_id.size());
      } else {
        RETURN_NOT_OK(EvalExpr(expr, table_row, value.Writer()));
      }
      RETURN_NOT_OK(pggate::WriteColumn(value.Value(), &result_buffer_));
    }
    rscol_index++;
  }
  return Status::OK();
}

Status PgsqlWriteOperation::GetDocPaths(GetDocPathsMode mode,
                                        DocPathsToLock *paths,
                                        IsolationLevel *level) const {
  // When this write operation requires a read, it requires a read snapshot so paths will be locked
  // in snapshot isolation for consistency. Otherwise, pure writes will happen in serializable
  // isolation so that they will serialize but do not conflict with one another.
  //
  // Currently, only keys that are being written are locked, no lock is taken on read at the
  // snapshot isolation level.
  *level = RequireReadSnapshot() ? IsolationLevel::SNAPSHOT_ISOLATION
                                 : IsolationLevel::SERIALIZABLE_ISOLATION;

  if (mode == GetDocPathsMode::kIntents) {
    const google::protobuf::RepeatedPtrField<PgsqlColumnValuePB>* column_values = nullptr;
    if (request_.stmt_type() == PgsqlWriteRequestPB::PGSQL_INSERT ||
        request_.stmt_type() == PgsqlWriteRequestPB::PGSQL_UPSERT) {
      column_values = &request_.column_values();
    } else if (request_.stmt_type() == PgsqlWriteRequestPB::PGSQL_UPDATE) {
      column_values = &request_.column_new_values();
    }

    if (column_values != nullptr && !column_values->empty()) {
      KeyBytes buffer;
      for (const auto& column_value : *column_values) {
        ColumnId column_id(column_value.column_id());
        Slice doc_key = encoded_doc_key_.as_slice();
        buffer.Clear();
        buffer.AppendValueType(ValueType::kColumnId);
        buffer.AppendColumnId(column_id);
        RefCntBuffer path(doc_key.size() + buffer.size());
        memcpy(path.data(), doc_key.data(), doc_key.size());
        buffer.AsSlice().CopyTo(path.data() + doc_key.size());
        paths->push_back(RefCntPrefix(path));
      }
      return Status::OK();
    }
  }
  if (encoded_doc_key_) {
    paths->push_back(encoded_doc_key_);
  }

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Result<size_t> PgsqlReadOperation::Execute(const common::YQLStorageIf& ql_storage,
                                           CoarseTimePoint deadline,
                                           const ReadHybridTime& read_time,
                                           const Schema& schema,
                                           const Schema *index_schema,
                                           faststring *result_buffer,
                                           HybridTime *restart_read_ht) {
  size_t fetched_rows = 0;
  // Reserve space for fetched rows count.
  pggate::PgWire::WriteInt64(0, result_buffer);
  auto se = ScopeExit([&fetched_rows, result_buffer] {
    NetworkByteOrder::Store64(result_buffer->data(), fetched_rows);
  });
  VLOG(4) << "Read, read time: " << read_time << ", txn: " << txn_op_context_;

  // Fetching data.
  bool has_paging_state = false;
  if (request_.batch_arguments_size() > 0) {
    if (request_.has_ybctid_column_value()) {
      fetched_rows = VERIFY_RESULT(ExecuteBatchYbctid(
          ql_storage, deadline, read_time, schema, result_buffer, restart_read_ht));
    } else {
      fetched_rows = VERIFY_RESULT(ExecuteBatch(ql_storage, deadline, read_time, schema,
                                                index_schema, result_buffer, restart_read_ht,
                                                &has_paging_state));
    }
  } else {
    fetched_rows = VERIFY_RESULT(ExecuteScalar(ql_storage, deadline, read_time, schema,
                                               index_schema, -1 /* batch_arg_index */,
                                               result_buffer, restart_read_ht, &has_paging_state));
  }

  if (FLAGS_trace_docdb_calls) {
    TRACE("Fetched $0 rows. $1 paging state", fetched_rows, (has_paging_state ? "No" : "Has"));
  }
  *restart_read_ht = table_iter_->RestartReadHt();
  return fetched_rows;
}

Result<size_t> PgsqlReadOperation::ExecuteScalar(const common::YQLStorageIf& ql_storage,
                                                 CoarseTimePoint deadline,
                                                 const ReadHybridTime& read_time,
                                                 const Schema& schema,
                                                 const Schema *index_schema,
                                                 int64_t batch_arg_index,
                                                 faststring *result_buffer,
                                                 HybridTime *restart_read_ht,
                                                 bool *has_paging_state) {
  *has_paging_state = false;

  size_t fetched_rows = 0;
  size_t row_count_limit = std::numeric_limits<std::size_t>::max();
  if (request_.has_limit()) {
    if (request_.limit() == 0) {
      return fetched_rows;
    }
    row_count_limit = request_.limit();
  }

  // Create the projection of regular columns selected by the row block plus any referenced in
  // the WHERE condition. When DocRowwiseIterator::NextRow() populates the value map, it uses this
  // projection only to scan sub-documents. The query schema is used to select only referenced
  // columns and key columns.
  Schema projection;
  Schema index_projection;
  common::YQLRowwiseIteratorIf *iter;
  const Schema* scan_schema;

  RETURN_NOT_OK(CreateProjection(schema, request_.column_refs(), &projection));
  RETURN_NOT_OK(ql_storage.GetIterator(request_, batch_arg_index,
                                       projection, schema, txn_op_context_,
                                       deadline, read_time, &table_iter_));

  ColumnId ybbasectid_id;
  if (request_.has_index_request()) {
    const PgsqlReadRequestPB& index_request = request_.index_request();
    RETURN_NOT_OK(CreateProjection(*index_schema, index_request.column_refs(), &index_projection));
    RETURN_NOT_OK(ql_storage.GetIterator(index_request, batch_arg_index,
                                         index_projection, *index_schema,
                                         txn_op_context_, deadline, read_time, &index_iter_));
    iter = index_iter_.get();
    const size_t idx = index_schema->find_column("ybidxbasectid");
    SCHECK_NE(idx, Schema::kColumnNotFound, Corruption, "ybidxbasectid not found in index schema");
    ybbasectid_id = index_schema->column_id(idx);
    scan_schema = index_schema;
  } else {
    iter = table_iter_.get();
    scan_schema = &schema;
  }

  if (FLAGS_trace_docdb_calls) {
    TRACE("Initialized iterator");
  }

  // Set scan start time.
  bool scan_time_exceeded = false;

  // Fetching data.
  int match_count = 0;
  QLTableRow row;
  while (fetched_rows < row_count_limit && VERIFY_RESULT(iter->HasNext()) &&
         !scan_time_exceeded) {

    row.Clear();

    // If there is an index request, fetch ybbasectid from the index and use it as ybctid
    // to fetch from the base table. Otherwise, fetch from the base table directly.
    if (request_.has_index_request()) {
      RETURN_NOT_OK(iter->NextRow(&row));
      const auto& tuple_id = row.GetValue(ybbasectid_id);
      SCHECK_NE(tuple_id, boost::none, Corruption, "ybbasectid not found in index row");
      if (!VERIFY_RESULT(table_iter_->SeekTuple(tuple_id->binary_value()))) {
        DocKey doc_key;
        RETURN_NOT_OK(doc_key.DecodeFrom(tuple_id->binary_value()));
        return STATUS_FORMAT(Corruption, "ybctid $0 not found in indexed table", doc_key);
      }
      row.Clear();
      RETURN_NOT_OK(table_iter_->NextRow(projection, &row));
    } else {
      RETURN_NOT_OK(iter->NextRow(projection, &row));
    }

    // Match the row with the where condition before adding to the row block.
    bool is_match = true;
    if (request_.has_where_expr()) {
      QLExprResult match;
      RETURN_NOT_OK(EvalExpr(request_.where_expr(), row, match.Writer()));
      is_match = match.Value().bool_value();
    }
    if (is_match) {
      match_count++;
      if (request_.is_aggregate()) {
        RETURN_NOT_OK(EvalAggregate(row));
      } else {
        RETURN_NOT_OK(PopulateResultSet(row, result_buffer));
        ++fetched_rows;
      }
    }

    // Check every row_count_limit matches whether we've exceeded our scan time.
    if (match_count % row_count_limit == 0) {
      scan_time_exceeded = CoarseMonoClock::now() >= deadline;
    }
  }

  if (request_.is_aggregate() && match_count > 0) {
    RETURN_NOT_OK(PopulateAggregate(row, result_buffer));
    ++fetched_rows;
  }

  if (PREDICT_FALSE(FLAGS_TEST_slowdown_pgsql_aggregate_read_ms > 0) && request_.is_aggregate()) {
    TRACE("Sleeping for $0 ms", FLAGS_TEST_slowdown_pgsql_aggregate_read_ms);
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_slowdown_pgsql_aggregate_read_ms));
  }

  RETURN_NOT_OK(SetPagingStateIfNecessary(iter, fetched_rows, row_count_limit, scan_time_exceeded,
                                          scan_schema, batch_arg_index, has_paging_state));
  return fetched_rows;
}

Result<size_t> PgsqlReadOperation::ExecuteBatch(const common::YQLStorageIf& ql_storage,
                                                CoarseTimePoint deadline,
                                                const ReadHybridTime& read_time,
                                                const Schema& schema,
                                                const Schema *index_schema,
                                                faststring *result_buffer,
                                                HybridTime *restart_read_ht,
                                                bool *has_paging_state) {
  size_t fetched_rows = 0;
  bool exec_has_paging_state = false;

  int32_t batch_arg_count = request_.batch_arguments_size();
  int64_t batch_arg_index = 0;
  while (!exec_has_paging_state && batch_arg_index < batch_arg_count) {
    fetched_rows += VERIFY_RESULT(ExecuteScalar(ql_storage, deadline, read_time, schema,
                                                index_schema, batch_arg_index, result_buffer,
                                                restart_read_ht, &exec_has_paging_state));
    batch_arg_index++;
  }
  *has_paging_state = exec_has_paging_state;

  response_.set_batch_arg_count(batch_arg_index);
  return fetched_rows;
}

Result<size_t> PgsqlReadOperation::ExecuteBatchYbctid(const common::YQLStorageIf& ql_storage,
                                                      CoarseTimePoint deadline,
                                                      const ReadHybridTime& read_time,
                                                      const Schema& schema,
                                                      faststring *result_buffer,
                                                      HybridTime *restart_read_ht) {
  Schema projection;
  RETURN_NOT_OK(CreateProjection(schema, request_.column_refs(), &projection));

  QLTableRow row;
  size_t row_count = 0;
  for (const PgsqlBatchArgumentPB& batch_argument : request_.batch_arguments()) {
    // Get the row.
    RETURN_NOT_OK(ql_storage.GetIterator(request_, projection, schema, txn_op_context_,
                                         deadline, read_time, batch_argument.ybctid().value(),
                                         &table_iter_));
    row.Clear();

    SCHECK(VERIFY_RESULT(table_iter_->HasNext()), Corruption,
           "Given ybctid is not associated with any row in table");
    RETURN_NOT_OK(table_iter_->NextRow(projection, &row));

    // Populate result set.
    RETURN_NOT_OK(PopulateResultSet(row, result_buffer));
    row_count++;
  }

  // Set status for this batch.
  response_.set_batch_arg_count(row_count);

  return row_count;
}

Status PgsqlReadOperation::SetPagingStateIfNecessary(const common::YQLRowwiseIteratorIf* iter,
                                                     size_t fetched_rows,
                                                     const size_t row_count_limit,
                                                     const bool scan_time_exceeded,
                                                     const Schema* schema,
                                                     int64_t batch_arg_index,
                                                     bool *has_paging_state) {
  *has_paging_state = false;
  if (!request_.return_paging_state()) {
    return Status::OK();
  }

  // Set the paging state for next row.
  if (fetched_rows >= row_count_limit || scan_time_exceeded) {
    SubDocKey next_row_key;
    RETURN_NOT_OK(iter->GetNextReadSubDocKey(&next_row_key));
    // When the "limit" number of rows are returned and we are asked to return the paging state,
    // return the partition key and row key of the next row to read in the paging state if there are
    // still more rows to read. Otherwise, leave the paging state empty which means we are done
    // reading from this tablet.
    if (!next_row_key.doc_key().empty()) {
      const auto& keybytes = next_row_key.Encode();
      PgsqlPagingStatePB* paging_state = response_.mutable_paging_state();
      DSCHECK(schema != nullptr, IllegalState, "Missing schema");
      if (schema->num_hash_key_columns() > 0) {
        paging_state->set_next_partition_key(
           PartitionSchema::EncodeMultiColumnHashValue(next_row_key.doc_key().hash()));
      } else {
        paging_state->set_next_partition_key(keybytes.ToStringBuffer());
      }
      paging_state->set_next_row_key(keybytes.ToStringBuffer());
      *has_paging_state = true;
    }
  }

  return Status::OK();
}

Status PgsqlReadOperation::PopulateResultSet(const QLTableRow& table_row,
                                             faststring *result_buffer) {
  QLExprResult result;
  for (const PgsqlExpressionPB& expr : request_.targets()) {
    RETURN_NOT_OK(EvalExpr(expr, table_row, result.Writer()));
    RETURN_NOT_OK(pggate::WriteColumn(result.Value(), result_buffer));
  }
  return Status::OK();
}

Status PgsqlReadOperation::GetTupleId(QLValue *result) const {
  // Get row key and save to QLValue.
  // TODO(neil) Check if we need to append a table_id and other info to TupleID. For example, we
  // might need info to make sure the TupleId by itself is a valid reference to a specific row of
  // a valid table.
  const Slice tuple_id = VERIFY_RESULT(table_iter_->GetTupleId());
  result->set_binary_value(tuple_id.data(), tuple_id.size());
  return Status::OK();
}

Status PgsqlReadOperation::EvalAggregate(const QLTableRow& table_row) {
  if (aggr_result_.empty()) {
    int column_count = request_.targets().size();
    aggr_result_.resize(column_count);
  }

  int aggr_index = 0;
  for (const PgsqlExpressionPB& expr : request_.targets()) {
    RETURN_NOT_OK(EvalExpr(expr, table_row, aggr_result_[aggr_index++].Writer()));
  }
  return Status::OK();
}

Status PgsqlReadOperation::PopulateAggregate(const QLTableRow& table_row,
                                             faststring *result_buffer) {
  int column_count = request_.targets().size();
  for (int rscol_index = 0; rscol_index < column_count; rscol_index++) {
    RETURN_NOT_OK(pggate::WriteColumn(aggr_result_[rscol_index].Value(), result_buffer));
  }
  return Status::OK();
}

Status PgsqlReadOperation::GetPartitionIntent(
    const Schema& schema,
    const google::protobuf::RepeatedPtrField<PgsqlExpressionPB> &column_values,
    KeyValueWriteBatchPB* out) {
  auto pair = out->mutable_read_pairs()->Add();

  std::vector<PrimitiveValue> hashed_components;
  RETURN_NOT_OK(InitKeyColumnPrimitiveValues(
      column_values, schema, 0 /* start_idx */, &hashed_components));

  DocKey doc_key(schema, request_.hash_code(), hashed_components);
  pair->set_key(doc_key.Encode().ToStringBuffer());
  pair->set_value(std::string(1, ValueTypeAsChar::kNullLow));

  return Status::OK();
}

Status PgsqlReadOperation::GetIntents(const Schema& schema, KeyValueWriteBatchPB* out) {
  if (request_.partition_column_values().empty()) {
    // Empty components mean that we don't have primary key at all, but request
    // could still contain hash_code as part of tablet routing.
    // So we should ignore it.
    DocKey doc_key(schema);
    auto pair = out->mutable_read_pairs()->Add();
    pair->set_key(doc_key.Encode().ToStringBuffer());
    pair->set_value(std::string(1, ValueTypeAsChar::kNullLow));
    return Status::OK();
  }

  // Use "true" condition as DocDB only supports scalar argument currently.
  if (true) {
    // Executing scalar argument.
    return GetPartitionIntent(schema, request_.partition_column_values(), out);

  } else {
    // Executing batch argument.
    // Currently, this code is still an experiment for executing requests in parallel.
    // NOTE: Batch arguments are used for parallelism execution by partitions, so the partition
    //       field must be present in each batch_argument.
    DCHECK_GT(request_.batch_arguments_size(), 0) << "Batch argument was not provided";

    for (const PgsqlBatchArgumentPB& batch_argument : request_.batch_arguments()) {
      DCHECK_GT(batch_argument.partition_column_values_size(), 0);
      RETURN_NOT_OK(GetPartitionIntent(schema, batch_argument.partition_column_values(), out));
    }
  }

  return Status::OK();
}

}  // namespace docdb
}  // namespace yb
