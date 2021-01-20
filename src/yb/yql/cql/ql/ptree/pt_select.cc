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
//
// Treenode definitions for SELECT statements.
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/ptree/pt_select.h"

#include <functional>

#include "yb/client/client.h"
#include "yb/client/table.h"

#include "yb/common/index.h"
#include "yb/yql/cql/ql/ptree/sem_context.h"
#include "yb/util/flag_tags.h"

DEFINE_bool(enable_uncovered_index_select, true,
            "Enable executing select statements using uncovered index");
TAG_FLAG(enable_uncovered_index_select, advanced);

namespace yb {
namespace ql {

using std::make_shared;
using std::string;
using std::unordered_map;
using std::vector;
using yb::bfql::TSOpcode;

//--------------------------------------------------------------------------------------------------

namespace {

// Selectivity of a column operator.
YB_DEFINE_ENUM(OpSelectivity, (kEqual)(kRange)(kNone));

// Returns the selectivity of a column operator.
OpSelectivity GetOperatorSelectivity(const QLOperator op) {
  switch (op) {
    case QL_OP_EQUAL: FALLTHROUGH_INTENDED;
    case QL_OP_IN:
      return OpSelectivity::kEqual;
    case QL_OP_GREATER_THAN: FALLTHROUGH_INTENDED;
    case QL_OP_GREATER_THAN_EQUAL: FALLTHROUGH_INTENDED;
    case QL_OP_LESS_THAN: FALLTHROUGH_INTENDED;
    case QL_OP_LESS_THAN_EQUAL:
      return OpSelectivity::kRange;
    case QL_OP_NOOP: FALLTHROUGH_INTENDED;
    case QL_OP_NOT_IN:
      break;
    default:
      // Should have been caught beforehand (as this is called in pt_select after analyzing the
      // where clause).
      break;
  }
  return OpSelectivity::kNone;
}

// Class to compare selectivity of an index for a SELECT statement.
class Selectivity {
 public:
  // Selectivity of the indexed table.
  Selectivity(MemoryContext *memctx, const PTSelectStmt& stmt)
      : is_local_(true),
        covers_fully_(true) {
    const client::YBSchema& schema = stmt.table()->schema();
    MCIdToIndexMap id_to_idx(memctx);
    for (size_t i = 0; i < schema.num_key_columns(); i++) {
      id_to_idx.emplace(schema.ColumnId(i), i);
    }
    Analyze(memctx, stmt, id_to_idx, schema.num_key_columns(), schema.num_hash_key_columns());
  }

  // Selectivity of an index.
  Selectivity(MemoryContext *memctx, const PTSelectStmt& stmt, const IndexInfo& index_info)
      : index_id_(index_info.table_id()),
        is_local_(index_info.is_local()),
        covers_fully_(stmt.CoversFully(index_info)),
        index_info_(&index_info) {

    MCIdToIndexMap id_to_idx(memctx);
    for (size_t i = 0; i < index_info.key_column_count(); i++) {
      // Map the column id if the index expression is just a column-ref.
      if (index_info.column(i).colexpr.expr_case() == QLExpressionPB::ExprCase::EXPR_NOT_SET ||
          index_info.column(i).colexpr.expr_case() == QLExpressionPB::ExprCase::kColumnId) {
        id_to_idx.emplace(index_info.column(i).indexed_column_id, i);
      }
    }
    Analyze(memctx, stmt, id_to_idx, index_info.key_column_count(), index_info.hash_column_count());
  }

  bool covers_fully() const { return covers_fully_; }

  const TableId& index_id() const { return index_id_; }

  // Comparison operator to sort the selectivity of an index.
  bool operator>(const Selectivity& other) const {
    // If one is a single-key read and the other is not, prefer the one that is.
    if (single_key_read_ != other.single_key_read_) {
      return single_key_read_ > other.single_key_read_;
    }

    // If one is a full-table scan and the other is not, prefer the one that is not.
    if (full_table_scan_ != other.full_table_scan_) {
      return full_table_scan_ < other.full_table_scan_;
    }

    // When neither is a full table scan, compare the scan ranges.
    if (!full_table_scan_ && !other.full_table_scan_) {

      // If the fully-specified prefixes are different, prefer the one with longer prefix.
      if (prefix_length_ != other.prefix_length_) {
        return prefix_length_ > other.prefix_length_;
      }

      // If one has a range clause after the fully specified prefix and the other does not, prefer
      // the one that does.
      if (ends_with_range_ != other.ends_with_range_) {
        return ends_with_range_ > other.ends_with_range_;
      }

      // If the numbers of non-primary-key column operators needs to be evaluated are different,
      // prefer the one with less.
      if (num_non_key_ops_ != other.num_non_key_ops_) {
        return num_non_key_ops_ < other.num_non_key_ops_;
      }
    }

    // If one covers the read fully and the other does not, prefer the one that does.
    if (covers_fully_ != other.covers_fully_) {
      return covers_fully_ > other.covers_fully_;
    }

    // If one is local read and the other is not, prefer the one that is.
    if (is_local_ != other.is_local_) {
      return is_local_ > other.is_local_;
    }

    // When all the above are equal, prefer the indexed table over the index.
    return index_id_.empty() > other.index_id_.empty();
  }

