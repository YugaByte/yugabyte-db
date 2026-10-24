/* ----------
 * pg_yb_utils.h
 *
 * Utilities for YugaByte/PostgreSQL integration that have to be defined on the
 * PostgreSQL side.
 *
 * Copyright (c) YugabyteDB, Inc.
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

#include "access/reloptions.h"
#include "catalog/pg_database.h"
#include "common/pg_yb_common.h"
#include "executor/instrument.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "storage/lock.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"
#include "utils/uuid.h"
#include "yb/yql/pggate/util/ybc_util.h"
#include "yb/yql/pggate/ybc_dist_trace.h"
#include "yb/yql/pggate/ybc_pg_typedefs.h"
#include "yb_ysql_conn_mgr_helper.h"

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

#define YB_CATCACHE_VERSION_UNINITIALIZED (0)

/*
 * Check if (const char *)FLAG is non-empty.
 */
#define IS_NON_EMPTY_STR_FLAG(flag) (flag != NULL && flag[0] != '\0')

/*
 * Must be kept the same as CatCacheMsgs and RelCacheMsgs in inval.c. YB has
 * added static_assert to ensure that.
 */
#define YB_CATCACHE_MSGS (0)
#define YB_RELCACHE_MSGS (1)

/*
 * Postgres code uses process-level storage (e.g. static variables) to store
 * long-lived data in a backend process. During expression pushdown, we may be
 * reading & writing to the same variable from multiple threads in the same
 * process, so using process-level storage is not safe. We use thread-local
 * storage to ensure thread safety for these variables.
 */
#define YB_THREAD_LOCAL __thread

/*
 * Utility to get the current cache version that accounts for the fact that
 * during a DDL we automatically apply the pending syscatalog changes to
 * the local cache (of the current session).
 * Therefore, if we are within a DDL we return yb_catalog_cache_version + 1.
 * Currently, this is only used during procedure/function compilation so that
 * compilation during CREATE FUNCTION/PROCEDURE is cached correctly.
 * TODO Is there a simpler way to handle this?
 */
extern uint64_t YBGetActiveCatalogCacheVersion();

extern uint64_t YbGetCatalogCacheVersion();
extern uint64_t YbGetNewCatalogVersion();
extern void YbSetNeedInvalidateAllTableCache();
extern void YbResetNeedInvalidateAllTableCache();
extern bool YbGetNeedInvalidateAllTableCache();
extern bool YbCanTryInvalidateTableCacheEntry();

extern void YbUpdateCatalogCacheVersion(uint64_t catalog_cache_version);
extern void YbUpdateCatalogCacheVersionNoPgStat(uint64_t catalog_cache_version);
extern void YbResetNewCatalogVersion();
extern void YbSetNewCatalogVersion(uint64_t new_version);

extern void YbSetLogicalClientCacheVersion(uint64_t logical_client_cache_version);
extern void YbResetLogicalClientCacheVersion();

extern void YbSendLogicalClientCacheVersionToFrontend();
extern void YbSendMasterLogicalClientVersionToFrontend();

extern void YbResetCatalogCacheVersion();

extern YbcPgLastKnownCatalogVersionInfo YbGetCatalogCacheVersionForTablePrefetching();

extern void YbUpdateLastKnownCatalogCacheVersion(uint64_t catalog_cache_version);

typedef enum YbGeolocationDistance
{
	ZONE_LOCAL,
	REGION_LOCAL,
	CLOUD_LOCAL,
	INTER_CLOUD,
	UNKNOWN_DISTANCE
} YbGeolocationDistance;

extern YbGeolocationDistance get_geolocation_distance(Oid tablespaceoid);

/**
 * Returns a null-terminated string representing the name of the
 * cloud this process is running on.
 */
extern const char *YBGetCurrentCloud();

/**
 * Returns a null-terminated string representing the region this
 * process is running on.
 */
extern const char *YBGetCurrentRegion();

/**
 * Returns a null-terminated string representing the zone this
 * process is running on.
 */
extern const char *YBGetCurrentZone();

/*
 * Checks whether YugaByte functionality is enabled within PostgreSQL.
 * This relies on pgapi being non-NULL, so probably should not be used
 * in postmaster (which does not need to talk to YB backend) or early
 * in backend process initialization. In those cases the
 * YBIsEnabledInPostgresEnvVar function might be more appropriate.
 */
extern bool IsYugaByteEnabled();

extern bool yb_enable_docdb_tracing;
extern bool yb_enable_pg_subscription;
extern bool yb_enable_spi_dist_tracing;

extern bool yb_read_from_followers;
extern bool yb_follower_reads_behavior_before_fixing_20482;
extern int32_t yb_follower_read_staleness_ms;

/*
 * Parsed span context from the yb_dist_tracecontext GUC.
 * Allocated in TopMemoryContext to ensure it persists across query executions.
 */
extern YbcOtelSpanContext yb_guc_remote_span_ctx;

/*
 * Iterate over databases and execute a given code snippet.
 * Should terminate with YB_FOR_EACH_DB_END.
 */
#define YB_FOR_EACH_DB(pg_db_tuple) \
	{ \
		/* Shared operations shouldn't be used during initdb. */ \
		Assert(!IsBootstrapProcessingMode()); \
		Relation    pg_db      = table_open(DatabaseRelationId, AccessExclusiveLock); \
		HeapTuple   pg_db_tuple; \
		SysScanDesc pg_db_scan = systable_beginscan( \
			pg_db, \
			InvalidOid /* indexId */ , \
			false /* indexOK */ , \
			NULL /* snapshot */ , \
			0 /* nkeys */ , \
			NULL /* key */ ); \
		while (HeapTupleIsValid(pg_db_tuple = systable_getnext(pg_db_scan))) \
		{ \

#define YB_FOR_EACH_DB_END \
		} \
		systable_endscan(pg_db_scan); \
		table_close(pg_db, AccessExclusiveLock); \
	}

/*
 * Given a relation kind, checks whether the relation is supported in YugaByte
 * mode.
 */
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
 * Returns whether a relation is TEMP table
 */
extern bool YbIsTempRelation(Relation relation);

/*
 * Returns true if the relation has temp persistence.
 * Returns false for all other relations, or if they are not found.
 */
extern bool YbIsRangeVarTempRelation(const RangeVar *relation);

/*
 * Returns whether a relation's attribute is a real column in the backing
 * YugaByte table. (It implies we can both read from and write to it).
 */
extern bool IsRealYBColumn(Relation rel, int attrNum);

/*
 * Returns whether a relation's attribute is a YB system column.
 */
extern bool IsYBSystemColumn(int attrNum);

extern AttrNumber YBGetFirstLowInvalidAttributeNumber(Relation relation);

extern AttrNumber YBGetFirstLowInvalidAttributeNumberFromOid(Oid relid);

extern int	YBAttnumToBmsIndex(Relation rel, AttrNumber attnum);

extern AttrNumber YBBmsIndexToAttnum(Relation rel, int idx);

extern int	YBAttnumToBmsIndexWithMinAttr(AttrNumber minattr, AttrNumber attnum);

extern AttrNumber YBBmsIndexToAttnumWithMinAttr(AttrNumber minattr, int idx);

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
 * Return whether a database with oid dbid is a colocated database.
 * legacy_colocated_database is one output parameter. Its value indicates
 * whether database with oid dbid is a legacy colocated database.
 */
extern bool YbIsDatabaseColocated(Oid dbid, bool *legacy_colocated_database);

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
 * Check if upsert (blind write) is unsafe on the given relation.
 * Blind writes skip reading the old row, which means secondary index
 * entries are updated incorrectly, triggers fire incorrectly, and
 * foreign key cascades are skipped.
 */
extern bool YBIsUpsertUnsafeOnRel(Relation relation);

/*
 * Whether to route BEGIN / COMMIT / ROLLBACK to YugaByte's distributed
 * transactions.
 */
extern bool YBTransactionsEnabled();

/*
 * Whether read committed isolation is supported for the cluster or not (via the TServer gflag
 * yb_enable_read_committed_isolation).
 */
extern bool YBIsReadCommittedSupported();

