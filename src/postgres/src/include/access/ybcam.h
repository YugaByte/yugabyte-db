/*--------------------------------------------------------------------------------------------------
 *
 * ybcam.h
 *	  prototypes for ybc/ybcam.c
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * src/include/executor/ybcam.h
 *
 *--------------------------------------------------------------------------------------------------
 */

#ifndef YBCAM_H
#define YBCAM_H

#include "postgres.h"

#include "skey.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "nodes/relation.h"
#include "utils/catcache.h"
#include "utils/resowner.h"
#include "utils/snapshot.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"
#include "executor/ybcExpr.h"

/*
 * SCAN PLAN - Two structures.
 * - "struct YbScanPlanData" contains variables that are used during preparing statement.
 * - "struct YbScanDescData" contains variables that are used thru out the life of the statement.
 *
 * YugaByte ScanPlan has two different lists.
 *   Binding list:
 *   - This list is used to receive the user-given data for key columns (WHERE clause).
 *   - "sk_attno" is the INDEX's attnum for the key columns that are used to query data.
 *     In YugaByte, primary index attnum is the same as column attnum in the UserTable.
 *   - "bind_desc" is the description of the index columns (count, types, ...).
 *   - The bind lists don't need to be included in the description as they are only needed
 *     during setup.
 *   Target list:
 *   - This list identifies the user-wanted data to be fetched from the database.
 *   - "target_desc" contains the description of the target columns (count, types, ...).
 *   - The target fields are used for double-checking key values after selecting data
 *     from database. It should be removed once we no longer need to double-check the key values.
 */
typedef struct YbScanDescData
{
#define YB_MAX_SCAN_KEYS (INDEX_MAX_KEYS * 2) /* A pair of lower/upper bounds per column max */

	/* The handle for the internal YB Select statement. */
	YBCPgStatement handle;
	ResourceOwner stmt_owner;
	bool is_exec_done;

	Relation index;

	int nkeys;
	ScanKey key;

	TupleDesc target_desc;
	AttrNumber target_key_attnums[YB_MAX_SCAN_KEYS];

	/* Oid of the table being scanned */
	Oid tableOid;

	/* Kept query-plan control to pass it to PgGate during preparation */
	YBCPgPrepareParameters prepare_params;

	/*
	 * Kept execution control to pass it to PgGate.
	 * - When YBC-index-scan layer is called by Postgres IndexScan functions, it will read the
	 *   "yb_exec_params" from Postgres IndexScan and kept the info in this attribute.
	 *
	 * - YBC-index-scan in-turn will passes this attribute to PgGate to control the index-scan
	 *   execution in YB tablet server.
	 */
	YBCPgExecParameters *exec_params;
} YbScanDescData;

typedef struct YbScanDescData *YbScanDesc;

/*
 * Access to YB-stored system catalogs (mirroring API from genam.c)
 * We ignore the index id and always do a regular YugaByte scan (Postgres
 * would do either heap scan or index scan depending on the params).
 */
extern SysScanDesc ybc_systable_beginscan(Relation relation,
										  Oid indexId,
										  bool indexOK,
										  Snapshot snapshot,
										  int nkeys,
										  ScanKey key);
extern HeapTuple ybc_systable_getnext(SysScanDesc scanDesc);
extern void ybc_systable_endscan(SysScanDesc scan_desc);

/*
 * Access to YB-stored system catalogs (mirroring API from heapam.c)
 * We will do a YugaByte scan instead of a heap scan.
 */
extern HeapScanDesc ybc_heap_beginscan(Relation relation,
                                       Snapshot snapshot,
                                       int nkeys,
                                       ScanKey key,
									   bool temp_snap);
extern HeapTuple ybc_heap_getnext(HeapScanDesc scanDesc);
extern void ybc_heap_endscan(HeapScanDesc scanDesc);

/*
 * The ybc_idx API is used to process the following SELECT.
 *   SELECT data FROM heapRelation WHERE rowid IN
 *     ( SELECT rowid FROM indexRelation WHERE key = given_value )
 */
YbScanDesc ybcBeginScan(Relation relation,
                        Relation index,
                        bool xs_want_itup,
                        int nkeys,
                        ScanKey key);

HeapTuple ybc_getnext_heaptuple(YbScanDesc ybScan, bool is_forward_scan, bool *recheck);
IndexTuple ybc_getnext_indextuple(YbScanDesc ybScan, bool is_forward_scan, bool *recheck);

void ybcEndScan(YbScanDesc ybScan);

/* Number of rows assumed for a YB table if no size estimates exist */
#define YBC_DEFAULT_NUM_ROWS  1000

#define YBC_SINGLE_ROW_SELECTIVITY	(1.0 / YBC_DEFAULT_NUM_ROWS)
#define YBC_SINGLE_KEY_SELECTIVITY	(10.0 / YBC_DEFAULT_NUM_ROWS)
#define YBC_HASH_SCAN_SELECTIVITY	(100.0 / YBC_DEFAULT_NUM_ROWS)
#define YBC_FULL_SCAN_SELECTIVITY	1.0

/*
 * For a partial index the index predicate will filter away some rows.
 * TODO: Evaluate this based on the predicate itself and table stats.
 */
#define YBC_PARTIAL_IDX_PRED_SELECTIVITY 0.8

/*
 * Backwards scans are more expensive in DocDB.
 * TODO: the ysql_backward_prefetch_scale_factor gflag is correlated to this
 * but is too low (1/16 implying 16x slower) to be used here.
 */
#define YBC_BACKWARDS_SCAN_COST_FACTOR 1.1

/*
 * Uncovered indexes will require extra RPCs to the main table to retrieve the
 * values for all required columns. These requests are now batched in PgGate
 * so the extra cost should be relatively low in general.
 */
#define YBC_UNCOVERED_INDEX_COST_FACTOR 1.1

extern void ybcCostEstimate(RelOptInfo *baserel, Selectivity selectivity,
                            bool is_backwards_scan, bool is_uncovered_idx_scan,
							Cost *startup_cost, Cost *total_cost);
extern void ybcIndexCostEstimate(IndexPath *path, Selectivity *selectivity,
								 Cost *startup_cost, Cost *total_cost);

/*
 * Fetch a single tuple by the ybctid.
 */
extern HeapTuple YBCFetchTuple(Relation relation, Datum ybctid);

#endif							/* YBCAM_H */
