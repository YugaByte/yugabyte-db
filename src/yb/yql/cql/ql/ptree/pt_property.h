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

#ifndef YB_YQL_CQL_QL_PTREE_PT_PROPERTY_H_
#define YB_YQL_CQL_QL_PTREE_PT_PROPERTY_H_

#include "yb/gutil/strings/substitute.h"
#include "yb/master/master.pb.h"
#include "yb/yql/cql/ql/ptree/list_node.h"
#include "yb/yql/cql/ql/ptree/pt_expr.h"
#include "yb/yql/cql/ql/ptree/tree_node.h"

namespace yb {
namespace ql {

#define RETURN_SEM_CONTEXT_ERROR_NOT_OK(s) do {                     \
    ::yb::Status _s = (s);                                          \
    if (PREDICT_FALSE(!_s.ok())) {                                  \
      auto err_str = s.ToUserMessage();                             \
      return sem_context->Error(this, err_str.c_str(),              \
                                ErrorCode::INVALID_TABLE_PROPERTY); \
    }                                                               \
  } while (0)

class PTProperty : public TreeNode {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTProperty> SharedPtr;
  typedef MCSharedPtr<const PTProperty> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructors and destructor.
  PTProperty(MemoryContext *memctx,
             YBLocation::SharedPtr loc,
             const MCSharedPtr<MCString>& lhs_,
             const PTExpr::SharedPtr& rhs_);

  PTProperty(MemoryContext *memctx,
             YBLocation::SharedPtr loc);

  virtual ~PTProperty();

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return TreeNodeOpcode::kPTProperty;
  }

  template<typename... TypeArgs>
  inline static PTProperty::SharedPtr MakeShared(MemoryContext *memctx,
                                                 TypeArgs&&... args) {
    return MCMakeShared<PTProperty>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override = 0;

  MCSharedPtr<MCString> lhs() const {
    return lhs_;
  }

  PTExpr::SharedPtr rhs() const {
    return rhs_;
  }

  static CHECKED_STATUS GetIntValueFromExpr(PTExpr::SharedPtr expr,
                                            const string& property_name,
                                            int64_t *val);

  static CHECKED_STATUS GetDoubleValueFromExpr(PTExpr::SharedPtr expr,
                                               const string& property_name,
                                               long double *val);

  static CHECKED_STATUS GetBoolValueFromExpr(PTExpr::SharedPtr expr,
                                             const string& property_name,
                                             bool *val);

  static CHECKED_STATUS GetStringValueFromExpr(PTExpr::SharedPtr expr,
                                               bool to_lower_case,
                                               const string& property_name,
                                               string *val);

 protected:
  // Parts of an expression 'lhs_ = rhs_' where lhs stands for left-hand side, and rhs for
  // right-hand side.
  MCSharedPtr<MCString> lhs_;
  PTExpr::SharedPtr rhs_;
};

class PTPropertyListNode : public TreeListNode<PTProperty> {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTPropertyListNode> SharedPtr;
  typedef MCSharedPtr<const PTPropertyListNode> SharedPtrConst;

  explicit PTPropertyListNode(MemoryContext *memory_context,
                              YBLocation::SharedPtr loc,
                              const MCSharedPtr<PTProperty>& tnode = nullptr)
      : TreeListNode<PTProperty>(memory_context, loc, tnode) {
  }

  virtual ~PTPropertyListNode() {
  }

  // Append a PTPropertyList to this list.
  void AppendList(const MCSharedPtr<PTPropertyListNode>& tnode_list) {
    if (tnode_list == nullptr) {
      return;
    }
    for (const auto& tnode : tnode_list->node_list()) {
      Append(tnode);
    }
  }

  template<typename... TypeArgs>
  inline static PTPropertyListNode::SharedPtr MakeShared(MemoryContext *memctx,
                                                         TypeArgs&&...args) {
    return MCMakeShared<PTPropertyListNode>(memctx, std::forward<TypeArgs>(args)...);
  }

  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;
};

} // namespace ql
} // namespace yb

#endif // YB_YQL_CQL_QL_PTREE_PT_PROPERTY_H_
