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

#ifndef YB_YQL_PGGATE_PG_DDL_H_
#define YB_YQL_PGGATE_PG_DDL_H_

#include "yb/yql/pggate/pg_statement.h"
#include "yb/common/transaction.h"

namespace yb {
namespace pggate {

class PgDdl : public PgStatement {
 public:
  explicit PgDdl(PgSession::ScopedRefPtr pg_session) : PgStatement(pg_session) {
  }

  virtual CHECKED_STATUS ClearBinds() {
    return STATUS(InvalidArgument, "This statement cannot be bound to any values");
  }
};

//--------------------------------------------------------------------------------------------------
// CREATE DATABASE
//--------------------------------------------------------------------------------------------------

class PgCreateDatabase : public PgDdl {
 public:
  PgCreateDatabase(PgSession::ScopedRefPtr pg_session,
                   const char *database_name,
                   PgOid database_oid,
                   PgOid source_database_oid,
                   PgOid next_oid,
                   const bool colocated);
  virtual ~PgCreateDatabase();

  void AddTransaction(std::shared_future<Result<TransactionMetadata>> transaction) {
    txn_future_ = transaction;
  }

  StmtOp stmt_op() const override { return StmtOp::STMT_CREATE_DATABASE; }

  // Execute.
  CHECKED_STATUS Exec();

 private:
  const char *database_name_;
  const PgOid database_oid_;
  const PgOid source_database_oid_;
  const PgOid next_oid_;
  bool colocated_ = false;
  boost::optional<std::shared_future<Result<TransactionMetadata>>> txn_future_ = boost::none;
};

class PgDropDatabase : public PgDdl {
 public:
  PgDropDatabase(PgSession::ScopedRefPtr pg_session, const char *database_name, PgOid database_oid);
  virtual ~PgDropDatabase();

  StmtOp stmt_op() const override { return StmtOp::STMT_DROP_DATABASE; }

  // Execute.
  CHECKED_STATUS Exec();

 private:
  const char *database_name_;
  const PgOid database_oid_;
};

class PgAlterDatabase : public PgDdl {
 public:
  PgAlterDatabase(PgSession::ScopedRefPtr pg_session,
                  const char *database_name,
                  PgOid database_oid);
  virtual ~PgAlterDatabase();

  StmtOp stmt_op() const override { return StmtOp::STMT_ALTER_DATABASE; }

  CHECKED_STATUS RenameDatabase(const char *newname);

  // Execute.
  CHECKED_STATUS Exec();

 private:
  client::YBNamespaceAlterer* namespace_alterer_;
};

//--------------------------------------------------------------------------------------------------
// CREATE / DROP TABLEGROUP
//--------------------------------------------------------------------------------------------------

class PgCreateTablegroup : public PgDdl {
 public:
  PgCreateTablegroup(PgSession::ScopedRefPtr pg_session,
                     const char *database_name,
                     const PgOid database_oid,
                     const PgOid tablegroup_oid);
  virtual ~PgCreateTablegroup();

  StmtOp stmt_op() const override { return StmtOp::STMT_CREATE_TABLEGROUP; }

  // Execute.
  CHECKED_STATUS Exec();

 private:
  const char *database_name_;
  const PgOid database_oid_;
  const PgOid tablegroup_oid_;
};

class PgDropTablegroup : public PgDdl {
 public:
  PgDropTablegroup(PgSession::ScopedRefPtr pg_session,
                   PgOid database_oid,
                   PgOid tablegroup_oid);
  virtual ~PgDropTablegroup();

  StmtOp stmt_op() const override { return StmtOp::STMT_DROP_TABLEGROUP; }

  // Execute.
  CHECKED_STATUS Exec();

 private:
  const PgOid database_oid_;
  const PgOid tablegroup_oid_;
};

//--------------------------------------------------------------------------------------------------
// CREATE TABLE
//--------------------------------------------------------------------------------------------------

class PgCreateTable : public PgDdl {
 public:
  PgCreateTable(PgSession::ScopedRefPtr pg_session,
                const char *database_name,
                const char *schema_name,
                const char *table_name,
                const PgObjectId& table_id,
                bool is_shared_table,
                bool if_not_exist,
                bool add_primary_key,
                const bool colocated,
                const PgObjectId& tablegroup_oid);

  StmtOp stmt_op() const override { return StmtOp::STMT_CREATE_TABLE; }

  // For PgCreateIndex: the indexed (base) table id and if this is a unique index.
  virtual boost::optional<const PgObjectId&> indexed_table_id() const { return boost::none; }
  virtual bool is_unique_index() const { return false; }
  virtual const bool skip_index_backfill() const { return false; }

  CHECKED_STATUS AddColumn(const char *attr_name,
                           int attr_num,
                           int attr_ybtype,
                           bool is_hash,
                           bool is_range,
                           ColumnSchema::SortingType sorting_type =
                              ColumnSchema::SortingType::kNotSpecified) {
    return AddColumnImpl(attr_name, attr_num, attr_ybtype, is_hash, is_range, sorting_type);
  }

  CHECKED_STATUS AddColumn(const char *attr_name,
                           int attr_num,
                           const YBCPgTypeEntity *attr_type,
                           bool is_hash,
                           bool is_range,
                           ColumnSchema::SortingType sorting_type =
                               ColumnSchema::SortingType::kNotSpecified) {
    return AddColumnImpl(attr_name, attr_num, attr_type->yb_type, is_hash, is_range, sorting_type);
  }

