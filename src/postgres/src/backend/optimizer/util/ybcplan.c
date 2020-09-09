/*--------------------------------------------------------------------------------------------------
 *
 * ybcplan.c
 *	  Utilities for YugaByte scan.
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
 * src/backend/executor/ybcplan.c
 *
 *--------------------------------------------------------------------------------------------------
 */


#include "postgres.h"

#include "optimizer/ybcplan.h"
#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/print.h"
#include "nodes/relation.h"
#include "utils/datum.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

/*
 * Theoretically, any expression that evaluates to a constant before YB
 * execution.
 * Currently restrict this to a subset of expressions.
 * Note: As enhance pushdown support in DocDB (e.g. expr evaluation) this
 * can be expanded.
 */
bool YBCIsSupportedSingleRowModifyWhereExpr(Expr *expr)
{
	switch (nodeTag(expr))
	{
		case T_Const:
			return true;
		case T_Param:
		{
			/* Bind variables. */
			Param *param = castNode(Param, expr);
			return param->paramkind == PARAM_EXTERN;
		}
		case T_RelabelType:
		{
			/*
			 * RelabelType is a "dummy" type coercion between two binary-
			 * compatible datatypes so we just recurse into its argument.
			 */
			RelabelType *rt = castNode(RelabelType, expr);
			return YBCIsSupportedSingleRowModifyWhereExpr(rt->arg);
		}
		case T_FuncExpr:
		case T_OpExpr:
		{
			List         *args = NULL;
			ListCell     *lc = NULL;
			Oid          funcid = InvalidOid;
			HeapTuple    tuple = NULL;

			/* Get the function info. */
			if (IsA(expr, FuncExpr))
			{
				FuncExpr *func_expr = castNode(FuncExpr, expr);
				args = func_expr->args;
				funcid = func_expr->funcid;
			}
			else if (IsA(expr, OpExpr))
			{
				OpExpr *op_expr = castNode(OpExpr, expr);
				args = op_expr->args;
				funcid = op_expr->opfuncid;
			}

			/*
			 * Only allow immutable functions as they cannot modify the
			 * database or do lookups.
			 */
			tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for function %u", funcid);
			char provolatile = ((Form_pg_proc) GETSTRUCT(tuple))->provolatile;
			ReleaseSysCache(tuple);
			if (provolatile != PROVOLATILE_IMMUTABLE)
			{
				return false;
			}

			/* Checking all arguments are valid (stable). */
			foreach (lc, args) {
				Expr* expr = (Expr *) lfirst(lc);
				if (!YBCIsSupportedSingleRowModifyWhereExpr(expr)) {
					return false;
				}
			}
			return true;
		}
		default:
			break;
	}

	return false;
}

static void YBCExprInstantiateParamsInternal(Expr* expr,
                                             ParamListInfo paramLI,
                                             Expr** parent_var)
{
	switch (nodeTag(expr))
	{
		case T_Const:
			return;
		case T_Var:
			return;
		case T_Param:
		{
			/* Bind variables. */
			Param *param = castNode(Param, expr);
			ParamExternData *prm = NULL;
			ParamExternData prmdata;
			if (paramLI->paramFetch != NULL)
				prm = paramLI->paramFetch(paramLI, param->paramid,
										true, &prmdata);
			else
				prm = &paramLI->params[param->paramid - 1];

			if (!OidIsValid(prm->ptype) ||
				prm->ptype != param->paramtype ||
				!(prm->pflags & PARAM_FLAG_CONST))
			{
				/* Planner should ensure this does not happen */
				elog(ERROR, "Invalid parameter: %s", nodeToString(param));
			}
			int16		typLen = 0;
			bool		typByVal = false;
			Datum		pval = 0;

			get_typlenbyval(param->paramtype,
							&typLen, &typByVal);
			if (prm->isnull || typByVal)
				pval = prm->value;
			else
				pval = datumCopy(prm->value, typByVal, typLen);

			Expr *const_expr = (Expr *) makeConst(param->paramtype,
										param->paramtypmod,
										param->paramcollid,
										(int) typLen,
										pval,
										prm->isnull,
										typByVal);

			*parent_var = const_expr;
			return;
		}
		case T_RelabelType:
		{
			/*
			 * RelabelType is a "dummy" type coercion between two binary-
			 * compatible datatypes so we just recurse into its argument.
			 */
			RelabelType *rt = castNode(RelabelType, expr);
			YBCExprInstantiateParamsInternal(rt->arg, paramLI, &rt->arg);
			return;
		}
		case T_FuncExpr:
		{
			FuncExpr *func_expr = castNode(FuncExpr, expr);
			ListCell *lc = NULL;
			foreach(lc, func_expr->args)
			{
				Expr *arg = (Expr *) lfirst(lc);
				YBCExprInstantiateParamsInternal(arg,
				                                 paramLI,
				                                 (Expr **)&lc->data.ptr_value);
			}
			return;
		}
		case T_OpExpr:
		{
			OpExpr   *op_expr = castNode(OpExpr, expr);
			ListCell *lc = NULL;
			foreach(lc, op_expr->args)
			{
				Expr *arg = (Expr *) lfirst(lc);
				YBCExprInstantiateParamsInternal(arg,
				                                 paramLI,
				                                 (Expr **)&lc->data.ptr_value);
			}
			return;
		}
		default:
			break;
	}

	/* Planner should ensure this does not happen */
	elog(ERROR, "Invalid expression: %s", nodeToString(expr));
}