  string ToString() const {
    return strings::Substitute("Selectivity: index_id $0 is_local $1 prefix_length $2 "
                               "single_key_read $3 full_table_scan $4 ends_with_range $5 "
                               "covers_fully $6", index_id_, is_local_, prefix_length_,
                               single_key_read_, full_table_scan_, ends_with_range_,
                               covers_fully_);
  }

 private:
  // Analyze selectivity, currently defined as length of longest fully specified prefix and
  // whether there is a range operator immediately after the prefix.
  using MCIdToIndexMap = MCUnorderedMap<int, size_t>;
  void Analyze(MemoryContext *memctx,
               const PTSelectStmt& stmt,
               const MCIdToIndexMap& id_to_idx,
               const size_t num_key_columns,
               const size_t num_hash_key_columns) {

    // NOTE: Instead of "id_to_idx" mapping, we can also use "index_info_->FindKeyIndex()" for
    // ColumnRef expressions, the same way as JsonRef and SubscriptRef expressions.  However,
    // "id_to_idx" mapping is more efficient, so don't remove this map.

    // The operator on each column, in the order of the columns in the table or index we analyze.
    MCVector<OpSelectivity> ops(id_to_idx.size(), OpSelectivity::kNone, memctx);
    for (const ColumnOp& col_op : stmt.key_where_ops()) {
      const auto iter = id_to_idx.find(col_op.desc()->id());
      if (iter != id_to_idx.end()) {
        ops[iter->second] = GetOperatorSelectivity(col_op.yb_op());
      } else {
        num_non_key_ops_++;
      }
    }
    for (const ColumnOp& col_op : stmt.where_ops()) {
      const auto iter = id_to_idx.find(col_op.desc()->id());
      if (iter != id_to_idx.end()) {
        ops[iter->second] = GetOperatorSelectivity(col_op.yb_op());
      } else {
        num_non_key_ops_++;
      }
    }

    if (index_info_) {
      for (const JsonColumnOp& col_op : stmt.json_col_where_ops()) {
        int32_t idx = index_info_->FindKeyIndex(col_op.IndexExprToColumnName());
        if (idx >= 0) {
          ops[idx] = GetOperatorSelectivity(col_op.yb_op());
        } else {
          num_non_key_ops_++;
        }
      }

      // Enable the following code-block when allowing INDEX of collection fields.
      if (false) {
        for (const SubscriptedColumnOp& col_op : stmt.subscripted_col_where_ops()) {
          int32_t idx = index_info_->FindKeyIndex(col_op.IndexExprToColumnName());
          if (idx >= 0) {
            ops[idx] = GetOperatorSelectivity(col_op.yb_op());
          } else {
            num_non_key_ops_++;
          }
        }
      }
    }

    // Find the length of fully specified prefix in index or indexed table.
    while (prefix_length_ < ops.size() && ops[prefix_length_] == OpSelectivity::kEqual) {
      prefix_length_++;
    }

    // Determine if it is a single-key read, a full-table scan, if there is a range clause after
    // prefix.
    single_key_read_ = prefix_length_ >= num_key_columns;
    full_table_scan_ = prefix_length_ < num_hash_key_columns;
    ends_with_range_ = prefix_length_ < ops.size() && ops[prefix_length_] == OpSelectivity::kRange;
  }