/*
 * Whether the current txn is of READ COMMITTED (or READ UNCOMMITTED) isolation level, and it uses
 * the new READ COMMITTED implementation instead of mapping to REPEATABLE READ level. The latter
 * condition is dictated by the value of gflag yb_enable_read_committed_isolation.
 */
extern bool IsYBReadCommitted();

/*
 * Whether wait-queues are enabled for the cluster or not (via the TServer gflag
 * enable_wait_queues).
 */
extern bool YBIsWaitQueueEnabled();

/*
 * Whether the per database catalog version mode is enabled.
 */
extern bool YBIsDBCatalogVersionMode();

/*
 * Whether the per database logical client version mode is enabled.
 */
extern bool YBIsDBLogicalClientVersionMode();

typedef enum YbObjectLockMode
{
	PG_OBJECT_LOCK_MODE,
	YB_OBJECT_LOCK_DISABLED,
	YB_OBJECT_LOCK_ENABLED
} YbObjectLockMode;
/*
 * Whether object locking is enabled for the cluster (via the TServer regular
 * gflag enable_object_locking_for_table_locks).
 */
extern YbObjectLockMode YBGetObjectLockMode();

/*
 * Whether we need to preload additional catalog tables.
 */
extern bool YbNeedAdditionalCatalogTables();

/* yb_test_catalog_preload_cache_list, implemented in relcache.c. */
extern bool yb_check_test_catalog_preload_cache_list(char **newval, void **extra,
													 GucSource source);
extern void yb_assign_test_catalog_preload_cache_list(const char *newval,
													  void *extra);
extern bool YbCatalogPreloadCacheListIsSet(void);

/*
 * Since DDL metadata in master DocDB and postgres system tables is not modified
 * in an atomic fashion, it is possible that we could have a table existing in
 * postgres metadata but not in DocDB. In the case of a delete it is really
 * problematic, since we can't delete the table nor can we create a new one with
 * the same name. So in this case we just ignore the DocDB 'NotFound' error and
 * delete our metadata.
 */
extern void HandleYBStatusIgnoreNotFound(YbcStatus status, bool *not_found);

/*
 * Handle YBStatus while logging a custom error for DocDB 'NotFound' error.
 */
extern void HandleYBStatusWithCustomErrorForNotFound(YbcStatus status,
													 const char *message_for_not_found);

/*
 * Same as HandleYBStatus but delete the table description first if the
 * status is not ok.
 */
extern void HandleYBTableDescStatus(YbcStatus status, YbcPgTableDesc table);

/*
 * YB initialization that needs to happen when a PostgreSQL backend process
 * is started. Reports errors using ereport.
 */

extern void YBInitPostgresBackend(const char *program_name, const YbcPgInitPostgresInfo *init_info);

/*
 * This should be called on all exit paths from the PostgreSQL backend process.
 * Only main PostgreSQL backend thread is expected to call this.
 */
extern void YBOnPostgresBackendShutdown();

/*
 * Signals PgTxnManager to recreate the transaction. This is used when we need
 * to restart a transaction that failed due to a transaction conflict error.
 */
extern void YBCRecreateTransaction();

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

extern void YBCSetActiveSubTransaction(SubTransactionId id);

extern void YBCRollbackToSubTransaction(SubTransactionId id);

/*
 * Get the type ID of a real or virtual attribute (column).
 * Returns InvalidOid if the attribute number is invalid.
 */
extern Oid	GetTypeId(int attrNum, TupleDesc tupleDesc);

/*
 * Return a string representation of the given type id, or say it is unknown.
 * What is returned is always a static C string constant.
 */
extern const char *YBPgTypeOidToStr(Oid type_id);

/*
 * Return a string representation of the given YbcPgDataType, or say it is unknown.
 * What is returned is always a static C string constant.
 */
extern const char *YBCPgDataTypeToStr(YbcPgDataType yb_type);

/*
 * Report an error saying the given type as not supported by YugaByte.
 */
extern void YBReportTypeNotSupported(Oid type_id);

/*
 * Log whether or not YugaByte is enabled.
 */
extern void YBReportIfYugaByteEnabled();

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
bool		YBShouldRestartAllChildrenIfOneCrashes();

/*
 * These functions help indicating if we are connected to template0 or template1.
 */
void		YbSetConnectedToTemplateDb();
bool		YbIsConnectedToTemplateDb();

/*
 * Get the database name for a relation id (accounts for system databases and
 * shared relations)
 */
const char *YBCGetDatabaseName(Oid relid);

/*
 * Get the schema name for a schema oid (accounts for system namespaces)
 */
const char *YBCGetSchemaName(Oid schemaoid);

/*
 * Get the real database id of a relation. For shared relations
 * (which are meant to be accessible from all databases), it will be template1.
 * Note that relations in yb_system database are also meant to be accessible by
 * all databases. Naturally, for these relations, it will be yb_system.
 */
Oid			YBCGetDatabaseOid(Relation rel);
Oid			YBCGetDatabaseOidByRelid(Oid relid);
extern Oid	YBCGetDatabaseOidFromShared(bool relisshared, bool belongs_to_yb_system_db);

/*
 * Raise an unsupported feature error with the given message and
 * linking to the referenced issue (if any).
 */
void		YBRaiseNotSupportedSignal(const char *msg, int issue_no, int signal_level);

/*
 * Return the value of (base ^ exponent) bounded by the upper limit.
 */
extern double PowerWithUpperLimit(double base, int exponent, double upper_limit);

/*
 * Return whether to use wholerow junk attribute for YB relations.
 *
 * For a leaf in an inheritance/partition hierarchy, the answer also depends
 * on transition-table triggers on the root.  Callers that already have the
 * root open should pass it as root_relation; otherwise pass NULL (and open
 * the root themselves if the no-root answer is insufficient).  This function
 * never opens or closes a relation.
 */
extern bool YbWholeRowAttrRequired(Relation relation, Relation root_relation,
								   CmdType operation);

extern Oid YbSystemDbOid();

/* ------------------------------------------------------------------------------ */
/* YB GUC variables. */

/**
 * YSQL guc variables that can be used to toggle yugabyte features.
 * See also the corresponding entries in guc.c.
 */

/* Enables tables/indexes to be created WITH (table_oid = x). */
extern bool yb_enable_create_with_table_oid;

/*
 * During CREATE INDEX, the delay between stages, from
 * - indislive=true to indisready=true
 * - indisready=true to launching backfill
 */
extern int	yb_index_state_flags_update_delay;

/*
 * Enables expression pushdown.
 * If true, planner sends supported expressions to DocDB for evaluation
 */
extern bool yb_enable_expression_pushdown;

/* Enables pushing INSERT ON CONFLICT DO NOTHING resolution to DocDB. */
extern bool yb_enable_ioc_do_nothing_pushdown;

/*
 * Enables distinct pushdown.
 * If true, send supported DISTINCT operations to DocDB
 */
extern bool yb_enable_distinct_pushdown;

/*
 * Enables index aggregate pushdown (IndexScan only, not IndexOnlyScan).
 * If true, request aggregated results from DocDB when possible.
 */
extern bool yb_enable_index_aggregate_pushdown;

/*
 * If true, secondary indexes are extended with decodable PK columns, enabling
 * Index Only Scans that decode PK values from ybidxbasectid without reading
 * the base table.
 */
extern bool yb_enable_primary_key_decode_from_index;

/*
 * YSQL guc variable that is used to enable the use of Postgres's selectivity
 * functions and YSQL table statistics.
 * e.g. 'SET yb_enable_optimizer_statistics = true'
 * See also the corresponding entries in guc.c.
 */
extern bool yb_enable_optimizer_statistics;

/*
 * Enables nonbreaking DDL mode in which a DDL statement is not considered as
 * a "breaking catalog change" and therefore will not cause running transactions
 * to abort.
 */
extern bool yb_make_next_ddl_statement_nonbreaking;

/*
 * Enables nonincrementing DDL mode in which a DDL statement is considered as a
 * "same version DDL" and therefore will not cause catalog version to increment.
 */
extern bool yb_make_next_ddl_statement_nonincrementing;

