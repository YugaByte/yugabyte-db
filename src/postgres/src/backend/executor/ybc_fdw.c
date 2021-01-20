/*--------------------------------------------------------------------------------------------------
 *
 * ybc_fdw.c
 *		  Foreign-data wrapper for YugabyteDB.
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
 * IDENTIFICATION
 *		  src/backend/executor/ybc_fdw.c
 *
 *--------------------------------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include "executor/ybc_fdw.h"

/*  TODO see which includes of this block are still needed. */
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "catalog/catalog.h"
#include "catalog/pg_foreign_table.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"

/*  YB includes. */
#include "commands/dbcommands.h"
#include "catalog/pg_operator.h"
#include "catalog/ybctype.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"
#include "access/ybcam.h"
#include "executor/ybcExpr.h"

#include "utils/resowner_private.h"

/* -------------------------------------------------------------------------- */
/*  Planner/Optimizer functions */

typedef struct YbFdwPlanState
{
	/* Bitmap of attribute (column) numbers that we need to fetch from YB. */
	Bitmapset *target_attrs;

} YbFdwPlanState;

/*
 * ybcGetForeignRelSize
 *		Obtain relation size estimates for a foreign table
 */
static void
ybcGetForeignRelSize(PlannerInfo *root,
					 RelOptInfo *baserel,
					 Oid foreigntableid)
{
	YbFdwPlanState		*ybc_plan = NULL;

	ybc_plan = (YbFdwPlanState *) palloc0(sizeof(YbFdwPlanState));

	/* Set the estimate for the total number of rows (tuples) in this table. */
	baserel->tuples = YBC_DEFAULT_NUM_ROWS;

	/*
	 * Initialize the estimate for the number of rows returned by this query.
	 * This does not yet take into account the restriction clauses, but it will
	 * be updated later by ybcIndexCostEstimate once it inspects the clauses.
	 */
	baserel->rows = baserel->tuples;

	baserel->fdw_private = ybc_plan;

	/*
	 * Test any indexes of rel for applicability also.
	 */
	check_index_predicates(root, baserel);
}

/*
 * ybcGetForeignPaths
 *		Create possible access paths for a scan on the foreign table, which is
 *      the full table scan plus available index paths (including the  primary key
 *      scan path if any).
 */
static void
ybcGetForeignPaths(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid)
{
	Cost startup_cost;
	Cost total_cost;

	/* Estimate costs */
	ybcCostEstimate(baserel, YBC_FULL_SCAN_SELECTIVITY,
	                false /* is_backwards scan */,
	                false /* is_uncovered_idx_scan */,
	                &startup_cost, &total_cost);

	/* Create a ForeignPath node and it as the scan path */
	add_path(baserel,
	         (Path *) create_foreignscan_path(root,
	                                          baserel,
	                                          NULL, /* default pathtarget */
	                                          baserel->rows,
	                                          startup_cost,
	                                          total_cost,
	                                          NIL,  /* no pathkeys */
	                                          NULL, /* no outer rel either */
	                                          NULL, /* no extra plan */
	                                          NULL  /* no options yet */ ));

	/* Add primary key and secondary index paths also */
	create_index_paths(root, baserel);
}

/*
 * ybcGetForeignPlan
 *		Create a ForeignScan plan node for scanning the foreign table
 */
