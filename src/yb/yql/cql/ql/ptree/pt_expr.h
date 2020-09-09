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
// Tree node definitions for expression.
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_CQL_QL_PTREE_PT_EXPR_H_
#define YB_YQL_CQL_QL_PTREE_PT_EXPR_H_

#include <boost/optional.hpp>

#include "yb/yql/cql/ql/ptree/column_desc.h"
#include "yb/yql/cql/ql/ptree/tree_node.h"
#include "yb/yql/cql/ql/ptree/pt_type.h"
#include "yb/yql/cql/ql/ptree/pt_name.h"
#include "yb/yql/cql/ql/ptree/sem_state.h"

#include "yb/util/bfql/tserver_opcodes.h"
#include "yb/util/decimal.h"

namespace yb {
namespace ql {

// Because statements own expressions and their headers include expression headers, we forward
// declare statement classes here.
class PTSelectStmt;
class PTDmlStmt;

//--------------------------------------------------------------------------------------------------
// The order of the following enum values are not important.
enum class ExprOperator : int {
  kNoOp = 0,

  // Reference to constants, columns, and variables.
  kConst = 1,
  kAlias = 2,
  kRef = 3,
  kSubColRef = 4,
  kBindVar = 5,
  kBcall = 6,

  // Operators that take one operand.
  kUMinus = 7,

  // Logical operators with one operand.
  kLogic1 = 8,

  // Logical operators with two operands.
  kLogic2 = 9,

  // Relation operators that take no operand.
  kRelation0 = 10,

  // Relation operators that take one operand.
  kRelation1 = 11,

  // Relation operators that take two operands.
  kRelation2 = 12,

  // Relation operators that take three operands.
  kRelation3 = 13,

  // Relation operators that take unspecified number of operands.
  kCollection = 14,

  // Reference to a column with json operators.
  kJsonOperatorRef = 15,
};

enum class JsonOperator {
  JSON_OBJECT,
  JSON_TEXT
};

//--------------------------------------------------------------------------------------------------
// Base class for all expressions.
class PTExpr : public TreeNode {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTExpr> SharedPtr;
  typedef MCSharedPtr<const PTExpr> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  explicit PTExpr(
      MemoryContext *memctx,
      YBLocation::SharedPtr loc,
      ExprOperator op = ExprOperator::kNoOp,
      yb::QLOperator ql_op = yb::QLOperator::QL_OP_NOOP,
      InternalType internal_type = InternalType::VALUE_NOT_SET,
      DataType ql_type_id = DataType::UNKNOWN_DATA)
      : PTExpr(memctx, loc, op, ql_op, internal_type, QLType::Create(ql_type_id)) {}
  explicit PTExpr(
      MemoryContext *memctx,
      YBLocation::SharedPtr loc,
      ExprOperator op,
      yb::QLOperator ql_op,
      InternalType internal_type,
      const QLType::SharedPtr& ql_type)
      : TreeNode(memctx, loc),
        op_(op),
        ql_op_(ql_op),
        internal_type_(internal_type),
        ql_type_(ql_type),
        expected_internal_type_(InternalType::VALUE_NOT_SET),
        index_name_(MCMakeShared<MCString>(memctx)) {}
  virtual ~PTExpr() {}

  // Expression return type in DocDB format.
  virtual InternalType internal_type() const {
    return internal_type_;
  }

  bool has_valid_internal_type() {
    // internal_type_ is not set in case of PTNull.
    return ql_type_->main() == DataType::NULL_VALUE_TYPE ||
           internal_type_ != InternalType::VALUE_NOT_SET;
  }

  virtual InternalType expected_internal_type() const {
    return expected_internal_type_;
  }

  // Expression return type in QL format.
  virtual const std::shared_ptr<QLType>& ql_type() const {
    return ql_type_;
  }

  // This is only useful during pre-exec phase.
  // Normally you'd want to use CheckExpectedTypeCompatibility instead.
  virtual void set_expected_internal_type(InternalType expected_internal_type) {
    expected_internal_type_ = expected_internal_type;
  }

  // Expression return result set column type in QL format.
  virtual void rscol_type_PB(QLTypePB *pb_type ) const {
    ql_type_->ToQLTypePB(pb_type);
  }

  virtual void set_ql_type(const std::shared_ptr<QLType>& ql_type) {
    ql_type_ = ql_type;
  }

  virtual void set_ql_type(DataType type_id) {
    ql_type_ = QLType::Create(type_id);
  }

  // TODO(neil or mihnea) Remove or replace all ql_type_id API & comments with QLType.
  virtual DataType ql_type_id() const {
    return ql_type_->main();
  }

  virtual void set_ql_type_id(DataType type_id) {
    ql_type_ = QLType::Create(type_id);
  }

  bool has_valid_ql_type_id() {
    return ql_type_->main() != DataType::UNKNOWN_DATA;
  }

  // Seeks index-columns that referenced by this expression and output mangled colum names.
  // NOTE:
  // - index-column can be either a column or an expression of the column.
  // - Currently, name of a column in an INDEX must be one of the following.
  //   * Mangled name of a column of scalar type (not a collection type such as map, jsonb).
  //   * Mangled name of a "jsonb->>field" expresion.
  virtual void CollectReferencedIndexColnames(MCSet<string> *col_names) const {
    if (op1()) {
      op1()->CollectReferencedIndexColnames(col_names);
    }
    if (op2()) {
      op2()->CollectReferencedIndexColnames(col_names);
    }
    if (op3()) {
      op3()->CollectReferencedIndexColnames(col_names);
    }
  }

  // Return name of expression.
  // - Option kUserOriginalName
  //     When report data to user, we use the original name that users enterred. In SELECT,
  //     each selected expression is assigned a name, and this method is to form the name of an
  //     expression using un-mangled column names. For example, when selected expr is a column of a
  //     table, QLName() would be the name of that column.
  //
  // - Option kMangledName
  //     When INDEX is created, YugaByte generates column name by mangling the original name from
  //     users for the index expression columns.
  //
  // - Option kMetadataName
  //     When loading column descriptor from Catalog::Table and Catalog::IndexTable, we might want
  //     to read the name that is kept in the Catalog. Unmangled name for regular column, and
  //     mangled name for index-expression column.
  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const {
    LOG(INFO) << "Missing QLName for expression("
              << static_cast<int>(expr_op())
              << ") that is being selected";
    return "expr";
  }

