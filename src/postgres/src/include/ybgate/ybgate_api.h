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


// This file includes C/C++ wrappers around some PG utilities such that it can be be included
// into YB C++ codebases.
// Specifically, it is currently used by DocDB to evaluate YSQL expresison (pushed down from
// the query layer).

#ifndef PG_YBGATE_YBGATE_API_H
#define PG_YBGATE_YBGATE_API_H

#include <stddef.h>
#include <stdint.h>
#include "yb/yql/pggate/ybc_pg_typedefs.h"

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------
// Error Reporting
//-----------------------------------------------------------------------------
// !!! IMPORTANT: All public functions in the ybgate directory MUST return a
// YbgStatus and call PG_SETUP_ERROR_REPORTING first, before any execution.
//
// This will ensure that any elog or ereport called internally by a YSQL/PG
// function will be caught/handled (and not crash the tserver process).
//-----------------------------------------------------------------------------

struct YbgStatus {
	int32_t err_code; /* error code (from elog.h), 0 means no error */
	const char *err_msg; /* NULL if no error, else may still be NULL */
};

#ifndef __cplusplus
typedef struct YbgStatus YbgStatus;
#endif

#define PG_STATUS_OK (YbgStatus) {0, NULL};

#define PG_STATUS(elevel, msg) (YbgStatus) {elevel, msg};

/*
 * We cannot use the standard Postgres PG_TRY() and friends here because error
 * reporting framework is not fully set up. But we use the same setjmp/longjmp
 * mechanism to report reasonable errors to DocDB.
 * TODO this should be better integrated with both PG/YSQL error reporting and
 * DocDB. Current limitations include:
 *   - PG error message handling -- some error message components (e.g. the
 *     DETAIL line) might not be set or fully processed.
 *   - currently DocDB will turn PG error code/message into a DocDB error
 *     code/message (e.g. add a "Query error: " prefix to the message).
 */
#define PG_SETUP_ERROR_REPORTING() \
	do { \
		jmp_buf buffer; \
		YBCPgSetThreadLocalJumpBuffer(&buffer); \
		int r = setjmp(buffer); \
		if (r != 0) { \
			return PG_STATUS(r, YBCPgGetThreadLocalErrMsg()); \
		} \
	} while(0) \

//-----------------------------------------------------------------------------
// Memory Context
//-----------------------------------------------------------------------------

YbgStatus YbgPrepareMemoryContext();

YbgStatus YbgResetMemoryContext();

YbgStatus YbgDeleteMemoryContext();

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

struct YbgTypeDesc {
	int32_t type_id; /* type identifier */
	int32_t type_mod; /* type modifier */
};

#ifndef __cplusplus
typedef struct YbgTypeDesc YbgTypeDesc;
#endif

/*
 * Get the type entity table for the primtive YSQL/PG types.
 * Used for for converting between DocDB values and YSQL datums.
 */
YbgStatus YbgGetTypeTable(const YBCPgTypeEntity **type_table, int *count);

//-----------------------------------------------------------------------------
// Expression Evaluation
//-----------------------------------------------------------------------------

#ifdef __cplusplus
typedef void* YbgExprContext;
#else
typedef struct YbgExprContextData* YbgExprContext;
#endif

/*
 * Create an expression context to evaluate expressions against.
 */
YbgStatus YbgExprContextCreate(int32_t min_attno, int32_t max_attno, YbgExprContext *expr_ctx);

/*
 * Add a column value from the table row.
 * Used by expression evaluation to resolve scan variables.
 */
YbgStatus YbgExprContextAddColValue(YbgExprContext expr_ctx, int32_t attno, uint64_t datum, bool is_null);

/*
 * Evaluate an expression, using the expression context to resolve scan variables.
 * Will filling in datum and is_null with the result.
 */
YbgStatus YbgEvalExpr(char* expr_cstring, YbgExprContext expr_ctx, uint64_t *datum, bool *is_null);

#ifdef __cplusplus
}
#endif

#endif  // PG_YBGATE_YBGATE_API_H
