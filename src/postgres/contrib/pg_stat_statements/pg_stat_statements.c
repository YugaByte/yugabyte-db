/*-------------------------------------------------------------------------
 *
 * pg_stat_statements.c
 *		Track statement planning and execution times as well as resource
 *		usage across a whole database cluster.
 *
 * Execution costs are totaled for each distinct source query, and kept in
 * a shared hashtable.  (We track only as many distinct queries as will fit
 * in the designated amount of shared memory.)
 *
 * Starting in Postgres 9.2, this module normalized query entries.  As of
 * Postgres 14, the normalization is done by the core if compute_query_id is
 * enabled, or optionally by third-party modules.
 *
 * To facilitate presenting entries to users, we create "representative" query
 * strings in which constants are replaced with parameter symbols ($n), to
 * make it clearer what a normalized entry can represent.  To save on shared
 * memory, and to avoid having to truncate oversized query strings, we store
 * these strings in a temporary external query-texts file.  Offsets into this
 * file are kept in shared memory.
 *
 * Note about locking issues: to create or delete an entry in the shared
 * hashtable, one must hold pgss->lock exclusively.  Modifying any field
 * in an entry except the counters requires the same.  To look up an entry,
 * one must hold the lock shared.  To read or update the counters within
 * an entry, one must hold the lock shared or exclusive (so the entry doesn't
 * disappear!) and also take the entry's mutex spinlock.
 * The shared state variable pgss->extent (the next free spot in the external
 * query-text file) should be accessed only while holding either the
 * pgss->mutex spinlock, or exclusive lock on pgss->lock.  We use the mutex to
 * allow reserving file space while holding only shared lock on pgss->lock.
 * Rewriting the entire external query-text file, eg for garbage collection,
 * requires holding pgss->lock exclusively; this allows individual entries
 * in the file to be read or written while holding only shared lock.
 *
 *
 * Copyright (c) 2008-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_statements/pg_stat_statements.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/parallel.h"
#include "catalog/pg_authid.h"
#include "common/hashfn.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/scanner.h"
#include "parser/scansup.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/queryjumble.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/* YB includes */
#include "common/jsonapi.h"
#include "common/pg_yb_common.h"
#include "hdr/hdr_histogram.h"
#include "lib/stringinfo.h"
#include "pg_yb_utils.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "yb/yql/pggate/webserver/ybc_pg_webserver_wrapper.h"
#include "yb_query_diagnostics.h"
#include <inttypes.h>
#include <stddef.h>

PG_MODULE_MAGIC;

/* Location of permanent stats file (valid when database is shut down) */
#define PGSS_DUMP_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_stat_statements.stat"

/*
 * Location of external query text file.
 */
#define PGSS_TEXT_FILE	PG_STAT_TMP_DIR "/pgss_query_texts.stat"

/* Magic number identifying the stats file format */
/* YB: Postgres 15 uses the following number.
static const uint32 PGSS_FILE_HEADER = 0x20220408;
*/
static const uint32 PGSS_FILE_HEADER = 0x20250425;

/* PostgreSQL major version number, changes in which invalidate all entries */
static const uint32 PGSS_PG_MAJOR_VERSION = PG_VERSION_NUM / 100;

/* XXX: Should USAGE_EXEC reflect execution time and/or buffer usage? */
#define USAGE_EXEC(duration)	(1.0)
#define USAGE_INIT				(1.0)	/* including initial planning */
#define ASSUMED_MEDIAN_INIT		(10.0)	/* initial assumed median usage */
#define ASSUMED_LENGTH_INIT		1024	/* initial assumed mean query length */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every entry_dealloc */
#define STICKY_DECREASE_FACTOR	(0.50)	/* factor for sticky entries */
#define USAGE_DEALLOC_PERCENT	5	/* free this % of entries at once */
#define IS_STICKY(c)	((c.calls[PGSS_PLAN] + c.calls[PGSS_EXEC]) == 0)

/*
 * Utility statements that pgss_ProcessUtility and pgss_post_parse_analyze
 * ignores.
 */
#define PGSS_HANDLED_UTILITY(n)		(!IsA(n, ExecuteStmt) && \
									!IsA(n, PrepareStmt) && \
									!IsA(n, DeallocateStmt))

#define YB_NUM_COUNTERS_INT 60
#define YB_NUM_COUNTERS_DBL 40

/*
 * Extension version number, for supporting older extension versions' objects
 */
typedef enum pgssVersion
{
	PGSS_V1_0 = 0,
	PGSS_V1_1,
	PGSS_V1_2,
	PGSS_V1_3,
	YB_PGSS_V1_4,
	PGSS_V1_8,
	PGSS_V1_9,
	PGSS_V1_10,
	YB_PGSS_V1_10,
	YB_V2_0_PGSS_V1_10,
	YB_V2_1_PGSS_V1_10,
} pgssVersion;

/*
 * yb change: Added YB_PGSS_V1_4 which adds a the column yb_latency_histogram to
 * pg_stat_statements. This necessitates new pg_stat_statements_internal changes
 * because the number of columns in the table has changed. Considering upgrades,
 * we use the PGSS_FILE_HEADER variable to determine whether a saved-to-disk
 * pg_stat_statements file contains a histogram or not. If not, we read in the
 * other pg_stat_statements information and create a fresh histogram. Histogram
 * data is tracked regardless of pgssVersion but the yb_latency_histogram column
 * only gets added to pg_stat_statements if api_version >= YB_PGSS_V1_4.
 */

typedef enum pgssStoreKind
{
	PGSS_INVALID = -1,

	/*
	 * PGSS_PLAN and PGSS_EXEC must be respectively 0 and 1 as they're used to
	 * reference the underlying values in the arrays in the Counters struct,
	 * and this order is required in pg_stat_statements_internal().
	 */
	PGSS_PLAN = 0,
	PGSS_EXEC,

	PGSS_NUMKIND				/* Must be last value of this enum */
} pgssStoreKind;

/*
 * Hashtable key that defines the identity of a hashtable entry.  We separate
 * queries by user and by database even if they are otherwise identical.
 *
 * If you add a new key to this struct, make sure to teach pgss_store() to
 * zero the padding bytes.  Otherwise, things will break, because pgss_hash is
 * created using HASH_BLOBS, and thus tag_hash is used to hash this.

 */
typedef struct pgssHashKey
{
	Oid			userid;			/* user OID */
	Oid			dbid;			/* database OID */
	uint64		queryid;		/* query identifier */
	bool		toplevel;		/* query executed at top level */
} pgssHashKey;

/*
 * Enum for YbCounters::counters
 */
typedef enum YbIntCounters
{
	YB_INT_DOCDB_READ_RPCS,
	YB_INT_DOCDB_WRITE_RPCS,
	YB_INT_DOCDB_READ_OPS,
	YB_INT_DOCDB_WRITE_OPS,
	YB_INT_DOCDB_ROWS_SCANNED,
	YB_INT_CONFLICT_RETRIES,
	YB_INT_READ_RESTART_RETRIES,
	YB_INT_TOTAL_RETRIES,
	YB_INT_DOCDB_ROWS_RETURNED,
	YB_INT_DOCDB_OBSOLETE_ROWS_SCANNED,
	YB_INT_DOCDB_SEEKS,
	YB_INT_DOCDB_NEXTS,
	YB_INT_DOCDB_PREVS,

	YB_INT_COUNTERS_LAST = YB_NUM_COUNTERS_INT
} YbIntCounters;

/*
 * Enum for YbCounters::counters_dbl
 */
typedef enum YbDoubleCounters
{
	YB_DBL_CATALOG_WAIT_TIME_MS,
	YB_DBL_DOCDB_WAIT_TIME_MS,
	YB_DBL_DOCDB_READ_TIME_MS,
	YB_DBL_DOCDB_WRITE_TIME_MS,

	YB_DBL_COUNTERS_LAST = YB_NUM_COUNTERS_DBL
} YbDoubleCounters;

/*
 * Struct for YB-specific counters. Currently, all counters are unreserved.
 */
typedef struct YbCounters
{
	int64		counters[YB_NUM_COUNTERS_INT];
	double		counters_dbl[YB_NUM_COUNTERS_DBL];
} YbCounters;

/*
 * The actual stats counters kept within pgssEntry.
 */
typedef struct Counters
{
	int64		calls[PGSS_NUMKIND];	/* # of times planned/executed */
	double		total_time[PGSS_NUMKIND];	/* total planning/execution time,
											 * in msec */
	double		min_time[PGSS_NUMKIND]; /* minimum planning/execution time in
										 * msec */
	double		max_time[PGSS_NUMKIND]; /* maximum planning/execution time in
										 * msec */
	double		mean_time[PGSS_NUMKIND];	/* mean planning/execution time in
											 * msec */
	double		sum_var_time[PGSS_NUMKIND]; /* sum of variances in
											 * planning/execution time in msec */
	int64		rows;			/* total # of retrieved or affected rows */
	int64		shared_blks_hit;	/* # of shared buffer hits */
	int64		shared_blks_read;	/* # of shared disk blocks read */
	int64		shared_blks_dirtied;	/* # of shared disk blocks dirtied */
	int64		shared_blks_written;	/* # of shared disk blocks written */
	int64		local_blks_hit; /* # of local buffer hits */
	int64		local_blks_read;	/* # of local disk blocks read */
	int64		local_blks_dirtied; /* # of local disk blocks dirtied */
	int64		local_blks_written; /* # of local disk blocks written */
	int64		temp_blks_read; /* # of temp blocks read */
	int64		temp_blks_written;	/* # of temp blocks written */
	double		blk_read_time;	/* time spent reading blocks, in msec */
	double		blk_write_time; /* time spent writing blocks, in msec */
	double		temp_blk_read_time; /* time spent reading temp blocks, in msec */
	double		temp_blk_write_time;	/* time spent writing temp blocks, in
										 * msec */
	double		usage;			/* usage factor */
	int64		wal_records;	/* # of WAL records generated */
	int64		wal_fpi;		/* # of WAL full page images generated */
	uint64		wal_bytes;		/* total amount of WAL generated in bytes */
	int64		jit_functions;	/* total number of JIT functions emitted */
	double		jit_generation_time;	/* total time to generate jit code */
	int64		jit_inlining_count; /* number of times inlining time has been
									 * > 0 */
	double		jit_inlining_time;	/* total time to inline jit code */
	int64		jit_optimization_count; /* number of times optimization time
										 * has been > 0 */
	double		jit_optimization_time;	/* total time to optimize jit code */
	int64		jit_emission_count; /* number of times emission time has been
									 * > 0 */
	double		jit_emission_time;	/* total time to emit jit code */
	YbCounters	yb_counters;	/* YB specific counters */
} Counters;

/*
 * hdr_histogram
 * defined in third party hdr_histogram.h
 */
typedef struct hdr_iter yb_hdr_iter;

/*
 * Global statistics for pg_stat_statements
 */
typedef struct pgssGlobalStats
{
	int64		dealloc;		/* # of times entries were deallocated */
	TimestampTz stats_reset;	/* timestamp with all stats reset */
} pgssGlobalStats;

/*
 * Statistics per statement
 *
 * Note: in event of a failure in garbage collection of the query text file,
 * we reset query_offset to zero and query_len to -1.  This will be seen as
 * an invalid state by qtext_fetch().
 */
typedef struct pgssEntry
{
	pgssHashKey key;			/* hash key of entry - MUST BE FIRST */
	Counters	counters;		/* the statistics for this query */
	Size		query_offset;	/* query text offset in external file */
	int			query_len;		/* # of valid bytes in query string, or -1 */
	int			encoding;		/* query text encoding */
	slock_t		mutex;			/* protects the counters only */
	size_t		yb_slow_executions; /* # of executions >= yb_hdr_max_value *
									 * yb_hdr_latency_res_ms */
	hdr_histogram yb_hdr_histogram; /* flexible array member at end - MUST BE
									 * LAST */
} pgssEntry;

typedef struct
{
	int32		num;
	int		   *buffer_size;
	char	  **buffer;

	/* hdr specific */
	int			temp_yb_hdr_count_bytes;
	bool		hdr_config_match;

} pgssYbReaderContext;

/*
 * Global shared state
 */
typedef struct pgssSharedState
{
	LWLock	   *lock;			/* protects hashtable search/modification */
	double		cur_median_usage;	/* current median usage in hashtable */
	Size		mean_query_len; /* current mean entry text length */
	slock_t		mutex;			/* protects following fields only: */
	Size		extent;			/* current extent of query file */
	int			n_writers;		/* number of active writers to query file */
	int			gc_count;		/* query file garbage collection cycle count */
	pgssGlobalStats stats;		/* global statistics for pgss */
} pgssSharedState;

/*---- Local variables ----*/

/* Current nesting depth of ExecutorRun+ProcessUtility calls */
static int	exec_nested_level = 0;

/* Current nesting depth of planner calls */
static int	plan_nested_level = 0;

/*
 * YB: Session-local set of queryIds whose queries contain constants that
 * require normalization (i.e. clocations_count > 0 at parse time).
 *
 * An entry is added by pgss_post_parse_analyze when the query has constants
 * needing normalization.
 *
 * pgss_store consults this set when the shared hash table entry is missing and
 * jstate is NULL: if the queryId is present, the entry was evicted or reset
 * and we discard stats rather than storing unnormalized text; if absent, the
 * query never needed normalization and raw text is stored normally.
 *
 * A hash set keyed on queryId (rather than a single boolean) is needed so that
 * the extended query protocol — where multiple queries with different
 * normalization needs can be parsed before any are executed — is handled
 * correctly.
 *
 * The set is session-local because it tracks what this backend has parsed;
 * every backend must parse a query before executing it, so the set is always
 * populated before it is read.  Session-local storage also avoids the need for
 * shared-memory sizing, locking, and cleanup on backend exit.
 *
 * queryIds are never removed from this set; once recorded, an id remains until
 * backend exit.  That is safe: at worst a recycled queryId is treated as
 * needing normalization and we discard a few stats, never the reverse error of
 * storing unnormalized text.
 */
typedef uint64 YbPgssQueryNeedingNormalization;

static HTAB *yb_pgss_queries_needing_normalization = NULL;

/* Saved hook values in case of unload */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Links to shared memory state */
static pgssSharedState *pgss = NULL;
static HTAB *pgss_hash = NULL;

/*---- GUC variables ----*/

typedef enum
{
	PGSS_TRACK_NONE,			/* track no statements */
	PGSS_TRACK_TOP,				/* only top level statements */
	PGSS_TRACK_ALL				/* all statements, including nested ones */
}			PGSSTrackLevel;

static const struct config_enum_entry track_options[] =
{
	{"none", PGSS_TRACK_NONE, false},
	{"top", PGSS_TRACK_TOP, false},
	{"all", PGSS_TRACK_ALL, false},
	{NULL, 0, false}
};

#define YB_HDR_DEFAULT_MAX_LATENCY_MS 1677721.6
#define YB_HDR_DEFAULT_LATENCY_RES_MS 0.1
#define YB_HDR_DEFAULT_BUCKET_FACTOR 16
#define YB_HDR_DEFAULT_MAX_VALUE YB_HDR_DEFAULT_MAX_LATENCY_MS / YB_HDR_DEFAULT_LATENCY_RES_MS
#define YB_DEFAULT_QTEXT_LIMIT_KB -1	/* max text file size read; -1 = no limit */
#define YB_DEFAULT_MAX_QUERY_TEXT_LENGTH -1	/* max text bytes per entry; -1 = no limit */

