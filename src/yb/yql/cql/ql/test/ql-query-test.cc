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

#include <thread>
#include <cmath>

#include "yb/client/table.h"
#include "yb/common/jsonb.h"
#include "yb/common/ql_value.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/master.h"
#include "yb/master/ts_manager.h"
#include "yb/util/yb_partition.h"
#include "yb/util/crypt.h"
#include "yb/yql/cql/ql/test/ql-test-base.h"

using std::string;
using std::unique_ptr;
using std::shared_ptr;
using strings::Substitute;

namespace yb {
namespace ql {

class TestQLQuery : public QLTestBase {
 public:
  TestQLQuery() : QLTestBase() {
  }

  std::shared_ptr<QLRowBlock> ExecSelect(TestQLProcessor *processor, int expected_rows = 1) {
    auto select = "SELECT c1, c2, c3 FROM test_table WHERE c1 = 1";
    Status s = processor->Run(select);
    CHECK(s.ok());
    auto row_block = processor->row_block();
    EXPECT_EQ(expected_rows, row_block->row_count());
    return row_block;
  }

  void VerifyExpiry(TestQLProcessor *processor) {
    ExecSelect(processor, 0);
  }

  void CreateTableAndInsertRow(TestQLProcessor *processor, bool with_ttl = true) {
    // Create the table.
    const char *create_stmt =
        "CREATE TABLE test_table(c1 int, c2 int, c3 int, "
            "primary key(c1));";
    Status s = processor->Run(create_stmt);
    CHECK(s.ok());

    std::string insert_stmt("INSERT INTO test_table(c1, c2, c3) VALUES(1, 2, 3)");
    if (with_ttl) {
      // Insert row with ttl.
      insert_stmt += " USING TTL 1;";
    } else {
      insert_stmt += ";";
    }
    s = processor->Run(insert_stmt);
    CHECK_OK(s);

    // Verify row is present.
    auto row_block = ExecSelect(processor);
    QLRow& row = row_block->row(0);

    EXPECT_EQ(1, row.column(0).int32_value());
    EXPECT_EQ(2, row.column(1).int32_value());
    EXPECT_EQ(3, row.column(2).int32_value());
  }

  void TestSelectWithBounds(
      string select_stmt,
      TestQLProcessor* processor,
      const std::vector<std::tuple<int64_t, int, string, int, int>>& rows,
      int start_idx, int end_idx, string order_by_clause) {
    std::vector<std::tuple<int64_t, int, string, int, int>> test_rows(
        &rows[start_idx], &rows[end_idx]);

    select_stmt += order_by_clause;

    if (order_by_clause == " ORDER BY r1 DESC") {
      std::reverse(test_rows.begin(), test_rows.end());
    }
    CHECK_OK(processor->Run(select_stmt));
    auto row_block = processor->row_block();
    // checking result
    ASSERT_EQ(test_rows.size(), row_block->row_count());
    for (int i = 0; i < test_rows.size(); i++) {
      QLRow &row = row_block->row(i);
      EXPECT_EQ(std::get<1>(test_rows[i]), row.column(0).int32_value());
      EXPECT_EQ(std::get<2>(test_rows[i]), row.column(1).string_value());
      EXPECT_EQ(std::get<3>(test_rows[i]), row.column(2).int32_value());
      EXPECT_EQ(std::get<4>(test_rows[i]), row.column(3).int32_value());
    }
  }

  // Make sure regular scan works, but errors when order by is present.
  void TestSelectWithoutOrderBy(
      string select_stmt,
      TestQLProcessor *processor,
      const std::vector<std::tuple<int64_t, int, string, int, int>> &rows,
      int start_idx, int end_idx) {
    // Forward scan
    TestSelectWithBounds(select_stmt, processor, rows, start_idx, end_idx, "");
    CHECK_INVALID_STMT(select_stmt + " ORDER BY r1 DESC");
    CHECK_INVALID_STMT(select_stmt + " ORDER BY r1 ASC");
  }

  // Make sure normal scans and those with order by clause all work.
  void TestSelectWithOrderBy(
      string select_stmt,
      TestQLProcessor *processor,
      const std::vector<std::tuple<int64_t, int, string, int, int>> &rows,
      int start_idx, int end_idx) {
    // Forward scan
    TestSelectWithBounds(select_stmt, processor, rows, start_idx, end_idx, "");
    TestSelectWithBounds(select_stmt, processor, rows, start_idx, end_idx, " ORDER BY r1 ASC");
    TestSelectWithBounds(select_stmt, processor, rows, start_idx, end_idx, " ORDER BY r1 DESC");
  }

