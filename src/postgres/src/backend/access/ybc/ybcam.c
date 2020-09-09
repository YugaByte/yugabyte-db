/*--------------------------------------------------------------------------------------------------
 *
 * ybcam.c
 *	  YugaByte catalog scan API.
 *	  This is used to access data from YugaByte's system catalog tables.
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
 * src/backend/executor/ybcam.c
 *
 *--------------------------------------------------------------------------------------------------
 */

#include <string.h>
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "commands/dbcommands.h"
#include "commands/tablegroup.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/catalog.h"
#include "catalog/pg_database.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/relation.h"
#include "optimizer/cost.h"
#include "utils/datum.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/resowner_private.h"
#include "utils/syscache.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"
#include "access/nbtree.h"

typedef struct YbScanPlanData
{
	/* The relation where to read data from */
	Relation target_relation;

	/* Primary and hash key columns of the referenced table/relation. */
	Bitmapset *primary_key;
	Bitmapset *hash_key;

	/* Set of key columns whose values will be used for scanning. */
	Bitmapset *sk_cols;

	/* Description and attnums of the columns to bind */
	TupleDesc bind_desc;
	AttrNumber bind_key_attnums[YB_MAX_SCAN_KEYS];
} YbScanPlanData;

typedef YbScanPlanData *YbScanPlan;

static void ybcAddAttributeColumn(YbScanPlan scan_plan, AttrNumber attnum)
{
  const int idx = YBAttnumToBmsIndex(scan_plan->target_relation, attnum);

  if (bms_is_member(idx, scan_plan->primary_key))
    scan_plan->sk_cols = bms_add_member(scan_plan->sk_cols, idx);
}

/*
 * Checks if an attribute is a hash or primary key column and note it in
 * the scan plan.
 */
static void ybcCheckPrimaryKeyAttribute(YbScanPlan      scan_plan,
										YBCPgTableDesc  ybc_table_desc,
										AttrNumber      attnum)
{
	bool is_primary = false;
	bool is_hash    = false;

	/*
	 * TODO(neil) We shouldn't need to upload YugaByte table descriptor here because the structure
	 * Postgres::Relation already has all information.
	 * - Primary key indicator: IndexRelation->rd_index->indisprimary
	 * - Number of key columns: IndexRelation->rd_index->indnkeyatts
	 * - Number of all columns: IndexRelation->rd_index->indnatts
	 * - Hash, range, etc: IndexRelation->rd_indoption (Bits INDOPTION_HASH, RANGE, etc)
	 */
	HandleYBTableDescStatus(YBCPgGetColumnInfo(ybc_table_desc,
											   attnum,
											   &is_primary,
											   &is_hash), ybc_table_desc);

	int idx = YBAttnumToBmsIndex(scan_plan->target_relation, attnum);

	if (is_hash)
	{
		scan_plan->hash_key = bms_add_member(scan_plan->hash_key, idx);
	}
	if (is_primary)
	{
		scan_plan->primary_key = bms_add_member(scan_plan->primary_key, idx);
	}
}

/*
 * Get YugaByte-specific table metadata and load it into the scan_plan.
 * Currently only the hash and primary key info.
 */
static void ybcLoadTableInfo(Relation relation, YbScanPlan scan_plan)
{
	Oid            dboid          = YBCGetDatabaseOid(relation);
	Oid            relid          = RelationGetRelid(relation);
	YBCPgTableDesc ybc_table_desc = NULL;

	HandleYBStatus(YBCPgGetTableDesc(dboid, relid, &ybc_table_desc));

	for (AttrNumber attnum = 1; attnum <= relation->rd_att->natts; attnum++)
	{
		ybcCheckPrimaryKeyAttribute(scan_plan, ybc_table_desc, attnum);
	}
	if (relation->rd_rel->relhasoids)
	{
		ybcCheckPrimaryKeyAttribute(scan_plan, ybc_table_desc, ObjectIdAttributeNumber);
	}
}

static Oid ybc_get_atttypid(TupleDesc bind_desc, AttrNumber attnum)
{
	Oid	atttypid;

	if (attnum > 0)
	{
		/* Get the type from the description */
		atttypid = TupleDescAttr(bind_desc, attnum - 1)->atttypid;
	}
	else
	{
		/* This must be an OID column. */
		atttypid = OIDOID;
	}

  return atttypid;
}

/*
 * Bind a scan key.
 */
static void ybcBindColumn(YbScanDesc ybScan, TupleDesc bind_desc, AttrNumber attnum, Datum value, bool is_null)
{
	Oid	atttypid = ybc_get_atttypid(bind_desc, attnum);

	YBCPgExpr ybc_expr = YBCNewConstant(ybScan->handle, atttypid, value, is_null);

	HandleYBStatusWithOwner(YBCPgDmlBindColumn(ybScan->handle, attnum, ybc_expr),
													ybScan->handle,
													ybScan->stmt_owner);
}

void ybcBindColumnCondEq(YbScanDesc ybScan, bool is_hash_key, TupleDesc bind_desc,
						 AttrNumber attnum, Datum value, bool is_null)
{
	Oid	atttypid = ybc_get_atttypid(bind_desc, attnum);

	YBCPgExpr ybc_expr = YBCNewConstant(ybScan->handle, atttypid, value, is_null);

	if (is_hash_key)
		HandleYBStatusWithOwner(YBCPgDmlBindColumn(ybScan->handle, attnum, ybc_expr),
														ybScan->handle,
														ybScan->stmt_owner);
	else
		HandleYBStatusWithOwner(YBCPgDmlBindColumnCondEq(ybScan->handle, attnum, ybc_expr),
														ybScan->handle,
														ybScan->stmt_owner);
}

static void ybcBindColumnCondBetween(YbScanDesc ybScan, TupleDesc bind_desc, AttrNumber attnum,
                                     bool start_valid, Datum value, bool end_valid, Datum value_end)
{
	Oid	atttypid = ybc_get_atttypid(bind_desc, attnum);

	YBCPgExpr ybc_expr = start_valid ? YBCNewConstant(ybScan->handle, atttypid, value,
      false /* isnull */) : NULL;
	YBCPgExpr ybc_expr_end = end_valid ? YBCNewConstant(ybScan->handle, atttypid, value_end,
      false /* isnull */) : NULL;

  HandleYBStatusWithOwner(YBCPgDmlBindColumnCondBetween(ybScan->handle, attnum, ybc_expr,
																												ybc_expr_end),
													ybScan->handle,
													ybScan->stmt_owner);
}

/*
 * Bind an array of scan keys for a column.
 */
static void ybcBindColumnCondIn(YbScanDesc ybScan, TupleDesc bind_desc, AttrNumber attnum,
                                int nvalues, Datum *values)
{
	Oid	atttypid = ybc_get_atttypid(bind_desc, attnum);

	YBCPgExpr ybc_exprs[nvalues]; /* VLA - scratch space */
	for (int i = 0; i < nvalues; i++) {
		/*
		 * For IN we are removing all null values in ybcBindScanKeys before
		 * getting here (relying on btree/lsm operators being strict).
		 * So we can safely set is_null to false for all options left here.
		 */
		ybc_exprs[i] = YBCNewConstant(ybScan->handle, atttypid, values[i], false /* is_null */);
	}

	HandleYBStatusWithOwner(YBCPgDmlBindColumnCondIn(ybScan->handle, attnum, nvalues, ybc_exprs),
	                                                 ybScan->handle,
	                                                 ybScan->stmt_owner);
}

