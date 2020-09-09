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
// Tree node definitions for SELECT statement.
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_CQL_QL_PTREE_PT_SELECT_H_
#define YB_YQL_CQL_QL_PTREE_PT_SELECT_H_

#include "yb/yql/cql/ql/ptree/list_node.h"
#include "yb/yql/cql/ql/ptree/tree_node.h"
#include "yb/yql/cql/ql/ptree/pt_name.h"
#include "yb/yql/cql/ql/ptree/pt_expr.h"
#include "yb/yql/cql/ql/ptree/pt_dml.h"

namespace yb {
namespace ql {

//--------------------------------------------------------------------------------------------------
// ORDER BY.
class PTOrderBy : public TreeNode {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTOrderBy> SharedPtr;
  typedef MCSharedPtr<const PTOrderBy> SharedPtrConst;

  enum Direction : int8_t { kASC = 0, kDESC };

  enum NullPlacement : int8_t { kFIRST = 0, kLAST };

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTOrderBy(MemoryContext *memctx,
            YBLocation::SharedPtr loc,
            const PTExpr::SharedPtr& order_expr,
            const Direction direction,
            const NullPlacement null_placement);
  virtual ~PTOrderBy();

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTOrderBy;
  }

  template<typename... TypeArgs>
  inline static PTOrderBy::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTOrderBy>(memctx, std::forward<TypeArgs>(args)...);
  }

  CHECKED_STATUS ValidateExpr(SemContext *sem_context);
  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;

  Direction direction() const {
    return direction_;
  }

  NullPlacement null_placement() const {
    return null_placement_;
  }

  PTExpr::SharedPtr order_expr() const {
    return order_expr_;
  }

 private:
  PTExpr::SharedPtr order_expr_;
  Direction direction_;
  NullPlacement null_placement_;
};

using PTOrderByListNode = TreeListNode<PTOrderBy>;

//--------------------------------------------------------------------------------------------------
// FROM <table ref list>.
class PTTableRef : public TreeNode {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTTableRef> SharedPtr;
  typedef MCSharedPtr<const PTTableRef> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTTableRef(MemoryContext *memctx,
             YBLocation::SharedPtr loc,
             const PTQualifiedName::SharedPtr& name,
             MCSharedPtr<MCString> alias);
  virtual ~PTTableRef();

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTTableRef;
  }

  template<typename... TypeArgs>
  inline static PTTableRef::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTTableRef>(memctx, std::forward<TypeArgs>(args)...);
  }

  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;

  client::YBTableName table_name() const {
    return name_->ToTableName();
  }

 private:
  PTQualifiedName::SharedPtr name_;
  MCSharedPtr<MCString> alias_;
};

using PTTableRefListNode = TreeListNode<PTTableRef>;

//--------------------------------------------------------------------------------------------------
// This class represents SELECT statement.
class PTSelectStmt : public PTDmlStmt {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTSelectStmt> SharedPtr;
  typedef MCSharedPtr<const PTSelectStmt> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTSelectStmt(MemoryContext *memctx,
               YBLocation::SharedPtr loc,
               bool distinct,
               PTExprListNode::SharedPtr selected_exprs,
               PTTableRefListNode::SharedPtr from_clause,
               PTExpr::SharedPtr where_clause,
               PTExpr::SharedPtr if_clause,
               PTListNode::SharedPtr group_by_clause,
               PTListNode::SharedPtr having_clause,
               PTOrderByListNode::SharedPtr order_by_clause,
               PTExpr::SharedPtr limit_clause,
               PTExpr::SharedPtr offset_clause);
  // Construct a nested select tnode to select from the index.
  PTSelectStmt(MemoryContext *memctx,
               const PTSelectStmt& parent,
               PTExprListNode::SharedPtr selected_exprs,
               const TableId& index_id,
               bool covers_fully);
  virtual ~PTSelectStmt();

  template<typename... TypeArgs>
  inline static PTSelectStmt::SharedPtr MakeShared(MemoryContext *memctx,
                                                   TypeArgs&&... args) {
    return MCMakeShared<PTSelectStmt>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;
  void PrintSemanticAnalysisResult(SemContext *sem_context);
  ExplainPlanPB AnalysisResultToPB() override;
  bool CoversFully(const IndexInfo& index_info) const;

  // Execution opcode.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTSelectStmt;
  }

  virtual void SetOrderByClause(PTOrderByListNode::SharedPtr order_by_clause) {
    order_by_clause_ = order_by_clause;
  }

  virtual void SetLimitClause(PTExpr::SharedPtr limit_clause) {
    limit_clause_ = limit_clause;
  }

