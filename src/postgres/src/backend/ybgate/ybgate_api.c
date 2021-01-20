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

#include "postgres.h"

#include <setjmp.h>

#include "ybgate/ybgate_api.h"

#include "nodes/primnodes.h"
#include "nodes/makefuncs.h"
#include "utils/numeric.h"
#include "utils/memutils.h"
#include "catalog/ybctype.h"
#include "common/int.h"
#include "nodes/execnodes.h"
#include "executor/executor.h"
#include "executor/execExpr.h"

//-----------------------------------------------------------------------------
// Memory Context
//-----------------------------------------------------------------------------


YbgStatus YbgPrepareMemoryContext()
{
	PG_SETUP_ERROR_REPORTING();

	PrepareThreadLocalCurrentMemoryContext();

	return PG_STATUS_OK;
}

YbgStatus YbgResetMemoryContext()
{
	PG_SETUP_ERROR_REPORTING();

	ResetThreadLocalCurrentMemoryContext();

	return PG_STATUS_OK;
}

YbgStatus YbgDeleteMemoryContext()
{
	PG_SETUP_ERROR_REPORTING();

	DeleteThreadLocalCurrentMemoryContext();

	return PG_STATUS_OK;
}

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

YbgStatus YbgGetTypeTable(const YBCPgTypeEntity **type_table, int *count)
{
	PG_SETUP_ERROR_REPORTING();

	YBCGetTypeTable(type_table, count);

	return PG_STATUS_OK;
}

//-----------------------------------------------------------------------------
// Expression Evaluation
//-----------------------------------------------------------------------------

/*
 * Expression context for evaluating a YSQL expression from DocDB.
 * Currently includes the table row values to resolve scan variables.
 * TODO Eventually this should probably also have schema/type information.
 */
struct YbgExprContextData
{
	// Values from table row.
	int32_t min_attno;
	int32_t max_attno;
	Datum *attr_vals;
	Bitmapset *attr_nulls;
};

/*
 * Evaluate an expression against an expression context.
 * Currently assumes the expression has been checked by the planner to only
 * allow immutable functions and the node types handled below.
 * TODO: this should use the general YSQL/PG expression evaluation framework, but
 * that requires syscaches and other dependencies to be fully initialized.
 */
static Datum evalExpr(YbgExprContext ctx, Expr* expr, bool *is_null)
{
	switch (expr->type)
	{
		case T_FuncExpr:
		case T_OpExpr:
		{
			Oid          funcid = InvalidOid;
			List         *args = NULL;
			ListCell     *lc = NULL;

			/* Get the (underlying) function info. */
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

			FmgrInfo *flinfo = palloc0(sizeof(FmgrInfo));
			FunctionCallInfo fcinfo = palloc0(SizeForFunctionCallInfo(args->length));

			fmgr_info(funcid, flinfo);
			InitFunctionCallInfoData(*fcinfo,
			                         flinfo,
			                         args->length,
			                         InvalidOid,
			                         NULL,
			                         NULL);
			int i = 0;
			foreach(lc, args)
			{
				Expr *arg = (Expr *) lfirst(lc);
				fcinfo->args[i].value = evalExpr(ctx, arg, &fcinfo->args[i].isnull);
				/*
				 * Strict functions are guaranteed to return NULL if any of
				 * their arguments are NULL.
				 */
				if (flinfo->fn_strict && fcinfo->args[i].isnull) {
					*is_null = true;
					return (Datum) 0;
				}
				i++;
			}
			Datum result = FunctionCallInvoke(fcinfo);
			*is_null = fcinfo->isnull;
			return result;
		}
		case T_RelabelType:
		{
			RelabelType *rt = castNode(RelabelType, expr);
			return evalExpr(ctx, rt->arg, is_null);
		}
		case T_Const:
		{
			Const* const_expr = castNode(Const, expr);
			*is_null = const_expr->constisnull;
			return const_expr->constvalue;
		}
		case T_Var:
		{
			Var* var_expr = castNode(Var, expr);
			int32_t att_idx = var_expr->varattno - ctx->min_attno;
			*is_null = bms_is_member(att_idx, ctx->attr_nulls);
			return ctx->attr_vals[att_idx];
		}
		default:
			/* Planner should ensure we never get here. */
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR), errmsg(
				"Unsupported YSQL expression received by DocDB")));
			break;
	}
	*is_null = true;
	return (Datum) 0;
}

YbgStatus YbgExprContextCreate(int32_t min_attno, int32_t max_attno, YbgExprContext *expr_ctx)
{
	PG_SETUP_ERROR_REPORTING();

	YbgExprContext ctx = (YbgExprContext) palloc0(sizeof(struct YbgExprContextData));
	ctx->min_attno = min_attno;
	ctx->max_attno = max_attno;
	int32_t num_attrs = max_attno - min_attno + 1;
	ctx->attr_vals = (Datum *) palloc0(sizeof(Datum) * num_attrs);
	ctx->attr_nulls = NULL;

	*expr_ctx = ctx;
	return PG_STATUS_OK;
}

YbgStatus YbgExprContextAddColValue(YbgExprContext expr_ctx,
                                    int32_t attno,
                                    uint64_t datum,
                                    bool is_null)
{
	PG_SETUP_ERROR_REPORTING();

	if (is_null)
	{
		expr_ctx->attr_nulls = bms_add_member(expr_ctx->attr_nulls, attno - expr_ctx->min_attno);
	}
	else
	{
		expr_ctx->attr_vals[attno - expr_ctx->min_attno] = (Datum) datum;
	}

	return PG_STATUS_OK;
}

YbgStatus YbgEvalExpr(char* expr_cstring, YbgExprContext expr_ctx, uint64_t *datum, bool *is_null)
{
	PG_SETUP_ERROR_REPORTING();
	Expr *expr = (Expr *) stringToNode(expr_cstring);
	*datum = (uint64_t) evalExpr(expr_ctx, expr, is_null);
	return PG_STATUS_OK;
}
