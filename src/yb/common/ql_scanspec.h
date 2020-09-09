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
//
// This file contains QLScanSpec that implements a QL scan (SELECT) specification.

#ifndef YB_COMMON_QL_SCANSPEC_H
#define YB_COMMON_QL_SCANSPEC_H

#include <map>

#include "yb/common/schema.h"
#include "yb/common/ql_protocol.pb.h"
#include "yb/common/ql_rowblock.h"

namespace yb {
namespace common {

//--------------------------------------------------------------------------------------------------
// YQL Scanspec.
// This class represents all scan specifications.
//--------------------------------------------------------------------------------------------------
class YQLScanSpec {
 public:
  explicit YQLScanSpec(QLClient client_type) : client_type_(client_type) {
  }

  virtual ~YQLScanSpec() {
  }

  QLClient client_type() const {
    return client_type_;
  }

 private:
  const QLClient client_type_;
};

//--------------------------------------------------------------------------------------------------
// CQL Support.
//--------------------------------------------------------------------------------------------------

// A class to determine the lower/upper-bound range components of a QL scan from its WHERE
// condition.
class QLScanRange {
 public:

  // Value range of a column
  struct QLRange {
    boost::optional<QLValuePB> min_value;
    boost::optional<QLValuePB> max_value;
  };

  QLScanRange(const Schema& schema, const QLConditionPB& condition);
  QLScanRange(const Schema& schema, const PgsqlConditionPB& condition);

  QLRange RangeFor(ColumnId col_id) const {
    const auto& iter = ranges_.find(col_id);
    return (iter == ranges_.end() ? QLRange() : iter->second);
  }

  bool has_in_range_options() const {
    return has_in_range_options_;
  }

  // Intersect / union / complement operators.
  QLScanRange& operator&=(const QLScanRange& other);
  QLScanRange& operator|=(const QLScanRange& other);
  QLScanRange& operator~();

  QLScanRange& operator=(QLScanRange&& other);

 private:

  // Table schema being scanned.
  const Schema& schema_;

  // Mapping of column id to the column value ranges (inclusive lower/upper bounds) to scan.
  std::unordered_map<ColumnId, QLRange> ranges_;

  // Whether the condition has an IN condition on a range (clustering) column.
  // Used in doc_ql_scanspec to try to construct the set of options for a multi-point scan.
  bool has_in_range_options_ = false;
};

// A scan specification for a QL scan. It may be used to scan either a specified doc key
// or a hash key + optional WHERE condition clause.
class QLScanSpec : public YQLScanSpec {
 public:
  explicit QLScanSpec(QLExprExecutorPtr executor = nullptr);

  // Scan for the given hash key and a condition.
  QLScanSpec(const QLConditionPB* condition,
             const QLConditionPB* if_condition,
             const bool is_forward_scan,
             QLExprExecutorPtr executor = nullptr);

  virtual ~QLScanSpec() {}

  // Evaluate the WHERE condition for the given row to decide if it is selected or not.
  // virtual to make the class polymorphic.
  virtual CHECKED_STATUS Match(const QLTableRow& table_row, bool* match) const;

  bool is_forward_scan() const {
    return is_forward_scan_;
  }

  // Get Schema if available.
  virtual const Schema* schema() const { return nullptr; }

 protected:
  const QLConditionPB* condition_;
  const QLConditionPB* if_condition_;
  const bool is_forward_scan_;
  QLExprExecutorPtr executor_;
};

//--------------------------------------------------------------------------------------------------
// PostgreSQL Support.
//--------------------------------------------------------------------------------------------------
class PgsqlScanSpec : public YQLScanSpec {
 public:
  typedef std::unique_ptr<common::PgsqlScanSpec> UniPtr;

  explicit PgsqlScanSpec(const PgsqlExpressionPB *where_expr,
                         QLExprExecutorPtr executor = nullptr);

  virtual ~PgsqlScanSpec();

  const PgsqlExpressionPB *where_expr() {
    return where_expr_;
  }

 protected:
  const PgsqlExpressionPB *where_expr_;
  QLExprExecutorPtr executor_;
};

} // namespace common
} // namespace yb

#endif // YB_COMMON_QL_SCANSPEC_H