  void TestScanWithBoundsPartitionOps(const std::string& func_name, int64_t max_hash,
                                      int64_t min_hash) {
    //----------------------------------------------------------------------------------------------
    // Setting up cluster
    //----------------------------------------------------------------------------------------------

    // Init the simulated cluster.
    ASSERT_NO_FATALS(CreateSimulatedCluster());

    // Get a processor.
    TestQLProcessor *processor = GetQLProcessor();

    // Create test table.
    CHECK_OK(processor->Run("CREATE TABLE scan_bounds_test (h1 int, h2 text, r1 int, v1 int,"
                                " PRIMARY KEY((h1, h2), r1));"));

    client::YBTableName name(YQL_DATABASE_CQL, kDefaultKeyspaceName, "scan_bounds_test");
    shared_ptr<client::YBTable> table;

    ASSERT_OK(client_->OpenTable(name, &table));

    //----------------------------------------------------------------------------------------------
    // Initializing rows data
    //----------------------------------------------------------------------------------------------

    // generating input data with hash_code and inserting into table
    std::vector<std::tuple<int64_t, int, string, int, int>> rows;
    for (int i = 0; i < 10; i++) {
      std::string i_str = std::to_string(i);
      google::protobuf::RepeatedPtrField<QLExpressionPB> row;
      row.Add()->mutable_value()->set_int32_value(i);
      row.Add()->mutable_value()->set_string_value(i_str);

      std::string part_key;
      CHECK_OK(table->partition_schema().EncodeKey(row, &part_key));
      uint16_t hash_code = PartitionSchema::DecodeMultiColumnHashValue(part_key);

      int64_t cql_hash = func_name == "token" ? YBPartition::YBToCqlHashCode(hash_code) : hash_code;
      std::tuple<int64_t, int, string, int, int> values(cql_hash, i, i_str, i, i);
      rows.push_back(values);
      string stmt = Substitute("INSERT INTO scan_bounds_test (h1, h2, r1, v1) VALUES "
                                   "($0, '$1', $2, $3);", i, i, i, i);
      CHECK_OK(processor->Run(stmt));
    }

    // ordering rows by hash code
    std::sort(rows.begin(), rows.end(),
              [](const std::tuple<int64_t, int, string, int, int>& r1,
                 const std::tuple<int64_t, int, string, int, int>& r2) -> bool {
                return std::get<0>(r1) < std::get<0>(r2);
              });

    // CQL uses 64 bit hashes, but YB uses 16-bit internally.
    // Our bucket range is [cql_hash, cql_hash + bucket_size) -- start-inclusive, end-exclusive
    // We test the bucket ranges below by choosing the appropriate values for the bounds.
    int64_t bucket_size = 1;
    if (func_name == "token") {
      bucket_size = bucket_size << 48;
    }

    //----------------------------------------------------------------------------------------------
    // Testing Select with lower bound
    //----------------------------------------------------------------------------------------------

    //---------------------------------- Exclusive Lower Bound -------------------------------------
    string select_stmt_template = "SELECT * FROM scan_bounds_test WHERE $0(h1, h2) > $1";

    // Entire range: hashes 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[0]) - 1), processor, rows,
        0, 10);
    // Partial range: hashes 4, 5, 6, 7, 8, 9
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[3])), processor, rows,
        4, 10);
    // Empty range: no hashes.
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[9])), processor, rows,
        0, 0);
    // Empty range: no hashes (checking overflow for max Cql hash)
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, max_hash), processor, rows,
        0, 0);

    //---------------------------------- Inclusive Lower Bound -------------------------------------
    select_stmt_template = "SELECT * FROM scan_bounds_test WHERE $0(h1, h2) >= $1";
    // Entire range: hashes 0, 1, 2, 3, 4, 5, 6, 7, 8, 9.
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[0]) + bucket_size - 1),
        processor, rows, 0, 10);
    // Partial range: hashes 6, 7, 8, 9
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[6]) + bucket_size - 1),
        processor, rows, 6, 10);
    // Empty range: no hashes
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[9]) + bucket_size), processor,
        rows, 0, 0);

    //----------------------------------------------------------------------------------------------
    // Testing Select with upper bound
    //----------------------------------------------------------------------------------------------

    //---------------------------------- Exclusive Upper Bound -------------------------------------
    select_stmt_template = "SELECT * FROM scan_bounds_test WHERE $0(h1, h2) < $1";
    // Entire range: hashes 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[9]) + bucket_size), processor,
        rows, 0, 10);
    // Partial range: hashes 0, 1, 2, 3, 4, 5, 6
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[7]) + bucket_size - 1),
        processor, rows, 0, 7);
    // Empty range: no hashes
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[0]) + bucket_size - 1),
        processor, rows, 0, 0);

    //---------------------------------- Inclusive Upper Bound -------------------------------------
    select_stmt_template = "SELECT * FROM scan_bounds_test WHERE $0(h1, h2) <= $1";
    // Entire range: hashes 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[9])), processor, rows, 0, 10);
    // Partial range: hashes 0, 1, 2
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[2])), processor, rows, 0, 3);
    // Empty range: no hashes
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[0]) - 1), processor, rows, 0,
        0);

    //----------------------------------------------------------------------------------------------
    // Testing Select with both lower and upper bounds
    //----------------------------------------------------------------------------------------------
    select_stmt_template = "SELECT * FROM scan_bounds_test WHERE $0(h1, h2) $1 $2 AND $3(h1, h2) "
        "$4 $5";
    // Entire range: hashes 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">=", std::get<0>(rows[0]) + bucket_size - 1,
                   func_name, "<=", std::get<0>(rows[9])), processor, rows, 0, 10);
    // Partial Range: hashes 2, 3, 4, 5, 6, 7, 8
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">", std::get<0>(rows[1]),
                   func_name, "<=", std::get<0>(rows[8])), processor, rows, 2, 9);
    // Partial Range: hashes 4, 5, 6
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">=", std::get<0>(rows[4]) + bucket_size - 1,
                   func_name, "<", std::get<0>(rows[7]) + bucket_size - 1), processor, rows,
        4, 7);
    // Empty Range (inclusive lower bound): no hashes
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">=", std::get<0>(rows[2]) + bucket_size - 1,
                   func_name, "<", std::get<0>(rows[2]) + bucket_size - 1), processor, rows, 0, 0);
    // Empty Range (inclusive upper bound): no hashes
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">", std::get<0>(rows[2]),
                   func_name, "<=", std::get<0>(rows[2])), processor, rows, 0, 0);
    // Empty range: no hashes (checking overflow for max Cql hash)
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, "<=", std::get<0>(rows[9]), func_name, ">",
        max_hash), processor, rows, 0, 0);

    //----------------------------------------------------------------------------------------------
    // Testing Select with range limits
    //----------------------------------------------------------------------------------------------
    // Entire range: [min, max].
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">=", min_hash, func_name, "<=", max_hash),
        processor, rows, 0, 10);

    // Entire range: [min, min] (upper bound min is treated as a special case in Cassandra).
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">=", min_hash, func_name,  "<=", min_hash),
        processor, rows, 0, func_name == "token" ? 10 : 0);

    // Entire range: [min, min). (upper bound min is treated as a special case in Cassandra).
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">=", min_hash, func_name, "<", min_hash),
        processor, rows, 0, func_name == "token" ? 10 : 0);

    // Empty range: (max, max].
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">", max_hash, func_name, "<=", max_hash),
        processor, rows, 0, 0);

    // Empty range: (max, min] (max as strict lower bound already excludes all hashes).
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, ">", max_hash, func_name, "<=", min_hash),
        processor, rows, 0, 0);

    //----------------------------------------------------------------------------------------------
    // Testing Select with exact partition key
    //----------------------------------------------------------------------------------------------
    select_stmt_template = "SELECT * FROM scan_bounds_test WHERE $0(h1, h2) = $1";
    // testing existing hash: 2
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[2])), processor, rows,
        2, 3);
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[2])), processor, rows,
        2, 3);
    // testing non-existing hash: empty
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[9]) + bucket_size), processor,
        rows, 0, 0);
    TestSelectWithoutOrderBy(
        Substitute(select_stmt_template, func_name, std::get<0>(rows[9]) + bucket_size), processor,
        rows, 0, 0);

    //----------------------------------------------------------------------------------------------
    // Testing Select with conditions on both partition key and individual hash columns
    //   - These queries are not always logical (i.e. partition key condition is usually irrelevant
    //   - if part. column values are given) but Cassandra supports them so we replicate its
    //     behavior
    //----------------------------------------------------------------------------------------------
    select_stmt_template = "SELECT * FROM scan_bounds_test WHERE "
        "h1 = $0 AND h2 = '$1' AND $2(h1, h2) $3 $4";

    // checking existing row with exact partition key: 6
    TestSelectWithOrderBy(
        Substitute(select_stmt_template, std::get<1>(rows[6]), std::get<2>(rows[6]), func_name,
                   "=", std::get<0>(rows[6])), processor, rows, 6, 7);
    // checking existing row with partition key bound: 3 with partition upper bound 7 (to include 3)
    TestSelectWithOrderBy(
        Substitute(select_stmt_template, std::get<1>(rows[3]), std::get<2>(rows[3]), func_name,
                   "<=", std::get<0>(rows[7])), processor, rows, 3, 4);
    // checking existing row with partition key bound: 4 with partition lower bound 5 (to exclude 4)
    TestSelectWithOrderBy(
        Substitute(select_stmt_template, std::get<1>(rows[4]), std::get<2>(rows[4]), func_name,
                   ">=", std::get<0>(rows[5])), processor, rows, 0, 0);
    // checking existing row with partition key bound: 7 with partition upper bound 6 (to exclude 7)
    TestSelectWithOrderBy(
        Substitute(select_stmt_template, std::get<1>(rows[7]), std::get<2>(rows[7]), func_name,
                   "<=", std::get<0>(rows[6])), processor, rows, 0, 0);

    //----------------------------------------------------------------------------------------------
    // Testing Invalid Statements
    //----------------------------------------------------------------------------------------------

    // Invalid number of arguments.
    CHECK_INVALID_STMT(Substitute("SELECT * FROM scan_bounds_test WHERE $0() > 0", func_name));
    CHECK_INVALID_STMT(Substitute("SELECT * FROM scan_bounds_test WHERE $0(h1) > 0", func_name));
    CHECK_INVALID_STMT(Substitute("SELECT * FROM scan_bounds_test WHERE $0(h1,h2,h1) > 0",
                                  func_name));

    // Invalid argument values.
    CHECK_INVALID_STMT(Substitute("SELECT * FROM scan_bounds_test WHERE $0(h2,h1) > 0", func_name));
    CHECK_INVALID_STMT(Substitute("SELECT * FROM scan_bounds_test WHERE $0(h1,h1) > 0", func_name));
    CHECK_INVALID_STMT(Substitute("SELECT * FROM scan_bounds_test WHERE $0(h2,h2) > 0", func_name));
    CHECK_INVALID_STMT(Substitute("SELECT * FROM scan_bounds_test WHERE $0(r1,h2) > 0", func_name));
    CHECK_INVALID_STMT(Substitute("SELECT * FROM scan_bounds_test WHERE $0(r1,v1) > 0", func_name));

    // Illogical conditions.
    // Two "greater-than" bounds
    CHECK_INVALID_STMT(
        Substitute("SELECT * FROM scan_bounds_test WHERE $0(h1,h2) > 0 AND $1(h1,h2) >= 0",
        func_name, func_name));
    // Two "less-than" bounds
    CHECK_INVALID_STMT(
        Substitute("SELECT * FROM scan_bounds_test WHERE $0(h1,h2) <= 0 AND $1(h1,h2) < 0",
        func_name, func_name));
    // Two "equal" conditions
    CHECK_INVALID_STMT(
        Substitute("SELECT * FROM scan_bounds_test WHERE $0(h1,h2) = 0 AND $1(h1,h2) = 0",
        func_name, func_name));
    // Both "equal" and "less than" conditions
    CHECK_INVALID_STMT(
        Substitute("SELECT * FROM scan_bounds_test WHERE $0(h1,h2) = 0 AND $1(h1,h2) <= 0",
        func_name, func_name));
    // Both "equal" and "greater than" conditions
    CHECK_INVALID_STMT(
        Substitute("SELECT * FROM scan_bounds_test WHERE $0(h1,h2) = 0 AND $1(h1,h2) >= 0",
        func_name, func_name));
  }

  void TestPartitionHash(const std::string& func_name) {
    //----------------------------------------------------------------------------------------------
    // Setting up cluster
    //----------------------------------------------------------------------------------------------

    // Init the simulated cluster.
    ASSERT_NO_FATALS(CreateSimulatedCluster());

    // Get a processor.
    TestQLProcessor *processor = GetQLProcessor();

    //----------------------------------------------------------------------------------------------
    // Testing all simple types (that are allowed in primary keys)
    //----------------------------------------------------------------------------------------------
    CHECK_OK(processor->Run(
        "create table partition_hash_bcall_simple_test("
        " h1 tinyint, h2 smallint, h3 int, h4 bigint, h5 varchar, h6 blob, h7 timestamp, "
        " h8 decimal, h9 inet, h10 uuid, h11 timeuuid, r int, v int,"
        " primary key((h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11), r));"));

    // Sample values to check hash value computation
    string key_values = "99, 9999, 999999, 99999999, 'foo bar', 0x12fe9a, "
        "'1999-12-01 16:32:44 GMT', 987654.0123, '204.101.0.168', "
        "97bda55b-6175-4c39-9e04-7c0205c709dc, 97bda55b-6175-1c39-9e04-7c0205c709dc";

    string insert_stmt = "INSERT INTO partition_hash_bcall_simple_test (h1, h2, h3, h4, h5, h6, "
        "h7, h8, h9, h10, h11, r, v) VALUES ($0, 1, 1);";

    CHECK_OK(processor->Run(Substitute(insert_stmt, key_values)));

    string select_stmt = Substitute(
        "SELECT * FROM partition_hash_bcall_simple_test WHERE "
            "$0(h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11) = $1($2)", func_name,
        func_name, key_values);

    CHECK_OK(processor->Run(select_stmt));
    auto row_block = processor->row_block();

    // Checking result.
    ASSERT_EQ(1, row_block->row_count());

    //----------------------------------------------------------------------------------------------
    // Testing separate UDT type.
    //----------------------------------------------------------------------------------------------
    CHECK_OK(processor->Run("CREATE TYPE udt_partition_hash_test(a int, b text)"));

    CHECK_OK(processor->Run(
        "CREATE TABLE partition_hash_bcall_udt_test("
        " h frozen<udt_partition_hash_test>, r int, v int, PRIMARY KEY ((h), r))"));

    // Sample values to check hash value computation
    key_values = "{a : 1, b : 'foo'}";

    insert_stmt = Substitute("INSERT INTO partition_hash_bcall_udt_test "
                             "(h, r, v) VALUES ($0, 1, 1);", key_values);
    CHECK_OK(processor->Run(insert_stmt));

    select_stmt = Substitute("SELECT * FROM partition_hash_bcall_udt_test WHERE "
                                 "$0(h) = $1($2)", func_name, func_name, key_values);
    CHECK_OK(processor->Run(select_stmt));

    // Checking result.
    row_block = processor->row_block();
    ASSERT_EQ(1, row_block->row_count());

    //----------------------------------------------------------------------------------------------
    // Testing parametric types (i.e. with frozen)
    //----------------------------------------------------------------------------------------------
    CHECK_OK(processor->Run(
        "CREATE TABLE partition_hash_bcall_frozen_test("
        " h1 frozen<map<int,text>>, h2 frozen<set<text>>, h3 frozen<list<int>>, "
        " h4 frozen<udt_partition_hash_test>, r int, v int, PRIMARY KEY ((h1, h2, h3, h4), r))"));

    // Sample values to check hash value computation
    key_values = "{1 : 'a', 2 : 'b'}, {'x', 'y'}, [3, 1, 2], {a : 1, b : 'foo'}";

    insert_stmt = Substitute("INSERT INTO partition_hash_bcall_frozen_test "
                                 "(h1, h2, h3, h4, r, v) VALUES ($0, 1, 1);", key_values);
    CHECK_OK(processor->Run(insert_stmt));

    select_stmt = Substitute("SELECT * FROM partition_hash_bcall_frozen_test WHERE "
                                 "$0(h1, h2, h3, h4) = $1($2)", func_name, func_name, key_values);
    CHECK_OK(processor->Run(select_stmt));

    // Checking result.
    row_block = processor->row_block();
    ASSERT_EQ(1, row_block->row_count());
  }

};

TEST_F(TestQLQuery, TestMissingSystemTable) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();
  const char* statement = "SELECT * FROM system.invalid_system_table_name";
  constexpr auto kRepetitions = 10;
  for (auto i = 0; i != kRepetitions; ++i) {
    CHECK_VALID_STMT(statement);
  }
}