static int	pgss_max;			/* max # statements to track */
static int	pgss_track;			/* tracking level */
static bool pgss_track_utility; /* whether to track utility commands */
static bool pgss_track_planning;	/* whether to track planning duration */
static bool pgss_save;			/* whether to save stats across shutdown */
static float yb_hdr_max_latency_ms = YB_HDR_DEFAULT_MAX_LATENCY_MS; /* hardcoded in phase 1,
																	 * max query latency
																	 * tracked by histogram */
static float yb_hdr_latency_res_ms = YB_HDR_DEFAULT_LATENCY_RES_MS; /* hardcoded in phase 1,
																	 * starting query
																	 * latency resolution
																	 * tracked by histogram */
static int	yb_hdr_bucket_factor;	/* subbuckets per bucket for histogram */
static int64_t yb_hdr_max_value = YB_HDR_DEFAULT_MAX_VALUE; /* default hardcode for
															 * phase 1, will need to
															 * be adjusted against
															 * latency_res */
static struct hdr_histogram_bucket_config cfg;
static int	yb_qtext_size_limit = YB_DEFAULT_QTEXT_LIMIT_KB;
static int	yb_max_query_text_length = YB_DEFAULT_MAX_QUERY_TEXT_LENGTH;

/*
 * yb_hdr_max_value is the integer representation of the max query latency we
 * want to track. Currently with a latency resolution of 0.1ms,
 * 16777216 * 0.1 = 1677721.6ms ~ 28min, our default max query latency. Max
 * query latency and latency resolution have not been discretely implemented as
 * GUC variables yet. In future phases, we will modify this value and
 * yb_hdr_max_latency_ms accordingly based on the histogram-supported max and
 * latency resolution.
 */

#define pgss_enabled(level) \
	(!IsParallelWorker() && \
	(pgss_track == PGSS_TRACK_ALL || \
	(pgss_track == PGSS_TRACK_TOP && (level) == 0)))

#define record_gc_qtexts() \
	do { \
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss; \
		SpinLockAcquire(&s->mutex); \
		s->gc_count++; \
		SpinLockRelease(&s->mutex); \
	} while(0)

/*---- Function declarations ----*/

void		_PG_init(void);

PG_FUNCTION_INFO_V1(pg_stat_statements_reset);
PG_FUNCTION_INFO_V1(pg_stat_statements_reset_1_7);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_2);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_3);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_8);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_9);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_10);
PG_FUNCTION_INFO_V1(pg_stat_statements);
PG_FUNCTION_INFO_V1(pg_stat_statements_info);

PG_FUNCTION_INFO_V1(yb_pg_stat_statements_1_4);
PG_FUNCTION_INFO_V1(yb_pg_stat_statements_1_10);
PG_FUNCTION_INFO_V1(yb_2_0_pg_stat_statements_1_10);
PG_FUNCTION_INFO_V1(yb_2_1_pg_stat_statements_1_10);

static void pgss_shmem_request(void);
static void pgss_shmem_startup(void);
static void pgss_shmem_shutdown(int code, Datum arg);
static void pgss_post_parse_analyze(ParseState *pstate, Query *query,
									JumbleState *jstate);
static PlannedStmt *pgss_planner(Query *parse,
								 const char *query_string,
								 int cursorOptions,
								 ParamListInfo boundParams);
static void pgss_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgss_ExecutorRun(QueryDesc *queryDesc,
							 ScanDirection direction,
							 uint64 count, bool execute_once);
static void pgss_ExecutorFinish(QueryDesc *queryDesc);
static void pgss_ExecutorEnd(QueryDesc *queryDesc);
static void pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								bool readOnlyTree,
								ProcessUtilityContext context, ParamListInfo params,
								QueryEnvironment *queryEnv,
								DestReceiver *dest, QueryCompletion *qc);
static void pgss_store(const char *query, uint64 queryId,
					   int query_location, int query_len,
					   pgssStoreKind kind,
					   double total_time, uint64 rows,
					   const BufferUsage *bufusage,
					   const WalUsage *walusage,
					   const struct JitInstrumentation *jitusage,
					   JumbleState *jstate,
					   bool yb_is_sensitive_stmt,
					   YbInstrumentation *yb_stats);
static void pg_stat_statements_internal(FunctionCallInfo fcinfo,
										pgssVersion api_version,
										bool showtext);
static Size pgss_memsize(void);
static pgssEntry *entry_alloc(pgssHashKey *key, Size query_offset, int query_len,
							  int encoding, bool sticky);
static void entry_dealloc(void);
static bool qtext_store(const char *query, int query_len,
						Size *query_offset, int *gc_count);
static char *qtext_load_file(Size *buffer_size);
static char *qtext_fetch(Size query_offset, int query_len,
						 char *buffer, Size buffer_size);
static bool need_gc_qtexts(void);
static void gc_qtexts(void);
static void entry_reset(Oid userid, Oid dbid, uint64 queryid);
static char *generate_normalized_query(JumbleState *jstate, const char *query,
									   int query_loc, int *query_len_p);
static void fill_in_constant_lengths(JumbleState *jstate, const char *query,
									 int query_loc);
static int	comp_location(const void *a, const void *b);

/* YB functions */
PG_FUNCTION_INFO_V1(yb_get_histogram_jsonb);
static Datum yb_get_histogram_jsonb_args(uint64 queryid, Oid userid, Oid dbid,
										 bool top_level);
static void yb_add_hdr_jsonb_object(JsonbParseState *state, char *buf,
									count_t count, JsonbPair *pair);
static Datum yb_add_histogram_jsonb(JsonbParseState *state,
									hdr_histogram *h, size_t yb_slow_executions);
static void yb_hdr_reset(hdr_histogram *h);
static int	read_entry_original(int header, FILE *file, FILE *qfile,
								pgssYbReaderContext *context);
static int	read_entry_hdr(int header, FILE *file, FILE *qfile,
						   pgssYbReaderContext *context);
static int	extended_header_reader(int header, FILE *file,
								   pgssYbReaderContext *context);
static int	query_buffer_helper(FILE *file, FILE *qfile, int qlen,
								Size *query_offset, int encoding, Counters *counters,
								pgssYbReaderContext *context);
static void enforce_bucket_factor(int *value);
static bool yb_track_nested_queries(void);
static int	YbGetPgssNormalizedQueryText(Size query_offset, int actual_query_len, char *normalized_query);
static char *yb_generate_normalized_backfill_query(YbBackfillIndexStmt *stmt,
												   int *query_len_p);
static void yb_pgss_mark_needs_normalization(uint64 queryId);
static bool yb_pgss_needs_normalization(uint64 queryId);
static char *yb_truncate_query_text(const char *query, int *query_len,
									int encoding);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_statements functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Inform the postmaster that we want to enable query_id calculation if
	 * compute_query_id is set to auto.
	 */
	EnableQueryId();

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable("pg_stat_statements.max",
							"Sets the maximum number of statements tracked by pg_stat_statements.",
							NULL,
							&pgss_max,
							5000,
							100,
							INT_MAX / 2,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("pg_stat_statements.track",
							 "Selects which statements are tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track,
							 PGSS_TRACK_ALL,	/* YB: change default */
							 track_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.track_utility",
							 "Selects whether utility commands are tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track_utility,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.track_planning",
							 "Selects whether planning duration is tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track_planning,
							 true,	/* YB: change default */
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.save",
							 "Save pg_stat_statements statistics across server shutdowns.",
							 NULL,
							 &pgss_save,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("pg_stat_statements.yb_hdr_bucket_factor",
							"Sets the number of subbuckets per bucket in pgss histogram stat tracking.",
							NULL,
							&yb_hdr_bucket_factor,
							YB_HDR_DEFAULT_BUCKET_FACTOR,
							8,
							32,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_stat_statements.yb_qtext_size_limit",
							"Sets the max size of the pg_stat_statements query text file.",
							NULL,
							&yb_qtext_size_limit,
							YB_DEFAULT_QTEXT_LIMIT_KB,
							-1,
							MAX_KILOBYTES,
							PGC_SUSET,
							GUC_UNIT_KB,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_stat_statements.yb_max_query_text_length",
							"Sets the maximum length in bytes of the query text stored for each statement.",
							"Longer texts are truncated and end with \"...\". -1 stores texts in full.",
							&yb_max_query_text_length,
							YB_DEFAULT_MAX_QUERY_TEXT_LENGTH,
							-1,
							INT_MAX / 2,
							PGC_SUSET,
							GUC_UNIT_BYTE,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("pg_stat_statements");

	/*
	 * Calculate actual max = 2^(sub_bucket_half_count_magnitude + bucket_count)
	 * So hdr's max val should be actual max - 1
	 * yb_hdr_max_latency_ms and yb_hdr_latency_res_ms will be made GUC vars
	 * Adjust yb_hdr_max_latency_ms accordingly
	 */
	int			prelim_max_value = yb_hdr_max_latency_ms / yb_hdr_latency_res_ms;

	enforce_bucket_factor(&yb_hdr_bucket_factor);

	if (yb_hdr_calculate_bucket_config(1, prelim_max_value - 1,
									   yb_hdr_bucket_factor, &cfg))
	{
		elog(LOG, "Cannot configure custom hdr values, reverting to default");
		yb_hdr_max_latency_ms = YB_HDR_DEFAULT_MAX_LATENCY_MS;
		yb_hdr_latency_res_ms = YB_HDR_DEFAULT_LATENCY_RES_MS;
		yb_hdr_bucket_factor = YB_HDR_DEFAULT_BUCKET_FACTOR;
		prelim_max_value = yb_hdr_max_latency_ms / yb_hdr_latency_res_ms;
		yb_hdr_calculate_bucket_config(1, prelim_max_value - 1,
									   yb_hdr_bucket_factor, &cfg);
	}
	int			derived_max_magnitude = (cfg.sub_bucket_half_count_magnitude +
										 cfg.bucket_count);

	yb_hdr_max_value = pow(2, derived_max_magnitude);
	/* cfg retains prelim_max_value, update for use in hdr_init_preallocated */
	if (prelim_max_value != yb_hdr_max_value)
		yb_hdr_calculate_bucket_config(1, yb_hdr_max_value - 1,
									   yb_hdr_bucket_factor, &cfg);
	yb_hdr_max_latency_ms = yb_hdr_max_value * yb_hdr_latency_res_ms;

	/*
	 * Install hooks.
	 */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgss_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgss_shmem_startup;
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pgss_post_parse_analyze;
	prev_planner_hook = planner_hook;
	planner_hook = pgss_planner;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgss_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pgss_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pgss_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgss_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pgss_ProcessUtility;

	/* Function pointer to check if nested queries should be tracked in ASH */
	yb_ash_track_nested_queries = yb_track_nested_queries;

	/*
	 * Initializing the function pointer required by yb_query_diagnostics.c
	 * to get normalized query text.
	 */
	yb_get_normalized_query = &YbGetPgssNormalizedQueryText;

	/*
	 * Initialize the function pointer required by yb_query_diagnostics.c
	 * to get constant lengths in the query text.
	 */
	yb_qd_fill_in_constant_lengths = &fill_in_constant_lengths;
}

/*
 * shmem_request hook: request additional shared resources.  We'll allocate or
 * attach to the shared resources in pgss_shmem_startup().
 */
static void
pgss_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pgss_memsize());
	RequestNamedLWLockTranche("pg_stat_statements", 1);
}

static void
resetYsqlStatementStats()
{
	pg_stat_statements_reset(NULL);
}

static void
yb_add_hist_json(void *cb_arg, hdr_histogram *h, size_t yb_slow_executions)
{
	yb_hdr_iter iter;
	StringInfoData buf;

	initStringInfo(&buf);

	hdr_iter_init(&iter, h);
	while (hdr_iter_next(&iter))
	{
		if (iter.count > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "[%.1f,%.1f)",
							 (iter.value_iterated_to) * yb_hdr_latency_res_ms,
							 (iter.highest_equivalent_value + 1) * yb_hdr_latency_res_ms);
			WriteIntValueObjectToJson(cb_arg, buf.data, &iter.count);
		}
	}

	if (yb_slow_executions > 0)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "[%.1f,)", yb_hdr_max_value * yb_hdr_latency_res_ms);
		WriteIntValueObjectToJson(cb_arg, buf.data, &yb_slow_executions);
	}
	pfree(buf.data);
}

static double
calc_stddev(int64 calls, double sum_var_time)
{
	return (calls > 1) ? (sqrt(sum_var_time / calls)) : 0.0;
}

static void
getYsqlStatementStats(void *cb_arg)
{
	HASH_SEQ_STATUS hash_seq;
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	pgssEntry  *entry;

	/*
	 * TODO(gauravsingh): Add memory context cleanup to prevent memory leak.
	 */
	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		return;

	LWLockAcquire(pgss->lock, LW_SHARED);

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		/* some entries have 0 calls and strange query text - ignore them */
		if (IS_STICKY(entry->counters))
			continue;

		char	   *qry = qtext_fetch(entry->query_offset, entry->query_len, qbuffer, qbuffer_size);

		if (qry != NULL)
		{
			WriteStartObjectToJson(cb_arg);

			WriteIntToJson(cb_arg, "userid", entry->key.userid);
			WriteIntToJson(cb_arg, "dbid", entry->key.dbid);
			WriteBoolToJson(cb_arg, "toplevel", entry->key.toplevel);
			WriteIntToJson(cb_arg, "query_id", entry->key.queryid);
			WriteStringToJson(cb_arg, "query", qry);
			WriteIntToJson(cb_arg, "calls", entry->counters.calls[PGSS_EXEC]);

			WriteDoubleToJson(cb_arg, "total_plan_time", entry->counters.total_time[PGSS_PLAN]);
			WriteDoubleToJson(cb_arg, "total_exec_time", entry->counters.total_time[PGSS_EXEC]);

			WriteDoubleToJson(cb_arg, "min_plan_time", entry->counters.min_time[PGSS_PLAN]);
			WriteDoubleToJson(cb_arg, "min_exec_time", entry->counters.min_time[PGSS_EXEC]);

			WriteDoubleToJson(cb_arg, "max_plan_time", entry->counters.max_time[PGSS_PLAN]);
			WriteDoubleToJson(cb_arg, "max_exec_time", entry->counters.max_time[PGSS_EXEC]);

			WriteDoubleToJson(cb_arg, "mean_plan_time", entry->counters.mean_time[PGSS_PLAN]);
			WriteDoubleToJson(cb_arg, "mean_exec_time", entry->counters.mean_time[PGSS_EXEC]);

			WriteDoubleToJson(cb_arg, "stddev_plan_time",
							  calc_stddev(entry->counters.calls[PGSS_PLAN],
										  entry->counters.sum_var_time[PGSS_PLAN]));
			WriteDoubleToJson(cb_arg, "stddev_exec_time",
							  calc_stddev(entry->counters.calls[PGSS_EXEC],
										  entry->counters.sum_var_time[PGSS_EXEC]));

			WriteIntToJson(cb_arg, "rows", entry->counters.rows);

			WriteIntToJson(cb_arg, "local_blks_hit", entry->counters.local_blks_hit);
			WriteIntToJson(cb_arg, "local_blks_read", entry->counters.local_blks_read);
			WriteIntToJson(cb_arg, "local_blks_dirtied", entry->counters.local_blks_dirtied);
			WriteIntToJson(cb_arg, "local_blks_written", entry->counters.local_blks_written);
			WriteIntToJson(cb_arg, "temp_blks_read", entry->counters.temp_blks_read);
			WriteIntToJson(cb_arg, "temp_blks_written", entry->counters.temp_blks_written);

			WriteArrayBeginToJson(cb_arg, "yb_latency_histogram");
			yb_add_hist_json(cb_arg, &entry->yb_hdr_histogram, entry->yb_slow_executions);
			WriteArrayEndToJson(cb_arg);
			WriteEndObjectToJson(cb_arg);
		}
	}

	LWLockRelease(pgss->lock);

	pfree(qbuffer);
}