/*
 * Allows capability to disable prefetching in a PLPGSQL FOR loop over a query.
 * This is introduced for some test(s) with lazy evaluation in READ COMMITTED
 * isolation that require the read rpcs to be issued over multiple invocations
 * of the lazily evaluable function. If prefetching is enabled, the first
 * invocation could possibly issue read rpcs to all tablets until the
 * specified number of rows is prefetched -- in which case no read rpcs would be
 * issued in later invocations.
 */
extern bool yb_plpgsql_disable_prefetch_in_for_query;

/*
 * Allow nextval() to fetch the value range and advance the sequence value in a
 * single operation.
 * If disabled, nextval() reads sequence value first, advances it and apply the
 * new value, which may fail due to concurrent modification and has to be
 * retried.
 */
extern bool yb_enable_sequence_pushdown;

/*
 * Disable waiting for backends to have up-to-date catalog version.
 */
extern bool yb_disable_wait_for_backends_catalog_version;

/*
 * Enables YB cost model for Sequential and Index scans
 */
extern bool yb_enable_base_scans_cost_model;

/*
 * When enabled, the planner warms the catalog cache with all of a relation's
 * pg_statistic rows in a single batched RPC (instead of one point lookup per
 * column) the first time the relation is planned in a backend.
 */
extern bool yb_prefetch_column_statistics;

/*
 * Enables update of reltuples in pg_class for the base table and index after
 * creating the index.
 */
extern bool yb_enable_update_reltuples_after_create_index;

/*
 * Enables the following index backfill scan optimizations:
 * - column projection (reads only the columns needed for the index)
 * - predicate pushdown for partial indexes (pushes the predicate to DocDB)
 * If true, index build/backfill reads only the columns needed for the index
 * (column projection) and pushes partial index predicates down to the base table
 * scan (predicate pushdown), reducing read RPCs.
 */
extern bool yb_enable_index_backfill_scan_optimization;

/*
 * Total timeout for waiting for backends to have up-to-date catalog version.
 */
extern int	yb_wait_for_backends_catalog_version_timeout;

/*
 * If true, we will always prefer batched nested loop join plans over nested
 * loop join plans.
 */
extern bool yb_prefer_bnl;

/*
 * If true, all fields that vary from run to run are hidden from the
 * output of EXPLAIN.
 */
extern bool yb_explain_hide_non_deterministic_fields;

/*
 * Enables scalar array operation pushdown.
 * If true, planner sends supported expressions to DocDB for evaluation
 */
extern bool yb_enable_saop_pushdown;

/*
 * Enables the use of TOAST compression for the Postgres catcache.
 */
extern int	yb_toast_catcache_threshold;

/*
 * Maximum number of tuples in a preloaded catalog cache for which a
 * list-cache miss can be answered by scanning the local cache instead of
 * doing an RPC.  Set to 0 to disable the optimization entirely.
 */
extern int	yb_catcache_list_from_preloaded_limit;

/*
 * Configure size of the parallel range in requests for parallel keys.
 */
extern int	yb_parallel_range_size;

/*
 * Disables parallel query for the SELECT planned by DDLs (CREATE TABLE AS,
 * SELECT INTO, CREATE/REFRESH MATERIALIZED VIEW, COPY (query) TO, and
 * EXPLAIN [ANALYZE] CREATE TABLE AS). Enabled by default because parallel
 * query in these DDLs has not been QA tested in YugabyteDB; set to off as an
 * escape hatch to restore upstream PostgreSQL behavior.
 */
extern bool yb_disable_parallel_query_in_ddl;

/*
 * INSERT ON CONFLICT batching read batch size.
 */
extern int	yb_insert_on_conflict_read_batch_size;

/*
 * Enable preloading of foreign key information into the relation cache.
 */
extern bool yb_enable_fkey_catcache;

/*
 * Enable batched DocDB lookup for foreign key constraint check when types
 * mismatch. The batch size is controlled by ysql_session_max_batch_size.
 */
extern bool yb_enable_fkey_batched_docdb_lookup_when_types_mismatch;

/*
 * Enable the nop alter role statement optimization.
 */
extern bool yb_enable_nop_alter_role_optimization;

/*
 * Compatibility option to ignore FREEZE with COPY FROM.
 */
extern bool yb_ignore_freeze_with_copy;

/* ------------------------------------------------------------------------------ */
/* GUC variables needed by YB via their YB pointers. */
extern int	StatementTimeout;


/*
 * Enable the skip intents write optimization.
 */
extern bool yb_enable_new_relation_fastpath_write;

/*
 * Enable the skip intents write optimization in transaction blocks.
 */
extern bool yb_enable_new_relation_fastpath_write_in_txn_blocks;

/* ------------------------------------------------------------------------------ */
/* YB Debug utils. */

/**
 * YSQL guc variables that can be used to toggle yugabyte debug features.
 * e.g. 'SET yb_debug_log_docdb_error_backtrace=true' and
 *      'RESET yb_debug_log_docdb_error_backtrace'.
 * See also the corresponding entries in guc.c.
 */

/* Add stacktrace information to errors received from DocDB/PgGate. */
extern bool yb_debug_log_docdb_error_backtrace;

/* Use Postgres or Yugabyte stacktrace formatting. */
extern bool yb_debug_original_backtrace_format;

/*
 * Log automatic statement (or transaction) restarts such as read-restarts and
 * schema-version restarts (e.g. catalog version mismatch errors).
 */
extern bool yb_debug_log_internal_restarts;

/*
 * Tracks whether a non-atomic (in-procedure) COMMIT has been executed during
 * the current top-level query. This is used to prevent unsafe retries of
 * CALL/DO statements: once a COMMIT has been performed inside a stored
 * procedure or DO block, retrying the entire statement from scratch would
 * re-execute already-committed work, potentially causing duplicates or other
 * incorrect behavior.
 *
 * Set to true in _SPI_commit() and reset at the start of each top-level query
 * in yb_exec_query_wrapper().
 */
extern bool yb_is_non_atomic_commit_done;

/*
 * When true, allows the query layer to retry CALL/DO statements even after a
 * non-atomic (in-procedure) COMMIT has been executed. This can lead to
 * re-execution of already-committed work, but is provided as a safety valve
 * for customers who depend on the old retry behavior.
 *
 * Default: false (safe behavior -- retries are blocked after in-procedure
 * COMMIT).
 */
extern bool yb_enable_retry_after_non_atomic_commit;

extern char *yb_extra_commands_to_retry_string;
extern bool *yb_extra_commands_to_retry;

extern char *yb_extra_commands_to_retry_in_proc_string;
extern bool *yb_extra_commands_to_retry_in_proc;

/*
 * Relaxes some internal sanity checks for system catalogs to allow creating them.
 */
extern bool yb_test_system_catalogs_creation;

/*
 * Sleep before executing a statement.
 * Can be used to simulate race conditions where catalog is updated between
 * planning and execution.
 */
extern int yb_test_sleep_before_executor_start_ms;

/*
 * If set to non-zero, next DDL operation will fail with the specified error level:
 * 0 = disabled (default), 1 = ERROR, 2 = FATAL, 3 = PANIC, 4 = crash.
 * Resets to 0 after triggering.
 */
extern int yb_test_fail_next_ddl;
extern bool yb_test_fail_stale_catalog_message_check;
extern bool yb_enable_stale_catalog_prefetch;

/* Test fault injection: fail drop after heap_drop_with_catalog. */
extern bool yb_test_fail_drop_after_heap_drop;

/*
 * If set to true,the next DDL will update the catalog in force mode which
 * allows it to operate even during ysql major catalog upgrades.
 */
extern bool yb_force_catalog_update_on_next_ddl;

/* If set to true, all drop commands will fail. */
extern bool yb_test_fail_all_drops;

/*
 * If set to true, a manual ANALYZE does not reset the auto-analyze mutation
 * counters, reverting to the pre-reset behavior. Test only.
 */
extern bool yb_test_analyze_dont_reset_mutations;

/*
 * If set to true, force invalidation of every base relation's index relcache
 * entries between add_base_rels_to_query() and make_one_rel() in
 * query_planner().  Used by tests to deterministically expose lazy-loading
 * bugs where planner code reads relcache fields (e.g. rd_indexprs) that get
 * reset by a mid-planning invalidation.
 */
