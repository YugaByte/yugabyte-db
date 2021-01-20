//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//--------------------------------------------------------------------------------------------------

#include "yb/docdb/doc_expr.h"

#include "yb/common/jsonb.h"
#include "yb/common/ql_value.h"
#include "yb/common/pg_system_attr.h"

#include "yb/client/schema.h"

#include "yb/docdb/subdocument.h"

#include "yb/util/decimal.h"
#include "yb/util/bfql/bfunc.h"

#include "yb/docdb/docdb_pgapi.h"

namespace yb {
namespace docdb {

using yb::util::Decimal;

//--------------------------------------------------------------------------------------------------

DocExprExecutor::DocExprExecutor() {}

DocExprExecutor::~DocExprExecutor() {}

CHECKED_STATUS DocExprExecutor::EvalColumnRef(ColumnIdRep col_id,
                                              const QLTableRow* table_row,
                                              QLExprResultWriter result_writer) {
  // Return NULL value if row is not provided.
  if (table_row == nullptr) {
    result_writer.SetNull();
    return Status::OK();
  }

  // Read value from given row.
  if (col_id >= 0) {
    return table_row->ReadColumn(col_id, result_writer);
  }

  // Read key of the given row.
  if (col_id == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
    return GetTupleId(&result_writer.NewValue());
  }

  return STATUS_SUBSTITUTE(InvalidArgument, "Invalid column ID: $0", col_id);
}

CHECKED_STATUS DocExprExecutor::GetTupleId(QLValue *result) const {
  result->SetNull();
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS DocExprExecutor::EvalTSCall(const QLBCallPB& tscall,
                                           const QLTableRow& table_row,
                                           QLValue *result,
                                           const Schema *schema) {
  bfql::TSOpcode tsopcode = static_cast<bfql::TSOpcode>(tscall.opcode());
  switch (tsopcode) {
    case bfql::TSOpcode::kNoOp:
    case bfql::TSOpcode::kScalarInsert:
      LOG(FATAL) << "Client should not generate function call instruction with operator "
                 << static_cast<int>(tsopcode);
      break;

    case bfql::TSOpcode::kTtl: {
      DCHECK_EQ(tscall.operands().size(), 1) << "WriteTime takes only one argument, a column";
      int64_t ttl_seconds = -1;
      RETURN_NOT_OK(table_row.GetTTL(tscall.operands(0).column_id(), &ttl_seconds));
      if (ttl_seconds != -1) {
        result->set_int64_value(ttl_seconds);
      } else {
        result->SetNull();
      }
      return Status::OK();
    }

    case bfql::TSOpcode::kWriteTime: {
      DCHECK_EQ(tscall.operands().size(), 1) << "WriteTime takes only one argument, a column";
      int64_t write_time = 0;
      RETURN_NOT_OK(table_row.GetWriteTime(tscall.operands(0).column_id(), &write_time));
      result->set_int64_value(write_time);
      return Status::OK();
    }

    case bfql::TSOpcode::kCount:
      if (tscall.operands(0).has_column_id()) {
        // Check if column value is NULL. CQL does not count NULL value of a column.
        QLExprResult arg_result;
        RETURN_NOT_OK(EvalExpr(tscall.operands(0), table_row, arg_result.Writer()));
        if (IsNull(arg_result.Value())) {
          return Status::OK();
        }
      }
      return EvalCount(result);

    case bfql::TSOpcode::kSum: {
      QLExprResult arg_result;
      RETURN_NOT_OK(EvalExpr(tscall.operands(0), table_row, arg_result.Writer()));
      return EvalSum(arg_result.Value(), result);
    }

    case bfql::TSOpcode::kMin: {
      QLExprResult arg_result;
      RETURN_NOT_OK(EvalExpr(tscall.operands(0), table_row, arg_result.Writer()));
      return EvalMin(arg_result.Value(), result);
    }

    case bfql::TSOpcode::kMax: {
      QLExprResult arg_result;
      RETURN_NOT_OK(EvalExpr(tscall.operands(0), table_row, arg_result.Writer()));
      return EvalMax(arg_result.Value(), result);
    }

    case bfql::TSOpcode::kAvg: {
      QLExprResult arg_result;
      RETURN_NOT_OK(EvalExpr(tscall.operands(0), table_row, arg_result.Writer()));
      return EvalAvg(arg_result.Value(), result);
    }

    case bfql::TSOpcode::kMapExtend: FALLTHROUGH_INTENDED;
    case bfql::TSOpcode::kMapRemove: FALLTHROUGH_INTENDED;
    case bfql::TSOpcode::kSetExtend: FALLTHROUGH_INTENDED;
    case bfql::TSOpcode::kSetRemove: FALLTHROUGH_INTENDED;
    case bfql::TSOpcode::kListAppend: {
      // Return the value of the second operand. The first operand must be a column ID.
      QLExprResult temp;
      RETURN_NOT_OK(EvalExpr(tscall.operands(1), table_row, temp.Writer()));
      temp.MoveTo(result->mutable_value());
      return Status::OK();
    }
    case bfql::TSOpcode::kListPrepend: {
      // Return the value of the first operand. The second operand is a column ID.
      QLExprResult temp;
      RETURN_NOT_OK(EvalExpr(tscall.operands(0), table_row, temp.Writer()));
      temp.MoveTo(result->mutable_value());
      return Status::OK();
    }
    case bfql::TSOpcode::kListRemove: {
      QLExprResult org_list_result;
      QLExprResult sub_list_result;
      RETURN_NOT_OK(EvalOperands(
          this, tscall.operands(), table_row, org_list_result.Writer(), sub_list_result.Writer()));
      QLValue org_list_value;
      QLValue sub_list_value;
      org_list_result.MoveTo(org_list_value.mutable_value());
      sub_list_result.MoveTo(sub_list_value.mutable_value());

      result->set_list_value();
      if (!org_list_value.IsNull() && !sub_list_value.IsNull()) {
        QLSeqValuePB* org_list = org_list_value.mutable_list_value();
        QLSeqValuePB* sub_list = sub_list_value.mutable_list_value();
        for (QLValuePB& org_elem : *org_list->mutable_elems()) {
          bool should_remove = false;
          for (QLValuePB& sub_elem : *sub_list->mutable_elems()) {
            if (org_elem == sub_elem) {
              should_remove = true;
              break;
            }
          }
          if (!should_remove) {
            *result->add_list_elem() = std::move(org_elem);
          }
        }
      }
      return Status::OK();
    }

    case bfql::TSOpcode::kToJson:
      return EvalParametricToJson(tscall.operands(0), table_row, result, schema);
  }

  result->SetNull();
  return Status::OK();
}

CHECKED_STATUS DocExprExecutor::EvalTSCall(const PgsqlBCallPB& tscall,
                                           const QLTableRow& table_row,
                                           QLValue *result,
                                           const Schema *schema) {
  bfpg::TSOpcode tsopcode = static_cast<bfpg::TSOpcode>(tscall.opcode());
  switch (tsopcode) {
    case bfpg::TSOpcode::kCount: {
      const auto& operand = tscall.operands(0);
      if (operand.has_column_id()) {
        // Check if column value is NULL. Postgres does not count NULL value of a column, unless
        // it's COUNT(*).
        QLExprResult arg_result;
        RETURN_NOT_OK(EvalExpr(operand, table_row, arg_result.Writer()));
        if (IsNull(arg_result.Value())) {
          return Status::OK();
        }
      } else if (operand.has_value() && QLValue::IsNull(operand.value())) {
        // We've got COUNT(null) which is bound to return zero.
        return Status::OK();
      }
      return EvalCount(result);
    }

    case bfpg::TSOpcode::kSumInt8:
      return EvalSumInt(tscall.operands(0), table_row, result, [](const QLValuePB& value) {
        return value.int8_value();
      });

    case bfpg::TSOpcode::kSumInt16:
      return EvalSumInt(tscall.operands(0), table_row, result, [](const QLValuePB& value) {
        return value.int16_value();
      });

    case bfpg::TSOpcode::kSumInt32:
      return EvalSumInt(tscall.operands(0), table_row, result, [](const QLValuePB& value) {
        return value.int32_value();
      });

    case bfpg::TSOpcode::kSumInt64:
      return EvalSumInt(tscall.operands(0), table_row, result, [](const QLValuePB& value) {
        return value.int64_value();
      });

    case bfpg::TSOpcode::kSumFloat:
      return EvalSumReal(
          tscall.operands(0), table_row, result,
          [](const QLValuePB& value) { return value.float_value(); },
          [](float value, QLValuePB* out) { return out->set_float_value(value); });

    case bfpg::TSOpcode::kSumDouble:
      return EvalSumReal(
          tscall.operands(0), table_row, result,
          [](const QLValuePB& value) { return value.double_value(); },
          [](double value, QLValuePB* out) { return out->set_double_value(value); });

    case bfpg::TSOpcode::kMin: {
      QLExprResult arg_result;
      RETURN_NOT_OK(EvalExpr(tscall.operands(0), table_row, arg_result.Writer()));
      return EvalMin(arg_result.Value(), result);
    }

    case bfpg::TSOpcode::kMax: {
      QLExprResult arg_result;
      RETURN_NOT_OK(EvalExpr(tscall.operands(0), table_row, arg_result.Writer()));
      return EvalMax(arg_result.Value(), result);
    }

    case bfpg::TSOpcode::kPgEvalExprCall: {
      const std::string& expr_str = tscall.operands(0).value().string_value();

      std::vector<DocPgParamDesc> params;
      int num_params = (tscall.operands_size() - 1) / 3;
      params.reserve(num_params);
      for (int i = 0; i < num_params; i++) {
        int32_t attno = tscall.operands(3*i + 1).value().int32_value();
        int32_t typid = tscall.operands(3*i + 2).value().int32_value();
        int32_t typmod = tscall.operands(3*i + 3).value().int32_value();
        params.emplace_back(attno, typid, typmod);
      }

      RETURN_NOT_OK(DocPgEvalExpr(expr_str,
                                  params,
                                  table_row,
                                  schema,
                                  result));

      return Status::OK();
    }

    default:
      LOG(FATAL) << "Client should not generate function call instruction with operator "
                 << static_cast<int>(tsopcode);
      break;
  }

  result->SetNull();
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS DocExprExecutor::EvalCount(QLValue *aggr_count) {
  if (aggr_count->IsNull()) {
    aggr_count->set_int64_value(1);
  } else {
    aggr_count->set_int64_value(aggr_count->int64_value() + 1);
  }
  return Status::OK();
}

CHECKED_STATUS DocExprExecutor::EvalSum(const QLValuePB& val, QLValue *aggr_sum) {
  if (IsNull(val)) {
    return Status::OK();
  }

  if (aggr_sum->IsNull()) {
    *aggr_sum = val;
    return Status::OK();
  }

  switch (aggr_sum->type()) {
    case InternalType::kInt8Value:
      aggr_sum->set_int8_value(aggr_sum->int8_value() + val.int8_value());
      break;
    case InternalType::kInt16Value:
      aggr_sum->set_int16_value(aggr_sum->int16_value() + val.int16_value());
      break;
    case InternalType::kInt32Value:
      aggr_sum->set_int32_value(aggr_sum->int32_value() + val.int32_value());
      break;
    case InternalType::kInt64Value:
      aggr_sum->set_int64_value(aggr_sum->int64_value() + val.int64_value());
      break;
    case InternalType::kVarintValue:
      aggr_sum->set_varint_value(aggr_sum->varint_value() + QLValue::varint_value(val));
      break;
    case InternalType::kFloatValue:
      aggr_sum->set_float_value(aggr_sum->float_value() + val.float_value());
      break;
    case InternalType::kDoubleValue:
      aggr_sum->set_double_value(aggr_sum->double_value() + val.double_value());
      break;
    case InternalType::kDecimalValue: {
      Decimal sum, value;
      RETURN_NOT_OK(sum.DecodeFromComparable(aggr_sum->decimal_value()));
      RETURN_NOT_OK(value.DecodeFromComparable(val.decimal_value()));
      sum = sum + value;
      aggr_sum->set_decimal_value(sum.EncodeToComparable());
      break;
    }
    default:
      return STATUS(RuntimeError, "Cannot find SUM of this column");
  }
  return Status::OK();
}

template <class Extractor>
CHECKED_STATUS DocExprExecutor::EvalSumInt(
    const PgsqlExpressionPB& operand, const QLTableRow& table_row, QLValue *aggr_sum,
    const Extractor& extractor) {
  QLExprResult arg_result;
  RETURN_NOT_OK(EvalExpr(operand, table_row, arg_result.Writer()));
  const auto& val = arg_result.Value();

  if (IsNull(val)) {
    return Status::OK();
  }

  if (aggr_sum->IsNull()) {
    aggr_sum->set_int64_value(extractor(val));
  } else {
    aggr_sum->set_int64_value(aggr_sum->int64_value() + extractor(val));
  }

  return Status::OK();
}

template <class Extractor, class Setter>
CHECKED_STATUS DocExprExecutor::EvalSumReal(
    const PgsqlExpressionPB& operand, const QLTableRow& table_row, QLValue *aggr_sum,
    const Extractor& extractor, const Setter& setter) {
  QLExprResult arg_result;
  RETURN_NOT_OK(EvalExpr(operand, table_row, arg_result.Writer()));
  const auto& val = arg_result.Value();

  if (IsNull(val)) {
    return Status::OK();
  }

  if (aggr_sum->IsNull()) {
    setter(extractor(val), aggr_sum->mutable_value());
  } else {
    setter(extractor(aggr_sum->value()) + extractor(val), aggr_sum->mutable_value());
  }

  return Status::OK();
}

CHECKED_STATUS DocExprExecutor::EvalMax(const QLValuePB& val, QLValue *aggr_max) {
  if (!IsNull(val) && (aggr_max->IsNull() || aggr_max->value() < val)) {
    *aggr_max = val;
  }
  return Status::OK();
}

CHECKED_STATUS DocExprExecutor::EvalMin(const QLValuePB& val, QLValue *aggr_min) {
  if (!IsNull(val) && (aggr_min->IsNull() || aggr_min->value() > val)) {
    *aggr_min = val;
  }
  return Status::OK();
}

CHECKED_STATUS DocExprExecutor::EvalAvg(const QLValuePB& val, QLValue *aggr_avg) {
  if (IsNull(val)) {
    return Status::OK();
  }

  QLValue sum, count;

  if (aggr_avg->IsNull()) {
    sum = val;
    count.set_int64_value(1);
    aggr_avg->set_map_value();
    *aggr_avg->add_map_key() = count.value();
    *aggr_avg->add_map_value() = sum.value();
    return Status::OK();
  }

  QLMapValuePB* map = aggr_avg->mutable_map_value();
  sum = QLValue(map->values(0));
  RETURN_NOT_OK(EvalSum(val, &sum));
  count = QLValue(map->keys(0));
  count.set_int64_value(count.int64_value() + 1);
  *map->mutable_keys(0) = count.value();
  *map->mutable_values(0) = sum.value();
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

namespace {

void UnpackUDTAndFrozen(const QLType::SharedPtr& type, QLValuePB* value) {
  if (type->IsUserDefined() && value->value_case() == QLValuePB::kMapValue) {
    // Change MAP<field_index:field_value> into MAP<field_name:field_value>
    // in case of UDT.
    const vector<string> field_names = type->udtype_field_names();
    QLMapValuePB* map = value->mutable_map_value();
    for (int i = 0; i < map->keys_size(); ++i) {
      map->mutable_keys(i)->set_string_value(field_names[i]);
    }
  } else if (type->IsFrozen() && value->value_case() == QLValuePB::kFrozenValue) {
    if (type->param_type()->IsUserDefined()) {
      // Change FROZEN[field_value,...] into MAP<field_name:field_value>
      // in case of FROZEN<UDT>.
      const vector<string> field_names = type->param_type()->udtype_field_names();
      QLSeqValuePB seq(value->frozen_value());
      DCHECK_EQ(seq.elems_size(), field_names.size());
      QLMapValuePB* map = value->mutable_map_value();

      if (seq.elems_size() == field_names.size()) {
        for (int i = 0; i < seq.elems_size(); ++i) {
          map->add_keys()->set_string_value(field_names[i]);
          *(map->add_values()) = seq.elems(i);
        }
      }
    } else if (type->param_type()->main() == MAP) {
      // Case: FROZEN<MAP>=[Key1,Value1,Key2,Value2] -> MAP<Key1:Value1, Key2:Value2>.
      QLSeqValuePB seq(value->frozen_value());
      DCHECK_EQ(seq.elems_size() % 2, 0);
      QLMapValuePB* map = value->mutable_map_value();

      for (int i = 0; i < seq.elems_size();) {
        QLValuePB* const key = map->add_keys();
        *key = seq.elems(i++);
        UnpackUDTAndFrozen(type->param_type()->keys_type(), key);

        QLValuePB* const value = map->add_values();
        *value = seq.elems(i++);
        UnpackUDTAndFrozen(type->param_type()->values_type(), value);
      }
    } else {
      DCHECK(type->param_type()->main() == LIST || type->param_type()->main() == SET);
      // Case: FROZEN<LIST/SET>
      QLSeqValuePB* seq = value->mutable_frozen_value();
      for (int i = 0; i < seq->elems_size(); ++i) {
        UnpackUDTAndFrozen(type->param_type()->param_type(), seq->mutable_elems(i));
      }
    }
  } else if (type->main() == LIST && value->value_case() == QLValuePB::kListValue) {
    QLSeqValuePB* seq = value->mutable_list_value();
    for (int i = 0; i < seq->elems_size(); ++i) {
      UnpackUDTAndFrozen(type->param_type(), seq->mutable_elems(i));
    }
  } else if (type->main() == SET && value->value_case() == QLValuePB::kSetValue) {
    QLSeqValuePB* seq = value->mutable_set_value();
    for (int i = 0; i < seq->elems_size(); ++i) {
      UnpackUDTAndFrozen(type->param_type(), seq->mutable_elems(i));
    }
  } else if (type->main() == MAP && value->value_case() == QLValuePB::kMapValue) {
    QLMapValuePB* map = value->mutable_map_value();
    DCHECK_EQ(map->keys_size(), map->values_size());
    for (int i = 0; i < map->keys_size(); ++i) {
      UnpackUDTAndFrozen(type->keys_type(), map->mutable_keys(i));
      UnpackUDTAndFrozen(type->values_type(), map->mutable_values(i));
    }
  } else if (type->main() == TUPLE) {
    // https://github.com/YugaByte/yugabyte-db/issues/936
    LOG(FATAL) << "Tuple type not implemented yet";
  }
}

} // namespace

CHECKED_STATUS DocExprExecutor::EvalParametricToJson(const QLExpressionPB& operand,
                                                     const QLTableRow& table_row,
                                                     QLValue *result,
                                                     const Schema *schema) {
  QLExprResult val;
  RETURN_NOT_OK(EvalExpr(operand, table_row, val.Writer(), schema));

  // Repack parametric types like UDT, FROZEN, SET<FROZEN>, etc.
  if (operand.has_column_id() && schema != nullptr) {
    Result<const ColumnSchema&> col = schema->column_by_id(ColumnId(operand.column_id()));
    DCHECK(col.ok());

    if (col.ok()) {
      UnpackUDTAndFrozen(col->type(), val.ForceNewValue().mutable_value());
    }
  }

  // Direct call of ToJson() for elementary type.
  return bfql::ToJson(&val.ForceNewValue(), result);
}

//--------------------------------------------------------------------------------------------------

}  // namespace docdb
}  // namespace yb