TEST_F(TestQLQuery, TestQLQuerySimple) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();
  LOG(INFO) << "Running simple query test.";
  // Create the table 1.
  const char *create_stmt =
    "CREATE TABLE test_table(h1 int, h2 varchar, "
                            "r1 int, r2 varchar, "
                            "v1 int, v2 varchar, "
                            "primary key((h1, h2), r1, r2));";
  CHECK_VALID_STMT(create_stmt);

  // Test NOTFOUND. Select from empty table.
  CHECK_VALID_STMT("SELECT * FROM test_table");
  std::shared_ptr<QLRowBlock> empty_row_block = processor->row_block();
  CHECK_EQ(empty_row_block->row_count(), 0);
  CHECK_VALID_STMT("SELECT * FROM test_table WHERE h1 = 0 AND h2 = ''");
  empty_row_block = processor->row_block();
  CHECK_EQ(empty_row_block->row_count(), 0);

  // Check for valid allow filtering clauses.
  CHECK_VALID_STMT("SELECT * FROM test_table WHERE h1 = 0 AND h2 = '' ALLOW FILTERING");
  empty_row_block = processor->row_block();
  CHECK_EQ(empty_row_block->row_count(), 0);

  CHECK_VALID_STMT("SELECT h1, h2, r1, r2, v1, v2 FROM test_table "
      "  WHERE h1 = 7 AND h2 = 'h7' AND v1 = 1007 ALLOW FILTERING");
  empty_row_block = processor->row_block();
  CHECK_EQ(empty_row_block->row_count(), 0);

  CHECK_VALID_STMT("SELECT h1, h2, r1, r2, v1, v2 FROM test_table "
      "  WHERE h1 = 7 AND h2 = 'h7' AND v1 = 100 ALLOW FILTERING");
  empty_row_block = processor->row_block();
  CHECK_EQ(empty_row_block->row_count(), 0);

  // Insert 100 rows into the table.
  static const int kNumRows = 100;
  for (int idx = 0; idx < kNumRows; idx++) {
    // INSERT: Valid statement with column list.
    string stmt = Substitute("INSERT INTO test_table(h1, h2, r1, r2, v1, v2) "
                             "VALUES($0, 'h$1', $2, 'r$3', $4, 'v$5');",
                             idx, idx, idx+100, idx+100, idx+1000, idx+1000);
    CHECK_VALID_STMT(stmt);
  }
  LOG(INFO) << kNumRows << " rows inserted";

  //------------------------------------------------------------------------------------------------
  // Test simple query and result.
  CHECK_VALID_STMT("SELECT h1, h2, r1, r2, v1, v2 FROM test_table "
                   "  WHERE h1 = 7 AND h2 = 'h7' AND r1 = 107;");

  std::shared_ptr<QLRowBlock> row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& ordered_row = row_block->row(0);
  CHECK_EQ(ordered_row.column(0).int32_value(), 7);
  CHECK_EQ(ordered_row.column(1).string_value(), "h7");
  CHECK_EQ(ordered_row.column(2).int32_value(), 107);
  CHECK_EQ(ordered_row.column(3).string_value(), "r107");
  CHECK_EQ(ordered_row.column(4).int32_value(), 1007);
  CHECK_EQ(ordered_row.column(5).string_value(), "v1007");

  // Test simple query and result with different order.
  CHECK_VALID_STMT("SELECT v1, v2, h1, h2, r1, r2 FROM test_table "
                   "  WHERE h1 = 7 AND h2 = 'h7' AND r1 = 107;");

  row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& unordered_row = row_block->row(0);
  CHECK_EQ(unordered_row.column(0).int32_value(), 1007);
  CHECK_EQ(unordered_row.column(1).string_value(), "v1007");
  CHECK_EQ(unordered_row.column(2).int32_value(), 7);
  CHECK_EQ(unordered_row.column(3).string_value(), "h7");
  CHECK_EQ(unordered_row.column(4).int32_value(), 107);
  CHECK_EQ(unordered_row.column(5).string_value(), "r107");

  // Test single row query for the whole table.
  for (int idx = 0; idx < kNumRows; idx++) {
    // SELECT: Valid statement with column list.
    string stmt = Substitute("SELECT h1, h2, r1, r2, v1, v2 FROM test_table "
                             "WHERE h1 = $0 AND h2 = 'h$1' AND r1 = $2 AND r2 = 'r$3';",
                             idx, idx, idx+100, idx+100);
    CHECK_VALID_STMT(stmt);

    row_block = processor->row_block();
    CHECK_EQ(row_block->row_count(), 1);
    const QLRow& row = row_block->row(0);
    CHECK_EQ(row.column(0).int32_value(), idx);
    CHECK_EQ(row.column(1).string_value(), Substitute("h$0", idx));
    CHECK_EQ(row.column(2).int32_value(), idx + 100);
    CHECK_EQ(row.column(3).string_value(), Substitute("r$0", idx + 100));
    CHECK_EQ(row.column(4).int32_value(), idx + 1000);
    CHECK_EQ(row.column(5).string_value(), Substitute("v$0", idx + 1000));
  }

  // Test multi row query for the whole table.
  // Insert 20 rows of the same hash key into the table.
  static const int kHashNumRows = 20;
  int32 h1_shared = 1111111;
  const string h2_shared = "h2_shared_key";
  for (int idx = 0; idx < kHashNumRows; idx++) {
    // INSERT: Valid statement with column list.
    string stmt = Substitute("INSERT INTO test_table(h1, h2, r1, r2, v1, v2) "
                             "VALUES($0, '$1', $2, 'r$3', $4, 'v$5');",
                             h1_shared, h2_shared, idx+100, idx+100, idx+1000, idx+1000);
    CHECK_VALID_STMT(stmt);
  }

  // Select all 20 rows and check the values.
  const string multi_select = Substitute("SELECT h1, h2, r1, r2, v1, v2 FROM test_table "
                                         "WHERE h1 = $0 AND h2 = '$1';",
                                         h1_shared, h2_shared);
  CHECK_VALID_STMT(multi_select);
  row_block = processor->row_block();

  // Check the result set.
  CHECK_EQ(row_block->row_count(), kHashNumRows);
  for (int idx = 0; idx < kHashNumRows; idx++) {
    const QLRow& row = row_block->row(idx);
    CHECK_EQ(row.column(0).int32_value(), h1_shared);
    CHECK_EQ(row.column(1).string_value(), h2_shared);
    CHECK_EQ(row.column(2).int32_value(), idx + 100);
    CHECK_EQ(row.column(3).string_value(), Substitute("r$0", idx + 100));
    CHECK_EQ(row.column(4).int32_value(), idx + 1000);
    CHECK_EQ(row.column(5).string_value(), Substitute("v$0", idx + 1000));
  }

  // Select only 2 rows and check the values.
  int limit = 2;
  string limit_select = Substitute("SELECT h1, h2, r1, r2, v1, v2 FROM test_table "
                                   "WHERE h1 = $0 AND h2 = '$1' LIMIT $2;",
                                   h1_shared, h2_shared, limit);
  CHECK_VALID_STMT(limit_select);
  row_block = processor->row_block();

  // Check the result set.
  CHECK_EQ(row_block->row_count(), limit);
  int32_t prev_r1 = 0;
  string prev_r2;
  for (int idx = 0; idx < limit; idx++) {
    const QLRow& row = row_block->row(idx);
    CHECK_EQ(row.column(0).int32_value(), h1_shared);
    CHECK_EQ(row.column(1).string_value(), h2_shared);
    CHECK_EQ(row.column(2).int32_value(), idx + 100);
    CHECK_EQ(row.column(3).string_value(), Substitute("r$0", idx + 100));
    CHECK_EQ(row.column(4).int32_value(), idx + 1000);
    CHECK_EQ(row.column(5).string_value(), Substitute("v$0", idx + 1000));
    CHECK_GT(row.column(2).int32_value(), prev_r1);
    CHECK_GT(row.column(3).string_value(), prev_r2);
    prev_r1 = row.column(2).int32_value();
    prev_r2 = row.column(3).string_value();
  }

  limit_select = Substitute("SELECT h1, h2, r1, r2, v1, v2 FROM test_table "
                            "WHERE h1 = $0 AND h2 = '$1' LIMIT $2 ALLOW FILTERING;",
                            h1_shared, h2_shared, limit);
  CHECK_VALID_STMT(limit_select);

  const string drop_stmt = "DROP TABLE test_table;";
  EXEC_VALID_STMT(drop_stmt);
}