/*
 * Add a target column.
 */
static void ybcAddTargetColumn(YbScanDesc ybScan, AttrNumber attnum)
{
	/* Regular (non-system) attribute. */
	Oid atttypid = InvalidOid;
	int32 atttypmod = 0;
	if (attnum > 0)
	{
		Form_pg_attribute attr = TupleDescAttr(ybScan->target_desc, attnum - 1);
		/* Ignore dropped attributes */
		if (attr->attisdropped)
			return;
		atttypid = attr->atttypid;
		atttypmod = attr->atttypmod;
	}

	YBCPgTypeAttrs type_attrs = { atttypmod };
	YBCPgExpr expr = YBCNewColumnRef(ybScan->handle, attnum, atttypid, &type_attrs);
	HandleYBStatusWithOwner(YBCPgDmlAppendTarget(ybScan->handle, expr),
													ybScan->handle,
													ybScan->stmt_owner);
}

static HeapTuple ybcFetchNextHeapTuple(YbScanDesc ybScan, bool is_forward_scan)
{
	HeapTuple tuple    = NULL;
	bool      has_data = false;
	TupleDesc tupdesc  = ybScan->target_desc;

	Datum           *values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	bool            *nulls  = (bool *) palloc(tupdesc->natts * sizeof(bool));
	YBCPgSysColumns syscols;

	/* Execute the select statement. */
	if (!ybScan->is_exec_done)
	{
		HandleYBStatusWithOwner(YBCPgSetForwardScan(ybScan->handle, is_forward_scan),
														ybScan->handle,
														ybScan->stmt_owner);
		HandleYBStatusWithOwner(YBCPgExecSelect(ybScan->handle, ybScan->exec_params),
														ybScan->handle,
														ybScan->stmt_owner);
		ybScan->is_exec_done = true;
	}

	/* Fetch one row. */
	HandleYBStatusWithOwner(YBCPgDmlFetch(ybScan->handle,
																				tupdesc->natts,
																				(uint64_t *) values,
																				nulls,
																				&syscols,
																				&has_data),
													ybScan->handle,
													ybScan->stmt_owner);

	if (has_data)
	{
		tuple = heap_form_tuple(tupdesc, values, nulls);

		if (syscols.oid != InvalidOid)
		{
			HeapTupleSetOid(tuple, syscols.oid);
		}
		if (syscols.ybctid != NULL)
		{
			tuple->t_ybctid = PointerGetDatum(syscols.ybctid);
		}
		if (ybScan->tableOid != InvalidOid)
		{
			tuple->t_tableOid = ybScan->tableOid;
		}
	}
	pfree(values);
	pfree(nulls);

	return tuple;
}

static IndexTuple ybcFetchNextIndexTuple(YbScanDesc ybScan, Relation index, bool is_forward_scan)
{
	IndexTuple tuple    = NULL;
	bool       has_data = false;
	TupleDesc  tupdesc  = ybScan->target_desc;

	Datum           *values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	bool            *nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));
	YBCPgSysColumns syscols;

	/* Execute the select statement. */
	if (!ybScan->is_exec_done)
	{
		HandleYBStatusWithOwner(YBCPgSetForwardScan(ybScan->handle, is_forward_scan),
									ybScan->handle,
									ybScan->stmt_owner);
		HandleYBStatusWithOwner(YBCPgExecSelect(ybScan->handle, ybScan->exec_params),
									ybScan->handle,
									ybScan->stmt_owner);
		ybScan->is_exec_done = true;
	}

	/* Fetch one row. */
	HandleYBStatusWithOwner(YBCPgDmlFetch(ybScan->handle,
	                                          tupdesc->natts,
	                                          (uint64_t *) values,
	                                          nulls,
	                                          &syscols,
	                                          &has_data),
	                            ybScan->handle,
	                            ybScan->stmt_owner);

	if (has_data)
	{
		/*
		 * Return the IndexTuple. If this is a primary key, reorder the values first as expected
		 * in the index's column order first.
		 */
		if (index->rd_index->indisprimary)
		{
			Assert(index->rd_index->indnatts <= INDEX_MAX_KEYS);

			Datum ivalues[INDEX_MAX_KEYS];
			bool  inulls[INDEX_MAX_KEYS];

			for (int i = 0; i < index->rd_index->indnatts; i++)
			{
				AttrNumber attno = index->rd_index->indkey.values[i];
				ivalues[i] = values[attno - 1];
				inulls[i]  = nulls[attno - 1];
			}

			tuple = index_form_tuple(RelationGetDescr(index), ivalues, inulls);
			if (syscols.ybctid != NULL)
			{
				tuple->t_ybctid = PointerGetDatum(syscols.ybctid);
			}
		}
		else
		{
			tuple = index_form_tuple(tupdesc, values, nulls);
			if (syscols.ybbasectid != NULL)
			{
				tuple->t_ybctid = PointerGetDatum(syscols.ybbasectid);
			}
		}

	}
	pfree(values);
	pfree(nulls);

	return tuple;
}

/*
 * Set up scan plan.
 * This function sets up target and bind columns for each type of scans.
 *    SELECT <Target_columns> FROM <Table> WHERE <Key_columns> op <Binds>
 *
 * 1. SequentialScan(Table) and PrimaryIndexScan(Table): index = 0
 *    - Table can be systable or usertable.
 *    - YugaByte doesn't have a separate PrimaryIndexTable. It's a special case.
 *    - Both target and bind descriptors are specified by the <Table>
 *
 * 2. IndexScan(SysTable, Index).
 *    - Target descriptor is specifed by the SysTable.
 *    - Bind descriptor is specified by the IndexTable.
 *    - For this scan, YugaByte returns a heap-tuple, which has all user's requested data.
 *
 * 3. IndexScan(UserTable, Index)
 *    - Both target and bind descriptors are specifed by the IndexTable.
 *    - For this scan, YugaByte returns an index-tuple, which has a ybctid (ROWID) to be used for
 *      querying data from the UserTable.
 *    - TODO(neil) By batching ybctid and processing it on YugaByte for all index-scans, the target
 *      for index-scan on regular table should also be the table itself (relation).
 *
 * 4. IndexOnlyScan(Table, Index)
 *    - Table can be systable or usertable.
 *    - Both target and bind descriptors are specifed by the IndexTable.
 *    - For this scan, YugaByte ALWAYS return index-tuple, which is expected by Postgres layer.
 */
