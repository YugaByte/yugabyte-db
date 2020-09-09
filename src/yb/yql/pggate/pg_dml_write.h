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

#ifndef YB_YQL_PGGATE_PG_DML_WRITE_H_
#define YB_YQL_PGGATE_PG_DML_WRITE_H_

#include "yb/yql/pggate/pg_dml.h"

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------
// DML WRITE - Insert, Update, Delete.
//--------------------------------------------------------------------------------------------------

class PgDmlWrite : public PgDml {
 public:
  // Abstract class without constructors.
  virtual ~PgDmlWrite();

  // Prepare write operations.
  virtual CHECKED_STATUS Prepare();

  // Setup internal structures for binding values during prepare.
  void PrepareColumns();

  // force_non_bufferable flag indicates this operation should not be buffered.
  CHECKED_STATUS Exec(bool force_non_bufferable = false);

  void SetIsSystemCatalogChange() {
    write_req_->set_is_ysql_catalog_change(true);
  }

  void SetCatalogCacheVersion(const uint64_t catalog_cache_version) override {
    write_req_->set_ysql_catalog_version(catalog_cache_version);
  }

  int32_t GetRowsAffectedCount() {
    return rows_affected_count_;
  }

  CHECKED_STATUS SetWriteTime(const HybridTime& write_time);

 protected:
  // Constructor.
  PgDmlWrite(PgSession::ScopedRefPtr pg_session,
             const PgObjectId& table_id,
             bool is_single_row_txn = false);

  // Allocate write request.
  void AllocWriteRequest();

  // Allocate column expression.
  PgsqlExpressionPB *AllocColumnBindPB(PgColumn *col) override;

  // Allocate target for selected or returned expressions.
  PgsqlExpressionPB *AllocTargetPB() override;

  // Allocate column expression.
  PgsqlExpressionPB *AllocColumnAssignPB(PgColumn *col) override;

  // Delete allocated target for columns that have no bind-values.
  CHECKED_STATUS DeleteEmptyPrimaryBinds();

  // Protobuf code.
  PgsqlWriteRequestPB *write_req_ = nullptr;

  bool is_single_row_txn_ = false; // default.

  int32_t rows_affected_count_ = 0;

 private:
  virtual std::unique_ptr<client::YBPgsqlWriteOp> AllocWriteOperation() const = 0;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_DML_WRITE_H_