static ForeignScan *
ybcGetForeignPlan(PlannerInfo *root,
				  RelOptInfo *baserel,
				  Oid foreigntableid,
				  ForeignPath *best_path,
				  List *tlist,
				  List *scan_clauses,
				  Plan *outer_plan)
{
	YbFdwPlanState *yb_plan_state = (YbFdwPlanState *) baserel->fdw_private;
	Index          scan_relid     = baserel->relid;
	ListCell       *lc;

	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Get the target columns that need to be retrieved from YugaByte */
	foreach(lc, baserel->reltarget->exprs)
	{
		Expr *expr = (Expr *) lfirst(lc);
		pull_varattnos_min_attr((Node *) expr,
		                        baserel->relid,
		                        &yb_plan_state->target_attrs,
		                        baserel->min_attr);
	}

	foreach(lc, scan_clauses)
	{
		Expr *expr = (Expr *) lfirst(lc);
		pull_varattnos_min_attr((Node *) expr,
		                        baserel->relid,
		                        &yb_plan_state->target_attrs,
		                        baserel->min_attr);
	}

	/* Set scan targets. */
	List *target_attrs = NULL;
	bool wholerow = false;
	for (AttrNumber attnum = baserel->min_attr; attnum <= baserel->max_attr; attnum++)
	{
		int bms_idx = attnum - baserel->min_attr + 1;
		if (wholerow || bms_is_member(bms_idx, yb_plan_state->target_attrs))
		{
			switch (attnum)
			{
				case InvalidAttrNumber:
					/*
					 * Postgres repurposes InvalidAttrNumber to represent the "wholerow"
					 * junk attribute.
					 */
					wholerow = true;
					break;
				case SelfItemPointerAttributeNumber:
				case MinTransactionIdAttributeNumber:
				case MinCommandIdAttributeNumber:
				case MaxTransactionIdAttributeNumber:
				case MaxCommandIdAttributeNumber:
					ereport(ERROR,
					        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(
							        "System column with id %d is not supported yet",
							        attnum)));
					break;
				case TableOidAttributeNumber:
					/* Nothing to do in YugaByte: Postgres will handle this. */
					break;
				case ObjectIdAttributeNumber:
				case YBTupleIdAttributeNumber:
				default: /* Regular column: attrNum > 0*/
				{
					TargetEntry *target = makeNode(TargetEntry);
					target->resno = attnum;
					target_attrs = lappend(target_attrs, target);
				}
			}
		}
	}

	/* Create the ForeignScan node */
	return make_foreignscan(tlist,  /* target list */
	                        scan_clauses,
	                        scan_relid,
	                        NIL,    /* expressions YB may evaluate (none) */
	                        target_attrs,  /* fdw_private data for YB */
	                        NIL,    /* custom YB target list (none for now) */
	                        NIL,    /* custom YB target list (none for now) */
	                        outer_plan);
}

/* ------------------------------------------------------------------------- */
/*  Scanning functions */

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */
typedef struct YbFdwExecState
{
	/* The handle for the internal YB Select statement. */
	YBCPgStatement	handle;
	ResourceOwner	stmt_owner;
	YBCPgExecParameters *exec_params; /* execution control parameters for YugaByte */
	bool is_exec_done; /* Each statement should be executed exactly one time */
} YbFdwExecState;

/*
 * ybcBeginForeignScan
 *		Initiate access to the Yugabyte by allocating a Select handle.
 */
static void
ybcBeginForeignScan(ForeignScanState *node, int eflags)
{
	EState      *estate      = node->ss.ps.state;
	Relation    relation     = node->ss.ss_currentRelation;

	YbFdwExecState *ybc_state = NULL;

	/* Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL. */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Allocate and initialize YB scan state. */
	ybc_state = (YbFdwExecState *) palloc0(sizeof(YbFdwExecState));

	node->fdw_state = (void *) ybc_state;
	HandleYBStatus(YBCPgNewSelect(YBCGetDatabaseOid(relation),
				   RelationGetRelid(relation),
				   NULL /* prepare_params */,
				   &ybc_state->handle));
	ResourceOwnerEnlargeYugaByteStmts(CurrentResourceOwner);
	ResourceOwnerRememberYugaByteStmt(CurrentResourceOwner, ybc_state->handle);
	ybc_state->stmt_owner = CurrentResourceOwner;
	ybc_state->exec_params = &estate->yb_exec_params;

	ybc_state->exec_params->rowmark = -1;
	ybc_state->exec_params->read_from_followers = YBReadFromFollowersEnabled();
	if (YBReadFromFollowersEnabled()) {
		ereport(DEBUG2, (errmsg("Doing read from followers")));
	}
	ListCell   *l;
	foreach(l, estate->es_rowMarks) {
		ExecRowMark *erm = (ExecRowMark *) lfirst(l);
		// Do not propagate non-row-locking row marks.
		if (erm->markType != ROW_MARK_REFERENCE &&
			erm->markType != ROW_MARK_COPY)
			ybc_state->exec_params->rowmark = erm->markType;
		break;
	}

	ybc_state->is_exec_done = false;

	/* Set the current syscatalog version (will check that we are up to date) */
	HandleYBStatusWithOwner(YBCPgSetCatalogCacheVersion(ybc_state->handle,
														yb_catalog_cache_version),
														ybc_state->handle,
														ybc_state->stmt_owner);
}

