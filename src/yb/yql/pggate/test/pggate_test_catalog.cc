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

#include <chrono>

#include "yb/yql/pggate/test/pggate_test.h"
#include "yb/common/ybc-internal.h"

using namespace std::chrono_literals;

namespace yb {
namespace pggate {

class PggateTestCatalog : public PggateTest {
};

TEST_F(PggateTestCatalog, TestDml) {
  CHECK_OK(Init("TestDml"));

  const char *tabname = "basic_table";
  const YBCPgOid tab_oid = 2;
  YBCPgStatement pg_stmt;

  // Create table in the connected database.
  int col_count = 0;
  CHECK_YBC_STATUS(YBCPgNewCreateTable(kDefaultDatabase, "pg_catalog", tabname,
                                       kDefaultDatabaseOid, tab_oid,
                                       false /* is_shared_table */, true /* if_not_exist */,
                                       false /* add_primary_key */, true /* colocated */,
                                       kInvalidOid /*tablegroup_id*/, &pg_stmt));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "company_id", ++col_count,
                                             DataType::INT64, false, true));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "empid", ++col_count,
                                             DataType::INT32, false, true));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "dependent_count", ++col_count,
                                             DataType::INT16, false, false));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "project_count", ++col_count,
                                             DataType::INT32, false, false));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "salary", ++col_count,
                                             DataType::FLOAT, false, false));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "job", ++col_count,
                                             DataType::STRING, false, false));
  CHECK_YBC_STATUS(YBCPgExecCreateTable(pg_stmt));
  pg_stmt = nullptr;

  // INSERT ----------------------------------------------------------------------------------------
  // Allocate new insert.
  CHECK_YBC_STATUS(YBCPgNewInsert(kDefaultDatabaseOid, tab_oid,
                                  false /* is_single_row_txn */, &pg_stmt));

  // Allocate constant expressions.
  // TODO(neil) We can also allocate expression with bind.
  int seed = 1;
  YBCPgExpr expr_compid;
  CHECK_YBC_STATUS(YBCTestNewConstantInt8(pg_stmt, 0, false, &expr_compid));
  YBCPgExpr expr_empid;
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, seed, false, &expr_empid));
  YBCPgExpr expr_depcnt;
  CHECK_YBC_STATUS(YBCTestNewConstantInt2(pg_stmt, seed, false, &expr_depcnt));
  YBCPgExpr expr_projcnt;
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, 100 + seed, false, &expr_projcnt));
  YBCPgExpr expr_salary;
  CHECK_YBC_STATUS(YBCTestNewConstantFloat4(pg_stmt, seed + 1.0*seed/10.0, false, &expr_salary));
  YBCPgExpr expr_job;
  string job = strings::Substitute("Job_title_$0", seed);
  CHECK_YBC_STATUS(YBCTestNewConstantText(pg_stmt, job.c_str(), false, &expr_job));

  // Set column value to be inserted.
  int attr_num = 0;
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_compid));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_empid));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_depcnt));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_projcnt));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_salary));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_job));
  CHECK_EQ(attr_num, col_count);

  const int kInsertRowCount = 7;
  for (int i = 0; i < kInsertRowCount; i++) {
    // Insert the row with the original seed.
    CHECK_YBC_STATUS(YBCPgExecInsert(pg_stmt));
    CommitTransaction();

    // Update the constant expresions to insert the next row.
    // TODO(neil) When we support binds, we can also call UpdateBind here.
    seed++;
    YBCPgUpdateConstInt4(expr_empid, seed, false);
    YBCPgUpdateConstInt2(expr_depcnt, seed, false);
    YBCPgUpdateConstInt4(expr_projcnt, 100 + seed, false);
    YBCPgUpdateConstFloat4(expr_salary, seed + 1.0*seed/10.0, false);
    job = strings::Substitute("Job_title_$0", seed);
    YBCPgUpdateConstChar(expr_job, job.c_str(), job.size(), false);
  }

  pg_stmt = nullptr;

  // SELECT ----------------------------------------------------------------------------------------
  LOG(INFO) << "Test SELECTing from non-partitioned table WITH RANGE values";
  CHECK_YBC_STATUS(YBCPgNewSelect(kDefaultDatabaseOid, tab_oid,
                                  NULL /* prepare_params */, &pg_stmt));

  // Specify the selected expressions.
  YBCPgExpr colref;
  YBCTestNewColumnRef(pg_stmt, 1, DataType::INT64, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 2, DataType::INT32, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 3, DataType::INT16, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 4, DataType::INT32, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 5, DataType::FLOAT, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 6, DataType::STRING, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));

  // Set partition and range columns for SELECT to select a specific row.
  // SELECT ... WHERE compid = 0 AND empid = 1.
  CHECK_YBC_STATUS(YBCTestNewConstantInt8(pg_stmt, 0, false, &expr_compid));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, 1, expr_compid));
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, 1, false, &expr_empid));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, 2, expr_empid));

  // Execute select statement.
  CHECK_YBC_STATUS(YBCPgExecSelect(pg_stmt, nullptr /* exec_params */));

  // Fetching rows and check their contents.
  uint64_t *values = static_cast<uint64_t*>(YBCPAlloc(col_count * sizeof(uint64_t)));
  bool *isnulls = static_cast<bool*>(YBCPAlloc(col_count * sizeof(bool)));
  int select_row_count = 0;
  bool has_data = false;
  YBCPgDmlFetch(pg_stmt, col_count, values, isnulls, nullptr, &has_data);
  CHECK(has_data);

  // Print result
  LOG(INFO) << "ROW: "
            << "compid = " << values[0]
            << ", empid = " << values[1]
            << ", dependent count = " << values[2]
            << ", project count = " << values[3]
            << ", salary = " << *reinterpret_cast<float*>(&values[4])
            << ", job = (" << reinterpret_cast<char*>(values[5]) << ")";

  // Check result.
  int col_index = 0;
  CHECK_EQ(values[col_index++], 0);  // compid : int64
  int32_t empid = values[col_index++];  // empid : int32
  CHECK_EQ(empid, 1) << "Unexpected result for compid column";
  CHECK_EQ(values[col_index++], empid);  // dependent_count : int16
  CHECK_EQ(values[col_index++], 100 + empid);  // project_count : int32

  float salary = *reinterpret_cast<float*>(&values[col_index++]); // salary : float
  CHECK_LE(salary, empid + 1.0*empid/10.0 + 0.01);
  CHECK_GE(salary, empid + 1.0*empid/10.0 - 0.01);

  string selected_job_name = reinterpret_cast<char*>(values[col_index++]);
  string expected_job_name = strings::Substitute("Job_title_$0", empid);
  CHECK_EQ(selected_job_name, expected_job_name);

  YBCPgDmlFetch(pg_stmt, col_count, values, isnulls, nullptr, &has_data);
  CHECK(!has_data);

  pg_stmt = nullptr;

  // SELECT ----------------------------------------------------------------------------------------
  LOG(INFO) << "Test SELECTing from non-partitioned table WITHOUT RANGE values";
  CHECK_YBC_STATUS(YBCPgNewSelect(kDefaultDatabaseOid, tab_oid,
                                  NULL /* prepare_params */, &pg_stmt));

  // Specify the selected expressions.
  YBCTestNewColumnRef(pg_stmt, 1, DataType::INT64, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 2, DataType::INT32, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 3, DataType::INT16, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 4, DataType::INT32, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 5, DataType::FLOAT, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 6, DataType::STRING, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));

  // Execute select statement.
  CHECK_YBC_STATUS(YBCPgExecSelect(pg_stmt, nullptr /* exec_params */));

  // Fetching rows and check their contents.
  values = static_cast<uint64_t*>(YBCPAlloc(col_count * sizeof(uint64_t)));
  isnulls = static_cast<bool*>(YBCPAlloc(col_count * sizeof(bool)));
  for (int i = 0; i < kInsertRowCount; i++) {
    bool has_data = false;
    YBCPgDmlFetch(pg_stmt, col_count, values, isnulls, nullptr, &has_data);
    CHECK(has_data) << "Not all inserted rows have been fetched: only "
                    << i << " rows fetched out of " << kInsertRowCount;

    // Print result
    LOG(INFO) << "ROW " << i << ": "
              << "compid = " << values[0]
              << ", empid = " << values[1]
              << ", dependent count = " << values[2]
              << ", project count = " << values[3]
              << ", salary = " << *reinterpret_cast<float*>(&values[4])
              << ", job = (" << values[5] << ")";

    // Check result.
    int col_index = 0;
    CHECK_EQ(values[col_index++], 0);  // compid : int64
    int32_t empid = values[col_index++];  // empid : int32
    CHECK_EQ(values[col_index++], empid);  // dependent_count : int16
    CHECK_EQ(values[col_index++], 100 + empid);  // project_count : int32

    float salary = *reinterpret_cast<float*>(&values[col_index++]); // salary : float
    CHECK_LE(salary, empid + 1.0*empid/10.0 + 0.01);
    CHECK_GE(salary, empid + 1.0*empid/10.0 - 0.01);

    string selected_job_name = reinterpret_cast<char*>(values[col_index++]);
    string expected_job_name = strings::Substitute("Job_title_$0", empid);
    CHECK_EQ(selected_job_name, expected_job_name);
  }

  pg_stmt = nullptr;

  // UPDATE ----------------------------------------------------------------------------------------
  // Allocate new update.
  CHECK_YBC_STATUS(YBCPgNewUpdate(kDefaultDatabaseOid, tab_oid,
                                  false /* is_single_row_txn */, &pg_stmt));

  // Allocate constant expressions.
  // TODO(neil) We can also allocate expression with bind.
  seed = 1;
  CHECK_YBC_STATUS(YBCTestNewConstantInt8(pg_stmt, 0, false, &expr_compid));
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, seed, false, &expr_empid));
  CHECK_YBC_STATUS(YBCTestNewConstantInt2(pg_stmt, 77 + seed, false, &expr_depcnt));
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, 77 + 100 + seed, false, &expr_projcnt));
  CHECK_YBC_STATUS(YBCTestNewConstantFloat4(pg_stmt, 77 + seed + 1.0*seed/10.0, false,
                                            &expr_salary));
  job = strings::Substitute("Job_title_$0", seed + 77);
  CHECK_YBC_STATUS(YBCTestNewConstantText(pg_stmt, job.c_str(), false, &expr_job));

  attr_num = 0;
  // Specify the row we want to update by binding primary columns.
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_compid));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_empid));

  // Specify new values for the columns we want to update.
  CHECK_YBC_STATUS(YBCPgDmlAssignColumn(pg_stmt, ++attr_num, expr_depcnt));
  CHECK_YBC_STATUS(YBCPgDmlAssignColumn(pg_stmt, ++attr_num, expr_projcnt));
  CHECK_YBC_STATUS(YBCPgDmlAssignColumn(pg_stmt, ++attr_num, expr_salary));
  CHECK_YBC_STATUS(YBCPgDmlAssignColumn(pg_stmt, ++attr_num, expr_job));
  CHECK_EQ(attr_num, col_count);

  // UPDATE all of odd rows.
  const int kOddEmpIdRowCount = (kInsertRowCount + 1) / 2;
  LOG(INFO) << "Updating " << kOddEmpIdRowCount << " rows with odd empid values";
  for (int i = 0; i < kOddEmpIdRowCount; i++) {
    LOG(INFO) << "Updating row with empid=" << seed;
    // Update the row with the original seed.
    CHECK_YBC_STATUS(YBCPgExecUpdate(pg_stmt));
    CommitTransaction();

    // Update the constant expresions to update the next row.
    // TODO(neil) When we support binds, we can also call UpdateBind here.
    seed = seed + 2;
    YBCPgUpdateConstInt4(expr_empid, seed, false);
    YBCPgUpdateConstInt2(expr_depcnt, 77 + seed, false);
    YBCPgUpdateConstInt4(expr_projcnt, 77 + 100 + seed, false);
    YBCPgUpdateConstFloat4(expr_salary, 77 + seed + 1.0*seed/10.0, false);
    job = strings::Substitute("Job_title_$0", 77 + seed);
    YBCPgUpdateConstChar(expr_job, job.c_str(), job.size(), false);
  }

  pg_stmt = nullptr;

  // SELECT ----------------------------------------------------------------------------------------
  LOG(INFO) << "Test SELECTing from non-partitioned table";
  CHECK_YBC_STATUS(YBCPgNewSelect(kDefaultDatabaseOid, tab_oid,
                                  NULL /* prepare_params */, &pg_stmt));

  // Specify the selected expressions.
  YBCTestNewColumnRef(pg_stmt, 1, DataType::INT64, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 2, DataType::INT32, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 3, DataType::INT16, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 4, DataType::INT32, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 5, DataType::FLOAT, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 6, DataType::STRING, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));

  // Execute select statement.
  CHECK_YBC_STATUS(YBCPgExecSelect(pg_stmt, nullptr /* exec_params */));

  // Fetching rows and checking their contents.
  select_row_count = 0;
  for (int i = 0; i < kInsertRowCount; i++) {
    bool has_data = false;
    YBCPgDmlFetch(pg_stmt, col_count, values, isnulls, nullptr, &has_data);
    if (!has_data) {
      break;
    }
    select_row_count++;

    // Print result
    LOG(INFO) << "ROW " << select_row_count << ": "
              << "compid = " << values[0]
              << ", empid = " << values[1]
              << ", dependent count = " << values[2]
              << ", project count = " << values[3]
              << ", salary = " << *reinterpret_cast<float*>(&values[4])
              << ", job = (" << reinterpret_cast<char*>(values[5]) << ")";

    // Check result.
    int col_index = 0;
    int32_t compid = values[col_index++];  // id : int32
    int32_t empid = values[col_index++];  // empid : int32
    CHECK_EQ(compid, 0);
    if (empid%2 == 0) {
      // Check if EVEN rows stays the same as inserted.
      CHECK_EQ(values[col_index++], empid);  // dependent_count : int16
      CHECK_EQ(values[col_index++], 100 + empid);  // project_count : int32

      // salary : float
      float salary = *reinterpret_cast<float*>(&values[col_index++]);
      CHECK_LE(salary, empid + 1.0*empid/10.0 + 0.01);
      CHECK_GE(salary, empid + 1.0*empid/10.0 - 0.01);

      string selected_job_name = reinterpret_cast<char*>(values[col_index++]);
      string expected_job_name = strings::Substitute("Job_title_$0", empid);
      CHECK_EQ(selected_job_name, expected_job_name);

    } else {
      // Check if ODD rows have been updated.
      CHECK_EQ(values[col_index++], 77 + empid);  // dependent_count : int16
      CHECK_EQ(values[col_index++], 77 + 100 + empid);  // project_count : int32

      // salary : float
      float salary = *reinterpret_cast<float*>(&values[col_index++]);
      CHECK_LE(salary, 77 + empid + 1.0*empid/10.0 + 0.01);
      CHECK_GE(salary, 77 + empid + 1.0*empid/10.0 - 0.01);

      string selected_job_name = reinterpret_cast<char*>(values[col_index++]);
      string expected_job_name = strings::Substitute("Job_title_$0", 77 + empid);
      CHECK_EQ(selected_job_name, expected_job_name);
    }
  }
  CHECK_EQ(select_row_count, kInsertRowCount) << "Unexpected row count";

  pg_stmt = nullptr;
}