  virtual string MangledName() const {
    return QLName(QLNameOption::kMangledName);
  }

  virtual string MetadataName() const {
    // If this expression was used to define an index column, use its descriptor name.
    return index_desc_ ? index_desc_->MetadataName() : QLName(QLNameOption::kMetadataName);
  }

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTExpr;
  }

  // Returns the expression operator.
  virtual ExprOperator expr_op() const {
    return op_;
  }

  yb::QLOperator ql_op() const {
    return ql_op_;
  }

  // Predicate for null.
  virtual bool is_null() const {
    return ql_type_->main() == DataType::NULL_VALUE_TYPE;
  }

  // Returns the operands of an expression.
  virtual PTExpr::SharedPtr op1() const {
    return nullptr;
  }

  virtual PTExpr::SharedPtr op2() const {
    return nullptr;
  }

  virtual PTExpr::SharedPtr op3() const {
    return nullptr;
  }

  // Predicate for constant tree node.
  bool is_constant() const {
    return ((expr_op() == ExprOperator::kConst) ||
            (expr_op() == ExprOperator::kUMinus && op1()->expr_op() == ExprOperator::kConst));
  }

  // Predicate for values.
  bool has_no_column_ref() const {
    return is_constant() ||
           expr_op() == ExprOperator::kBindVar ||
           expr_op() == ExprOperator::kCollection;
  }

  virtual bool IsDummyStar() const {
    return false;
  }

  // Predicate for calls to aggregate functions.
  virtual bool IsAggregateCall() const {
    return false;
  }
  virtual yb::bfql::TSOpcode aggregate_opcode() const {
    return yb::bfql::TSOpcode::kNoOp;
  }

  // Predicate for expressions that have no column reference.
  // - When an expression does not have ColumnRef, it can be evaluated without reading table data.
  // - By default, returns true to indicate so that optimization doesn't take place unless we
  //   know for sure ColumnRef is used.
  //
  // Examples:
  // - Constant has no column reference.
  // - PTRef always has column-ref.
  // - All other epxressions are dependent on whether or not its argument list contains a column.
  //   NOW() and COUNT(*) have no reference. The '*' argument is translated to PTStar (DummyStar)
  //   because we don't need to read any extra information from DocDB to process the statement for
  //   this expression.
  virtual bool HaveColumnRef() const {
    return true;
  }

  static PTExpr::SharedPtr CreateConst(MemoryContext *memctx,
                                       YBLocation::SharedPtr loc,
                                       PTBaseType::SharedPtr data_type);

  // Predicate for updating counter.  Only '+' and '-' expression support counter update.
  virtual CHECKED_STATUS CheckCounterUpdateSupport(SemContext *sem_context) const;

  // All expressions must define this Analyze() function, which does the following steps.
  // - Call Analyze() on child treenodes to run semantic analysis on the child nodes. The child
  //   nodes will then call their own child nodes and so forth. In short, we traverse the expression
  //   parse tree to run semantic analysis on them.
  // - Run semantic analysis on this node.
  // - The main job of semantics analysis is to run type resolution to find the correct values for
  //   ql_type and internal_type_ for expressions.
  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override = 0;

  // Check if this expression represents a column in an INDEX table.
  bool CheckIndexColumn(SemContext *sem_context);

  // Check if an operator is allowed in the current context before analyzing it.
  virtual CHECKED_STATUS CheckOperator(SemContext *sem_context);

  // The following functions are called to setup the states before analyzing an operand.
  virtual CHECKED_STATUS SetupSemStateForOp1(SemState *sem_state);
  virtual CHECKED_STATUS SetupSemStateForOp2(SemState *sem_state);
  virtual CHECKED_STATUS SetupSemStateForOp3(SemState *sem_state);

  // These functions are called by analyze to run type resolution on this expression.
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context);
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context,
                                         PTExpr::SharedPtr op1);
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context,
                                         PTExpr::SharedPtr op1,
                                         PTExpr::SharedPtr op2);
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context,
                                         PTExpr::SharedPtr op1,
                                         PTExpr::SharedPtr op2,
                                         PTExpr::SharedPtr op3);

  // Analyze LHS expression.
  virtual CHECKED_STATUS CheckLhsExpr(SemContext *sem_context);

  // Analyze RHS expression.
  virtual CHECKED_STATUS CheckRhsExpr(SemContext *sem_context);

  // Check if left and right values are compatible.
  virtual CHECKED_STATUS CheckInequalityOperands(SemContext *sem_context,
                                                 PTExpr::SharedPtr lhs,
                                                 PTExpr::SharedPtr rhs);
  // Check if left and right values are compatible.
  virtual CHECKED_STATUS CheckEqualityOperands(SemContext *sem_context,
                                               PTExpr::SharedPtr lhs,
                                               PTExpr::SharedPtr rhs);

  // Compare this node datatype with the expected type from the parent treenode.
  virtual CHECKED_STATUS CheckExpectedTypeCompatibility(SemContext *sem_context);

  // Access function for descriptor.
  const ColumnDesc *index_desc() const {
    return index_desc_;
  }

  const MCSharedPtr<MCString>& index_name() const {
    return index_name_;
  }

 protected:
  // Get the column descriptor for this expression. IndexTable can have expression as its column.
  const ColumnDesc *GetColumnDesc(const SemContext *sem_context);

  // Get the descriptor for a column name.
  const ColumnDesc *GetColumnDesc(const SemContext *sem_context, const MCString& col_name) const;

  // Get the descriptor for a column or expr name from either a DML STMT or a TABLE.
  const ColumnDesc *GetColumnDesc(const SemContext *sem_context,
                                  const MCString& col_name,
                                  PTDmlStmt *stmt) const;

  ExprOperator op_;
  yb::QLOperator ql_op_;
  InternalType internal_type_;
  QLType::SharedPtr ql_type_;
  InternalType expected_internal_type_;

  // Fields that should be resolved by semantic analysis.
  // An expression might be a reference to a column in an INDEX.
  const ColumnDesc *index_desc_ = nullptr;
  MCSharedPtr<MCString> index_name_;
};