  TableId index_id_;      // Index table id (null for indexed table).
  bool is_local_ = false; // Is the index local? (true for indexed table)
  size_t prefix_length_ = 0;     // Length of fully-specified prefix in index or indexed table.
  bool single_key_read_ = false; // Will this be a single-key read?
  bool full_table_scan_ = false; // Will this be a full table scan?
  bool ends_with_range_ = false; // Is there a range clause after prefix?
  size_t num_non_key_ops_ = 0; // How many non-primary-key column operators needs to be evaluated?
  bool covers_fully_ = false;  // Does the index cover the read fully? (true for indexed table)
  const IndexInfo* index_info_ = nullptr;
};

} // namespace

//--------------------------------------------------------------------------------------------------

PTSelectStmt::PTSelectStmt(MemoryContext *memctx,
                           YBLocation::SharedPtr loc,
                           const bool distinct,
                           PTExprListNode::SharedPtr selected_exprs,
                           PTTableRefListNode::SharedPtr from_clause,
                           PTExpr::SharedPtr where_clause,
                           PTExpr::SharedPtr if_clause,
                           PTListNode::SharedPtr group_by_clause,
                           PTListNode::SharedPtr having_clause,
                           PTOrderByListNode::SharedPtr order_by_clause,
                           PTExpr::SharedPtr limit_clause,
                           PTExpr::SharedPtr offset_clause)
    : PTDmlStmt(memctx, loc, where_clause, if_clause),
      distinct_(distinct),
      selected_exprs_(selected_exprs),
      from_clause_(from_clause),
      group_by_clause_(group_by_clause),
      having_clause_(having_clause),
      order_by_clause_(order_by_clause),
      limit_clause_(limit_clause),
      offset_clause_(offset_clause),
      covering_exprs_(memctx),
      referenced_index_colnames_(memctx) {
}

// Construct a nested select tnode to select from the index. Only the syntactic information
// populated by the parser should be cloned or set here. Semantic information should be left in
// the initial state to be populated when this tnode is analyzed.
// NOTE:
//   Only copy and execute IF clause on IndexTable if all expressions are fully covered.
PTSelectStmt::PTSelectStmt(MemoryContext *memctx,
                           const PTSelectStmt& other,
                           PTExprListNode::SharedPtr selected_exprs,
                           const TableId& index_id,
                           const bool covers_fully)
    : PTDmlStmt(memctx, other, covers_fully),
      distinct_(other.distinct_),
      selected_exprs_(selected_exprs),
      from_clause_(other.from_clause_),
      group_by_clause_(other.group_by_clause_),
      having_clause_(other.having_clause_),
      order_by_clause_(other.order_by_clause_),
      limit_clause_(other.limit_clause_),
      offset_clause_(other.offset_clause_),
      covering_exprs_(memctx),
      index_id_(index_id),
      covers_fully_(covers_fully),
      referenced_index_colnames_(memctx) {
}

PTSelectStmt::~PTSelectStmt() {
}

Status PTSelectStmt::LookupIndex(SemContext *sem_context) {
  VLOG(3) << "Loading table descriptor for index " << index_id_;
  table_ = sem_context->GetTableDesc(index_id_);
  if (!table_ || !table_->IsIndex() ||
      // Only looking for CQL Indexes.
      (table_->table_type() != client::YBTableType::YQL_TABLE_TYPE)) {
    return sem_context->Error(table_loc(), ErrorCode::OBJECT_NOT_FOUND);
  }

  VLOG(3) << "Found index. name = " << table_->name().ToString() << ", id = " << index_id_;
  LoadSchema(sem_context, table_, &column_map_, true /* is_index */);
  return Status::OK();
}

CHECKED_STATUS PTSelectStmt::Analyze(SemContext *sem_context) {
  // If use_cassandra_authentication is set, permissions are checked in PTDmlStmt::Analyze.
  RETURN_NOT_OK(PTDmlStmt::Analyze(sem_context));

  if (index_id_.empty()) {
    // Get the table descriptor.
    if (from_clause_->size() > 1) {
      return sem_context->Error(from_clause_, "Only one selected table is allowed",
                                ErrorCode::CQL_STATEMENT_INVALID);
    }
    RETURN_NOT_OK(from_clause_->Analyze(sem_context));

    // Collect table's schema for semantic analysis.
    Status s = LookupTable(sem_context);
    if (PREDICT_FALSE(!s.ok())) {
      // If it is a system table and it does not exist, do not analyze further. We will return
      // void result when the SELECT statement is executed.
      return (is_system() && table_ == nullptr) ? Status::OK() : s;
    }
  } else {
    RETURN_NOT_OK(LookupIndex(sem_context));
  }

  // Analyze clauses in select statements and check that references to columns in selected_exprs
  // are valid and used appropriately.
  SemState sem_state(sem_context);
  sem_state.set_allowing_aggregate(true);
  sem_state.set_allowing_column_refs(true);

  RETURN_NOT_OK(selected_exprs_->Analyze(sem_context));

  sem_state.set_allowing_aggregate(false);
  sem_state.set_allowing_column_refs(false);

  if (distinct_) {
    RETURN_NOT_OK(AnalyzeDistinctClause(sem_context));
  }

  // Collect covering expression.
  for (auto expr_node : selected_exprs_->node_list()) {
    covering_exprs_.push_back(expr_node.get());
  }

  // Check if this is an aggregate read.
  bool has_aggregate_expr = false;
  bool has_singular_expr = false;
  for (auto expr_node : selected_exprs_->node_list()) {
    if (expr_node->IsAggregateCall()) {
      has_aggregate_expr = true;
    } else {
      has_singular_expr = true;
    }
  }
  if (has_aggregate_expr && has_singular_expr) {
    return sem_context->Error(
        selected_exprs_,
        "Selecting aggregate together with rows of non-aggregate values is not allowed",
        ErrorCode::CQL_STATEMENT_INVALID);
  }
  is_aggregate_ = has_aggregate_expr;

  // Run error checking on the WHERE conditions.
  RETURN_NOT_OK(AnalyzeWhereClause(sem_context));

  // Run error checking on the IF conditions.
  RETURN_NOT_OK(AnalyzeIfClause(sem_context));

  // Check if there is an index to use. If there is and it covers the query fully, we will query
  // just the index and that is it.
  if (index_id_.empty()) {
    // Validate the ordering expressions without processing ORDER BY clause. This check is to
    // verify that ordering column exists, and the column's datatype allows comparison. The entire
    // ORDER BY clause can only be analyzed after an INDEX is chosen.
    RETURN_NOT_OK(ValidateOrderByExprs(sem_context));

    // TODO(neil) Remove this construction if "referenced_index_colnames_" is no longer needed.
    //
    // Constructing a list of index column names that is being referenced.
    // - This is similar to the list "column_refs_", but this is a list of column names instead of
    //   column ids. When indexing by expression, a mangled-name of the expression is used to
    //   represent the column, so column id cannot be used to identify coverage.
    //
    // - This list is to support a quick fix for github #4881. Once column and expression names
    //   are mangled correctly, this code should be removed.
    //
    // - In CQL semantics, ORDER BY must used only indexed column, so not need to check for its
    //   coverage. Neither "column_refs_" nor "referenced_index_colnames_" has ORDER BY columns.
    for (const PTExpr *expr : covering_exprs_) {
      if (!expr->HaveColumnRef()) {
        continue;
      }
      if (expr->opcode() == TreeNodeOpcode::kPTAllColumns) {
        for (const ColumnDesc& coldesc : static_cast<const PTAllColumns*>(expr)->columns()) {
          referenced_index_colnames_.insert(coldesc.MangledName());
        }
      } else {
        expr->CollectReferencedIndexColnames(&referenced_index_colnames_);
      }
    }
    for (const PTExpr *expr : filtering_exprs_) {
      expr->CollectReferencedIndexColnames(&referenced_index_colnames_);
    }

    RETURN_NOT_OK(AnalyzeIndexes(sem_context));
    if (child_select_ && child_select_->covers_fully_) {
      return Status::OK();
    }
  }

  // Run error checking on order by for the chosen INDEX.
  RETURN_NOT_OK(AnalyzeOrderByClause(sem_context));

  // Run error checking on the LIMIT clause.
  RETURN_NOT_OK(AnalyzeLimitClause(sem_context));

  // Run error checking on the OFFSET clause.
  RETURN_NOT_OK(AnalyzeOffsetClause(sem_context));

  // Constructing the schema of the result set.
  RETURN_NOT_OK(ConstructSelectedSchema());

  return Status::OK();
}

void PTSelectStmt::PrintSemanticAnalysisResult(SemContext *sem_context) {
  VLOG(3) << "SEMANTIC ANALYSIS RESULT (" << *loc_ << "):\n" << "Not yet avail";
}

ExplainPlanPB PTSelectStmt::AnalysisResultToPB() {
  ExplainPlanPB explain_plan;
  SelectPlanPB *select_plan = explain_plan.mutable_select_plan();
  // Determines scan_type, child_select_ != null means an index is being used.
  if (child_select_) {
    string index_type = (child_select_->covers_fully() ? "Index Only" : "Index");
    string lookup_type = (child_select_->select_has_primary_keys_set_ ? "Key Lookup" : "Scan");
    select_plan->set_select_type(index_type + " " + lookup_type + " using " +
      child_select()->table()->name().ToString() + " on " + table_name().ToString());
  // Index is not being used, query only uses main table.
  } else if (select_has_primary_keys_set_) {
    select_plan->set_select_type("Primary Key Lookup on " + table_name().ToString());
  } else if (!(key_where_ops().empty() && partition_key_ops().empty())) {
    select_plan->set_select_type("Range Scan on " + table_name().ToString());
  } else {
    select_plan->set_select_type("Seq Scan on " + table_name().ToString());
  }
  string key_conditions = "  Key Conditions: ";
  string filter = "  Filter: ";
  size_t longest = 0;
  // If overarching information( "Aggregate" | "Limit") then rest of the explain plan output needs
  // to be indented.
  if (is_aggregate() || limit_clause_) {
    string aggr = (is_aggregate()) ? "Aggregate" : "Limit";
    select_plan->set_aggregate(aggr);
    key_conditions = "      " + key_conditions;
    filter = "      " + filter;
    select_plan->set_select_type("  ->  " + select_plan->select_type());
    longest = max(longest, aggr.length());
  }
  longest = max(longest, select_plan->select_type().length());
  // If index is being used, change the split of key conditions and filters to that of the index.
  const auto& keys = child_select_ ? child_select_->key_where_ops() : key_where_ops();
  const auto& filters = child_select_ ? child_select_->where_ops() : where_ops();
  // Rebuild the conditions and filter into strings from internal format.
  string filled_key_conds = conditionsToString<MCVector<ColumnOp>>(keys);
  string filled_filter = conditionsToString<MCList<ColumnOp>>(filters);

  filled_key_conds += partitionkeyToString(partition_key_ops());

  // If the query has key conditions or filters on either the index or the main table, then output
  // to query plan.
  if (!filled_key_conds.empty()) {
    key_conditions += filled_key_conds;
    longest = max(longest, key_conditions.length());
    select_plan->set_key_conditions(key_conditions);
  }
  if (!filled_filter.empty()) {
    filter += filled_filter;
    longest = max(longest, filter.length());
    select_plan->set_filter(filter);
  }

  // Set the output_width that has been calculated throughout the construction of the query plan.
  select_plan->set_output_width(longest);
  return explain_plan;
}
//--------------------------------------------------------------------------------------------------

// Check whether we can use an index.
CHECKED_STATUS PTSelectStmt::AnalyzeIndexes(SemContext *sem_context) {
  VLOG(3) << "AnalyzeIndexes: " << sem_context->stmt();
  // Skip if there is no index, or the query involves token() since the query by partition key is
  // more efficient on the indexed table.
  if (table_->index_map().empty() || !partition_key_ops_.empty()) {
    return Status::OK();
  }

  // We can now find the best index for this query vs the indexed table. See Selectivity's
  // comparison operator for the criterias for the best index.
  MCVector<Selectivity> selectivities(sem_context->PTempMem());
  selectivities.reserve(table_->index_map().size() + 1);
  selectivities.emplace_back(sem_context->PTempMem(), *this);
  for (const std::pair<TableId, IndexInfo> index : table_->index_map()) {
    if (index.second.HasReadPermission()) {
      selectivities.emplace_back(sem_context->PTempMem(), *this, index.second);
    }
  }
  std::sort(selectivities.begin(), selectivities.end(), std::greater<Selectivity>());
  if (VLOG_IS_ON(3)) {
    for (const auto& selectivity : selectivities) {
      VLOG(3) << selectivity.ToString();
    }
  }

  // Find the best selectivity.
  for (const Selectivity& selectivity : selectivities) {
    if (!FLAGS_enable_uncovered_index_select && !selectivity.covers_fully()) {
      continue;
    }

    VLOG(3) << "Selected = " << selectivity.ToString();

    // If an index can be used, analyze the select on the index.
    if (!selectivity.index_id().empty()) {
      MemoryContext* memctx = sem_context->PTreeMem();
      auto selected_exprs = selected_exprs_;

      // If the index does not cover the query fully, select the primary key from the index.
      if (!selectivity.covers_fully()) {
        const auto& loc = selected_exprs_->loc_ptr();
        selected_exprs = PTExprListNode::MakeShared(memctx, loc);
        for (int i = 0; i < num_key_columns(); i++) {
          const client::YBColumnSchema& column = table_->schema().Column(i);
          auto column_name_str = MCMakeShared<MCString>(memctx, column.name().c_str());
          auto column_name = PTQualifiedName::MakeShared(memctx, loc, column_name_str);
          selected_exprs->Append(PTRef::MakeShared(memctx, loc, column_name));
        }

        // Clear the primary key operations. They be filled after the primary key is fetched from
        // the index.
        key_where_ops_.clear();
        std::remove_if(where_ops_.begin(),
                       where_ops_.end(),
                       [](const ColumnOp& op) { return op.desc()->is_primary(); });
        const client::YBSchema& schema = table_->schema();
        for (size_t i = 0; i < schema.num_key_columns(); i++) {
          column_refs_.insert(schema.ColumnId(i));
        }
      }

      // Create a child select statement to query the index.
      child_select_ = MakeShared(memctx, *this, selected_exprs,
                                 selectivity.index_id(), selectivity.covers_fully());

      // If an index will be used, the limit and offset clauses should be used by the select from
      // the index only.
      order_by_clause_ = nullptr;
      limit_clause_ = nullptr;
      offset_clause_ = nullptr;

      // Now analyze the select from the index.
      SemState select_state(sem_context);
      select_state.set_selecting_from_index(true);
      return child_select_->Analyze(sem_context);
    }
    break;
  }

  return Status::OK();
}

// Return whether the index covers the read fully.
// INDEXes that were created before v2.0 are defined by column IDs instead of mangled_names.
// - Returns TRUE if a list of column refs of a statement is a subset of INDEX columns.
// - Use ColumnID to check if a column in a query is covered by the index.
// - The list "column_refs_" contains IDs of all columns that are referred to by SELECT.
// - The list "IndexInfo::columns_" contains the IDs of all columns in the INDEX.
bool PTSelectStmt::CoversFully(const IndexInfo& index_info) const {
  // First, check covering by ID.
  bool all_ref_id_covered = true;
  for (const int32 table_col_id : column_refs_) {
    if (!index_info.IsColumnCovered(ColumnId(table_col_id))) {
      all_ref_id_covered = false;
    }
  }

  // Return result if name-resolution for covering is NOT needed.
  // - If all column references are found by ID, return true without further resolution.
  // - Index is not by expression, so name resolution for covering is not needed.
  // - Index uses older Protobuf versions, which doesn't use column name (id only).
  if (all_ref_id_covered ||
      !index_info.has_index_by_expr() ||
      !index_info.use_mangled_column_name()) {
    return all_ref_id_covered;
  }

  // Now, check for covering by column names to support index by expression.
  //
  // TODO(neil) We use a quick fix for now, but eventually we should mangle column name correctly
  // and remove the following quick fix and its associated code. See github #4881 for the bug
  // in column name mangling.
  //
  // Iterating names in "referenced_index_colnames_", which consists of names of all columns that
  // are referenced by SELECT and check if the name is covered in the index.
  for (const string &column_name : referenced_index_colnames_) {
    if (!index_info.IsColumnCovered(column_name)) {
      return false;
    }
  }

  // TODO(neil) Change INDEX's metadata for column name-mangling to support the following code.
  // As shown in github #4881, the following is not working correctly.
  //   CREATE INDEX idx ON a_table(v);
  //   - Index column "v" is named as "$C_v"
  //   - Index column "v1" is named as "$C_v1"
  // As a result, IsExprCovered("$C_v1") would return true as it thought "$C_v1" is just an
  // expression for "$C_v" column.

  // Correct fix for #4881 would be adding a postfix when mangling column name. For example:
  // - column name "v" is mangled to "$C_v_E$"
  // - column name "v1" is mangled to "$C_v1_E$"
  // That way, IsExprCovered() would know that "$C_v1_E$" is NOT an expression of "$C_v_E$".
  // However, this changes metadata for INDEX, so compatible flag must be added to distinguish
  // between different mangling method, such as
  //   "use_mangled_column_name" versus "use_mangled_column_name_2"
  if (false) {
    // Check all ColumnRef in selected list.
    for (const PTExpr *expr : covering_exprs_) {
      // If this expression does not have column reference, it is considered "coverred".
      if (!expr->HaveColumnRef()) {
        continue;
      }

      if (expr->opcode() == TreeNodeOpcode::kPTAllColumns) {
        for (const ColumnDesc& coldesc : static_cast<const PTAllColumns*>(expr)->columns()) {
          if (index_info.IsExprCovered(coldesc.MangledName()) < 0) {
            return false;
          }
        }
      } else if (index_info.IsExprCovered(expr->MangledName()) < 0) {
        return false;
      }
    }

    // Check all ColumnRef in filtering list.
    for (const PTExpr *expr : filtering_exprs_) {
      if (index_info.IsExprCovered(expr->MangledName()) < 0) {
        return false;
      }
    }
  }

  // All referenced columns are covered.
  return true;
}

// -------------------------------------------------------------------------------------------------

CHECKED_STATUS PTSelectStmt::AnalyzeDistinctClause(SemContext *sem_context) {
  // Only partition and static columns are allowed to be used with distinct clause.
  int key_count = 0;
  for (const auto& pair : column_map_) {
    const ColumnDesc& desc = pair.second;
    if (desc.is_hash()) {
      if (column_refs_.find(desc.id()) != column_refs_.end()) {
        key_count++;
      }
    } else if (!desc.is_static()) {
      if (column_refs_.find(desc.id()) != column_refs_.end()) {
        return sem_context->Error(
            selected_exprs_,
            "Selecting distinct must request only partition keys and static columns",
            ErrorCode::CQL_STATEMENT_INVALID);
      }
    }
  }

  if (key_count != 0 && key_count != num_hash_key_columns()) {
    return sem_context->Error(selected_exprs_,
                              "Selecting distinct must request all or none of partition keys",
                              ErrorCode::CQL_STATEMENT_INVALID);
  }
  return Status::OK();
}

bool PTSelectStmt::IsReadableByAllSystemTable() const {
  const client::YBTableName t = table_name();
  const string& keyspace = t.namespace_name();
  const string& table = t.table_name();
  if (keyspace == master::kSystemSchemaNamespaceName) {
    return true;
  } else if (keyspace == master::kSystemNamespaceName) {
    if (table == master::kSystemLocalTableName ||
        table == master::kSystemPeersTableName ||
        table == master::kSystemPeersV2TableName ||
        table == master::kSystemPartitionsTableName) {
      return true;
    }
  }
  return false;
}

//--------------------------------------------------------------------------------------------------

namespace {

PTOrderBy::Direction directionFromSortingType(ColumnSchema::SortingType sorting_type) {
  return sorting_type == ColumnSchema::SortingType::kDescending ?
      PTOrderBy::Direction::kDESC : PTOrderBy::Direction::kASC;
}

} // namespace

CHECKED_STATUS PTSelectStmt::ValidateOrderByExprs(SemContext *sem_context) {
  if (order_by_clause_ != nullptr) {
    for (auto& order_by : order_by_clause_->node_list()) {
      RETURN_NOT_OK(order_by->ValidateExpr(sem_context));
    }
  }
  return Status::OK();
}

CHECKED_STATUS PTSelectStmt::AnalyzeOrderByClause(SemContext *sem_context) {
  if (order_by_clause_ != nullptr) {
    if (key_where_ops_.empty()) {
      return sem_context->Error(
          order_by_clause_,
          "All hash columns must be set if order by clause is present.",
          ErrorCode::INVALID_ARGUMENTS);
    }

    unordered_map<string, PTOrderBy::Direction> order_by_map;
    for (auto& order_by : order_by_clause_->node_list()) {
      RETURN_NOT_OK(order_by->Analyze(sem_context));
      order_by_map[order_by->order_expr()->MetadataName()] = order_by->direction();
    }
    const auto& schema = table_->schema();
    vector<bool> is_column_forward;
    is_column_forward.reserve(schema.num_range_key_columns());
    bool last_column_order_specified = true;
    for (size_t i = schema.num_hash_key_columns(); i < schema.num_key_columns(); i++) {
      const auto& column = schema.Column(i);
      if (order_by_map.find(column.name()) != order_by_map.end()) {
        if (!last_column_order_specified) {
          return sem_context->Error(
              order_by_clause_,
              "Order by currently only support the ordering of columns following their declared"
                  " order in the PRIMARY KEY", ErrorCode::INVALID_ARGUMENTS);
        }
        is_column_forward.push_back(
            directionFromSortingType(column.sorting_type()) == order_by_map[column.name()]);
        order_by_map.erase(column.name());
      } else {
        last_column_order_specified = false;
        is_column_forward.push_back(is_column_forward.empty() || is_column_forward.back());
      }
    }
    if (!order_by_map.empty()) {
      return sem_context->Error(
          order_by_clause_,
          ("Order by clause should only contain clustering columns, got "
           + order_by_map.begin()->first).c_str(),
          ErrorCode::INVALID_ARGUMENTS);
    }
    is_forward_scan_ = is_column_forward[0];
    for (auto&& b : is_column_forward) {
      if (b != is_forward_scan_) {
        return sem_context->Error(
            order_by_clause_,
            "Unsupported order by relation", ErrorCode::INVALID_ARGUMENTS);
      }
    }
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS PTSelectStmt::AnalyzeLimitClause(SemContext *sem_context) {
  if (limit_clause_ == nullptr) {
    return Status::OK();
  }

  RETURN_NOT_OK(limit_clause_->CheckRhsExpr(sem_context));

  SemState sem_state(sem_context, QLType::Create(INT32), InternalType::kInt32Value);
  sem_state.set_bindvar_name(PTBindVar::limit_bindvar_name());
  RETURN_NOT_OK(limit_clause_->Analyze(sem_context));

  return Status::OK();
}

CHECKED_STATUS PTSelectStmt::AnalyzeOffsetClause(SemContext *sem_context) {
  if (offset_clause_ == nullptr) {
    return Status::OK();
  }

  RETURN_NOT_OK(offset_clause_->CheckRhsExpr(sem_context));

  SemState sem_state(sem_context, QLType::Create(INT32), InternalType::kInt32Value);
  sem_state.set_bindvar_name(PTBindVar::offset_bindvar_name());
  RETURN_NOT_OK(offset_clause_->Analyze(sem_context));

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS PTSelectStmt::ConstructSelectedSchema() {
  const MCList<PTExpr::SharedPtr>& exprs = selected_exprs();
  selected_schemas_ = make_shared<vector<ColumnSchema>>();
  selected_schemas_->reserve(exprs.size());
  for (auto expr : exprs) {
    if (expr->opcode() == TreeNodeOpcode::kPTAllColumns) {
      const PTAllColumns *ref = static_cast<const PTAllColumns*>(expr.get());
      for (const auto& col_desc : ref->columns()) {
        selected_schemas_->emplace_back(col_desc.name(), col_desc.ql_type());
      }
    } else {
      selected_schemas_->emplace_back(expr->QLName(), expr->ql_type());
    }
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

PTOrderBy::PTOrderBy(MemoryContext *memctx,
                     YBLocation::SharedPtr loc,
                     const PTExpr::SharedPtr& order_expr,
                     const Direction direction,
                     const NullPlacement null_placement)
  : TreeNode(memctx, loc),
    order_expr_(order_expr),
    direction_(direction),
    null_placement_(null_placement) {
}

Status PTOrderBy::ValidateExpr(SemContext *sem_context) {
  RETURN_NOT_OK(order_expr_->Analyze(sem_context));
  return Status::OK();
}

Status PTOrderBy::Analyze(SemContext *sem_context) {
  RETURN_NOT_OK(order_expr_->Analyze(sem_context));

  if (order_expr_->expr_op() == ExprOperator::kRef) {
    // This check is for clause ORDER BY <column> that is not part of the index (NULL desc).
    // Example
    //   Statement: CREATE INDEX ON table ( x ) INCLUDE ( y );
    //   Columns "x" and "y" would be part of the index.
    auto colref = dynamic_cast<PTRef*>(order_expr_.get());
    if (!colref || !colref->desc()) {
      return sem_context->Error(
          this, "Order by clause contains invalid columns", ErrorCode::INVALID_ARGUMENTS);
    }

  } else if (!order_expr_->index_desc()) {
    // This check is for clause ORDER BY <expression> that is not part of the index (NULL desc).
    // Example
    //   Statement: CREATE INDEX ON table ( <expr> );
    //   Column "<expr>" would be part of the index.
    return sem_context->Error(
        this, "Order by clause contains invalid expression", ErrorCode::INVALID_ARGUMENTS);
  }

  return Status::OK();
}

PTOrderBy::~PTOrderBy() {
}

//--------------------------------------------------------------------------------------------------

PTTableRef::PTTableRef(MemoryContext *memctx,
                       YBLocation::SharedPtr loc,
                       const PTQualifiedName::SharedPtr& name,
                       MCSharedPtr<MCString> alias)
    : TreeNode(memctx, loc),
      name_(name),
      alias_(alias) {
}

PTTableRef::~PTTableRef() {
}

CHECKED_STATUS PTTableRef::Analyze(SemContext *sem_context) {
  if (alias_ != nullptr) {
    return sem_context->Error(this, "Alias is not allowed", ErrorCode::CQL_STATEMENT_INVALID);
  }
  return name_->AnalyzeName(sem_context, OBJECT_TABLE);
}

//--------------------------------------------------------------------------------------------------

}  // namespace ql
}  // namespace yb