void YBCExprInstantiateParams(Expr* expr, ParamListInfo paramLI)
{
	/* Fast-path if there are no params. */
	if (paramLI == NULL)
		return;

	YBCExprInstantiateParamsInternal(expr, paramLI, NULL);
}

/*
 * Check if the function/procedure can be executed by DocDB (i.e. if we can
 * pushdown its execution).
 * The main current limitation is that DocDB's execution layer does not have
 * syscatalog access (cache lookup) so only specific builtins are supported.q
 */
static bool YBCIsSupportedDocDBFunctionId(Oid funcid, Form_pg_proc pg_proc) {
	if (!is_builtin_func(funcid))
	{
		return false;
	}

	/*
	 * Polymorhipc pseduo-types (e.g. anyarray) may require additional
	 * processing (requiring syscatalog access) to fully resolve to a concrete
	 * type. Therefore they are not supported by DocDB.
	 */
	if (IsPolymorphicType(pg_proc->prorettype))
	{
		return false;
	}

	for (int i = 0; i < pg_proc->pronargs; i++)
	{
		if (IsPolymorphicType(pg_proc->proargtypes.values[i]))
		{
			return false;
		}
	}

	return true;
}

static bool YBCAnalyzeExpression(Expr *expr, AttrNumber target_attnum, bool *has_vars, bool *has_docdb_unsupported_funcs) {
	switch (nodeTag(expr))
	{
		case T_Const:
			return true;
		case T_Var:
		{
			/* References to table attrs (to be read) */
			Var *var = castNode(Var, expr);
			*has_vars = true;
			return var->varattno == target_attnum;
		}
		case T_Param:
		{
			/* Bind variables. */
			Param *param = castNode(Param, expr);
			return param->paramkind == PARAM_EXTERN;
		}
		case T_RelabelType:
		{
			/*
			 * RelabelType is a "dummy" type coercion between two binary-
			 * compatible datatypes so we just recurse into its argument.
			 */
			RelabelType *rt = castNode(RelabelType, expr);
			return YBCAnalyzeExpression(rt->arg, target_attnum, has_vars, has_docdb_unsupported_funcs);
		}
		case T_FuncExpr:
		case T_OpExpr:
		{
			List         *args = NULL;
			ListCell     *lc = NULL;
			Oid          funcid = InvalidOid;
			HeapTuple    tuple = NULL;

			/* Get the function info. */
			if (IsA(expr, FuncExpr))
			{
				FuncExpr *func_expr = castNode(FuncExpr, expr);
				args = func_expr->args;
				funcid = func_expr->funcid;
			}
			else if (IsA(expr, OpExpr))
			{
				OpExpr *op_expr = castNode(OpExpr, expr);
				args = op_expr->args;
				funcid = op_expr->opfuncid;
			}

			/*
			 * Only allow immutable functions as they cannot modify the
			 * database or do lookups.
			 */
			tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for function %u", funcid);
			Form_pg_proc pg_proc = ((Form_pg_proc) GETSTRUCT(tuple));

			if (pg_proc->provolatile != PROVOLATILE_IMMUTABLE)
			{
				ReleaseSysCache(tuple);
				return false;
			}

			if (!YBCIsSupportedDocDBFunctionId(funcid, pg_proc)) {
				*has_docdb_unsupported_funcs = true;
			}
			ReleaseSysCache(tuple);

			/* Checking all arguments are valid (stable). */
			foreach (lc, args) {
				Expr* expr = (Expr *) lfirst(lc);
				if (!YBCAnalyzeExpression(expr, target_attnum, has_vars, has_docdb_unsupported_funcs)) {
				    return false;
				}
			}
			return true;
		}
		default:
			break;
	}

	return false;
}

/*
 * Can expression be evaluated in DocDB.
 * Eventually any immutable expression whose only variables are column references.
 * Currently, limit to the case where the only referenced column is the target column.
 */
bool YBCIsSupportedSingleRowModifyAssignExpr(Expr *expr, AttrNumber target_attnum, bool *needs_pushdown) {
	bool has_vars = false;
	bool has_docdb_unsupported_funcs = false;
	bool is_basic_expr = YBCAnalyzeExpression(expr, target_attnum, &has_vars, &has_docdb_unsupported_funcs);

	/* default, will set to true below if needed. */
	*needs_pushdown = false;

	/* Immediately bail for complex expressions */
	if (!is_basic_expr)
		return false;

	/* If there are no variables, we can just evaluate it to a const. */
	if (!has_vars)
	{
		return true;
	}

	/* We can push down expression evaluation to DocDB. */
	if (has_vars && !has_docdb_unsupported_funcs)
	{
		*needs_pushdown = true;
		return true;
	}

	/* Variables plus DocDB-unsupported funcs -> query layer must evaluate. */
	return false;
}