extern bool yb_test_invalidate_relcache_in_planner;

/*
 * If set to true, next increment catalog version operation will fail and
 * reset this back to false.
 */
extern bool yb_test_fail_next_inc_catalog_version;

/*
 * This number times disable_cost is added to the cost for some unsupported
 * ybgin index scans.
 */
extern double yb_test_ybgin_disable_cost_factor;

/*
 * Block the given index creation phase.
 * - "indisready": index state change to indisready
 *   (not supported for non-concurrent)
 * - "backfill": index backfill phase
 * - "postbackfill": post-backfill operations like validation and event triggers
 */
extern char *yb_test_block_index_phase;

/*
 * Same as above, but fails the operation at the given stage instead of
 * blocking.
 */
extern char *yb_test_fail_index_state_change;

/*
 * GUC variable that specifies default replica identity for tables at the time of creation.
 */
extern char *yb_default_replica_identity;

/*
 * GUC variable to pass a W3C traceparent for distributed tracing.
 */
extern char *yb_dist_tracecontext;

/*
 * Fraction (0 to 1) of protocol cycles without a traceparent that start a
 * distributed trace of their own; 0 disables sampling.
 */
extern double yb_dist_trace_sample_rate;

/*
 * If set to true, any DDLs that rewrite tables/indexes will fail after
 * the new table is created.
 */
extern bool yb_test_fail_table_rewrite_after_creation;

/*
 * If set to true, force a full catalog cache refresh before
 * executing the next top level statement.
 */
extern bool yb_test_preload_catalog_tables;

/*
 * If set to true, any DDLs that rewrite tables/indexes will not drop the
 * old relfilenode/DocDB table.
 */
extern bool yb_test_table_rewrite_keep_old_table;

/*
 * If set to true, inject code to make psql output stable across linux and mac.
 */
extern bool yb_test_collation;

/*
 * If set to true, fill padding bytes with zeros when creating a shared
 * invalidation message.
 */
extern bool yb_test_inval_message_portability;

/*
 * If > 0, add a delay after apply invalidation messages.
 */
extern int	yb_test_delay_after_applying_inval_message_ms;

/*
 * If > 0, add a delay before calling YBCPgSetTserverCatalogMessageList.
 */
extern int	yb_test_delay_set_local_tserver_inval_message_ms;

/*
 * If > 0, sleep for this many ms before committing the DDL
 */
extern double yb_test_delay_next_ddl;

/*
 * If > 0, then puts a hard limit on the number of retries.
 */
extern int	yb_test_reset_retry_counts;

/*
 * If set to true, the walsender sends a keepalive after every decoded record.
 */
extern bool yb_test_walsender_keepalive_after_each_record;

/*
 * Denotes whether DDL operations touching DocDB system catalog will be rolled
 * back upon failure. These two GUC variables are used together. See comments
 * for the gflag --ysql_enable_ddl_atomicity_infra in common_flags.cc.
*/
extern bool yb_enable_ddl_atomicity_infra;

/* Enable shared replication origin write tagging. */
extern bool yb_enable_replication_origin_shared;

/*
 * Allow to return to the client SQL status codes defined by YugabyteDB (YBxxx).
 * Those codes are used internally to determine if transparent retry is
 * possible. If disabled, they are replaced with similar Postgres defined codes.
 */
extern bool yb_enable_extended_sql_codes;

extern bool yb_ddl_rollback_enabled;
static inline bool
YbDdlRollbackEnabled()
{
	return yb_enable_ddl_atomicity_infra && yb_ddl_rollback_enabled;
}

extern bool yb_use_hash_splitting_by_default;

/*
 * If set to true, non-key columns of secondary indexes are updated in-place
 * when no key columns are modified.
 */
extern bool yb_enable_inplace_index_update;

/*
 * Enable the advisory lock feature. (DEPRECATED)
 */
extern bool yb_enable_advisory_locks;

/*
 * Enable invalidation messages.
 */
extern bool yb_enable_invalidation_messages;
extern bool yb_enable_invalidate_table_cache_entry;
extern int	yb_invalidation_message_expiration_secs;
extern int	yb_max_num_invalidation_messages;

extern int	yb_ddl_wait_for_master_prefetch_drain_ms;

/*
 * Mirrors YsqlCatalogPrefetchLoadPB in master_heartbeat.proto, naming every value it can carry
 * even where the code only compares against some of them. The master sends a value in the
 * heartbeat response, the tserver stores it in this node's shared memory, and backends read it
 * from there.
 */
typedef enum YbCatalogPrefetchLoad
{
	YB_CATALOG_PREFETCH_LOAD_UNKNOWN = 0,
	YB_CATALOG_PREFETCH_LOAD_LOW = 1,
	YB_CATALOG_PREFETCH_LOAD_BUSY = 2,
	YB_CATALOG_PREFETCH_LOAD_SUPER_BUSY = 3,
} YbCatalogPrefetchLoad;

extern void YbWaitForMasterCatalogPrefetchDrain(void);

/*
 * Enable parallel query for different relation sharding types
 */
extern bool	yb_enable_parallel_scan_colocated;
extern bool	yb_enable_parallel_scan_hash_sharded;
extern bool	yb_enable_parallel_scan_range_sharded;
extern bool	yb_enable_parallel_scan_system;

/*
 * Test-only GUC for exercising parallel plan code paths.
 */
typedef enum YbForceParallel
{
	YB_FORCE_PARALLEL_OFF = 0,
	YB_FORCE_PARALLEL_PREFER = 1,
	YB_FORCE_PARALLEL_FORCE = 2,
} YbForceParallel;

extern int	yb_test_force_parallel;

/*
 * If set to true, all DDL statements will cause the catalog version to increment.
 */
extern bool yb_test_make_all_ddl_statements_incrementing;

/*
 * If set to true, all DDL statements will cause the catalog version to increment.
 * Unlike yb_test_make_all_ddl_statements_incrementing, this controls ONLY the
 * version incrementing behavior.
 */
extern bool yb_always_increment_catalog_version_on_ddl;

/*
 * If set to true, negative catcache entries are enabled. A negative cache entry
 * is created when a lookup returns no result. Unlike
 * yb_test_make_all_ddl_statements_incrementing, this controls ONLY the negative
 * caching behavior.
 */
extern bool yb_enable_negative_catcache_entries;

typedef struct YBUpdateOptimizationOptions
{
	bool		has_infra;
	bool		is_enabled;
	int			num_cols_to_compare;
	int			max_cols_size_to_compare;
} YBUpdateOptimizationOptions;

/* GUC variables to control the behavior of optimizing update queries. */
extern YBUpdateOptimizationOptions yb_update_optimization_options;

/* GUC variables to control the speculative executive of PL statements. */
extern bool yb_speculatively_execute_pl_statements;
extern bool yb_whitelist_extra_stmts_for_pl_speculative_execution;

extern bool yb_enable_docdb_vector_type;

/*
 * Deprecated no-op; kept so existing postgresql.conf / init scripts that set this
 * parameter still load. Advisory locking ignores this variable (see lockfuncs.c).
 */
extern bool yb_silence_advisory_locks_not_supported_error;

/*
 * GUC to indicate DDL executed in a Automatic xCluster mode target universe.
 */
extern bool yb_xcluster_automatic_mode_target_ddl;

extern bool yb_enable_xcluster_analyze_replication;

extern bool yb_user_ddls_preempt_auto_analyze;

/*
 * If true, enable RPC execution time stats for pg_stat_statements.
 */
extern bool yb_enable_pg_stat_statements_rpc_stats;

extern bool yb_enable_global_views;

/*
 * If true, enable DocDB metrics collection for pg_stat_statements.
 */
extern bool yb_enable_pg_stat_statements_docdb_metrics;

/*
 * See also ybc_util.h which contains additional such variable declarations for
 * variables that are (also) used in the pggate layer.
 * Currently: yb_debug_log_docdb_requests.
 */

/*
 * Get a string representation of a datum (given its type).
 */