/*
 * Parse in query text from disk during shared memory startup, and write that
 * text into the query dump file pointed to by qfile.
 */
static int
query_buffer_helper(FILE *file, FILE *qfile, int qlen,
					Size *query_offset, int encoding, Counters *counters,
					pgssYbReaderContext *context)
{
	/* Encoding is the only field we can easily sanity-check */
	if (!PG_VALID_BE_ENCODING(encoding))
		return -1;

	/* Resize buffer as needed */
	if (qlen >= *context->buffer_size)
	{
		*context->buffer_size = Max(*context->buffer_size * 2, qlen + 1);
		*context->buffer = repalloc(*context->buffer, *context->buffer_size);
	}

	if (fread(*context->buffer, 1, qlen + 1, file) != qlen + 1)
		return -1;

	/* Should have a trailing null, but let's make sure */
	(*context->buffer)[qlen] = '\0';

	/* Skip loading "sticky" entries */
	if ((counters->calls[PGSS_EXEC] + counters->calls[PGSS_PLAN]) == 0)
		return 0;

	/* Store the query text */
	*query_offset = pgss->extent;
	if (fwrite(*context->buffer, 1, qlen + 1, qfile) != qlen + 1)
		return -1;
	pgss->extent += qlen + 1;
	return 0;
}

/*
 * Parse in pgssEntries from disk during shared memory startup, for .stat files
 * generated by pre-histogram code.
 */
static int
read_entry_original(int header, FILE *file, FILE *qfile,
					pgssYbReaderContext *context)
{
	struct pgssEntry_original
	{
		pgssHashKey key;		/* hash key of entry - MUST BE FIRST */
		Counters	counters;	/* the statistics for this query */
		Size		query_offset;	/* query text offset in external file */
		int			query_len;	/* # of valid bytes in query string, or -1 */
		int			encoding;	/* query text encoding */
		slock_t		mutex;		/* protects the counters only */
	};

	Assert(header == 0x20171004);

	size_t		pgssEntry_size = sizeof(struct pgssEntry_original);

	for (int i = 0; i < context->num; i++)
	{
		struct pgssEntry_original temp;
		pgssEntry  *entry;
		Size		query_offset;

		int			qlen = 0;
		int			encoding = 0;
		pgssHashKey key;
		Counters	counters;

		if (fread(&temp, pgssEntry_size, 1, file) != 1)
			return -1;
		qlen = temp.query_len;
		encoding = temp.encoding;
		key = temp.key;
		counters = temp.counters;

		if (query_buffer_helper(file, qfile, qlen, &query_offset, encoding,
								&counters, context))
			return -1;

		entry = entry_alloc(&key, query_offset, qlen,
							encoding, false);

		/* copy in the actual stats */
		entry->counters = counters;
	}
	return 0;
}

/*
 * Parse in post-histogram pgssEntries from disk, throw out histogram parts if
 * config variables have changed between restarts.
 * File header version 0x20230330 can no longer be read as the file format has
 * changed between 0x20230330 and 0x20250425. The stats file is discarded in
 * pgss_shmem_startup() if the header is not 0x20250425.
 */
static int
read_entry_hdr(int header, FILE *file, FILE *qfile,
			   pgssYbReaderContext *context)
{
	Assert(header == 0x20250425);

	/* TODO: address case where hdr_histogram size changes due to 3p update */
	int			prev_entry_total_size = (sizeof(pgssEntry) +
										 context->temp_yb_hdr_count_bytes);
	char		prev_entry[prev_entry_total_size];

	for (int i = 0; i < context->num; i++)
	{
		pgssEntry  *temp = (pgssEntry *) prev_entry;
		pgssEntry  *entry;
		Size		query_offset;

		int			qlen = 0;
		int			encoding = 0;
		pgssHashKey key;
		Counters	counters;

		if (fread(prev_entry, prev_entry_total_size, 1, file) != 1)
			return -1;
		qlen = temp->query_len;
		encoding = temp->encoding;
		key = temp->key;
		counters = temp->counters;

		if (query_buffer_helper(file, qfile, qlen, &query_offset, encoding,
								&counters, context))
			return -1;

		entry = entry_alloc(&key, query_offset, qlen,
							encoding, false);

		/*
		 * only pass in old slow exec counts and old histograms if configs match
		 */
		if (context->hdr_config_match)
		{
			memcpy(&entry->yb_slow_executions,
				   &temp->yb_slow_executions,
				   sizeof(size_t) + sizeof(hdr_histogram) +
				   cfg.counts_len * sizeof(count_t));
		}
		/* copy in the actual stats */
		entry->counters = counters;
	}
	return 0;
}

/*
 * Post-histogram pg_stat_statements.stat files will have histogram config
 * variables to parse following the standard file header.
 */
static int
extended_header_reader(int header, FILE *file,
					   pgssYbReaderContext *context)
{
	if (header != 0x20250425)
		return -1;

	int64_t		temp_yb_hdr_max_value;
	int			temp_yb_hdr_count_bytes;
	int			temp_yb_hdr_bucket_factor;

	if (fread(&temp_yb_hdr_max_value, sizeof(int64_t), 1, file) != 1 ||
		fread(&temp_yb_hdr_count_bytes, sizeof(int), 1, file) != 1 ||
		fread(&temp_yb_hdr_bucket_factor, sizeof(int), 1, file) != 1)
		return -1;

	context->temp_yb_hdr_count_bytes = temp_yb_hdr_count_bytes;
	context->hdr_config_match = (temp_yb_hdr_max_value == yb_hdr_max_value &&
								 temp_yb_hdr_count_bytes == cfg.counts_len * sizeof(count_t) &&
								 temp_yb_hdr_bucket_factor == yb_hdr_bucket_factor);
	return 0;
}

typedef struct pgssYbReader
{
	uint32		header;
	int			(*extended_header_reader) (int, FILE *, pgssYbReaderContext *context);
	int			(*entry_reader) (int, FILE *, FILE *, pgssYbReaderContext *context);
} pgssYbReader;

/*
 * Array of pgssEntry deserialization functions, organized by header member.
 * Future contributors should add a row here and write a read_entry_[feature]()
 * if they want to parse in their new versions of pgssEntry.
 * IMPORTANT: this approach has no resilience against changes to Counters or
 * pgssHashKey. If those structures changed between pgss upgrades, the proper
 * offsets would be lost.
 */
static const int pgssReaderEndMarker = -1;
pgssYbReader pgssReaderList[] =
{
	{0x20171004, NULL, read_entry_original},
	{0x20230330, extended_header_reader, read_entry_hdr},
	{0x20250425, extended_header_reader, read_entry_hdr},
	{pgssReaderEndMarker, NULL, NULL}
};

static void
enforce_bucket_factor(int *value)
{
	if (*value != 8 && *value != 16 && *value != 32)
	{
		elog(LOG, "Unsupported yb_hdr_bucket_factor, defaulting to %d",
			 YB_HDR_DEFAULT_BUCKET_FACTOR);
		*value = YB_HDR_DEFAULT_BUCKET_FACTOR;
	}
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing statistics from file.
 * Also create and load the query-texts file, which is expected to exist
 * (even if empty) while the module is enabled.
 */
static void
pgss_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;
	FILE	   *file = NULL;
	FILE	   *qfile = NULL;
	uint32		header;
	int32		num;
	int32		pgver;

	int			buffer_size;
	char	   *buffer = NULL;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	RegisterGetYsqlStatStatements(&getYsqlStatementStats);
	RegisterResetYsqlStatStatements(&resetYsqlStatementStats);

	/* reset in case this is a restart within the postmaster */
	pgss = NULL;
	pgss_hash = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgss = ShmemInitStruct("pg_stat_statements",
						   sizeof(pgssSharedState),
						   &found);

	if (!found)
	{
		/* First time through ... */
		pgss->lock = &(GetNamedLWLockTranche("pg_stat_statements"))->lock;
		pgss->cur_median_usage = ASSUMED_MEDIAN_INIT;
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;
		SpinLockInit(&pgss->mutex);
		pgss->extent = 0;
		pgss->n_writers = 0;
		pgss->gc_count = 0;
		pgss->stats.dealloc = 0;
		pgss->stats.stats_reset = GetCurrentTimestamp();
	}

	info.keysize = sizeof(pgssHashKey);
	info.entrysize = sizeof(pgssEntry) + cfg.counts_len * sizeof(count_t);
	pgss_hash = ShmemInitHash("pg_stat_statements hash",
							  pgss_max, pgss_max,
							  &info,
							  HASH_ELEM | HASH_BLOBS);

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(pgss_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 */
	if (found)
		return;

	/*
	 * Note: we don't bother with locks here, because there should be no other
	 * processes running when this code is reached.
	 */

	/* Unlink query text file possibly left over from crash */
	unlink(PGSS_TEXT_FILE);

	/* Allocate new query text temp file */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
		goto write_error;

	/*
	 * If we were told not to load old statistics, we're done.  (Note we do
	 * not try to unlink any old dump file in this case.  This seems a bit
	 * questionable but it's the historical behavior.)
	 */
	if (!pgss_save)
	{
		FreeFile(qfile);
		return;
	}

	/*
	 * Attempt to load old statistics from the dump file.
	 */
	file = AllocateFile(PGSS_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		/* No existing persisted stats file, so we're done */
		FreeFile(qfile);
		return;
	}

	buffer_size = 2048;
	buffer = (char *) palloc(buffer_size);

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&pgver, sizeof(uint32), 1, file) != 1 ||
		fread(&num, sizeof(int32), 1, file) != 1)
		goto read_error;

	if (header != PGSS_FILE_HEADER ||
		pgver != PGSS_PG_MAJOR_VERSION)
		goto data_error;

	pgssYbReader *version_reader = NULL;
	int			i = 0;

	while (pgssReaderList[i].header != pgssReaderEndMarker)
	{
		if (header == pgssReaderList[i].header)
		{
			version_reader = &pgssReaderList[i];
			break;
		}
		i++;
	}

	if (version_reader == NULL)
		goto read_error;

	pgssYbReaderContext context;

	context.num = num;
	context.buffer_size = &buffer_size;
	context.buffer = &buffer;
	context.hdr_config_match = false;
	if (version_reader->extended_header_reader != NULL)
	{
		if (version_reader->extended_header_reader(header, file, &context))
			goto read_error;
	}
	if (version_reader->entry_reader(header, file, qfile, &context))
		goto read_error;

	/* Read global statistics for pg_stat_statements */
	if (fread(&pgss->stats, sizeof(pgssGlobalStats), 1, file) != 1)
		goto read_error;

	pfree(buffer);
	FreeFile(file);
	FreeFile(qfile);

	/*
	 * Remove the persisted stats file so it's not included in
	 * backups/replication standbys, etc.  A new file will be written on next
	 * shutdown.
	 *
	 * Note: it's okay if the PGSS_TEXT_FILE is included in a basebackup,
	 * because we remove that file on startup; it acts inversely to
	 * PGSS_DUMP_FILE, in that it is only supposed to be around when the
	 * server is running, whereas PGSS_DUMP_FILE is only supposed to be around
	 * when the server is not running.  Leaving the file creates no danger of
	 * a newly restored database having a spurious record of execution costs,
	 * which is what we're really concerned about here.
	 */
	unlink(PGSS_DUMP_FILE);

	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read file \"%s\": %m",
					PGSS_DUMP_FILE)));
	goto fail;
data_error:
	ereport(LOG,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("ignoring invalid data in file \"%s\"",
					PGSS_DUMP_FILE)));
	goto fail;
write_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_TEXT_FILE)));
fail:
	if (buffer)
		pfree(buffer);
	if (file)
		FreeFile(file);
	if (qfile)
		FreeFile(qfile);
	/* If possible, throw away the bogus file; ignore any error */
	unlink(PGSS_DUMP_FILE);

	/*
	 * Don't unlink PGSS_TEXT_FILE here; it should always be around while the
	 * server is running with pg_stat_statements enabled
	 */
}

/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
pgss_shmem_shutdown(int code, Datum arg)
{
	FILE	   *file;
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	HASH_SEQ_STATUS hash_seq;
	int32		num_entries;
	pgssEntry  *entry;

	/* Don't try to dump during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!pgss || !pgss_hash)
		return;

	/* Don't dump if told not to. */
	if (!pgss_save)
		return;

	file = AllocateFile(PGSS_DUMP_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&PGSS_FILE_HEADER, sizeof(uint32), 1, file) != 1)
		goto error;
	if (fwrite(&PGSS_PG_MAJOR_VERSION, sizeof(uint32), 1, file) != 1)
		goto error;
	num_entries = hash_get_num_entries(pgss_hash);
	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;
	int			yb_hdr_count_bytes = cfg.counts_len * sizeof(count_t);

	if (fwrite(&yb_hdr_max_value, sizeof(int64_t), 1, file) != 1)
		goto error;
	if (fwrite(&yb_hdr_count_bytes, sizeof(int), 1, file) != 1)
		goto error;
	if (fwrite(&yb_hdr_bucket_factor, sizeof(int), 1, file) != 1)
		goto error;

	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		goto error;

	/*
	 * When serializing to disk, we store query texts immediately after their
	 * entry data.  Any orphaned query texts are thereby excluded.
	 */
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			len = entry->query_len;
		char	   *qstr = qtext_fetch(entry->query_offset, len,
									   qbuffer, qbuffer_size);

		if (qstr == NULL)
			continue;			/* Ignore any entries with bogus texts */

		if (fwrite(entry, sizeof(pgssEntry) + cfg.counts_len * sizeof(count_t),
				   1, file) != 1 ||
			fwrite(qstr, 1, len + 1, file) != len + 1)
		{
			/* note: we assume hash_seq_term won't change errno */
			hash_seq_term(&hash_seq);
			goto error;
		}
	}

	/* Dump global statistics for pg_stat_statements */
	if (fwrite(&pgss->stats, sizeof(pgssGlobalStats), 1, file) != 1)
		goto error;

	pfree(qbuffer);
	qbuffer = NULL;

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	/*
	 * Rename file into place, so we atomically replace any old one.
	 */
	(void) durable_rename(PGSS_DUMP_FILE ".tmp", PGSS_DUMP_FILE, LOG);

	/* Unlink query-texts file; it's not needed while shutdown */
	unlink(PGSS_TEXT_FILE);

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_DUMP_FILE ".tmp")));
	if (qbuffer)
		pfree(qbuffer);
	if (file)
		FreeFile(file);
	unlink(PGSS_DUMP_FILE ".tmp");
	unlink(PGSS_TEXT_FILE);
}

