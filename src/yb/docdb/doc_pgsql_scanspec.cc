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

#include "yb/docdb/doc_pgsql_scanspec.h"

#include "yb/common/pgsql_protocol.pb.h"
#include "yb/common/ql_value.h"

#include "yb/docdb/doc_expr.h"
#include "yb/docdb/doc_scanspec_util.h"
#include "yb/rocksdb/db/compaction.h"

namespace yb {
namespace docdb {

//--------------------------------------------------------------------------------------------------
extern rocksdb::UserBoundaryTag TagForRangeComponent(size_t index);

// TODO(neil) The following implementation is just a prototype. Need to complete the implementation
// and test accordingly.
class PgsqlRangeBasedFileFilter : public rocksdb::ReadFileFilter {
 public:
  PgsqlRangeBasedFileFilter(const std::vector<PrimitiveValue>& lower_bounds,
                            const std::vector<PrimitiveValue>& upper_bounds)
      : lower_bounds_(EncodePrimitiveValues(lower_bounds, upper_bounds.size())),
        upper_bounds_(EncodePrimitiveValues(upper_bounds, lower_bounds.size())) {
  }

  std::vector<KeyBytes> EncodePrimitiveValues(const std::vector<PrimitiveValue>& source,
                                              size_t min_size) {
    size_t size = source.size();
    std::vector<KeyBytes> result(std::max(min_size, size));
    for (size_t i = 0; i != size; ++i) {
      if (source[i].value_type() != ValueType::kTombstone) {
        source[i].AppendToKey(&result[i]);
      }
    }
    return result;
  }

  bool Filter(const rocksdb::FdWithBoundaries& file) const override {
    for (size_t i = 0; i != lower_bounds_.size(); ++i) {
      const Slice lower_bound = lower_bounds_[i].AsSlice();
      const Slice upper_bound = upper_bounds_[i].AsSlice();

      rocksdb::UserBoundaryTag tag = TagForRangeComponent(i);
      const Slice *smallest = file.smallest.user_value_with_tag(tag);
      const Slice *largest = file.largest.user_value_with_tag(tag);

      if (!GreaterOrEquals(&upper_bound, smallest) || !GreaterOrEquals(largest, &lower_bound)) {
        return false;
      }
    }
    return true;
  }

  bool GreaterOrEquals(const Slice *lhs, const Slice *rhs) const {
    // TODO(neil) Need to double check this NULL-equals-all logic or make the code clearer.
    if (lhs == nullptr || rhs == nullptr) {
      return true;
    }
    if (lhs->empty() || rhs->empty()) {
      return true;
    }
    return lhs->compare(*rhs) >= 0;
  }

