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
// This generic context is used for all processes on parse tree such as parsing, semantics analysis,
// and code generation.
//
// The execution step operates on a read-only (const) parse tree and does not hold a unique_ptr to
// it. Accordingly, the execution context subclasses from PgProcessContext which does not have
// the parse tree.
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_PGSQL_PTREE_PG_PROCESS_CONTEXT_H_
#define YB_YQL_PGSQL_PTREE_PG_PROCESS_CONTEXT_H_

#include "yb/yql/pgsql/ptree/parse_tree.h"
#include "yb/yql/pgsql/ptree/pg_tlocation.h"
#include "yb/yql/pgsql/util/pg_errcodes.h"
#include "yb/util/memory/mc_types.h"

namespace yb {
namespace pgsql {

//--------------------------------------------------------------------------------------------------

class PgProcessContext {
 public:
  //------------------------------------------------------------------------------------------------
  // Constructor & destructor.
  PgProcessContext(const char *stmt, size_t stmt_len);
  virtual ~PgProcessContext();

  // Handling parsing warning.
  void Warn(const PgTLocation& l, const std::string& m, ErrorCode error_code);

  // Handling parsing error.
  CHECKED_STATUS Error(const PgTLocation& l,
                       const char *m,
                       ErrorCode error_code,
                       const char* token = nullptr);
  CHECKED_STATUS Error(const PgTLocation& l, const char *m, const char* token = nullptr);
  CHECKED_STATUS Error(const PgTLocation& l, ErrorCode error_code, const char* token = nullptr);

  // Variants of Error() that report location of tnode as the error location.
  CHECKED_STATUS Error(const TreeNode *tnode, ErrorCode error_code);
  CHECKED_STATUS Error(const TreeNode *tnode, const char *m, ErrorCode error_code);
  CHECKED_STATUS Error(const TreeNode *tnode, const Status& s, ErrorCode error_code);

  CHECKED_STATUS Error(const TreeNode::SharedPtr& tnode, ErrorCode error_code);
  CHECKED_STATUS Error(const TreeNode::SharedPtr& tnode, const char *m, ErrorCode error_code);
  CHECKED_STATUS Error(const TreeNode::SharedPtr& tnode, const Status& s, ErrorCode error_code);

  // Memory pool for allocating and deallocating operating memory spaces during a process.
  MemoryContext *PTempMem() const {
    if (ptemp_mem_ == nullptr) {
      ptemp_mem_.reset(new Arena());
    }
    return ptemp_mem_.get();
  }

  // Access function for stmt_.
  const char *stmt() const {
    return stmt_;
  }

  // Access function for stmt_len_.
  size_t stmt_len() const {
    return stmt_len_;
  }

  // Access function for error_code_.
  ErrorCode error_code() const {
    return error_code_;
  }

  // Return status of a process.
  CHECKED_STATUS GetStatus();

 protected:
  MCString* error_msgs();

  //------------------------------------------------------------------------------------------------
  // SQL statement to be scanned.
  const char *stmt_;

  // SQL statement length.
  const size_t stmt_len_;

  // Temporary memory pool is used during a process. This pool is deleted as soon as the process is
  // completed.
  //
  // For performance, the temp arena and the error message that depends on it are created only when
  // needed.
  mutable std::unique_ptr<Arena> ptemp_mem_;

  // Latest parsing or scanning error code.
  ErrorCode error_code_;

  // Error messages. All reported error messages will be concatenated to the end.
  std::unique_ptr<MCString> error_msgs_;
};

}  // namespace pgsql
}  // namespace yb

#endif  // YB_YQL_PGSQL_PTREE_PG_PROCESS_CONTEXT_H_
