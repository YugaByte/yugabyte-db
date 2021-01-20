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

#include <iostream>

#include "yb/common/ql_expr.h"
#include "yb/common/ql_value.h"
#include "yb/docdb/primitive_value_util.h"

using std::vector;

namespace yb {
namespace docdb {

// Add primary key column values to the component group. Verify that they are in the same order
// as in the table schema.
Status QLKeyColumnValuesToPrimitiveValues(
    const google::protobuf::RepeatedPtrField<QLExpressionPB> &column_values,
    const Schema &schema, size_t column_idx, const size_t column_count,
    vector<PrimitiveValue> *components) {
  for (const auto& column_value : column_values) {
    if (!schema.is_key_column(column_idx)) {
      auto status = STATUS_FORMAT(
          Corruption, "Column at $0 is not key column in $1", column_idx, schema.ToString());
      LOG(DFATAL) << status;
      return status;
    }

    if (!column_value.has_value() || IsNull(column_value.value())) {
      components->push_back(PrimitiveValue(ValueType::kNullLow));
    } else {
      components->push_back(PrimitiveValue::FromQLValuePB(
          column_value.value(), schema.column(column_idx).sorting_type()));
    }
    column_idx++;
  }
  return Status::OK();
}

// ------------------------------------------------------------------------------------------------
Result<vector<PrimitiveValue>> InitKeyColumnPrimitiveValues(
    const google::protobuf::RepeatedPtrField<PgsqlExpressionPB> &column_values,
    const Schema &schema,
    size_t start_idx) {
  vector<PrimitiveValue> values;
  values.reserve(column_values.size());
  size_t column_idx = start_idx;
  for (const auto& column_value : column_values) {
    if (!schema.is_key_column(column_idx)) {
      auto status = STATUS_FORMAT(
          Corruption, "Column at $0 is not key column in $1", column_idx, schema.ToString());
      LOG(DFATAL) << status;
      return status;
    }
    const auto sorting_type = schema.column(column_idx).sorting_type();
    if (column_value.has_value()) {
      const auto& value = column_value.value();
      values.push_back(IsNull(value) ? PrimitiveValue::NullValue(sorting_type)
                                     : PrimitiveValue::FromQLValuePB(value, sorting_type));
    } else {
      // TODO(neil) The current setup only works for CQL as it assumes primary key value must not
      // be dependent on any column values. This needs to be fixed as PostgreSQL expression might
      // require a read from a table.
      //
      // Use regular executor for now.
      QLExprExecutor executor;
      QLExprResult result;
      RETURN_NOT_OK(executor.EvalExpr(column_value, nullptr, result.Writer(), &schema));

      values.push_back(PrimitiveValue::FromQLValuePB(result.Value(), sorting_type));
    }
    column_idx++;
  }
  return std::move(values);
}

}  // namespace docdb
}  // namespace yb
