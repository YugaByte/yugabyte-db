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
//
//--------------------------------------------------------------------------------------------------

#include "yb/common/table_properties_constants.h"
#include "yb/yql/cql/ql/test/ql-test-base.h"

namespace yb {
namespace ql {

class TestQLInsertTable : public QLTestBase {
 public:
  TestQLInsertTable() : QLTestBase() {
  }

  std::string InsertStmtWithTTL(std::string ttl_seconds) {
    return strings::Substitute(
        "INSERT INTO human_resource(id, name, salary) VALUES(1, 'Scott Tiger', 100) USING TTL $0;",
        ttl_seconds);
  }
};

TEST_F(TestQLInsertTable, TestQLInsertTableSimple) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  // Create the table 1.
  EXEC_VALID_STMT(
      "CREATE TABLE human_resource(id int, name varchar, salary int, primary key(id, name));");

  // INSERT: Valid statement with column list.
  EXEC_VALID_STMT("INSERT INTO human_resource(id, name, salary) VALUES(1, 'Scott Tiger', 100);");
  EXEC_VALID_STMT("INSERT INTO human_resource(id, name, salary) VALUES(-1, 'Scott Tiger', -100);");

  // INSERT: Valid statement with column list and ttl.
  EXEC_VALID_STMT(InsertStmtWithTTL(std::to_string(1)));

  // INSERT: Valid statement with ttl at the lower limit.
  EXEC_VALID_STMT(InsertStmtWithTTL(std::to_string(yb::common::kCassandraMinTtlSeconds)));

  // INSERT: Valid statement with ttl at the upper limit.
  EXEC_VALID_STMT(InsertStmtWithTTL(std::to_string(yb::common::kCassandraMaxTtlSeconds)));

  // INSERT: Invalid statement with ttl just over limit.
  EXEC_INVALID_STMT(InsertStmtWithTTL(std::to_string(yb::common::kCassandraMaxTtlSeconds + 1)));

  // INSERT: Invalid statement with ttl just below lower limit.
  EXEC_INVALID_STMT(InsertStmtWithTTL(std::to_string(yb::common::kCassandraMinTtlSeconds - 1)));

  // INSERT: Invalid statement with ttl too high.
  EXEC_INVALID_STMT(InsertStmtWithTTL(std::to_string(std::numeric_limits<int64_t>::max())));

  // INSERT: Invalid statement with float ttl.
  EXEC_INVALID_STMT(InsertStmtWithTTL("1.1"));

  // INSERT: Invalid statement with negative ttl.
  EXEC_INVALID_STMT(InsertStmtWithTTL(std::to_string(-1)));

  // INSERT: Invalid statement with hex ttl.
  EXEC_INVALID_STMT(InsertStmtWithTTL("0x100"));

  // INSERT: Statement with invalid TTL syntax.
  EXEC_INVALID_STMT("INSERT INTO human_resource(id, name, salary) VALUES(1, 'Scott Tiger', 100) "
      "TTL 1000;");
  EXEC_INVALID_STMT("INSERT INTO human_resource(id, name, salary) VALUES(-1, 'Scott Tiger', -100) "
      "TTL 1000;");

  // INSERT: Statement with invalid TTL value.
  EXEC_INVALID_STMT(InsertStmtWithTTL("abcxyz"));

  // INSERT: Invalid CQL statement though it's a valid SQL statement without column list.
  EXEC_INVALID_STMT("INSERT INTO human_resource VALUES(2, 'Scott Tiger', 100);");
  EXEC_INVALID_STMT("INSERT INTO human_resource VALUES(-2, 'Scott Tiger', -100);");

  // INSERT: Invalid statement - Duplicate key.
  // IF NOT EXISTS is not implemented yet, so we ignore this test case for now.
  EXEC_VALID_STMT("INSERT INTO human_resource(id, name, salary) VALUES(1, 'Scott Tiger', 100) "
                  "IF NOT EXISTS;");
  EXEC_VALID_STMT("INSERT INTO human_resource(id, name, salary) VALUES(-1, 'Scott Tiger', -100) "
                  "IF NOT EXISTS;");

  // INSERT: Invalid statement - "--" is not yet supported.
  // TODO(neil) Parser needs to handle this error more elegantly.
  EXEC_INVALID_STMT("INSERT INTO human_resource_wrong(id, name, salary)"
                    "  VALUES(--1, 'Scott Tiger', 100);");

  // INSERT: Invalid statement - Wrong table name.
  EXEC_INVALID_STMT("INSERT INTO human_resource_wrong(id, name, salary)"
                    "  VALUES(1, 'Scott Tiger', 100);");

  // INSERT: Invalid statement - Mismatch column and argument count.
  EXEC_INVALID_STMT("INSERT INTO human_resource(id, name, salary) VALUES(1, 'Scott Tiger');");

  // INSERT: Invalid statement - Mismatch column and argument count.
  EXEC_INVALID_STMT("INSERT INTO human_resource(id, name, salary)"
                    " VALUES(1, 'Scott Tiger', 100, 200);");

  // INSERT: Invalid statement - Mismatch column and argument count.
  EXEC_INVALID_STMT("INSERT INTO human_resource(id, name, salary) VALUES(1);");

  // INSERT: Invalid statement - Mismatch column and argument count.
  EXEC_INVALID_STMT("INSERT INTO human_resource(id, name) VALUES(1, 'Scott Tiger', 100);");

  // INSERT: Invalid statement - Missing primary key (id).
  EXEC_INVALID_STMT("INSERT INTO human_resource(name, salary) VALUES('Scott Tiger', 100);");

  // INSERT: Invalid statement - Missing primary key (name).
  EXEC_INVALID_STMT("INSERT INTO human_resource(id, salary) VALUES(2, 100);");

  // Because string and numeric datatypes are implicitly compatible, we cannot test datatype
  // mismatch yet. Once timestamp, boolean, ... types are introduced, type incompability should be
  // tested here also.
}

} // namespace ql
} // namespace yb
