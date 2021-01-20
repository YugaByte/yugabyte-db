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

#include <algorithm>
#include <string>

#include <cds/init.h> // NOLINT

#include "yb/common/common_flags.h"
#include "yb/common/ybc-internal.h"
#include "yb/util/atomic.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "yb/yql/pggate/pggate.h"
#include "yb/yql/pggate/pggate_thread_local_vars.h"
#include "yb/yql/pggate/pg_txn_manager.h"
#include "yb/yql/pggate/pggate_flags.h"

DECLARE_bool(client_suppress_created_logs);

DEFINE_int32(pggate_num_connections_to_server, 1,
             "Number of underlying connections to each server from a PostgreSQL backend process. "
             "This overrides the value of --num_connections_to_server.");

DECLARE_int32(num_connections_to_server);

DECLARE_int32(delay_alter_sequence_sec);

DECLARE_int32(client_read_write_timeout_ms);

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------
// C++ Implementation.
// All C++ objects and structures in this module are listed in the following namespace.
//--------------------------------------------------------------------------------------------------
namespace {

// Using a raw pointer here to fully control object initialization and destruction.
pggate::PgApiImpl* pgapi;
std::atomic<bool> pgapi_shutdown_done;

template<class T>
YBCStatus ExtractValueFromResult(const Result<T>& result, T* value) {
  if (result.ok()) {
    *value = *result;
    return YBCStatusOK();
  }
  return ToYBCStatus(result.status());
}

YBCStatus ProcessYbctid(
    const YBCPgYBTupleIdDescriptor& source,
    const std::function<Status(PgOid, const Slice&)>& processor) {
  return ToYBCStatus(pgapi->ProcessYBTupleId(
      source,
      std::bind(processor, source.table_oid, std::placeholders::_1)));
}

} // anonymous namespace

//--------------------------------------------------------------------------------------------------
// C API.
//--------------------------------------------------------------------------------------------------
extern "C" {

void YBCInitPgGate(const YBCPgTypeEntity *YBCDataTypeTable, int count, PgCallbacks pg_callbacks) {
  InitThreading();

  CHECK(pgapi == nullptr) << ": " << __PRETTY_FUNCTION__ << " can only be called once";

  YBCInitFlags();

  pgapi_shutdown_done.exchange(false);
  pgapi = new pggate::PgApiImpl(YBCDataTypeTable, count, pg_callbacks);
  VLOG(1) << "PgGate open";
}

void YBCDestroyPgGate() {
  if (pgapi_shutdown_done.exchange(true)) {
    LOG(DFATAL) << __PRETTY_FUNCTION__ << " should only be called once";
  } else {
    pggate::PgApiImpl* local_pgapi = pgapi;
    pgapi = nullptr; // YBCPgIsYugaByteEnabled() must return false from now on.
    delete local_pgapi;
    VLOG(1) << __PRETTY_FUNCTION__ << " finished";
  }
}

YBCStatus YBCPgCreateEnv(YBCPgEnv *pg_env) {
  return ToYBCStatus(pgapi->CreateEnv(pg_env));
}

YBCStatus YBCPgDestroyEnv(YBCPgEnv pg_env) {
  return ToYBCStatus(pgapi->DestroyEnv(pg_env));
}

YBCStatus YBCPgInitSession(const YBCPgEnv pg_env,
                           const char *database_name) {
  const string db_name(database_name ? database_name : "");
  return ToYBCStatus(pgapi->InitSession(pg_env, db_name));
}

YBCPgMemctx YBCPgCreateMemctx() {
  return pgapi->CreateMemctx();
}

YBCStatus YBCPgDestroyMemctx(YBCPgMemctx memctx) {
  return ToYBCStatus(pgapi->DestroyMemctx(memctx));
}

YBCStatus YBCPgResetMemctx(YBCPgMemctx memctx) {
  return ToYBCStatus(pgapi->ResetMemctx(memctx));
}

void YBCPgDeleteStatement(YBCPgStatement handle) {
  pgapi->DeleteStatement(handle);
}

YBCStatus YBCPgInvalidateCache() {
  return ToYBCStatus(pgapi->InvalidateCache());
}

const YBCPgTypeEntity *YBCPgFindTypeEntity(int type_oid) {
  return pgapi->FindTypeEntity(type_oid);
}

YBCPgDataType YBCPgGetType(const YBCPgTypeEntity *type_entity) {
  if (type_entity) {
    return type_entity->yb_type;
  }
  return YB_YQL_DATA_TYPE_UNKNOWN_DATA;
}

bool YBCPgAllowForPrimaryKey(const YBCPgTypeEntity *type_entity) {
  if (type_entity) {
    return type_entity->allow_for_primary_key;
  }
  return false;
}

//--------------------------------------------------------------------------------------------------
// DDL Statements.
//--------------------------------------------------------------------------------------------------
// Database Operations -----------------------------------------------------------------------------

YBCStatus YBCPgConnectDatabase(const char *database_name) {
  return ToYBCStatus(pgapi->ConnectDatabase(database_name));
}

YBCStatus YBCPgIsDatabaseColocated(const YBCPgOid database_oid, bool *colocated) {
  return ToYBCStatus(pgapi->IsDatabaseColocated(database_oid, colocated));
}

YBCStatus YBCPgNewCreateDatabase(const char *database_name,
                                 const YBCPgOid database_oid,
                                 const YBCPgOid source_database_oid,
                                 const YBCPgOid next_oid,
                                 const bool colocated,
                                 YBCPgStatement *handle) {
  return ToYBCStatus(pgapi->NewCreateDatabase(
      database_name, database_oid, source_database_oid, next_oid, colocated, handle));
}

YBCStatus YBCPgExecCreateDatabase(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecCreateDatabase(handle));
}