TEST_F(TestQLQuery, TestPagingState) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  LOG(INFO) << "Running paging state test.";

  // Create table.
  CHECK_VALID_STMT("CREATE TABLE t (h int, r int, v int, primary key((h), r));");

  static constexpr int kNumRows = 100;
  // Insert 100 rows of the same hash key into the table.
  {
    for (int i = 1; i <= kNumRows; i++) {
      // INSERT: Valid statement with column list.
      string stmt = Substitute("INSERT INTO t (h, r, v) VALUES ($0, $1, $2);", 1, i, 100 + i);
      CHECK_VALID_STMT(stmt);
    }
    LOG(INFO) << kNumRows << " rows inserted";
  }

  // Read a single row. Verify row and that the paging state is empty.
  CHECK_VALID_STMT("SELECT h, r, v FROM t WHERE h = 1 AND r = 1;");
  std::shared_ptr<QLRowBlock> row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& row = row_block->row(0);
  CHECK_EQ(row.column(0).int32_value(), 1);
  CHECK_EQ(row.column(1).int32_value(), 1);
  CHECK_EQ(row.column(2).int32_value(), 101);
  CHECK(processor->rows_result()->paging_state().empty());

  // Read all rows. Verify rows and that they are read in the number of pages expected.
  {
    StatementParameters params;
    int kPageSize = 5;
    params.set_page_size(kPageSize);
    int page_count = 0;
    int i = 0;
    do {
      CHECK_OK(processor->Run("SELECT h, r, v FROM t WHERE h = 1;", params));
      std::shared_ptr<QLRowBlock> row_block = processor->row_block();
      CHECK_EQ(row_block->row_count(), kPageSize);
      for (int j = 0; j < kPageSize; j++) {
        const QLRow& row = row_block->row(j);
        i++;
        CHECK_EQ(row.column(0).int32_value(), 1);
        CHECK_EQ(row.column(1).int32_value(), i);
        CHECK_EQ(row.column(2).int32_value(), 100 + i);
      }
      page_count++;
      if (processor->rows_result()->paging_state().empty()) {
        break;
      }
      CHECK_OK(params.SetPagingState(processor->rows_result()->paging_state()));
    } while (true);
    CHECK_EQ(page_count, kNumRows / kPageSize);
  }

  // Read rows with a LIMIT. Verify rows and that they are read in the number of pages expected.
  {
    StatementParameters params;
    static constexpr int kLimit = 53;
    static constexpr int kPageSize = 5;
    params.set_page_size(kPageSize);
    int page_count = 0;
    int i = 0;
    string select_stmt = Substitute("SELECT h, r, v FROM t WHERE h = 1 LIMIT $0;", kLimit);
    do {
      CHECK_OK(processor->Run(select_stmt, params));
      std::shared_ptr<QLRowBlock> row_block = processor->row_block();
      for (int j = 0; j < row_block->row_count(); j++) {
        const QLRow& row = row_block->row(j);
        i++;
        CHECK_EQ(row.column(0).int32_value(), 1);
        CHECK_EQ(row.column(1).int32_value(), i);
        CHECK_EQ(row.column(2).int32_value(), 100 + i);
      }
      page_count++;
      if (processor->rows_result()->paging_state().empty()) {
        break;
      }
      CHECK_EQ(row_block->row_count(), kPageSize);
      CHECK_OK(params.SetPagingState(processor->rows_result()->paging_state()));
    } while (true);
    CHECK_EQ(i, kLimit);
    CHECK_EQ(page_count, static_cast<int>(ceil(static_cast<double>(kLimit) /
                                               static_cast<double>(kPageSize))));
  }

  // Insert anther 100 rows of different hash keys into the table.
  {
    for (int i = 1; i <= kNumRows; i++) {
      // INSERT: Valid statement with column list.
      string stmt = Substitute("INSERT INTO t (h, r, v) VALUES ($0, $1, $2);", i, 100 + i, 200 + i);
      CHECK_VALID_STMT(stmt);
    }
    LOG(INFO) << kNumRows << " rows inserted";
  }

  // Test full-table query without a hash key.

  // Read all rows. Verify rows and that they are read in the number of pages expected.
  {
    StatementParameters params;
    int kPageSize = 5;
    params.set_page_size(kPageSize);
    int page_count = 0;
    int row_count = 0;
    int sum = 0;
    do {
      CHECK_OK(processor->Run("SELECT h, r, v FROM t WHERE r > 100;", params));
      std::shared_ptr<QLRowBlock> row_block = processor->row_block();
      for (int j = 0; j < row_block->row_count(); j++) {
        const QLRow& row = row_block->row(j);
        CHECK_EQ(row.column(0).int32_value() + 100, row.column(1).int32_value());
        sum += row.column(0).int32_value();
        row_count++;
      }
      page_count++;
      if (processor->rows_result()->paging_state().empty()) {
        break;
      }
      CHECK_OK(params.SetPagingState(processor->rows_result()->paging_state()));
    } while (true);
    CHECK_EQ(row_count, kNumRows);
    // Page count should be at least "kNumRows / kPageSize". Can be more because some pages may not
    // be fully filled depending on the hash key distribution.
    CHECK_GE(page_count, kNumRows / kPageSize);
    CHECK_EQ(sum, (1 + kNumRows) * kNumRows / 2);
  }

  // Read rows with a LIMIT. Verify rows and that they are read in the number of pages expected.
  {
    StatementParameters params;
    static constexpr int kLimit = 53;
    static constexpr int kPageSize = 5;
    params.set_page_size(kPageSize);
    int page_count = 0;
    int row_count = 0;
    int sum = 0;
    string select_stmt = Substitute("SELECT h, r, v FROM t WHERE r > 100 LIMIT $0;", kLimit);
    do {
      CHECK_OK(processor->Run(select_stmt, params));
      std::shared_ptr<QLRowBlock> row_block = processor->row_block();
      for (int j = 0; j < row_block->row_count(); j++) {
        const QLRow& row = row_block->row(j);
        CHECK_EQ(row.column(0).int32_value() + 100, row.column(1).int32_value());
        sum += row.column(0).int32_value();
        row_count++;
      }
      page_count++;
      if (processor->rows_result()->paging_state().empty()) {
        break;
      }
      CHECK_OK(params.SetPagingState(processor->rows_result()->paging_state()));
    } while (true);
    CHECK_EQ(row_count, kLimit);
    // Page count should be at least "kLimit / kPageSize". Can be more because some pages may not
    // be fully filled depending on the hash key distribution. Same for sum which should be at
    // least the sum of the lowest consecutive kLimit number of "h" values. Can be more.
    CHECK_GE(page_count, static_cast<int>(ceil(static_cast<double>(kLimit) /
                                               static_cast<double>(kPageSize))));
    CHECK_GE(sum, (1 + kLimit) * kLimit / 2);
  }

  // Test queries with IN condition on hash key.
  // Verify rows and that they are read in the number of pages expected.
  {
    string select_stmt = "SELECT h, r, v FROM t WHERE h IN (1, 12, 23, 34, 45) AND r > 98;";
    VerifyPaginationSelect(processor, select_stmt, 2,
        "{ { int32:1, int32:99, int32:199 }, { int32:1, int32:100, int32:200 } }"
        "{ { int32:1, int32:101, int32:201 }, { int32:12, int32:112, int32:212 } }"
        "{ { int32:23, int32:123, int32:223 }, { int32:34, int32:134, int32:234 } }"
        "{ { int32:45, int32:145, int32:245 } }");
  }

  // Read rows with a LIMIT. Verify rows and that they are read in the number of pages expected.
  {
    string select_stmt = "SELECT h, r, v FROM t WHERE h IN (1, 12, 23, 34, 45) AND r > 98 LIMIT 5;";
    VerifyPaginationSelect(processor, select_stmt, 2,
        "{ { int32:1, int32:99, int32:199 }, { int32:1, int32:100, int32:200 } }"
            "{ { int32:1, int32:101, int32:201 }, { int32:12, int32:112, int32:212 } }"
            "{ { int32:23, int32:123, int32:223 } }");

    select_stmt = "SELECT h, r, v FROM t WHERE h IN (1, 12, 23, 34, 45) AND r > 98 LIMIT 2;";
    VerifyPaginationSelect(processor, select_stmt, 3,
        "{ { int32:1, int32:99, int32:199 }, { int32:1, int32:100, int32:200 } }");
  }
}

#define RUN_PAGINATION_WITH_DESC_TEST(processor, type, values, rows)                               \
do {                                                                                               \
  /* Creating the table. */                                                                        \
  string create_stmt = Substitute("CREATE TABLE t_$0 (h int, r1 $1, r2 $2, v int, "                \
      "primary key((h), r1, r2)) WITH CLUSTERING ORDER BY (r1 DESC, r2 ASC);", type, type, type);  \
  CHECK_VALID_STMT(create_stmt);                                                                   \
                                                                                                   \
  /* Inserting the values. */                                                                      \
  for (auto& value : values) {                                                                     \
    string stmt = Substitute("INSERT INTO t_$0 (h, r1, r2, v) VALUES (1, $1, $2, $3);",            \
        type, value, value, 0);                                                                  \
    CHECK_VALID_STMT(stmt);                                                                        \
  }                                                                                                \
  /* Seting up low page size for reading to test paging. */                                        \
  StatementParameters params;                                                                      \
  int kPageSize = 5;                                                                               \
  params.set_page_size(kPageSize);                                                                 \
  /* Setting up range query, will include all values except minimum and maximum */                 \
  int num_rows = values.size();                                                                    \
  auto min_val = values[0];                                                                        \
  auto max_val = values[num_rows - 1];                                                             \
  int page_count = 0;                                                                              \
  string select_stmt = Substitute("SELECT h, r1, r2, v FROM t_$0 WHERE h = 1 AND "                 \
    "r1 > $1 AND r2 > $2 AND r1 < $3 AND r2 < $4;", type, min_val, min_val, max_val, max_val );    \
  /* Reading rows, loading the rows vector to be checked later for each case */                    \
  do {                                                                                             \
    CHECK_OK(processor->Run(select_stmt, params));                                                 \
    std::shared_ptr<QLRowBlock> row_block = processor->row_block();                               \
    for (int j = 0; j < row_block->row_count(); j++) {                                             \
    const QLRow& row = row_block->row(j);                                                         \
      rows->push_back(row);                                                                        \
    }                                                                                              \
    page_count++;                                                                                  \
    if (processor->rows_result()->paging_state().empty()) {                                        \
      break;                                                                                       \
    }                                                                                              \
    CHECK_OK(params.SetPagingState(processor->rows_result()->paging_state()));                   \
  } while (true);                                                                                  \
  /* Page count should be at least "<nrRowsRead> / kPageSize". */                                  \
  /* Can be more since some pages may not be fully filled depending on hash key distribution. */   \
  CHECK_GE(page_count, (num_rows - 2) / kPageSize);                                                \
} while(0)