extern const char *YBDatumToString(Datum datum, Oid typid);

/*
 * Get a string representation of a tuple (row) given its tuple description
 * (schema) and is_omitted values.
 *
 * This function also logs the is_omitted values which indicates attributes
 * which were omitted due to the value of the replica identity.
 */
extern const char *YbHeapTupleToStringWithIsOmitted(HeapTuple tuple,
													TupleDesc tupleDesc,
													bool *is_omitted);

/* Same as above except it takes slot instead of tuple. */
extern const char *YbSlotToString(TupleTableSlot *slot);

extern const char *YbSlotToStringWithIsOmitted(TupleTableSlot *slot,
											   bool *is_omitted);

/*
 * Checks if the master thinks initdb has already been done.
 */
bool		YBIsInitDbAlreadyDone();

extern bool YBIsDdlTransactionBlockEnabled();
extern int	YBGetDdlNestingLevel();
extern NodeTag YBGetCurrentStmtDdlNodeTag();
extern bool YBIsCurrentStmtDdl();
extern bool YBCurrentStmtCannotIncrementCatalogVersion();
extern CommandTag YBGetCurrentStmtDdlCommandTag();
extern CommandTag YBGetTopLevelStmtDdlCommandTag();
extern CommandTag YBGetGlobalDdlCommandTag();
extern CommandTag YBGetBreakingDdlCommandTag();
extern bool YBGetDdlUseRegularTransactionBlock();

/*
 * Snapshot of the current top-level DDL statement identity. Used to preserve
 * and restore state across intermediate commits (YbCommitTransactionCommandIntermediate).
 */
typedef struct YbDdlOriginalStmtState
{
	NodeTag		node_tag;
	CommandTag	command_tag;
	bool		is_top_level_ddl_active;
} YbDdlOriginalStmtState;

extern void YBGetDdlOriginalStmtState(YbDdlOriginalStmtState *state);
extern void YBSetDdlOriginalStmtState(const YbDdlOriginalStmtState *state);
extern void YbSetIsGlobalDDL();
extern void YbIncrementPgTxnsCommitted();
extern bool YbTrackPgTxnInvalMessagesForAnalyze();
extern void YbCheckNewLocalCatalogVersionOptimization();
extern void YbTrackAlteredTableId(Oid relid);
extern void YbInvalidateTableCacheForAlteredTables();


typedef enum YbSysCatalogModificationAspect
{
	YB_SYS_CAT_MOD_ASPECT_ALTERING_EXISTING_DATA = 1,
	YB_SYS_CAT_MOD_ASPECT_VERSION_INCREMENT = 2,
	YB_SYS_CAT_MOD_ASPECT_BREAKING_CHANGE = 4,
} YbSysCatalogModificationAspect;

typedef enum YbDdlMode
{
	YB_DDL_MODE_NO_ALTERING = 0,

	YB_DDL_MODE_SILENT_ALTERING = YB_SYS_CAT_MOD_ASPECT_ALTERING_EXISTING_DATA,

	YB_DDL_MODE_VERSION_INCREMENT = (YB_SYS_CAT_MOD_ASPECT_ALTERING_EXISTING_DATA |
									 YB_SYS_CAT_MOD_ASPECT_VERSION_INCREMENT),

	YB_DDL_MODE_BREAKING_CHANGE = (YB_SYS_CAT_MOD_ASPECT_ALTERING_EXISTING_DATA |
								   YB_SYS_CAT_MOD_ASPECT_VERSION_INCREMENT |
								   YB_SYS_CAT_MOD_ASPECT_BREAKING_CHANGE),
} YbDdlMode;

void		YBIncrementDdlNestingLevel(YbDdlMode mode);
void		YBDecrementDdlNestingLevel();

extern void YBAddDdlTxnState(YbDdlMode mode);
extern void YBMergeDdlTxnState();
extern void YBCommitTransactionContainingDDL();

typedef struct YbDdlModeOptional
{
	bool		has_value;
	YbDdlMode	value;
} YbDdlModeOptional;

extern YbDdlMode YBGetCurrentDdlMode();
extern YbDdlModeOptional YbGetDdlMode(PlannedStmt *pstmt,
									  ProcessUtilityContext context,
									  bool *requires_autonomous_transaction);
void		YBAddModificationAspects(YbDdlMode mode);

extern void YBBeginOperationsBuffering();
extern void YBEndOperationsBuffering();
extern void YBResetOperationsBuffering();
extern void YBFlushBufferedOperations(YbcFlushDebugContext debug_context);
extern void YBAdjustOperationsBuffering(int multiple);

struct QueryDesc;
/* Called at the start of every ExecutorRun and ExecutorFinish. */
extern void YBOnExecutorOperationBegin();
/*
 * Called at the end of every ExecutorRun and ExecutorFinish. At the top
 * level, saves DocDB stats onto queryDesc for pg_stat_statements.
 */
extern void YBOnExecutorOperationEnd(struct QueryDesc *queryDesc);
/* True for the outermost ExecutorRun/ExecutorFinish. */
extern bool YBIsTopLevelExecutorOperation();

/* Called at the start of every tracked ProcessUtility call. */
extern void YBOnUtilityOperationBegin();
/*
 * Called at the end of every tracked ProcessUtility call. At the top
 * level, saves DocDB stats into the YBGetUtilityOperationStats() slot.
 */
extern void YBOnUtilityOperationEnd();
/* DocDB stats from the last top-level utility (for pg_stat_statements). */
extern YbInstrumentation *YBGetUtilityOperationStats();

/* Resets executor/utility nesting counters and stats after a failed transaction. */
extern void YBResetOperationTracking();

bool		YBEnableTracing();
bool		YBReadFromFollowersEnabled();
int32_t		YBFollowerReadStalenessMs();
bool		YBFollowerReadsBehaviorBefore20482();

/*
 * Allocates YbcPgYBTupleIdDescriptor with nattrs arguments by using palloc.
 * Resulted object can be released with pfree.
 */
YbcPgYBTupleIdDescriptor *YBCCreateYBTupleIdDescriptor(Oid db_oid, Oid table_relfilenode_oid,
													   int nattrs);
void		YBCFillUniqueIndexNullAttribute(YbcPgYBTupleIdDescriptor *descr);

/*
 * Lazily loads yb_table_properties field in Relation.
 *
 * YbGetTableProperties expects the table to be present in the DocDB, while
 * YbTryGetTableProperties queries the DocDB first and returns NULL if not found.
 *
 * Both calls returns the same yb_table_properties field from Relation
 * for convenience (can be NULL for the second call).
 *
 * Note that these calls will rarely send out RPC because of
 * Relation/TableDesc cache.
 *
 * TODO(alex):
 *    An optimization we could use is to amend RelationBuildDesc or
 *    ScanPgRelation to do a custom RPC fetching YB properties as well.
 *    However, TableDesc cache makes this low-priority.
 */
YbcTableProperties YbGetTableProperties(Relation rel);
YbcTableProperties YbGetTablePropertiesById(Oid relid);
YbcTableProperties YbTryGetTableProperties(Relation rel);

typedef enum YbTableDistribution
{
	YB_SYSTEM,
	YB_COLOCATED,
	YB_HASH_SHARDED,
	YB_RANGE_SHARDED
} YbTableDistribution;
extern YbTableDistribution YbGetTableDistribution(Relation rel);
extern YbTableDistribution YbGetTableDistributionById(Oid relid);

/*
 * Check whether the given libc locale is supported in YugaByte mode.
 */
extern bool YBIsSupportedLibcLocale(const char *localebuf);

/*
 * Check for unsupported libc locale in YugaByte mode.
 */
extern void YbCheckUnsupportedLibcLocale(const char *localebuf);

/* Spin wait while test guc var actual equals expected. */
extern void YbTestGucBlockWhileStrEqual(char **actual, const char *expected,
										const char *msg);

/* Spin wait until test guc var actual equals expected. */
extern void YbTestGucBlockWhileIntNotEqual(int *actual, int expected,
										   const char *msg);

extern void YbTestGucFailIfStrEqual(char *actual, const char *expected);

