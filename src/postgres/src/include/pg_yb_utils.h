/* ----------
 * pg_yb_utils.h
 *
 * Utilities for YugaByte/PostgreSQL integration that have to be defined on the
 * PostgreSQL side.
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * src/include/pg_yb_utils.h
 * ----------
 */

#ifndef PG_YB_UTILS_H
#define PG_YB_UTILS_H

#include "postgres.h"
#include "utils/relcache.h"

#include "common/pg_yb_common.h"
#include "yb/common/ybc_util.h"
#include "yb/yql/pggate/ybc_pggate.h"
#include "access/reloptions.h"

#include "utils/resowner.h"

/*
 * Version of the catalog entries in the relcache and catcache.
 * We (only) rely on a following invariant: If the catalog cache version here is
 * actually the latest (master) version, then the catalog data is indeed up to
 * date. In any other case, we will end up doing a cache refresh anyway.
 * (I.e. cache data is only valid if the version below matches with master's
 * version, otherwise all bets are off and we need to refresh.)
 *
 * So we should handle cases like:
 * 1. yb_catalog_cache_version being behind the actual data in the caches.
 * 2. Data in the caches spanning multiple version (because catalog was updated
 *    during a cache refresh).
 * As long as the invariant above is not violated we should (at most) end up
 * doing a redundant cache refresh.
 *
 * TODO: Improve cache versioning and refresh logic to be more fine-grained to
 * reduce frequency and/or duration of cache refreshes.
 */
extern uint64_t yb_catalog_cache_version;

#define YB_CATCACHE_VERSION_UNINITIALIZED (0)

/*
 * Checks whether YugaByte functionality is enabled within PostgreSQL.
 * This relies on pgapi being non-NULL, so probably should not be used
 * in postmaster (which does not need to talk to YB backend) or early
 * in backend process initialization. In those cases the
 * YBIsEnabledInPostgresEnvVar function might be more appropriate.
 */
extern bool IsYugaByteEnabled();

/*
 * Given a relation, checks whether the relation is supported in YugaByte mode.
 */
extern void CheckIsYBSupportedRelation(Relation relation);

extern void CheckIsYBSupportedRelationByKind(char relkind);

/*
 * Given a relation (table) id, returns whether this table is handled by
 * YugaByte: i.e. it is not a temporary or foreign table.
 */
extern bool IsYBRelationById(Oid relid);

extern bool IsYBRelation(Relation relation);

/*
 * Same as IsYBRelation but it additionally includes views on YugaByte
 * relations i.e. views on persistent (non-temporary) tables.
 */
extern bool IsYBBackedRelation(Relation relation);

/*
 * Returns whether a relation's attribute is a real column in the backing
 * YugaByte table. (It implies we can both read from and write to it).
 */
extern bool IsRealYBColumn(Relation rel, int attrNum);

/*
 * Returns whether a relation's attribute is a YB system column.
 */
extern bool IsYBSystemColumn(int attrNum);

extern bool YBNeedRetryAfterCacheRefresh(ErrorData *edata);

extern void YBReportFeatureUnsupported(const char *err_msg);

extern AttrNumber YBGetFirstLowInvalidAttributeNumber(Relation relation);

extern AttrNumber YBGetFirstLowInvalidAttributeNumberFromOid(Oid relid);

extern int YBAttnumToBmsIndex(Relation rel, AttrNumber attnum);

extern AttrNumber YBBmsIndexToAttnum(Relation rel, int idx);

/*
 * Get primary key columns as bitmap set of a table for real YB columns.
 * Subtracts YBGetFirstLowInvalidAttributeNumber from column attribute numbers.
 */
extern Bitmapset *YBGetTablePrimaryKeyBms(Relation rel);

/*
 * Get primary key columns as bitmap set of a table for real and system YB columns.
 * Subtracts (YBSystemFirstLowInvalidAttributeNumber + 1) from column attribute numbers.
 */
extern Bitmapset *YBGetTableFullPrimaryKeyBms(Relation rel);

/*
 * Check if a relation has row triggers that may reference the old row.
 * Specifically for an update/delete DML (where there actually is an old row).
 */
extern bool YBRelHasOldRowTriggers(Relation rel, CmdType operation);

/*
 * Check if a relation has secondary indices.
 */
extern bool YBRelHasSecondaryIndices(Relation relation);

/*
 * Whether to route BEGIN / COMMIT / ROLLBACK to YugaByte's distributed
 * transactions.
 */
extern bool YBTransactionsEnabled();

/*
 * Given a status returned by YB C++ code, reports that status using ereport if
 * it is not OK.
 */
extern void	HandleYBStatus(YBCStatus status);

/*
 * Since DDL metadata in master DocDB and postgres system tables is not modified
 * in an atomic fashion, it is possible that we could have a table existing in
 * postgres metadata but not in DocDB. In the case of a delete it is really
 * problematic, since we can't delete the table nor can we create a new one with
 * the same name. So in this case we just ignore the DocDB 'NotFound' error and
 * delete our metadata.
 */
extern void HandleYBStatusIgnoreNotFound(YBCStatus status, bool *not_found);

/*
 * Same as HandleYBStatus but also ask the given resource owner to forget
 * the given YugaByte statement.
 */
extern void HandleYBStatusWithOwner(YBCStatus status,
									YBCPgStatement ybc_stmt,
									ResourceOwner owner);

/*
 * Same as HandleYBStatus but delete the table description first if the
 * status is not ok.
 */
extern void HandleYBTableDescStatus(YBCStatus status, YBCPgTableDesc table);
/*
 * YB initialization that needs to happen when a PostgreSQL backend process
 * is started. Reports errors using ereport.
 */