/*
 * YB: Lazily initialize the session-local hash set that tracks which queryIds
 * need normalization.
 *
 * There is no extension hook at connection startup to allocate this table, and
 * many backends never execute a query with normalizable constants, so we create
 * the hash on first use rather than at shared-memory attach time.
 */
static void
yb_pgss_init_normalization_set(void)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(YbPgssQueryNeedingNormalization);
	ctl.entrysize = sizeof(YbPgssQueryNeedingNormalization);
	ctl.hcxt = TopMemoryContext;
	yb_pgss_queries_needing_normalization = hash_create("YB pgss queries needing normalization",
												16,
												&ctl,
												HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void
yb_pgss_mark_needs_normalization(uint64 queryId)
{
	if (!yb_pgss_queries_needing_normalization)
		yb_pgss_init_normalization_set();
	hash_search(yb_pgss_queries_needing_normalization, &queryId, HASH_ENTER, NULL);
}

static bool
yb_pgss_needs_normalization(uint64 queryId)
{
	if (!yb_pgss_queries_needing_normalization)
		return false;
	return hash_search(yb_pgss_queries_needing_normalization, &queryId,
					   HASH_FIND, NULL) != NULL;
}

/*
 * Post-parse-analysis hook: mark query with a queryId
 */
static void
pgss_post_parse_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	/*
	 * YB: Mark before the tracking check.  A query parsed while it is not
	 * tracked, such as one prepared under track = 'none' or nested under
	 * track = 'top', can still be executed while tracked.
	 */
	if (jstate && jstate->clocations_count > 0)
		yb_pgss_mark_needs_normalization(query->queryId);

	/* Safety check... */
	if (!pgss || !pgss_hash || !pgss_enabled(exec_nested_level))
		return;

	/*
	 * Clear queryId for prepared statements related utility, as those will
	 * inherit from the underlying statement's one (except DEALLOCATE which is
	 * entirely untracked).
	 */
	if (query->utilityStmt)
	{
		if (pgss_track_utility && !PGSS_HANDLED_UTILITY(query->utilityStmt))
			query->queryId = UINT64CONST(0);
		return;
	}

	/*
	 * If query jumbling were able to identify any ignorable constants, we
	 * immediately create a hash table entry for the query, so that we can
	 * record the normalized form of the query string.  If there were no such
	 * constants, the normalized string would be the same as the query text
	 * anyway, so there's no need for an early entry.
	 */
	if (jstate && jstate->clocations_count > 0)
		pgss_store(pstate->p_sourcetext,
				   query->queryId,
				   query->stmt_location,
				   query->stmt_len,
				   PGSS_INVALID,
				   0,
				   0,
				   NULL,
				   NULL,
				   NULL,
				   jstate,
				   false /* yb_is_sensitive_stmt */ ,
				   NULL /* yb_stats */ );
}

/*
 * Planner hook: forward to regular planner, but measure planning time
 * if needed.
 */
static PlannedStmt *
pgss_planner(Query *parse,
			 const char *query_string,
			 int cursorOptions,
			 ParamListInfo boundParams)
{
	PlannedStmt *result;

	/*
	 * We can't process the query if no query_string is provided, as
	 * pgss_store needs it.  We also ignore query without queryid, as it would
	 * be treated as a utility statement, which may not be the case.
	 *
	 * Note that planner_hook can be called from the planner itself, so we
	 * have a specific nesting level for the planner.  However, utility
	 * commands containing optimizable statements can also call the planner,
	 * same for regular DML (for instance for underlying foreign key queries).
	 * So testing the planner nesting level only is not enough to detect real
	 * top level planner call.
	 */
	if (pgss_enabled(plan_nested_level + exec_nested_level)
		&& pgss_track_planning && query_string
		&& parse->queryId != UINT64CONST(0))
	{
		instr_time	start;
		instr_time	duration;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		/* We need to track buffer usage as the planner can access them. */
		bufusage_start = pgBufferUsage;

		/*
		 * Similarly the planner could write some WAL records in some cases
		 * (e.g. setting a hint bit with those being WAL-logged)
		 */
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		plan_nested_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions,
										   boundParams);
			else
				result = standard_planner(parse, query_string, cursorOptions,
										  boundParams);
		}
		PG_FINALLY();
		{
			plan_nested_level--;
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/* calc differences of buffer counters. */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters. */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

		pgss_store(query_string,
				   parse->queryId,
				   parse->stmt_location,
				   parse->stmt_len,
				   PGSS_PLAN,
				   INSTR_TIME_GET_MILLISEC(duration),
				   0,
				   &bufusage,
				   &walusage,
				   NULL,
				   NULL,
				   false /* yb_is_sensitive_stmt */ ,
				   NULL /* yb_stats */ );
	}
	else
	{
		if (prev_planner_hook)
			result = prev_planner_hook(parse, query_string, cursorOptions,
									   boundParams);
		else
			result = standard_planner(parse, query_string, cursorOptions,
									  boundParams);
	}

	return result;
}

/*
 * ExecutorStart hook: start up tracking if needed
 */
static void
pgss_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * If query has queryId zero, don't track it.  This prevents double
	 * counting of optimizable statements that are directly contained in
	 * utility statements.
	 */
	if (pgss_enabled(exec_nested_level) && queryDesc->plannedstmt->queryId != UINT64CONST(0))
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
				 bool execute_once)
{
	exec_nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
	}
	PG_FINALLY();
	{
		exec_nested_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorFinish(QueryDesc *queryDesc)
{
	exec_nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		exec_nested_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: store results if needed
 */
static void
pgss_ExecutorEnd(QueryDesc *queryDesc)
{
	uint64		queryId = queryDesc->plannedstmt->queryId;

	if (queryId != UINT64CONST(0) && queryDesc->totaltime &&
		pgss_enabled(exec_nested_level))
	{
		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);

		pgss_store(queryDesc->sourceText,
				   queryId,
				   queryDesc->plannedstmt->stmt_location,
				   queryDesc->plannedstmt->stmt_len,
				   PGSS_EXEC,
				   queryDesc->totaltime->total * 1000.0,	/* convert to msec */
				   queryDesc->estate->es_processed,
				   &queryDesc->totaltime->bufusage,
				   &queryDesc->totaltime->walusage,
				   queryDesc->estate->es_jit ? &queryDesc->estate->es_jit->instr : NULL,
				   NULL,
				   false /* yb_is_sensitive_stmt */ ,
				   &queryDesc->yb_query_stats->yb_instr);
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ProcessUtility hook
 */
static void
pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree,
					ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	uint64		saved_queryId = pstmt->queryId;
	int			saved_stmt_location = pstmt->stmt_location;
	int			saved_stmt_len = pstmt->stmt_len;

	bool		yb_is_backfill = IsA(parsetree, YbBackfillIndexStmt);

	/*
	 * Force utility statements to get queryId zero.  We do this even in cases
	 * where the statement contains an optimizable statement for which a
	 * queryId could be derived (such as EXPLAIN or DECLARE CURSOR).  For such
	 * cases, runtime control will first go through ProcessUtility and then
	 * the executor, and we don't want the executor hooks to do anything,
	 * since we are already measuring the statement's costs at the utility
	 * level.
	 *
	 * Note that this is only done if pg_stat_statements is enabled and
	 * configured to track utility statements, in the unlikely possibility
	 * that user configured another extension to handle utility statements
	 * only.
	 */
	if (pgss_enabled(exec_nested_level) && pgss_track_utility)
		pstmt->queryId = UINT64CONST(0);

	/*
	 * If it's an EXECUTE statement, we don't track it and don't increment the
	 * nesting level.  This allows the cycles to be charged to the underlying
	 * PREPARE instead (by the Executor hooks), which is much more useful.
	 *
	 * We also don't track execution of PREPARE.  If we did, we would get one
	 * hash table entry for the PREPARE (with hash calculated from the query
	 * string), and then a different one with the same query string (but hash
	 * calculated from the query tree) would be used to accumulate costs of
	 * ensuing EXECUTEs.  This would be confusing, and inconsistent with other
	 * cases where planning time is not included at all.
	 *
	 * Likewise, we don't track execution of DEALLOCATE.
	 */
	if (pgss_track_utility && pgss_enabled(exec_nested_level) &&
		PGSS_HANDLED_UTILITY(parsetree))
	{
		instr_time	start;
		instr_time	duration;
		uint64		rows;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		bufusage_start = pgBufferUsage;
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		exec_nested_level++;
		PG_TRY();
		{
			if (prev_ProcessUtility)
				prev_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
			else
				standard_ProcessUtility(pstmt, queryString, readOnlyTree,
										context, params, queryEnv,
										dest, qc);
		}
		PG_FINALLY();
		{
			exec_nested_level--;
		}
		PG_END_TRY();

		/*
		 * CAUTION: do not access the *pstmt data structure again below here.
		 * If it was a ROLLBACK or similar, that data structure may have been
		 * freed.  We must copy everything we still need into local variables,
		 * which we did above.
		 *
		 * For the same reason, we can't risk restoring pstmt->queryId to its
		 * former value, which'd otherwise be a good idea.
		 */

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/*
		 * Track the total number of rows retrieved or affected by the utility
		 * statements of COPY, FETCH, CREATE TABLE AS, CREATE MATERIALIZED
		 * VIEW, REFRESH MATERIALIZED VIEW and SELECT INTO.
		 */
		rows = (qc && (qc->commandTag == CMDTAG_COPY ||
					   qc->commandTag == CMDTAG_FETCH ||
					   qc->commandTag == CMDTAG_SELECT ||
					   qc->commandTag == CMDTAG_REFRESH_MATERIALIZED_VIEW)) ?
			qc->nprocessed : 0;

		/* calc differences of buffer counters. */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters. */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

		/*
		 * YB note: UTILITY statements are the only kind that are treated as
		 * sensitive. The other pgss hooks (planner, analyze, end-of-execution)
		 * are not invoked for utility statements.
		 *
		 * For BACKFILL INDEX commands, generate a normalized query string so
		 * that all backfill calls for the same index are aggregated.
		 */
		if (yb_is_backfill)
		{
			char	   *norm_query;
			int			norm_query_len;

			YbBackfillIndexStmt *bf_stmt =
				(YbBackfillIndexStmt *) parsetree;
			norm_query = yb_generate_normalized_backfill_query(bf_stmt, &norm_query_len);
			pgss_store(norm_query,
					   saved_queryId,
					   0,
					   norm_query_len,
					   PGSS_EXEC,
					   INSTR_TIME_GET_MILLISEC(duration),
					   rows,
					   &bufusage,
					   &walusage,
					   NULL,
					   NULL,
					   false /* yb_is_sensitive_stmt */ ,
					   YBGetUtilityOperationStats());
			pfree(norm_query);
		}
		else
		{
			pgss_store(queryString,
					   saved_queryId,
					   saved_stmt_location,
					   saved_stmt_len,
					   PGSS_EXEC,
					   INSTR_TIME_GET_MILLISEC(duration),
					   rows,
					   &bufusage,
					   &walusage,
					   NULL,
					   NULL,
					   true /* yb_is_sensitive_stmt */ ,
					   YBGetUtilityOperationStats());
		}
	}
	else
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
	}
}

/*
 * Store some statistics for a statement.
 *
 * If jstate is not NULL then we're trying to create an entry for which
 * we have no statistics as yet; we just want to record the normalized
 * query string.  total_time, rows, bufusage and walusage are ignored in this
 * case.
 *
 * If kind is PGSS_PLAN or PGSS_EXEC, its value is used as the array position
 * for the arrays in the Counters field.
 */
static void
pgss_store(const char *query, uint64 queryId,
		   int query_location, int query_len,
		   pgssStoreKind kind,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage,
		   const WalUsage *walusage,
		   const struct JitInstrumentation *jitusage,
		   JumbleState *jstate,
		   bool yb_is_sensitive_stmt,
		   YbInstrumentation *yb_stats)
{
	pgssHashKey key;
	pgssEntry  *entry;
	char	   *norm_query = NULL;
	int			encoding = GetDatabaseEncoding();

	Assert(query != NULL);

	/* Safety check... */
	if (!pgss || !pgss_hash)
		return;

	if (yb_is_calling_internal_sql_for_ddl)
		return;

	/*
	 * Nothing to do if compute_query_id isn't enabled and no other module
	 * computed a query identifier.
	 */
	if (queryId == UINT64CONST(0))
	{
		return;
	}

	/*
	 * Confine our attention to the relevant part of the string, if the query
	 * is a portion of a multi-statement source string, and update query
	 * location and length if needed.
	 */
	query = CleanQuerytext(query, &query_location, &query_len);

	/*
	 * The query string may include multiple statements, so consider only the
	 * substring that we are interested in for redaction. Note that the
	 * substring in question does not contain a semi-colon at the end.
	 */
	if (yb_is_sensitive_stmt)
		query = YbGetRedactedQueryString(pnstrdup(query, query_len), &query_len);

	if (yb_enable_query_diagnostics && !jstate)
		YbQueryDiagnosticsAccumulatePgss(queryId, (YbQdPgssStoreKind) kind,
										 total_time, rows, bufusage, walusage, jitusage);

	/* Set up key for hashtable search */

	/* clear padding */
	memset(&key, 0, sizeof(pgssHashKey));

	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.queryid = queryId;
	key.toplevel = (exec_nested_level == 0);

	/* Lookup the hash table entry with shared lock. */
	LWLockAcquire(pgss->lock, LW_SHARED);

	entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_FIND, NULL);

	/* Create new entry, if not present */
	if (!entry)
	{
		Size		query_offset;
		int			gc_count;
		bool		stored;
		bool		do_gc;

		/*
		 * Create a new, normalized query string if caller asked.  We don't
		 * need to hold the lock while doing this work.  (Note: in any case,
		 * it's possible that someone else creates a duplicate hashtable entry
		 * in the interval where we don't hold the lock below.  That case is
		 * handled by entry_alloc.)
		 */
		if (jstate)
		{
			LWLockRelease(pgss->lock);
			norm_query = generate_normalized_query(jstate, query,
												   query_location,
												   &query_len);
			LWLockAcquire(pgss->lock, LW_SHARED);
		}
		else if (yb_pgss_needs_normalization(queryId))
		{
			/*
			 * YB: This query has constants that require normalization, but its
			 * shared hash table entry has been evicted or reset.  We can't
			 * normalize without jstate, so discard the execution stats rather
			 * than storing unnormalized literals.  The entry will be re-created
			 * with proper normalization on the next full parse cycle.
			 */
			goto done;
		}

		/*
		 * YB: Enforce yb_max_query_text_length.  The truncated copy takes the
		 * place of norm_query so that the code below stores and frees it.
		 */
		if (yb_max_query_text_length >= 0 &&
			query_len > yb_max_query_text_length)
		{
			char	   *truncated = yb_truncate_query_text(norm_query ? norm_query : query,
														   &query_len, encoding);

			if (norm_query)
				pfree(norm_query);
			norm_query = truncated;
		}

		/* Append new query text to file with only shared lock held */
		stored = qtext_store(norm_query ? norm_query : query, query_len,
							 &query_offset, &gc_count);

		/*
		 * Determine whether we need to garbage collect external query texts
		 * while the shared lock is still held.  This micro-optimization
		 * avoids taking the time to decide this while holding exclusive lock.
		 */
		do_gc = need_gc_qtexts();

		/* Need exclusive lock to make a new hashtable entry - promote */
		LWLockRelease(pgss->lock);
		LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

		/*
		 * A garbage collection may have occurred while we weren't holding the
		 * lock.  In the unlikely event that this happens, the query text we
		 * stored above will have been garbage collected, so write it again.
		 * This should be infrequent enough that doing it while holding
		 * exclusive lock isn't a performance problem.
		 */
		if (!stored || pgss->gc_count != gc_count)
			stored = qtext_store(norm_query ? norm_query : query, query_len,
								 &query_offset, NULL);

		/* If we failed to write to the text file, give up */
		if (!stored)
			goto done;

		/* OK to create a new hashtable entry */
		entry = entry_alloc(&key, query_offset, query_len, encoding,
							jstate != NULL);

		/* If needed, perform garbage collection while exclusive lock held */
		if (do_gc)
			gc_qtexts();
	}

	if (yb_enable_query_diagnostics)
		YbSetPgssNormalizedQueryText(queryId, entry->query_offset,
									 entry->query_len);

	/* Increment the counts, except when jstate is not NULL */
	if (!jstate)
	{
		/*
		 * Grab the spinlock while updating the counters (see comment about
		 * locking rules at the head of the file)
		 */
		volatile pgssEntry *e = (volatile pgssEntry *) entry;

		Assert(kind == PGSS_PLAN || kind == PGSS_EXEC);

		SpinLockAcquire(&e->mutex);

		/* "Unstick" entry if it was previously sticky */
		if (IS_STICKY(e->counters))
			e->counters.usage = USAGE_INIT;

		e->counters.calls[kind] += 1;
		e->counters.total_time[kind] += total_time;

		/*
		 * Add record for only execution time for queries in latency
		 * histograms
		 */
		if (IsYugaByteEnabled() && kind == PGSS_EXEC)
		{
			int64_t		tt_int = (int64_t) (total_time / yb_hdr_latency_res_ms);

			if (tt_int < yb_hdr_max_value)
				hdr_record_value((hdr_histogram *) &e->yb_hdr_histogram, tt_int);
			else
				e->yb_slow_executions++;
		}


		if (e->counters.calls[kind] == 1)
		{
			e->counters.min_time[kind] = total_time;
			e->counters.max_time[kind] = total_time;
			e->counters.mean_time[kind] = total_time;
		}
		else
		{
			/*
			 * Welford's method for accurately computing variance. See
			 * <http://www.johndcook.com/blog/standard_deviation/>
			 */
			double		old_mean = e->counters.mean_time[kind];

			e->counters.mean_time[kind] +=
				(total_time - old_mean) / e->counters.calls[kind];
			e->counters.sum_var_time[kind] +=
				(total_time - old_mean) * (total_time - e->counters.mean_time[kind]);

			/* calculate min and max time */
			if (e->counters.min_time[kind] > total_time)
				e->counters.min_time[kind] = total_time;
			if (e->counters.max_time[kind] < total_time)
				e->counters.max_time[kind] = total_time;
		}
		e->counters.rows += rows;
		e->counters.shared_blks_hit += bufusage->shared_blks_hit;
		e->counters.shared_blks_read += bufusage->shared_blks_read;
		e->counters.shared_blks_dirtied += bufusage->shared_blks_dirtied;
		e->counters.shared_blks_written += bufusage->shared_blks_written;
		e->counters.local_blks_hit += bufusage->local_blks_hit;
		e->counters.local_blks_read += bufusage->local_blks_read;
		e->counters.local_blks_dirtied += bufusage->local_blks_dirtied;
		e->counters.local_blks_written += bufusage->local_blks_written;
		e->counters.temp_blks_read += bufusage->temp_blks_read;
		e->counters.temp_blks_written += bufusage->temp_blks_written;
		e->counters.blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->blk_read_time);
		e->counters.blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->blk_write_time);
		e->counters.temp_blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->temp_blk_read_time);
		e->counters.temp_blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->temp_blk_write_time);
		e->counters.usage += USAGE_EXEC(total_time);
		e->counters.wal_records += walusage->wal_records;
		e->counters.wal_fpi += walusage->wal_fpi;
		e->counters.wal_bytes += walusage->wal_bytes;
		if (jitusage)
		{
			e->counters.jit_functions += jitusage->created_functions;
			e->counters.jit_generation_time += INSTR_TIME_GET_MILLISEC(jitusage->generation_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->inlining_counter))
				e->counters.jit_inlining_count++;
			e->counters.jit_inlining_time += INSTR_TIME_GET_MILLISEC(jitusage->inlining_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->optimization_counter))
				e->counters.jit_optimization_count++;
			e->counters.jit_optimization_time += INSTR_TIME_GET_MILLISEC(jitusage->optimization_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->emission_counter))
				e->counters.jit_emission_count++;
			e->counters.jit_emission_time += INSTR_TIME_GET_MILLISEC(jitusage->emission_counter);
		}

		if (kind == PGSS_EXEC)
		{
			if (yb_stats)
			{
				e->counters.yb_counters.counters[YB_INT_DOCDB_READ_RPCS] +=
					yb_stats->tbl_reads.count + yb_stats->index_reads.count;
				e->counters.yb_counters.counters[YB_INT_DOCDB_READ_OPS] +=
					yb_stats->tbl_reads.ops_count + yb_stats->index_reads.ops_count;
				e->counters.yb_counters.counters[YB_INT_DOCDB_WRITE_RPCS] +=
					yb_stats->write_flushes.count;
				e->counters.yb_counters.counters[YB_INT_DOCDB_WRITE_OPS] +=
					yb_stats->tbl_writes + yb_stats->index_writes;
				e->counters.yb_counters.counters[YB_INT_DOCDB_ROWS_SCANNED] +=
					yb_stats->tbl_reads.rows_scanned + yb_stats->index_reads.rows_scanned;
				e->counters.yb_counters.counters[YB_INT_DOCDB_ROWS_RETURNED] +=
					yb_stats->tbl_reads.rows_received + yb_stats->index_reads.rows_received;

				e->counters.yb_counters.counters_dbl[YB_DBL_CATALOG_WAIT_TIME_MS] +=
					(yb_stats->catalog_reads.wait_time) / 1000000.0;
				e->counters.yb_counters.counters_dbl[YB_DBL_DOCDB_WAIT_TIME_MS] +=
					(yb_stats->tbl_reads.wait_time + yb_stats->write_flushes.wait_time +
					 yb_stats->index_reads.wait_time) / 1000000.0;
			}

			/* Update retry statistics, only after the statement has completed */
			e->counters.yb_counters.counters[YB_INT_CONFLICT_RETRIES] +=
				YbGetRetryCount(YB_TXN_CONFLICT);
			e->counters.yb_counters.counters[YB_INT_READ_RESTART_RETRIES] +=
				YbGetRetryCount(YB_TXN_RESTART_READ);
			e->counters.yb_counters.counters[YB_INT_TOTAL_RETRIES] += YbGetTotalRetryCount();

			if (yb_stats && yb_stats->read_metrics.version)
			{
				e->counters.yb_counters.counters[YB_INT_DOCDB_OBSOLETE_ROWS_SCANNED] +=
					yb_stats->read_metrics.counters[YB_STORAGE_COUNTER_DOCDB_OBSOLETE_KEYS_FOUND];
				e->counters.yb_counters.counters[YB_INT_DOCDB_SEEKS] +=
					yb_stats->read_metrics.gauges[YB_STORAGE_GAUGE_REGULARDB_NUMBER_DB_SEEK];
				e->counters.yb_counters.counters[YB_INT_DOCDB_NEXTS] +=
					yb_stats->read_metrics.gauges[YB_STORAGE_GAUGE_REGULARDB_NUMBER_DB_NEXT];
				e->counters.yb_counters.counters[YB_INT_DOCDB_PREVS] +=
					yb_stats->read_metrics.gauges[YB_STORAGE_GAUGE_REGULARDB_NUMBER_DB_PREV];
				e->counters.yb_counters.counters_dbl[YB_DBL_DOCDB_READ_TIME_MS] +=
					yb_stats->read_metrics.events[YB_STORAGE_EVENT_QL_READ_LATENCY].sum / 1000.0;
			}

			if (yb_stats && yb_stats->write_metrics.version)
			{
				e->counters.yb_counters.counters[YB_INT_DOCDB_OBSOLETE_ROWS_SCANNED] +=
					yb_stats->write_metrics.counters[YB_STORAGE_COUNTER_DOCDB_OBSOLETE_KEYS_FOUND];
				e->counters.yb_counters.counters[YB_INT_DOCDB_SEEKS] +=
					yb_stats->write_metrics.gauges[YB_STORAGE_GAUGE_REGULARDB_NUMBER_DB_SEEK];
				e->counters.yb_counters.counters[YB_INT_DOCDB_NEXTS] +=
					yb_stats->write_metrics.gauges[YB_STORAGE_GAUGE_REGULARDB_NUMBER_DB_NEXT];
				e->counters.yb_counters.counters[YB_INT_DOCDB_PREVS] +=
					yb_stats->write_metrics.gauges[YB_STORAGE_GAUGE_REGULARDB_NUMBER_DB_PREV];
				e->counters.yb_counters.counters_dbl[YB_DBL_DOCDB_WRITE_TIME_MS] +=
					yb_stats->write_metrics.events[YB_STORAGE_EVENT_QL_WRITE_LATENCY].sum / 1000.0;
			}
		}

		SpinLockRelease(&e->mutex);
	}