static void
ybcSetupScanPlan(Relation relation, Relation index, bool xs_want_itup,
				 YbScanDesc ybScan, YbScanPlan scan_plan)
{
	int i;
	memset(scan_plan, 0, sizeof(*scan_plan));

	/*
	 * Setup control-parameters for Yugabyte preparing statements for different
	 * types of scan.
	 * - "querying_colocated_table": Support optimizations for (system and
	 *   user) colocated tables
	 * - "index_oid, index_only_scan, use_secondary_index": Different index
	 *   scans.
	 * NOTE: Primary index is a special case as there isn't a primary index
	 * table in YugaByte.
	 */
	ybScan->index = index;

	ybScan->prepare_params.querying_colocated_table =
		IsSystemRelation(relation);
	if (!ybScan->prepare_params.querying_colocated_table &&
		MyDatabaseColocated)
	{
		bool colocated = false;
		bool notfound;
		HandleYBStatusIgnoreNotFound(YBCPgIsTableColocated(MyDatabaseId,
																											 RelationGetRelid(relation),
																											 &colocated),
																 &notfound);
		ybScan->prepare_params.querying_colocated_table |= colocated;
	}
	else if (!ybScan->prepare_params.querying_colocated_table)
	{
		Oid tablegroupId = InvalidOid;
		if (TablegroupCatalogExists)
			tablegroupId = get_tablegroup_oid_by_table_oid(RelationGetRelid(relation));
		ybScan->prepare_params.querying_colocated_table |= (tablegroupId != InvalidOid);
	}

	if (index)
	{
		ybScan->prepare_params.index_oid = RelationGetRelid(index);
		ybScan->prepare_params.index_only_scan = xs_want_itup;
		ybScan->prepare_params.use_secondary_index = !index->rd_index->indisprimary;
	}

	/* Setup descriptors for target and bind. */
	if (!index || index->rd_index->indisprimary)
	{
		/*
		 * SequentialScan or PrimaryIndexScan
		 * - YugaByte does not have a separate table for PrimaryIndex.
		 * - The target table descriptor, where data is read and returned, is the main table.
		 * - The binding table descriptor, whose column is bound to values, is also the main table.
		 */
		scan_plan->target_relation = relation;
		ybcLoadTableInfo(relation, scan_plan);
		ybScan->target_desc = RelationGetDescr(relation);
		scan_plan->bind_desc = RelationGetDescr(relation);
	}
	else
	{
		/*
		 * Index-Scan: SELECT data FROM UserTable WHERE rowid IN (SELECT ybctid FROM indexTable)
		 *
		 */

		if (ybScan->prepare_params.index_only_scan)
		{
			/*
			 * IndexOnlyScan
			 * - This special case is optimized where data is read from index table.
			 * - The target table descriptor, where data is read and returned, is the index table.
			 * - The binding table descriptor, whose column is bound to values, is also the index table.
			 */
			scan_plan->target_relation = index;
			ybScan->target_desc = RelationGetDescr(index);
		}
		else
		{
			/*
			 * IndexScan ( SysTable / UserTable)
			 * - YugaByte will use the binds to query base-ybctid in the index table, which is then used
			 *   to query data from the main table.
			 * - The target table descriptor, where data is read and returned, is the main table.
			 * - The binding table descriptor, whose column is bound to values, is the index table.
			 */
			scan_plan->target_relation = relation;
			ybScan->target_desc = RelationGetDescr(relation);
		}

		ybcLoadTableInfo(index, scan_plan);
		scan_plan->bind_desc = RelationGetDescr(index);
	}

	/*
	 * Setup bind and target attnum of ScanKey.
	 * - The target-attnum comes from the table that is being read by the scan
	 * - The bind-attnum comes from the table that is being scan by the scan.
	 *
	 * Examples:
	 * - For IndexScan(SysTable, Index), SysTable is used for targets, but Index is for binds.
	 * - For IndexOnlyScan(Table, Index), only Index is used to setup both target and bind.
	 */
	for (i = 0; i < ybScan->nkeys; i++)
	{
		if (!index)
		{
			/* Sequential scan */
			ybScan->target_key_attnums[i] = ybScan->key[i].sk_attno;
			scan_plan->bind_key_attnums[i] = ybScan->key[i].sk_attno;
		}
		else if (index->rd_index->indisprimary)
		{
			/*
			 * PrimaryIndex scan: This is a special case in YugaByte. There is no PrimaryIndexTable.
			 * The table itself will be scanned.
			 */
			ybScan->target_key_attnums[i] =	scan_plan->bind_key_attnums[i] =
				index->rd_index->indkey.values[ybScan->key[i].sk_attno - 1];
		}
		else if (ybScan->prepare_params.index_only_scan)
		{
			/*
			 * IndexOnlyScan(Table, Index) returns IndexTuple.
			 * Use the index attnum for both targets and binds.
			 */
			scan_plan->bind_key_attnums[i] = ybScan->key[i].sk_attno;
			ybScan->target_key_attnums[i] =	ybScan->key[i].sk_attno;
		}
		else
		{
			/*
			 * IndexScan(SysTable or UserTable, Index) returns HeapTuple.
			 * Use SysTable attnum for targets. Use its index attnum for binds.
			 */
			scan_plan->bind_key_attnums[i] = ybScan->key[i].sk_attno;
			ybScan->target_key_attnums[i] =	index->rd_index->indkey.values[ybScan->key[i].sk_attno - 1];
		}
	}
}

static bool ybc_should_pushdown_op(YbScanPlan scan_plan, AttrNumber attnum, int op_strategy)
{
	const int idx =  YBAttnumToBmsIndex(scan_plan->target_relation, attnum);

	switch (op_strategy)
	{
		case BTEqualStrategyNumber:
			return bms_is_member(idx, scan_plan->primary_key);

		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			/* range key */
			return (!bms_is_member(idx, scan_plan->hash_key) &&
				bms_is_member(idx, scan_plan->primary_key));

		default:
			/* TODO: support other logical operators */
			return false;
	}
}

/*
 * Is this a basic (c =/</<=/>=/> value) (in)equality condition.
 * TODO: The null value case (SK_ISNULL) should always evaluate to false
 *       per SQL semantics but in DocDB it will be true. So this case
 *       will require PG filtering (for null values only).
 */
static bool IsBasicOpSearch(int sk_flags) {
	return sk_flags == 0 || sk_flags == SK_ISNULL;
}

/*
 * Is this a null search (c IS NULL) -- same as equality cond for DocDB.
 */
static bool IsSearchNull(int sk_flags) {
	return sk_flags == (SK_ISNULL | SK_SEARCHNULL);
}

/*
 * Is this an array search (c = ANY(..) or c IN ..).
 */
static bool IsSearchArray(int sk_flags) {
	return sk_flags == SK_SEARCHARRAY;
}

static bool
ShouldPushdownScanKey(Relation relation, YbScanPlan scan_plan, AttrNumber attnum,
                      ScanKey key, bool is_primary_key) {
	if (IsSystemRelation(relation))
	{
		/*
		 * Only support eq operators for system tables.
		 * TODO: we can probably allow ineq conditions for system tables now.
		 */
		return IsBasicOpSearch(key->sk_flags) &&
			key->sk_strategy == BTEqualStrategyNumber &&
			is_primary_key;
	}
	else
	{
		if (IsBasicOpSearch(key->sk_flags))
		{
			/* Eq strategy for hash key, eq + ineq for range key. */
			return ybc_should_pushdown_op(scan_plan, attnum, key->sk_strategy);
		}

		if (IsSearchNull(key->sk_flags))
		{
			/* Always expect InvalidStrategy for NULL search. */
			Assert(key->sk_strategy == InvalidStrategy);
			return is_primary_key;
		}

		if (IsSearchArray(key->sk_flags))
		{
			/*
			 * Expect equal strategy here (i.e. IN .. or = ANY(..) conditions,
			 * NOT IN will generate <> which is not a supported LSM/BTREE
			 * operator, so it should not get to this point.
			 */
			Assert(key->sk_strategy == BTEqualStrategyNumber);
			return is_primary_key;
		}
		/* No other operators are supported. */
		return false;
	}
}