extern void YBInitPostgresBackend(const char *program_name,
								  const char *db_name,
								  const char *user_name);

/*
 * This should be called on all exit paths from the PostgreSQL backend process.
 * Only main PostgreSQL backend thread is expected to call this.
 */
extern void YBOnPostgresBackendShutdown();

/*
 * Signals PgTxnManager to restart current transaction - pick a new read point, etc.
 * This relies on transaction/session read time already being marked for restart by YB layer.
 */
extern void YBCRestartTransaction();

/*
 * Commits the current YugaByte-level transaction (if any).
 */
extern void YBCCommitTransaction();

/*
 * Aborts the current YugaByte-level transaction.
 */
extern void YBCAbortTransaction();

/*
 * Return true if we want to allow PostgreSQL's own locking. This is needed
 * while system tables are still managed by PostgreSQL.
 */
extern bool YBIsPgLockingEnabled();

/*
 * Return a string representation of the given type id, or say it is unknown.
 * What is returned is always a static C string constant.
 */
extern const char* YBPgTypeOidToStr(Oid type_id);

/*
 * Return a string representation of the given PgDataType, or say it is unknown.
 * What is returned is always a static C string constant.
 */
extern const char* YBCPgDataTypeToStr(YBCPgDataType yb_type);

/*
 * Report an error saying the given type as not supported by YugaByte.
 */
extern void YBReportTypeNotSupported(Oid type_id);

/*
 * Log whether or not YugaByte is enabled.
 */
extern void YBReportIfYugaByteEnabled();

#define YB_REPORT_TYPE_NOT_SUPPORTED(type_id) do { \
		Oid computed_type_id = type_id; \
		ereport(ERROR, \
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
					errmsg("Type not yet supported in YugaByte: %d (%s)", \
						computed_type_id, YBPgTypeOidToStr(computed_type_id)))); \
	} while (0)

/*
 * Determines if PostgreSQL should restart all child processes if one of them
 * crashes. This behavior usually shows up in the log like so:
 *
 * WARNING:  terminating connection because of crash of another server process
 * DETAIL:  The postmaster has commanded this server process to roll back the
 *          current transaction and exit, because another server process exited
 *          abnormally and possibly corrupted shared memory.
 *
 * However, we want to avoid this behavior in some cases, e.g. when our test
 * framework is trying to intentionally cause core dumps of stuck backend
 * processes and analyze them. Disabling this behavior is controlled by setting
 * the YB_PG_NO_RESTART_ALL_CHILDREN_ON_CRASH_FLAG_PATH variable to a file path,
 * which could be created or deleted at run time, and its existence is always
 * checked.
 */
bool YBShouldRestartAllChildrenIfOneCrashes();

/*
 * These functions help indicating if we are creating system catalog.
 */
void YBSetPreparingTemplates();
bool YBIsPreparingTemplates();

/*
 * Whether every ereport of the ERROR level and higher should log a stack trace.
 */
bool YBShouldLogStackTraceOnError();

/*
 * Converts the PostgreSQL error level as listed in elog.h to a string. Always
 * returns a static const char string.
 */
const char* YBPgErrorLevelToString(int elevel);

/*
 * Get the database name for a relation id (accounts for system databases and
 * shared relations)
 */
const char* YBCGetDatabaseName(Oid relid);

/*
 * Get the schema name for a schema oid (accounts for system namespaces)
 */
const char* YBCGetSchemaName(Oid schemaoid);

/*
 * Get the real database id of a relation. For shared relations, it will be
 * template1.
 */
Oid YBCGetDatabaseOid(Relation rel);

/*
 * Raise an unsupported feature error with the given message and
 * linking to the referenced issue (if any).
 */
void YBRaiseNotSupported(const char *msg, int issue_no);
void YBRaiseNotSupportedSignal(const char *msg, int issue_no, int signal_level);

//------------------------------------------------------------------------------
// YB Debug utils.

/**
 * YSQL guc variables that can be used to toggle yugabyte debug features.
 * e.g. 'SET yb_debug_report_error_stacktrace=true' and
 *      'RESET yb_debug_report_error_stacktrace'.
 * See also the corresponding entries in guc.c.
 */

/* Add stacktrace information to every YSQL error. */
extern bool yb_debug_report_error_stacktrace;

/* Log cache misses and cache refresh events. */
extern bool yb_debug_log_catcache_events;

/*
 * Log automatic statement (or transaction) restarts such as read-restarts and
 * schema-version restarts (e.g. catalog version mismatch errors).
 */
extern bool yb_debug_log_internal_restarts;

/*
 * See also ybc_util.h which contains additional such variable declarations for
 * variables that are (also) used in the pggate layer.
 * Currently: yb_debug_log_docdb_requests.
 */

/*
 * Get a string representation of a datum (given its type).
 */
extern const char* YBDatumToString(Datum datum, Oid typid);

/*
 * Get a string representation of a tuple (row) given its tuple description (schema).
 */
extern const char* YBHeapTupleToString(HeapTuple tuple, TupleDesc tupleDesc);

/*
 * Checks if the master thinks initdb has already been done.
 */
bool YBIsInitDbAlreadyDone();

int YBGetDdlNestingLevel();
void YBIncrementDdlNestingLevel();
void YBDecrementDdlNestingLevel(bool success);

extern void YBBeginOperationsBuffering();
extern void YBEndOperationsBuffering();
extern void YBResetOperationsBuffering();

#endif /* PG_YB_UTILS_H */
