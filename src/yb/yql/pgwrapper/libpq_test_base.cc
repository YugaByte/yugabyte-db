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


#include <string>

#include "yb/util/monotime.h"
#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/libpq_test_base.h"

#include "yb/common/common.pb.h"
#include "yb/common/pgsql_error.h"

using namespace std::literals;

DECLARE_int64(external_mini_cluster_max_log_bytes);

namespace yb {
namespace pgwrapper {

void LibPqTestBase::SetUp() {
  // YSQL has very verbose logging in case of conflicts
  // TODO: reduce the verbosity of that logging.
  FLAGS_external_mini_cluster_max_log_bytes = 512_MB;
  PgWrapperTestBase::SetUp();
}

Result<PGConn> LibPqTestBase::Connect() {
  return PGConn::Connect(HostPort(pg_ts->bind_host(), pg_ts->pgsql_rpc_port()));
}

Result<PGConn> LibPqTestBase::ConnectToDB(const string& db_name) {
  return PGConn::Connect(HostPort(pg_ts->bind_host(), pg_ts->pgsql_rpc_port()), db_name);
}

Result<PGConn> LibPqTestBase::ConnectToDBAsUser(const string& db_name, const string& user) {
  return PGConn::Connect(HostPort(pg_ts->bind_host(), pg_ts->pgsql_rpc_port()), db_name, user);
}

Result<PGConn> LibPqTestBase::ConnectUsingString(const string& conn_str, CoarseTimePoint deadline) {
  return PGConn::Connect(conn_str, deadline);
}

bool LibPqTestBase::TransactionalFailure(const Status& status) {
  const uint8_t* pgerr = status.ErrorData(PgsqlErrorTag::kCategory);
  if (pgerr == nullptr) {
    return false;
  }
  YBPgErrorCode code = PgsqlErrorTag::Decode(pgerr);
  return code == YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE;
}

} // namespace pgwrapper
} // namespace yb