using PTExprListNode = TreeListNode<PTExpr>;

//--------------------------------------------------------------------------------------------------
// Tree Nodes for Collections -- treated as expressions with flexible arity
//--------------------------------------------------------------------------------------------------

class PTCollectionExpr : public PTExpr {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTCollectionExpr> SharedPtr;
  typedef MCSharedPtr<const PTCollectionExpr> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTCollectionExpr(MemoryContext* memctx,
                   YBLocation::SharedPtr loc,
                   const QLType::SharedPtr& ql_type)
      : PTExpr(memctx, loc, ExprOperator::kCollection, yb::QLOperator::QL_OP_NOOP,
               client::YBColumnSchema::ToInternalDataType(ql_type), ql_type),
        keys_(memctx), values_(memctx), udtype_field_values_(memctx) {}
  PTCollectionExpr(MemoryContext* memctx, YBLocation::SharedPtr loc, DataType literal_type)
      : PTCollectionExpr(memctx, loc, QLType::Create(literal_type)) {}
  virtual ~PTCollectionExpr() { }

  void AddKeyValuePair(PTExpr::SharedPtr key, PTExpr::SharedPtr value) {
    keys_.emplace_back(key);
    values_.emplace_back(value);
  }

  void AddElement(PTExpr::SharedPtr value) {
    values_.emplace_back(value);
  }

  // Fill in udtype_field_values collection, copying values in accordance to UDT field order
  CHECKED_STATUS InitializeUDTValues(const QLType::SharedPtr& expected_type,
                                     ProcessContextBase* process_context);

  int size() const {
    return static_cast<int>(values_.size());
  }

  const MCList<PTExpr::SharedPtr>& keys() const {
    return keys_;
  }

  const MCList<PTExpr::SharedPtr>& values() const {
    return values_;
  }

  const MCVector<PTExpr::SharedPtr>& udtype_field_values() const {
    return udtype_field_values_;
  }

  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTCollectionExpr::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTCollectionExpr>(memctx, std::forward<TypeArgs>(args)...);
  }

 private:
  MCList<PTExpr::SharedPtr> keys_;
  MCList<PTExpr::SharedPtr> values_;

  // This field will be decorated during analysis if this collection represents a user-defined type
  // It contains the field values in the order specified by the type (or nullptr for missing values)
  MCVector<PTExpr::SharedPtr> udtype_field_values_;
};

//--------------------------------------------------------------------------------------------------
// Template for expression with no operand (0 input).
template<InternalType itype, DataType ytype, class expr_class = PTExpr>
class PTExpr0 : public expr_class {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTExpr0> SharedPtr;
  typedef MCSharedPtr<const PTExpr0> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTExpr0(MemoryContext *memctx,
          YBLocation::SharedPtr loc,
          ExprOperator op,
          yb::QLOperator ql_op)
      : expr_class(memctx, loc, op, ql_op, itype, ytype) {
  }
  virtual ~PTExpr0() {
  }

  // Shared pointer support.
  template<typename... TypeArgs>
  inline static PTExpr0::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTExpr0>(memctx, std::forward<TypeArgs>(args)...);
  }

  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override {
    // Before traversing the expression, check if this whole expression is actually a column.
    if (this->CheckIndexColumn(sem_context)) {
      return Status::OK();
    }

    RETURN_NOT_OK(this->CheckOperator(sem_context));

    // Analyze this node operator and setup its ql_type and internal_type_.
    RETURN_NOT_OK(this->AnalyzeOperator(sem_context));

    // Make sure that this expression has valid data type.
    return this->CheckExpectedTypeCompatibility(sem_context);
  }
};

//--------------------------------------------------------------------------------------------------
// Template for expression with one operand (1 input).
template<InternalType itype, DataType ytype, class expr_class = PTExpr>
class PTExpr1 : public expr_class {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTExpr1> SharedPtr;
  typedef MCSharedPtr<const PTExpr1> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTExpr1(MemoryContext *memctx,
          YBLocation::SharedPtr loc,
          ExprOperator op,
          yb::QLOperator ql_op,
          PTExpr::SharedPtr op1)
      : expr_class(memctx, loc, op, ql_op, itype, ytype),
        op1_(op1) {
  }
  virtual ~PTExpr1() {
  }

  // Shared pointer support.
  template<typename... TypeArgs>
  inline static PTExpr1::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTExpr1>(memctx, std::forward<TypeArgs>(args)...);
  }

  PTExpr::SharedPtr op1() const override {
    return op1_;
  }

  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override {
    // Before traversing the expression, check if this whole expression is actually a column.
    if (this->CheckIndexColumn(sem_context)) {
      return Status::OK();
    }

    RETURN_NOT_OK(this->CheckOperator(sem_context));

    // Run semantic analysis on child nodes.
    SemState sem_state(sem_context);
    RETURN_NOT_OK(this->SetupSemStateForOp1(&sem_state));
    RETURN_NOT_OK(op1_->Analyze(sem_context));
    sem_state.ResetContextState();

    // Analyze this node operator and setup its ql_type and internal_type_.
    RETURN_NOT_OK(this->AnalyzeOperator(sem_context, op1_));

    // Make sure that it has valid data type.
    return this->CheckExpectedTypeCompatibility(sem_context);
  }

 protected:
  //------------------------------------------------------------------------------------------------
  // Operand.
  PTExpr::SharedPtr op1_;
};

//--------------------------------------------------------------------------------------------------
// Template for expression with two operands (2 inputs).
template<InternalType itype, DataType ytype, class expr_class = PTExpr>
class PTExpr2 : public expr_class {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTExpr2> SharedPtr;
  typedef MCSharedPtr<const PTExpr2> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTExpr2(MemoryContext *memctx,
          YBLocation::SharedPtr loc,
          ExprOperator op,
          yb::QLOperator ql_op,
          const PTExpr::SharedPtr& op1,
          const PTExpr::SharedPtr& op2)
      : expr_class(memctx, loc, op, ql_op, itype, ytype),
        op1_(op1),
        op2_(op2) {
  }
  virtual ~PTExpr2() {
  }

