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
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_PGGATE_PG_STATEMENT_H_
#define YB_YQL_PGGATE_PG_STATEMENT_H_

#include <string>
#include <vector>

#include <boost/intrusive/list.hpp>

#include "yb/gutil/ref_counted.h"

#include "yb/yql/pggate/pg_session.h"
#include "yb/yql/pggate/pg_env.h"
#include "yb/yql/pggate/pg_expr.h"
#include "yb/yql/pggate/pg_memctx.h"

namespace yb {
namespace pggate {

// Statement types.
// - Might use it for error reporting or debugging or if different operations share the same
//   CAPI calls.
// - TODO(neil) Remove StmtOp if we don't need it.
enum class StmtOp {
  STMT_NOOP = 0,
  STMT_CREATE_DATABASE,
  STMT_DROP_DATABASE,
  STMT_CREATE_SCHEMA,
  STMT_DROP_SCHEMA,
  STMT_CREATE_TABLE,
  STMT_DROP_TABLE,
  STMT_TRUNCATE_TABLE,
  STMT_CREATE_INDEX,
  STMT_DROP_INDEX,
  STMT_ALTER_TABLE,
  STMT_INSERT,
  STMT_UPDATE,
  STMT_DELETE,
  STMT_TRUNCATE,
  STMT_SELECT,
  STMT_ALTER_DATABASE,
  STMT_CREATE_TABLEGROUP,
  STMT_DROP_TABLEGROUP,
};

class PgStatement : public PgMemctx::Registrable {
 public:
  //------------------------------------------------------------------------------------------------
  // Constructors.
  // pg_session is the session that this statement belongs to. If PostgreSQL cancels the session
  // while statement is running, pg_session::sharedptr can still be accessed without crashing.
  explicit PgStatement(PgSession::ScopedRefPtr pg_session);
  virtual ~PgStatement();

  const PgSession::ScopedRefPtr& pg_session() {
    return pg_session_;
  }

  // Statement type.
  virtual StmtOp stmt_op() const = 0;

  //------------------------------------------------------------------------------------------------
  static bool IsValidStmt(PgStatement* stmt, StmtOp op) {
    return (stmt != nullptr && stmt->stmt_op() == op);
  }

  //------------------------------------------------------------------------------------------------
  // Add expressions that are belong to this statement.
  void AddExpr(PgExpr::SharedPtr expr);

  //------------------------------------------------------------------------------------------------
  // Clear all values and expressions that were bound to the given statement.
  virtual CHECKED_STATUS ClearBinds() = 0;

 protected:
  // YBSession that this statement belongs to.
  PgSession::ScopedRefPtr pg_session_;

  // Execution status.
  Status status_;
  string errmsg_;

  // Expression list to be destroyed as soon as the statement is removed from the API.
  std::list<PgExpr::SharedPtr> exprs_;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_STATEMENT_H_