/*
 * Setup the scan targets (either columns or aggregates).
 */
static void
ybcSetupScanTargets(ForeignScanState *node)
{
	EState *estate = node->ss.ps.state;
	ForeignScan *foreignScan = (ForeignScan *) node->ss.ps.plan;
	Relation relation = node->ss.ss_currentRelation;
	YbFdwExecState *ybc_state = (YbFdwExecState *) node->fdw_state;
	TupleDesc tupdesc = RelationGetDescr(relation);
	ListCell *lc;

	/* Planning function above should ensure target list is set */
	List *target_attrs = foreignScan->fdw_private;

	MemoryContext oldcontext =
		MemoryContextSwitchTo(node->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	/* Set scan targets. */
	if (node->yb_fdw_aggs == NIL)
	{
		/* Set non-aggregate column targets. */
		bool has_targets = false;
		foreach(lc, target_attrs)
		{
			TargetEntry *target = (TargetEntry *) lfirst(lc);

			/* For regular (non-system) attribute check if they were deleted */
			Oid   attr_typid  = InvalidOid;
			int32 attr_typmod = 0;
			if (target->resno > 0)
			{
				Form_pg_attribute attr;
				attr = TupleDescAttr(tupdesc, target->resno - 1);
				/* Ignore dropped attributes */
				if (attr->attisdropped)
				{
					continue;
				}
				attr_typid  = attr->atttypid;
				attr_typmod = attr->atttypmod;
			}

			YBCPgTypeAttrs type_attrs = {attr_typmod};
			YBCPgExpr      expr       = YBCNewColumnRef(ybc_state->handle,
														target->resno,
														attr_typid,
														&type_attrs);
			HandleYBStatusWithOwner(YBCPgDmlAppendTarget(ybc_state->handle,
																									 expr),
															ybc_state->handle,
															ybc_state->stmt_owner);
			has_targets = true;
		}

		/*
		 * We can have no target columns at this point for e.g. a count(*). For now
		 * we request the first non-dropped column in that case.
		 * TODO look into handling this on YugaByte side.
		 */
		if (!has_targets)
		{
			for (int16_t i = 0; i < tupdesc->natts; i++)
			{
				/* Ignore dropped attributes */
				if (TupleDescAttr(tupdesc, i)->attisdropped)
				{
					continue;
				}

				YBCPgTypeAttrs type_attrs = { TupleDescAttr(tupdesc, i)->atttypmod };
				YBCPgExpr      expr       = YBCNewColumnRef(ybc_state->handle,
															i + 1,
															TupleDescAttr(tupdesc, i)->atttypid,
															&type_attrs);
				HandleYBStatusWithOwner(YBCPgDmlAppendTarget(ybc_state->handle,
																 expr),
											ybc_state->handle,
											ybc_state->stmt_owner);
				break;
			}
		}
	}
	else
	{
		/* Set aggregate scan targets. */
		foreach(lc, node->yb_fdw_aggs)
		{
			Aggref *aggref = lfirst_node(Aggref, lc);
			char *func_name = get_func_name(aggref->aggfnoid);
			ListCell *lc_arg;
			YBCPgExpr op_handle;
			const YBCPgTypeEntity *type_entity;

			/* Get type entity for the operator from the aggref. */
			type_entity = YBCPgFindTypeEntity(aggref->aggtranstype);

			/* Create operator. */
			HandleYBStatusWithOwner(YBCPgNewOperator(ybc_state->handle,
													 func_name,
													 type_entity,
													 &op_handle),
									ybc_state->handle,
									ybc_state->stmt_owner);

			/* Handle arguments. */
			if (aggref->aggstar) {
				/*
				 * Add dummy argument for COUNT(*) case, turning it into COUNT(0).
				 * We don't use a column reference as we want to count rows
				 * even if all column values are NULL.
				 */
				YBCPgExpr const_handle;
				YBCPgNewConstant(ybc_state->handle,
								 type_entity,
								 0 /* datum */,
								 false /* is_null */,
								 &const_handle);
				HandleYBStatusWithOwner(YBCPgOperatorAppendArg(op_handle, const_handle),
										ybc_state->handle,
										ybc_state->stmt_owner);
			} else {
				/* Add aggregate arguments to operator. */
				foreach(lc_arg, aggref->args)
				{
					TargetEntry *tle = lfirst_node(TargetEntry, lc_arg);
					if (IsA(tle->expr, Const))
					{
						Const* const_node = castNode(Const, tle->expr);
						/* Already checked by yb_agg_pushdown_supported */
						Assert(const_node->constisnull || const_node->constbyval);

						YBCPgExpr const_handle;
						YBCPgNewConstant(ybc_state->handle,
										 type_entity,
										 const_node->constvalue,
										 const_node->constisnull,
										 &const_handle);
						HandleYBStatusWithOwner(YBCPgOperatorAppendArg(op_handle, const_handle),
												ybc_state->handle,
												ybc_state->stmt_owner);
					}
					else if (IsA(tle->expr, Var))
					{
						/*
						 * Use original attribute number (varoattno) instead of projected one (varattno)
						 * as projection is disabled for tuples produced by pushed down operators.
						 */
						int attno = castNode(Var, tle->expr)->varoattno;
						Form_pg_attribute attr = TupleDescAttr(tupdesc, attno - 1);
						YBCPgTypeAttrs type_attrs = {attr->atttypmod};

						YBCPgExpr arg = YBCNewColumnRef(ybc_state->handle,
														attno,
														attr->atttypid,
														&type_attrs);
						HandleYBStatusWithOwner(YBCPgOperatorAppendArg(op_handle, arg),
												ybc_state->handle,
												ybc_state->stmt_owner);
					}
					else
					{
						/* Should never happen. */
						ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("unsupported aggregate function argument type")));
					}
				}
			}

			/* Add aggregate operator as scan target. */
			HandleYBStatusWithOwner(YBCPgDmlAppendTarget(ybc_state->handle,
														 op_handle),
														 ybc_state->handle,
														 ybc_state->stmt_owner);
		}

		/*
		 * Setup the scan slot based on new tuple descriptor for the given targets. This is a dummy
		 * tupledesc that only includes the number of attributes. Switch to per-query memory from
		 * per-tuple memory so the slot persists across iterations.
		 */
		TupleDesc target_tupdesc = CreateTemplateTupleDesc(list_length(node->yb_fdw_aggs),
														   false /* hasoid */);
		ExecInitScanTupleSlot(estate, &node->ss, target_tupdesc);
	}
	MemoryContextSwitchTo(oldcontext);
}