  // Shared pointer support.
  template<typename... TypeArgs>
  inline static PTExpr2::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTExpr2>(memctx, std::forward<TypeArgs>(args)...);
  }

  PTExpr::SharedPtr op1() const override {
    return op1_;
  }

  PTExpr::SharedPtr op2() const override {
    return op2_;
  }

  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override {
    // Before traversing the expression, check if this whole expression is actually a column.
    if (this->CheckIndexColumn(sem_context)) {
      return Status::OK();
    }

    RETURN_NOT_OK(this->CheckOperator(sem_context));

    // Run semantic analysis on child nodes.
    SemState sem_state(sem_context);
    RETURN_NOT_OK(this->SetupSemStateForOp1(&sem_state));
    RETURN_NOT_OK(op1_->Analyze(sem_context));

    RETURN_NOT_OK(this->SetupSemStateForOp2(&sem_state));
    RETURN_NOT_OK(op2_->Analyze(sem_context));
    sem_state.ResetContextState();

    // Analyze this node operator and setup its ql_type and internal_type_.
    RETURN_NOT_OK(this->AnalyzeOperator(sem_context, op1_, op2_));

    // Make sure that it has valid data type.
    return this->CheckExpectedTypeCompatibility(sem_context);
  }

 protected:
  //------------------------------------------------------------------------------------------------
  // Operand 1 and 2.
  PTExpr::SharedPtr op1_;
  PTExpr::SharedPtr op2_;
};

//--------------------------------------------------------------------------------------------------
// Template for expression with two operands (3 inputs).
template<InternalType itype, DataType ytype, class expr_class = PTExpr>
class PTExpr3 : public expr_class {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTExpr3> SharedPtr;
  typedef MCSharedPtr<const PTExpr3> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTExpr3(MemoryContext *memctx,
          YBLocation::SharedPtr loc,
          ExprOperator op,
          yb::QLOperator ql_op,
          const PTExpr::SharedPtr& op1,
          const PTExpr::SharedPtr& op2,
          const PTExpr::SharedPtr& op3)
      : expr_class(memctx, loc, op, ql_op, itype, ytype),
        op1_(op1),
        op2_(op2),
        op3_(op3) {
  }
  virtual ~PTExpr3() {
  }

  // Shared pointer support.
  template<typename... TypeArgs>
  inline static PTExpr3::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTExpr3>(memctx, std::forward<TypeArgs>(args)...);
  }

  PTExpr::SharedPtr op1() const override {
    return op1_;
  }

  PTExpr::SharedPtr op2() const override {
    return op2_;
  }

  PTExpr::SharedPtr op3() const override {
    return op3_;
  }

  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override {
    // Before traversing the expression, check if this whole expression is actually a column.
    if (this->CheckIndexColumn(sem_context)) {
      return Status::OK();
    }

    RETURN_NOT_OK(this->CheckOperator(sem_context));

    // Run semantic analysis on child nodes.
    SemState sem_state(sem_context);
    RETURN_NOT_OK(this->SetupSemStateForOp1(&sem_state));
    RETURN_NOT_OK(op1_->Analyze(sem_context));

    RETURN_NOT_OK(this->SetupSemStateForOp2(&sem_state));
    RETURN_NOT_OK(op2_->Analyze(sem_context));

    RETURN_NOT_OK(this->SetupSemStateForOp3(&sem_state));
    RETURN_NOT_OK(op3_->Analyze(sem_context));
    sem_state.ResetContextState();

    // Analyze this node operator and setup its ql_type and internal_type_.
    RETURN_NOT_OK(this->AnalyzeOperator(sem_context, op1_, op2_, op3_));

    // Make sure that it has valid data type.
    return this->CheckExpectedTypeCompatibility(sem_context);
  }

 protected:
  //------------------------------------------------------------------------------------------------
  // Operand 1 and 2.
  PTExpr::SharedPtr op1_;
  PTExpr::SharedPtr op2_;
  PTExpr::SharedPtr op3_;
};

//--------------------------------------------------------------------------------------------------
// Tree node for constants
//--------------------------------------------------------------------------------------------------
// Template for constant expressions.
template<typename ReturnType>
class PTLiteral {
 public:
  explicit PTLiteral(ReturnType value) : value_(value) { }

  virtual ~PTLiteral() { }

  virtual ReturnType Eval() const { return value_; }

  virtual ReturnType value() const { return value_; }

  virtual string ToQLName(int64_t value) const {
    return std::to_string(value);
  }

  virtual string ToQLName(int32_t value) const {
    return std::to_string(value);
  }

  virtual string ToQLName(int16_t value) const {
    return std::to_string(value);
  }

  virtual string ToQLName(uint32_t value) const {
    return std::to_string(value);
  }

  virtual string ToQLName(long double value) const {
    return std::to_string(value);
  }

  virtual string ToQLName(float value) const {
    return std::to_string(value);
  }

  virtual string ToQLName(const string& value) const {
    return value;
  }

  virtual string ToQLName(bool value) const {
    return std::to_string(value);
  }

  virtual string ToQLName(void *ptr) const {
    return "NULL";
  }

  virtual string ToQLName(const MCSharedPtr<MCString>& value) const {
    return value->c_str();
  }

 protected:
  ReturnType value_;
};

template<InternalType itype, DataType ytype,
         typename ReturnType, typename LiteralType = PTLiteral<ReturnType>>