TEST_F(TestQLQuery, TestPaginationWithDescSort) {

  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  LOG(INFO) << "Running paging state test.";

  //------------------------------------------------------------------------------------------------
  // Testing integer types.
  //------------------------------------------------------------------------------------------------
  {
    std::vector<int> values;
    for (int i = 1; i <= 100; i++) {
      values.push_back(i);
    }
    std::vector<QLRow> rows;
    auto rows_ptr = &rows;
    RUN_PAGINATION_WITH_DESC_TEST(processor, "int", values, rows_ptr);
    // Checking rows values -- expecting results in descending order except for min and max values.
    CHECK_EQ(rows.size(), values.size() - 2);
    // Results should start from second-largest value.
    size_t curr_row_no = values.size() - 2;
    for (auto& row : rows) {
      CHECK_EQ(row.column(0).int32_value(), 1);
      // Expecting results in descending order.
      CHECK_EQ(row.column(1).int32_value(), values[curr_row_no]);
      CHECK_EQ(row.column(2).int32_value(), values[curr_row_no]);
      CHECK_EQ(row.column(3).int32_value(), 0);
      curr_row_no--;
    }
  }

  //------------------------------------------------------------------------------------------------
  // Testing timestamp type.
  //------------------------------------------------------------------------------------------------
  {
    std::vector<int> values;
    for (int i = 1; i <= 100; i++) {
      values.push_back(i);
    }
    std::vector<QLRow> rows;
    auto rows_ptr = &rows;
    RUN_PAGINATION_WITH_DESC_TEST(processor, "timestamp", values, rows_ptr);
    // Checking rows values -- expecting results in descending order except for min and max values.
    CHECK_EQ(rows.size(), values.size() - 2);
    // Results should start from second-largest value.
    size_t curr_row_no = values.size() - 2;
    int kMicrosPerMilli = 1000;

    for (auto& row : rows) {
      CHECK_EQ(row.column(0).int32_value(), 1);
      // Expecting results in descending order.
      CHECK_EQ(row.column(1).timestamp_value().ToInt64(), values[curr_row_no] * kMicrosPerMilli);
      CHECK_EQ(row.column(2).timestamp_value().ToInt64(), values[curr_row_no] * kMicrosPerMilli);
      CHECK_EQ(row.column(3).int32_value(), 0);
      curr_row_no--;
    }
  }

  //------------------------------------------------------------------------------------------------
  // Testing inet type.
  //------------------------------------------------------------------------------------------------
  {
    std::vector<string> values;
    std::vector<string> input_values;
    // Insert 100 rows of the same hash key into the table.
    for (int i = 1; i <= 100; i++) {
      string value = "127.0.0." + std::to_string(i);
      values.push_back(value);
      input_values.push_back("'" + value + "'");
    }
    std::vector<QLRow> rows;
    auto rows_ptr = &rows;
    RUN_PAGINATION_WITH_DESC_TEST(processor, "inet", input_values, rows_ptr);
    // Checking rows values -- expecting results in descending order except for min and max values.
    CHECK_EQ(rows.size(), values.size() - 2);
    // Results should start from second-largest value.
    size_t curr_row_no = values.size() - 2;
    for (auto& row : rows) {
      CHECK_EQ(row.column(0).int32_value(), 1);
      // Expecting results in descending order.
      CHECK_EQ(row.column(1).inetaddress_value().ToString(), values[curr_row_no]);
      CHECK_EQ(row.column(2).inetaddress_value().ToString(), values[curr_row_no]);
      CHECK_EQ(row.column(3).int32_value(), 0);
      curr_row_no--;
    }
  }

  //------------------------------------------------------------------------------------------------
  // Testing uuid types.
  //------------------------------------------------------------------------------------------------
  {
    std::vector<string> values;
    // Uuid prefix value -- missing last two digits.
    string uuid_prefix = "123e4567-e89b-02d3-a456-4266554400";
    // Insert 90 rows (two digit numbers) of the same hash key into the table.
    for (int i = 10; i < 100; i++) {
      values.push_back(uuid_prefix + std::to_string(i));
    }
    std::vector<QLRow> rows;
    auto rows_ptr = &rows;
    RUN_PAGINATION_WITH_DESC_TEST(processor, "uuid", values, rows_ptr);
    // Checking rows values -- expecting results in descending order except for min and max values.
    CHECK_EQ(rows.size(), values.size() - 2);
    // Results should start from second-largest value.
    size_t curr_row_no = values.size() - 2;
    for (auto &row : rows) {
      CHECK_EQ(row.column(0).int32_value(), 1);
      // Expecting results in descending order.
      CHECK_EQ(row.column(1).uuid_value().ToString(), values[curr_row_no]);
      CHECK_EQ(row.column(2).uuid_value().ToString(), values[curr_row_no]);
      CHECK_EQ(row.column(3).int32_value(), 0);
      curr_row_no--;
    }
  }

  //------------------------------------------------------------------------------------------------
  // Testing decimal type.
  //------------------------------------------------------------------------------------------------
  {
    std::vector<string> values;
    // Insert 100 rows of the same hash key into the table.
    for (int i = 10; i <= 100; i++) {
      values.push_back(std::to_string(i) + ".25");
    }
    std::vector<QLRow> rows;
    auto rows_ptr = &rows;
    RUN_PAGINATION_WITH_DESC_TEST(processor, "decimal", values, rows_ptr);
    // Checking rows values -- expecting results in descending order except for min and max values.
    CHECK_EQ(rows.size(), values.size() - 2);
    // Results should start from second-largest value.
    size_t curr_row_no = values.size() - 2;
    for (auto &row : rows) {
      CHECK_EQ(row.column(0).int32_value(), 1);
      // Expecting results in descending order.
      util::Decimal dec;
      CHECK_OK(dec.DecodeFromComparable(row.column(1).decimal_value()));
      CHECK_EQ(dec.ToString(), values[curr_row_no]);
      CHECK_OK(dec.DecodeFromComparable(row.column(2).decimal_value()));
      CHECK_EQ(dec.ToString(), values[curr_row_no]);
      CHECK_EQ(row.column(3).int32_value(), 0);
      curr_row_no--;
    }
  }

  //------------------------------------------------------------------------------------------------
  // Testing float type.
  //------------------------------------------------------------------------------------------------
  {
    std::vector<float> values;
    // Insert 100 rows of the same hash key into the table.
    for (int i = 10; i <= 100; i++) {
      values.push_back(i + .25);
    }
    std::vector<QLRow> rows;
    auto rows_ptr = &rows;
    RUN_PAGINATION_WITH_DESC_TEST(processor, "float", values, rows_ptr);
    // Checking rows values -- expecting results in descending order except for min and max values.
    CHECK_EQ(rows.size(), values.size() - 2);
    // Results should start from second-largest value.
    size_t curr_row_no = values.size() - 2;
    for (auto &row : rows) {
      CHECK_EQ(row.column(0).int32_value(), 1);
      // Expecting results in descending order.
      CHECK_EQ(row.column(1).float_value(), values[curr_row_no]);
      CHECK_EQ(row.column(2).float_value(), values[curr_row_no]);
      CHECK_EQ(row.column(3).int32_value(), 0);
      curr_row_no--;
    }
  }

  //------------------------------------------------------------------------------------------------
  // Testing double type.
  //------------------------------------------------------------------------------------------------
  {
    std::vector<double> values;
    // Insert 100 rows of the same hash key into the table.
    for (int i = 10; i <= 100; i++) {
      values.push_back(i + .25);
    }
    std::vector<QLRow> rows;
    auto rows_ptr = &rows;
    RUN_PAGINATION_WITH_DESC_TEST(processor, "double", values, rows_ptr);
    // Checking rows values -- expecting results in descending order except for min and max values.
    CHECK_EQ(rows.size(), values.size() - 2);
    // Results should start from second-largest value.
    size_t curr_row_no = values.size() - 2;
    for (auto &row : rows) {
      CHECK_EQ(row.column(0).int32_value(), 1);
      // Expecting results in descending order.
      CHECK_EQ(row.column(1).double_value(), values[curr_row_no]);
      CHECK_EQ(row.column(2).double_value(), values[curr_row_no]);
      CHECK_EQ(row.column(3).int32_value(), 0);
      curr_row_no--;
    }
  }

  //------------------------------------------------------------------------------------------------
  // Testing string types.
  //------------------------------------------------------------------------------------------------
  {
    std::vector<string> values;
    std::vector<string> input_values;
    // Insert 90 rows (two digit numbers) of the same hash key into the table.
    for (int i = 10; i < 100; i++) {
      values.push_back(std::to_string(i));
      input_values.push_back("'" + std::to_string(i) + "'");
    }
    std::vector<QLRow> rows;
    auto rows_ptr = &rows;
    RUN_PAGINATION_WITH_DESC_TEST(processor, "varchar", input_values, rows_ptr);
    // Checking rows values -- expecting results in descending order except for min and max values.
    CHECK_EQ(rows.size(), values.size() - 2);
    // Results should start from second-largest value.
    size_t curr_row_no = values.size() - 2;
    for (auto &row : rows) {
      CHECK_EQ(row.column(0).int32_value(), 1);
      // Expecting results in descending order.
      CHECK_EQ(row.column(1).string_value(), values[curr_row_no]);
      CHECK_EQ(row.column(2).string_value(), values[curr_row_no]);
      CHECK_EQ(row.column(3).int32_value(), 0);
      curr_row_no--;
    }
  }
}