/* int comparator for qsort() */
static int int_compar_cb(const void *v1, const void *v2)
{
  const int *k1 = v1;
  const int *k2 = v2;

  if (*k1 < *k2)
    return -1;

  if (*k1 > *k2)
    return 1;

  return 0;
}

/* Use the scan-descriptor and scan-plan to setup scan key for filtering */
static void	ybcSetupScanKeys(Relation relation,
							 Relation index,
							 YbScanDesc ybScan,
							 YbScanPlan scan_plan)
{
	/*
	 * Find the scan keys that are the primary key.
	 */
	for (int i = 0; i < ybScan->nkeys; i++)
	{
		if (scan_plan->bind_key_attnums[i] == InvalidOid)
			break;

		int idx = YBAttnumToBmsIndex(scan_plan->target_relation, scan_plan->bind_key_attnums[i]);
		/*
		 * TODO: Can we have bound keys on non-pkey columns here?
		 *       If not we do not need the is_primary_key below.
		 */
		bool is_primary_key = bms_is_member(idx, scan_plan->primary_key);

		if (!ShouldPushdownScanKey(relation, scan_plan, scan_plan->bind_key_attnums[i],
		                           &ybScan->key[i], is_primary_key))
			continue;

		if (is_primary_key)
			scan_plan->sk_cols = bms_add_member(scan_plan->sk_cols, idx);
	}

	/*
	 * All or some of the keys should not be pushed down if any of the following is true.
	 * - If hash key is not fully set, we must do a full-table scan so we will clear all the scan
	 * keys.
	 * - For RANGE columns, if condition on a precedent column in RANGE is not specified, the
	 * subsequent columns in RANGE are dropped from the optimization.
	 *
	 * Implementation Notes:
	 * Because internally, hash and range columns are cached and stored prior to other columns in
	 * YugaByte, the columns' indexes are different from the columns' attnum.
	 * Example:
	 *   CREATE TABLE tab(i int, j int, k int, primary key(k HASH, j ASC))
	 *   Column k's index is 1, but its attnum is 3.
	 *
	 * Additionally, we currently have the following setup.
	 * - For PRIMARY KEY SCAN, the key is specified by columns' attnums by both Postgres and YugaByte
	 *   code components.
	 * - For SECONDARY INDEX SCAN and INDEX-ONLY SCAN, column_attnums and column_indexes are
	 *   identical, so they can be both used interchangeably. This is because of CREATE_INDEX
	 *   syntax rules enforce that HASH columns are specified before RANGE columns which comes
	 *   before INCLUDE columns.
	 * - For SYSTEM SCAN, Postgres's layer use attnums to specify a catalog INDEX, but YugaByte
	 *   layer is using column_indexes to specify them.
	 * - For SEQUENTIAL SCAN, column_attnums and column_indexes are the same.
	 *
	 * TODO(neil) The above differences between different INDEX code path should be changed so that
	 * different kinds of indexes and scans share the same behavior.
	 */
	bool delete_key = !bms_is_subset(scan_plan->hash_key, scan_plan->sk_cols);
	if (index && index->rd_index->indisprimary) {
		/* For primary key, column_attnums are used, so we process it different from other scans */
		for (int i = 0; i < index->rd_index->indnatts; i++) {
			int key_column = YBAttnumToBmsIndex(index, index->rd_index->indkey.values[i]);
			if (!delete_key && !bms_is_member(key_column, scan_plan->sk_cols)) {
				delete_key = true;
			}

			if (delete_key)
				bms_del_member(scan_plan->sk_cols, key_column);
		}
	} else {
		int max_idx = YBAttnumToBmsIndex(relation, scan_plan->bind_desc->natts);
		for (int idx = 0; idx <= max_idx; idx++)
		{
			if (!delete_key &&
					bms_is_member(idx, scan_plan->primary_key) &&
					!bms_is_member(idx, scan_plan->sk_cols))
				delete_key = true;

			if (delete_key)
				bms_del_member(scan_plan->sk_cols, idx);
		}
	}
}