class PTExprConst : public PTExpr0<itype, ytype>,
                    public LiteralType {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTExprConst> SharedPtr;
  typedef MCSharedPtr<const PTExprConst> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTExprConst(MemoryContext *memctx,
              YBLocation::SharedPtr loc,
              ReturnType value)
      : PTExpr0<itype, ytype>(memctx, loc, ExprOperator::kConst, yb::QLOperator::QL_OP_NOOP),
        LiteralType(value) {
  }
  virtual ~PTExprConst() {
  }

  // Shared pointer support.
  template<typename... TypeArgs>
  inline static PTExprConst::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTExprConst>(memctx, std::forward<TypeArgs>(args)...);
  }

  using PTExpr0<itype, ytype>::AnalyzeOperator;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context) override {
    // Nothing to do: constant expressions should be initialized with valid data type already
    return Status::OK();
  };

  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName)
      const override {
    return LiteralType::ToQLName(LiteralType::value());
  }

  virtual bool HaveColumnRef() const override {
    return false;
  }
};

// NULL constant.
using PTConstArg = PTExprConst<InternalType::VALUE_NOT_SET,
                               DataType::UNKNOWN_DATA,
                               void*>;

using PTNull = PTExprConst<InternalType::VALUE_NOT_SET,
                           DataType::NULL_VALUE_TYPE,
                           void*>;

// This class is used only for the dummy (meaningless) '*' such as in COUNT(*).
class PTStar : public PTNull {
 public:
  // Public types.
  typedef MCSharedPtr<PTStar> SharedPtr;
  typedef MCSharedPtr<const PTStar> SharedPtrConst;

  // Constructor and destructor.
  PTStar(MemoryContext *memctx, YBLocation::SharedPtr loc)
      : PTNull(memctx, loc, nullptr) {
  }

  // Shared pointer support.
  template<typename... TypeArgs>
  inline static PTStar::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTStar>(memctx, std::forward<TypeArgs>(args)...);
  }

  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const override {
    return "";
  }

  virtual bool IsDummyStar() const override {
    return true;
  }

  virtual bool HaveColumnRef() const override {
    return false;
  }
};

// String base classes for constant expression.
class PTLiteralString : public PTLiteral<MCSharedPtr<MCString>> {
 public:
  explicit PTLiteralString(MCSharedPtr<MCString> value);
  virtual ~PTLiteralString();

  CHECKED_STATUS ToInt64(int64_t *value, bool negate) const;
  CHECKED_STATUS ToDouble(long double *value, bool negate) const;
  CHECKED_STATUS ToDecimal(util::Decimal *value, bool negate) const;
  CHECKED_STATUS ToDecimal(std::string *value, bool negate) const;
  CHECKED_STATUS ToVarInt(std::string *value, bool negate) const;

  std::string ToString() const;

  CHECKED_STATUS ToString(std::string *value) const;
  CHECKED_STATUS ToTimestamp(int64_t *value) const;
  CHECKED_STATUS ToDate(uint32_t *value) const;
  CHECKED_STATUS ToTime(int64_t *value) const;

  CHECKED_STATUS ToInetaddress(InetAddress *value) const;
};
using PTConstText = PTExprConst<InternalType::kStringValue,
                                DataType::STRING,
                                MCSharedPtr<MCString>,
                                PTLiteralString>;
using PTConstVarInt = PTExprConst<InternalType::kVarintValue,
                                  DataType::VARINT,
                                  MCSharedPtr<MCString>,
                                  PTLiteralString>;
using PTConstDecimal = PTExprConst<InternalType::kDecimalValue,
                                   DataType::DECIMAL,
                                   MCSharedPtr<MCString>,
                                   PTLiteralString>;
using PTConstUuid = PTExprConst<InternalType::kUuidValue,
                                DataType::UUID,
                                MCSharedPtr<MCString>,
                                PTLiteralString>;
using PTConstBinary = PTExprConst<InternalType::kBinaryValue,
                                  DataType::BINARY,
                                  MCSharedPtr<MCString>,
                                  PTLiteralString>;

// Boolean constant.
using PTConstBool = PTExprConst<InternalType::kBoolValue,
                                DataType::BOOL,
                                bool>;

// Obsolete numeric constant classes.
using PTConstInt = PTExprConst<InternalType::kInt64Value,
                               DataType::INT64,
                               int64_t>;

using PTConstInt32 = PTExprConst<InternalType::kInt32Value,
                                 DataType::INT32,
                                 int32_t>;

using PTConstInt16 = PTExprConst<InternalType::kInt16Value,
                                 DataType::INT16,
                                 int16_t>;

using PTConstDouble = PTExprConst<InternalType::kDoubleValue,
                                  DataType::DOUBLE,
                                  long double>;

using PTConstFloat = PTExprConst<InternalType::kFloatValue,
                                 DataType::FLOAT,
                                 float>;

using PTConstTimestamp = PTExprConst<InternalType::kTimestampValue,
                                     DataType::TIMESTAMP,
                                     int64_t>;

using PTConstDate = PTExprConst<InternalType::kDateValue,
                                DataType::DATE,
                                uint32_t>;

// Class representing a json operator.
class PTJsonOperator : public PTExpr {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTJsonOperator> SharedPtr;
  typedef MCSharedPtr<const PTJsonOperator> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructors and destructor.
  PTJsonOperator(MemoryContext *memctx,
                 YBLocation::SharedPtr loc,
                 const JsonOperator& json_operator,
                 const PTExpr::SharedPtr& arg);

  virtual ~PTJsonOperator();

  template<typename... TypeArgs>
  inline static PTJsonOperator::SharedPtr MakeShared(MemoryContext *memctx,
                                                     TypeArgs&&... args) {
    return MCMakeShared<PTJsonOperator>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;

  const PTExpr::SharedPtr& arg() const {
    return arg_;
  }

  JsonOperator json_operator() const {
    return json_operator_;
  }

  // Selected name.
  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const override {
    string jquote = "'";
    string op_name = json_operator_ == JsonOperator::JSON_OBJECT ? "->" : "->>";
    string jattr = arg_->QLName(option);
    if (option == QLNameOption::kMangledName) {
      jattr = YcqlName::MangleJsonAttrName(jattr);
    }

    return op_name + jquote + jattr + jquote;
  }

 protected:
  JsonOperator json_operator_;
  PTExpr::SharedPtr arg_;
};



//--------------------------------------------------------------------------------------------------
// Tree node for logical expressions (AND, OR, NOT, ...).
//--------------------------------------------------------------------------------------------------
class PTLogicExpr : public PTExpr {
 public:
  explicit PTLogicExpr(
      MemoryContext *memctx,
      YBLocation::SharedPtr loc,
      ExprOperator op = ExprOperator::kNoOp,
      yb::QLOperator ql_op = yb::QLOperator::QL_OP_NOOP,
      InternalType internal_type = InternalType::VALUE_NOT_SET,
      DataType ql_type_id = DataType::BOOL)
      : PTExpr(memctx, loc, op, ql_op, internal_type, ql_type_id) {
  }