YBCStatus YBCPgNewDropDatabase(const char *database_name,
                               const YBCPgOid database_oid,
                               YBCPgStatement *handle) {
  return ToYBCStatus(pgapi->NewDropDatabase(database_name, database_oid, handle));
}

YBCStatus YBCPgExecDropDatabase(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecDropDatabase(handle));
}

YBCStatus YBCPgNewAlterDatabase(const char *database_name,
                               const YBCPgOid database_oid,
                               YBCPgStatement *handle) {
  return ToYBCStatus(pgapi->NewAlterDatabase(database_name, database_oid, handle));
}

YBCStatus YBCPgAlterDatabaseRenameDatabase(YBCPgStatement handle, const char *newname) {
  return ToYBCStatus(pgapi->AlterDatabaseRenameDatabase(handle, newname));
}

YBCStatus YBCPgExecAlterDatabase(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecAlterDatabase(handle));
}

YBCStatus YBCPgReserveOids(const YBCPgOid database_oid,
                           const YBCPgOid next_oid,
                           const uint32_t count,
                           YBCPgOid *begin_oid,
                           YBCPgOid *end_oid) {
  return ToYBCStatus(pgapi->ReserveOids(database_oid, next_oid, count,
                                        begin_oid, end_oid));
}

YBCStatus YBCPgGetCatalogMasterVersion(uint64_t *version) {
  return ToYBCStatus(pgapi->GetCatalogMasterVersion(version));
}

void YBCPgInvalidateTableCache(
    const YBCPgOid database_oid,
    const YBCPgOid table_oid) {
  const PgObjectId table_id(database_oid, table_oid);
  pgapi->InvalidateTableCache(table_id);
}

YBCStatus YBCPgInvalidateTableCacheByTableId(const char *table_id) {
  if (table_id == NULL) {
    return ToYBCStatus(STATUS(InvalidArgument, "table_id is null"));
  }
  std::string table_id_str = table_id;
  const PgObjectId pg_object_id(table_id_str);
  pgapi->InvalidateTableCache(pg_object_id);
  return YBCStatusOK();
}

// Tablegroup Operations ---------------------------------------------------------------------------

YBCStatus YBCPgNewCreateTablegroup(const char *database_name,
                                   YBCPgOid database_oid,
                                   YBCPgOid tablegroup_oid,
                                   YBCPgStatement *handle) {
  return ToYBCStatus(pgapi->NewCreateTablegroup(database_name,
                                                database_oid,
                                                tablegroup_oid,
                                                handle));
}

YBCStatus YBCPgExecCreateTablegroup(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecCreateTablegroup(handle));
}

YBCStatus YBCPgNewDropTablegroup(YBCPgOid database_oid,
                                 YBCPgOid tablegroup_oid,
                                 YBCPgStatement *handle) {
  return ToYBCStatus(pgapi->NewDropTablegroup(database_oid,
                                              tablegroup_oid,
                                              handle));
}
YBCStatus YBCPgExecDropTablegroup(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecDropTablegroup(handle));
}

// Statement Operations ----------------------------------------------------------------------------

YBCStatus YBCPgClearBinds(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ClearBinds(handle));
}

// Sequence Operations -----------------------------------------------------------------------------

YBCStatus YBCInsertSequenceTuple(int64_t db_oid,
                                 int64_t seq_oid,
                                 uint64_t ysql_catalog_version,
                                 int64_t last_val,
                                 bool is_called) {
  return ToYBCStatus(pgapi->InsertSequenceTuple(
      db_oid, seq_oid, ysql_catalog_version, last_val, is_called));
}

