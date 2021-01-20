//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------

#include "yb/yql/pggate/pg_dml_read.h"
#include "yb/yql/pggate/pg_select_index.h"
#include "yb/yql/pggate/util/pg_doc_data.h"
#include "yb/client/yb_op.h"
#include "yb/common/pg_system_attr.h"
#include "yb/docdb/primitive_value.h"

namespace yb {
namespace pggate {

using std::make_shared;

//--------------------------------------------------------------------------------------------------
// PgDmlRead
//--------------------------------------------------------------------------------------------------

PgDmlRead::PgDmlRead(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id,
                     const PgObjectId& index_id, const PgPrepareParameters *prepare_params)
    : PgDml(std::move(pg_session), table_id, index_id, prepare_params) {
}

PgDmlRead::~PgDmlRead() {
}

void PgDmlRead::PrepareBinds() {
  if (!bind_desc_) {
    // This statement doesn't have bindings.
    return;
  }

  for (PgColumn &col : bind_desc_->columns()) {
    col.AllocPrimaryBindPB(read_req_);
  }
}

void PgDmlRead::SetForwardScan(const bool is_forward_scan) {
  if (secondary_index_query_) {
    return secondary_index_query_->SetForwardScan(is_forward_scan);
  }
  read_req_->set_is_forward_scan(is_forward_scan);
}

//--------------------------------------------------------------------------------------------------
// DML support.
// TODO(neil) WHERE clause is not yet supported. Revisit this function when it is.

PgsqlExpressionPB *PgDmlRead::AllocColumnBindPB(PgColumn *col) {
  return col->AllocBindPB(read_req_);
}

PgsqlExpressionPB *PgDmlRead::AllocColumnBindConditionExprPB(PgColumn *col) {
  return col->AllocBindConditionExprPB(read_req_);
}

PgsqlExpressionPB *PgDmlRead::AllocColumnAssignPB(PgColumn *col) {
  // SELECT statement should not have an assign expression (SET clause).
  LOG(FATAL) << "Pure virtual function is being call";
  return nullptr;
}

PgsqlExpressionPB *PgDmlRead::AllocTargetPB() {
  return read_req_->add_targets();
}

//--------------------------------------------------------------------------------------------------
// RESULT SET SUPPORT.
// For now, selected expressions are just a list of column names (ref).
//   SELECT column_l, column_m, column_n FROM ...

void PgDmlRead::SetColumnRefs() {
  if (secondary_index_query_) {
    DCHECK(!has_aggregate_targets()) << "Aggregate pushdown should not happen with index";
  }
  read_req_->set_is_aggregate(has_aggregate_targets());
  ColumnRefsToPB(read_req_->mutable_column_refs());
}

Status PgDmlRead::DeleteEmptyPrimaryBinds() {
  if (secondary_index_query_) {
    RETURN_NOT_OK(secondary_index_query_->DeleteEmptyPrimaryBinds());
  }

  if (!bind_desc_) {
    // This query does not have any binds.
    read_req_->clear_partition_column_values();
    read_req_->clear_range_column_values();
    return Status::OK();
  }

  // NOTE: ybctid is a system column and not processed as bind.
  bool miss_partition_columns = false;
  bool has_partition_columns = false;

  for (size_t i = 0; i < bind_desc_->num_hash_key_columns(); i++) {
    PgColumn& col = bind_desc_->columns()[i];
    PgsqlExpressionPB* expr = col.bind_pb();
    if (expr_binds_.find(expr) == expr_binds_.end() && !expr->has_condition()) {
      // For IN clause on hash_column, expr->has_condition() returns 'true'.
      miss_partition_columns = true;
    } else {
      has_partition_columns = true;
    }
  }

  if (miss_partition_columns) {
    VLOG(1) << "Full scan is needed";
    read_req_->clear_partition_column_values();
    read_req_->clear_range_column_values();
  }

  if (has_partition_columns && miss_partition_columns) {
    return STATUS(InvalidArgument, "Partition key must be fully specified");
  }

  bool miss_range_columns = false;
  size_t num_bound_range_columns = 0;

  for (size_t i = bind_desc_->num_hash_key_columns(); i < bind_desc_->num_key_columns(); i++) {
    PgColumn &col = bind_desc_->columns()[i];
    if (expr_binds_.find(col.bind_pb()) == expr_binds_.end()) {
      miss_range_columns = true;
    } else if (miss_range_columns) {
      return STATUS(InvalidArgument,
                    "Unspecified range key column must be at the end of the range key");
    } else {
      num_bound_range_columns++;
    }
  }

  auto *range_column_values = read_req_->mutable_range_column_values();
  range_column_values->DeleteSubrange(num_bound_range_columns,
                                      range_column_values->size() - num_bound_range_columns);
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Status PgDmlRead::Exec(const PgExecParameters *exec_params) {
  // Initialize doc operator.
  if (doc_op_) {
    RETURN_NOT_OK(doc_op_->ExecuteInit(exec_params));
  }

  // Set column references in protobuf and whether query is aggregate.
  SetColumnRefs();

  // Delete key columns that are not bound to any values.
  RETURN_NOT_OK(DeleteEmptyPrimaryBinds());

  // First, process the secondary index request.
  bool has_ybctid = VERIFY_RESULT(ProcessSecondaryIndexRequest(exec_params));

  if (!has_ybctid && secondary_index_query_ && secondary_index_query_->has_doc_op()) {
    // No ybctid is found from the IndexScan. Instruct "doc_op_" to abandon the execution and not
    // querying any data from tablet server.
    //
    // Note: For system catalog (colocated table), the secondary_index_query_ won't send a separate
    // scan read request to DocDB.  For this case, the index request is embedded inside the SELECT
    // request (PgsqlReadRequestPB::index_request).
    doc_op_->AbandonExecution();

  } else {
    // Update bind values for constants and placeholders.
    RETURN_NOT_OK(UpdateBindPBs());

    // Execute select statement and prefetching data from DocDB.
    // Note: For SysTable, doc_op_ === null, IndexScan doesn't send separate request.
    if (doc_op_) {
      SCHECK_EQ(VERIFY_RESULT(doc_op_->Execute()), RequestSent::kTrue, IllegalState,
                "YSQL read operation was not sent");
    }
  }

  return Status::OK();
}

Status PgDmlRead::BindColumnCondEq(int attr_num, PgExpr *attr_value) {
  if (secondary_index_query_) {
    // Bind by secondary key.
    return secondary_index_query_->BindColumnCondEq(attr_num, attr_value);
  }

  // Find column.
  PgColumn *col = VERIFY_RESULT(bind_desc_->FindColumn(attr_num));

  // Check datatype.
  if (attr_value) {
    SCHECK_EQ(col->internal_type(), attr_value->internal_type(), Corruption,
              "Attribute value type does not match column type");
  }

  // Alloc the protobuf.
  PgsqlExpressionPB *condition_expr_pb = AllocColumnBindConditionExprPB(col);

  if (attr_value != nullptr) {
    condition_expr_pb->mutable_condition()->set_op(QL_OP_EQUAL);

    auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
    auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();

    op1_pb->set_column_id(col->id());

    RETURN_NOT_OK(attr_value->Eval(this, op2_pb->mutable_value()));
  }

  if (attr_num == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
    CHECK(attr_value->is_constant()) << "Column ybctid must be bound to constant";
    ybctid_bind_ = true;
  }

  return Status::OK();
}

Status PgDmlRead::BindColumnCondBetween(int attr_num, PgExpr *attr_value, PgExpr *attr_value_end) {
  if (secondary_index_query_) {
    // Bind by secondary key.
    return secondary_index_query_->BindColumnCondBetween(attr_num, attr_value, attr_value_end);
  }

  DCHECK(attr_num != static_cast<int>(PgSystemAttrNum::kYBTupleId))
    << "Operator BETWEEN cannot be applied to ROWID";

  // Find column.
  PgColumn *col = VERIFY_RESULT(bind_desc_->FindColumn(attr_num));

  // Check datatype.
  if (attr_value) {
    SCHECK_EQ(col->internal_type(), attr_value->internal_type(), Corruption,
              "Attribute value type does not match column type");
  }

  if (attr_value_end) {
    SCHECK_EQ(col->internal_type(), attr_value_end->internal_type(), Corruption,
              "Attribute value type does not match column type");
  }

  CHECK(!col->desc()->is_partition()) << "This method cannot be used for binding partition column!";

  // Alloc the protobuf.
  PgsqlExpressionPB *condition_expr_pb = AllocColumnBindConditionExprPB(col);

  if (attr_value != nullptr) {
    if (attr_value_end != nullptr) {
      condition_expr_pb->mutable_condition()->set_op(QL_OP_BETWEEN);

      auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
      auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();
      auto op3_pb = condition_expr_pb->mutable_condition()->add_operands();

      op1_pb->set_column_id(col->id());

      RETURN_NOT_OK(attr_value->Eval(this, op2_pb->mutable_value()));
      RETURN_NOT_OK(attr_value_end->Eval(this, op3_pb->mutable_value()));
    } else {
      condition_expr_pb->mutable_condition()->set_op(QL_OP_GREATER_THAN_EQUAL);

      auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
      auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();

      op1_pb->set_column_id(col->id());

      RETURN_NOT_OK(attr_value->Eval(this, op2_pb->mutable_value()));
    }
  } else {
    if (attr_value_end != nullptr) {
      condition_expr_pb->mutable_condition()->set_op(QL_OP_LESS_THAN_EQUAL);

      auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
      auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();

      op1_pb->set_column_id(col->id());

      RETURN_NOT_OK(attr_value_end->Eval(this, op2_pb->mutable_value()));
    } else {
      // Unreachable.
    }
  }

  return Status::OK();
}

Status PgDmlRead::BindColumnCondIn(int attr_num, int n_attr_values, PgExpr **attr_values) {
  if (secondary_index_query_) {
    // Bind by secondary key.
    return secondary_index_query_->BindColumnCondIn(attr_num, n_attr_values, attr_values);
  }

  DCHECK(attr_num != static_cast<int>(PgSystemAttrNum::kYBTupleId))
    << "Operator IN cannot be applied to ROWID";

  // Find column.
  PgColumn *col = VERIFY_RESULT(bind_desc_->FindColumn(attr_num));

  // Check datatype.
  // TODO(neil) Current code combine TEXT and BINARY datatypes into ONE representation.  Once that
  // is fixed, we can remove the special if() check for BINARY type.
  if (col->internal_type() != InternalType::kBinaryValue) {
    for (int i = 0; i < n_attr_values; i++) {
      if (attr_values[i]) {
        SCHECK_EQ(col->internal_type(), attr_values[i]->internal_type(), Corruption,
            "Attribute value type does not match column type");
      }
    }
  }

  if (col->desc()->is_partition()) {
    // Alloc the protobuf.
    PgsqlExpressionPB* bind_pb = col->bind_pb();
    if (bind_pb == nullptr) {
      bind_pb = AllocColumnBindPB(col);
    } else {
      if (expr_binds_.find(bind_pb) != expr_binds_.end()) {
        LOG(WARNING) << strings::Substitute("Column $0 is already bound to another value.",
                                            attr_num);
      }
    }

    bind_pb->mutable_condition()->set_op(QL_OP_IN);
    bind_pb->mutable_condition()->add_operands()->set_column_id(col->id());

    // There's no "list of expressions" field so we simulate it with an artificial nested OR
    // with repeated operands, one per bind expression.
    // This is only used for operation unrolling in pg_doc_op and is not understood by DocDB.
    auto op2_pb = bind_pb->mutable_condition()->add_operands();
    op2_pb->mutable_condition()->set_op(QL_OP_OR);

    for (int i = 0; i < n_attr_values; i++) {
      auto* attr_pb = op2_pb->mutable_condition()->add_operands();
      // Link the expression and protobuf. During execution, expr will write result to the pb.
      RETURN_NOT_OK(attr_values[i]->PrepareForRead(this, attr_pb));

      expr_binds_[attr_pb] = attr_values[i];

      if (attr_num == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
        CHECK(attr_values[i]->is_constant()) << "Column ybctid must be bound to constant";
        ybctid_bind_ = true;
      }
    }
  } else {
    // Alloc the protobuf.
    PgsqlExpressionPB *condition_expr_pb = AllocColumnBindConditionExprPB(col);

    condition_expr_pb->mutable_condition()->set_op(QL_OP_IN);

    auto op1_pb = condition_expr_pb->mutable_condition()->add_operands();
    auto op2_pb = condition_expr_pb->mutable_condition()->add_operands();

    op1_pb->set_column_id(col->id());

    for (int i = 0; i < n_attr_values; i++) {
      // Link the given expression "attr_value" with the allocated protobuf.
      // Note that except for constants and place_holders, all other expressions can be setup
      // just one time during prepare.
      // Examples:
      // - Bind values for primary columns in where clause.
      //     WHERE hash = ?
      // - Bind values for a column in INSERT statement.
      //     INSERT INTO a_table(hash, key, col) VALUES(?, ?, ?)

      if (attr_values[i]) {
        RETURN_NOT_OK(attr_values[i]->Eval(this,
              op2_pb->mutable_value()->mutable_list_value()->add_elems()));
      }

      if (attr_num == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
        CHECK(attr_values[i]->is_constant()) << "Column ybctid must be bound to constant";
        ybctid_bind_ = true;
      }
    }
  }

  return Status::OK();
}

}  // namespace pggate
}  // namespace yb
