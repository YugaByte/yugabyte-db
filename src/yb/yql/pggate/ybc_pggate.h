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

// C wrappers around "pggate" for PostgreSQL to call.

#ifndef YB_YQL_PGGATE_YBC_PGGATE_H
#define YB_YQL_PGGATE_YBC_PGGATE_H

#include <stdint.h>

#include "yb/common/ybc_util.h"
#include "yb/yql/pggate/ybc_pg_typedefs.h"

#ifdef __cplusplus
extern "C" {
#endif

// This must be called exactly once to initialize the YB/PostgreSQL gateway API before any other
// functions in this API are called.
void YBCInitPgGate(const YBCPgTypeEntity *YBCDataTypeTable, int count, YBCPgCallbacks pg_callbacks);
void YBCDestroyPgGate();

//--------------------------------------------------------------------------------------------------
// Environment and Session.

// Initialize ENV within which PGSQL calls will be executed.
YBCStatus YBCPgCreateEnv(YBCPgEnv *pg_env);
YBCStatus YBCPgDestroyEnv(YBCPgEnv pg_env);

// Initialize a session to process statements that come from the same client connection.
YBCStatus YBCPgInitSession(const YBCPgEnv pg_env, const char *database_name);

// Initialize YBCPgMemCtx.
// - Postgres uses memory context to hold all of its allocated space. Once all associated operations
//   are done, the context is destroyed.
// - There YugaByte objects are bound to Postgres operations. All of these objects' allocated
//   memory will be held by YBCPgMemCtx, whose handle belongs to Postgres MemoryContext. Once all
//   Postgres operations are done, associated YugaByte memory context (YBCPgMemCtx) will be
//   destroyed toghether with Postgres memory context.
YBCPgMemctx YBCPgCreateMemctx();
YBCStatus YBCPgDestroyMemctx(YBCPgMemctx memctx);
YBCStatus YBCPgResetMemctx(YBCPgMemctx memctx);
void YBCPgDeleteStatement(YBCPgStatement handle);

// Invalidate the sessions table cache.
YBCStatus YBCPgInvalidateCache();

// Clear all values and expressions that were bound to the given statement.
YBCStatus YBCPgClearBinds(YBCPgStatement handle);

// Check if initdb has been already run.
YBCStatus YBCPgIsInitDbDone(bool* initdb_done);

// Get gflag TEST_ysql_disable_transparent_cache_refresh_retry
const bool YBCGetDisableTransparentCacheRefreshRetry();

// Set catalog_version to the local tserver's catalog version stored in shared memory.  Return error
// if the shared memory has not been initialized (e.g. in initdb).
YBCStatus YBCGetSharedCatalogVersion(uint64_t* catalog_version);
// Set auth_key to the local tserver's postgres authentication key stored in shared memory.  Return
// error if the shared memory has not been initialized (e.g. in initdb).
YBCStatus YBCGetSharedAuthKey(uint64_t* auth_key);

//--------------------------------------------------------------------------------------------------
// DDL Statements
//--------------------------------------------------------------------------------------------------

// DATABASE ----------------------------------------------------------------------------------------
// Connect database. Switch the connected database to the given "database_name".
YBCStatus YBCPgConnectDatabase(const char *database_name);

// Get whether the given database is colocated.
YBCStatus YBCPgIsDatabaseColocated(const YBCPgOid database_oid, bool *colocated);

YBCStatus YBCInsertSequenceTuple(int64_t db_oid,
                                 int64_t seq_oid,
                                 uint64_t ysql_catalog_version,
                                 int64_t last_val,
                                 bool is_called);

YBCStatus YBCUpdateSequenceTupleConditionally(int64_t db_oid,
                                              int64_t seq_oid,
                                              uint64_t ysql_catalog_version,
                                              int64_t last_val,
                                              bool is_called,
                                              int64_t expected_last_val,
                                              bool expected_is_called,
                                              bool *skipped);

YBCStatus YBCUpdateSequenceTuple(int64_t db_oid,
                                 int64_t seq_oid,
                                 uint64_t ysql_catalog_version,
                                 int64_t last_val,
                                 bool is_called,
                                 bool* skipped);

YBCStatus YBCReadSequenceTuple(int64_t db_oid,
                               int64_t seq_oid,
                               uint64_t ysql_catalog_version,
                               int64_t *last_val,
                               bool *is_called);

YBCStatus YBCDeleteSequenceTuple(int64_t db_oid, int64_t seq_oid);

// Create database.
YBCStatus YBCPgNewCreateDatabase(const char *database_name,
                                 YBCPgOid database_oid,
                                 YBCPgOid source_database_oid,
                                 YBCPgOid next_oid,
                                 const bool colocated,
                                 YBCPgStatement *handle);
YBCStatus YBCPgExecCreateDatabase(YBCPgStatement handle);

// Drop database.
YBCStatus YBCPgNewDropDatabase(const char *database_name,
                               YBCPgOid database_oid,
                               YBCPgStatement *handle);
YBCStatus YBCPgExecDropDatabase(YBCPgStatement handle);

// Alter database.
YBCStatus YBCPgNewAlterDatabase(const char *database_name,
                               YBCPgOid database_oid,
                               YBCPgStatement *handle);
YBCStatus YBCPgAlterDatabaseRenameDatabase(YBCPgStatement handle, const char *newname);
YBCStatus YBCPgExecAlterDatabase(YBCPgStatement handle);

// Reserve oids.
YBCStatus YBCPgReserveOids(YBCPgOid database_oid,
                           YBCPgOid next_oid,
                           uint32_t count,
                           YBCPgOid *begin_oid,
                           YBCPgOid *end_oid);

// Retrieve the protobuf-based catalog version (now deprecated for new clusters).
YBCStatus YBCPgGetCatalogMasterVersion(uint64_t *version);

void YBCPgInvalidateTableCache(
    const YBCPgOid database_oid,
    const YBCPgOid table_oid);
YBCStatus YBCPgInvalidateTableCacheByTableId(const char *table_id);

// TABLEGROUP --------------------------------------------------------------------------------------

// Create tablegroup.
YBCStatus YBCPgNewCreateTablegroup(const char *database_name,
                                   YBCPgOid database_oid,
                                   YBCPgOid tablegroup_oid,
                                   YBCPgStatement *handle);
YBCStatus YBCPgExecCreateTablegroup(YBCPgStatement handle);

// Drop tablegroup.
YBCStatus YBCPgNewDropTablegroup(YBCPgOid database_oid,
                                 YBCPgOid tablegroup_oid,
                                 YBCPgStatement *handle);
YBCStatus YBCPgExecDropTablegroup(YBCPgStatement handle);

// TABLE -------------------------------------------------------------------------------------------
// Create and drop table "database_name.schema_name.table_name()".
// - When "schema_name" is NULL, the table "database_name.table_name" is created.
// - When "database_name" is NULL, the table "connected_database_name.table_name" is created.
YBCStatus YBCPgNewCreateTable(const char *database_name,
                              const char *schema_name,
                              const char *table_name,
                              YBCPgOid database_oid,
                              YBCPgOid table_oid,
                              bool is_shared_table,
                              bool if_not_exist,
                              bool add_primary_key,
                              const bool colocated,
                              YBCPgOid tablegroup_oid,
                              YBCPgStatement *handle);

YBCStatus YBCPgCreateTableAddColumn(YBCPgStatement handle, const char *attr_name, int attr_num,
                                    const YBCPgTypeEntity *attr_type, bool is_hash, bool is_range,
                                    bool is_desc, bool is_nulls_first);

YBCStatus YBCPgCreateTableSetNumTablets(YBCPgStatement handle, int32_t num_tablets);

YBCStatus YBCPgAddSplitBoundary(YBCPgStatement handle, YBCPgExpr *exprs, int expr_count);

YBCStatus YBCPgExecCreateTable(YBCPgStatement handle);

YBCStatus YBCPgNewAlterTable(YBCPgOid database_oid,
                             YBCPgOid table_oid,
                             YBCPgStatement *handle);

YBCStatus YBCPgAlterTableAddColumn(YBCPgStatement handle, const char *name, int order,
                                   const YBCPgTypeEntity *attr_type);

YBCStatus YBCPgAlterTableRenameColumn(YBCPgStatement handle, const char *oldname,
                                      const char *newname);

YBCStatus YBCPgAlterTableDropColumn(YBCPgStatement handle, const char *name);

YBCStatus YBCPgAlterTableRenameTable(YBCPgStatement handle, const char *db_name,
                                     const char *newname);

YBCStatus YBCPgExecAlterTable(YBCPgStatement handle);

YBCStatus YBCPgNewDropTable(YBCPgOid database_oid,
                            YBCPgOid table_oid,
                            bool if_exist,
                            YBCPgStatement *handle);

YBCStatus YBCPgNewTruncateTable(YBCPgOid database_oid,
                                YBCPgOid table_oid,
                                YBCPgStatement *handle);

YBCStatus YBCPgExecTruncateTable(YBCPgStatement handle);

YBCStatus YBCPgGetTableDesc(YBCPgOid database_oid,
                            YBCPgOid table_oid,
                            YBCPgTableDesc *handle);

YBCStatus YBCPgGetColumnInfo(YBCPgTableDesc table_desc,
                             int16_t attr_number,
                             bool *is_primary,
                             bool *is_hash);

YBCStatus YBCPgGetTableProperties(YBCPgTableDesc table_desc,
                                  YBCPgTableProperties *properties);

YBCStatus YBCPgDmlModifiesRow(YBCPgStatement handle, bool *modifies_row);

YBCStatus YBCPgSetIsSysCatalogVersionChange(YBCPgStatement handle);

YBCStatus YBCPgSetCatalogCacheVersion(YBCPgStatement handle, uint64_t catalog_cache_version);

YBCStatus YBCPgIsTableColocated(const YBCPgOid database_oid,
                                const YBCPgOid table_oid,
                                bool *colocated);

YBCStatus YBCPgTableExists(const YBCPgOid database_oid,
                           const YBCPgOid table_oid,
                           bool *exists);

// INDEX -------------------------------------------------------------------------------------------
// Create and drop index "database_name.schema_name.index_name()".
// - When "schema_name" is NULL, the index "database_name.index_name" is created.
// - When "database_name" is NULL, the index "connected_database_name.index_name" is created.
YBCStatus YBCPgNewCreateIndex(const char *database_name,
                              const char *schema_name,
                              const char *index_name,
                              YBCPgOid database_oid,
                              YBCPgOid index_oid,
                              YBCPgOid table_oid,
                              bool is_shared_index,
                              bool is_unique_index,
                              const bool skip_index_backfill,
                              bool if_not_exist,
                              YBCPgOid tablegroup_oid,
                              YBCPgStatement *handle);

YBCStatus YBCPgCreateIndexAddColumn(YBCPgStatement handle, const char *attr_name, int attr_num,
                                    const YBCPgTypeEntity *attr_type, bool is_hash, bool is_range,
                                    bool is_desc, bool is_nulls_first);

YBCStatus YBCPgCreateIndexSetNumTablets(YBCPgStatement handle, int32_t num_tablets);

YBCStatus YBCPgExecCreateIndex(YBCPgStatement handle);

YBCStatus YBCPgNewDropIndex(YBCPgOid database_oid,
                            YBCPgOid index_oid,
                            bool if_exist,
                            YBCPgStatement *handle);

YBCStatus YBCPgWaitUntilIndexPermissionsAtLeast(
    const YBCPgOid database_oid,
    const YBCPgOid table_oid,
    const YBCPgOid index_oid,
    const uint32_t target_index_permissions,
    uint32_t *actual_index_permissions);

YBCStatus YBCPgAsyncUpdateIndexPermissions(
    const YBCPgOid database_oid,
    const YBCPgOid indexed_table_oid);

YBCStatus YBCPgExecPostponedDdlStmt(YBCPgStatement handle);

YBCStatus YBCPgBackfillIndex(
    const YBCPgOid database_oid,
    const YBCPgOid index_oid);

//--------------------------------------------------------------------------------------------------
// DML statements (select, insert, update, delete, truncate)
//--------------------------------------------------------------------------------------------------

// This function is for specifying the selected or returned expressions.
// - SELECT target_expr1, target_expr2, ...
// - INSERT / UPDATE / DELETE ... RETURNING target_expr1, target_expr2, ...
YBCStatus YBCPgDmlAppendTarget(YBCPgStatement handle, YBCPgExpr target);

// Binding Columns: Bind column with a value (expression) in a statement.
// + This API is used to identify the rows you want to operate on. If binding columns are not
//   there, that means you want to operate on all rows (full scan). You can view this as a
//   a definitions of an initial rowset or an optimization over full-scan.
//
// + There are some restrictions on when BindColumn() can be used.
//   Case 1: INSERT INTO tab(x) VALUES(x_expr)
//   - BindColumn() can be used for BOTH primary-key and regular columns.
//   - This bind-column function is used to bind "x" with "x_expr", and "x_expr" that can contain
//     bind-variables (placeholders) and constants whose values can be updated for each execution
//     of the same allocated statement.
//
//   Case 2: SELECT / UPDATE / DELETE <WHERE key = "key_expr">
//   - BindColumn() can only be used for primary-key columns.
//   - This bind-column function is used to bind the primary column "key" with "key_expr" that can
//     contain bind-variables (placeholders) and constants whose values can be updated for each
//     execution of the same allocated statement.
//
// NOTE ON KEY BINDING
// - For Sequential Scan, the target columns of the bind are those in the main table.
// - For Primary Scan, the target columns of the bind are those in the main table.
// - For Index Scan, the target columns of the bind are those in the index table.
//   The index-scan will use the bind to find base-ybctid which is then use to read data from
//   the main-table, and therefore the bind-arguments are not associated with columns in main table.
YBCStatus YBCPgDmlBindColumn(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value);
YBCStatus YBCPgDmlBindColumnCondEq(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value);
YBCStatus YBCPgDmlBindColumnCondBetween(YBCPgStatement handle, int attr_num, YBCPgExpr attr_value,
    YBCPgExpr attr_value_end);
YBCStatus YBCPgDmlBindColumnCondIn(YBCPgStatement handle, int attr_num, int n_attr_values,
    YBCPgExpr *attr_values);

// Binding Tables: Bind the whole table in a statement.  Do not use with BindColumn.
YBCStatus YBCPgDmlBindTable(YBCPgStatement handle);

// API for SET clause.
YBCStatus YBCPgDmlAssignColumn(YBCPgStatement handle,
                               int attr_num,
                               YBCPgExpr attr_value);

// This function is to fetch the targets in YBCPgDmlAppendTarget() from the rows that were defined
// by YBCPgDmlBindColumn().
YBCStatus YBCPgDmlFetch(YBCPgStatement handle, int32_t natts, uint64_t *values, bool *isnulls,
                        YBCPgSysColumns *syscols, bool *has_data);

// Utility method that checks stmt type and calls either exec insert, update, or delete internally.
YBCStatus YBCPgDmlExecWriteOp(YBCPgStatement handle, int32_t *rows_affected_count);

// This function returns the tuple id (ybctid) of a Postgres tuple.
YBCStatus YBCPgBuildYBTupleId(const YBCPgYBTupleIdDescriptor* data, uint64_t *ybctid);

// DB Operations: WHERE, ORDER_BY, GROUP_BY, etc.
// + The following operations are run by DocDB.
//   - Not yet
//
// + The following operations are run by Postgres layer. An API might be added to move these
//   operations to DocDB.
//   - API for "where_expr"
//   - API for "order_by_expr"
//   - API for "group_by_expr"


// Buffer write operations.
void YBCPgStartOperationsBuffering();
YBCStatus YBCPgStopOperationsBuffering();
YBCStatus YBCPgResetOperationsBuffering();
YBCStatus YBCPgFlushBufferedOperations();
void YBCPgDropBufferedOperations();

// INSERT ------------------------------------------------------------------------------------------
YBCStatus YBCPgNewInsert(YBCPgOid database_oid,
                         YBCPgOid table_oid,
                         bool is_single_row_txn,
                         YBCPgStatement *handle);

YBCStatus YBCPgExecInsert(YBCPgStatement handle);

YBCStatus YBCPgInsertStmtSetUpsertMode(YBCPgStatement handle);

YBCStatus YBCPgInsertStmtSetWriteTime(YBCPgStatement handle, const uint64_t write_time);

YBCStatus YBCPgInsertStmtSetIsBackfill(YBCPgStatement handle, const bool is_backfill);

// UPDATE ------------------------------------------------------------------------------------------
YBCStatus YBCPgNewUpdate(YBCPgOid database_oid,
                         YBCPgOid table_oid,
                         bool is_single_row_txn,
                         YBCPgStatement *handle);

YBCStatus YBCPgExecUpdate(YBCPgStatement handle);

// Retrieve value of ysql_enable_update_batching gflag.
bool YBCGetEnableUpdateBatching();

// DELETE ------------------------------------------------------------------------------------------
YBCStatus YBCPgNewDelete(YBCPgOid database_oid,
                         YBCPgOid table_oid,
                         bool is_single_row_txn,
                         YBCPgStatement *handle);

YBCStatus YBCPgExecDelete(YBCPgStatement handle);

// Colocated TRUNCATE ------------------------------------------------------------------------------
YBCStatus YBCPgNewTruncateColocated(YBCPgOid database_oid,
                                    YBCPgOid table_oid,
                                    bool is_single_row_txn,
                                    YBCPgStatement *handle);

YBCStatus YBCPgExecTruncateColocated(YBCPgStatement handle);

// SELECT ------------------------------------------------------------------------------------------
YBCStatus YBCPgNewSelect(YBCPgOid database_oid,
                         YBCPgOid table_oid,
                         const YBCPgPrepareParameters *prepare_params,
                         YBCPgStatement *handle);

// Set forward/backward scan direction.
YBCStatus YBCPgSetForwardScan(YBCPgStatement handle, bool is_forward_scan);

YBCStatus YBCPgExecSelect(YBCPgStatement handle, const YBCPgExecParameters *exec_params);

// Transaction control -----------------------------------------------------------------------------
YBCStatus YBCPgBeginTransaction();
YBCStatus YBCPgRecreateTransaction();
YBCStatus YBCPgRestartTransaction();
YBCStatus YBCPgCommitTransaction();
YBCStatus YBCPgAbortTransaction();
YBCStatus YBCPgSetTransactionIsolationLevel(int isolation);
YBCStatus YBCPgSetTransactionReadOnly(bool read_only);
YBCStatus YBCPgSetTransactionDeferrable(bool deferrable);
YBCStatus YBCPgEnterSeparateDdlTxnMode();
YBCStatus YBCPgExitSeparateDdlTxnMode(bool success);

//--------------------------------------------------------------------------------------------------
// Expressions.

// Column references.
YBCStatus YBCPgNewColumnRef(YBCPgStatement stmt, int attr_num, const YBCPgTypeEntity *type_entity,
                            const YBCPgTypeAttrs *type_attrs, YBCPgExpr *expr_handle);

// Constant expressions.
// Construct an actual constant value.
YBCStatus YBCPgNewConstant(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                           uint64_t datum, bool is_null, YBCPgExpr *expr_handle);
// Construct a virtual constant value.
YBCStatus YBCPgNewConstantVirtual(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                                  YBCPgDatumKind datum_kind, YBCPgExpr *expr_handle);
// Construct an operator expression on a constant.
YBCStatus YBCPgNewConstantOp(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                             uint64_t datum, bool is_null, YBCPgExpr *expr_handle, bool is_gt);

// The following update functions only work for constants.
// Overwriting the constant expression with new value.
YBCStatus YBCPgUpdateConstInt2(YBCPgExpr expr, int16_t value, bool is_null);
YBCStatus YBCPgUpdateConstInt4(YBCPgExpr expr, int32_t value, bool is_null);
YBCStatus YBCPgUpdateConstInt8(YBCPgExpr expr, int64_t value, bool is_null);
YBCStatus YBCPgUpdateConstFloat4(YBCPgExpr expr, float value, bool is_null);
YBCStatus YBCPgUpdateConstFloat8(YBCPgExpr expr, double value, bool is_null);
YBCStatus YBCPgUpdateConstText(YBCPgExpr expr, const char *value, bool is_null);
YBCStatus YBCPgUpdateConstChar(YBCPgExpr expr, const char *value, int64_t bytes, bool is_null);

// Expressions with operators "=", "+", "between", "in", ...
YBCStatus YBCPgNewOperator(YBCPgStatement stmt, const char *opname,
                           const YBCPgTypeEntity *type_entity,
                           YBCPgExpr *op_handle);
YBCStatus YBCPgOperatorAppendArg(YBCPgExpr op_handle, YBCPgExpr arg);

// Referential Integrity Check Caching.
void YBCPgDeleteFromForeignKeyReferenceCache(YBCPgOid table_oid, uint64_t ybctid);
YBCStatus YBCPgForeignKeyReferenceCacheDelete(const YBCPgYBTupleIdDescriptor* descr);
YBCStatus YBCForeignKeyReferenceExists(const YBCPgYBTupleIdDescriptor* descr, bool* res);
YBCStatus YBCAddForeignKeyReferenceIntent(const YBCPgYBTupleIdDescriptor* descr);

bool YBCIsInitDbModeEnvVarSet();

// This is called by initdb. Used to customize some behavior.
void YBCInitFlags();

// Retrieves value of ysql_max_read_restart_attempts gflag
int32_t YBCGetMaxReadRestartAttempts();

// Retrieves the value of ysql_max_write_restart_attempts gflag.
int32_t YBCGetMaxWriteRestartAttempts();

// Retrieves the value of ysql_sleep_before_retry_on_txn_conflict gflag.
bool YBCShouldSleepBeforeRetryOnTxnConflict();

// Retrieves value of ysql_output_buffer_size gflag
int32_t YBCGetOutputBufferSize();

// Retrieves value of ysql_sequence_cache_minval gflag
int32_t YBCGetSequenceCacheMinval();

// Retrieve value of ysql_disable_index_backfill gflag.
bool YBCGetDisableIndexBackfill();

// Retrieve value of TEST_ysql_index_state_flags_update_delay_ms gflag.
int32_t YBCGetTestIndexStateFlagsUpdateDelayMs();

bool YBCPgIsYugaByteEnabled();

// Sets the specified timeout in the rpc service.
void YBCSetTimeout(int timeout_ms, void* extra);

//--------------------------------------------------------------------------------------------------
// Thread-Local variables.

void* YBCPgGetThreadLocalCurrentMemoryContext();

void* YBCPgSetThreadLocalCurrentMemoryContext(void *memctx);

void YBCPgResetCurrentMemCtxThreadLocalVars();

void* YBCPgGetThreadLocalStrTokPtr();

void YBCPgSetThreadLocalStrTokPtr(char *new_pg_strtok_ptr);

void* YBCPgSetThreadLocalJumpBuffer(void* new_buffer);

void* YBCPgGetThreadLocalJumpBuffer();

void YBCPgSetThreadLocalErrMsg(const void* new_msg);

const void* YBCPgGetThreadLocalErrMsg();

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // YB_YQL_PGGATE_YBC_PGGATE_H