  // Setup states before analyzing operand.
  virtual CHECKED_STATUS SetupSemStateForOp1(SemState *sem_state) override;
  virtual CHECKED_STATUS SetupSemStateForOp2(SemState *sem_state) override;

  // Analyze this operator.
  using PTExpr::AnalyzeOperator;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context,
                                         PTExpr::SharedPtr op1) override;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context,
                                         PTExpr::SharedPtr op1,
                                         PTExpr::SharedPtr op2) override;
};
using PTLogic1 = PTExpr1<InternalType::kBoolValue, DataType::BOOL, PTLogicExpr>;
using PTLogic2 = PTExpr2<InternalType::kBoolValue, DataType::BOOL, PTLogicExpr>;

//--------------------------------------------------------------------------------------------------
// Tree node for relational expressions (=, !=, >, ...).
//--------------------------------------------------------------------------------------------------
class PTRelationExpr : public PTExpr {
 public:
  explicit PTRelationExpr(
      MemoryContext *memctx,
      YBLocation::SharedPtr loc,
      ExprOperator op = ExprOperator::kNoOp,
      yb::QLOperator ql_op = yb::QLOperator::QL_OP_NOOP,
      InternalType internal_type = InternalType::VALUE_NOT_SET,
      DataType ql_type_id = DataType::BOOL)
      : PTExpr(memctx, loc, op, ql_op, internal_type, ql_type_id) {
  }

  // Setup states before analyzing operands.
  virtual CHECKED_STATUS SetupSemStateForOp1(SemState *sem_state) override;
  virtual CHECKED_STATUS SetupSemStateForOp2(SemState *sem_state) override;
  virtual CHECKED_STATUS SetupSemStateForOp3(SemState *sem_state) override;

  // Analyze this operator after all operands were analyzed.
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context) override;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context,
                                         PTExpr::SharedPtr op1) override;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context,
                                         PTExpr::SharedPtr op1,
                                         PTExpr::SharedPtr op2) override;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context,
                                         PTExpr::SharedPtr op1,
                                         PTExpr::SharedPtr op2,
                                         PTExpr::SharedPtr op3) override;
  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const override;
};
using PTRelation0 = PTExpr0<InternalType::kBoolValue, DataType::BOOL, PTRelationExpr>;
using PTRelation1 = PTExpr1<InternalType::kBoolValue, DataType::BOOL, PTRelationExpr>;
using PTRelation2 = PTExpr2<InternalType::kBoolValue, DataType::BOOL, PTRelationExpr>;
using PTRelation3 = PTExpr3<InternalType::kBoolValue, DataType::BOOL, PTRelationExpr>;

//--------------------------------------------------------------------------------------------------
// Tree node for operators.
// - Currently, we only have unary '-'.
// - Generally, we only need PTOperator for performance purposes. All operations can be supported
//   by adding it to builtin library, but that method is less efficient and PTOperator.
//--------------------------------------------------------------------------------------------------
class PTOperatorExpr : public PTExpr {
 public:
  explicit PTOperatorExpr(
      MemoryContext *memctx,
      YBLocation::SharedPtr loc,
      ExprOperator op = ExprOperator::kNoOp,
      yb::QLOperator ql_op = yb::QLOperator::QL_OP_NOOP,
      InternalType internal_type = InternalType::VALUE_NOT_SET,
      DataType ql_type_id = DataType::UNKNOWN_DATA)
      : PTExpr(memctx, loc, op, ql_op, internal_type, ql_type_id) {
  }

  // Setup states before analyzing operands.
  virtual CHECKED_STATUS SetupSemStateForOp1(SemState *sem_state) override;

  // Analyze this operator after all operands were analyzed.
  using PTExpr::AnalyzeOperator;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context, PTExpr::SharedPtr op1) override;
};

using PTOperator0 = PTExpr0<InternalType::VALUE_NOT_SET, DataType::UNKNOWN_DATA, PTOperatorExpr>;
using PTOperator1 = PTExpr1<InternalType::VALUE_NOT_SET, DataType::UNKNOWN_DATA, PTOperatorExpr>;
using PTOperator2 = PTExpr2<InternalType::VALUE_NOT_SET, DataType::UNKNOWN_DATA, PTOperatorExpr>;
using PTOperator3 = PTExpr3<InternalType::VALUE_NOT_SET, DataType::UNKNOWN_DATA, PTOperatorExpr>;

//--------------------------------------------------------------------------------------------------
// Column Reference. The datatype of this expression would need to be resolved by the analyzer.
class PTRef : public PTOperator0 {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTRef> SharedPtr;
  typedef MCSharedPtr<const PTRef> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTRef(MemoryContext *memctx,
        YBLocation::SharedPtr loc,
        const PTQualifiedName::SharedPtr& name);
  virtual ~PTRef();

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTRef::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTRef>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  void PrintSemanticAnalysisResult(SemContext *sem_context);

