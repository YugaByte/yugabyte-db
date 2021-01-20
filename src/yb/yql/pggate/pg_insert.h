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

#ifndef YB_YQL_PGGATE_PG_INSERT_H_
#define YB_YQL_PGGATE_PG_INSERT_H_

#include "yb/yql/pggate/pg_dml_write.h"

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------
// INSERT
//--------------------------------------------------------------------------------------------------

class PgInsert : public PgDmlWrite {
 public:
  PgInsert(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id, bool is_single_row_txn)
      : PgDmlWrite(std::move(pg_session), table_id, is_single_row_txn) {}

  StmtOp stmt_op() const override { return StmtOp::STMT_INSERT; }

  void SetUpsertMode() {
    write_req_->set_stmt_type(PgsqlWriteRequestPB::PGSQL_UPSERT);
  }

  void SetIsBackfill(const bool is_backfill) {
    write_req_->set_is_backfill(is_backfill);
  }

 private:
  std::unique_ptr<client::YBPgsqlWriteOp> AllocWriteOperation() const override {
    return target_desc_->NewPgsqlInsert();
  }
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_INSERT_H_