done:
	LWLockRelease(pgss->lock);

	/* We postpone this clean-up until we're out of the lock */
	if (norm_query)
		pfree(norm_query);
}

/*
 * Reset statement statistics corresponding to userid, dbid, and queryid.
 */
Datum
pg_stat_statements_reset_1_7(PG_FUNCTION_ARGS)
{
	if (yb_enable_query_diagnostics)
		*yb_pgss_last_reset_time = GetCurrentTimestamp();

	Oid			userid;
	Oid			dbid;
	uint64		queryid;

	userid = PG_GETARG_OID(0);
	dbid = PG_GETARG_OID(1);
	queryid = (uint64) PG_GETARG_INT64(2);

	entry_reset(userid, dbid, queryid);

	PG_RETURN_VOID();
}

/*
 * Reset statement statistics.
 */
Datum
pg_stat_statements_reset(PG_FUNCTION_ARGS)
{
	if (yb_enable_query_diagnostics)
		*yb_pgss_last_reset_time = GetCurrentTimestamp();

	entry_reset(0, 0, 0);

	PG_RETURN_VOID();
}

/* Number of output arguments (columns) for various API versions */
#define PG_STAT_STATEMENTS_COLS_V1_0	14
#define PG_STAT_STATEMENTS_COLS_V1_1	18
#define PG_STAT_STATEMENTS_COLS_V1_2	19
#define PG_STAT_STATEMENTS_COLS_V1_3	23
#define YB_PG_STAT_STATEMENTS_COLS_V1_4	24
#define PG_STAT_STATEMENTS_COLS_V1_8	32
#define PG_STAT_STATEMENTS_COLS_V1_9	33
#define PG_STAT_STATEMENTS_COLS_V1_10	43
#define YB_PG_STAT_STATEMENTS_COLS_V1_10	44
#define YB_V2_0_PG_STAT_STATEMENTS_COLS_V1_10	55
#define YB_V2_1_PG_STAT_STATEMENTS_COLS_V1_10	61
#define PG_STAT_STATEMENTS_COLS			61	/* maximum of above */

/*
 * Retrieve statement statistics.
 *
 * The SQL API of this function has changed multiple times, and will likely
 * do so again in future.  To support the case where a newer version of this
 * loadable module is being used with an old SQL declaration of the function,
 * we continue to support the older API versions.  For 1.2 and later, the
 * expected API version is identified by embedding it in the C name of the
 * function.  Unfortunately we weren't bright enough to do that for 1.1.
 */
Datum
yb_pg_stat_statements_1_4(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, YB_PGSS_V1_4, showtext);

	return (Datum) 0;
}

Datum
yb_2_0_pg_stat_statements_1_10(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, YB_V2_0_PGSS_V1_10, showtext);

	return (Datum) 0;
}

Datum
yb_2_1_pg_stat_statements_1_10(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, YB_V2_1_PGSS_V1_10, showtext);

	return (Datum) 0;
}

