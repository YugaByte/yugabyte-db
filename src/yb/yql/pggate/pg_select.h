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

#ifndef YB_YQL_PGGATE_PG_SELECT_H_
#define YB_YQL_PGGATE_PG_SELECT_H_

#include <list>

#include "yb/gutil/ref_counted.h"

#include "yb/yql/pggate/pg_select_index.h"

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------
// SELECT
//--------------------------------------------------------------------------------------------------

class PgSelect : public PgDmlRead {
 public:
  PgSelect(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id,
           const PgObjectId& index_id, const PgPrepareParameters *prepare_params);
  virtual ~PgSelect();

  // Prepare query before execution.
  virtual CHECKED_STATUS Prepare();

  // Prepare secondary index if that index is used by this query.
  CHECKED_STATUS PrepareSecondaryIndex();
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_SELECT_H_