TEST_F(PggateTestCatalog, TestCopydb) {
  CHECK_OK(Init("TestCopydb"));

  const char *tabname = "basic_table";
  const char *copy_db_name = "pggate_test_copy";
  const YBCPgOid copy_db_oid = 101;
  const YBCPgOid tab_oid = 2;
  YBCPgStatement pg_stmt;

  // Create sys catalog table in default database.
  LOG(INFO) << "Create database with source database";
  CHECK_YBC_STATUS(YBCPgNewCreateTable(kDefaultDatabase, "pg_catalog", tabname,
                                       kDefaultDatabaseOid, tab_oid,
                                       false /* is_shared_table */, true /* if_not_exist */,
                                       false /* add_primary_key */, true /* colocated */,
                                       kInvalidOid /*tablegroup_id*/, &pg_stmt));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "key", 1, DataType::INT32, false, true));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "value", 2, DataType::INT32, false, false));
  CHECK_YBC_STATUS(YBCPgExecCreateTable(pg_stmt));
  pg_stmt = nullptr;

  CHECK_YBC_STATUS(YBCPgNewInsert(kDefaultDatabaseOid, tab_oid,
                                  false /* is_single_row_txn */, &pg_stmt));

  YBCPgExpr expr_key;
  YBCPgExpr expr_value;
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, 0, false, &expr_key));
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, 10, false, &expr_value));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, 1, expr_key));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, 2, expr_value));

  const int kInsertRowCount = 7;
  for (int i = 0; i < kInsertRowCount; i++) {
    // Insert the row with the original seed.
    CHECK_YBC_STATUS(YBCPgExecInsert(pg_stmt));
    CommitTransaction();

    YBCPgUpdateConstInt4(expr_key, i+1, false);
    YBCPgUpdateConstInt4(expr_value, i+11, false);
  }
  pg_stmt = nullptr;

  // COPYDB ----------------------------------------------------------------------------------------
  LOG(INFO) << "Create another database from default database";
  CHECK_YBC_STATUS(YBCPgNewCreateDatabase(copy_db_name, copy_db_oid,
                                          kDefaultDatabaseOid, kInvalidOid /* next_oid */,
                                          false /* colocated */,
                                          &pg_stmt));
  CHECK_YBC_STATUS(YBCPgExecCreateDatabase(pg_stmt));
  pg_stmt = nullptr;

  // SELECT ----------------------------------------------------------------------------------------
  LOG(INFO) << "Select from from test table in the new database";
  CHECK_YBC_STATUS(YBCPgNewSelect(copy_db_oid, tab_oid,
                                  NULL /* prepare_params */, &pg_stmt));

  // Specify the selected expressions.
  YBCPgExpr colref;
  YBCTestNewColumnRef(pg_stmt, 1, DataType::INT32, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));
  YBCTestNewColumnRef(pg_stmt, 2, DataType::INT32, &colref);
  CHECK_YBC_STATUS(YBCPgDmlAppendTarget(pg_stmt, colref));

  // Execute select statement.
  CHECK_YBC_STATUS(YBCPgExecSelect(pg_stmt, nullptr /* exec_params */));

  // Fetching rows and check their contents.
  uint64_t *values = static_cast<uint64_t*>(YBCPAlloc(2 * sizeof(uint64_t)));
  bool *isnulls = static_cast<bool*>(YBCPAlloc(2 * sizeof(bool)));
  for (int i = 0; i < kInsertRowCount; i++) {
    bool has_data = false;
    YBCPgDmlFetch(pg_stmt, 2, values, isnulls, nullptr, &has_data);
    CHECK(has_data) << "Not all inserted rows have been fetched";

    // Print result
    LOG(INFO) << "ROW " << i << ": key = " << values[0] << ", value = " << values[1];

    // Check result.
    EXPECT_EQ(values[0], i);  // key : int32
    EXPECT_EQ(values[1], i + 10);  // value : int32
  }

  pg_stmt = nullptr;
}