YBCStatus YBCUpdateSequenceTupleConditionally(int64_t db_oid,
                                              int64_t seq_oid,
                                              uint64_t ysql_catalog_version,
                                              int64_t last_val,
                                              bool is_called,
                                              int64_t expected_last_val,
                                              bool expected_is_called,
                                              bool *skipped) {
  return ToYBCStatus(
      pgapi->UpdateSequenceTupleConditionally(db_oid, seq_oid, ysql_catalog_version,
          last_val, is_called, expected_last_val, expected_is_called, skipped));
}

YBCStatus YBCUpdateSequenceTuple(int64_t db_oid,
                                 int64_t seq_oid,
                                 uint64_t ysql_catalog_version,
                                 int64_t last_val,
                                 bool is_called,
                                 bool* skipped) {
  return ToYBCStatus(pgapi->UpdateSequenceTuple(
      db_oid, seq_oid, ysql_catalog_version, last_val, is_called, skipped));
}

YBCStatus YBCReadSequenceTuple(int64_t db_oid,
                               int64_t seq_oid,
                               uint64_t ysql_catalog_version,
                               int64_t *last_val,
                               bool *is_called) {
  return ToYBCStatus(pgapi->ReadSequenceTuple(
      db_oid, seq_oid, ysql_catalog_version, last_val, is_called));
}

YBCStatus YBCDeleteSequenceTuple(int64_t db_oid, int64_t seq_oid) {
  return ToYBCStatus(pgapi->DeleteSequenceTuple(db_oid, seq_oid));
}

// Table Operations -------------------------------------------------------------------------------

YBCStatus YBCPgNewCreateTable(const char *database_name,
                              const char *schema_name,
                              const char *table_name,
                              const YBCPgOid database_oid,
                              const YBCPgOid table_oid,
                              bool is_shared_table,
                              bool if_not_exist,
                              bool add_primary_key,
                              const bool colocated,
                              const YBCPgOid tablegroup_oid,
                              YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  const PgObjectId tablegroup_id(database_oid, tablegroup_oid);
  return ToYBCStatus(pgapi->NewCreateTable(
      database_name, schema_name, table_name, table_id, is_shared_table,
      if_not_exist, add_primary_key, colocated, tablegroup_id, handle));
}

YBCStatus YBCPgCreateTableAddColumn(YBCPgStatement handle, const char *attr_name, int attr_num,
                                    const YBCPgTypeEntity *attr_type, bool is_hash, bool is_range,
                                    bool is_desc, bool is_nulls_first) {
  return ToYBCStatus(pgapi->CreateTableAddColumn(handle, attr_name, attr_num, attr_type,
                                                 is_hash, is_range, is_desc, is_nulls_first));
}

YBCStatus YBCPgCreateTableSetNumTablets(YBCPgStatement handle, int32_t num_tablets) {
  return ToYBCStatus(pgapi->CreateTableSetNumTablets(handle, num_tablets));
}

YBCStatus YBCPgAddSplitBoundary(YBCPgStatement handle, YBCPgExpr *exprs, int expr_count) {
  return ToYBCStatus(pgapi->AddSplitBoundary(handle, exprs, expr_count));
}

YBCStatus YBCPgExecCreateTable(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecCreateTable(handle));
}

YBCStatus YBCPgNewAlterTable(const YBCPgOid database_oid,
                             const YBCPgOid table_oid,
                             YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewAlterTable(table_id, handle));
}

YBCStatus YBCPgAlterTableAddColumn(YBCPgStatement handle, const char *name, int order,
                                   const YBCPgTypeEntity *attr_type) {
  return ToYBCStatus(pgapi->AlterTableAddColumn(handle, name, order, attr_type));
}

YBCStatus YBCPgAlterTableRenameColumn(YBCPgStatement handle, const char *oldname,
                                      const char *newname) {
  return ToYBCStatus(pgapi->AlterTableRenameColumn(handle, oldname, newname));
}

YBCStatus YBCPgAlterTableDropColumn(YBCPgStatement handle, const char *name) {
  return ToYBCStatus(pgapi->AlterTableDropColumn(handle, name));
}

YBCStatus YBCPgAlterTableRenameTable(YBCPgStatement handle, const char *db_name,
                                     const char *newname) {
  return ToYBCStatus(pgapi->AlterTableRenameTable(handle, db_name, newname));
}

YBCStatus YBCPgExecAlterTable(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecAlterTable(handle));
}

YBCStatus YBCPgNewDropTable(const YBCPgOid database_oid,
                            const YBCPgOid table_oid,
                            bool if_exist,
                            YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewDropTable(table_id, if_exist, handle));
}