extern int	YbGetNumberOfFunctionOutputColumns(Oid func_oid);
extern int  YbGetNumberOfFunctionInputParameters(Oid func_oid);

char	   *YBDetailSorted(char *input);

/*
 * For given collation, type and value, setup collation info.
 */
extern void YBGetCollationInfo(Oid collation_id,
							   const YbcPgTypeEntity *type_entity, Datum datum,
							   bool is_null,
							   YbcPgCollationInfo *collation_info);

/*
 * Setup collation info in attr.
 */
extern void YBSetupAttrCollationInfo(YbcPgAttrValueDescriptor *attr, const YbcPgColumnInfo *column_info);

/*
 * Check whether the collation is a valid non-C collation.
 */
extern bool YBIsCollationValidNonC(Oid collation_id);

/*
 * Check whether the DB collation is UTF-8.
 */
extern bool YBIsDbLocaleDefault();

extern bool YBRequiresCacheToCheckLocale(Oid collation_id);

/*
 * For the column 'attr_num' and its collation id, return the collation id that
 * will be used to do collation encoding. For example, if the column 'attr_num'
 * represents a non-key column, we do not need to store the collation key and
 * this function will return InvalidOid which will disable collation encoding
 * for the column string value.
 */
Oid			YBEncodingCollation(YbcPgStatement handle, int attr_num, Oid attcollation);

/*
 * Check whether the user ID is of a user who has the yb_extension role.
 */
bool		IsYbExtensionUser(Oid member);

/*
 * Check whether the user ID is of a user who has the yb_fdw role.
 */
bool		IsYbFdwUser(Oid member);

/*
 * These functions are unsafe to run in a multi-threaded environment. There is
 * no specific attribute that identifies them as such, so we have to manually
 * identify them.
 */
extern const uint32 yb_funcs_unsafe_for_pushdown[];

/*
 * These functions are some of the more commonly used functions, and are less
 * likely to cause issues when run in mixed mode pushdown. This list is not
 * exhaustive, but gives us a useful starting point.
 */
extern const uint32 yb_funcs_safe_for_mixed_mode_pushdown[];

/*
 * List of functions that are safe to push down, but need to be evaluated
 * to a constant value before being sent to DocDB.
 */
extern const uint32 yb_pushdown_funcs_to_constify[];

/*
 * List of SQL standard time/date functions that are evaluated once per
 * statement and pushed down as constants.
 */
extern const SQLValueFunctionOp yb_pushdown_sqlvaluefunctions[];

/*
 * Number of functions in the lists above.
 */
extern const int yb_funcs_unsafe_for_pushdown_count;
extern const int yb_funcs_safe_for_mixed_mode_pushdown_count;
extern const int yb_pushdown_funcs_to_constify_count;
extern const int yb_pushdown_sqlvaluefunctions_count;

/**
 * Given a relation, return it's relfilenode OID. In YB, the relfilenode OID
 * maps to the relation's DocDB table ID. Note: if the table has not
 * previously been rewritten, this function returns the OID of the table.
 */
Oid			YbGetRelfileNodeId(Relation relation);

/**
 * Given a relation ID, return the relation's relfilenode OID.
 */
Oid			YbGetRelfileNodeIdFromRelId(Oid relationId);

/*
 * Check whether the user ID is of a user who has the yb_db_admin role.
 */
bool		IsYbDbAdminUser(Oid member);

/*
 * Check whether the user ID is of a user who has the yb_db_admin role
 * (excluding superusers).
 */
bool		IsYbDbAdminUserNosuper(Oid member);

/*
 * Check unsupported system columns and report error.
 */
void		YbCheckUnsupportedSystemColumns(int attnum, const char *colname, RangeTblEntry *rte);

/*
 * Register system table for prefetching.
 */
void		YbRegisterSysTableForPrefetching(int sys_table_id);
void		YbTryRegisterCatalogVersionTableForPrefetching();
extern void	YbTryRegisterLogicalClientVersionTableForPrefetching();

YbcPgTableLocalityInfo
YbBuildTableLocalityInfo(Relation rel);

YbcPgTableLocalityInfo
YbBuildSystemTableLocalityInfo(Oid sys_rel_oid);

/*
 * Return NULL for all non-range-partitioned tables.
 * Return an empty string for one-tablet range-partitioned tables.
 * Return SPLIT AT VALUES clause string (i.e. SPLIT AT VALUES(...))
 * for all range-partitioned tables with more than one tablet.
 * Return an empty string when duplicate split points exist
 * after tablet splitting.
 * Return an emptry string when a NULL value is present in split points
 * after tablet splitting.
 */
extern Datum yb_get_range_split_clause(PG_FUNCTION_ARGS);

extern bool check_yb_xcluster_consistency_level(char **newval, void **extra,
												GucSource source);
extern void assign_yb_xcluster_consistency_level(const char *newval,
												 void *extra);

/*
 * Updates the session stats snapshot with the collected stats and copies the
 * difference to the query execution context's instrumentation.
 */
void		YbUpdateSessionStats(YbInstrumentation *yb_instr);

extern bool check_yb_read_time(char **newval, void **extra, GucSource source);
extern void assign_yb_read_time(const char *newval, void *extra);
extern bool check_yb_read_time_skip_catalog(bool *newval, void **extra,
											GucSource source);
extern void assign_yb_read_time_skip_catalog(bool newval, void *extra);

extern uint64_t yb_origination_time;
extern void YbRefreshOriginationTime(void);
extern bool yb_check_origination_time_override(char **newval, void **extra,
											   GucSource source);
extern void yb_assign_origination_time_override(const char *newval,
												void *extra);

/* GUC assign hook for max_replication_slots */
extern void yb_assign_max_replication_slots(int newval, void *extra);

/*
 * Refreshes the session stats snapshot with the collected stats. This function
 * is to be invoked before the query has started its execution.
 */
extern void YbRefreshSessionStatsBeforeExecution();

/*
 * Refreshes the session stats snapshot with the collected stats. This function
 * is to be invoked when during/after query execution.
 */
extern void YbRefreshSessionStatsDuringExecution();

/*
 * Updates the global flag indicating whether RPC requests to the underlying
 * storage layer need to be timed.
 */
extern void YbToggleSessionStatsTimer(bool timing_on);

/* Indicates whether timing of RPC requests is enabled. */
extern bool YbIsSessionStatsTimerEnabled();

/*
 * Updates the global flag indicating whether stats need to be collected at the
 * time of transaction commit for EXPLAIN.
 */
extern void YbToggleCommitStatsCollection(bool enable);

/* Indicates whether commit stats collection is enabled. */
extern bool YbIsCommitStatsCollectionEnabled();

/*
 * Records the latency of the last transaction commit operation in a global
 * variable.
 */
extern void YbRecordCommitLatency(uint64_t latency_us);

/*
 * Returns the running total of main table rows scanned by DocDB for this
 * session.  Unlike the number of rows returned to the query layer, this
 * includes rows that DocDB filtered out while evaluating a pushed down
 * expression.
 */
extern uint64_t YbGetTableRowsScanned();

/**
 * Update the global flag indicating what metric changes to capture and return
 * from the tserver to PG.
 */
void		YbSetMetricsCaptureType(YbcPgMetricsCaptureType metrics_capture);

/*
 * Update the global flag indicating what metric changes to capture and return
 * from the tserver to PG only if the flag is YB_YQL_METRICS_CAPTURE_NONE.
 */
 extern void YbSetMetricsCaptureTypeIfUnset(YbcPgMetricsCaptureType metrics_capture);

/*
 * If the tserver gflag --ysql_disable_server_file_access is set to
 * true, then prevent any server file writes/reads/execution.
 */
extern void YBCheckServerAccessIsAllowed();

void		YbSetCatalogCacheVersion(YbcPgStatement handle, uint64_t version);

uint64_t	YbGetSharedCatalogVersion();
uint32_t	YbGetNumberOfDatabases();