/* Use the scan-descriptor and scan-plan to setup binds for the queryplan */
static void ybcBindScanKeys(Relation relation,
							Relation index,
							YbScanDesc ybScan,
							YbScanPlan scan_plan) {
	Oid		dboid    = YBCGetDatabaseOid(relation);
	Oid		relid    = RelationGetRelid(relation);

	HandleYBStatus(YBCPgNewSelect(dboid, relid, &ybScan->prepare_params, &ybScan->handle));
	ResourceOwnerEnlargeYugaByteStmts(CurrentResourceOwner);
	ResourceOwnerRememberYugaByteStmt(CurrentResourceOwner, ybScan->handle);
	ybScan->stmt_owner = CurrentResourceOwner;

	if (IsSystemRelation(relation))
	{
		/* Bind the scan keys */
		for (int i = 0; i < ybScan->nkeys; i++)
		{
			int idx = YBAttnumToBmsIndex(relation, scan_plan->bind_key_attnums[i]);
			if (bms_is_member(idx, scan_plan->sk_cols))
			{
				bool is_null = (ybScan->key[i].sk_flags & SK_ISNULL) == SK_ISNULL;

				ybcBindColumn(ybScan, scan_plan->bind_desc, scan_plan->bind_key_attnums[i],
							  ybScan->key[i].sk_argument, is_null);
			}
		}
	}
	else
	{
		/* Find max number of cols in schema in use in query */
		int max_idx = 0;
		for (int i = 0; i < ybScan->nkeys; i++)
		{
			int idx = YBAttnumToBmsIndex(relation, scan_plan->bind_key_attnums[i]);
			if (!bms_is_member(idx, scan_plan->sk_cols))
				continue;

			if (max_idx < idx)
				max_idx = idx;
		}
		max_idx++;

		/* Find intervals for columns */

		bool is_column_bound[max_idx]; /* VLA - scratch space */
		memset(is_column_bound, 0, sizeof(bool) * max_idx);

		bool start_valid[max_idx]; /* VLA - scratch space */
		memset(start_valid, 0, sizeof(bool) * max_idx);

		bool end_valid[max_idx]; /* VLA - scratch space */
		memset(end_valid, 0, sizeof(bool) * max_idx);

		Datum start[max_idx]; /* VLA - scratch space */
		Datum end[max_idx]; /* VLA - scratch space */

		/*
		 * find an order of relevant keys such that for the same column, an EQUAL
		 * condition is encountered before IN or BETWEEN. is_column_bound is then used
		 * to establish priority order EQUAL > IN > BETWEEN.
		 */
		int noffsets = 0;
		int offsets[ybScan->nkeys + 1]; /* VLA - scratch space: +1 to avoid zero elements */

		for (int i = 0; i < ybScan->nkeys; i++)
		{
			/* Check if this is primary columns */
			int idx = YBAttnumToBmsIndex(relation, scan_plan->bind_key_attnums[i]);
			if (!bms_is_member(idx, scan_plan->sk_cols))
				continue;

			/* Assign key offsets */
			switch (ybScan->key[i].sk_strategy)
			{
				case InvalidStrategy:
					/* Should be ensured during planning. */
					Assert(IsSearchNull(ybScan->key[i].sk_flags));
					/* fallthrough  -- treating IS NULL as (DocDB) = (null) */
				case BTEqualStrategyNumber:
					if (IsBasicOpSearch(ybScan->key[i].sk_flags) ||
						IsSearchNull(ybScan->key[i].sk_flags))
					{
						/* Use a -ve value so that qsort places EQUAL before others */
						offsets[noffsets++] = -i;
					}
					else if (IsSearchArray(ybScan->key[i].sk_flags))
					{
						offsets[noffsets++] = i;
					}
					break;
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					offsets[noffsets++] = i;

				default:
					break; /* unreachable */
			}
		}

		qsort(offsets, noffsets, sizeof(int), int_compar_cb);
		/* restore -ve offsets to +ve */
		for (int i = 0; i < noffsets; i++)
		if (offsets[i] < 0)
			offsets[i] = -offsets[i];
		else
			break;

		/* Bind keys for EQUALS and IN */
		for (int k = 0; k < noffsets; k++)
		{
			int i = offsets[k];
			int idx = YBAttnumToBmsIndex(relation, scan_plan->bind_key_attnums[i]);

			/* Do not bind more than one condition to a column */
			if (is_column_bound[idx])
				continue;

			bool is_hash_key = bms_is_member(idx, scan_plan->hash_key);
			bool is_primary_key = bms_is_member(idx, scan_plan->primary_key); // Includes hash key

			switch (ybScan->key[i].sk_strategy)
			{
				case InvalidStrategy: /* fallthrough, c IS NULL -> c = NULL (checked above) */
				case BTEqualStrategyNumber:
					/* Bind the scan keys */
					if (IsBasicOpSearch(ybScan->key[i].sk_flags) ||
						IsSearchNull(ybScan->key[i].sk_flags))
					{
						/* Either c = NULL or c IS NULL. */
						bool is_null = (ybScan->key[i].sk_flags & SK_ISNULL) == SK_ISNULL;
						ybcBindColumnCondEq(ybScan, is_hash_key, scan_plan->bind_desc,
											scan_plan->bind_key_attnums[i],
											ybScan->key[i].sk_argument, is_null);
						is_column_bound[idx] = true;
					}
					else if (IsSearchArray(ybScan->key[i].sk_flags) && is_primary_key)
					{
						/* based on _bt_preprocess_array_keys() */
						ArrayType  *arrayval;
						int16		elmlen;
						bool		elmbyval;
						char		elmalign;
						int			num_elems;
						Datum	   *elem_values;
						bool	   *elem_nulls;
						int			num_nonnulls;
						int			j;

						ScanKey cur = &ybScan->key[i];

						/*
						 * First, deconstruct the array into elements.  Anything allocated
						 * here (including a possibly detoasted array value) is in the
						 * workspace context.
						 */
						arrayval = DatumGetArrayTypeP(cur->sk_argument);
						/* We could cache this data, but not clear it's worth it */
						get_typlenbyvalalign(ARR_ELEMTYPE(arrayval), &elmlen,
						                     &elmbyval, &elmalign);
						deconstruct_array(arrayval,
						                  ARR_ELEMTYPE(arrayval),
						                  elmlen, elmbyval, elmalign,
						                  &elem_values, &elem_nulls, &num_elems);

						/*
						 * Compress out any null elements.  We can ignore them since we assume
						 * all btree operators are strict.
						 */
						num_nonnulls = 0;
						for (j = 0; j < num_elems; j++)
						{
							if (!elem_nulls[j])
							elem_values[num_nonnulls++] = elem_values[j];
						}

						/* We could pfree(elem_nulls) now, but not worth the cycles */

						/*
						 * If there's no non-nulls, the scan qual is unsatisfiable
						 * TODO(rajukumaryb): when num_nonnulls is zero, the query should not be
						 * sent to DocDB as it will return rows that will all be dropped.
						 * Example: SELECT ... FROM ... WHERE h = ... AND r IN (NULL,NULL);
						 */
						if (num_nonnulls == 0)
							break;

						/* Build temporary vars */
						IndexScanDescData tmp_scan_desc;
						memset(&tmp_scan_desc, 0, sizeof(IndexScanDescData));
						tmp_scan_desc.indexRelation = index;

						/*
						 * Sort the non-null elements and eliminate any duplicates.  We must
						 * sort in the same ordering used by the index column, so that the
						 * successive primitive indexscans produce data in index order.
						 */
						num_elems = _bt_sort_array_elements(&tmp_scan_desc, cur,
						                                    false /* reverse */,
						                                    elem_values, num_nonnulls);

						/*
						 * And set up the BTArrayKeyInfo data.
						 */
						ybcBindColumnCondIn(ybScan, scan_plan->bind_desc, scan_plan->bind_key_attnums[i],
						                    num_elems, elem_values);
						is_column_bound[idx] = true;
					} else {
						/* unreachable */
					}
					break;

				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
					if (start_valid[idx]) {
						/* take max of old value and new value */
						bool is_gt = DatumGetBool(FunctionCall2Coll(&ybScan->key[i].sk_func,
						                                            ybScan->key[i].sk_collation,
						                                            start[idx],
						                                            ybScan->key[i].sk_argument));
						if (!is_gt) {
						start[idx] = ybScan->key[i].sk_argument;
						}
					}
					else
					{
						start[idx] = ybScan->key[i].sk_argument;
						start_valid[idx] = true;
					}
					break;

				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					if (end_valid[idx])
					{
						/* take min of old value and new value */
						bool is_lt = DatumGetBool(FunctionCall2Coll(&ybScan->key[i].sk_func,
						                                            ybScan->key[i].sk_collation,
						                                            end[idx],
						                                            ybScan->key[i].sk_argument));
						if (!is_lt) {
							end[idx] = ybScan->key[i].sk_argument;
						}
					}
					else
					{
						end[idx] = ybScan->key[i].sk_argument;
						end_valid[idx] = true;
					}
					break;

				default:
					break; /* unreachable */
			}
		}

		/* Bind keys for BETWEEN */
		int min_idx = YBAttnumToBmsIndex(relation, 1);
		for (int idx = min_idx; idx < max_idx; idx++)
		{
			/* Do not bind more than one condition to a column */
			if (is_column_bound[idx])
				continue;

			if (!start_valid[idx] && !end_valid[idx])
				continue;

			ybcBindColumnCondBetween(ybScan,
			                         scan_plan->bind_desc,
									 YBBmsIndexToAttnum(relation, idx),
									 start_valid[idx], start[idx],
									 end_valid[idx], end[idx]);
			is_column_bound[idx] = true;
		}
	}
}

