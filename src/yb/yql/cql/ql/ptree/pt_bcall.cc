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
// Treenode definitions for expressions.
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/ptree/pt_bcall.h"

#include "yb/client/client.h"
#include "yb/client/table.h"

#include "yb/yql/cql/ql/ptree/sem_context.h"
#include "yb/util/bfql/bfql.h"
#include "yb/common/ql_bfunc.h"

namespace yb {
namespace ql {

using std::vector;
using std::shared_ptr;
using std::make_shared;
using strings::Substitute;

using yb::bfql::BFOpcode;
using yb::bfql::BFOPCODE_NOOP;
using yb::bfql::TSOpcode;
using yb::bfql::BFDecl;

using yb::client::YBColumnSchema;

using BfuncCompile = yb::bfql::BFCompileApi<PTExpr, PTExpr>;

//--------------------------------------------------------------------------------------------------

PTBcall::PTBcall(MemoryContext *memctx,
                 YBLocation::SharedPtr loc,
                 const MCSharedPtr<MCString>& name,
                 PTExprListNode::SharedPtr args)
  : PTExpr(memctx, loc, ExprOperator::kBcall, QLOperator::QL_OP_NOOP),
    name_(name),
    args_(args),
    is_server_operator_(false),
    bfopcode_(static_cast<int32_t>(BFOPCODE_NOOP)),
    cast_ops_(memctx),
    result_cast_op_(BFOPCODE_NOOP) {
}

PTBcall::~PTBcall() {
}

void PTBcall::CollectReferencedIndexColnames(MCSet<string> *col_names) const {
  for (auto arg : args_->node_list()) {
    arg->CollectReferencedIndexColnames(col_names);
  }
}

string PTBcall::QLName(QLNameOption option) const {
  string arg_names;
  string keyspace;

  // cql_cast() is displayed as "cast(<col> as <type>)".
  if (strcmp(name_->c_str(), bfql::kCqlCastFuncName) == 0) {
    CHECK_GE(args_->size(), 2);
    const string column_name = args_->element(0)->QLName(option);
    const string type =  QLType::ToCQLString(args_->element(1)->ql_type()->type_info()->type());
    return strings::Substitute("cast($0 as $1)", column_name, type);
  }

  for (auto arg : args_->node_list()) {
    if (!arg_names.empty()) {
      arg_names += ", ";
    }
    arg_names += arg->QLName(option);
  }
  if (IsAggregateCall()) {
    // count(*) is displayed as "count".
    if (arg_names.empty()) {
      return name_->c_str();
    }
    keyspace += "system.";
  }
  return strings::Substitute("$0$1($2)", keyspace, name_->c_str(), arg_names);
}

bool PTBcall::IsAggregateCall() const {
  return is_server_operator_ && BFDecl::is_aggregate_op(static_cast<TSOpcode>(bfopcode_));
}

CHECKED_STATUS PTBcall::Analyze(SemContext *sem_context) {
  // Before traversing the expression, check if this whole expression is actually a column.
  if (CheckIndexColumn(sem_context)) {
    return Status::OK();
  }

  RETURN_NOT_OK(CheckOperator(sem_context));

  // Analyze arguments of the function call.
  // - Because of function overloading, we cannot determine the expected type before reading the
  //   argument. At this point, the expected type is set to unknown, which is compatible with
  //   all data type.
  // - If the datatype of an argument (such as bind variable) is not determined, and the overloaded
  //   function call cannot be resolved by the rest of the arguments, the parser should raised
  //   error for multiple matches.
  SemState sem_state(sem_context, QLType::Create(UNKNOWN_DATA), InternalType::VALUE_NOT_SET);

  int pindex = 0;
  const MCList<PTExpr::SharedPtr>& exprs = args_->node_list();
  vector<PTExpr::SharedPtr> params(exprs.size());
  for (const PTExpr::SharedPtr& expr : exprs) {
    RETURN_NOT_OK(expr->Analyze(sem_context));
    RETURN_NOT_OK(expr->CheckRhsExpr(sem_context));

    params[pindex] = expr;
    pindex++;
  }

  RETURN_NOT_OK(CheckOperatorAfterArgAnalyze(sem_context));

  // Reset the semantics state after analyzing the arguments.
  sem_state.ResetContextState();

  // Type check the builtin call.
  BFOpcode bfopcode;
  const BFDecl *bfdecl;
  PTExpr::SharedPtr pt_result = PTConstArg::MakeShared(sem_context->PTempMem(), loc_, nullptr);
  Status s = BfuncCompile::FindQLOpcode(name_->c_str(), params, &bfopcode, &bfdecl, pt_result);
  if (!s.ok()) {
    std::string err_msg = string("Failed calling '") + name_->c_str();
    bool got_first_arg = false;
    err_msg += "(";
    for (auto param : params) {
      if (got_first_arg) {
        err_msg += ",";
      }
      err_msg += param->ql_type()->ToString();
      got_first_arg = true;
    }
    err_msg += ")'. ";
    err_msg += s.ToUserMessage();
    LOG(INFO) << err_msg;
    return sem_context->Error(this, err_msg.c_str(), ErrorCode::INVALID_FUNCTION_CALL);
  }

  if (!bfdecl->is_server_operator()) {
    // Use the builtin-function opcode since this is a regular builtin call.
    bfopcode_ = static_cast<int32_t>(bfopcode);

    if (*name_ == "cql_cast" || *name_ == "tojson") {
      // Argument must be of primitive type for these operators.
      for (const PTExpr::SharedPtr &expr : exprs) {
        if (expr->expr_op() == ExprOperator::kCollection) {
          return sem_context->Error(expr, "Input argument must be of primitive type",
                                    ErrorCode::INVALID_ARGUMENTS);
        }
      }
    }
  } else {
    // Use the server opcode since this is a server operator. Ignore the BFOpcode.
    is_server_operator_ = true;
    TSOpcode tsop = bfdecl->tsopcode();
    bfopcode_ = static_cast<int32_t>(tsop);

    // Check error for special cases.
    // TODO(neil) This should be part of the builtin table. Each entry in builtin table should have
    // a function pointer for CheckRequirement().  During analysis, QL will all apply these function
    // pointers to check for any special requirement of an entry.
    if (bfql::IsAggregateOpcode(tsop)) {
      if (!sem_context->allowing_aggregate()) {
        string errmsg =
          Substitute("Aggregate function $0() cannot be called in this context", name_->c_str());
        return sem_context->Error(loc(), errmsg.c_str(), ErrorCode::INVALID_ARGUMENTS);
      }
      const PTExpr::SharedPtr& expr = exprs.front();
      if (expr->expr_op() != ExprOperator::kRef &&
          (!expr->IsDummyStar() || tsop != TSOpcode::kCount)) {
        string errmsg = Substitute("Input argument for $0 must be a column", name_->c_str());
        return sem_context->Error(expr->loc(), errmsg.c_str(), ErrorCode::INVALID_ARGUMENTS);
      }
      if (tsop == TSOpcode::kMin || tsop == TSOpcode::kMax || tsop == TSOpcode::kAvg) {
        if (pt_result->ql_type()->IsAnyType()) {
          pt_result->set_ql_type(args_->element(0)->ql_type());
        }
      }
    } else if (tsop == TSOpcode::kTtl || tsop == TSOpcode::kWriteTime) {
      const PTExpr::SharedPtr& expr = exprs.front();
      if (expr->expr_op() != ExprOperator::kRef) {
        string errmsg = Substitute("Input argument for $0 must be a column", name_->c_str());
        return sem_context->Error(expr->loc(), errmsg.c_str(), ErrorCode::INVALID_ARGUMENTS);
      }
      const PTRef *ref = static_cast<const PTRef *>(expr.get());
      if (ref->desc()->is_primary()) {
        string errmsg = Substitute("Input argument for $0 cannot be primary key", name_->c_str());
        return sem_context->Error(expr->loc(), errmsg.c_str(), ErrorCode::INVALID_ARGUMENTS);
      }
      if (ref->desc()->ql_type()->IsParametric()) {
        string errmsg = Substitute("Input argument for $0 is of wrong datatype", name_->c_str());
        return sem_context->Error(expr->loc(), errmsg.c_str(), ErrorCode::INVALID_ARGUMENTS);
      }

    } else if (bfdecl->is_collection_bcall()) {
      // Collection operations require special handling during type analysis
      // 1. Casting check is not needed since conversion between collection types is not allowed
      // 2. Additional type inference is needed for the parameter types of the collections
      DCHECK(pt_result->ql_type()->IsParametric());
      cast_ops_.resize(exprs.size(), BFOPCODE_NOOP);

      if (!sem_context->processing_set_clause()) {
        return sem_context->Error(this, "Collection operations only allowed in set clause",
                                  ErrorCode::INVALID_FUNCTION_CALL);
      }

      auto it = exprs.begin();
      auto col_ref = *it;
      auto arg = *(++it);

      // list addition allows column ref to be second argument
      if (bfopcode == BFOpcode::OPCODE_LIST_PREPEND && col_ref->expr_op() != ExprOperator::kRef) {
        arg = col_ref;
        col_ref = *it;
      }

      if (col_ref->expr_op() != ExprOperator::kRef) {
        return sem_context->Error(col_ref.get(),
                                  "Expected reference to column of a collection datatype",
                                  ErrorCode::INVALID_FUNCTION_CALL);
      }

      PTRef *pt_ref = static_cast<PTRef *>(col_ref.get());
      if (sem_context->lhs_col() != pt_ref->desc()) {
        return sem_context->Error(
            this,
            "Expected main argument for collection operation to reference the column being set",
            ErrorCode::INVALID_FUNCTION_CALL);
      }

      if (arg->expr_op() != ExprOperator::kCollection && arg->expr_op() != ExprOperator::kBindVar) {
        return sem_context->Error(
            this,
            "Expected auxiliary argument for collection operations to be collection literal",
            ErrorCode::INVALID_FUNCTION_CALL);
      }

      SemState state(sem_context);
      // Argument type is same as type of the referenced column (except for subtracting from MAP)
      auto arg_ytype = col_ref->ql_type();
      auto arg_itype = col_ref->internal_type();
      // Subtraction from MAP takes keys to be removed as param so arg type for MAP<A,B> is SET<A>
      if (bfopcode == BFOpcode::OPCODE_MAP_REMOVE) {
        arg_ytype = QLType::CreateTypeSet(col_ref->ql_type()->param_type(0));
        arg_itype = InternalType::kSetValue;
      }
      state.SetExprState(arg_ytype, arg_itype);
      state.set_bindvar_name(pt_ref->desc()->name());
      RETURN_NOT_OK(arg->Analyze(sem_context));

      // Type resolution (for map/set): (cref + <expr>) should have same type as (cref).
      ql_type_ = col_ref->ql_type();
      internal_type_ = arg_itype;

      // return type is same as type of the referenced column
      state.SetExprState(col_ref->ql_type(), col_ref->internal_type());
      RETURN_NOT_OK(CheckExpectedTypeCompatibility(sem_context));

      // For "UPDATE ... SET list = list - x ..." , the list needs to be read first in order to
      // subtract (remove) elements from it.
      if (bfopcode == BFOpcode::OPCODE_LIST_REMOVE) {
        sem_context->current_dml_stmt()->AddColumnRef(*pt_ref->desc());
      }

      return Status::OK();
    }
  }

  // Find the cast operator if arguments' ql_type_id are not an exact match with signature.
  pindex = 0;
  cast_ops_.resize(exprs.size(), BFOPCODE_NOOP);
  const std::vector<DataType> &formal_types = bfdecl->param_types();
  for (const auto &expr : exprs) {
    if (formal_types[pindex] == DataType::TYPEARGS) {
      // For variadic functions, accept all arguments without casting.
      break;
    }

    // Converting or casting arguments to expected type for the function call.
    // - If argument and formal datatypes are the same, no conversion is needed. It's a NOOP.
    // - Currently, we only allowed constant expressions which would be folded to the correct type
    //   by the QL engine before creating protobuf messages.
    // - When we support more complex expressions, CAST would be used for the conversion. It'd be
    //   a bug if casting failed.
    if (expr->expr_op() == ExprOperator::kConst || expr->expr_op() == ExprOperator::kBindVar) {
      // Check if constant folding is possible between the two datatype.
      SemState arg_state(sem_context,
                         QLType::Create(formal_types[pindex]),
                         YBColumnSchema::ToInternalDataType(QLType::Create(formal_types[pindex])),
                         sem_context->bindvar_name());

      RETURN_NOT_OK(expr->Analyze(sem_context));

    } else if (expr->ql_type_id() != formal_types[pindex]) {
      // Future usage: Need to cast from argument type to formal type.
      s = BfuncCompile::FindCastOpcode(expr->ql_type_id(), formal_types[pindex],
          &cast_ops_[pindex]);
      if (!s.ok()) {
        LOG(ERROR) << "Arguments to builtin call " << name_->c_str() << "() is compatible with "
                   << "its signature but converting the argument to the desired type failed";
        string err_msg = (s.ToUserMessage() + "CAST " + expr->ql_type()->ToString() + " AS " +
            QLType::ToCQLString(formal_types[pindex]) + " failed");
        return sem_context->Error(this, err_msg.c_str(), ErrorCode::INVALID_FUNCTION_CALL);
      }
    }
    pindex++;
  }

  // TODO(neil) Not just BCALL, but each expression should have a "result_cast_op_". At execution
  // time if the cast is present, the result of the expression must be casted to the correct type.
  // Find the correct casting op for the result if expected type is not the same as return type.
  if (!sem_context->expr_expected_ql_type()->IsUnknown()) {
    s = BfuncCompile::FindCastOpcode(pt_result->ql_type()->main(),
                                     sem_context->expr_expected_ql_type()->main(),
                                     &result_cast_op_);
    if (!s.ok()) {
      string err_msg = Substitute("Cannot cast builtin call return type '$0' to expected type '$1'",
                                  pt_result->ql_type()->ToString(),
                                  sem_context->expr_expected_ql_type()->ToString());
      return sem_context->Error(this, err_msg.c_str(), ErrorCode::DATATYPE_MISMATCH);
    }

    ql_type_ = sem_context->expr_expected_ql_type();
  } else {
    ql_type_ = pt_result->ql_type();
  }

  internal_type_ = yb::client::YBColumnSchema::ToInternalDataType(ql_type_);
  return CheckExpectedTypeCompatibility(sem_context);
}

bool PTBcall::HaveColumnRef() const {
  const MCList<PTExpr::SharedPtr>& exprs = args_->node_list();
  vector<PTExpr::SharedPtr> params(exprs.size());
  for (const PTExpr::SharedPtr& expr : exprs) {
    if (expr->HaveColumnRef()) {
      return true;
    }
  }

  return false;
}

CHECKED_STATUS PTBcall::CheckOperator(SemContext *sem_context) {
  if (sem_context->processing_set_clause() && sem_context->lhs_col() != nullptr) {
    if (sem_context->lhs_col()->ql_type()->IsCollection()) {
      // Only increment ("+") and decrement ("-") operators are allowed for collections.
      const string type_name = QLType::ToCQLString(sem_context->lhs_col()->ql_type()->main());
      if (*name_ == "+" || *name_ == "-") {
        if (args_->element(0)->opcode() == TreeNodeOpcode::kPTRef) {
          name_->insert(0, type_name.c_str());
        } else {
          name_->append(type_name.c_str());
        }
      }
    } else if (sem_context->lhs_col()->is_counter()) {
      // Only increment ("+") and decrement ("-") operators are allowed for counters.
      if (strcmp(name_->c_str(), "+") == 0 || strcmp(name_->c_str(), "-") == 0) {
        name_->insert(0, "counter");
      } else if (strcmp(name_->c_str(), "counter+") != 0 &&
                 strcmp(name_->c_str(), "counter-") != 0) {
        return sem_context->Error(this, ErrorCode::INVALID_COUNTING_EXPR);
      }
    }
  }

  return Status::OK();
}

CHECKED_STATUS PTBcall::CheckCounterUpdateSupport(SemContext *sem_context) const {
  PTExpr::SharedPtr arg1 = args_->element(0);
  if (arg1->expr_op() != ExprOperator::kRef) {
    return sem_context->Error(arg1, "Right and left arguments must reference the same counter",
                              ErrorCode::INVALID_COUNTING_EXPR);
  }

  const PTRef *ref = static_cast<const PTRef*>(arg1.get());
  if (ref->desc() != sem_context->lhs_col()) {
    return sem_context->Error(arg1, "Right and left arguments must reference the same counter",
                              ErrorCode::INVALID_COUNTING_EXPR);
  }

  return Status::OK();
}

CHECKED_STATUS PTBcall::CheckOperatorAfterArgAnalyze(SemContext *sem_context) {
  if (*name_ == "tojson") {
    // The arguments must be analyzed and correct types must be set.
    const QLType::SharedPtr type = args_->element(0)->ql_type();
    DCHECK(!type->IsUnknown());

    if (type->main() == TUPLE) {
      // https://github.com/YugaByte/yugabyte-db/issues/936
      return sem_context->Error(args_->element(0),
          "Tuple type not implemented yet", ErrorCode::FEATURE_NOT_YET_IMPLEMENTED);
    }

    if (type->Contains(FROZEN) || type->Contains(USER_DEFINED_TYPE)) {
      // Only the server side implementation allows complex types unwrapping based on the schema.
      name_->insert(0, "server_");
    }
  }

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

// Collection constants.
CHECKED_STATUS PTToken::Analyze(SemContext *sem_context) {

  RETURN_NOT_OK(PTBcall::Analyze(sem_context));

  // Analyzing the arguments: their types need to be inferred based on table schema (hash columns).
  size_t size = sem_context->current_dml_stmt()->table()->schema().num_hash_key_columns();
  if (args().size() != size) {
    return sem_context->Error(
        this,
        Substitute("Invalid $0 call, wrong number of arguments", func_name()).c_str(),
        ErrorCode::CQL_STATEMENT_INVALID);
  }

  // Check if reference to partition key.
  if (args().front()->expr_op() == ExprOperator::kRef) {
    size_t index = 0;
    for (const PTExpr::SharedPtr &arg : args()) {
      if (arg->expr_op() != ExprOperator::kRef) {
        return sem_context->Error(arg,
            Substitute("Invalid $0 call, all arguments must be either column references or "
            "literals", func_name()).c_str(),
            ErrorCode::CQL_STATEMENT_INVALID);
      }

      PTRef *col_ref = static_cast<PTRef *>(arg.get());
      RETURN_NOT_OK(col_ref->Analyze(sem_context));
      if (col_ref->desc()->index() != index) {
        return sem_context->Error(
            col_ref,
            Substitute("Invalid $0 call, found reference to unexpected column",
                       func_name()).c_str(),
            ErrorCode::CQL_STATEMENT_INVALID);
      }
      index++;
    }
    is_partition_key_ref_ = true;
    return CheckExpectedTypeCompatibility(sem_context);
  }

  // Otherwise, it could be a call with literal values to be evaluated.
  size_t index = 0;
  auto& schema = sem_context->current_dml_stmt()->table()->schema();
  for (const PTExpr::SharedPtr &arg : args()) {
    if (arg->expr_op() != ExprOperator::kConst &&
        arg->expr_op() != ExprOperator::kUMinus &&
        arg->expr_op() != ExprOperator::kCollection &&
        arg->expr_op() != ExprOperator::kBindVar) {
      return sem_context->Error(arg,
          Substitute("Invalid $0 call, all arguments must be either column references or "
          "literals", func_name()).c_str(),
          ErrorCode::CQL_STATEMENT_INVALID);
    }
    SemState sem_state(sem_context);
    sem_state.SetExprState(schema.Column(index).type(),
                           YBColumnSchema::ToInternalDataType(schema.Column(index).type()));
    if (arg->expr_op() == ExprOperator::kBindVar) {
      sem_state.set_bindvar_name(PTBindVar::bcall_arg_bindvar_name(func_name(), index));
    }

    // All arguments are literals and are folded to the corresponding column type during analysis.
    // Therefore, we don't need an explicit cast to guarantee the correct types during execution.
    RETURN_NOT_OK(arg->Analyze(sem_context));
    index++;
  }
  is_partition_key_ref_ = false;
  return CheckExpectedTypeCompatibility(sem_context);
}

CHECKED_STATUS PTToken::CheckOperator(SemContext *sem_context) {
  // Nothing to do.
  return Status::OK();
}

}  // namespace ql
}  // namespace yb