YBCStatus YBCPgGetTableDesc(const YBCPgOid database_oid,
                            const YBCPgOid table_oid,
                            YBCPgTableDesc *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->GetTableDesc(table_id, handle));
}

YBCStatus YBCPgGetColumnInfo(YBCPgTableDesc table_desc,
                             int16_t attr_number,
                             bool *is_primary,
                             bool *is_hash) {
  return ToYBCStatus(pgapi->GetColumnInfo(table_desc, attr_number, is_primary, is_hash));
}

YBCStatus YBCPgSetCatalogCacheVersion(YBCPgStatement handle,
                                      uint64_t catalog_cache_version) {
  return ToYBCStatus(pgapi->SetCatalogCacheVersion(handle, catalog_cache_version));
}

YBCStatus YBCPgDmlModifiesRow(YBCPgStatement handle, bool *modifies_row) {
  return ToYBCStatus(pgapi->DmlModifiesRow(handle, modifies_row));
}

YBCStatus YBCPgSetIsSysCatalogVersionChange(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->SetIsSysCatalogVersionChange(handle));
}

YBCStatus YBCPgNewTruncateTable(const YBCPgOid database_oid,
                                const YBCPgOid table_oid,
                                YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewTruncateTable(table_id, handle));
}

YBCStatus YBCPgExecTruncateTable(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecTruncateTable(handle));
}

YBCStatus YBCPgIsTableColocated(const YBCPgOid database_oid,
                                const YBCPgOid table_oid,
                                bool *colocated) {
  const PgObjectId table_id(database_oid, table_oid);
  PgTableDesc::ScopedRefPtr table_desc;
  YBCStatus status = ExtractValueFromResult(pgapi->LoadTable(table_id), &table_desc);
  if (status) {
    return status;
  } else {
    *colocated = table_desc->IsColocated();
    return YBCStatusOK();
  }
}

YBCStatus YBCPgGetTableProperties(YBCPgTableDesc table_desc,
                                  YBCPgTableProperties *properties) {
  CHECK_NOTNULL(properties)->num_tablets = table_desc->GetPartitionCount();
  properties->num_hash_key_columns = table_desc->num_hash_key_columns();
  properties->is_colocated = table_desc->IsColocated();
  return YBCStatusOK();
}

YBCStatus YBCPgTableExists(const YBCPgOid database_oid,
                           const YBCPgOid table_oid,
                           bool *exists) {
  const PgObjectId table_id(database_oid, table_oid);
  const auto result = pgapi->LoadTable(table_id);

  if (result.ok()) {
    *exists = true;
    return YBCStatusOK();
  } else if (result.status().IsNotFound()) {
    *exists = false;
    return YBCStatusOK();
  } else {
    return ToYBCStatus(result.status());
  }
}

// Index Operations -------------------------------------------------------------------------------

YBCStatus YBCPgNewCreateIndex(const char *database_name,
                              const char *schema_name,
                              const char *index_name,
                              const YBCPgOid database_oid,
                              const YBCPgOid index_oid,
                              const YBCPgOid table_oid,
                              bool is_shared_index,
                              bool is_unique_index,
                              const bool skip_index_backfill,
                              bool if_not_exist,
                              const YBCPgOid tablegroup_oid,
                              YBCPgStatement *handle) {
  const PgObjectId index_id(database_oid, index_oid);
  const PgObjectId table_id(database_oid, table_oid);
  const PgObjectId tablegroup_id(database_oid, tablegroup_oid);
  return ToYBCStatus(pgapi->NewCreateIndex(database_name, schema_name, index_name, index_id,
                                           table_id, is_shared_index, is_unique_index,
                                           skip_index_backfill, if_not_exist, tablegroup_id,
                                           handle));
}

YBCStatus YBCPgCreateIndexAddColumn(YBCPgStatement handle, const char *attr_name, int attr_num,
                                    const YBCPgTypeEntity *attr_type, bool is_hash, bool is_range,
                                    bool is_desc, bool is_nulls_first) {
  return ToYBCStatus(pgapi->CreateIndexAddColumn(handle, attr_name, attr_num, attr_type,
                                                 is_hash, is_range, is_desc, is_nulls_first));
}

YBCStatus YBCPgCreateIndexSetNumTablets(YBCPgStatement handle, int32_t num_tablets) {
  return ToYBCStatus(pgapi->CreateIndexSetNumTablets(handle, num_tablets));
}

YBCStatus YBCPgExecCreateIndex(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecCreateIndex(handle));
}