/*
 * Returns true if the following are all true:
 *  - is insert, update, or delete command.
 *  - only one target table.
 *  - there are no ON CONFLICT or WITH clauses.
 *  - source data is a VALUES clause with one value set.
 *  - all values are either constants or bind markers.
 *
 *  Additionally, during execution we will also check:
 *  - not in transaction block.
 *  - is a single-plan execution.
 *  - target table has no triggers.
 *  - target table has no indexes.
 *  And if all are true we will execute this op as a single-row transaction
 *  rather than a distributed transaction.
 */
static bool ModifyTableIsSingleRowWrite(ModifyTable *modifyTable)
{

	/* Support INSERT, UPDATE, and DELETE. */
	if (modifyTable->operation != CMD_INSERT &&
		modifyTable->operation != CMD_UPDATE &&
		modifyTable->operation != CMD_DELETE)
		return false;

	/* Multi-relation implies multi-shard. */
	if (list_length(modifyTable->resultRelations) != 1)
		return false;

	/* ON CONFLICT clause is not supported here yet. */
	if (modifyTable->onConflictAction != ONCONFLICT_NONE)
		return false;

	/* WITH clause is not supported here yet. */
	if (modifyTable->plan.initPlan != NIL)
		return false;

	/* Check the data source, only allow a values clause right now */
	if (list_length(modifyTable->plans) != 1)
		return false;

	switch nodeTag(linitial(modifyTable->plans))
	{
		case T_Result:
		{
			/* Simple values clause: one valueset (single row) */
			Result *values = (Result *)linitial(modifyTable->plans);
			ListCell *lc;
			foreach(lc, values->plan.targetlist)
			{
				TargetEntry *target = (TargetEntry *) lfirst(lc);
				bool needs_pushdown = false;
				if (!YBCIsSupportedSingleRowModifyAssignExpr(target->expr,
				                                             target->resno,
				                                             &needs_pushdown))
				{
					return false;
				}
			}
			break;
		}
		case T_ValuesScan:
		{
			/*
			 * Simple values clause: multiple valueset (multi-row).
			 * TODO: Eventually we could inspect hash key values to check
			 *       if single shard and optimize that.
			 *       ---
			 *       In this case we'd need some other way to explicitly filter out
			 *       updates involving primary key - right now we simply rely on
			 *       planner not setting the node to Result.
			 */
			return false;

		}
		default:
			/* Do not support any other data sources. */
			return false;
	}

	/* If all our checks passed return true */
	return true;
}

bool YBCIsSingleRowModify(PlannedStmt *pstmt)
{
	if (pstmt->planTree && IsA(pstmt->planTree, ModifyTable))
	{
		ModifyTable *node = castNode(ModifyTable, pstmt->planTree);
		return ModifyTableIsSingleRowWrite(node);
	}

	return false;
}

/*
 * Returns true if the following are all true:
 *  - is update or delete command.
 *  - source data is a Result node (meaning we are skipping scan and thus
 *    are single row).
 */
bool YBCIsSingleRowUpdateOrDelete(ModifyTable *modifyTable)
{
	/* Support UPDATE and DELETE. */
	if (modifyTable->operation != CMD_UPDATE &&
		modifyTable->operation != CMD_DELETE)
		return false;

	/* Should only have one data source. */
	if (list_length(modifyTable->plans) != 1)
		return false;

	/* Verify the single data source is a Result node. */
	if (!IsA(linitial(modifyTable->plans), Result))
		return false;

	return true;
}

/*
 * Returns true if provided Bitmapset of attribute numbers
 * matches the primary key attribute numbers of the relation.
 * Expects YBGetFirstLowInvalidAttributeNumber to be subtracted from attribute numbers.
 */
bool YBCAllPrimaryKeysProvided(Relation rel, Bitmapset *attrs)
{
	if (bms_is_empty(attrs))
	{
		/*
		 * If we don't explicitly check for empty attributes it is possible
		 * for this function to improperly return true. This is because in the
		 * case where a table does not have any primary key attributes we will
		 * use a hidden RowId column which is not exposed to the PG side, so
		 * both the YB primary key attributes and the input attributes would
		 * appear empty and would be equal, even though this is incorrect as
		 * the YB table has the hidden RowId primary key column.
		 */
		return false;
	}

	Bitmapset *primary_key_attrs = YBGetTablePrimaryKeyBms(rel);

	/* Verify the sets are the same. */
	return bms_equal(attrs, primary_key_attrs);
}
