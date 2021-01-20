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
// List Node Declaration.
//
// This modules includes specifications for nodes that contain a list of tree nodes.
//
// YCQL audit expects these nodes to be DMLs encased between PTStartTransaction and PTCommit,
// which is verified during Analyze step.
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_CQL_QL_PTREE_LIST_NODE_H_
#define YB_YQL_CQL_QL_PTREE_LIST_NODE_H_

#include "yb/yql/cql/ql/ptree/tree_node.h"

namespace yb {
namespace ql {

// Operations that apply to each treenode of this list.
template<typename ContextType, typename NodeType = TreeNode>
using TreeNodePtrOperator = std::function<Status(NodeType*, ContextType*)>;

// TreeNode base class.
template<typename NodeType = TreeNode, TreeNodeOpcode op = TreeNodeOpcode::kPTListNode>
class TreeListNode : public TreeNode {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<TreeListNode> SharedPtr;
  typedef MCSharedPtr<const TreeListNode> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Public functions.
  explicit TreeListNode(MemoryContext *memory_context,
                        YBLocation::SharedPtr loc,
                        const MCSharedPtr<NodeType>& tnode = nullptr)
      : TreeNode(memory_context, loc),
        node_list_(memory_context) {
    Append(tnode);
  }
  virtual ~TreeListNode() {
  }

  // Node type.
  virtual TreeNodeOpcode opcode() const override {
    return op;
  }

  // Add a tree node at the end.
  void Append(const MCSharedPtr<NodeType>& tnode) {
    if (tnode != nullptr) {
      node_list_.push_back(tnode);
    }
  }

  // Move all listed entries from "tnode" to this node. The list in tnode will become empty after
  // this call.
  void Splice(const MCSharedPtr<TreeListNode>& tnode) {
    if (tnode != nullptr) {
      // tnode->node_list_ would be empty after this call to "splice".
      node_list_.splice(node_list_.end(), tnode->node_list_);
    }
  }

  // Add a tree node at the beginning.
  void Prepend(const MCSharedPtr<NodeType>& tnode) {
    if (tnode != nullptr) {
      node_list_.push_front(tnode);
    }
  }

  template<typename... TypeArgs>
  inline static TreeListNode::SharedPtr MakeShared(MemoryContext *memctx, TypeArgs&&... args) {
    return MCMakeShared<TreeListNode>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Run semantics analysis on this node.
  CHECKED_STATUS Analyze(SemContext *sem_context) override {
    for (auto tnode : node_list_) {
      RETURN_NOT_OK(tnode->Analyze(sem_context));
    }
    return Status::OK();
  }

  virtual CHECKED_STATUS Analyze(SemContext *sem_context,
                                 TreeNodePtrOperator<SemContext, NodeType> node_op) {
    for (auto tnode : node_list_) {
      RETURN_NOT_OK(node_op(tnode.get(), sem_context));
    }
    return Status::OK();
  }

  // Apply an operator on each node in the list.
  template<typename ContextType, typename DerivedType = NodeType>
  CHECKED_STATUS Apply(ContextType *context,
                       TreeNodePtrOperator<ContextType, DerivedType> node_op,
                       int max_nested_level = 0,
                       int max_nested_count = 0,
                       TreeNodePtrOperator<ContextType, DerivedType> nested_node_op = nullptr) {

    int nested_level = 0;
    int nested_count = 0;

    for (TreeNode::SharedPtr tnode : node_list_) {
      if (tnode->opcode() != TreeNodeOpcode::kPTListNode) {
        // Cast the node from (TreeNode*) to the given template type.
        DerivedType *node = static_cast<DerivedType*>(tnode.get());
        // Call the given node operation on the node.
        RETURN_NOT_OK(node_op(node, context));
      } else {
        if (++nested_count > max_nested_count) {
          return context->Error(this, "Number of nested lists exceeds allowable limit",
                                ErrorCode::SYNTAX_ERROR);
        }

        if (++nested_level > max_nested_level) {
          return context->Error(this, "Nested level of parenthesis exceeds allowable limit",
                                ErrorCode::SYNTAX_ERROR);
        }

        // Cast the node from (TreeNode*) to the given template type.
        TreeListNode *node = static_cast<TreeListNode*>(tnode.get());
        // Apply the operation to a nested list.
        RETURN_NOT_OK((node->Apply<ContextType, DerivedType>(context,
                                                             nested_node_op,
                                                             max_nested_level - 1,
                                                             max_nested_count,
                                                             nested_node_op)));
        nested_level--;
      }
    }

    return Status::OK();
  }

  // List count.
  int size() const {
    return node_list_.size();
  }

  // Access function to node_list_.
  const MCList<MCSharedPtr<NodeType>>& node_list() const {
    return node_list_;
  }

  MCList<MCSharedPtr<NodeType>>& node_list() {
    return node_list_;
  }

  // Returns the nth element.
  MCSharedPtr<NodeType> element(int n) const {
    DCHECK_GE(n, 0);
    for (const MCSharedPtr<NodeType>& tnode : node_list_) {
      if (n == 0) {
        return tnode;
      }
      n--;
    }
    return nullptr;
  }

 private:
  MCList<MCSharedPtr<NodeType>> node_list_;
};

class PTListNode : public TreeListNode<> {
 public:
  // Run semantics analysis on a statement block.
  CHECKED_STATUS AnalyzeStatementBlock(SemContext *sem_context);
};

}  // namespace ql
}  // namespace yb

#endif  // YB_YQL_CQL_QL_PTREE_LIST_NODE_H_