YBCStatus YBCPgNewDropIndex(const YBCPgOid database_oid,
                            const YBCPgOid index_oid,
                            bool if_exist,
                            YBCPgStatement *handle) {
  const PgObjectId index_id(database_oid, index_oid);
  return ToYBCStatus(pgapi->NewDropIndex(index_id, if_exist, handle));
}

YBCStatus YBCPgWaitUntilIndexPermissionsAtLeast(
    const YBCPgOid database_oid,
    const YBCPgOid table_oid,
    const YBCPgOid index_oid,
    const uint32_t target_index_permissions,
    uint32_t *actual_index_permissions) {
  const PgObjectId table_id(database_oid, table_oid);
  const PgObjectId index_id(database_oid, index_oid);
  IndexPermissions returned_index_permissions = IndexPermissions::INDEX_PERM_DELETE_ONLY;
  YBCStatus s = ExtractValueFromResult(pgapi->WaitUntilIndexPermissionsAtLeast(
        table_id,
        index_id,
        static_cast<IndexPermissions>(target_index_permissions)),
      &returned_index_permissions);
  if (s) {
    // Bad status.
    return s;
  }
  *actual_index_permissions = static_cast<uint32_t>(returned_index_permissions);
  return YBCStatusOK();
}

YBCStatus YBCPgAsyncUpdateIndexPermissions(
    const YBCPgOid database_oid,
    const YBCPgOid indexed_table_oid) {
  const PgObjectId indexed_table_id(database_oid, indexed_table_oid);
  return ToYBCStatus(pgapi->AsyncUpdateIndexPermissions(indexed_table_id));
}

YBCStatus YBCPgExecPostponedDdlStmt(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecPostponedDdlStmt(handle));
}

YBCStatus YBCPgBackfillIndex(
    const YBCPgOid database_oid,
    const YBCPgOid index_oid) {
  const PgObjectId index_id(database_oid, index_oid);
  return ToYBCStatus(pgapi->BackfillIndex(index_id));
}

//--------------------------------------------------------------------------------------------------
// DML Statements.
//--------------------------------------------------------------------------------------------------

YBCStatus YBCPgDmlAppendTarget(YBCPgStatement handle, YBCPgExpr target) {
  return ToYBCStatus(pgapi->DmlAppendTarget(handle, target));
}

YBCStatus YBCPgDmlBindColumn(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value) {
  return ToYBCStatus(pgapi->DmlBindColumn(handle, attr_num, attr_value));
}

YBCStatus YBCPgDmlBindColumnCondEq(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value) {
  return ToYBCStatus(pgapi->DmlBindColumnCondEq(handle, attr_num, attr_value));
}

YBCStatus YBCPgDmlBindColumnCondBetween(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value,
    YBCPgExpr attr_value_end) {
  return ToYBCStatus(pgapi->DmlBindColumnCondBetween(handle, attr_num, attr_value, attr_value_end));
}

YBCStatus YBCPgDmlBindColumnCondIn(YBCPgStatement handle, int attr_num, int n_attr_values,
    YBCPgExpr *attr_values) {
  return ToYBCStatus(pgapi->DmlBindColumnCondIn(handle, attr_num, n_attr_values, attr_values));
}

YBCStatus YBCPgDmlBindTable(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->DmlBindTable(handle));
}

YBCStatus YBCPgDmlAssignColumn(YBCPgStatement handle,
                               int attr_num,
                               YBCPgExpr attr_value) {
  return ToYBCStatus(pgapi->DmlAssignColumn(handle, attr_num, attr_value));
}

YBCStatus YBCPgDmlFetch(YBCPgStatement handle, int32_t natts, uint64_t *values, bool *isnulls,
                        YBCPgSysColumns *syscols, bool *has_data) {
  return ToYBCStatus(pgapi->DmlFetch(handle, natts, values, isnulls, syscols, has_data));
}

void YBCPgStartOperationsBuffering() {
  pgapi->StartOperationsBuffering();
}

YBCStatus YBCPgStopOperationsBuffering() {
  return ToYBCStatus(pgapi->StopOperationsBuffering());
}

YBCStatus YBCPgResetOperationsBuffering() {
  return ToYBCStatus(pgapi->ResetOperationsBuffering());
}

YBCStatus YBCPgFlushBufferedOperations() {
  return ToYBCStatus(pgapi->FlushBufferedOperations());
}

void YBCPgDropBufferedOperations() {
  pgapi->DropBufferedOperations();
}

YBCStatus YBCPgDmlExecWriteOp(YBCPgStatement handle, int32_t *rows_affected_count) {
  return ToYBCStatus(pgapi->DmlExecWriteOp(handle, rows_affected_count));
}