TEST_F(PggateTestCatalog, TestReserveOids) {
  CHECK_OK(Init("TestReserveOids"));

  // CREATE DATABASE ------------------------------------------------------------------------------
  LOG(INFO) << "Create database";
  const char *db_name = "pggate_reserve_oids";
  const YBCPgOid db_oid = 101;
  YBCPgStatement pg_stmt;

  CHECK_YBC_STATUS(YBCPgNewCreateDatabase(db_name, db_oid,
                                          kInvalidOid /* source_database_oid */,
                                          100 /* next_oid */, false /* colocated */, &pg_stmt));
  CHECK_YBC_STATUS(YBCPgExecCreateDatabase(pg_stmt));
  pg_stmt = nullptr;

  // RESERVE OIDS ---------------------------------------------------------------------------------
  // Request next oid below the initial next oid above. Verify the range returned starts with the
  // initial range still.
  LOG(INFO) << "Reserve oids";
  YBCPgOid begin_oid = 0;
  YBCPgOid end_oid = 0;
  CHECK_YBC_STATUS(YBCPgReserveOids(db_oid, 50 /* next_oid */, 100 /* count */,
                                    &begin_oid, &end_oid));
  EXPECT_EQ(begin_oid, 100);
  EXPECT_EQ(end_oid, 200);
}

} // namespace pggate
} // namespace yb