/* Setup the targets */
static void ybcSetupTargets(Relation relation,
														Relation index,
														YbScanDesc ybScan,
														YbScanPlan scan_plan) {
	if (scan_plan->target_relation->rd_rel->relhasoids)
		ybcAddTargetColumn(ybScan, ObjectIdAttributeNumber);

	if (ybScan->prepare_params.index_only_scan && index->rd_index->indisprimary)
		/*
		 * Special case: For Primary-Key-ONLY-Scan, we select ONLY the primary key from the target
		 * table instead of the whole target table.
		 */
		for (int i = 0; i < index->rd_index->indnatts; i++)
			ybcAddTargetColumn(ybScan, index->rd_index->indkey.values[i]);
	else
		for (AttrNumber attnum = 1; attnum <= ybScan->target_desc->natts; attnum++)
			ybcAddTargetColumn(ybScan, attnum);

	if (scan_plan->target_relation->rd_index)
		/*
		 * IndexOnlyScan:
		 *   SELECT [ data, ] ybbasectid (ROWID of UserTable, relation) FROM secondary-index-table
		 * In this case, Postgres requests base_ctid and maybe also data from IndexTable and then uses
		 * them for further processing.
		 */
		ybcAddTargetColumn(ybScan, YBIdxBaseTupleIdAttributeNumber);
	else
	{
		/* Two cases:
		 * - Primary Scan (Key or sequential)
		 *     SELECT data, ybctid FROM table [ WHERE primary-key-condition ]
		 * - Secondary IndexScan
		 *     SELECT data, ybctid FROM table WHERE ybctid IN ( SELECT base_ybctid FROM IndexTable )
		 */
		ybcAddTargetColumn(ybScan, YBTupleIdAttributeNumber);
		if (index && !index->rd_index->indisprimary)
		{
			/*
			 * IndexScan: Postgres layer sends both actual-query and index-scan to PgGate, who will
			 * select and immediately use base_ctid to query data before responding.
			 */
			ybcAddTargetColumn(ybScan, YBIdxBaseTupleIdAttributeNumber);
		}
	}
}

/*
 * Begin a scan for
 *   SELECT <Targets> FROM <Relation relation> USING <Relation index>
 * NOTES:
 * - "relation" is the table being SELECTed.
 * - "index" identify the INDEX that will be used for scaning.
 * - "nkeys" and "key" identify which key columns are provided in the SELECT WHERE clause.
 *   nkeys = Number of key.
 *   keys[].sk_attno = the columns' attnum in the IndexTable or "index"
 *                     (This is not the attnum in UserTable or "relation")
 *
 * - If "xs_want_itup" is true, Postgres layer is expecting an IndexTuple that has ybctid to
 *   identify the desired row.
 */
YbScanDesc
ybcBeginScan(Relation relation, Relation index, bool xs_want_itup, int nkeys, ScanKey key)
{
	if (nkeys > YB_MAX_SCAN_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d predicates in a table or index scan",
						YB_MAX_SCAN_KEYS)));

	/* Set up YugaByte scan description */
	YbScanDesc ybScan = (YbScanDesc) palloc0(sizeof(YbScanDescData));
	ybScan->key   = key;
	ybScan->nkeys = nkeys;
	ybScan->exec_params = NULL;
	ybScan->tableOid = RelationGetRelid(relation);

	/* Setup the scan plan */
	YbScanPlanData	scan_plan;
	ybcSetupScanPlan(relation, index, xs_want_itup, ybScan, &scan_plan);

	/* Setup binds for the scan-key */
	ybcSetupScanKeys(relation, index, ybScan, &scan_plan);
	ybcBindScanKeys(relation, index, ybScan, &scan_plan);

	/*
	 * Set up the scan targets. If the table is indexed and only the indexed columns should be
	 * returned, fetch just those columns. Otherwise, fetch all "real" columns.
	 */
	ybcSetupTargets(relation, index, ybScan, &scan_plan);

	/*
	 * Set the current syscatalog version (will check that we are up to date).
	 * Avoid it for syscatalog tables so that we can still use this for
	 * refreshing the caches when we are behind.
	 * Note: This works because we do not allow modifying schemas (alter/drop)
	 * for system catalog tables.
	 */
	if (!IsSystemRelation(relation))
	{
		HandleYBStatusWithOwner(YBCPgSetCatalogCacheVersion(ybScan->handle,
		                                                        yb_catalog_cache_version),
		                            ybScan->handle,
		                            ybScan->stmt_owner);
	}

	bms_free(scan_plan.hash_key);
	bms_free(scan_plan.primary_key);
	bms_free(scan_plan.sk_cols);

	return ybScan;
}

void ybcEndScan(YbScanDesc ybScan)
{
	if (ybScan->handle)
	{
		ResourceOwnerForgetYugaByteStmt(ybScan->stmt_owner, ybScan->handle);
	}
	pfree(ybScan);
}

static bool
heaptuple_matches_key(HeapTuple tup,
					  TupleDesc tupdesc,
					  int nkeys,
					  ScanKey key,
					  AttrNumber sk_attno[],
					  bool *recheck)
{
	*recheck = false;

	for (int i = 0; i < nkeys; i++)
	{
		if (sk_attno[i] == InvalidOid)
			break;

		bool  is_null = false;
		Datum res_datum = heap_getattr(tup, sk_attno[i], tupdesc, &is_null);

		if (key[i].sk_flags & SK_SEARCHNULL)
		{
			if (is_null)
				continue;
			else
				return false;
		}

		if (key[i].sk_flags & SK_SEARCHNOTNULL)
		{
			if (!is_null)
				continue;
			else
				return false;
		}

		/*
		 * TODO: support the different search options like SK_SEARCHARRAY.
		 */
		if (key[i].sk_flags != 0)
		{
			*recheck = true;
			continue;
		}

		if (is_null)
			return false;

		bool matches = DatumGetBool(FunctionCall2Coll(&key[i].sk_func,
		                                              key[i].sk_collation,
		                                              res_datum,
		                                              key[i].sk_argument));
		if (!matches)
			return false;
	}

	return true;
}

static bool
indextuple_matches_key(IndexTuple tup,
					   TupleDesc tupdesc,
					   int nkeys,
					   ScanKey key,
					   AttrNumber sk_attno[],
					   bool *recheck)
{
	*recheck = false;

	for (int i = 0; i < nkeys; i++)
	{
		if (sk_attno[i] == InvalidOid)
			break;

		bool  is_null = false;
		Datum res_datum = index_getattr(tup, sk_attno[i], tupdesc, &is_null);

		if (key[i].sk_flags & SK_SEARCHNULL)
		{
			if (is_null)
				continue;
			else
				return false;
		}

		if (key[i].sk_flags & SK_SEARCHNOTNULL)
		{
			if (!is_null)
				continue;
			else
				return false;
		}

		/*
		 * TODO: support the different search options like SK_SEARCHARRAY.
		 */
		if (key[i].sk_flags != 0)
		{
			*recheck = true;
			continue;
		}

		if (is_null)
			return false;

		bool matches = DatumGetBool(FunctionCall2Coll(&key[i].sk_func,
		                                              key[i].sk_collation,
		                                              res_datum,
		                                              key[i].sk_argument));
		if (!matches)
			return false;
	}

	return true;
}

HeapTuple ybc_getnext_heaptuple(YbScanDesc ybScan, bool is_forward_scan, bool *recheck)
{
	int         nkeys    = ybScan->nkeys;
	ScanKey     key      = ybScan->key;
	AttrNumber *sk_attno = ybScan->target_key_attnums;
	HeapTuple   tup      = NULL;

	/*
	 * YB Scan may not be able to push down the scan key condition so we may
	 * need additional filtering here.
	 */
	while (HeapTupleIsValid(tup = ybcFetchNextHeapTuple(ybScan, is_forward_scan)))
	{
		if (heaptuple_matches_key(tup, ybScan->target_desc, nkeys, key, sk_attno, recheck))
			return tup;

		heap_freetuple(tup);
	}

	return NULL;
}