YBCStatus YBCPgBuildYBTupleId(const YBCPgYBTupleIdDescriptor *source, uint64_t *ybctid) {
  return ProcessYbctid(*source, [ybctid](const auto&, const auto& yid) {
    const auto* type_entity = pgapi->FindTypeEntity(kPgByteArrayOid);
    *ybctid = type_entity->yb_to_datum(yid.cdata(), yid.size(), nullptr /* type_attrs */);
    return Status::OK();
  });
}

// INSERT Operations -------------------------------------------------------------------------------
YBCStatus YBCPgNewInsert(const YBCPgOid database_oid,
                         const YBCPgOid table_oid,
                         const bool is_single_row_txn,
                         YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewInsert(table_id, is_single_row_txn, handle));
}

YBCStatus YBCPgExecInsert(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecInsert(handle));
}

YBCStatus YBCPgInsertStmtSetUpsertMode(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->InsertStmtSetUpsertMode(handle));
}

YBCStatus YBCPgInsertStmtSetWriteTime(YBCPgStatement handle, const uint64_t write_time) {
  HybridTime write_hybrid_time;
  YBCStatus status = ToYBCStatus(write_hybrid_time.FromUint64(write_time));
  if (status) {
    return status;
  } else {
    return ToYBCStatus(pgapi->InsertStmtSetWriteTime(handle, write_hybrid_time));
  }
}

YBCStatus YBCPgInsertStmtSetIsBackfill(YBCPgStatement handle, const bool is_backfill) {
  return ToYBCStatus(pgapi->InsertStmtSetIsBackfill(handle, is_backfill));
}

// UPDATE Operations -------------------------------------------------------------------------------
YBCStatus YBCPgNewUpdate(const YBCPgOid database_oid,
                         const YBCPgOid table_oid,
                         bool is_single_row_txn,
                         YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewUpdate(table_id, is_single_row_txn, handle));
}

YBCStatus YBCPgExecUpdate(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecUpdate(handle));
}

bool YBCGetEnableUpdateBatching() {
  return FLAGS_ysql_enable_update_batching;
}

// DELETE Operations -------------------------------------------------------------------------------
YBCStatus YBCPgNewDelete(const YBCPgOid database_oid,
                         const YBCPgOid table_oid,
                         bool is_single_row_txn,
                         YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewDelete(table_id, is_single_row_txn, handle));
}

YBCStatus YBCPgExecDelete(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecDelete(handle));
}

// Colocated TRUNCATE Operations -------------------------------------------------------------------
YBCStatus YBCPgNewTruncateColocated(const YBCPgOid database_oid,
                                    const YBCPgOid table_oid,
                                    bool is_single_row_txn,
                                    YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  return ToYBCStatus(pgapi->NewTruncateColocated(table_id, is_single_row_txn, handle));
}

YBCStatus YBCPgExecTruncateColocated(YBCPgStatement handle) {
  return ToYBCStatus(pgapi->ExecTruncateColocated(handle));
}

// SELECT Operations -------------------------------------------------------------------------------
YBCStatus YBCPgNewSelect(const YBCPgOid database_oid,
                         const YBCPgOid table_oid,
                         const YBCPgPrepareParameters *prepare_params,
                         YBCPgStatement *handle) {
  const PgObjectId table_id(database_oid, table_oid);
  const PgObjectId index_id(database_oid,
                            prepare_params ? prepare_params->index_oid : kInvalidOid);
  return ToYBCStatus(pgapi->NewSelect(table_id, index_id, prepare_params, handle));
}

YBCStatus YBCPgSetForwardScan(YBCPgStatement handle, bool is_forward_scan) {
  return ToYBCStatus(pgapi->SetForwardScan(handle, is_forward_scan));
}

YBCStatus YBCPgExecSelect(YBCPgStatement handle, const YBCPgExecParameters *exec_params) {
  return ToYBCStatus(pgapi->ExecSelect(handle, exec_params));
}

//--------------------------------------------------------------------------------------------------
// Expression Operations
//--------------------------------------------------------------------------------------------------

YBCStatus YBCPgNewColumnRef(YBCPgStatement stmt, int attr_num, const YBCPgTypeEntity *type_entity,
                            const YBCPgTypeAttrs *type_attrs, YBCPgExpr *expr_handle) {
  return ToYBCStatus(pgapi->NewColumnRef(stmt, attr_num, type_entity, type_attrs, expr_handle));
}

YBCStatus YBCPgNewConstant(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                           uint64_t datum, bool is_null, YBCPgExpr *expr_handle) {
  return ToYBCStatus(pgapi->NewConstant(stmt, type_entity, datum, is_null, expr_handle));
}

YBCStatus YBCPgNewConstantVirtual(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                                  YBCPgDatumKind datum_kind, YBCPgExpr *expr_handle) {
  return ToYBCStatus(pgapi->NewConstantVirtual(stmt, type_entity, datum_kind, expr_handle));
}

