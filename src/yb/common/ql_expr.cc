//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//--------------------------------------------------------------------------------------------------

#include "yb/common/ql_expr.h"

#include "yb/common/ql_bfunc.h"
#include "yb/common/ql_value.h"
#include "yb/common/jsonb.h"

namespace yb {

namespace {

constexpr size_t kInvalidIndex = std::numeric_limits<size_t>::max();

}

bfql::TSOpcode QLExprExecutor::GetTSWriteInstruction(const QLExpressionPB& ql_expr) const {
  // "kSubDocInsert" instructs the tablet server to insert a new value or replace an existing value.
  if (ql_expr.has_tscall()) {
    return static_cast<bfql::TSOpcode>(ql_expr.tscall().opcode());
  }
  return bfql::TSOpcode::kScalarInsert;
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalExpr(const QLExpressionPB& ql_expr,
                                        const QLTableRow& table_row,
                                        QLExprResultWriter result_writer,
                                        const Schema *schema) {
  switch (ql_expr.expr_case()) {
    case QLExpressionPB::ExprCase::kValue:
      result_writer.SetExisting(&ql_expr.value());
      break;

    case QLExpressionPB::ExprCase::kColumnId:
      RETURN_NOT_OK(table_row.ReadColumn(ql_expr.column_id(), result_writer));
      break;

    case QLExpressionPB::ExprCase::kJsonColumn: {
      QLExprResult temp;
      const QLJsonColumnOperationsPB& json_ops = ql_expr.json_column();
      RETURN_NOT_OK(table_row.ReadColumn(json_ops.column_id(), temp.Writer()));
      if (temp.IsNull()) {
        result_writer.SetNull();
      } else {
        common::Jsonb jsonb;
        temp.MoveToJsonb(&jsonb);
        RETURN_NOT_OK(jsonb.ApplyJsonbOperators(json_ops, &result_writer.NewValue()));
      }
      break;
    }

    case QLExpressionPB::ExprCase::kSubscriptedCol:
      if (table_row.IsEmpty()) {
        result_writer.SetNull();
      } else {
        QLExprResult index_arg;
        const QLSubscriptedColPB& subcol = ql_expr.subscripted_col();
        RETURN_NOT_OK(EvalExpr(subcol.subscript_args(0), table_row, index_arg.Writer()));
        RETURN_NOT_OK(table_row.ReadSubscriptedColumn(subcol, index_arg.Value(), result_writer));
      }
      break;

    case QLExpressionPB::ExprCase::kBfcall:
      return EvalBFCall(ql_expr.bfcall(), table_row, &result_writer.NewValue());

    case QLExpressionPB::ExprCase::kTscall:
      return EvalTSCall(ql_expr.tscall(), table_row, &result_writer.NewValue(), schema);

    case QLExpressionPB::ExprCase::kCondition:
      return EvalCondition(ql_expr.condition(), table_row, &result_writer.NewValue());

    case QLExpressionPB::ExprCase::kBocall: FALLTHROUGH_INTENDED;
    case QLExpressionPB::ExprCase::kBindId: FALLTHROUGH_INTENDED;
    case QLExpressionPB::ExprCase::EXPR_NOT_SET:
      result_writer.SetNull();
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalExpr(QLExpressionPB* ql_expr,
                                        const QLTableRow& table_row,
                                        const Schema *schema) {
  if (!ql_expr->has_value()) {
    QLExprResult temp;
    RETURN_NOT_OK(EvalExpr(*ql_expr, table_row, temp.Writer(), schema));
    temp.MoveTo(ql_expr->mutable_value());
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::ReadExprValue(const QLExpressionPB& ql_expr,
                                             const QLTableRow& table_row,
                                             QLExprResultWriter result_writer) {
  if (ql_expr.expr_case() == QLExpressionPB::ExprCase::kTscall) {
    return ReadTSCallValue(ql_expr.tscall(), table_row, result_writer);
  } else {
    return EvalExpr(ql_expr, table_row, result_writer);
  }
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalBFCall(const QLBCallPB& bfcall,
                                          const QLTableRow& table_row,
                                          QLValue *result) {
  // TODO(neil)
  // - Use TSOpode for collection expression if only TabletServer can execute.
  // OR
  // - Introduce BuiltinOperator in addition to builtin function. Use builtin operators for all
  //   special operations including collection operations. That way, we don't need special cases.

  // Special cases: for collection operations of the form "cref = cref +/- <value>" we avoid
  // reading column cref and instead tell doc writer to modify it in-place.
  //   "AddMapMap"
  //   "AddSetSet"
  //   "SubMapSet"
  //   "SubSetSet"
  //   "AddListList"
  //   "SubListList"

  const bfql::BFOpcode bf_opcode = static_cast<bfql::BFOpcode>(bfcall.opcode());
  // First evaluate the arguments.
  vector<QLValue> args(bfcall.operands().size());
  int arg_index = 0;
  QLExprResult temp;
  for (auto operand : bfcall.operands()) {
    RETURN_NOT_OK(EvalExpr(operand, table_row, temp.Writer()));
    temp.MoveTo(args[arg_index++].mutable_value());
  }

  // Execute the builtin call associated with the given opcode.
  return QLBfunc::Exec(bf_opcode, &args, result);
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalTSCall(const QLBCallPB& ql_expr,
                                          const QLTableRow& table_row,
                                          QLValue *result,
                                          const Schema *schema) {
  result->SetNull();
  return STATUS(RuntimeError, "Only tablet server can execute this operator");
}

CHECKED_STATUS QLExprExecutor::ReadTSCallValue(const QLBCallPB& ql_expr,
                                               const QLTableRow& table_row,
                                               QLExprResultWriter result_writer) {
  result_writer.SetNull();
  return STATUS(RuntimeError, "Only tablet server can execute this operator");
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalCondition(const QLConditionPB& condition,
                                             const QLTableRow& table_row,
                                             bool* result) {
  QLValue result_pb;
  RETURN_NOT_OK(EvalCondition(condition, table_row, &result_pb));
  *result = result_pb.bool_value();
  return Status::OK();
}

template <class Operands>
Result<bool> In(
    QLExprExecutor* executor, const Operands& operands, const QLTableRow& table_row) {
  QLExprResult left, right;
  RETURN_NOT_OK(EvalOperands(executor, operands, table_row, left.Writer(), right.Writer()));

  for (const QLValuePB& elem : right.Value().list_value().elems()) {
    if (!Comparable(elem, left.Value())) {
       return STATUS(RuntimeError, "values not comparable");
    }
    if (elem == left.Value()) {
      return true;
    }
  }

  return false;
}

template <class Operands, class Op>
Result<bool> EvalRelationalOp(
    QLExprExecutor* executor, const Operands& operands, const QLTableRow& table_row, const Op& op) {
  QLExprResult left, right;
  RETURN_NOT_OK(EvalOperands(executor, operands, table_row, left.Writer(), right.Writer()));
  if (!Comparable(left.Value(), right.Value())) {
    return STATUS(RuntimeError, "values not comparable");
  }
  return op(left.Value(), right.Value());
}

template<bool Value, class Operands>
Result<bool> Is(
  QLExprExecutor* executor, const Operands& operands, const QLTableRow& table_row) {
  QLExprResult temp;
  RETURN_NOT_OK(EvalOperands(executor, operands, table_row, temp.Writer()));
  if (temp.Value().value_case() != InternalType::kBoolValue) {
    return STATUS(RuntimeError, "not a bool value");
  }
  return !IsNull(temp.Value()) && temp.Value().bool_value() == Value;
}

template<class Operands>
Result<bool> Between(
  QLExprExecutor* executor, const Operands& operands, const QLTableRow& table_row) {
  CHECK_EQ(operands.size(), 3);
  QLExprResult temp, lower, upper;
  RETURN_NOT_OK(EvalOperands(
       executor, operands, table_row, temp.Writer(), lower.Writer(), upper.Writer()));
  if (!Comparable(temp.Value(), lower.Value()) || !Comparable(temp.Value(), upper.Value())) {
    return STATUS(RuntimeError, "values not comparable");
  }
  return temp.Value() >= lower.Value() && temp.Value() <= upper.Value();
}

CHECKED_STATUS QLExprExecutor::EvalCondition(const QLConditionPB& condition,
                                             const QLTableRow& table_row,
                                             QLValue *result) {
#define QL_EVALUATE_RELATIONAL_OP(op) \
  result->set_bool_value(VERIFY_RESULT(EvalRelationalOp(this, operands, table_row, op))); \
  return Status::OK();

  QLExprResult temp;
  const auto& operands = condition.operands();
  switch (condition.op()) {
    case QL_OP_NOT: {
      CHECK_EQ(operands.size(), 1);
      CHECK_EQ(operands.Get(0).expr_case(), QLExpressionPB::ExprCase::kCondition);
      QLValue sub_result;
      RETURN_NOT_OK(EvalCondition(operands.Get(0).condition(), table_row, &sub_result));
      result->set_bool_value(!sub_result.bool_value());
      return Status::OK();
    }

    case QL_OP_IS_NULL:
      CHECK_EQ(operands.size(), 1);
      RETURN_NOT_OK(EvalExpr(operands.Get(0), table_row, temp.Writer()));
      result->set_bool_value(IsNull(temp.Value()));
      return Status::OK();

    case QL_OP_IS_NOT_NULL:
      CHECK_EQ(operands.size(), 1);
      RETURN_NOT_OK(EvalExpr(operands.Get(0), table_row, temp.Writer()));
      result->set_bool_value(!IsNull(temp.Value()));
      return Status::OK();

    case QL_OP_IS_TRUE:
      result->set_bool_value(VERIFY_RESULT(Is<true>(this, operands, table_row)));
      return Status::OK();

    case QL_OP_IS_FALSE:
      result->set_bool_value(VERIFY_RESULT(Is<false>(this, operands, table_row)));
      return Status::OK();

    case QL_OP_EQUAL:
      QL_EVALUATE_RELATIONAL_OP(std::equal_to<>());

    case QL_OP_LESS_THAN:
      QL_EVALUATE_RELATIONAL_OP(std::less<>());

    case QL_OP_LESS_THAN_EQUAL:
      QL_EVALUATE_RELATIONAL_OP(std::less_equal<>());

    case QL_OP_GREATER_THAN:
      QL_EVALUATE_RELATIONAL_OP(std::greater<>());

    case QL_OP_GREATER_THAN_EQUAL:
      QL_EVALUATE_RELATIONAL_OP(std::greater_equal<>());

    case QL_OP_NOT_EQUAL:
      QL_EVALUATE_RELATIONAL_OP(std::not_equal_to<>());

    case QL_OP_AND:
      CHECK_GT(operands.size(), 0);
      for (const auto &operand : operands) {
        CHECK_EQ(operand.expr_case(), QLExpressionPB::ExprCase::kCondition);
        RETURN_NOT_OK(EvalCondition(operand.condition(), table_row, result));
        if (!result->bool_value()) {
          break;
        }
      }
      return Status::OK();

    case QL_OP_OR:
      CHECK_GT(operands.size(), 0);
      for (const auto &operand : operands) {
        CHECK_EQ(operand.expr_case(), QLExpressionPB::ExprCase::kCondition);
        RETURN_NOT_OK(EvalCondition(operand.condition(), table_row, result));
        if (result->bool_value()) {
          break;
        }
      }
      return Status::OK();

    case QL_OP_BETWEEN:
      result->set_bool_value(VERIFY_RESULT(Between(this, operands, table_row)));
      return Status::OK();

    case QL_OP_NOT_BETWEEN:
      result->set_bool_value(!VERIFY_RESULT(Between(this, operands, table_row)));
      return Status::OK();

    // When a row exists, the primary key columns are always populated in the row (value-map) by
    // DocRowwiseIterator and only when it exists. Therefore, the row exists if and only if
    // the row (value-map) is not empty.
    case QL_OP_EXISTS:
      result->set_bool_value(!table_row.IsEmpty());
      return Status::OK();

    case QL_OP_NOT_EXISTS:
      result->set_bool_value(table_row.IsEmpty());
      return Status::OK();

    case QL_OP_IN:
      CHECK_EQ(operands.size(), 2);
      result->set_bool_value(VERIFY_RESULT(In(this, operands, table_row)));
      return Status::OK();

    case QL_OP_NOT_IN:
      CHECK_EQ(operands.size(), 2);
      result->set_bool_value(!VERIFY_RESULT(In(this, operands, table_row)));
      return Status::OK();

    case QL_OP_LIKE: FALLTHROUGH_INTENDED;
    case QL_OP_NOT_LIKE:
      LOG(ERROR) << "Internal error: illegal or unknown operator " << condition.op();
      break;

    case QL_OP_NOOP:
      break;
  }

  result->SetNull();
  return STATUS(RuntimeError, "Internal error: illegal or unknown operator");

#undef QL_EVALUATE_RELATIONAL_OP
}

//--------------------------------------------------------------------------------------------------

bfpg::TSOpcode QLExprExecutor::GetTSWriteInstruction(const PgsqlExpressionPB& ql_expr) const {
  // "kSubDocInsert" instructs the tablet server to insert a new value or replace an existing value.
  if (ql_expr.has_tscall()) {
    return static_cast<bfpg::TSOpcode>(ql_expr.tscall().opcode());
  }
  return bfpg::TSOpcode::kScalarInsert;
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalExpr(const PgsqlExpressionPB& ql_expr,
                                        const QLTableRow* table_row,
                                        QLExprResultWriter result_writer,
                                        const Schema *schema) {
  switch (ql_expr.expr_case()) {
    case PgsqlExpressionPB::ExprCase::kValue:
      result_writer.SetExisting(&ql_expr.value());
      break;

    case PgsqlExpressionPB::ExprCase::kColumnId:
      return EvalColumnRef(ql_expr.column_id(), table_row, result_writer);

    case PgsqlExpressionPB::ExprCase::kBfcall:
      return EvalBFCall(ql_expr.bfcall(), *table_row, &result_writer.NewValue());

    case PgsqlExpressionPB::ExprCase::kTscall:
      return EvalTSCall(ql_expr.tscall(), *table_row, &result_writer.NewValue(), schema);

    case PgsqlExpressionPB::ExprCase::kCondition:
      return EvalCondition(ql_expr.condition(), *table_row, &result_writer.NewValue());

    case PgsqlExpressionPB::ExprCase::kBocall: FALLTHROUGH_INTENDED;
    case PgsqlExpressionPB::ExprCase::kBindId: FALLTHROUGH_INTENDED;
    case PgsqlExpressionPB::ExprCase::kAliasId: FALLTHROUGH_INTENDED;
    case PgsqlExpressionPB::ExprCase::EXPR_NOT_SET:
      result_writer.SetNull();
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::ReadExprValue(const PgsqlExpressionPB& ql_expr,
                                             const QLTableRow& table_row,
                                             QLExprResultWriter result_writer) {
  if (ql_expr.expr_case() == PgsqlExpressionPB::ExprCase::kTscall) {
    return ReadTSCallValue(ql_expr.tscall(), table_row, result_writer);
  } else {
    return EvalExpr(ql_expr, table_row, result_writer);
  }
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalColumnRef(ColumnIdRep col_id,
                                             const QLTableRow* table_row,
                                             QLExprResultWriter result_writer) {
  if (table_row == nullptr) {
    result_writer.SetNull();
  } else {
    RETURN_NOT_OK(table_row->ReadColumn(col_id, result_writer));
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalBFCall(const PgsqlBCallPB& bfcall,
                                          const QLTableRow& table_row,
                                          QLValue *result) {
  // TODO(neil)
  // - Use TSOpode for collection expression if only TabletServer can execute.
  // OR
  // - Introduce BuiltinOperator in addition to builtin function. Use builtin operators for all
  //   special operations including collection operations. That way, we don't need special cases.

  // First, evaluate the arguments.
  vector<QLValue> args(bfcall.operands().size());
  int arg_index = 0;
  QLExprResult temp;
  for (auto operand : bfcall.operands()) {
    RETURN_NOT_OK(EvalExpr(operand, &table_row, temp.Writer()));
    temp.MoveTo(args[arg_index++].mutable_value());
  }

  // Now, execute the builtin call associated with the given opcode.
  return PgsqlBfunc::Exec(static_cast<bfpg::BFOpcode>(bfcall.opcode()), &args, result);
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalTSCall(const PgsqlBCallPB& ql_expr,
                                          const QLTableRow& table_row,
                                          QLValue *result,
                                          const Schema *schema) {
  result->SetNull();
  return STATUS(RuntimeError, "Only tablet server can execute this operator");
}

CHECKED_STATUS QLExprExecutor::ReadTSCallValue(const PgsqlBCallPB& ql_expr,
                                               const QLTableRow& table_row,
                                               QLExprResultWriter result_writer) {
  result_writer.SetNull();
  return STATUS(RuntimeError, "Only tablet server can execute this operator");
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS QLExprExecutor::EvalCondition(const PgsqlConditionPB& condition,
                                             const QLTableRow& table_row,
                                             bool* result) {
  QLValue result_pb;
  RETURN_NOT_OK(EvalCondition(condition, table_row, &result_pb));
  *result = result_pb.bool_value();
  return Status::OK();
}

CHECKED_STATUS QLExprExecutor::EvalCondition(const PgsqlConditionPB& condition,
                                             const QLTableRow& table_row,
                                             QLValue *result) {
#define QL_EVALUATE_RELATIONAL_OP(op)                                                              \
  result->set_bool_value(VERIFY_RESULT(EvalRelationalOp(this, operands, table_row, op))); \
  return Status::OK();

  QLExprResult temp;
  const auto& operands = condition.operands();
  switch (condition.op()) {
    case QL_OP_NOT: {
      CHECK_EQ(operands.size(), 1);
      CHECK_EQ(operands.Get(0).expr_case(), PgsqlExpressionPB::ExprCase::kCondition);
      QLValue sub_result;
      RETURN_NOT_OK(EvalCondition(operands.Get(0).condition(), table_row, &sub_result));
      result->set_bool_value(!sub_result.bool_value());
      return Status::OK();
    }

    case QL_OP_IS_NULL:
      CHECK_EQ(operands.size(), 1);
      RETURN_NOT_OK(EvalExpr(operands.Get(0), table_row, temp.Writer()));
      result->set_bool_value(IsNull(temp.Value()));
      return Status::OK();

    case QL_OP_IS_NOT_NULL:
      CHECK_EQ(operands.size(), 1);
      RETURN_NOT_OK(EvalExpr(operands.Get(0), table_row, temp.Writer()));
      result->set_bool_value(!IsNull(temp.Value()));
      return Status::OK();

    case QL_OP_IS_TRUE:
      result->set_bool_value(VERIFY_RESULT(Is<true>(this, operands, table_row)));
      return Status::OK();

    case QL_OP_IS_FALSE: {
      result->set_bool_value(VERIFY_RESULT(Is<false>(this, operands, table_row)));
      return Status::OK();
    }

    case QL_OP_EQUAL:
      QL_EVALUATE_RELATIONAL_OP(std::equal_to<>());

    case QL_OP_LESS_THAN:
      QL_EVALUATE_RELATIONAL_OP(std::less<>());

    case QL_OP_LESS_THAN_EQUAL:
      QL_EVALUATE_RELATIONAL_OP(std::less_equal<>());

    case QL_OP_GREATER_THAN:
      QL_EVALUATE_RELATIONAL_OP(std::greater<>());

    case QL_OP_GREATER_THAN_EQUAL:
      QL_EVALUATE_RELATIONAL_OP(std::greater_equal<>());

    case QL_OP_NOT_EQUAL:
      QL_EVALUATE_RELATIONAL_OP(std::not_equal_to<>());

    case QL_OP_AND:
      CHECK_GT(operands.size(), 0);
      for (const auto &operand : operands) {
        CHECK_EQ(operand.expr_case(), PgsqlExpressionPB::ExprCase::kCondition);
        RETURN_NOT_OK(EvalCondition(operand.condition(), table_row, result));
        if (!result->bool_value()) {
          break;
        }
      }
      return Status::OK();

    case QL_OP_OR:
      CHECK_GT(operands.size(), 0);
      for (const auto &operand : operands) {
        CHECK_EQ(operand.expr_case(), PgsqlExpressionPB::ExprCase::kCondition);
        RETURN_NOT_OK(EvalCondition(operand.condition(), table_row, result));
        if (result->bool_value()) {
          break;
        }
      }
      return Status::OK();

    case QL_OP_BETWEEN:
      result->set_bool_value(VERIFY_RESULT(Between(this, operands, table_row)));
      return Status::OK();

    case QL_OP_NOT_BETWEEN:
      result->set_bool_value(!VERIFY_RESULT(Between(this, operands, table_row)));
      return Status::OK();

      // When a row exists, the primary key columns are always populated in the row (value-map) by
      // DocRowwiseIterator and only when it exists. Therefore, the row exists if and only if
      // the row (value-map) is not empty.
    case QL_OP_EXISTS:
      result->set_bool_value(!table_row.IsEmpty());
      return Status::OK();

    case QL_OP_NOT_EXISTS:
      result->set_bool_value(table_row.IsEmpty());
      return Status::OK();

    case QL_OP_IN:
      CHECK_EQ(operands.size(), 2);
      result->set_bool_value(VERIFY_RESULT(In(this, operands, table_row)));
      break;

    case QL_OP_NOT_IN:
      CHECK_EQ(operands.size(), 2);
      result->set_bool_value(!VERIFY_RESULT(In(this, operands, table_row)));
      break;

    case QL_OP_LIKE: FALLTHROUGH_INTENDED;
    case QL_OP_NOT_LIKE:
      LOG(ERROR) << "Internal error: illegal or unknown operator " << condition.op();
      break;

    case QL_OP_NOOP:
      break;
  }

  result->SetNull();
  return STATUS(RuntimeError, "Internal error: illegal or unknown operator");

#undef QL_EVALUATE_RELATIONAL_OP
#undef QL_EVALUATE_BETWEEN
}

//--------------------------------------------------------------------------------------------------

const QLTableRow& QLTableRow::empty_row() {
  static QLTableRow empty_row;
  return empty_row;
}

size_t QLTableRow::ColumnCount() const {
  size_t result = 0;
  for (auto i : assigned_) {
    result += i;
  }
  return result;
}

void QLTableRow::Clear() {
  if (num_assigned_ == 0) {
    return;
  }

  memset(assigned_.data(), 0, assigned_.size());
  num_assigned_ = 0;
}

size_t QLTableRow::ColumnIndex(ColumnIdRep col_id) const {
  if (col_id < kFirstNonPreallocatedColumnId && col_id >= kFirstColumnIdRep) {
    return col_id - kFirstColumnIdRep;
  }
  const auto& col_iter = column_id_to_index_.find(col_id);
  if (col_iter == column_id_to_index_.end()) {
    return kInvalidIndex;
  }

  return col_iter->second;
}

const QLTableColumn* QLTableRow::FindColumn(ColumnIdRep col_id) const {
  size_t index = ColumnIndex(col_id);
  if (index == kInvalidIndex || index >= assigned_.size() || !assigned_[index]) {
    return nullptr;
  }

  return &values_[index];
}

const QLValuePB* QLTableRow::GetColumn(ColumnIdRep col_id) const {
  const auto* column = FindColumn(col_id);
  return column ? &column->value : nullptr;
}

CHECKED_STATUS QLTableRow::ReadColumn(ColumnIdRep col_id, QLExprResultWriter result_writer) const {
  auto value = GetColumn(col_id);
  if (value == nullptr) {
    result_writer.SetNull();
    return Status::OK();
  }

  result_writer.SetExisting(value);
  return Status::OK();
}

CHECKED_STATUS QLTableRow::ReadSubscriptedColumn(const QLSubscriptedColPB& subcol,
                                                 const QLValuePB& index_arg,
                                                 QLExprResultWriter result_writer) const {
  const auto* value = GetColumn(subcol.column_id());
  if (!value) {
    // Does not exist.
    result_writer.SetNull();
    return Status::OK();
  }

  if (value->has_map_value()) {
    // map['key']
    auto& map = value->map_value();
    for (int i = 0; i < map.keys_size(); i++) {
      if (map.keys(i) == index_arg) {
        result_writer.SetExisting(&map.values(i));
        return Status::OK();
      }
    }
  } else if (value->has_list_value()) {
    // list[index]
    auto& list = value->list_value();
    if (index_arg.has_int32_value()) {
      int list_index = index_arg.int32_value();
      if (list_index >= 0 && list_index < list.elems_size()) {
        result_writer.SetExisting(&list.elems(list_index));
        return Status::OK();
      }
    }
  }

  result_writer.SetNull();
  return Status::OK();
}

Result<const QLTableColumn&> QLTableRow::Column(ColumnIdRep col_id) const {
  const auto* column = FindColumn(col_id);
  if (column == nullptr) {
    // Does not exist.
    return STATUS(InternalError, "Column unexpectedly not found in cache");
  }

  return *column;
}

CHECKED_STATUS QLTableRow::GetTTL(ColumnIdRep col_id, int64_t *ttl_seconds) const {
  *ttl_seconds = VERIFY_RESULT(Column(col_id)).get().ttl_seconds;
  return Status::OK();
}

CHECKED_STATUS QLTableRow::GetWriteTime(ColumnIdRep col_id, int64_t *write_time) const {
  const QLTableColumn& column = VERIFY_RESULT(Column(col_id));
  DCHECK_NE(QLTableColumn::kUninitializedWriteTime, column.write_time) << "Column id: " << col_id;
  *write_time = column.write_time;
  return Status::OK();
}

CHECKED_STATUS QLTableRow::GetValue(ColumnIdRep col_id, QLValue *column) const {
  *column = VERIFY_RESULT(Column(col_id)).get().value;
  return Status::OK();
}

boost::optional<const QLValuePB&> QLTableRow::GetValue(ColumnIdRep col_id) const {
  const auto* column = FindColumn(col_id);
  if (column) {
    return column->value;
  }
  return boost::none;
}

bool QLTableRow::IsColumnSpecified(ColumnIdRep col_id) const {
  size_t index = ColumnIndex(col_id);
  if (index == kInvalidIndex) {
    LOG(DFATAL) << "Checking whether unknown column is specified: " << col_id;
    return false;
  }
  return assigned_[index];
}

void QLTableRow::MarkTombstoned(ColumnIdRep col_id) {
  AllocColumn(col_id).value.Clear();
}

bool QLTableRow::MatchColumn(ColumnIdRep col_id, const QLTableRow& source) const {
  const auto* this_column = FindColumn(col_id);
  const auto* source_column = source.FindColumn(col_id);
  if (this_column && source_column) {
    return this_column->value == source_column->value;
  }
  return !this_column && !source_column;
}

QLTableColumn& QLTableRow::AppendColumn() {
  values_.emplace_back();
  assigned_.push_back(true);
  ++num_assigned_;
  return values_.back();
}

QLTableColumn& QLTableRow::AllocColumn(ColumnIdRep col_id) {
  size_t index = col_id;
  if (index < kFirstNonPreallocatedColumnId && index >= kFirstColumnIdRep) {
    index -= kFirstColumnIdRep;
    // We are in directly mapped part. Ensure that vector is big enough.
    if (values_.size() <= index) {
      // We don't need reserve here, because no allocation would take place.
      if (values_.size() < index) {
        values_.resize(index);
        assigned_.resize(index);
      }
      // This column was not yet allocated, so allocate it. Also vector has `col_id` size, so
      // new column will be added at `col_id` position.
      return AppendColumn();
    }
  } else {
    // We are in part that is mapped using `column_id_to_index_`, so need to allocate at least
    // part that is mapped directly, to avoid index overlapping.
    if (values_.size() < kPreallocatedSize) {
      values_.resize(kPreallocatedSize);
      assigned_.resize(kPreallocatedSize);
    }
    auto iterator_and_flag = column_id_to_index_.emplace(col_id, values_.size());
    if (iterator_and_flag.second) {
      return AppendColumn();
    }
    index = iterator_and_flag.first->second;
  }

  if (!assigned_[index]) {
    assigned_[index] = true;
    ++num_assigned_;
  }
  return values_[index];
}

QLTableColumn& QLTableRow::AllocColumn(ColumnIdRep col_id, const QLValue& ql_value) {
  return AllocColumn(col_id, ql_value.value());
}

QLTableColumn& QLTableRow::AllocColumn(ColumnIdRep col_id, const QLValuePB& ql_value) {
  QLTableColumn& result = AllocColumn(col_id);
  result.value = ql_value;
  return result;
}

QLTableColumn& QLTableRow::AllocColumn(ColumnIdRep col_id, QLValuePB&& ql_value) {
  QLTableColumn& result = AllocColumn(col_id);
  result.value = std::move(ql_value);
  return result;
}

void QLTableRow::CopyColumn(ColumnIdRep col_id, const QLTableRow& source) {
  const auto* value = source.FindColumn(col_id);
  if (value) {
    AllocColumn(col_id) = *value;
    return;
  }

  auto index = ColumnIndex(col_id);
  if (index == kInvalidIndex) {
    return;
  }
  if (assigned_[index]) {
    assigned_[index] = false;
    --num_assigned_;
  }
}

std::string QLTableRow::ToString() const {
  std::string ret("{ ");

  for (size_t i = 0; i != kPreallocatedSize; ++i) {
    if (i >= values_.size()) {
      break;
    }
    if (!assigned_[i]) {
      continue;
    }
    ret.append(Format("$0 => $1 ", i + kFirstColumnIdRep, values_[i]));
  }

  for (auto p : column_id_to_index_) {
    if (!assigned_[p.second]) {
      continue;
    }

    ret.append(Format("$0 => $1 ", p.first, values_[p.second]));
  }

  ret.append("}");
  return ret;
}

std::string QLTableRow::ToString(const Schema& schema) const {
  std::string ret;
  ret.append("{ ");

  for (size_t col_idx = 0; col_idx < schema.num_columns(); col_idx++) {
    const auto* value = GetColumn(schema.column_id(col_idx));
    if (value && value->value_case() != QLValuePB::VALUE_NOT_SET) {
      ret += value->ShortDebugString();
    } else {
      ret += "null";
    }
    ret += ' ';
  }
  ret.append("}");
  return ret;
}

void QLExprResult::MoveToJsonb(common::Jsonb* out) {
  if (existing_value_) {
    out->Assign(existing_value_->jsonb_value());
    existing_value_ = nullptr;
  } else {
    out->Assign(std::move(*value_.mutable_value()->mutable_jsonb_value()));
  }
}

const QLValuePB& QLExprResult::Value() const {
  if (existing_value_) {
    return *existing_value_;
  }

  return value_.value();
}

bool QLExprResult::IsNull() const {
  if (existing_value_) {
    return yb::IsNull(*existing_value_);
  }

  return value_.IsNull();
}

void QLExprResult::MoveTo(QLValuePB* out) {
  if (existing_value_) {
    *out = *existing_value_;
    existing_value_ = nullptr;
  } else {
    value_.mutable_value()->Swap(out);
  }
}

QLValue& QLExprResult::ForceNewValue() {
  if (existing_value_) {
    value_ = *existing_value_;
    existing_value_ = nullptr;
  }

  return value_;
}

QLExprResultWriter QLExprResult::Writer() {
  return QLExprResultWriter(this);
}

void QLExprResultWriter::SetNull() {
  result_->value_.SetNull();
}

void QLExprResultWriter::SetExisting(const QLValuePB* existing_value) {
  result_->existing_value_ = existing_value;
}

QLValue& QLExprResultWriter::NewValue() {
  return result_->value_;
}

} // namespace yb