Datum
yb_pg_stat_statements_1_10(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, YB_PGSS_V1_10, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_10(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_10, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_9(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_9, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_8(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_8, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_3(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_3, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_2(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_2, showtext);

	return (Datum) 0;
}

/*
 * Legacy entry point for pg_stat_statements() API versions 1.0 and 1.1.
 * This can be removed someday, perhaps.
 */
Datum
pg_stat_statements(PG_FUNCTION_ARGS)
{
	/* If it's really API 1.1, we'll figure that out below */
	pg_stat_statements_internal(fcinfo, PGSS_V1_0, true);

	return (Datum) 0;
}

/* Common code for all versions of pg_stat_statements() */
static void
pg_stat_statements_internal(FunctionCallInfo fcinfo,
							pgssVersion api_version,
							bool showtext)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			userid = GetUserId();
	bool		is_allowed_role = false;
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	Size		extent = 0;
	int			gc_count = 0;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;

	/*
	 * Superusers or roles with the privileges of pg_read_all_stats members
	 * are allowed
	 */
	is_allowed_role = has_privs_of_role(userid, ROLE_PG_READ_ALL_STATS);

	/* hash table must exist already */
	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via shared_preload_libraries")));

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Check we have the expected number of output arguments.  Aside from
	 * being a good safety check, we need a kluge here to detect API version
	 * 1.1, which was wedged into the code in an ill-considered way.
	 */
	switch (rsinfo->setDesc->natts)
	{
		case PG_STAT_STATEMENTS_COLS_V1_0:
			if (api_version != PGSS_V1_0)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_1:
			/* pg_stat_statements() should have told us 1.0 */
			if (api_version != PGSS_V1_0)
				elog(ERROR, "incorrect number of output arguments");
			api_version = PGSS_V1_1;
			break;
		case PG_STAT_STATEMENTS_COLS_V1_2:
			if (api_version != PGSS_V1_2)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_3:
			if (api_version != PGSS_V1_3)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_8:
			if (api_version != PGSS_V1_8)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_9:
			if (api_version != PGSS_V1_9)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_10:
			if (api_version != PGSS_V1_10)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case YB_PG_STAT_STATEMENTS_COLS_V1_4:
			if (api_version != YB_PGSS_V1_4)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case YB_PG_STAT_STATEMENTS_COLS_V1_10:
			if (api_version != YB_PGSS_V1_10)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case YB_V2_0_PG_STAT_STATEMENTS_COLS_V1_10:
			if (api_version != YB_V2_0_PGSS_V1_10)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case YB_V2_1_PG_STAT_STATEMENTS_COLS_V1_10:
			if (api_version != YB_V2_1_PGSS_V1_10)
				elog(ERROR, "incorrect number of output arguments");
			break;
		default:
			elog(ERROR, "incorrect number of output arguments");
	}

	/*
	 * We'd like to load the query text file (if needed) while not holding any
	 * lock on pgss->lock.  In the worst case we'll have to do this again
	 * after we have the lock, but it's unlikely enough to make this a win
	 * despite occasional duplicated work.  We need to reload if anybody
	 * writes to the file (either a retail qtext_store(), or a garbage
	 * collection) between this point and where we've gotten shared lock.  If
	 * a qtext_store is actually in progress when we look, we might as well
	 * skip the speculative load entirely.
	 */
	if (showtext)
	{
		int			n_writers;

		/* Take the mutex so we can examine variables */
		{
			volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

			SpinLockAcquire(&s->mutex);
			extent = s->extent;
			n_writers = s->n_writers;
			gc_count = s->gc_count;
			SpinLockRelease(&s->mutex);
		}

		/* No point in loading file now if there are active writers */
		if (n_writers == 0)
			qbuffer = qtext_load_file(&qbuffer_size);
	}

	/*
	 * Get shared lock, load or reload the query text file if we must, and
	 * iterate over the hashtable entries.
	 *
	 * With a large hash table, we might be holding the lock rather longer
	 * than one could wish.  However, this only blocks creation of new hash
	 * table entries, and the larger the hash table the less likely that is to
	 * be needed.  So we can hope this is okay.  Perhaps someday we'll decide
	 * we need to partition the hash table to limit the time spent holding any
	 * one lock.
	 */
	LWLockAcquire(pgss->lock, LW_SHARED);

	if (showtext)
	{
		/*
		 * Here it is safe to examine extent and gc_count without taking the
		 * mutex.  Note that although other processes might change
		 * pgss->extent just after we look at it, the strings they then write
		 * into the file cannot yet be referenced in the hashtable, so we
		 * don't care whether we see them or not.
		 *
		 * If qtext_load_file fails, we just press on; we'll return NULL for
		 * every query text.
		 */
		if (qbuffer == NULL ||
			pgss->extent != extent ||
			pgss->gc_count != gc_count)
		{
			if (qbuffer)
				pfree(qbuffer);
			qbuffer = qtext_load_file(&qbuffer_size);
		}
	}

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PG_STAT_STATEMENTS_COLS];
		bool		nulls[PG_STAT_STATEMENTS_COLS];
		int			i = 0;
		Counters	tmp;
		double		stddev;
		int64		queryid = entry->key.queryid;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		if (api_version >= PGSS_V1_9)
			values[i++] = BoolGetDatum(entry->key.toplevel);

		if (is_allowed_role || entry->key.userid == userid)
		{
			if (api_version >= PGSS_V1_2)
				values[i++] = Int64GetDatumFast(queryid);

			if (showtext)
			{
				char	   *qstr = qtext_fetch(entry->query_offset,
											   entry->query_len,
											   qbuffer,
											   qbuffer_size);

				if (qstr)
				{
					char	   *enc;

					enc = pg_any_to_server(qstr,
										   entry->query_len,
										   entry->encoding);

					values[i++] = CStringGetTextDatum(enc);

					if (enc != qstr)
						pfree(enc);
				}
				else
				{
					/* Just return a null if we fail to find the text */
					nulls[i++] = true;
				}
			}
			else
			{
				/* Query text not requested */
				nulls[i++] = true;
			}
		}
		else
		{
			/* Don't show queryid */
			if (api_version >= PGSS_V1_2)
				nulls[i++] = true;

			/*
			 * Don't show query text, but hint as to the reason for not doing
			 * so if it was requested
			 */
			if (showtext)
				values[i++] = CStringGetTextDatum("<insufficient privilege>");
			else
				nulls[i++] = true;
		}

		/* copy counters to a local variable to keep locking time short */
		{
			volatile pgssEntry *e = (volatile pgssEntry *) entry;

			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}

		/* Skip entry if unexecuted (ie, it's a pending "sticky" entry) */
		if (IS_STICKY(tmp))
			continue;

		/* Note that we rely on PGSS_PLAN being 0 and PGSS_EXEC being 1. */
		for (int kind = 0; kind < PGSS_NUMKIND; kind++)
		{
			if (kind == PGSS_EXEC || api_version >= PGSS_V1_8)
			{
				values[i++] = Int64GetDatumFast(tmp.calls[kind]);
				values[i++] = Float8GetDatumFast(tmp.total_time[kind]);
			}

			if ((kind == PGSS_EXEC && api_version >= PGSS_V1_3) ||
				api_version >= PGSS_V1_8)
			{
				values[i++] = Float8GetDatumFast(tmp.min_time[kind]);
				values[i++] = Float8GetDatumFast(tmp.max_time[kind]);
				values[i++] = Float8GetDatumFast(tmp.mean_time[kind]);

				/*
				 * Note we are calculating the population variance here, not
				 * the sample variance, as we have data for the whole
				 * population, so Bessel's correction is not used, and we
				 * don't divide by tmp.calls - 1.
				 */
				if (tmp.calls[kind] > 1)
					stddev = sqrt(tmp.sum_var_time[kind] / tmp.calls[kind]);
				else
					stddev = 0.0;
				values[i++] = Float8GetDatumFast(stddev);
			}
		}
		values[i++] = Int64GetDatumFast(tmp.rows);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_read);
		if (api_version >= PGSS_V1_1)
			values[i++] = Int64GetDatumFast(tmp.shared_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_written);
		values[i++] = Int64GetDatumFast(tmp.local_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.local_blks_read);
		if (api_version >= PGSS_V1_1)
			values[i++] = Int64GetDatumFast(tmp.local_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.local_blks_written);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_read);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_written);
		if (api_version >= PGSS_V1_1)
		{
			values[i++] = Float8GetDatumFast(tmp.blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.blk_write_time);
		}
		if (api_version >= PGSS_V1_10)
		{
			values[i++] = Float8GetDatumFast(tmp.temp_blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.temp_blk_write_time);
		}
		if (api_version >= PGSS_V1_8)
		{
			char		buf[256];
			Datum		wal_bytes;

			values[i++] = Int64GetDatumFast(tmp.wal_records);
			values[i++] = Int64GetDatumFast(tmp.wal_fpi);

			snprintf(buf, sizeof buf, UINT64_FORMAT, tmp.wal_bytes);

			/* Convert to numeric. */
			wal_bytes = DirectFunctionCall3(numeric_in,
											CStringGetDatum(buf),
											ObjectIdGetDatum(0),
											Int32GetDatum(-1));
			values[i++] = wal_bytes;
		}
		if (api_version >= PGSS_V1_10)
		{
			values[i++] = Int64GetDatumFast(tmp.jit_functions);
			values[i++] = Float8GetDatumFast(tmp.jit_generation_time);
			values[i++] = Int64GetDatumFast(tmp.jit_inlining_count);
			values[i++] = Float8GetDatumFast(tmp.jit_inlining_time);
			values[i++] = Int64GetDatumFast(tmp.jit_optimization_count);
			values[i++] = Float8GetDatumFast(tmp.jit_optimization_time);
			values[i++] = Int64GetDatumFast(tmp.jit_emission_count);
			values[i++] = Float8GetDatumFast(tmp.jit_emission_time);
		}

		if (api_version == YB_PGSS_V1_4 || api_version == YB_PGSS_V1_10 ||
			api_version == YB_V2_0_PGSS_V1_10 || api_version == YB_V2_1_PGSS_V1_10)
		{
			values[i++] = yb_get_histogram_jsonb_args(queryid,
													  entry->key.userid,
													  entry->key.dbid,
													  entry->key.toplevel);
		}

		if (api_version >= YB_V2_0_PGSS_V1_10)
		{
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_READ_RPCS]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_WRITE_RPCS]);
			values[i++] = Float8GetDatumFast(tmp.yb_counters.counters_dbl[YB_DBL_CATALOG_WAIT_TIME_MS]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_READ_OPS]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_WRITE_OPS]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_ROWS_SCANNED]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_ROWS_RETURNED]);
			values[i++] = Float8GetDatumFast(tmp.yb_counters.counters_dbl[YB_DBL_DOCDB_WAIT_TIME_MS]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_CONFLICT_RETRIES]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_READ_RESTART_RETRIES]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_TOTAL_RETRIES]);
		}

		if (api_version >= YB_V2_1_PGSS_V1_10)
		{
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_OBSOLETE_ROWS_SCANNED]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_SEEKS]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_NEXTS]);
			values[i++] = Int64GetDatumFast(tmp.yb_counters.counters[YB_INT_DOCDB_PREVS]);
			values[i++] = Float8GetDatumFast(tmp.yb_counters.counters_dbl[YB_DBL_DOCDB_READ_TIME_MS]);
			values[i++] = Float8GetDatumFast(tmp.yb_counters.counters_dbl[YB_DBL_DOCDB_WRITE_TIME_MS]);
		}

		Assert(i == (api_version == PGSS_V1_0 ? PG_STAT_STATEMENTS_COLS_V1_0 :
					 api_version == PGSS_V1_1 ? PG_STAT_STATEMENTS_COLS_V1_1 :
					 api_version == PGSS_V1_2 ? PG_STAT_STATEMENTS_COLS_V1_2 :
					 api_version == PGSS_V1_3 ? PG_STAT_STATEMENTS_COLS_V1_3 :
					 api_version == PGSS_V1_8 ? PG_STAT_STATEMENTS_COLS_V1_8 :
					 api_version == PGSS_V1_9 ? PG_STAT_STATEMENTS_COLS_V1_9 :
					 api_version == PGSS_V1_10 ? PG_STAT_STATEMENTS_COLS_V1_10 :
					 api_version == YB_PGSS_V1_4 ? YB_PG_STAT_STATEMENTS_COLS_V1_4 :
					 api_version == YB_PGSS_V1_10 ? YB_PG_STAT_STATEMENTS_COLS_V1_10 :
					 api_version == YB_V2_0_PGSS_V1_10 ? YB_V2_0_PG_STAT_STATEMENTS_COLS_V1_10 :
					 api_version == YB_V2_1_PGSS_V1_10 ? YB_V2_1_PG_STAT_STATEMENTS_COLS_V1_10 :
					 -1 /* fail if you forget to update this assert */ ));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	LWLockRelease(pgss->lock);

	if (qbuffer)
		pfree(qbuffer);
}

/* Number of output arguments (columns) for pg_stat_statements_info */
#define PG_STAT_STATEMENTS_INFO_COLS	2

/*
 * Return statistics of pg_stat_statements.
 */
Datum
pg_stat_statements_info(PG_FUNCTION_ARGS)
{
	pgssGlobalStats stats;
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_STATEMENTS_INFO_COLS];
	bool		nulls[PG_STAT_STATEMENTS_INFO_COLS];

	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via shared_preload_libraries")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Read global statistics for pg_stat_statements */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		stats = s->stats;
		SpinLockRelease(&s->mutex);
	}

	values[0] = Int64GetDatum(stats.dealloc);
	values[1] = TimestampTzGetDatum(stats.stats_reset);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Estimate shared memory space needed.
 */
static Size
pgss_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(pgssSharedState));
	size = add_size(size, hash_estimate_size(pgss_max,
											 sizeof(pgssEntry) + cfg.counts_len * sizeof(count_t)));

	return size;
}

/*
 * Allocate a new hashtable entry.
 * caller must hold an exclusive lock on pgss->lock
 *
 * "query" need not be null-terminated; we rely on query_len instead
 *
 * If "sticky" is true, make the new entry artificially sticky so that it will
 * probably still be there when the query finishes execution.  We do this by
 * giving it a median usage value rather than the normal value.  (Strictly
 * speaking, query strings are normalized on a best effort basis, though it
 * would be difficult to demonstrate this even under artificial conditions.)
 *
 * Note: despite needing exclusive lock, it's not an error for the target
 * entry to already exist.  This is because pgss_store releases and
 * reacquires lock after failing to find a match; so someone else could
 * have made the entry while we waited to get exclusive lock.
 */
static pgssEntry *
entry_alloc(pgssHashKey *key, Size query_offset, int query_len, int encoding,
			bool sticky)
{
	pgssEntry  *entry;
	bool		found;

	/* Make space if needed */
	while (hash_get_num_entries(pgss_hash) >= pgss_max)
		entry_dealloc();

	/* Find or create an entry with desired hash code */
	entry = (pgssEntry *) hash_search(pgss_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it */

		/* reset the statistics */
		memset(&entry->counters, 0, sizeof(Counters));
		/* set the appropriate initial usage count */
		entry->counters.usage = sticky ? pgss->cur_median_usage : USAGE_INIT;
		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
		/* ... and don't forget the query text metadata */
		Assert(query_len >= 0);
		entry->query_offset = query_offset;
		entry->query_len = query_len;
		entry->encoding = encoding;
		entry->yb_slow_executions = 0;
		/* zero out histogram space when new entry is created */
		yb_hdr_reset(&entry->yb_hdr_histogram);
		hdr_init_preallocated(&entry->yb_hdr_histogram, &cfg);
	}

	return entry;
}

/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = (*(pgssEntry *const *) lhs)->counters.usage;
	double		r_usage = (*(pgssEntry *const *) rhs)->counters.usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}

/*
 * Deallocate least-used entries.
 *
 * Caller must hold an exclusive lock on pgss->lock.
 */
static void
entry_dealloc(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry **entries;
	pgssEntry  *entry;
	int			nvictims;
	int			i;
	Size		tottextlen;
	int			nvalidtexts;

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values, and update the mean query length.
	 *
	 * Note that the mean query length is almost immediately obsolete, since
	 * we compute it before not after discarding the least-used entries.
	 * Hopefully, that doesn't affect the mean too much; it doesn't seem worth
	 * making two passes to get a more current result.  Likewise, the new
	 * cur_median_usage includes the entries we're about to zap.
	 */

	entries = palloc(hash_get_num_entries(pgss_hash) * sizeof(pgssEntry *));

	i = 0;
	tottextlen = 0;
	nvalidtexts = 0;

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
		/* "Sticky" entries get a different usage decay rate. */
		if (IS_STICKY(entry->counters))
			entry->counters.usage *= STICKY_DECREASE_FACTOR;
		else
			entry->counters.usage *= USAGE_DECREASE_FACTOR;
		/* In the mean length computation, ignore dropped texts. */
		if (entry->query_len >= 0)
		{
			tottextlen += entry->query_len + 1;
			nvalidtexts++;
		}
	}

	/* Sort into increasing order by usage */
	qsort(entries, i, sizeof(pgssEntry *), entry_cmp);

	/* Record the (approximate) median usage */
	if (i > 0)
		pgss->cur_median_usage = entries[i / 2]->counters.usage;
	/* Record the mean query length */
	if (nvalidtexts > 0)
		pgss->mean_query_len = tottextlen / nvalidtexts;
	else
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	/* Now zap an appropriate fraction of lowest-usage entries */
	nvictims = Max(10, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(pgss_hash, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);

	/* Increment the number of times entries are deallocated */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		s->stats.dealloc += 1;
		SpinLockRelease(&s->mutex);
	}
}

