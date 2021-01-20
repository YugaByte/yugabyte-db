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

#ifndef YB_YQL_PGGATE_PG_SELECT_INDEX_H_
#define YB_YQL_PGGATE_PG_SELECT_INDEX_H_

#include "yb/yql/pggate/pg_dml_read.h"

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------
// SELECT FROM Secondary Index Table
//--------------------------------------------------------------------------------------------------

class PgSelectIndex : public PgDmlRead {
 public:
  PgSelectIndex(PgSession::ScopedRefPtr pg_session,
                const PgObjectId& table_id,
                const PgObjectId& index_id,
                const PgPrepareParameters *prepare_params);
  virtual ~PgSelectIndex();

  // Prepare query for secondary index. This function is called when Postgres layer is accessing
  // the IndexTable directy (IndexOnlyScan).
  CHECKED_STATUS Prepare();

  // Prepare NESTED query for secondary index. This function is called when Postgres layer is
  // accessing the IndexTable via an outer select (Sequential or primary scans)
  CHECKED_STATUS PrepareSubquery(PgsqlReadRequestPB *read_req);

  CHECKED_STATUS PrepareQuery(PgsqlReadRequestPB *read_req);

  // The output parameter "ybctids" are pointer to the data buffer in "ybctid_batch_".
  Result<bool> FetchYbctidBatch(const vector<Slice> **ybctids);

  // Get next batch of ybctids from either PgGate::cache or server.
  Result<bool> GetNextYbctidBatch();

  void set_is_executed(bool value) {
    is_executed_ = value;
  }

  bool is_executed() {
    return is_executed_;
  }

 private:
  // Collect ybctids from IndexTable.
  CHECKED_STATUS FetchYbctids();

  // This secondary query should be executed just one time.
  bool is_executed_ = false;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_SELECT_INDEX_H_