/*
 * ybcIterateForeignScan
 *		Read next record from the data file and store it into the
 *		ScanTupleSlot as a virtual tuple
 */
static TupleTableSlot *
ybcIterateForeignScan(ForeignScanState *node)
{
	TupleTableSlot *slot;
	YbFdwExecState *ybc_state = (YbFdwExecState *) node->fdw_state;
	bool           has_data   = false;

	/* Execute the select statement one time.
	 * TODO(neil) Check whether YugaByte PgGate should combine Exec() and Fetch() into one function.
	 * - The first fetch from YugaByte PgGate requires a number of operations including allocating
	 *   operators and protobufs. These operations are done by YBCPgExecSelect() function.
	 * - The subsequent fetches don't need to setup the query with these operations again.
	 */
	if (!ybc_state->is_exec_done) {
		ybcSetupScanTargets(node);
		HandleYBStatusWithOwner(YBCPgExecSelect(ybc_state->handle, ybc_state->exec_params),
								ybc_state->handle,
								ybc_state->stmt_owner);
		ybc_state->is_exec_done = true;
	}

	/* Clear tuple slot before starting */
	slot = node->ss.ss_ScanTupleSlot;
	ExecClearTuple(slot);

	TupleDesc       tupdesc = slot->tts_tupleDescriptor;
	Datum           *values = slot->tts_values;
	bool            *isnull = slot->tts_isnull;
	YBCPgSysColumns syscols;

	/* Fetch one row. */
	HandleYBStatusWithOwner(YBCPgDmlFetch(ybc_state->handle,
	                                      tupdesc->natts,
	                                      (uint64_t *) values,
	                                      isnull,
	                                      &syscols,
	                                      &has_data),
	                        ybc_state->handle,
	                        ybc_state->stmt_owner);

	/* If we have result(s) update the tuple slot. */
	if (has_data)
	{
		if (node->yb_fdw_aggs == NIL)
		{
			HeapTuple tuple = heap_form_tuple(tupdesc, values, isnull);
			if (syscols.oid != InvalidOid)
			{
				HeapTupleSetOid(tuple, syscols.oid);
			}

			slot = ExecStoreHeapTuple(tuple, slot, false);

			/* Setup special columns in the slot */
			slot->tts_ybctid = PointerGetDatum(syscols.ybctid);
		}
		else
		{
			/*
			 * Aggregate results stored in virtual slot (no tuple). Set the
			 * number of valid values and mark as non-empty.
			 */
			slot->tts_nvalid = tupdesc->natts;
			slot->tts_isempty = false;
		}
	}

	return slot;
}