/*
 * Given a query string (not necessarily null-terminated), allocate a new
 * entry in the external query text file and store the string there.
 *
 * If successful, returns true, and stores the new entry's offset in the file
 * into *query_offset.  Also, if gc_count isn't NULL, *gc_count is set to the
 * number of garbage collections that have occurred so far.
 *
 * On failure, returns false.
 *
 * At least a shared lock on pgss->lock must be held by the caller, so as
 * to prevent a concurrent garbage collection.  Share-lock-holding callers
 * should pass a gc_count pointer to obtain the number of garbage collections,
 * so that they can recheck the count after obtaining exclusive lock to
 * detect whether a garbage collection occurred (and removed this entry).
 */
static bool
qtext_store(const char *query, int query_len,
			Size *query_offset, int *gc_count)
{
	Size		off;
	int			fd;

	/*
	 * We use a spinlock to protect extent/n_writers/gc_count, so that
	 * multiple processes may execute this function concurrently.
	 */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		off = s->extent;
		s->extent += query_len + 1;
		s->n_writers++;
		if (gc_count)
			*gc_count = s->gc_count;
		SpinLockRelease(&s->mutex);
	}

	*query_offset = off;

	/*
	 * Don't allow the file to grow larger than what qtext_load_file can
	 * (theoretically) handle.  This has been seen to be reachable on 32-bit
	 * platforms.
	 */
	if (unlikely(query_len >= MaxAllocHugeSize - off))
	{
		errno = EFBIG;			/* not quite right, but it'll do */
		fd = -1;
		goto error;
	}

	/* Now write the data into the successfully-reserved part of the file */
	fd = OpenTransientFile(PGSS_TEXT_FILE, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0)
		goto error;

	if (pg_pwrite(fd, query, query_len, off) != query_len)
		goto error;
	if (pg_pwrite(fd, "\0", 1, off + query_len) != 1)
		goto error;

	CloseTransientFile(fd);

	/* Mark our write complete */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		s->n_writers--;
		SpinLockRelease(&s->mutex);
	}

	return true;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_TEXT_FILE)));

	if (fd >= 0)
		CloseTransientFile(fd);

	/* Mark our write complete */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		s->n_writers--;
		SpinLockRelease(&s->mutex);
	}

	return false;
}

/*
 * Read the external query text file into a palloc'd buffer.
 *
 * Returns NULL (without throwing an error) if unable to read, eg
 * file not there or insufficient memory.
 *
 * On success, the buffer size is also returned into *buffer_size.
 *
 * This can be called without any lock on pgss->lock, but in that case
 * the caller is responsible for verifying that the result is sane.
 */
static char *
qtext_load_file(Size *buffer_size)
{
	char	   *buf;
	int			fd;
	struct stat stat;
	Size		nread;

	fd = OpenTransientFile(PGSS_TEXT_FILE, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							PGSS_TEXT_FILE)));
		return NULL;
	}

	/* Get file length */
	if (fstat(fd, &stat))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						PGSS_TEXT_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/* Allocate buffer; beware that off_t might be wider than size_t */
	if ((yb_qtext_size_limit >= 0 && stat.st_size > yb_qtext_size_limit * 1024) ||
		!AllocHugeSizeIsValid(stat.st_size))
		buf = NULL;
	else
		buf = (char *) palloc_extended(stat.st_size, MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);

	if (buf == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Could not allocate %lld bytes needed to read file \"%s\".",
						   (long long) stat.st_size, PGSS_TEXT_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/*
	 * OK, slurp in the file.  Windows fails if we try to read more than
	 * INT_MAX bytes at once, and other platforms might not like that either,
	 * so read a very large file in 1GB segments.
	 */
	nread = 0;
	while (nread < stat.st_size)
	{
		int			toread = Min(1024 * 1024 * 1024, stat.st_size - nread);

		/*
		 * If we get a short read and errno doesn't get set, the reason is
		 * probably that garbage collection truncated the file since we did
		 * the fstat(), so we don't log a complaint --- but we don't return
		 * the data, either, since it's most likely corrupt due to concurrent
		 * writes from garbage collection.
		 */
		errno = 0;
		if (read(fd, buf + nread, toread) != toread)
		{
			if (errno)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m",
								PGSS_TEXT_FILE)));
			pfree(buf);
			CloseTransientFile(fd);
			return NULL;
		}
		nread += toread;
	}

	if (CloseTransientFile(fd) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", PGSS_TEXT_FILE)));

	*buffer_size = nread;
	return buf;
}

/*
 * Locate a query text in the file image previously read by qtext_load_file().
 *
 * We validate the given offset/length, and return NULL if bogus.  Otherwise,
 * the result points to a null-terminated string within the buffer.
 */
static char *
qtext_fetch(Size query_offset, int query_len,
			char *buffer, Size buffer_size)
{
	/* File read failed? */
	if (buffer == NULL)
		return NULL;
	/* Bogus offset/length? */
	if (query_len < 0 ||
		query_offset + query_len >= buffer_size)
		return NULL;
	/* As a further sanity check, make sure there's a trailing null */
	if (buffer[query_offset + query_len] != '\0')
		return NULL;
	/* Looks OK */
	return buffer + query_offset;
}

/*
 * Do we need to garbage-collect the external query text file?
 *
 * Caller should hold at least a shared lock on pgss->lock.
 */
static bool
need_gc_qtexts(void)
{
	Size		extent;

	/* Read shared extent pointer */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		extent = s->extent;
		SpinLockRelease(&s->mutex);
	}

	/*
	 * Don't proceed if file does not exceed 512 bytes per possible entry.
	 *
	 * Here and in the next test, 32-bit machines have overflow hazards if
	 * pgss_max and/or mean_query_len are large.  Force the multiplications
	 * and comparisons to be done in uint64 arithmetic to forestall trouble.
	 */
	if ((uint64) extent < (uint64) 512 * pgss_max)
		return false;

	/*
	 * Don't proceed if file is less than about 50% bloat.  Nothing can or
	 * should be done in the event of unusually large query texts accounting
	 * for file's large size.  We go to the trouble of maintaining the mean
	 * query length in order to prevent garbage collection from thrashing
	 * uselessly.
	 */
	if ((uint64) extent < (uint64) pgss->mean_query_len * pgss_max * 2)
		return false;

	return true;
}

/*
 * Garbage-collect orphaned query texts in external file.
 *
 * This won't be called often in the typical case, since it's likely that
 * there won't be too much churn, and besides, a similar compaction process
 * occurs when serializing to disk at shutdown or as part of resetting.
 * Despite this, it seems prudent to plan for the edge case where the file
 * becomes unreasonably large, with no other method of compaction likely to
 * occur in the foreseeable future.
 *
 * The caller must hold an exclusive lock on pgss->lock.
 *
 * At the first sign of trouble we unlink the query text file to get a clean
 * slate (although existing statistics are retained), rather than risk
 * thrashing by allowing the same problem case to recur indefinitely.
 */
static void
gc_qtexts(void)
{
	char	   *qbuffer;
	Size		qbuffer_size;
	FILE	   *qfile = NULL;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	Size		extent;
	int			nentries;

	/*
	 * When called from pgss_store, some other session might have proceeded
	 * with garbage collection in the no-lock-held interim of lock strength
	 * escalation.  Check once more that this is actually necessary.
	 */
	if (!need_gc_qtexts())
		return;

	/*
	 * Load the old texts file.  If we fail (out of memory, for instance),
	 * invalidate query texts.  Hopefully this is rare.  It might seem better
	 * to leave things alone on an OOM failure, but the problem is that the
	 * file is only going to get bigger; hoping for a future non-OOM result is
	 * risky and can easily lead to complete denial of service.
	 */
	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		goto gc_fail;

	/*
	 * We overwrite the query texts file in place, so as to reduce the risk of
	 * an out-of-disk-space failure.  Since the file is guaranteed not to get
	 * larger, this should always work on traditional filesystems; though we
	 * could still lose on copy-on-write filesystems.
	 */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PGSS_TEXT_FILE)));
		goto gc_fail;
	}

	extent = 0;
	nentries = 0;

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			query_len = entry->query_len;
		char	   *qry = qtext_fetch(entry->query_offset,
									  query_len,
									  qbuffer,
									  qbuffer_size);

		if (qry == NULL)
		{
			/* Trouble ... drop the text */
			entry->query_offset = 0;
			entry->query_len = -1;
			/* entry will not be counted in mean query length computation */
			continue;
		}

		if (fwrite(qry, 1, query_len + 1, qfile) != query_len + 1)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							PGSS_TEXT_FILE)));
			hash_seq_term(&hash_seq);
			goto gc_fail;
		}

		entry->query_offset = extent;
		extent += query_len + 1;
		nentries++;
	}

	/*
	 * Truncate away any now-unused space.  If this fails for some odd reason,
	 * we log it, but there's no need to fail.
	 */
	if (ftruncate(fileno(qfile), extent) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m",
						PGSS_TEXT_FILE)));

	if (FreeFile(qfile))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PGSS_TEXT_FILE)));
		qfile = NULL;
		goto gc_fail;
	}

	elog(DEBUG1, "pgss gc of queries file shrunk size from %zu to %zu",
		 pgss->extent, extent);

	/* Reset the shared extent pointer */
	pgss->extent = extent;

	/*
	 * Also update the mean query length, to be sure that need_gc_qtexts()
	 * won't still think we have a problem.
	 */
	if (nentries > 0)
		pgss->mean_query_len = extent / nentries;
	else
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	pfree(qbuffer);

	/*
	 * OK, count a garbage collection cycle.  (Note: even though we have
	 * exclusive lock on pgss->lock, we must take pgss->mutex for this, since
	 * other processes may examine gc_count while holding only the mutex.
	 * Also, we have to advance the count *after* we've rewritten the file,
	 * else other processes might not realize they read a stale file.)
	 */
	record_gc_qtexts();

	return;

gc_fail:
	/* clean up resources */
	if (qfile)
		FreeFile(qfile);
	if (qbuffer)
		pfree(qbuffer);

	/*
	 * Since the contents of the external file are now uncertain, mark all
	 * hashtable entries as having invalid texts.
	 */
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entry->query_offset = 0;
		entry->query_len = -1;
	}

	/*
	 * Destroy the query text file and create a new, empty one
	 */
	(void) unlink(PGSS_TEXT_FILE);
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not recreate file \"%s\": %m",
						PGSS_TEXT_FILE)));
	else
		FreeFile(qfile);

	/* Reset the shared extent pointer */
	pgss->extent = 0;

	/* Reset mean_query_len to match the new state */
	pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	/*
	 * Bump the GC count even though we failed.
	 *
	 * This is needed to make concurrent readers of file without any lock on
	 * pgss->lock notice existence of new version of file.  Once readers
	 * subsequently observe a change in GC count with pgss->lock held, that
	 * forces a safe reopen of file.  Writers also require that we bump here,
	 * of course.  (As required by locking protocol, readers and writers don't
	 * trust earlier file contents until gc_count is found unchanged after
	 * pgss->lock acquired in shared or exclusive mode respectively.)
	 */
	record_gc_qtexts();
}

/*
 * Function that caches environmental variable
 * FLAGS_TEST_yb_lwlock_crash_after_acquire_pg_stat_statements_reset.
 *
 * This avoids the process of checking the value of the environmental variable
 * time and again.
 */
bool
yb_lwlock_crash_after_acquire_pg_stat_statements_reset()
{
	static int	cached_value = -1;

	if (cached_value == -1)
	{
		cached_value = YBCIsEnvVarTrue("FLAGS_TEST_yb_lwlock_crash_after_acquire_pg_stat_statements_reset");
	}
	return cached_value;

}

/*
 * Release entries corresponding to parameters passed.
 */
static void
entry_reset(Oid userid, Oid dbid, uint64 queryid)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	FILE	   *qfile;
	long		num_entries;
	long		num_remove = 0;
	pgssHashKey key;

	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via shared_preload_libraries")));

	LWLockAcquire(pgss->lock, LW_EXCLUSIVE);
	num_entries = hash_get_num_entries(pgss_hash);

	if (userid != 0 && dbid != 0 && queryid != UINT64CONST(0))
	{
		/* If all the parameters are available, use the fast path. */
		memset(&key, 0, sizeof(pgssHashKey));
		key.userid = userid;
		key.dbid = dbid;
		key.queryid = queryid;

		/*
		 * Remove the key if it exists, starting with the non-top-level entry.
		 */
		key.toplevel = false;
		entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_REMOVE, NULL);
		if (entry)				/* found */
			num_remove++;

		/* Also remove the top-level entry if it exists. */
		key.toplevel = true;
		entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_REMOVE, NULL);
		if (entry)				/* found */
			num_remove++;
	}
	else if (userid != 0 || dbid != 0 || queryid != UINT64CONST(0))
	{
		/* Remove entries corresponding to valid parameters. */
		hash_seq_init(&hash_seq, pgss_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			if ((!userid || entry->key.userid == userid) &&
				(!dbid || entry->key.dbid == dbid) &&
				(!queryid || entry->key.queryid == queryid))
			{
				hash_search(pgss_hash, &entry->key, HASH_REMOVE, NULL);
				num_remove++;
			}
		}
	}
	else
	{
		/* Remove all entries. */
		hash_seq_init(&hash_seq, pgss_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			hash_search(pgss_hash, &entry->key, HASH_REMOVE, NULL);
			num_remove++;
		}
	}

	/* All entries are removed? */
	if (num_entries != num_remove)
		goto release_lock;

	/*
	 * Reset global statistics for pg_stat_statements since all entries are
	 * removed.
	 */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;
		TimestampTz stats_reset = GetCurrentTimestamp();

		SpinLockAcquire(&s->mutex);
		s->stats.dealloc = 0;
		s->stats.stats_reset = stats_reset;
		SpinLockRelease(&s->mutex);
	}

	if (yb_lwlock_crash_after_acquire_pg_stat_statements_reset())
		kill(getpid(), 9);
	/*
	 * Write new empty query file, perhaps even creating a new one to recover
	 * if the file was missing.
	 */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						PGSS_TEXT_FILE)));
		goto done;
	}

	/* If ftruncate fails, log it, but it's not a fatal problem */
	if (ftruncate(fileno(qfile), 0) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m",
						PGSS_TEXT_FILE)));

	FreeFile(qfile);

done:
	pgss->extent = 0;
	/* This counts as a query text garbage collection for our purposes */
	record_gc_qtexts();

release_lock:
	LWLockRelease(pgss->lock);
}

/*
 * Generate a normalized version of the query string that will be used to
 * represent all similar queries.
 *
 * Note that the normalized representation may well vary depending on
 * just which "equivalent" query is used to create the hashtable entry.
 * We assume this is OK.
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * *query_len_p contains the input string length, and is updated with
 * the result string length on exit.  The resulting string might be longer
 * or shorter depending on what happens with replacement of constants.
 *
 * Returns a palloc'd string.
 */
