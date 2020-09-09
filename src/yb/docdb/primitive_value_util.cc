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
Status InitKeyColumnPrimitiveValues(
    const google::protobuf::RepeatedPtrField<PgsqlExpressionPB> &column_values,
    const Schema &schema,
    size_t start_idx,
    vector<PrimitiveValue> *components) {
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
      components->push_back(IsNull(value) ? PrimitiveValue::NullValue(sorting_type)
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

      components->push_back(PrimitiveValue::FromQLValuePB(result.Value(), sorting_type));
    }
    column_idx++;
  }
  return Status::OK();
}

namespace {
  template <typename TemplatePB>
  boost::optional<int32_t> HashCodeFromPB(const TemplatePB pb) {
    return pb.has_hash_code() ? boost::make_optional<int32_t>(pb.hash_code()) : boost::none;
  }

  template <typename TemplatePB>
  boost::optional<int32_t> MaxHashCodeFromPB(const TemplatePB pb) {
    return pb.has_max_hash_code() ? boost::make_optional<int32_t>(pb.max_hash_code()) : boost::none;
  }
} // namespace

boost::optional<int32_t> DocHashCode(const PgsqlReadRequestPB& request,
                                     int64_t batch_arg_index) {
  if (batch_arg_index < 0) {
    return HashCodeFromPB(request);
  }
  return HashCodeFromPB(request.batch_arguments(batch_arg_index));
}

boost::optional<int32_t> DocMaxHashCode(const PgsqlReadRequestPB& request,
                                        int64_t batch_arg_index) {
  if (batch_arg_index < 0) {
    return MaxHashCodeFromPB(request);
  }
  return MaxHashCodeFromPB(request.batch_arguments(batch_arg_index));
}

bool
DocHasPartitionValues(const PgsqlReadRequestPB& request, int64_t batch_arg_index) {
  return batch_arg_index < 0 ? request.partition_column_values_size() > 0
      : request.batch_arguments(batch_arg_index).partition_column_values_size() > 0;
}

const google::protobuf::RepeatedPtrField<PgsqlExpressionPB>&
DocPartitionValues(const PgsqlReadRequestPB& request, int64_t batch_arg_index) {
  return batch_arg_index < 0 ? request.partition_column_values()
                             : request.batch_arguments(batch_arg_index).partition_column_values();
}

bool
DocHasRangeValues(const PgsqlReadRequestPB& request, int64_t batch_arg_index) {
  // Use shared range value in "request" because currently we do not batch RANGE values.
  return request.range_column_values().size() > 0;
}

const google::protobuf::RepeatedPtrField<PgsqlExpressionPB>&
DocRangeValues(const PgsqlReadRequestPB& request, int64_t batch_arg_index) {
  // Use shared range value in "request" because currently we do not batch RANGE values.
  return request.range_column_values();
}

}  // namespace docdb
}  // namespace yb