/*
 * This function maps the user intended row-level lock policy i.e., "pg_wait_policy" of
 * type enum LockWaitPolicy to the "docdb_wait_policy" of type enum WaitPolicy as defined in
 * common.proto.
 * Note: enum WaitPolicy values are equal to enum LockWaitPolicy.
 *       That is why function maps enum LockWaitPolicy into enum LockWaitPolicy.
 *
 * The semantics of the WaitPolicy enum differ slightly from those of the traditional LockWaitPolicy
 * in Postgres, as explained in common.proto. This is for historical reasons. WaitPolicy in
 * common.proto was created as a copy of LockWaitPolicy to be passed to the Tserver to help in
 * appropriate conflict-resolution steps for the different row-level lock policies.
 *
 * In isolation level SERIALIZABLE, this function returns WAIT_BLOCK as
 * this is the only policy currently supported for SERIALIZABLE.
 *
 * However, if wait queues aren't enabled in the following cases:
 *  * Isolation level SERIALIZABLE
 *  * The user requested LockWaitBlock in another isolation level
 * this function returns WAIT_ERROR (which actually uses the "Fail on Conflict"
 * conflict management policy instead of "no wait" semantics, as explained in "enum WaitPolicy" in
 * common.proto).
 *
 * Logs a warning:
 * 1. In isolation level SERIALIZABLE for a pg_wait_policy of LockWaitSkip and LockWaitError
 *    because SKIP LOCKED and NOWAIT are not supported yet.
 * 2. In isolation level REPEATABLE READ for a pg_wait_policy of LockWaitError because NOWAIT
 *    is not supported.
 */
LockWaitPolicy YBGetDocDBWaitPolicy(LockWaitPolicy pg_wait_policy);

const char *yb_fetch_current_transaction_priority(void);

bool		YbIsBatchedExecution();
void		YbSetIsBatchedExecution(bool value);

/* Check if the given column is a part of the relation's key. */
bool		YbIsColumnPartOfKey(Relation rel, const char *column_name);

/* Get a relation's split options. */
YbOptSplit *YbGetSplitOptions(Relation rel);

/*
 * Convert split_points List to a string representation.
 * The format is "((val1, val2), (val3, val4), ...)" where each inner
 * parenthesized list is a split point.
 * Returns NULL if split_points is NIL.
 */
char	   *YbSplitPointsToString(List *split_points);

/*
 * Parse a yb_presplit reloption string back to a YbOptSplit structure.
 * The format can be:
 *   - A number (e.g., "5") for SPLIT INTO N TABLETS
 *   - Split points (e.g., "((100),(200))") for SPLIT AT VALUES
 *   - A full SPLIT clause (e.g., "SPLIT INTO 5 TABLETS")
 * Returns NULL if the string is NULL or empty.  Syntax errors are
 * reported via ereport.
 */
YbOptSplit *YbParsePresplitString(const char *presplit_str);

/*
 * validate_cb for the yb_presplit string reloption.  Validates syntax by
 * running the value through YbParsePresplitString.
 */
extern void YbValidatePresplitReloption(const char *value);

/*
 * Validate that a yb_presplit string is compatible with `rel`'s partitioning
 * kind (hash vs range).  Caller is expected to have already validated syntax
 * (e.g. via the reloption validate_cb).  ereport(ERROR) on mismatch.
 */
extern void YbValidatePresplitForRelation(Relation rel, const char *presplit_str);

/*
 * Convert a YbOptSplit structure to a yb_presplit reloption string.
 * Returns NULL if split_options is NULL.
 */
char	   *YbSplitOptionsToPresplitString(YbOptSplit *split_options);

/*
 * Reconcile a statement's SPLIT clause and yb_presplit reloption so that both
 * representations agree before the relation is created.  See the function
 * comment in pg_yb_utils.c for the detailed contract.  Used by both CREATE
 * TABLE and CREATE INDEX paths.
 */
extern void YbSyncSplitOptionsAndPresplit(YbOptSplit **split_options,
										  List **options);

extern void HandleYBStatusAtErrorLevelImpl(YbcStatus status, int elevel, const char *text_domain,
											 const char *filename, int lineno, const char *funcname);

#define HandleYBStatus(status) \
	HandleYBStatusAtErrorLevel(status, ERROR)

/*
 * Macro to convert DocDB Status to Postgres error.
 */
#define HandleYBStatusAtErrorLevel(status, elevel) \
	do \
	{ \
		AssertMacro(!IsMultiThreadedMode()); \
		YbcStatus _status = (status); \
		if (_status) \
			HandleYBStatusAtErrorLevelImpl(_status, elevel, TEXTDOMAIN, __FILE__, __LINE__, PG_FUNCNAME_MACRO); \
	} while (0)

/*
 * Increments a tally of sticky objects (TEMP TABLES/WITH HOLD CURSORS)
 * maintained for every transaction.
 */
extern void increment_sticky_object_count();

/*
 * Decrements a tally of sticky objects (TEMP TABLES/WITH HOLD CURSORS)
 * maintained for every transaction.
 */
extern void decrement_sticky_object_count();

/*
 * Check if there exists a database object that requires a sticky connection.
 */
extern bool YbIsStickyConnection(int *change);

/*
 * Creates a shallow copy of the pointer list.
 */
extern void **YbPtrListToArray(const List *str_list, size_t *length);

/*
 * Reads the contents of the given file assuming that the filename is an
 * absolute path.
 *
 * The file contents are returned as a single palloc'd chunk with an extra \0
 * byte added to the end.
 */
extern char *YbReadWholeFile(const char *filename, int *length, int elevel);

extern bool yb_use_tserver_key_auth;

extern bool yb_use_tserver_key_auth_check_hook(bool *newval,
											   void **extra, GucSource source);

extern void YbATCopyPrimaryKeyToCreateStmt(Relation rel,
										   Relation pg_constraint,
										   CreateStmt *create_stmt);

extern void YbIndexSetNewRelfileNode(Relation indexRel, Oid relfileNodeId,
									 bool yb_copy_split_options,
									 YbOptSplit *preserved_index_split_options);

/*
 * Returns the ordering type for a primary key. By default, the first element of
 * YB relations are sorted by HASH, unless Postgres sorting is set, or the table
 * is colocated.
 */
extern SortByDir YbSortOrdering(SortByDir ordering, bool is_colocated, bool is_tablegroup, bool is_first_key);

extern const char *YbGetRedactedQueryString(const char *query, int *redacted_query_len);

/* Check if optimizations for UPDATE queries have been enabled. */
extern bool YbIsUpdateOptimizationEnabled();

extern bool YbIsIocPushdownEnabled(OnConflictAction action);

extern void YbRelationSetNewRelfileNode(Relation rel, Oid relfileNodeId,
										bool yb_copy_split_options,
										bool is_truncate);

extern Relation YbGetRelationWithOverwrittenReplicaIdentity(Oid relid,
															char replident);

extern void YBCUpdateYbReadTimeAndInvalidateRelcache(uint64_t read_time);

extern void YBCSetHistoricalReadContext(uint64_t read_time_ht,
										uint64_t in_txn_limit_ht,
										const char *docdb_txn_id);

extern void YBCInvalidateCachesForHistoricalReadContext(void);

extern void YBCSetHistoricalReadContextAndInvalidateCaches(uint64_t read_time_ht,
															 uint64_t in_txn_limit_ht,
															 const char *docdb_txn_id);

extern void YBCResetHistoricalReadContextAndInvalidateRelcache(void);

extern void YBCResetYbReadTimeAndInvalidateRelcache();

extern uint64_t YbCalculateTimeDifferenceInMicros(TimestampTz yb_start_time);

static inline bool
YbIsNormalDbOidReserved(Oid db_oid)
{
	return db_oid == kYBCPgSequencesDataDatabaseOid;
}

extern Oid	YbGetSQLIncrementCatalogVersionsFunctionOid();

extern bool YbIsReadCommittedTxn();
extern bool YbSkipPgSnapshotManagement();

extern YbOptionalReadPointHandle YbBuildCurrentReadPointHandle();
extern void YbUseSnapshotReadTime(uint64_t read_time);
extern YbOptionalReadPointHandle YbRegisterSnapshotReadTime(uint64_t read_time);
extern YbOptionalReadPointHandle YbResetTransactionReadPoint(bool is_catalog_snapshot);

extern bool YbUseFastBackwardScan();