YBCStatus YBCPgNewConstantOp(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                           uint64_t datum, bool is_null, YBCPgExpr *expr_handle, bool is_gt) {
  return ToYBCStatus(pgapi->NewConstantOp(stmt, type_entity, datum, is_null, expr_handle, is_gt));
}

// Overwriting the expression's result with any desired values.
YBCStatus YBCPgUpdateConstInt2(YBCPgExpr expr, int16_t value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstInt4(YBCPgExpr expr, int32_t value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstInt8(YBCPgExpr expr, int64_t value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstFloat4(YBCPgExpr expr, float value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstFloat8(YBCPgExpr expr, double value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstText(YBCPgExpr expr, const char *value, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, is_null));
}

YBCStatus YBCPgUpdateConstChar(YBCPgExpr expr, const char *value,  int64_t bytes, bool is_null) {
  return ToYBCStatus(pgapi->UpdateConstant(expr, value, bytes, is_null));
}

YBCStatus YBCPgNewOperator(YBCPgStatement stmt, const char *opname,
                           const YBCPgTypeEntity *type_entity,
                           YBCPgExpr *op_handle) {
  return ToYBCStatus(pgapi->NewOperator(stmt, opname, type_entity, op_handle));
}

YBCStatus YBCPgOperatorAppendArg(YBCPgExpr op_handle, YBCPgExpr arg) {
  return ToYBCStatus(pgapi->OperatorAppendArg(op_handle, arg));
}

//------------------------------------------------------------------------------------------------
// Transaction operation.
//------------------------------------------------------------------------------------------------

YBCStatus YBCPgBeginTransaction() {
  return ToYBCStatus(pgapi->BeginTransaction());
}

YBCStatus YBCPgRecreateTransaction() {
  return ToYBCStatus(pgapi->RecreateTransaction());
}

YBCStatus YBCPgRestartTransaction() {
  return ToYBCStatus(pgapi->RestartTransaction());
}

YBCStatus YBCPgCommitTransaction() {
  return ToYBCStatus(pgapi->CommitTransaction());
}

YBCStatus YBCPgAbortTransaction() {
  return ToYBCStatus(pgapi->AbortTransaction());
}

YBCStatus YBCPgSetTransactionIsolationLevel(int isolation) {
  return ToYBCStatus(pgapi->SetTransactionIsolationLevel(isolation));
}

YBCStatus YBCPgSetTransactionReadOnly(bool read_only) {
  return ToYBCStatus(pgapi->SetTransactionReadOnly(read_only));
}

YBCStatus YBCPgSetTransactionDeferrable(bool deferrable) {
  return ToYBCStatus(pgapi->SetTransactionDeferrable(deferrable));
}

YBCStatus YBCPgEnterSeparateDdlTxnMode() {
  return ToYBCStatus(pgapi->EnterSeparateDdlTxnMode());
}

YBCStatus YBCPgExitSeparateDdlTxnMode(bool success) {
  return ToYBCStatus(pgapi->ExitSeparateDdlTxnMode(success));
}

// Referential Integrity Caching
YBCStatus YBCPgForeignKeyReferenceCacheDelete(const YBCPgYBTupleIdDescriptor *source) {
  return ProcessYbctid(*source, [](auto table_id, const auto& ybctid){
    pgapi->DeleteForeignKeyReference(table_id, ybctid);
    return Status::OK();
  });
}

void YBCPgDeleteFromForeignKeyReferenceCache(YBCPgOid table_oid, uint64_t ybctid) {
  char *value;
  int64_t bytes;

  const YBCPgTypeEntity *type_entity = pgapi->FindTypeEntity(kPgByteArrayOid);
  type_entity->datum_to_yb(ybctid, &value, &bytes);
  pgapi->DeleteForeignKeyReference(table_oid, Slice(value, bytes));
}

YBCStatus YBCForeignKeyReferenceExists(const YBCPgYBTupleIdDescriptor *source, bool* res) {
  return ProcessYbctid(*source, [res, source](auto table_id, const auto& ybctid) -> Status {
    *res = VERIFY_RESULT(pgapi->ForeignKeyReferenceExists(table_id, ybctid, source->database_oid));
    return Status::OK();
  });
}

YBCStatus YBCAddForeignKeyReferenceIntent(const YBCPgYBTupleIdDescriptor *source) {
  return ProcessYbctid(*source, [](auto table_id, const auto& ybctid) {
    pgapi->AddForeignKeyReferenceIntent(table_id, ybctid);
    return Status::OK();
  });
}

bool YBCIsInitDbModeEnvVarSet() {
  static bool cached_value = false;
  static bool cached = false;

  if (!cached) {
    const char* initdb_mode_env_var_value = getenv("YB_PG_INITDB_MODE");
    cached_value = initdb_mode_env_var_value && strcmp(initdb_mode_env_var_value, "1") == 0;
    cached = true;
  }

  return cached_value;
}

void YBCInitFlags() {
  if (YBCIsInitDbModeEnvVarSet()) {
    // Suppress log spew during initdb.
    FLAGS_client_suppress_created_logs = true;
  }

  SetAtomicFlag(GetAtomicFlag(&FLAGS_pggate_num_connections_to_server),
                &FLAGS_num_connections_to_server);

  // TODO(neil) Init a gflag for "YB_PG_TRANSACTIONS_ENABLED" here also.
  // Mikhail agreed that this flag should just be initialized once at the beginning here.
  // Currently, it is initialized for every CREATE statement.
}

YBCStatus YBCPgIsInitDbDone(bool* initdb_done) {
  return ExtractValueFromResult(pgapi->IsInitDbDone(), initdb_done);
}

const bool YBCGetDisableTransparentCacheRefreshRetry() {
  return pgapi->GetDisableTransparentCacheRefreshRetry();
}

YBCStatus YBCGetSharedCatalogVersion(uint64_t* catalog_version) {
  return ExtractValueFromResult(pgapi->GetSharedCatalogVersion(), catalog_version);
}

YBCStatus YBCGetSharedAuthKey(uint64_t* auth_key) {
  return ExtractValueFromResult(pgapi->GetSharedAuthKey(), auth_key);
}

int32_t YBCGetMaxReadRestartAttempts() {
  return FLAGS_ysql_max_read_restart_attempts;
}

int32_t YBCGetMaxWriteRestartAttempts() {
  return FLAGS_ysql_max_write_restart_attempts;
}

bool YBCShouldSleepBeforeRetryOnTxnConflict() {
  return FLAGS_ysql_sleep_before_retry_on_txn_conflict;
}

int32_t YBCGetOutputBufferSize() {
  return FLAGS_ysql_output_buffer_size;
}

int32_t YBCGetSequenceCacheMinval() {
  return FLAGS_ysql_sequence_cache_minval;
}

bool YBCGetDisableIndexBackfill() {
  return FLAGS_ysql_disable_index_backfill;
}

int32_t YBCGetTestIndexStateFlagsUpdateDelayMs() {
  return FLAGS_TEST_ysql_index_state_flags_update_delay_ms;
}

bool YBCPgIsYugaByteEnabled() {
  return pgapi;
}

void YBCSetTimeout(int timeout_ms, void* extra) {
  // We set the rpc timeouts as a min{STATEMENT_TIMEOUT, FLAGS_client_read_write_timeout_ms}.
  if (timeout_ms <= 0) {
    // The timeout is not valid. Use the default GFLAG value.
    return;
  }
  timeout_ms = std::min(timeout_ms, FLAGS_client_read_write_timeout_ms);

  // The statement timeout is lesser than FLAGS_client_read_write_timeout_ms, hence the rpcs would
  // need to use a shorter timeout.
  pgapi->SetTimeout(timeout_ms);
}

//------------------------------------------------------------------------------------------------
// Thread-local variables.
//------------------------------------------------------------------------------------------------

void* YBCPgGetThreadLocalCurrentMemoryContext() {
  return PgGetThreadLocalCurrentMemoryContext();
}

void* YBCPgSetThreadLocalCurrentMemoryContext(void *memctx) {
  return PgSetThreadLocalCurrentMemoryContext(memctx);
}

void YBCPgResetCurrentMemCtxThreadLocalVars() {
  PgResetCurrentMemCtxThreadLocalVars();
}

void* YBCPgGetThreadLocalStrTokPtr() {
  return PgGetThreadLocalStrTokPtr();
}

void YBCPgSetThreadLocalStrTokPtr(char *new_pg_strtok_ptr) {
  PgSetThreadLocalStrTokPtr(new_pg_strtok_ptr);
}

void* YBCPgSetThreadLocalJumpBuffer(void* new_buffer) {
  return PgSetThreadLocalJumpBuffer(new_buffer);
}

void* YBCPgGetThreadLocalJumpBuffer() {
  return PgGetThreadLocalJumpBuffer();
}

void YBCPgSetThreadLocalErrMsg(const void* new_msg) {
  PgSetThreadLocalErrMsg(new_msg);
}

const void* YBCPgGetThreadLocalErrMsg() {
  return PgGetThreadLocalErrMsg();
}

} // extern "C"

} // namespace pggate
} // namespace yb