TEST_F(TestQLQuery, TestQLQueryPartialHash) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  LOG(INFO) << "Running partial hash test.";
  // Create the table 1.
  const char *create_stmt =
    "CREATE TABLE test_table(h1 int, h2 varchar, "
                            "h3 bigint, h4 varchar, "
                            "r1 int, r2 varchar, "
                            "v1 int, v2 varchar, "
                            "primary key((h1, h2, h3, h4), r1, r2));";
  CHECK_VALID_STMT(create_stmt);

  // Test NOTFOUND. Select from empty table.
  CHECK_VALID_STMT("SELECT * FROM test_table");
  std::shared_ptr<QLRowBlock> empty_row_block = processor->row_block();
  CHECK_EQ(empty_row_block->row_count(), 0);
  CHECK_VALID_STMT("SELECT * FROM test_table WHERE h1 = 0 AND h2 = ''");
  empty_row_block = processor->row_block();
  CHECK_EQ(empty_row_block->row_count(), 0);

  // Insert 100 rows into the table.
  static const int kNumRows = 100;
  for (int idx = 0; idx < kNumRows; idx++) {
    // INSERT: Valid statement with column list.
    string stmt = Substitute("INSERT INTO test_table(h1, h2, h3, h4, r1, r2, v1, v2) "
                             "VALUES($0, 'h$1', $2, 'h$3', $4, 'r$5', $6, 'v$7');",
                             idx, idx, idx+100, idx+100, idx+1000, idx+1000, idx+10000, idx+10000);
    CHECK_VALID_STMT(stmt);
  }
  LOG(INFO) << kNumRows << " rows inserted";

  //------------------------------------------------------------------------------------------------
  // Check invalid case for using other operators for hash keys.
  CHECK_INVALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                     "  WHERE h1 < 7;");
  CHECK_INVALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                     "  WHERE h1 > 7 AND h2 > 'h7';");

  //------------------------------------------------------------------------------------------------
  // Test partial hash keys and results.
  LOG(INFO) << "Testing 3 out of 4 keys";
  CHECK_VALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                   "  WHERE h1 = 7 AND h2 = 'h7' AND h3 = 107;");
  std::shared_ptr<QLRowBlock> row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& no_hash_row1 = row_block->row(0);
  CHECK_EQ(no_hash_row1.column(0).int32_value(), 7);
  CHECK_EQ(no_hash_row1.column(1).string_value(), "h7");
  CHECK_EQ(no_hash_row1.column(2).int64_value(), 107);
  CHECK_EQ(no_hash_row1.column(3).string_value(), "h107");
  CHECK_EQ(no_hash_row1.column(4).int32_value(), 1007);
  CHECK_EQ(no_hash_row1.column(5).string_value(), "r1007");
  CHECK_EQ(no_hash_row1.column(6).int32_value(), 10007);
  CHECK_EQ(no_hash_row1.column(7).string_value(), "v10007");

  LOG(INFO) << "Testing 2 out of 4 keys";
  CHECK_VALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                   "  WHERE h1 = 7 AND h2 = 'h7';");
  row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& no_hash_row2 = row_block->row(0);
  CHECK_EQ(no_hash_row2.column(0).int32_value(), 7);
  CHECK_EQ(no_hash_row2.column(1).string_value(), "h7");
  CHECK_EQ(no_hash_row2.column(2).int64_value(), 107);
  CHECK_EQ(no_hash_row2.column(3).string_value(), "h107");
  CHECK_EQ(no_hash_row2.column(4).int32_value(), 1007);
  CHECK_EQ(no_hash_row2.column(5).string_value(), "r1007");
  CHECK_EQ(no_hash_row2.column(6).int32_value(), 10007);
  CHECK_EQ(no_hash_row2.column(7).string_value(), "v10007");

  LOG(INFO) << "Testing 1 out of 4 keys";
  CHECK_VALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                   "  WHERE h1 = 7;");
  row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& no_hash_row3 = row_block->row(0);
  CHECK_EQ(no_hash_row3.column(0).int32_value(), 7);
  CHECK_EQ(no_hash_row3.column(1).string_value(), "h7");
  CHECK_EQ(no_hash_row3.column(2).int64_value(), 107);
  CHECK_EQ(no_hash_row3.column(3).string_value(), "h107");
  CHECK_EQ(no_hash_row3.column(4).int32_value(), 1007);
  CHECK_EQ(no_hash_row3.column(5).string_value(), "r1007");
  CHECK_EQ(no_hash_row3.column(6).int32_value(), 10007);
  CHECK_EQ(no_hash_row3.column(7).string_value(), "v10007");

  // Test simple query with only range key and check result.
  LOG(INFO) << "Testing 0 out of 4 keys";
  CHECK_VALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                   "WHERE r1 = 1007;");
  row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& no_hash_row4 = row_block->row(0);
  CHECK_EQ(no_hash_row4.column(0).int32_value(), 7);
  CHECK_EQ(no_hash_row4.column(1).string_value(), "h7");
  CHECK_EQ(no_hash_row4.column(2).int64_value(), 107);
  CHECK_EQ(no_hash_row4.column(3).string_value(), "h107");
  CHECK_EQ(no_hash_row4.column(4).int32_value(), 1007);
  CHECK_EQ(no_hash_row4.column(5).string_value(), "r1007");
  CHECK_EQ(no_hash_row4.column(6).int32_value(), 10007);
  CHECK_EQ(no_hash_row4.column(7).string_value(), "v10007");

  LOG(INFO) << "Testing 1 of every key each.";
  // Test simple query with partial hash key and check result.
  CHECK_VALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                   "WHERE h1 = 7;");
  row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& h1_hash_row = row_block->row(0);
  CHECK_EQ(h1_hash_row.column(0).int32_value(), 7);
  CHECK_EQ(h1_hash_row.column(1).string_value(), "h7");
  CHECK_EQ(h1_hash_row.column(2).int64_value(), 107);
  CHECK_EQ(h1_hash_row.column(3).string_value(), "h107");
  CHECK_EQ(h1_hash_row.column(4).int32_value(), 1007);
  CHECK_EQ(h1_hash_row.column(5).string_value(), "r1007");
  CHECK_EQ(h1_hash_row.column(6).int32_value(), 10007);
  CHECK_EQ(h1_hash_row.column(7).string_value(), "v10007");

  CHECK_VALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                   "  WHERE h2 = 'h7';");
  row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& h2_hash_row = row_block->row(0);
  CHECK_EQ(h2_hash_row.column(0).int32_value(), 7);
  CHECK_EQ(h2_hash_row.column(1).string_value(), "h7");
  CHECK_EQ(h2_hash_row.column(2).int64_value(), 107);
  CHECK_EQ(h2_hash_row.column(3).string_value(), "h107");
  CHECK_EQ(h2_hash_row.column(4).int32_value(), 1007);
  CHECK_EQ(h2_hash_row.column(5).string_value(), "r1007");
  CHECK_EQ(h2_hash_row.column(6).int32_value(), 10007);
  CHECK_EQ(h2_hash_row.column(7).string_value(), "v10007");

  CHECK_VALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                   "  WHERE h3 = 107;");
  row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& h3_hash_row = row_block->row(0);
  CHECK_EQ(h3_hash_row.column(0).int32_value(), 7);
  CHECK_EQ(h3_hash_row.column(1).string_value(), "h7");
  CHECK_EQ(h3_hash_row.column(2).int64_value(), 107);
  CHECK_EQ(h3_hash_row.column(3).string_value(), "h107");
  CHECK_EQ(h3_hash_row.column(4).int32_value(), 1007);
  CHECK_EQ(h3_hash_row.column(5).string_value(), "r1007");
  CHECK_EQ(h3_hash_row.column(6).int32_value(), 10007);
  CHECK_EQ(h3_hash_row.column(7).string_value(), "v10007");

  CHECK_VALID_STMT("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                   "  WHERE h4 = 'h107';");
  row_block = processor->row_block();
  CHECK_EQ(row_block->row_count(), 1);
  const QLRow& h4_hash_row = row_block->row(0);
  CHECK_EQ(h4_hash_row.column(0).int32_value(), 7);
  CHECK_EQ(h4_hash_row.column(1).string_value(), "h7");
  CHECK_EQ(h4_hash_row.column(2).int64_value(), 107);
  CHECK_EQ(h4_hash_row.column(3).string_value(), "h107");
  CHECK_EQ(h4_hash_row.column(4).int32_value(), 1007);
  CHECK_EQ(h4_hash_row.column(5).string_value(), "r1007");
  CHECK_EQ(h4_hash_row.column(6).int32_value(), 10007);
  CHECK_EQ(h4_hash_row.column(7).string_value(), "v10007");


  // Test multi row query for the whole table.
  // Insert 20 rows of the same hash key into the table.
  static const int kHashNumRows = 20;
  static const int kNumFilterRows = 10;
  int32 h1_shared = 1111111;
  const string h2_shared = "h2_shared_key";
  int64 h3_shared = 111111111;
  const string h4_shared = "h4_shared_key";
  for (int idx = 0; idx < kHashNumRows; idx++) {
    // INSERT: Valid statement with column list.
    string stmt = Substitute("INSERT INTO test_table(h1, h2, h3, h4, r1, r2, v1, v2) "
                             "VALUES($0, '$1', $2, '$3', $4, 'r$5', $6, 'v$7');",
                             h1_shared, h2_shared, h3_shared, h4_shared,
                             idx+100, idx+100, idx+1000, idx+1000);
    CHECK_VALID_STMT(stmt);
  }

  // Select select rows and check the values.
  // This test scans multiple tservers. Query result tested in java.
  // TODO: Make QL understand paging states and continue.
  LOG(INFO) << "Testing filter with partial hash keys.";
  const string multi_select = Substitute("SELECT h1, h2, h3, h4, r1, r2, v1, v2 FROM test_table "
                                         "WHERE h1 = $0 AND h2 = '$1' AND r1 > $2;",
                                         h1_shared, h2_shared, kNumFilterRows + 100);
  CHECK_VALID_STMT(multi_select);

  const string drop_stmt = "DROP TABLE test_table;";
  EXEC_VALID_STMT(drop_stmt);
}

TEST_F(TestQLQuery, TestInsertWithTTL) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  CreateTableAndInsertRow(processor);

  // Sleep for 1.1 seconds and verify ttl has expired.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  VerifyExpiry(processor);
}

TEST_F(TestQLQuery, TestUpdateWithTTL) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  CreateTableAndInsertRow(processor, false);

  // Now update the row with a TTL.
  std::string update_stmt("UPDATE test_table USING TTL 1 SET c2 = 4, c3 = 5 WHERE c1 = 1;");
  Status s = processor->Run(update_stmt);
  CHECK(s.ok());

  // Sleep for 1.1 seconds and verify ttl has expired.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // c1 = 1 should still exist.
  auto row_block = ExecSelect(processor);
  QLRow& row = row_block->row(0);

  EXPECT_EQ(1, row.column(0).int32_value());
  EXPECT_TRUE(row.column(1).IsNull());
  EXPECT_TRUE(row.column(2).IsNull());

  // Try an update by setting the primary key, which should fail since set clause can't have
  // primary keys.
  std::string invalid_update_stmt("UPDATE test_table USING TTL 1 SET c1 = 4 WHERE c1 = 1;");
  s = processor->Run(invalid_update_stmt);
  CHECK(!s.ok());
}

// The main goal of this test is to check that the serialization/deserialization operations match
// The Java tests are more comprehensive but do not test the deserialization -- since they use the
// Cassandra deserializer instead
TEST_F(TestQLQuery, TestCollectionTypes) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  //------------------------------------------------------------------------------------------------
  // Testing Map type
  //------------------------------------------------------------------------------------------------

  // Create table.
  const char *map_create_stmt =
      "CREATE TABLE map_test (id int PRIMARY KEY, v int, mp map<int, varchar>, c varchar);";
  Status s = processor->Run(map_create_stmt);
  CHECK(s.ok());

  // Insert Values
  std::string map_insert_stmt("INSERT INTO map_test (id, v, mp, c) values "
      "(1, 3, {21 : 'a', 22 : 'b', 23 : 'c'}, 'x');");
  s = processor->Run(map_insert_stmt);
  CHECK_OK(s);

  // Check Select
  auto map_select_stmt = "SELECT * FROM map_test WHERE id = 1";
  s = processor->Run(map_select_stmt);
  CHECK(s.ok());
  auto map_row_block = processor->row_block();
  EXPECT_EQ(1, map_row_block->row_count());
  QLRow& map_row = map_row_block->row(0);

  // check row
  EXPECT_EQ(1, map_row.column(0).int32_value());
  EXPECT_EQ(3, map_row.column(1).int32_value());
  EXPECT_EQ("x", map_row.column(3).string_value());
  // check map
  EXPECT_EQ(InternalType::kMapValue, map_row.column(2).type());
  QLMapValuePB map_value = map_row.column(2).map_value();
  // check keys
  EXPECT_EQ(3, map_value.keys_size());
  EXPECT_EQ(21, map_value.keys(0).int32_value());
  EXPECT_EQ(22, map_value.keys(1).int32_value());
  EXPECT_EQ(23, map_value.keys(2).int32_value());
  // check values
  EXPECT_EQ(3, map_value.values_size());
  EXPECT_EQ("a", map_value.values(0).string_value());
  EXPECT_EQ("b", map_value.values(1).string_value());
  EXPECT_EQ("c", map_value.values(2).string_value());

  //------------------------------------------------------------------------------------------------
  // Testing Set type
  //------------------------------------------------------------------------------------------------

  // Create table.
  const char *set_create_stmt =
      "CREATE TABLE set_test (id int PRIMARY KEY, v int, st set<int>, c varchar);";
  s = processor->Run(set_create_stmt);
  CHECK(s.ok());

  // Insert Values
  std::string set_insert_stmt("INSERT INTO set_test (id, v, st, c) values "
      "(1, 3, {3, 4, 1, 1, 2, 4, 2}, 'x');");
  s = processor->Run(set_insert_stmt);
  CHECK_OK(s);

  // Check Select
  auto set_select_stmt = "SELECT * FROM set_test WHERE id = 1";
  s = processor->Run(set_select_stmt);
  CHECK(s.ok());
  auto set_row_block = processor->row_block();
  EXPECT_EQ(1, set_row_block->row_count());
  QLRow& set_row = set_row_block->row(0);

  // check row
  EXPECT_EQ(1, set_row.column(0).int32_value());
  EXPECT_EQ(3, set_row.column(1).int32_value());
  EXPECT_EQ("x", set_row.column(3).string_value());
  // check set
  EXPECT_EQ(InternalType::kSetValue, set_row.column(2).type());
  QLSeqValuePB set_value = set_row.column(2).set_value();
  // check elems
  // returned set should have no duplicates
  EXPECT_EQ(4, set_value.elems_size());
  // set elements should be in default (ascending) order
  EXPECT_EQ(1, set_value.elems(0).int32_value());
  EXPECT_EQ(2, set_value.elems(1).int32_value());
  EXPECT_EQ(3, set_value.elems(2).int32_value());
  EXPECT_EQ(4, set_value.elems(3).int32_value());

  //------------------------------------------------------------------------------------------------
  // Testing List type
  //------------------------------------------------------------------------------------------------

  // Create table.
  const char *list_create_stmt =
      "CREATE TABLE list_test"
      "  (id int PRIMARY KEY, v int, ls list<varchar>, c varchar, lb list<blob>);";
  s = processor->Run(list_create_stmt);
  CHECK(s.ok());

  // Insert Values
  std::string list_insert_stmt("INSERT INTO list_test (id, v, ls, c, lb) values "
      "(1, 3, ['c', 'd', 'a', 'b', 'd', 'b'], 'x', [0x01, 0X02, 0x33, 0x04]);");
  s = processor->Run(list_insert_stmt);
  CHECK_OK(s);

  // Check Select
  auto list_select_stmt = "SELECT * FROM list_test WHERE id = 1";
  s = processor->Run(list_select_stmt);
  CHECK(s.ok());
  auto list_row_block = processor->row_block();
  EXPECT_EQ(1, list_row_block->row_count());
  QLRow& list_row = list_row_block->row(0);

  // check row
  EXPECT_EQ(1, list_row.column(0).int32_value());
  EXPECT_EQ(3, list_row.column(1).int32_value());
  EXPECT_EQ("x", list_row.column(3).string_value());
  // check set
  EXPECT_EQ(InternalType::kListValue, list_row.column(2).type());
  EXPECT_EQ(InternalType::kListValue, list_row.column(4).type());

  // check elems
  // lists should preserve input length (keep duplicates if any)
  QLSeqValuePB list_value = list_row.column(2).list_value();
  EXPECT_EQ(6, list_value.elems_size());
  QLSeqValuePB blist_value = list_row.column(4).list_value();
  EXPECT_EQ(4, blist_value.elems_size());
  // list elements should preserve input order
  EXPECT_EQ("c", list_value.elems(0).string_value());
  EXPECT_EQ("d", list_value.elems(1).string_value());
  EXPECT_EQ("a", list_value.elems(2).string_value());
  EXPECT_EQ("b", list_value.elems(3).string_value());
  EXPECT_EQ("d", list_value.elems(4).string_value());
  EXPECT_EQ("b", list_value.elems(5).string_value());

  EXPECT_EQ("\x1", blist_value.elems(0).binary_value());
  EXPECT_EQ("\x2", blist_value.elems(1).binary_value());
  EXPECT_EQ("\x33", blist_value.elems(2).binary_value());
  EXPECT_EQ("\x4", blist_value.elems(3).binary_value());
}