IndexTuple ybc_getnext_indextuple(YbScanDesc ybScan, bool is_forward_scan, bool *recheck)
{
	int         nkeys    = ybScan->nkeys;
	ScanKey     key      = ybScan->key;
	AttrNumber *sk_attno = ybScan->target_key_attnums;
	Relation    index    = ybScan->index;
	IndexTuple  tup      = NULL;

	/*
	 * YB Scan may not be able to push down the scan key condition so we may
	 * need additional filtering here.
	 */
	while (PointerIsValid(tup = ybcFetchNextIndexTuple(ybScan, index, is_forward_scan)))
	{
		if (indextuple_matches_key(tup, RelationGetDescr(index), nkeys, key, sk_attno, recheck))
			return tup;

		pfree(tup);
	}

	return NULL;
}

SysScanDesc ybc_systable_beginscan(Relation relation,
                                   Oid indexId,
                                   bool indexOK,
                                   Snapshot snapshot,
                                   int nkeys,
                                   ScanKey key)
{
	Relation index = NULL;

	/*
	 * Look up the index to scan with if we can. If the index is the primary key which is part
	 * of the table in YugaByte, we should scan the table directly.
	 */
	if (indexOK && !IgnoreSystemIndexes && !ReindexIsProcessingIndex(indexId))
	{
		index = RelationIdGetRelation(indexId);
		if (index->rd_index->indisprimary)
		{
			RelationClose(index);
			index = NULL;
		}

		if (index) {
			/*
			 * Change attribute numbers to be index column numbers.
			 * - This conversion is the same as function systable_beginscan() in file "genam.c". If we
			 *   ever reuse Postgres index code, this conversion is a must because the key entries must
			 *   match what Postgres code expects.
			 *
			 * - When selecting using INDEX, the key values are bound to the IndexTable, so index attnum
			 *   must be used for bindings.
			 */
			int i, j;
			for (i = 0; i < nkeys; i++)
			{
				for (j = 0; j < IndexRelationGetNumberOfAttributes(index); j++)
				{
					if (key[i].sk_attno == index->rd_index->indkey.values[j])
					{
						key[i].sk_attno = j + 1;
						break;
					}
				}
				if (j == IndexRelationGetNumberOfAttributes(index))
					elog(ERROR, "column is not in index");
			}
		}
	}

	YbScanDesc ybScan = ybcBeginScan(relation, index, false /* xs_want_itup */, nkeys, key);

	/* Set up Postgres sys table scan description */
	SysScanDesc scan_desc = (SysScanDesc) palloc0(sizeof(SysScanDescData));
	scan_desc->heap_rel   = relation;
	scan_desc->snapshot   = snapshot;
	scan_desc->ybscan     = ybScan;

	if (index)
	{
		RelationClose(index);
	}

	return scan_desc;
}

HeapTuple ybc_systable_getnext(SysScanDesc scan_desc)
{
	bool recheck = false;

	Assert(PointerIsValid(scan_desc->ybscan));

	HeapTuple tuple = ybc_getnext_heaptuple(scan_desc->ybscan, true /* is_forward_scan */,
											&recheck);

	Assert(!recheck);

	return tuple;
}


void ybc_systable_endscan(SysScanDesc scan_desc)
{
	Assert(PointerIsValid(scan_desc->ybscan));
	ybcEndScan(scan_desc->ybscan);
}

HeapScanDesc ybc_heap_beginscan(Relation relation,
                                Snapshot snapshot,
                                int nkeys,
                                ScanKey key,
                                bool temp_snap)
{
	/* Restart should not be prevented if operation caused by system read of system table. */
	YbScanDesc ybScan = ybcBeginScan(relation, NULL /* index */, false /* xs_want_itup */,
	                                 nkeys, key);

	/* Set up Postgres sys table scan description */
	HeapScanDesc scan_desc = (HeapScanDesc) palloc0(sizeof(HeapScanDescData));
	scan_desc->rs_rd        = relation;
	scan_desc->rs_snapshot  = snapshot;
	scan_desc->rs_temp_snap = temp_snap;
	scan_desc->rs_cblock    = InvalidBlockNumber;
	scan_desc->ybscan       = ybScan;

	return scan_desc;
}

HeapTuple ybc_heap_getnext(HeapScanDesc scan_desc)
{
	bool recheck = false;

	Assert(PointerIsValid(scan_desc->ybscan));

	HeapTuple tuple = ybc_getnext_heaptuple(scan_desc->ybscan, true /* is_forward_scan */,
											&recheck);

	Assert(!recheck);

	return tuple;
}

void ybc_heap_endscan(HeapScanDesc scan_desc)
{
	Assert(PointerIsValid(scan_desc->ybscan));
	ybcEndScan(scan_desc->ybscan);
	if (scan_desc->rs_temp_snap)
		UnregisterSnapshot(scan_desc->rs_snapshot);
	pfree(scan_desc);
}

/* --------------------------------------------------------------------------------------------- */

void ybcCostEstimate(RelOptInfo *baserel, Selectivity selectivity,
					 bool is_backwards_scan, bool is_uncovered_idx_scan,
					 Cost *startup_cost, Cost *total_cost)
{
	/*
	 * Yugabyte-specific per-tuple cost considerations:
	 *   - 10x the regular CPU cost to account for network/RPC + DocDB overhead.
	 *   - backwards scan scale factor as it will need that many more fetches
	 *     to get all rows/tuples.
	 *   - uncovered index scan is more costly than index-only or seq scan because
	 *     it requires extra request to the main table.
	 */
	Cost yb_per_tuple_cost_factor = 10;
	if (is_backwards_scan)
	{
		yb_per_tuple_cost_factor *= YBC_BACKWARDS_SCAN_COST_FACTOR;
	}
	if (is_uncovered_idx_scan)
	{
		yb_per_tuple_cost_factor *= YBC_UNCOVERED_INDEX_COST_FACTOR;
	}

	Cost cost_per_tuple = cpu_tuple_cost * yb_per_tuple_cost_factor +
	                      baserel->baserestrictcost.per_tuple;

	*startup_cost = baserel->baserestrictcost.startup;

	*total_cost   = *startup_cost + cost_per_tuple * baserel->tuples * selectivity;
}

/*
 * Evaluate the selectivity for some qualified cols given the hash and primary key cols.
 * TODO this should look into the actual operators and distinguish, for instance
 * equality and inequality conditions (for ASC/DESC columns) better.
 */
static double ybcIndexEvalClauseSelectivity(Bitmapset *qual_cols,
                                            bool is_unique_idx,
                                            Bitmapset *hash_key,
                                            Bitmapset *primary_key)
{
	/*
	 * If there is no search condition, or not all of the hash columns have
	 * search conditions, it will be a full-table scan.
	 */
	if (bms_is_empty(qual_cols) || !bms_is_subset(hash_key, qual_cols))
	{
		return YBC_FULL_SCAN_SELECTIVITY;
	}

	/*
	 * Otherwise, it will be either a primary key lookup or range scan
	 * on a hash key.
	 */
	if (bms_is_subset(primary_key, qual_cols))
	{
		/* For unique indexes full key guarantees single row. */
		return is_unique_idx ? YBC_SINGLE_ROW_SELECTIVITY
						     : YBC_SINGLE_KEY_SELECTIVITY;
	}

	return YBC_HASH_SCAN_SELECTIVITY;
}

