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

#include "yb/common/ql_resultset.h"

#include "yb/common/ql_value.h"

#include "yb/docdb/cql_operation.h"
#include "yb/docdb/pgsql_operation.h"

#include "yb/tablet/abstract_tablet.h"
#include "yb/util/trace.h"
#include "yb/yql/pggate/util/pg_doc_data.h"

namespace yb {
namespace tablet {

Status AbstractTablet::HandleQLReadRequest(CoarseTimePoint deadline,
                                           const ReadHybridTime& read_time,
                                           const QLReadRequestPB& ql_read_request,
                                           const TransactionOperationContextOpt& txn_op_context,
                                           QLReadRequestResult* result) {

  // TODO(Robert): verify that all key column values are provided
  docdb::QLReadOperation doc_op(ql_read_request, txn_op_context);

  // Form a schema of columns that are referenced by this query.
  const SchemaPtr schema = GetSchema();
  Schema projection;
  const QLReferencedColumnsPB& column_pbs = ql_read_request.column_refs();
  vector<ColumnId> column_refs;
  for (int32_t id : column_pbs.static_ids()) {
    column_refs.emplace_back(id);
  }
  for (int32_t id : column_pbs.ids()) {
    column_refs.emplace_back(id);
  }
  RETURN_NOT_OK(schema->CreateProjectionByIdsIgnoreMissing(column_refs, &projection));

  const QLRSRowDesc rsrow_desc(ql_read_request.rsrow_desc());
  QLResultSet resultset(&rsrow_desc, &result->rows_data);
  TRACE("Start Execute");
  const Status s = doc_op.Execute(
      QLStorage(), deadline, read_time, *schema, projection, &resultset, &result->restart_read_ht);
  TRACE("Done Execute");
  if (!s.ok()) {
    if (s.IsQLError()) {
      result->response.set_status(QLResponsePB::YQL_STATUS_USAGE_ERROR);
    } else {
      result->response.set_status(QLResponsePB::YQL_STATUS_RUNTIME_ERROR);
    }
    result->response.set_error_message(s.message().cdata(), s.message().size());
    return Status::OK();
  }
  result->response.Swap(&doc_op.response());

  RETURN_NOT_OK(CreatePagingStateForRead(
      ql_read_request, resultset.rsrow_count(), &result->response));

  result->response.set_status(QLResponsePB::YQL_STATUS_OK);
  return Status::OK();
}

Status AbstractTablet::HandlePgsqlReadRequest(CoarseTimePoint deadline,
                                              const ReadHybridTime& read_time,
                                              bool is_explicit_request_read_time,
                                              const PgsqlReadRequestPB& pgsql_read_request,
                                              const TransactionOperationContextOpt& txn_op_context,
                                              PgsqlReadRequestResult* result,
                                              size_t* num_rows_read) {

  docdb::PgsqlReadOperation doc_op(pgsql_read_request, txn_op_context);

  // Form a schema of columns that are referenced by this query.
  const SchemaPtr schema = GetSchema(pgsql_read_request.table_id());
  const SchemaPtr index_schema = pgsql_read_request.has_index_request()
      ? GetSchema(pgsql_read_request.index_request().table_id()) : nullptr;

  TRACE("Start Execute");
  auto fetched_rows = doc_op.Execute(
      QLStorage(), deadline, read_time, is_explicit_request_read_time, *schema, index_schema.get(),
      &result->rows_data, &result->restart_read_ht);
  TRACE("Done Execute");
  if (!fetched_rows.ok()) {
    result->response.set_status(PgsqlResponsePB::PGSQL_STATUS_RUNTIME_ERROR);
    const auto& s = fetched_rows.status();
    result->response.set_error_message(s.message().cdata(), s.message().size());
    return Status::OK();
  }
  result->response.Swap(&doc_op.response());

  if (num_rows_read) {
    *num_rows_read = *fetched_rows;
  }

  RETURN_NOT_OK(CreatePagingStateForRead(
      pgsql_read_request, *fetched_rows, &result->response));

  // TODO(neil) The clients' request should indicate what encoding method should be used. When
  // multi-shard is used to process more complicated queries, proxy-server might prefer a different
  // encoding. For now, we'll call PgsqlSerialize() without checking encoding method.
  result->response.set_status(PgsqlResponsePB::PGSQL_STATUS_OK);

  // Serializing data for PgGate API.
  CHECK(!pgsql_read_request.has_rsrow_desc()) << "Row description is not needed";
  TRACE("Done Handle");

  return Status::OK();
}

}  // namespace tablet
}  // namespace yb