  // Specify the number of tablets explicitly.
  CHECKED_STATUS SetNumTablets(int32_t num_tablets);

  CHECKED_STATUS AddSplitBoundary(PgExpr **exprs, int expr_count);

  void AddTransaction(std::shared_future<Result<TransactionMetadata>> txn) {
    txn_future_ = txn;
  }

  // Execute.
  virtual CHECKED_STATUS Exec();

 protected:
  virtual CHECKED_STATUS AddColumnImpl(const char *attr_name,
                                       int attr_num,
                                       int attr_ybtype,
                                       bool is_hash,
                                       bool is_range,
                                       ColumnSchema::SortingType sorting_type =
                                           ColumnSchema::SortingType::kNotSpecified);

  virtual size_t PrimaryKeyRangeColumnCount() const;

 private:
  Result<std::vector<std::string>> BuildSplitRows(const client::YBSchema& schema);

  client::YBTableName table_name_;
  const PgObjectId table_id_;
  int32_t num_tablets_;
  bool is_pg_catalog_table_;
  bool is_shared_table_;
  bool if_not_exist_;
  bool colocated_ = true;
  const PgObjectId tablegroup_oid_;
  boost::optional<YBHashSchema> hash_schema_;
  std::vector<std::string> range_columns_;
  std::vector<std::vector<QLValuePB>> split_rows_; // Split rows for range tables
  client::YBSchemaBuilder schema_builder_;
  boost::optional<std::shared_future<Result<TransactionMetadata>>> txn_future_ = boost::none;
};

class PgDropTable : public PgDdl {
 public:
  PgDropTable(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id, bool if_exist);
  virtual ~PgDropTable();

  StmtOp stmt_op() const override { return StmtOp::STMT_DROP_TABLE; }

  // Execute.
  CHECKED_STATUS Exec();

 protected:
  const PgObjectId table_id_;
  bool if_exist_;
};

class PgTruncateTable : public PgDdl {
 public:
  PgTruncateTable(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id);
  virtual ~PgTruncateTable();

  StmtOp stmt_op() const override { return StmtOp::STMT_TRUNCATE_TABLE; }

  // Execute.
  CHECKED_STATUS Exec();

 private:
  const PgObjectId table_id_;
};

//--------------------------------------------------------------------------------------------------
// CREATE INDEX
//--------------------------------------------------------------------------------------------------

class PgCreateIndex : public PgCreateTable {
 public:
  PgCreateIndex(PgSession::ScopedRefPtr pg_session,
                const char *database_name,
                const char *schema_name,
                const char *index_name,
                const PgObjectId& index_id,
                const PgObjectId& base_table_id,
                bool is_shared_index,
                bool is_unique_index,
                const bool skip_index_backfill,
                bool if_not_exist,
                const PgObjectId& tablegroup_oid);

  StmtOp stmt_op() const override { return StmtOp::STMT_CREATE_INDEX; }

  boost::optional<const PgObjectId&> indexed_table_id() const override {
    return base_table_id_;
  }

  bool is_unique_index() const override {
    return is_unique_index_;
  }

  const bool skip_index_backfill() const override {
    return skip_index_backfill_;
  }

  // Execute.
  CHECKED_STATUS Exec() override;

 protected:
  CHECKED_STATUS AddColumnImpl(const char *attr_name,
                               int attr_num,
                               int attr_ybtype,
                               bool is_hash,
                               bool is_range,
                               ColumnSchema::SortingType sorting_type) override;

 private:
  size_t PrimaryKeyRangeColumnCount() const override;

  CHECKED_STATUS AddYBbasectidColumn();

  const PgObjectId base_table_id_;
  bool is_unique_index_ = false;
  bool skip_index_backfill_ = false;
  bool ybbasectid_added_ = false;
  size_t primary_key_range_column_count_ = 0;
};

class PgDropIndex : public PgDropTable {
 public:
  PgDropIndex(PgSession::ScopedRefPtr pg_session, const PgObjectId& index_id, bool if_exist);
  virtual ~PgDropIndex();

  StmtOp stmt_op() const override { return StmtOp::STMT_DROP_INDEX; }

  // Execute.
  CHECKED_STATUS Exec();
};

//--------------------------------------------------------------------------------------------------
// ALTER TABLE
//--------------------------------------------------------------------------------------------------

class PgAlterTable : public PgDdl {
 public:
  PgAlterTable(PgSession::ScopedRefPtr pg_session,
               const PgObjectId& table_id);

  CHECKED_STATUS AddColumn(const char *name,
                           const YBCPgTypeEntity *attr_type,
                           int order);

  CHECKED_STATUS RenameColumn(const char *oldname, const char *newname);

  CHECKED_STATUS DropColumn(const char *name);

  CHECKED_STATUS RenameTable(const char *db_name, const char *newname);

  CHECKED_STATUS Exec();

  virtual ~PgAlterTable();

  StmtOp stmt_op() const override { return StmtOp::STMT_ALTER_TABLE; }

 private:
  const client::YBTableName table_name_;
  const PgObjectId table_id_;
  std::unique_ptr<client::YBTableAlterer> table_alterer;

};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_DDL_H_