TEST_F(TestQLQuery, TestSystemLocal) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  auto set_select_stmt = "SELECT * FROM system.local";
  CHECK_OK(processor->Run(set_select_stmt));

  // Validate rows.
  auto row_block = processor->row_block();
  EXPECT_EQ(1, row_block->row_count());
  QLRow& row = row_block->row(0);
  EXPECT_EQ("127.0.0.3", row.column(2).inetaddress_value().ToString()); // broadcast address.
}

TEST_F(TestQLQuery, TestSystemTablesWithRestart) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  // Verify system table query works.
  ASSERT_OK(processor->Run("SELECT * FROM system.peers"));

  // Restart the cluster.
  ASSERT_OK(cluster_->RestartSync());

  // Verify system table query still works.
  ASSERT_OK(processor->Run("SELECT * FROM system.peers"));
}

TEST_F(TestQLQuery, TestInvalidPeerTableEntries) {
  // Init the simulated cluster and wait for tservers.
  int num_tservers = 3;
  ASSERT_NO_FATALS(CreateSimulatedCluster(num_tservers));
  ASSERT_OK(cluster_->WaitForTabletServerCount(num_tservers));

  // Verify system peers table.
  TestQLProcessor* processor = GetQLProcessor();
  ASSERT_OK(processor->Run("SELECT * FROM system.peers"));
  std::shared_ptr<QLRowBlock> row_block = processor->row_block();
  ASSERT_EQ(num_tservers - 1, row_block->row_count()) << row_block->ToString();

  auto ts_manager = cluster_->leader_mini_master()->master()->ts_manager();
  NodeInstancePB instance;
  instance.set_permanent_uuid("test");
  instance.set_instance_seqno(0);

  // Use an invalid hostname for registration.
  master::TSRegistrationPB registration;
  auto hostport_pb = registration.mutable_common()->add_private_rpc_addresses();
  const string invalid_host = "randomhost";
  hostport_pb->set_host(invalid_host);
  hostport_pb->set_port(123);

  ASSERT_OK(ts_manager->RegisterTS(instance, registration, CloudInfoPB(), nullptr));

  // Verify the peers table and ensure the invalid host is not present.
  ASSERT_OK(processor->Run("SELECT * FROM system.peers"));
  row_block = processor->row_block();
  ASSERT_EQ(num_tservers - 1, row_block->row_count()) << row_block->ToString();
  for (const auto& row : row_block->rows()) {
    ASSERT_NE(invalid_host, row.column(0).inetaddress_value().ToString());
  }
}

TEST_F(TestQLQuery, TestPagination) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  // Create test table.
  CHECK_OK(processor->Run("CREATE TABLE page_test (c int PRIMARY KEY);"));

  // Insert 10 different hash keys. They should go to different tablets.
  for (int i = 1; i <= 10; i++) {
    string stmt = Substitute("INSERT INTO page_test (c) VALUES ($0);", i);
    CHECK_OK(processor->Run(stmt));
  }

  // Do full-table query. All rows should be returned in one block.
  CHECK_VALID_STMT("SELECT * FROM page_test;");

  auto row_block = processor->row_block();
  EXPECT_EQ(10, row_block->row_count());
  int sum = 0;
  for (int i = 0; i < row_block->row_count(); i++) {
    sum += row_block->row(i).column(0).int32_value();
  }
  EXPECT_EQ(55, sum);
}

TEST_F(TestQLQuery, TestTokenBcall) {
  TestPartitionHash("token");
}

TEST_F(TestQLQuery, TestPartitionHashBcall) {
  TestPartitionHash("partition_hash");
}

TEST_F(TestQLQuery, TestScanWithBoundsToken) {
  TestScanWithBoundsPartitionOps("token", std::numeric_limits<int64_t>::max(),
                                 std::numeric_limits<int64_t>::min());
}

TEST_F(TestQLQuery, TestScanWithBoundsPartitionHash) {
  TestScanWithBoundsPartitionOps("partition_hash", std::numeric_limits<uint16_t>::max(),
                                 std::numeric_limits<uint16_t>::min());
  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();
  CHECK_INVALID_STMT("SELECT * FROM scan_bounds_test WHERE partition_hash(h1, h2) > 65536");
  CHECK_INVALID_STMT("SELECT * FROM scan_bounds_test WHERE partition_hash(h1, h2) > -1");

  // Test mix of partition hash and token
  CHECK_INVALID_STMT(
      "SELECT * FROM scan_bounds_test WHERE partition_hash(h1,h2) > 0 AND token(h1,h2) >= 0");
  // Two "less-than" bounds
  CHECK_INVALID_STMT(
      "SELECT * FROM scan_bounds_test WHERE partition_hash(h1,h2) <= 0 AND token(h1,h2) < 0");
  // Two "equal" conditions
  CHECK_INVALID_STMT(
      "SELECT * FROM scan_bounds_test WHERE token(h1,h2) = 0 AND partition_hash(h1,h2) = 0");
  // Both "equal" and "less than" conditions
  CHECK_INVALID_STMT(
      "SELECT * FROM scan_bounds_test WHERE token(h1,h2) = 0 AND partition_hash(h1,h2) <= 0");
  // Both "equal" and "greater than" conditions
  CHECK_INVALID_STMT(
      "SELECT * FROM scan_bounds_test WHERE partition_hash(h1,h2) = 0 AND token(h1,h2) >= 0");
}

TEST_F(TestQLQuery, TestInvalidGrammar) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  CHECK_INVALID_STMT("SELECT;");
  CHECK_INVALID_STMT("SELECT 1;");
  CHECK_INVALID_STMT("SELECT count ;");
  CHECK_INVALID_STMT("SELECT \n   \n;");
  CHECK_INVALID_STMT("SELECT \n  \n from \n \n \n ;");
  CHECK_INVALID_STMT("SELECT * \n \n \n  from \n \n ;");
  CHECK_INVALID_STMT("SELECT * from \"long \n  multiline table  name wi\nth  spaces   \"  \n ;");
}

TEST_F(TestQLQuery, TestDeleteColumn) {
  //------------------------------------------------------------------------------------------------
  // Setting up cluster
  //------------------------------------------------------------------------------------------------

  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  TestQLProcessor *processor = GetQLProcessor();

  // Create test table.
  CHECK_OK(processor->Run("CREATE TABLE delete_column (h int, v1 int, v2 int,"
                            " PRIMARY KEY(h));"));

  client::YBTableName name(YQL_DATABASE_CQL, kDefaultKeyspaceName, "delete_column");

  for (int i = 0; i < 2; i++) {
    string stmt = Substitute("INSERT INTO delete_column (h, v1, v2) VALUES "
                               "($0, $1, $2);", i, i, i);
    CHECK_OK(processor->Run(stmt));
  }

  // Deleting the value
  string delete_stmt = "DELETE v1 FROM delete_column WHERE h = 0";
  CHECK_OK(processor->Run(delete_stmt));

  string select_stmt_template = "SELECT $0 FROM delete_column WHERE h = 0";
  // Check that v1 is null
  CHECK_OK(processor->Run(Substitute(select_stmt_template, "v1")));
  auto row_block = processor->row_block();
  ASSERT_EQ(1, row_block->row_count());
  const QLRow &row1 = row_block->row(0);
  EXPECT_TRUE(row1.column(0).IsNull());

  // Check that v2 is 0
  CHECK_OK(processor->Run(Substitute(select_stmt_template, "v2")));
  row_block = processor->row_block();
  ASSERT_EQ(1, row_block->row_count());
  const QLRow &row2 = row_block->row(0);
  EXPECT_EQ(0, row2.column(0).int32_value());

  // Check that primary keys and * cannot be deleted
  CHECK_INVALID_STMT("DELETE * FROM delete_column WHERE h = 0;");
  CHECK_INVALID_STMT("DELETE h FROM delete_column WHERE h = 0;");
}

