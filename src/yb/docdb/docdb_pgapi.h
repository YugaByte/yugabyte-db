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

// This file contains the C++ API for the PG/YSQL backend library to be used by DocDB.
// Analogous to YCQL this should be used for write expressions (e.g. SET clause), filtering
// (e.g. WHERE clause), expression projections (e.g. SELECT targets) etc.
// Currently it just supports evaluating an YSQL expressions into a QLValues and is only used
// for the UPDATE .. SET clause.
//
// The implementation for these should typically call (and/or extend) either the PG/YSQL C API
// from pgapi.h or  directly the pggate C++ API.

#ifndef YB_DOCDB_DOCDB_PGAPI_H_
#define YB_DOCDB_DOCDB_PGAPI_H_

#include "ybgate/ybgate_api.h"

#include "yb/common/ql_value.h"
#include "yb/common/schema.h"

namespace yb {
namespace docdb {

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

struct DocPgParamDesc {
    int32_t attno;
    int32_t typid;
    int32_t typmod;

    DocPgParamDesc(int32_t attno, int32_t typid, int32_t typmod)
        : attno(attno), typid(typid), typmod(typmod)
    {}
};

const YBCPgTypeEntity* DocPgGetTypeEntity(YbgTypeDesc pg_type);

//-----------------------------------------------------------------------------
// Expressions/Values
//-----------------------------------------------------------------------------

Status DocPgEvalExpr(const std::string& expr_str,
                     std::vector<DocPgParamDesc> params,
                     const QLTableRow& table_row,
                     const Schema *schema,
                     QLValue* result);

} // namespace docdb
} // namespace yb

#endif // YB_DOCDB_DOCDB_PGAPI_H_