  using PTOperatorExpr::AnalyzeOperator;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context) override;

  // Add the name of column that is being referenced to output parameter.
  void CollectReferencedIndexColnames(MCSet<string> *col_names) const override {
    col_names->insert(QLName(QLNameOption::kMangledName));
  }

  // Selected name.
  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const override {
    if (option == QLNameOption::kMetadataName) {
      // Should only be called after the descriptor is loaded from the catalog by Analyze().
      CHECK(desc_) << "Metadata is not yet loaded to this node";
      return desc_->MetadataName();
    }

    if (option == QLNameOption::kMangledName) {
      return YcqlName::MangleColumnName(name_->QLName());
    }

    return name_->QLName();
  }

  // Access function for name.
  const PTQualifiedName::SharedPtr& name() const {
    return name_;
  }

  // Construct bind variable name from the name of this column.
  const MCSharedPtr<MCString>& bindvar_name() const {
    return name_->bindvar_name();
  }

  // Access function for descriptor.
  const ColumnDesc *desc() const {
    return desc_;
  }

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTRef;
  }

  // Analyze LHS expression.
  virtual CHECKED_STATUS CheckLhsExpr(SemContext *sem_context) override;

 private:
  PTQualifiedName::SharedPtr name_;

  // Fields that should be resolved by semantic analysis.
  const ColumnDesc *desc_;
};

// A json column with json operators applied to the column.
class PTJsonColumnWithOperators : public PTOperator0 {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTJsonColumnWithOperators> SharedPtr;
  typedef MCSharedPtr<const PTJsonColumnWithOperators> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTJsonColumnWithOperators(MemoryContext *memctx,
                            YBLocation::SharedPtr loc,
                            const PTQualifiedName::SharedPtr& name,
                            const PTExprListNode::SharedPtr& operators);
  virtual ~PTJsonColumnWithOperators();

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTJsonColumnWithOperators::SharedPtr MakeShared(MemoryContext *memctx,
                                                          TypeArgs&&... args) {
    return MCMakeShared<PTJsonColumnWithOperators>(memctx, std::forward<TypeArgs>(args)...);
  }

  using PTOperatorExpr::AnalyzeOperator;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context) override;

  // Access function for name.
  const PTQualifiedName::SharedPtr& name() const {
    return name_;
  }

  // Add the name of this JSONB expression to output parameter.
  void CollectReferencedIndexColnames(MCSet<string> *col_names) const override {
    col_names->insert(QLName(QLNameOption::kMangledName));
  }

  // Selected name.
  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const override {
    string qlname;
    if (option == QLNameOption::kMetadataName) {
      DCHECK(desc_) << "Metadata is not yet loaded to this node";
      qlname = desc_->MetadataName();
    } else if (option == QLNameOption::kMangledName) {
      qlname = YcqlName::MangleColumnName(name_->QLName());
    } else {
      qlname = name_->QLName();
    }

    for (PTExpr::SharedPtr expr : operators_->node_list()) {
      qlname += expr->QLName(option);
    }
    return qlname;
  }

  const PTExprListNode::SharedPtr& operators() const {
    return operators_;
  }

  // Access function for descriptor.
  const ColumnDesc *desc() const {
    return desc_;
  }

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTJsonOp;
  }

  // Analyze LHS expression.
  virtual CHECKED_STATUS CheckLhsExpr(SemContext *sem_context) override;

 private:
  PTQualifiedName::SharedPtr name_;
  PTExprListNode::SharedPtr operators_;

  // Fields that should be resolved by semantic analysis.
  const ColumnDesc *desc_ = nullptr;
};

// SubColumn Reference. The datatype of this expression would need to be resolved by the analyzer.
class PTSubscriptedColumn : public PTOperator0 {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTSubscriptedColumn> SharedPtr;
  typedef MCSharedPtr<const PTSubscriptedColumn> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTSubscriptedColumn(MemoryContext *memctx,
        YBLocation::SharedPtr loc,
        const PTQualifiedName::SharedPtr& name,
        const PTExprListNode::SharedPtr& args);
  virtual ~PTSubscriptedColumn();

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTSubscriptedColumn::SharedPtr MakeShared(MemoryContext *memctx,
                                                          TypeArgs&&... args) {
    return MCMakeShared<PTSubscriptedColumn>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  void PrintSemanticAnalysisResult(SemContext *sem_context);

  using PTOperatorExpr::AnalyzeOperator;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context) override;

  // Access function for name.
  const PTQualifiedName::SharedPtr& name() const {
    return name_;
  }

  // Access function for name.
  const PTExprListNode::SharedPtr& args() const {
    return args_;
  }

  // Construct bind variable name from the name of this column.
  const MCSharedPtr<MCString>& bindvar_name() const {
    return name_->bindvar_name();
  }

  // Access function for descriptor.
  const ColumnDesc *desc() const {
    return desc_;
  }

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTSubscript;
  }

  // Analyze LHS expression.
  virtual CHECKED_STATUS CheckLhsExpr(SemContext *sem_context) override;

 private:
  PTQualifiedName::SharedPtr name_;
  PTExprListNode::SharedPtr args_;

  // Fields that should be resolved by semantic analysis.
  const ColumnDesc *desc_;
};

//--------------------------------------------------------------------------------------------------
// Reference to all columns of all selected tables.
class PTAllColumns : public PTOperator0 {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTAllColumns> SharedPtr;
  typedef MCSharedPtr<const PTAllColumns> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTAllColumns(MemoryContext *memctx, YBLocation::SharedPtr loc);
  virtual ~PTAllColumns();

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTAllColumns::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTAllColumns>(memctx, std::forward<TypeArgs>(args)...);
  }

  using PTOperatorExpr::AnalyzeOperator;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context) override;

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTAllColumns;
  }

  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const override {
    // We should not get here as '*' should have been converted into a list of column name before
    // the selected tuple is constructed and described.
    VLOG(3) << "Calling QLName for '*' is not expected";
    return "*";
  }

  const MCVector<ColumnDesc>& columns() const {
    return columns_;
  }

 private:
  // Fields that should be resolved by semantic analysis.
  MCVector<ColumnDesc> columns_;
};