static char *
generate_normalized_query(JumbleState *jstate, const char *query,
						  int query_loc, int *query_len_p)
{
	char	   *norm_query;
	int			query_len = *query_len_p;
	int			i,
				norm_query_buflen,	/* Space allowed for norm_query */
				len_to_wrt,		/* Length (in bytes) to write */
				quer_loc = 0,	/* Source query byte location */
				n_quer_loc = 0, /* Normalized query byte location */
				last_off = 0,	/* Offset from start for previous tok */
				last_tok_len = 0;	/* Length (in bytes) of that tok */

	/*
	 * Get constants' lengths (core system only gives us locations).  Note
	 * this also ensures the items are sorted by location.
	 */
	fill_in_constant_lengths(jstate, query, query_loc);

	/*
	 * Allow for $n symbols to be longer than the constants they replace.
	 * Constants must take at least one byte in text form, while a $n symbol
	 * certainly isn't more than 11 bytes, even if n reaches INT_MAX.  We
	 * could refine that limit based on the max value of n for the current
	 * query, but it hardly seems worth any extra effort to do so.
	 */
	norm_query_buflen = query_len + jstate->clocations_count * 10;

	/* Allocate result buffer */
	norm_query = palloc(norm_query_buflen + 1);

	for (i = 0; i < jstate->clocations_count; i++)
	{
		int			off,		/* Offset from start for cur tok */
					tok_len;	/* Length (in bytes) of that tok */

		off = jstate->clocations[i].location;
		/* Adjust recorded location if we're dealing with partial string */
		off -= query_loc;

		tok_len = jstate->clocations[i].length;

		if (tok_len < 0)
			continue;			/* ignore any duplicates */

		/* Copy next chunk (what precedes the next constant) */
		len_to_wrt = off - last_off;
		len_to_wrt -= last_tok_len;

		Assert(len_to_wrt >= 0);
		memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
		n_quer_loc += len_to_wrt;

		/* And insert a param symbol in place of the constant token */
		n_quer_loc += sprintf(norm_query + n_quer_loc, "$%d",
							  i + 1 + jstate->highest_extern_param_id);

		quer_loc = off + tok_len;
		last_off = off;
		last_tok_len = tok_len;
	}

	/*
	 * We've copied up until the last ignorable constant.  Copy over the
	 * remaining bytes of the original query string.
	 */
	len_to_wrt = query_len - quer_loc;

	Assert(len_to_wrt >= 0);
	memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
	n_quer_loc += len_to_wrt;

	Assert(n_quer_loc <= norm_query_buflen);
	norm_query[n_quer_loc] = '\0';

	*query_len_p = n_quer_loc;
	return norm_query;
}

/*
 * Given a valid SQL string and an array of constant-location records,
 * fill in the textual lengths of those constants.
 *
 * The constants may use any allowed constant syntax, such as float literals,
 * bit-strings, single-quoted strings and dollar-quoted strings.  This is
 * accomplished by using the public API for the core scanner.
 *
 * It is the caller's job to ensure that the string is a valid SQL statement
 * with constants at the indicated locations.  Since in practice the string
 * has already been parsed, and the locations that the caller provides will
 * have originated from within the authoritative parser, this should not be
 * a problem.
 *
 * Duplicate constant pointers are possible, and will have their lengths
 * marked as '-1', so that they are later ignored.  (Actually, we assume the
 * lengths were initialized as -1 to start with, and don't change them here.)
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * N.B. There is an assumption that a '-' character at a Const location begins
 * a negative numeric constant.  This precludes there ever being another
 * reason for a constant to start with a '-'.
 */
static void
fill_in_constant_lengths(JumbleState *jstate, const char *query,
						 int query_loc)
{
	LocationLen *locs;
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	int			last_loc = -1;
	int			i;

	/*
	 * Sort the records by location so that we can process them in order while
	 * scanning the query text.
	 */
	if (jstate->clocations_count > 1)
		qsort(jstate->clocations, jstate->clocations_count,
			  sizeof(LocationLen), comp_location);
	locs = jstate->clocations;

	/* initialize the flex scanner --- should match raw_parser() */
	yyscanner = scanner_init(query,
							 &yyextra,
							 &ScanKeywords,
							 ScanKeywordTokens);

	/* we don't want to re-emit any escape string warnings */
	yyextra.escape_string_warning = false;

	/* Search for each constant, in sequence */
	for (i = 0; i < jstate->clocations_count; i++)
	{
		int			loc = locs[i].location;
		int			tok;

		/* Adjust recorded location if we're dealing with partial string */
		loc -= query_loc;

		Assert(loc >= 0);

		if (loc <= last_loc)
			continue;			/* Duplicate constant, ignore */

		/* Lex tokens until we find the desired constant */
		for (;;)
		{
			tok = core_yylex(&yylval, &yylloc, yyscanner);

			/* We should not hit end-of-string, but if we do, behave sanely */
			if (tok == 0)
				break;			/* out of inner for-loop */

			/*
			 * We should find the token position exactly, but if we somehow
			 * run past it, work with that.
			 */
			if (yylloc >= loc)
			{
				if (query[loc] == '-')
				{
					/*
					 * It's a negative value - this is the one and only case
					 * where we replace more than a single token.
					 *
					 * Do not compensate for the core system's special-case
					 * adjustment of location to that of the leading '-'
					 * operator in the event of a negative constant.  It is
					 * also useful for our purposes to start from the minus
					 * symbol.  In this way, queries like "select * from foo
					 * where bar = 1" and "select * from foo where bar = -2"
					 * will have identical normalized query strings.
					 */
					tok = core_yylex(&yylval, &yylloc, yyscanner);
					if (tok == 0)
						break;	/* out of inner for-loop */
				}

				/*
				 * We now rely on the assumption that flex has placed a zero
				 * byte after the text of the current token in scanbuf.
				 */
				locs[i].length = strlen(yyextra.scanbuf + loc);
				break;			/* out of inner for-loop */
			}
		}

		/* If we hit end-of-string, give up, leaving remaining lengths -1 */
		if (tok == 0)
			break;

		last_loc = loc;
	}

	scanner_finish(yyscanner);
}

/*
 * comp_location: comparator for qsorting LocationLen structs by location
 */
static int
comp_location(const void *a, const void *b)
{
	int			l = ((const LocationLen *) a)->location;
	int			r = ((const LocationLen *) b)->location;

	if (l < r)
		return -1;
	else if (l > r)
		return +1;
	else
		return 0;
}

/*
 * DIRECT CALL IN PG_STAT_STATEMENTS.C
 * Given queryid, userid, dbid and top_level, returns jsonb histogram of execution time
 * of that query. Userid and dbid can be passed as NULL or 0 to use the
 * currently connected userid and dbid.
 */
static Datum
yb_get_histogram_jsonb_args(uint64 queryid, Oid userid, Oid dbid, bool top_level)
{
	userid = userid != InvalidOid ? userid : GetUserId();
	dbid = dbid != InvalidOid ? dbid : MyDatabaseId;
	pgssHashKey key;
	pgssEntry  *entry;

	memset(&key, 0, sizeof(pgssHashKey));
	key.queryid = queryid;
	key.userid = userid;
	key.dbid = dbid;
	key.toplevel = top_level;

	entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_FIND, NULL);

	if (!entry)
	{
		ereport(ERROR,
				(errmsg("invalid combination of queryid, userid, dbid, and top_level"),
				 errhint("Please refer to pg_stat_statements for the correct details.")));
		PG_RETURN_DATUM(0);
	}

	JsonbParseState *state = NULL;

	return yb_add_histogram_jsonb(state, &entry->yb_hdr_histogram,
								  entry->yb_slow_executions);
}

/*
 * Add histogram entries to the JsonbParseState.
 */
static Datum
yb_add_histogram_jsonb(JsonbParseState *state, hdr_histogram *h,
					   size_t yb_slow_executions)
{
	yb_hdr_iter iter;

	StringInfoData buf;

	initStringInfo(&buf);

	JsonbValue *res;
	JsonbPair	pair;

	pair.key.type = jbvString;
	pair.value.type = jbvNumeric;

	MemoryContext tempContext = AllocSetContextCreate(CurrentMemoryContext,
													  "JSONB processing temporary context",
													  ALLOCSET_DEFAULT_SIZES);
	MemoryContext oldContext = MemoryContextSwitchTo(tempContext);

	pushJsonbValue(&state, WJB_BEGIN_ARRAY, NULL);
	hdr_iter_init(&iter, h);
	while (hdr_iter_next(&iter))
	{
		if (iter.count > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "[%.1f,%.1f)",
							 (iter.value_iterated_to) * yb_hdr_latency_res_ms,
							 (iter.highest_equivalent_value + 1) * yb_hdr_latency_res_ms);
			yb_add_hdr_jsonb_object(state, buf.data, iter.count, &pair);
		}
	}

	if (yb_slow_executions > 0)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "[%.1f,)", yb_hdr_max_value * yb_hdr_latency_res_ms);
		yb_add_hdr_jsonb_object(state, buf.data, yb_slow_executions, &pair);
	}

	res = pushJsonbValue(&state, WJB_END_ARRAY, NULL);
	MemoryContextSwitchTo(oldContext);
	Jsonb	   *ret = JsonbValueToJsonb(res);

	MemoryContextDelete(tempContext);
	pfree(buf.data);

	PG_RETURN_POINTER(ret);
}

static void
yb_add_hdr_jsonb_object(JsonbParseState *state, char *buf,
						count_t count, JsonbPair *pair)
{
	pair->key.val.string.len = strlen(buf);
	pair->key.val.string.val = pstrdup(buf);
	pair->value.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric,
																  (int64_t) count));

	(void) pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
	(void) pushJsonbValue(&state, WJB_KEY, &pair->key);
	(void) pushJsonbValue(&state, WJB_VALUE, &pair->value);
	(void) pushJsonbValue(&state, WJB_END_OBJECT, NULL);
}

/*
 * Initialized as callable function in Postgres (kept for debugging)
 * Given queryid, userid, and dbid, returns jsonb histogram of execution time of
 * that query. Userid and dbid can be passed as NULL, 0, or empty parameters to
 * use the currently connected userid and dbid.
 */
Datum
yb_get_histogram_jsonb(PG_FUNCTION_ARGS)
{
	uint64		queryid = PG_GETARG_INT64(0);
	Oid			userid = (PG_NARGS() >= 2 && PG_GETARG_OID(1) != InvalidOid ?
						  PG_GETARG_OID(1) :
						  InvalidOid);
	Oid			dbid = (PG_NARGS() >= 3 && PG_GETARG_OID(2) != InvalidOid ?
						PG_GETARG_OID(2) :
						InvalidOid);
	bool		top_level = PG_NARGS() == 4 ? PG_GETARG_BOOL(3) : false;

	/*
	 * Check for null queryid
	 */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	return yb_get_histogram_jsonb_args(queryid, userid, dbid, top_level);
}

/*
* Wipes histogram counts array only, expects pgssEntry memory cleanup to
* handle deallocating the actual histogram struct
*/
static void
yb_hdr_reset(hdr_histogram *h)
{
	memset(h, 0, sizeof(hdr_histogram) + (sizeof(count_t) * h->counts_len));
}

static bool
yb_track_nested_queries(void)
{
	return pgss_track == PGSS_TRACK_ALL;
}

/*
 * Get the normalized query text from the pgss_query_texts.stat file
 * and copy it to the normalized_query buffer.
 * Note that normalized_query is expected to be a buffer of size at max
 * 1024 bytes.
 *
 * Returns:
 *  YB_DIAGNOSTICS_SUCCESS if normalized query fetched successfully,
 *	YB_DIAGNOSTICS_ERROR otherwise.
 */
static int
YbGetPgssNormalizedQueryText(Size query_offset, int actual_query_len, char *normalized_query)
{
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	int			query_len = Min(actual_query_len, YB_QD_MAX_PGSS_QUERY_LEN);

	qbuffer = qtext_load_file(&qbuffer_size);

	if (qbuffer == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not load pgss query text file")));
		return YB_DIAGNOSTICS_ERROR;
	}

	/*
	* Its important to fetch the query and then memcpy to normalized_query,
	* as qtext_fetch requires query length to be exact length of the query,
	* whereas query_len can be 1024 bytes in case pgss_query_len >= 1024.
	*/
	char	   *fetched_query = qtext_fetch(query_offset, actual_query_len,
											qbuffer, qbuffer_size);

	if (fetched_query == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not load pgss query text file")));
		pfree(qbuffer);
		return YB_DIAGNOSTICS_ERROR;
	}

	memcpy(normalized_query, fetched_query, query_len);
	normalized_query[query_len] = '\0'; /* Ensure null-termination */

	pfree(qbuffer);
	return YB_DIAGNOSTICS_SUCCESS;
}

/*
 * YB: Generate a normalized query string for a BACKFILL command.
 *
 * Replaces varying parameters (bfinstr, partition key, row keys) with $N
 * parameter placeholders so that all BACKFILL calls for the same index are
 * represented by a single normalized query in pg_stat_statements.
 *
 * Returns a palloc'd string.
 */
static char *
yb_generate_normalized_backfill_query(YbBackfillIndexStmt *stmt,
									  int *query_len_p)
{
	StringInfoData buf;
	ListCell   *lc;
	bool		first = true;
	int			param_num = 1;

	initStringInfo(&buf);

	appendStringInfoString(&buf, "BACKFILL INDEX ");

	/* Append OID list (these are part of the query identity, not abstracted) */
	foreach(lc, stmt->oid_list)
	{
		if (!first)
			appendStringInfoString(&buf, ", ");
		appendStringInfo(&buf, "%u", lfirst_oid(lc));
		first = false;
	}

	/* Append normalized parameters */
	if (stmt->bfinfo->bfinstr)
		appendStringInfo(&buf, " WITH $%d", param_num++);

	/* Read time is consistent for backfills of a given CREATE INDEX */
	appendStringInfo(&buf, " READ TIME " UINT64_FORMAT,
					(uint64) stmt->bfinfo->read_time);

	if (stmt->bfinfo->row_bounds)
	{
		if (stmt->bfinfo->row_bounds->partition_key)
			appendStringInfo(&buf, " PARTITION $%d", param_num++);

		if (stmt->bfinfo->row_bounds->row_key_start)
			appendStringInfo(&buf, " FROM $%d", param_num++);

		if (stmt->bfinfo->row_bounds->row_key_end)
			appendStringInfo(&buf, " TO $%d", param_num++);
	}

	*query_len_p = buf.len;
	return buf.data;
}

/*
 * YB: Truncate a query text to yb_max_query_text_length bytes without
 * splitting a multibyte character, and append "..." to show the truncation,
 * as upstream does for log_parameter_max_length and COPY error context.
 * "query" need not be null-terminated.  Returns a palloc'd string and sets
 * *query_len to its length.
 */
static char *
yb_truncate_query_text(const char *query, int *query_len, int encoding)
{
	int			cliplen;
	char	   *result;

	/*
	 * Length of the longest prefix of at most yb_max_query_text_length bytes
	 * that ends on a character boundary. Upstream clips texts the same way,
	 * via pg_mbcliplen, for track_activity_query_size and
	 * log_parameter_max_length.
	 */
	cliplen = pg_encoding_mbcliplen(encoding, query, *query_len,
									yb_max_query_text_length);

	result = palloc(cliplen + 4);
	memcpy(result, query, cliplen);
	strcpy(result + cliplen, "...");

	*query_len = cliplen + 3;
	return result;
}