extern bool YbIsYsqlConnMgrWarmupModeEnabled();

extern bool YbIsAuthBackend();
extern bool YbIsAuthPassthroughControlBackend();

extern bool YbIsYsqlConnMgrEnabled();

bool		YbIsAttrPrimaryKeyColumn(Relation rel, AttrNumber attnum);

SortByDir	YbGetIndexKeySortOrdering(Relation indexRel);

typedef enum YbTruncateType
{
	YB_SAFE_TRUNCATE,
	YB_UNSAFE_TRUNCATE_SYSTEM_RELATION,
	YB_UNSAFE_TRUNCATE_TABLE_REWRITE_DISABLED,
} YbTruncateType;

extern YbTruncateType YbUseUnsafeTruncate(Relation rel);

extern AttrNumber YbGetIndexAttnum(Relation index, AttrNumber table_attno);

extern bool yb_ysql_conn_mgr_superuser_existed;

extern Oid	YbGetDatabaseOidToIncrementCatalogVersion();

extern bool yb_default_collation_resolved;

/*
 * Whether every message in the lists is one this backend knows how to apply. Looks only at the
 * message bytes, so a caller can ask before it has anything to apply them to.
 */
extern bool YbCanApplyInvalidationMessages(YbcCatalogMessageLists *message_lists);
extern bool YbApplyInvalidationMessages(YbcCatalogMessageLists *message_lists);
extern void YbUpdatePggateTableCacheAfterApply(void);

extern bool YbInvalidationMessagesTableExists();

extern bool yb_is_calling_internal_sql_for_ddl;

extern char *YbGetPotentiallyHiddenOidText(Oid oid);

extern void YbWaitForSharedCatalogVersionToCatchup(uint64_t version);

/* Whether this backend is authenticating on connection manager's behalf. */
extern bool YbIsPerformingConnMgrAuth();

/* The admission priority to ask the master for when prefetching ahead of authentication. */
extern YbcPgSysTablePrefetchKind YbAuthPrefetchKind();

extern bool YbIsInvalidationMessageEnabled();

extern bool YbRefreshMatviewInPlace();

extern void YbForceSendInvalMessages();

extern long YbGetPeakRssKb();

extern bool YbIsAnyDependentGeneratedColPK(Relation rel, AttrNumber attnum);

extern bool YbCheckTserverResponseCacheForAuthGflags();

extern bool YbUseTserverResponseCacheForAuth(uint64_t shared_catalog_version);

typedef enum YbTxnError
{
	YB_TXN_CONFLICT,
	YB_TXN_RESTART_READ,
	YB_TXN_DEADLOCK,
	YB_TXN_ABORTED,
	YB_TXN_SKIP_LOCKING,
	YB_TXN_LOCK_NOT_FOUND,
	YB_TXN_CONFLICT_KIND_COUNT, /* Must be last value of this enum */
} YbTxnError;

typedef enum
{
	YB_QPM_TRACK_NONE,
	YB_QPM_TRACK_TOP,
	YB_QPM_TRACK_ALL
} YbQpmTrackEnum;

typedef enum
{
	YB_QPM_SIMPLE_CLOCK_LRU,
	YB_QPM_TRUE_LRU
} YbCacheReplacementAlgorithmEnum;

typedef struct YbQpmConfiguration
{
	int track;
	int cache_replacement_algorithm;
	int max_cache_size;
	bool track_catalog_queries;
	int plan_format;
	bool verbose_plans;
	bool compress_text;
	bool show_max_exec_params;
} YbQpmConfiguration;

extern YbQpmConfiguration yb_qpm_configuration;

extern void YbResetRetryCounts();
extern void YbIncrementRetryCount(YbTxnError kind);
extern uint64_t YbGetRetryCount(YbTxnError kind);
extern uint64_t YbGetTotalRetryCount();
extern YbTxnError YbSqlErrorCodeToTransactionError(int sqlerrcode);

extern bool YbCatalogPreloadRequired();
extern bool YbUseMinimalCatalogCachesPreload();

extern YbcPgStatement YbNewSample(Relation rel, int targrows, double rstate_w,
																	uint64_t rand_state_s0, uint64_t rand_state_s1);

extern YbcPgStatement YbNewSelect(Relation rel, const YbcPgPrepareParameters *prepare_params);

extern YbcPgStatement YbNewUpdateForDb(Oid db_oid, Relation rel,
									  YbcPgTransactionSetting transaction_setting);

extern YbcPgStatement YbNewUpdate(Relation rel, YbcPgTransactionSetting transaction_setting);

extern YbcPgStatement YbNewDelete(Relation rel, YbcPgTransactionSetting transaction_setting);

extern YbcPgStatement YbNewInsertForDb(Oid db_oid, Relation rel,
									   YbcPgTransactionSetting transaction_setting);

extern YbcPgStatement YbNewInsert(Relation rel, YbcPgTransactionSetting transaction_setting);

extern YbcPgStatement YbNewInsertBlock(Relation rel, YbcPgTransactionSetting transaction_setting);

extern YbcPgStatement YbNewTruncateColocated(Relation rel,
											 YbcPgTransactionSetting transaction_setting);

extern YbcPgStatement YbNewTruncateColocatedIgnoreNotFound(Relation rel,
														   YbcPgTransactionSetting transaction_setting);
extern YbcPgSkipIntentsOptimizationInfo YbGetSkipIntentsOptimizationInfoWrite(Relation rel);
extern void YbEnableSkipIntentsForNewTransaction();
extern void YbMaybeDisableSkipIntentsForCDCSDK(Oid database_oid);

extern const unsigned char *YbGetLocalTServerUuid();
extern void YbUCharToUuid(const unsigned char *in, pg_uuid_t *out);

typedef enum
{
	YB_TRACEPARENT_OK,
	YB_TRACEPARENT_NO_COMMENT,
	YB_TRACEPARENT_NO_FIELD,
	YB_TRACEPARENT_WRONG_SIZE,
	YB_TRACEPARENT_MISSING_OPEN_QUOTE,
	YB_TRACEPARENT_MISSING_CLOSE_QUOTE
} YbTraceparentResult;

extern const char *YbGetTraceparentResultErrmsg(YbTraceparentResult result);

extern YbTraceparentResult YbGetTraceparentFromTraceContext(const char *trace_context,
															size_t trace_context_len,
															char *traceparent_out);
extern bool YBHasSkippedIntentsWrite();

/*
 * Returns true if 'relid' is a foreign table whose foreign server has
 * server_type = 'federatedYugabyteDB'.
 */
extern bool yb_is_federated_yb_foreign_table(Oid relid);

/*
 * If 'ft_relid' is a federatedYugabyteDB foreign table, resolve the local
 * relation named by its schema_name/table_name options and return its OID.
 * Returns InvalidOid if the table is not federated or the relation is not
 * found locally.
 */
extern Oid	YbGetFederatedForeignTableBackingRelid(Oid ft_relid);

struct PlannerInfo;
struct RelOptInfo;
struct RangeTblEntry;
extern void YbAddFederatedPartitionTserverUuid(struct PlannerInfo *root,
											  Index rti,
											  const char *tserver_uuid);
extern const char *YbGetFederatedPartitionTserverUuid(const struct PlannerInfo *root,
													  Index rti);

extern void YbInvalidatePlannerRelcache(struct PlannerInfo *root);

extern void YbHandleConflictError(Relation rel, LockWaitPolicy wait_policy);

extern void HandleExplicitRowLockStatus(YbcPgExplicitRowLockStatus status);

/*
 * YB: db_oid namespace for internal advisory locks. InvalidOid cannot collide
 * with user advisory locks, which always use MyDatabaseId.
 */
#define YB_INTERNAL_ADVISORY_LOCK_DB_OID	InvalidOid

/* YB: classid for replication slot advisory locks. */
#define ADVISORY_LOCK_CLASSID_REPL_SLOT	2

extern YbcAdvisoryLockId GetYBAdvisoryLockId(LOCKTAG tag);
extern bool HandleStatusIgnoreSkipLocking(YbcStatus status);

#endif							/* PG_YB_UTILS_H */