//--------------------------------------------------------------------------------------------------
// Expression alias - Name of an expression including reference to column.
class PTExprAlias : public PTOperator1 {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTExprAlias> SharedPtr;
  typedef MCSharedPtr<const PTExprAlias> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTExprAlias(MemoryContext *memctx,
              YBLocation::SharedPtr loc,
              const PTExpr::SharedPtr& expr,
              const MCSharedPtr<MCString>& alias);
  virtual ~PTExprAlias();

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTExprAlias::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTExprAlias>(memctx, std::forward<TypeArgs>(args)...);
  }

  virtual CHECKED_STATUS SetupSemStateForOp1(SemState *sem_state) override;

  using PTOperatorExpr::AnalyzeOperator;
  virtual CHECKED_STATUS AnalyzeOperator(SemContext *sem_context, PTExpr::SharedPtr op1) override;

  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const override {
    return alias_->c_str();
  }

  // Alias result set column type in QL format.
  virtual void rscol_type_PB(QLTypePB *pb_type) const override {
    return op1_->rscol_type_PB(pb_type);
  }

  // Predicate for calls to aggregate functions.
  virtual bool IsAggregateCall() const override {
    return (op1_ != nullptr && op1_->IsAggregateCall());
  }
  virtual yb::bfql::TSOpcode aggregate_opcode() const override {
    DCHECK(op1_ != nullptr) << "Reading aggregate opcode from a NULL operator";
    return op1_->aggregate_opcode();
  }

 private:
  MCSharedPtr<MCString> alias_;
};

//--------------------------------------------------------------------------------------------------
// Bind variable. The datatype of this expression would need to be resolved by the analyzer.
class PTBindVar : public PTExpr {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTBindVar> SharedPtr;
  typedef MCSharedPtr<const PTBindVar> SharedPtrConst;

  // Compare 2 bind variable positions in a statement.
  struct SetCmp {
    bool operator() (const PTBindVar* v1, const PTBindVar* v2) const {
      const YBLocation& l1 = v1->loc();
      const YBLocation& l2 = v2->loc();
      return (l1.BeginLine() < l2.BeginLine() ||
              (l1.BeginLine() == l2.BeginLine() && l1.BeginColumn() < l2.BeginColumn()));
    }
  };

  // Compare 2 bind variables for their hash column ids.
  struct HashColCmp {
    bool operator() (const PTBindVar* v1, const PTBindVar* v2) const {
      DCHECK(v1->hash_col() != nullptr) << "bindvar pos " << v1->pos() << " is not a hash column";
      DCHECK(v2->hash_col() != nullptr) << "bindvar pos " << v2->pos() << " is not a hash column";
      return v1->hash_col()->id() < v2->hash_col()->id();
    }
  };

  //------------------------------------------------------------------------------------------------
  // Constructor and destructor.
  PTBindVar(MemoryContext *memctx,
            YBLocation::SharedPtr loc,
            const MCSharedPtr<MCString>& name = nullptr);
  PTBindVar(MemoryContext *memctx,
            YBLocation::SharedPtr loc,
            PTConstVarInt::SharedPtr user_pos);
  virtual ~PTBindVar();

  // Support for shared_ptr.
  template<typename... TypeArgs>
  inline static PTBindVar::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<PTBindVar>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;
  void PrintSemanticAnalysisResult(SemContext *sem_context);

  // Access functions for position.
  int64_t pos() const {
    return *pos_;
  }
  void set_pos(const int64_t pos) {
    pos_ = pos;
  }
  bool is_unset_pos() const {
    return !pos_;
  }

  // Access functions for name.
  const MCSharedPtr<MCString>& name() const {
    return name_;
  }

  // Access function for hash column if available.
  const ColumnDesc *hash_col() const {
    return hash_col_;
  }

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTBindVar;
  }

  virtual string QLName(QLNameOption option = QLNameOption::kUserOriginalName) const override {
    string qlname = (user_pos_) ? user_pos_->ToString() : name()->c_str();
    return ":" +  qlname;
  }

  // Access to op_.
  virtual ExprOperator expr_op() const override {
    return ExprOperator::kBindVar;
  }

  // The name Cassandra uses for the virtual column when binding LIMIT clause
  static const string& limit_bindvar_name() {
    static string limit_bindvar_name = "[limit]";
    return limit_bindvar_name;
  }

  // The name Cassandra uses for the virtual column when binding OFFSET clause
  static const string& offset_bindvar_name() {
    static string offset_bindvar_name = "[offset]";
    return offset_bindvar_name;
  }

  // The name Cassandra uses for the virtual column when binding USING TTL clause
  static const string& ttl_bindvar_name() {
    static string ttl_bindvar_name = "[ttl]";
    return ttl_bindvar_name;
  }

  // The name Cassandra uses for the virtual column when binding USING TIMESTAMP clause
  static const string& timestamp_bindvar_name() {
    static string timestamp_bindvar_name = "[timestamp]";
    return timestamp_bindvar_name;
  }

  // The name Cassandra uses for the virtual column when binding the partition key (i.e. with token)
  static const string& token_bindvar_name() {
    static string token_bindvar_name = "partition key token";
    return token_bindvar_name;
  }

  // Name used for binding the 'partition_hash()' builtin function.
  static const string& partition_hash_bindvar_name() {
    static string partition_hash_bindvar_name = "[partition_hash]";
    return partition_hash_bindvar_name;
  }

  // The name Cassandra uses for binding the args of a builtin system call e.g. "token(?, ?)"
  static const string bcall_arg_bindvar_name(const string& bcall_name, size_t arg_position) {
    return strings::Substitute("arg$0(system.$1)", arg_position, bcall_name);
  }

  // The name Cassandra uses for binding the collection elements.
  static const string coll_bindvar_name(const string& col_name) {
    return strings::Substitute("value($0)", col_name);
  }

  // The name for binding the JSON attributes.
  static const string json_bindvar_name(const string& col_name) {
    return strings::Substitute("json_attr($0)", col_name);
  }

  // Use the binding name by default (if no other cases applicable).
  static const string& default_bindvar_name() {
    static string default_bindvar_name = "expr";
    return default_bindvar_name;
  }

 private:
  // 0-based position.
  PTConstVarInt::SharedPtr user_pos_; // pos used for parsing.
  boost::optional<int64_t> pos_; // pos after parsing is done.
  // Variable name.
  MCSharedPtr<MCString> name_;
  // Hash column descriptor.
  const ColumnDesc *hash_col_ = nullptr;
};

}  // namespace ql
}  // namespace yb

#endif  // YB_YQL_CQL_QL_PTREE_PT_EXPR_H_
