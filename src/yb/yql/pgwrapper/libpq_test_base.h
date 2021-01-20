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
//

#ifndef YB_YQL_PGWRAPPER_LIBPQ_TEST_BASE_H
#define YB_YQL_PGWRAPPER_LIBPQ_TEST_BASE_H

#include "yb/util/monotime.h"
#include "yb/yql/pgwrapper/pg_wrapper_test_base.h"
#include "yb/yql/pgwrapper/libpq_utils.h"

namespace yb {
namespace pgwrapper {

class LibPqTestBase : public PgWrapperTestBase {
 protected:
  void SetUp() override;
  Result<PGConn> Connect();
  Result<PGConn> ConnectToDB(const string& db_name);
  Result<PGConn> ConnectToDBAsUser(const string& db_name, const string& user);
  Result<PGConn> ConnectUsingString(
      const string& conn_str,
      CoarseTimePoint deadline = CoarseMonoClock::Now() + MonoDelta::FromSeconds(10));
  static bool TransactionalFailure(const Status& status);
};

} // namespace pgwrapper
} // namespace yb

#endif // YB_YQL_PGWRAPPER_LIBPQ_TEST_BASE_H
