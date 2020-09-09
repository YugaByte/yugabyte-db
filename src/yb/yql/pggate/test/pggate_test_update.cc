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

#include "yb/yql/pggate/test/pggate_test.h"
#include "yb/common/ybc-internal.h"

namespace yb {
namespace pggate {

class PggateTestDelete : public PggateTest {
};

TEST_F(PggateTestDelete, TestDelete) {
  CHECK_OK(Init("TestDelete"));

  const char *tabname = "basic_table";
  const YBCPgOid tab_oid = 3;
  YBCPgStatement pg_stmt;

  // Create table in the connected database.
  int col_count = 0;
  CHECK_YBC_STATUS(YBCPgNewCreateTable(kDefaultDatabase, kDefaultSchema, tabname,
                                       kDefaultDatabaseOid, tab_oid,
                                       false /* is_shared_table */, true /* if_not_exist */,
                                       false /* add_primary_key */, true /* colocated */,
                                       kInvalidOid /*tablegroup_id*/, &pg_stmt));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "hash_key", ++col_count,
                                               DataType::INT64, true, true));
  CHECK_YBC_STATUS(YBCTestCreateTableAddColumn(pg_stmt, "id", ++col_count,
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
  YBCPgExpr expr_hash;
  CHECK_YBC_STATUS(YBCTestNewConstantInt8(pg_stmt, seed, false, &expr_hash));
  YBCPgExpr expr_id;
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, seed, false, &expr_id));
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
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_hash));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_id));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_depcnt));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_projcnt));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_salary));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_job));
  CHECK_EQ(attr_num, col_count);

  const int insert_row_count = 7;
  for (int i = 0; i < insert_row_count; i++) {
    // Insert the row with the original seed.
    CHECK_YBC_STATUS(YBCPgExecInsert(pg_stmt));
    CommitTransaction();

    // Update the constant expresions to insert the next row.
    // TODO(neil) When we support binds, we can also call UpdateBind here.
    seed++;
    YBCPgUpdateConstInt8(expr_hash, seed, false);
    YBCPgUpdateConstInt4(expr_id, seed, false);
    YBCPgUpdateConstInt2(expr_depcnt, seed, false);
    YBCPgUpdateConstInt4(expr_projcnt, 100 + seed, false);
    YBCPgUpdateConstFloat4(expr_salary, seed + 1.0*seed/10.0, false);
    job = strings::Substitute("Job_title_$0", seed);
    YBCPgUpdateConstChar(expr_job, job.c_str(), job.size(), false);
  }

  pg_stmt = nullptr;

  // UPDATE ----------------------------------------------------------------------------------------
  // Allocate new update.
  CHECK_YBC_STATUS(YBCPgNewUpdate(kDefaultDatabaseOid, tab_oid,
                                  false /* is_single_row_txn */, &pg_stmt));

  // Allocate constant expressions.
  // TODO(neil) We can also allocate expression with bind.
  seed = 1;
  CHECK_YBC_STATUS(YBCTestNewConstantInt8(pg_stmt, seed, false, &expr_hash));
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, seed, false, &expr_id));
  CHECK_YBC_STATUS(YBCTestNewConstantInt2(pg_stmt, 77 + seed, false, &expr_depcnt));
  CHECK_YBC_STATUS(YBCTestNewConstantInt4(pg_stmt, 77 + 100 + seed, false, &expr_projcnt));
  CHECK_YBC_STATUS(YBCTestNewConstantFloat4(pg_stmt, 77 + seed + 1.0*seed/10.0, false,
                                            &expr_salary));
  job = strings::Substitute("Job_title_$0", seed + 77);
  CHECK_YBC_STATUS(YBCTestNewConstantText(pg_stmt, job.c_str(), false, &expr_job));

  attr_num = 0;
  // Specify the rows we want to update by binding primary columns.
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_hash));
  CHECK_YBC_STATUS(YBCPgDmlBindColumn(pg_stmt, ++attr_num, expr_id));

  // Specify new values for the columns we want to update.
  CHECK_YBC_STATUS(YBCPgDmlAssignColumn(pg_stmt, ++attr_num, expr_depcnt));
  CHECK_YBC_STATUS(YBCPgDmlAssignColumn(pg_stmt, ++attr_num, expr_projcnt));
  CHECK_YBC_STATUS(YBCPgDmlAssignColumn(pg_stmt, ++attr_num, expr_salary));
  CHECK_YBC_STATUS(YBCPgDmlAssignColumn(pg_stmt, ++attr_num, expr_job));
  CHECK_EQ(attr_num, col_count);

  // UPDATE all of odd rows.
  const int update_row_count = (insert_row_count + 1)/ 2;
  for (int i = 0; i < update_row_count; i++) {
    // Update the row with the original seed.
    CHECK_YBC_STATUS(YBCPgExecUpdate(pg_stmt));
    CommitTransaction();

    // Update the constant expresions to update the next row.
    // TODO(neil) When we support binds, we can also call UpdateBind here.
    seed = seed + 2;
    YBCPgUpdateConstInt8(expr_hash, seed, false);
    YBCPgUpdateConstInt4(expr_id, seed, false);
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

  // Execute select statement.
  YBCPgExecSelect(pg_stmt, nullptr /* exec_params */);

  // Fetching rows and check their contents.
  uint64_t *values = static_cast<uint64_t*>(YBCPAlloc(col_count * sizeof(uint64_t)));
  bool *isnulls = static_cast<bool*>(YBCPAlloc(col_count * sizeof(bool)));
  int select_row_count = 0;
  for (int i = 0; i < insert_row_count; i++) {
    bool has_data = false;
    YBCPgDmlFetch(pg_stmt, col_count, values, isnulls, nullptr, &has_data);
    if (!has_data) {
      break;
    }
    select_row_count++;

    // Print result
    LOG(INFO) << "ROW " << select_row_count << ": "
              << "hash_key = " << values[0]
              << ", id = " << values[1]
              << ", dependent count = " << values[2]
              << ", project count = " << values[3]
              << ", salary = " << *reinterpret_cast<float*>(&values[4])
              << ", job = (" << values[5] << ")";

    // Check result.
    int col_index = 0;
    int32_t hash_id = values[col_index++];  // id : int32
    int32_t id = values[col_index++];  // id : int32
    CHECK_EQ(hash_id, id) << "Expect hash and range key share the same value";
    if (id%2 == 0) {
      // Check if EVEN rows stays the same as inserted.
      CHECK_EQ(values[col_index++], id);  // dependent_count : int16
      CHECK_EQ(values[col_index++], 100 + id);  // project_count : int32

      // salary : float
      float salary = *reinterpret_cast<float*>(&values[col_index++]);
      CHECK_LE(salary, id + 1.0*id/10.0 + 0.01);
      CHECK_GE(salary, id + 1.0*id/10.0 - 0.01);

      string selected_job_name = reinterpret_cast<char*>(values[col_index++]);
      string expected_job_name = strings::Substitute("Job_title_$0", id);
      CHECK_EQ(selected_job_name, expected_job_name);

    } else {
      // Check if ODD rows have been updated.
      CHECK_EQ(values[col_index++], 77 + id);  // dependent_count : int16
      CHECK_EQ(values[col_index++], 77 + 100 + id);  // project_count : int32

      // salary : float
      float salary = *reinterpret_cast<float*>(&values[col_index++]);
      CHECK_LE(salary, 77 + id + 1.0*id/10.0 + 0.01);
      CHECK_GE(salary, 77 + id + 1.0*id/10.0 - 0.01);

      string selected_job_name = reinterpret_cast<char*>(values[col_index++]);
      string expected_job_name = strings::Substitute("Job_title_$0", 77 + id);
      CHECK_EQ(selected_job_name, expected_job_name);
    }
  }
  CHECK_EQ(select_row_count, insert_row_count) << "Unexpected row count";

  pg_stmt = nullptr;
}

} // namespace pggate
} // namespace yb