  virtual void SetOffsetClause(PTExpr::SharedPtr offset_clause) {
    offset_clause_ = offset_clause;
  }

  bool distinct() const {
    return distinct_;
  }

  bool is_forward_scan() const {
    return is_forward_scan_;
  }

  const PTExpr::SharedPtr& limit() const {
    return limit_clause_;
  }

  const PTExpr::SharedPtr& offset() const {
    return offset_clause_;
  }

  const MCList<PTExpr::SharedPtr>& selected_exprs() const {
    return selected_exprs_->node_list();
  }

  // Returns table name.
  virtual client::YBTableName table_name() const override {
    // CQL only allows one table at a time.
    return from_clause_->element(0)->table_name();
  }

  // Returns location of table name.
  virtual const YBLocation& table_loc() const override {
    return from_clause_->loc();
  }

  PTOrderByListNode::SharedPtr order_by_clause() const {
    return order_by_clause_;
  }

  bool is_aggregate() const {
    return is_aggregate_;
  }

  const PTSelectStmt::SharedPtr& child_select() const {
    return child_select_;
  }

  const TableId& index_id() const {
    return index_id_;
  }

  bool covers_fully() const {
    return covers_fully_;
  }

  // Certain tables can be read by any authorized role specifically because they are being used
  // by the Cassandra driver:
  // system_schema.keyspaces
  // system_schema.columns
  // system_schema.tables
  // system.local
  // system.peers
  bool IsReadableByAllSystemTable() const;

  const std::shared_ptr<client::YBTable>& bind_table() const override {
    return child_select_ ? child_select_->bind_table() : PTDmlStmt::bind_table();
  }

  const MCVector<PTBindVar*> &bind_variables() const override {
    return child_select_ ? child_select_->bind_variables() : PTDmlStmt::bind_variables();
  }
  MCVector<PTBindVar*> &bind_variables() override {
    return child_select_ ? child_select_->bind_variables() : PTDmlStmt::bind_variables();
  }

  std::vector<int64_t> hash_col_indices() const override {
    return child_select_ ? child_select_->hash_col_indices() : PTDmlStmt::hash_col_indices();
  }

 private:
  CHECKED_STATUS LookupIndex(SemContext *sem_context);
  CHECKED_STATUS AnalyzeIndexes(SemContext *sem_context);
  CHECKED_STATUS AnalyzeDistinctClause(SemContext *sem_context);
  CHECKED_STATUS ValidateOrderByExprs(SemContext *sem_context);
  CHECKED_STATUS AnalyzeOrderByClause(SemContext *sem_context);
  CHECKED_STATUS AnalyzeLimitClause(SemContext *sem_context);
  CHECKED_STATUS AnalyzeOffsetClause(SemContext *sem_context);
  CHECKED_STATUS ConstructSelectedSchema();

  // --- The parser will decorate this node with the following information --

  // The following members represent different components of SELECT statement. However, Cassandra
  // doesn't support all of SQL syntax and semantics.
  //
  // SELECT [DISTINCT] <selected_exprs_>
  //   FROM      <from_clause_>
  //   WHERE     <where_clause_>
  //   GROUP BY  <group_by_clause_> HAVING <having_clause_>
  //   ORDER BY  <order_by_clause_>
  //   LIMIT     <limit_clause_>
  //   OFFSET    <offset_clause_>
  const bool distinct_ = false;
  const PTExprListNode::SharedPtr selected_exprs_;
  const PTTableRefListNode::SharedPtr from_clause_;
  const PTListNode::SharedPtr group_by_clause_;
  const PTListNode::SharedPtr having_clause_;
  PTOrderByListNode::SharedPtr order_by_clause_;
  PTExpr::SharedPtr limit_clause_;
  PTExpr::SharedPtr offset_clause_;
  MCVector<const PTExpr*> covering_exprs_;

  // -- The semantic analyzer will decorate this node with the following information --

  bool is_forward_scan_ = true;
  bool is_aggregate_ = false;

  // Child select statement. Currently only a select statement using an index (covered or uncovered)
  // has a child select statement to query an index.
  PTSelectStmt::SharedPtr child_select_;

  // For nested select from an index: the index id and whether it covers the query fully.
  TableId index_id_;
  bool covers_fully_ = false;

  // Name of all columns the SELECT statement is referenced. Similar to the list "column_refs_",
  // but this is a list of column names instead of column ids.
  MCSet<string> referenced_index_colnames_;
};

}  // namespace ql
}  // namespace yb

#endif  // YB_YQL_CQL_QL_PTREE_PT_SELECT_H_