void ybcIndexCostEstimate(IndexPath *path, Selectivity *selectivity,
						  Cost *startup_cost, Cost *total_cost)
{
	Relation	index = RelationIdGetRelation(path->indexinfo->indexoid);
	bool		isprimary = index->rd_index->indisprimary;
	Relation	relation = isprimary ? RelationIdGetRelation(index->rd_index->indrelid) : NULL;
	RelOptInfo *baserel = path->path.parent;
	List	   *qinfos;
	ListCell   *lc;
	bool        is_backwards_scan = path->indexscandir == BackwardScanDirection;
	bool        is_unique = index->rd_index->indisunique;
	bool        is_partial_idx = path->indexinfo->indpred != NIL && path->indexinfo->predOK;
	Bitmapset  *const_quals = NULL;

	/* Primary-index scans are always covered in Yugabyte (internally) */
	bool       is_uncovered_idx_scan = !index->rd_index->indisprimary &&
	                                   path->path.pathtype != T_IndexOnlyScan;

	YbScanPlanData	scan_plan;
	memset(&scan_plan, 0, sizeof(scan_plan));
	scan_plan.target_relation = isprimary ? relation : index;
	ybcLoadTableInfo(scan_plan.target_relation, &scan_plan);

	/* Do preliminary analysis of indexquals */
	qinfos = deconstruct_indexquals(path);

	/* Find out the search conditions on the primary key columns */
	foreach(lc, qinfos)
	{
		IndexQualInfo *qinfo = (IndexQualInfo *) lfirst(lc);
		RestrictInfo *rinfo = qinfo->rinfo;
		AttrNumber	 attnum = isprimary ? index->rd_index->indkey.values[qinfo->indexcol]
										: (qinfo->indexcol + 1);
		Expr	   *clause = rinfo->clause;
		Oid			clause_op;
		int			op_strategy;
		int			bms_idx = YBAttnumToBmsIndex(scan_plan.target_relation, attnum);

		if (IsA(clause, NullTest))
		{
			NullTest *nt = (NullTest *) clause;
			/* We only support IS NULL (i.e. not IS NOT NULL). */
			if (nt->nulltesttype == IS_NULL)
			{
				const_quals = bms_add_member(const_quals, bms_idx);
				ybcAddAttributeColumn(&scan_plan, attnum);
			}
		}
		else
		{
			clause_op = qinfo->clause_op;

			if (OidIsValid(clause_op))
			{
				op_strategy = get_op_opfamily_strategy(clause_op,
													   path->indexinfo->opfamily[qinfo->indexcol]);
				Assert(op_strategy != 0);  /* not a member of opfamily?? */

				if (ybc_should_pushdown_op(&scan_plan, attnum, op_strategy))
				{
					ybcAddAttributeColumn(&scan_plan, attnum);
					if (qinfo->other_operand && IsA(qinfo->other_operand, Const))
						const_quals = bms_add_member(const_quals, bms_idx);
				}
			}
		}
	}

	/*
	 * If there is no search condition, or not all of the hash columns have search conditions, it
	 * will be a full-table scan. Otherwise, it will be either a primary key lookup or range scan
	 * on a hash key.
	 */
	*selectivity = ybcIndexEvalClauseSelectivity(scan_plan.sk_cols,
	                                             is_unique,
	                                             scan_plan.hash_key,
	                                             scan_plan.primary_key);
	path->path.rows = baserel->tuples * (*selectivity);

	/*
	 * For partial indexes, scale down the rows to account for the predicate.
	 * Do this after setting the baserel rows since this does not apply to base rel.
	 * TODO: this should be evaluated based on the index condition in the future.
	 */
	if (is_partial_idx)
	{
		*selectivity *= YBC_PARTIAL_IDX_PRED_SELECTIVITY;
	}

	ybcCostEstimate(baserel, *selectivity, is_backwards_scan,
	                is_uncovered_idx_scan, startup_cost, total_cost);

	/*
	 * Try to evaluate the number of rows this baserel might return.
	 * We cannot rely on the join conditions here (e.g. t1.c1 = t2.c2) because
	 * they may not be applied if another join path is chosen.
	 * So only use the t1.c1 = <const_value> quals (filtered above) for this.
	 */
	double const_qual_selectivity = ybcIndexEvalClauseSelectivity(const_quals,
	                                                              is_unique,
	                                                              scan_plan.hash_key,
	                                                              scan_plan.primary_key);
	double baserel_rows_estimate = const_qual_selectivity * baserel->tuples;
	if (baserel_rows_estimate < baserel->rows)
	{
		baserel->rows = baserel_rows_estimate;
	}


	if (relation)
		RelationClose(relation);

	RelationClose(index);
}

HeapTuple YBCFetchTuple(Relation relation, Datum ybctid)
{
	YBCPgStatement ybc_stmt;
	TupleDesc      tupdesc = RelationGetDescr(relation);

	HandleYBStatus(YBCPgNewSelect(YBCGetDatabaseOid(relation),
																RelationGetRelid(relation),
																NULL /* prepare_params */,
																&ybc_stmt));

	/* Bind ybctid to identify the current row. */
	YBCPgExpr ybctid_expr = YBCNewConstant(ybc_stmt,
										   BYTEAOID,
										   ybctid,
										   false);
	HandleYBStatus(YBCPgDmlBindColumn(ybc_stmt, YBTupleIdAttributeNumber, ybctid_expr));

	/*
	 * Set up the scan targets. For index-based scan we need to return all "real" columns.
	 */
	if (RelationGetForm(relation)->relhasoids)
	{
		YBCPgTypeAttrs type_attrs = { 0 };
		YBCPgExpr   expr = YBCNewColumnRef(ybc_stmt, ObjectIdAttributeNumber, InvalidOid,
										   &type_attrs);
		HandleYBStatus(YBCPgDmlAppendTarget(ybc_stmt, expr));
	}
	for (AttrNumber attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);
		YBCPgTypeAttrs type_attrs = { att->atttypmod };
		YBCPgExpr   expr = YBCNewColumnRef(ybc_stmt, attnum, att->atttypid, &type_attrs);
		HandleYBStatus(YBCPgDmlAppendTarget(ybc_stmt, expr));
	}
	YBCPgTypeAttrs type_attrs = { 0 };
	YBCPgExpr   expr = YBCNewColumnRef(ybc_stmt, YBTupleIdAttributeNumber, InvalidOid,
									   &type_attrs);
	HandleYBStatus(YBCPgDmlAppendTarget(ybc_stmt, expr));

	/*
	 * Execute the select statement.
	 * This select statement fetch the row for a specific YBCTID, LIMIT setting is not needed.
	 */
	HandleYBStatus(YBCPgExecSelect(ybc_stmt, NULL /* exec_params */));

	HeapTuple tuple    = NULL;
	bool      has_data = false;

	Datum           *values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	bool            *nulls  = (bool *) palloc(tupdesc->natts * sizeof(bool));
	YBCPgSysColumns syscols;

	/* Fetch one row. */
	HandleYBStatus(YBCPgDmlFetch(ybc_stmt,
															 tupdesc->natts,
															 (uint64_t *) values,
															 nulls,
															 &syscols,
															 &has_data));

	if (has_data)
	{
		tuple = heap_form_tuple(tupdesc, values, nulls);

		if (syscols.oid != InvalidOid)
		{
			HeapTupleSetOid(tuple, syscols.oid);
		}
		if (syscols.ybctid != NULL)
		{
			tuple->t_ybctid = PointerGetDatum(syscols.ybctid);
		}
		tuple->t_tableOid = RelationGetRelid(relation);
	}
	pfree(values);
	pfree(nulls);

	return tuple;
}