static void
ybcFreeStatementObject(YbFdwExecState* yb_fdw_exec_state)
{
	/* If yb_fdw_exec_state is NULL, we are in EXPLAIN; nothing to do */
	if (yb_fdw_exec_state != NULL && yb_fdw_exec_state->handle != NULL)
	{
		ResourceOwnerForgetYugaByteStmt(yb_fdw_exec_state->stmt_owner,
										yb_fdw_exec_state->handle);
		YBCPgDeleteStatement(yb_fdw_exec_state->handle);
		yb_fdw_exec_state->handle = NULL;
		yb_fdw_exec_state->stmt_owner = NULL;
		yb_fdw_exec_state->exec_params = NULL;
		yb_fdw_exec_state->is_exec_done = false;
	}
}

/*
 * fileReScanForeignScan
 *		Rescan table, possibly with new parameters
 */
static void
ybcReScanForeignScan(ForeignScanState *node)
{
	YbFdwExecState *ybc_state = (YbFdwExecState *) node->fdw_state;

	/* Clear (delete) the previous select */
	ybcFreeStatementObject(ybc_state);

	/* Re-allocate and execute the select. */
	ybcBeginForeignScan(node, 0 /* eflags */);
}

/*
 * ybcEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
ybcEndForeignScan(ForeignScanState *node)
{
	YbFdwExecState *ybc_state = (YbFdwExecState *) node->fdw_state;
	ybcFreeStatementObject(ybc_state);
}

/* ------------------------------------------------------------------------- */
/*  FDW declaration */

/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to YugaByte callback routines.
 */
Datum
ybc_fdw_handler()
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	fdwroutine->GetForeignRelSize  = ybcGetForeignRelSize;
	fdwroutine->GetForeignPaths    = ybcGetForeignPaths;
	fdwroutine->GetForeignPlan     = ybcGetForeignPlan;
	fdwroutine->BeginForeignScan   = ybcBeginForeignScan;
	fdwroutine->IterateForeignScan = ybcIterateForeignScan;
	fdwroutine->ReScanForeignScan  = ybcReScanForeignScan;
	fdwroutine->EndForeignScan     = ybcEndForeignScan;

	/* TODO: These are optional but we should support them eventually. */
	/* fdwroutine->ExplainForeignScan = ybcExplainForeignScan; */
	/* fdwroutine->AnalyzeForeignTable = ybcAnalyzeForeignTable; */
	/* fdwroutine->IsForeignScanParallelSafe = ybcIsForeignScanParallelSafe; */

	PG_RETURN_POINTER(fdwroutine);
}
