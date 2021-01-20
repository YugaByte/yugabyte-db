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

#include "yb/docdb/docdb_pgapi.h"

#include "yb/util/status.h"
#include "yb/common/ql_expr.h"
#include "yb/yql/pggate/ybc_pg_typedefs.h"
#include "yb/yql/pggate/pg_value.h"

using yb::pggate::PgValueFromPB;
using yb::pggate::PgValueToPB;

namespace yb {
namespace docdb {

#define PG_RETURN_NOT_OK(status) \
  do { \
    if (status.err_code != 0) { \
      std::string msg; \
      if (status.err_msg != NULL) { \
        msg = std::string(status.err_msg); \
      } else { \
        msg = std::string("Unexpected error while evaluating expression"); \
      } \
      YbgResetMemoryContext(); \
      return STATUS(QLError, msg); \
    } \
  } while(0);


//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

class DocPgTypeAnalyzer {
 public:
  const YBCPgTypeEntity* GetTypeEntity(int32_t type_oid) {
    const auto iter = type_map_.find(type_oid);
    if (iter != type_map_.end()) {
      return iter->second;
    }
    return nullptr;
  }

 private:
  DocPgTypeAnalyzer() {
    // Setup type mapping.
    const YBCPgTypeEntity *type_table;
    int count;

    YbgGetTypeTable(&type_table, &count);
    for (int idx = 0; idx < count; idx++) {
        const YBCPgTypeEntity *type_entity = &type_table[idx];
        type_map_[type_entity->type_oid] = type_entity;
    }
  }

  // Mapping table of YugaByte and PostgreSQL datatypes.
  std::unordered_map<int, const YBCPgTypeEntity *> type_map_;

  friend class Singleton<DocPgTypeAnalyzer>;
  DISALLOW_COPY_AND_ASSIGN(DocPgTypeAnalyzer);
};

//-----------------------------------------------------------------------------
// Expressions/Values
//-----------------------------------------------------------------------------

const YBCPgTypeEntity* DocPgGetTypeEntity(YbgTypeDesc pg_type) {
    return Singleton<DocPgTypeAnalyzer>::get()->GetTypeEntity(pg_type.type_id);
}

Status DocPgEvalExpr(const std::string& expr_str,
                     std::vector<DocPgParamDesc> params,
                     const QLTableRow& table_row,
                     const Schema *schema,
                     QLValue* result) {
  PG_RETURN_NOT_OK(YbgPrepareMemoryContext());

  char *expr_cstring = const_cast<char *>(expr_str.c_str());

  // Create the context expression evaluation.
  // Since we currently only allow referencing the target col just set min/max attr to col_attno.
  // TODO Eventually this context should be created once per row and contain all (referenced)
  //      column values. Then the context can be reused for all expressions.
  YbgExprContext expr_ctx;
  int32_t min_attno = params[0].attno;
  int32_t max_attno = params[0].attno;

  for (int i = 1; i < params.size(); i++) {
    min_attno = std::min(min_attno, params[i].attno);
    max_attno = std::max(max_attno, params[i].attno);
  }

  PG_RETURN_NOT_OK(YbgExprContextCreate(min_attno, max_attno, &expr_ctx));

  // Set the column values (used to resolve scan variables in the expression).
  for (const ColumnId& col_id : schema->column_ids()) {
    auto column = schema->column_by_id(col_id);
    SCHECK(column.ok(), InternalError, "Invalid Schema");

    // Loop here is ok as params.size() will always be 1 for user tables,
    // and 2 for some internal queries (catalog version increment).
    // TODO Rethink this if we ever allow more params here.
    DCHECK_LT(params.size(), 3);
    for (int i = 0; i < params.size(); i++) {
      if (column->order() == params[i].attno) {
        const QLValuePB* val = table_row.GetColumn(col_id.rep());
        bool is_null = false;
        uint64_t datum = 0;
        YbgTypeDesc pg_arg_type = {params[i].typid, params[i].typmod};
        const YBCPgTypeEntity *arg_type = DocPgGetTypeEntity(pg_arg_type);
        YBCPgTypeAttrs arg_type_attrs = { pg_arg_type.type_mod };

        Status s = PgValueFromPB(arg_type, arg_type_attrs, *val, &datum, &is_null);
        if (!s.ok()) {
          PG_RETURN_NOT_OK(YbgResetMemoryContext());
          return s;
        }

        PG_RETURN_NOT_OK(YbgExprContextAddColValue(expr_ctx, column->order(), datum, is_null));
        break;
      }
    }
  }

  // Evaluate the expression and get the result.
  bool is_null = false;
  uint64_t datum;
  PG_RETURN_NOT_OK(YbgEvalExpr(expr_cstring, expr_ctx, &datum, &is_null));

  // Assuming first arg is the target column, so using it for the return type.
  // YSQL layer should guarantee this when producing the params.
  YbgTypeDesc pg_type = {params[0].typid, params[0].typmod};
  const YBCPgTypeEntity *ret_type = DocPgGetTypeEntity(pg_type);

  Status s = PgValueToPB(ret_type, datum, is_null, result);
  PG_RETURN_NOT_OK(YbgResetMemoryContext());
  return s;
}


}  // namespace docdb
}  // namespace yb