TEST_F(TestQLQuery, TestTtlWritetimeInWhereClauseOfSelectStatements) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  TestQLProcessor *processor = GetQLProcessor();

  CHECK_OK(
      processor->Run("CREATE TABLE ttl_writetime_test (h int, v1 int, v2 int, PRIMARY KEY(h))"));

  client::YBTableName name(YQL_DATABASE_CQL, kDefaultKeyspaceName, "ttl_writetime_test");

  shared_ptr<client::YBTable> table;

  ASSERT_OK(client_->OpenTable(name, &table));

  // generating input data with hash_code and inserting into table
  std::vector<std::tuple<int, int, int>> rows;
  for (int i = 0; i < 5; i++) {
    std::string i_str = std::to_string(i);
    YBPartialRow row(&table->InternalSchema());
    CHECK_OK(row.SetInt32(0, i));
    CHECK_OK(row.SetInt32(1, i));
    CHECK_OK(row.SetInt32(2, i));
    std::tuple<int, int, int> values(i, i, i);
    rows.push_back(values);
    string stmt = Substitute("INSERT INTO ttl_writetime_test (h, v1, v2) VALUES "
                               "($0, $1, $2) using ttl 100;", i, i, i);
    CHECK_OK(processor->Run(stmt));
  }

  for (int i = 5; i < 10; i++) {
    std::string i_str = std::to_string(i);
    YBPartialRow row(&table->InternalSchema());
    CHECK_OK(row.SetInt32(0, i));
    CHECK_OK(row.SetInt32(1, i));
    CHECK_OK(row.SetInt32(2, i));
    std::tuple<int, int, int> values(i, i, i);
    rows.push_back(values);
    string stmt = Substitute("INSERT INTO ttl_writetime_test (h, v1, v2) VALUES "
                               "($0, $1, $2) using ttl 200;", i, i, i);
    CHECK_OK(processor->Run(stmt));
  }

  std::sort(rows.begin(), rows.end(),
            [](const std::tuple<int, int, int>& r1,
               const std::tuple<int, int, int>& r2) -> bool {
              return std::get<0>(r1) < std::get<0>(r2);
            });


  // test that for ttl > 150, there are 5 elements that match what we expect
  string select_stmt_template = "SELECT * FROM ttl_writetime_test WHERE ttl($0) $1 $2";

  string select_stmt = Substitute(select_stmt_template, "v1", "<", 150);
  CHECK_OK(processor->Run(select_stmt));
  auto row_block = processor->row_block();
  std::vector<QLRow>& returned_rows_1 = row_block->rows();
  std::sort(returned_rows_1.begin(), returned_rows_1.end(),
            [](const QLRow &r1,
               const QLRow &r2) -> bool {
              return r1.column(0).int32_value() < r2.column(0).int32_value();
            });
  // checking result
  ASSERT_EQ(5, row_block->row_count());
  for (int i = 0; i < 5; i++) {
    QLRow &row = returned_rows_1.at(i);
    EXPECT_EQ(std::get<0>(rows[i]), row.column(0).int32_value());
    EXPECT_EQ(std::get<1>(rows[i]), row.column(1).int32_value());
    EXPECT_EQ(std::get<2>(rows[i]), row.column(2).int32_value());
  }

  // Now, let us test that when we update a column with a new ttl, that it shows up
  // Should update 2 entries
  string update_stmt = "UPDATE ttl_writetime_test using ttl 300 set v1 = 7 where h = 7;";
  CHECK_OK(processor->Run(update_stmt));
  update_stmt = "UPDATE ttl_writetime_test using ttl 300 set v1 = 8 where h = 8;";
  CHECK_OK(processor->Run(update_stmt));
  select_stmt = Substitute(select_stmt_template, "v1", ">", 250);
  CHECK_OK(processor->Run(select_stmt));
  row_block = processor->row_block();
  std::vector<QLRow>& returned_rows_2 = row_block->rows();
  std::sort(returned_rows_2.begin(), returned_rows_2.end(),
            [](const QLRow &r1,
               const QLRow &r2) -> bool {
              return r1.column(0).int32_value() < r2.column(0).int32_value();
            });
  // checking result
  ASSERT_EQ(2, row_block->row_count());
  for (int i = 7; i < 9; i++) {
    QLRow &row = returned_rows_2.at(i - 7);
    EXPECT_EQ(std::get<0>(rows[i]), row.column(0).int32_value());
    EXPECT_EQ(std::get<1>(rows[i]), row.column(1).int32_value());
    EXPECT_EQ(std::get<2>(rows[i]), row.column(2).int32_value());
  }

  select_stmt = Substitute(select_stmt_template, "v2", ">", 250);
  CHECK_OK(processor->Run(select_stmt));
  row_block = processor->row_block();
  // checking result
  ASSERT_EQ(0, row_block->row_count());
}

TEST_F(TestQLQuery, TestFloatPrimaryKey) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  float my_keys[] = {0.0f, 1.5f, 1.5001f, 1.51f, 2.0f};

  const string create_stmt = "CREATE TABLE float_key (a float, b int, primary key (a))";
  string insert_template = "INSERT INTO float_key (a, b) VALUES ($0, $1);";
  string select_template = "SELECT $0 FROM float_key where a = $1;";

  CHECK_OK(processor->Run(create_stmt));
  for (int i = 0; i < 5; i++) {
    LOG(INFO) << "Inserting " << my_keys[i];
    CHECK_OK(processor->Run(Substitute(insert_template, my_keys[i], i)));
  }

  for (int i = 0; i < 5; i++) {
    LOG(INFO) << "Selecting " << my_keys[i];
    CHECK_OK(processor->Run(Substitute(select_template, "*", my_keys[i])));
    auto row_block = processor->row_block();
    ASSERT_EQ(1, row_block->row_count());
    std::vector<QLRow> &returned_rows = row_block->rows();
    QLRow &row = returned_rows.at(0);
    EXPECT_EQ(my_keys[i], row.column(0).float_value());
    EXPECT_EQ(i, row.column(1).int32_value());
  }
}

TEST_F(TestQLQuery, TestDoublePrimaryKey) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();

  double my_keys[] = {0.0l, 1.5l, 1.5000000001l, 1.51l, 2.0l};
  // Assert that the two numbers are equal as floats but not equal as doubles.
  ASSERT_EQ(1.5f, 1.5000000001f);
  ASSERT_LT(1.5l, 1.5000000001l);

  const string create_stmt = "CREATE TABLE double_key (a double, b int, primary key (a))";
  string insert_template = "INSERT INTO double_key (a, b) VALUES ($0, $1);";
  string select_template = "SELECT $0 FROM double_key where a = $1;";

  CHECK_OK(processor->Run(create_stmt));
  for (int i = 0; i < 5; i++) {
    LOG(INFO) << "Inserting " << my_keys[i];
    CHECK_OK(processor->Run(Substitute(insert_template, my_keys[i], i)));
  }

  for (int i = 0; i < 5; i++) {
    LOG(INFO) << "Selecting " << my_keys[i];
    CHECK_OK(processor->Run(Substitute(select_template, "*", my_keys[i])));
    auto row_block = processor->row_block();
    ASSERT_EQ(1, row_block->row_count());
    std::vector<QLRow> &returned_rows = row_block->rows();
    QLRow &row = returned_rows.at(0);
    EXPECT_EQ(my_keys[i], row.column(0).double_value());
    EXPECT_EQ(i, row.column(1).int32_value());
  }
}


void verifyJson(std::shared_ptr<QLRowBlock> row_block) {
  // Verify.
  ASSERT_EQ(1, row_block->row_count());
  std::vector<QLRow> &returned_rows = row_block->rows();
  QLRow &row = returned_rows.at(0);
  // All spaces are removed by rapidjson writer.
  string json;
  common::Jsonb jsonb(row.column(1).jsonb_value());
  ASSERT_OK(jsonb.ToJsonString(&json));
  EXPECT_EQ("{\"a\":1,\"b\":2}", json);
  faststring buffer;
  row.column(1).Serialize(QLType::Create(DataType::JSONB), YQL_CLIENT_CQL, &buffer);
  int32_t len = 0;
  Slice data(buffer);
  ASSERT_OK(CQLDecodeNum(sizeof(len), NetworkByteOrder::Load32, &data, &len));
  string val;
  ASSERT_OK(CQLDecodeBytes(len, &data, &val));
  EXPECT_EQ("{\"a\":1,\"b\":2}", val);
}

TEST_F(TestQLQuery, TestJson) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();
  CHECK_OK(processor->Run("CREATE TABLE test_json (k1 int PRIMARY KEY, data jsonb)"));
  CHECK_OK(processor->Run(
      "INSERT INTO test_json (k1, data) values (1, '{ \"a\" : 1, \"b\" : 2 }')"));
  CHECK_OK(processor->Run("SELECT * FROM test_json"));
  verifyJson(processor->row_block());

  CHECK_OK(processor->Run("SELECT * FROM test_json WHERE k1=1 AND data='{\"a\":1,\"b\":2}'"));
  verifyJson(processor->row_block());

  CHECK_OK(processor->Run("SELECT * FROM test_json WHERE k1=1 AND data='{\"a\" : 1,\"b\" : 2}'"));
  verifyJson(processor->row_block());

  CHECK_OK(processor->Run("SELECT * FROM test_json WHERE k1=1 AND data='{\"b\" : 2,\"a\" : 1}'"));
  verifyJson(processor->row_block());

  CHECK_OK(processor->Run("SELECT * FROM test_json WHERE k1=1 AND data->'a' = '1'"));
  verifyJson(processor->row_block());

  CHECK_OK(processor->Run("SELECT * FROM test_json WHERE k1=1 AND data->>'a' = '1'"));
  verifyJson(processor->row_block());

  ASSERT_NOK(processor->Run("CREATE TABLE test_json1 (k1 jsonb PRIMARY KEY, data jsonb)"));
  ASSERT_NOK(processor->Run("CREATE TABLE test_json2 (h1 int, r1 jsonb, c1 int, PRIMARY KEY ((h1)"
                                ", r1))"));
}

TEST_F(TestQLQuery, TestJsonUpdate) {
  // Init the simulated cluster.
  ASSERT_NO_FATALS(CreateSimulatedCluster());

  // Get a processor.
  TestQLProcessor *processor = GetQLProcessor();
  ASSERT_OK(processor->Run("CREATE TABLE test_json (k1 int PRIMARY KEY, data jsonb)"));
  ASSERT_OK(processor->Run(
      "INSERT INTO test_json (k1, data) values (1, '{ \"a\" : 1, \"b\" : 2 }')"));
  ASSERT_OK(processor->Run("SELECT * FROM test_json"));
  verifyJson(processor->row_block());

  ASSERT_OK(processor->Run("UPDATE test_json SET data->'a' = '100' WHERE k1 = 1"));
  ASSERT_NOK(processor->Run("UPDATE test_json SET data->'new-field'->'c' = '100' WHERE k1 = 1"));
  ASSERT_OK(processor->Run("UPDATE test_json SET data->'new-field' = '100' WHERE k1 = 1"));

  ASSERT_OK(processor->Run("UPDATE test_json SET data =  '{ \"a\": 2, \"b\": 4 }' WHERE k1 = 2"));
  ASSERT_NOK(processor->Run("UPDATE test_json SET data->'a' = '3' WHERE k1 = 3"));

  // Setting primitive value in JSON column should work
  ASSERT_OK(processor->Run("UPDATE test_json SET data='true' WHERE k1 = 1"));
  // Trying to update primitive value in JSON column using field name should error out
  ASSERT_NOK(processor->Run("UPDATE test_json SET data->'a' WHERE k1 = 1"));
}

} // namespace ql
} // namespace yb