 private:
  std::vector<KeyBytes> lower_bounds_;
  std::vector<KeyBytes> upper_bounds_;
};

//--------------------------------------------------------------------------------------------------

DocPgsqlScanSpec::DocPgsqlScanSpec(const Schema& schema,
                                   const rocksdb::QueryId query_id,
                                   const DocKey& doc_key,
                                   const DocKey& start_doc_key,
                                   bool is_forward_scan)
    : PgsqlScanSpec(nullptr),
      schema_(schema),
      query_id_(query_id),
      hashed_components_(nullptr),
      start_doc_key_(start_doc_key.empty() ? KeyBytes() : start_doc_key.Encode()),
      lower_doc_key_(doc_key.Encode()),
      is_forward_scan_(is_forward_scan) {

  // Compute lower and upper doc_key.
  // We add +inf as an extra component to make sure this is greater than all keys in range.
  // For lower bound, this is true already, because dockey + suffix is > dockey
  upper_doc_key_ = lower_doc_key_;
  upper_doc_key_.AppendValueTypeBeforeGroupEnd(ValueType::kHighest);
}

DocPgsqlScanSpec::DocPgsqlScanSpec(const Schema& schema,
                                   const rocksdb::QueryId query_id,
                                   const std::vector<PrimitiveValue>& hashed_components,
                                   const PgsqlConditionPB* condition,
                                   const boost::optional<int32_t> hash_code,
                                   const boost::optional<int32_t> max_hash_code,
                                   const PgsqlExpressionPB *where_expr,
                                   const DocKey& start_doc_key,
                                   bool is_forward_scan)
    : PgsqlScanSpec(where_expr),
      range_bounds_(condition ? new common::QLScanRange(schema, *condition) : nullptr),
      schema_(schema),
      query_id_(query_id),
      hashed_components_(&hashed_components),
      hash_code_(hash_code),
      max_hash_code_(max_hash_code),
      start_doc_key_(start_doc_key.empty() ? KeyBytes() : start_doc_key.Encode()),
      lower_doc_key_(bound_key(schema, true)),
      upper_doc_key_(bound_key(schema, false)),
      is_forward_scan_(is_forward_scan) {
  if (where_expr_) {
    // Should never get here until WHERE clause is supported.
    LOG(FATAL) << "DEVELOPERS: Add support for condition (where clause)";
  }

  // If the hash key is fixed and we have range columns with IN condition, try to construct the
  // exact list of range options to scan for.
  if ((!hashed_components_->empty() || schema_.num_hash_key_columns() == 0) &&
      schema_.num_range_key_columns() > 0 &&
      range_bounds_ && range_bounds_->has_in_range_options()) {
    DCHECK(condition);
    range_options_ =
        std::make_shared<std::vector<std::vector<PrimitiveValue>>>(schema_.num_range_key_columns());
    InitRangeOptions(*condition);

    // Range options are only valid if all range columns are set (i.e. have one or more options).
    for (int i = 0; i < schema_.num_range_key_columns(); i++) {
      if ((*range_options_)[i].empty()) {
        range_options_ = nullptr;
        break;
      }
    }
  }
}

void DocPgsqlScanSpec::InitRangeOptions(const PgsqlConditionPB& condition) {
  size_t num_hash_cols = schema_.num_hash_key_columns();
  switch (condition.op()) {
    case QLOperator::QL_OP_AND:
      for (const auto& operand : condition.operands()) {
        DCHECK(operand.has_condition());
        InitRangeOptions(operand.condition());
      }
      break;

    case QLOperator::QL_OP_EQUAL:
    case QLOperator::QL_OP_IN: {
      DCHECK_EQ(condition.operands_size(), 2);
      // Skip any condition where LHS is not a column (e.g. subscript columns: 'map[k] = v')
      if (condition.operands(0).expr_case() != PgsqlExpressionPB::kColumnId) {
        return;
      }

      // Skip any RHS expressions that are not evaluated yet.
      if (condition.operands(1).expr_case() != PgsqlExpressionPB::kValue) {
        return;
      }

      int col_idx = schema_.find_column_by_id(ColumnId(condition.operands(0).column_id()));

      // Skip any non-range columns.
      if (!schema_.is_range_column(col_idx)) {
        return;
      }

      ColumnSchema::SortingType sortingType = schema_.column(col_idx).sorting_type();

      if (condition.op() == QL_OP_EQUAL) {
        auto pv = PrimitiveValue::FromQLValuePB(condition.operands(1).value(), sortingType);
        (*range_options_)[col_idx - num_hash_cols].push_back(std::move(pv));
      } else { // QL_OP_IN
        DCHECK_EQ(condition.op(), QL_OP_IN);
        DCHECK(condition.operands(1).value().has_list_value());
        const auto &options = condition.operands(1).value().list_value();
        int opt_size = options.elems_size();
        (*range_options_)[col_idx - num_hash_cols].reserve(opt_size);

        // IN arguments should have been de-duplicated and ordered ascendingly by the executor.
        bool is_reverse_order = is_forward_scan_ ^ (sortingType == ColumnSchema::kAscending ||
            sortingType == ColumnSchema::kAscendingNullsLast);
        for (int i = 0; i < opt_size; i++) {
          int elem_idx = is_reverse_order ? opt_size - i - 1 : i;
          const auto &elem = options.elems(elem_idx);
          auto pv = PrimitiveValue::FromQLValuePB(elem, sortingType);
          (*range_options_)[col_idx - num_hash_cols].push_back(std::move(pv));
        }
      }

      break;
    }

    default:
      // We don't support any other operators at this level.
      break;
  }
}

KeyBytes DocPgsqlScanSpec::bound_key(const Schema& schema, const bool lower_bound) const {
  KeyBytes result;
  auto encoder = DocKeyEncoder(&result).Schema(schema);

  bool has_hash_columns = schema_.num_hash_key_columns() > 0;
  bool hash_components_unset = has_hash_columns && hashed_components_->empty();
  if (hash_components_unset) {
    // use lower bound hash code if set in request (for scans using token)
    if (lower_bound && hash_code_) {
      encoder.HashAndRange(*hash_code_, { PrimitiveValue(ValueType::kLowest) }, {});
    }
    // use upper bound hash code if set in request (for scans using token)
    if (!lower_bound) {
      if (max_hash_code_) {
        encoder.HashAndRange(*max_hash_code_, { PrimitiveValue(ValueType::kHighest) }, {});
      } else {
        result.AppendValueTypeBeforeGroupEnd(ValueType::kHighest);
      }
    }
    return result;
  }

  if (has_hash_columns) {
    DocKeyHash min_hash = hash_code_ ?
        static_cast<DocKeyHash> (*hash_code_) : std::numeric_limits<DocKeyHash>::min();
    DocKeyHash max_hash = max_hash_code_ ?
        static_cast<DocKeyHash> (*max_hash_code_) : std::numeric_limits<DocKeyHash>::max();

    encoder.HashAndRange(lower_bound ? min_hash : max_hash,
                         *hashed_components_,
                         range_components(lower_bound));
  } else {
    // If no hash columns use default hash code (0).
    encoder.Hash(false, 0, *hashed_components_).Range(range_components(lower_bound));
  }
  return result;
}

std::vector<PrimitiveValue> DocPgsqlScanSpec::range_components(const bool lower_bound) const {
  return GetRangeKeyScanSpec(schema_, range_bounds_.get(), lower_bound);
}

// Return inclusive lower/upper range doc key considering the start_doc_key.
Result<KeyBytes> DocPgsqlScanSpec::Bound(const bool lower_bound) const {
  if (start_doc_key_.empty()) {
    return lower_bound ? lower_doc_key_ : upper_doc_key_;
  }

  // When paging state is present, start_doc_key_ should have been provided, and the scan starting
  // point should be start_doc_key_ instead of the initial bounds.
  if (start_doc_key_ < lower_doc_key_ || start_doc_key_ > upper_doc_key_) {
    return STATUS_FORMAT(Corruption, "Invalid start_doc_key: $0. Range: $1, $2",
                         start_doc_key_, lower_doc_key_, upper_doc_key_);
  }

  // Paging state + forward scan.
  if (is_forward_scan_) {
    return lower_bound ? start_doc_key_ : upper_doc_key_;
  }

  // Paging state + reverse scan.
  if (lower_bound) {
    return lower_doc_key_;
  }

  // If using start_doc_key_ as upper bound append +inf as extra component to ensure it includes
  // the target start_doc_key itself (dockey + suffix < dockey + kHighest).
  // For lower bound, this is true already, because dockey + suffix is > dockey.
  KeyBytes result = start_doc_key_;
  result.AppendValueTypeBeforeGroupEnd(ValueType::kHighest);
  return result;
}

std::shared_ptr<rocksdb::ReadFileFilter> DocPgsqlScanSpec::CreateFileFilter() const {
  auto lower_bound = range_components(true);
  auto upper_bound = range_components(false);
  if (lower_bound.empty() && upper_bound.empty()) {
    return std::shared_ptr<rocksdb::ReadFileFilter>();
  } else {
    return std::make_shared<PgsqlRangeBasedFileFilter>(std::move(lower_bound),
                                                       std::move(upper_bound));
  }
}

}  // namespace docdb
}  // namespace yb
