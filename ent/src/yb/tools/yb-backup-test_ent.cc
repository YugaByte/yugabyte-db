// Copyright (c) YugaByte, Inc.
//
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

// Tests for the yb_backup.py script.

#include "yb/yql/pgwrapper/pg_wrapper_test_base.h"

#include "yb/client/ql-dml-test-base.h"
#include "yb/gutil/strings/split.h"
#include "yb/util/jsonreader.h"
#include "yb/util/random_util.h"
#include "yb/util/subprocess.h"

using std::unique_ptr;
using std::vector;
using strings::Split;

namespace yb {
namespace tools {

namespace helpers {
YB_DEFINE_ENUM(TableOp, (kDropTable)(kKeepTable));
}

class YBBackupTest : public pgwrapper::PgCommandTestBase {
 protected:
  YBBackupTest() : pgwrapper::PgCommandTestBase(false, false) {}

  void SetUp() override {
    pgwrapper::PgCommandTestBase::SetUp();
    ASSERT_OK(CreateClient());
  }

  void DoBeforeTearDown() override {
    if (!tmp_dir_.empty()) {
      LOG(INFO) << "Deleting temporary folder: " << tmp_dir_;
      ASSERT_OK(Env::Default()->DeleteRecursively(tmp_dir_));
    }
  }

  string GetTempDir(const string& subdir) {
    if (tmp_dir_.empty()) {
      EXPECT_OK(Env::Default()->GetTestDirectory(&tmp_dir_));
      tmp_dir_ = JoinPathSegments(
        tmp_dir_, string(CURRENT_TEST_CASE_NAME()) + '_' + RandomHumanReadableString(8));
    }

    return JoinPathSegments(tmp_dir_, subdir);
  }

  Status RunBackupCommand(const vector<string>& args) {
    const HostPort pg_hp = cluster_->pgsql_hostport(0);
    std::stringstream command;
    command << "python3 " << GetToolPath("../../../managed/devops/bin", "yb_backup.py")
            << " --masters " << cluster_->GetMasterAddresses()
            << " --remote_yb_admin_binary=" << GetToolPath("yb-admin")
            << " --remote_ysql_dump_binary=" << GetPgToolPath("ysql_dump")
            << " --remote_ysql_shell_binary=" << GetPgToolPath("ysqlsh")
            << " --ysql_host=" << pg_hp.host()
            << " --ysql_port=" << pg_hp.port()
            << " --storage_type nfs"
            << " --no_ssh"
            << " --no_auto_name";
#if defined(__APPLE__)
    command << " --mac" << " --verbose";
#endif // defined(__APPLE__)
    string backup_cmd;
    for (const auto& a : args) {
      command << " " << a;
      if (a == "create" || a == "restore") {
        backup_cmd = a;
      }
    }

    const auto command_str = command.str();
    LOG(INFO) << "Run tool: " << command_str;
    string output;
    RETURN_NOT_OK(Subprocess::Call(Split(command_str, " "), &output));
    LOG(INFO) << "Tool output: " << output;

    JsonReader r(output);
    RETURN_NOT_OK(r.Init());
    string error;
    Status s = r.ExtractString(r.root(), "error", &error);
    if (s.ok()) {
      LOG(ERROR) << "yb_backup.py error: " << error;
      return STATUS(RuntimeError, "yb_backup.py error", error);
    }

    if (backup_cmd == "create") {
      string url;
      RETURN_NOT_OK(r.ExtractString(r.root(), "snapshot_url", &url));
      LOG(INFO) << "Backup-create operation result - snapshot url: " << url;
    } else if (backup_cmd == "restore") {
      bool result_ok = false;
      RETURN_NOT_OK(r.ExtractBool(r.root(), "success", &result_ok));
      LOG(INFO) << "Backup-restore operation result: " << result_ok;
      if (!result_ok) {
        return STATUS(RuntimeError, "Failed backup restore operation");
      }
    } else {
      return STATUS(InvalidArgument, "Unknown backup command", ToString(args));
    }

    return Status::OK();
  }

  void DoTestYSQLKeyspaceBackup(helpers::TableOp tableOp);

  client::TableHandle table_;
  string tmp_dir_;
};

TEST_F(YBBackupTest, YB_DISABLE_TEST_IN_SANITIZERS(TestYCQLKeyspaceBackup)) {
  client::KeyValueTableTest::CreateTable(
      client::Transactional::kFalse, CalcNumTablets(3), client_.get(), &table_);
  const string& keyspace = table_.name().namespace_name();

  const string backup_dir = GetTempDir("backup");

  ASSERT_OK(RunBackupCommand(
      {"--backup_location", backup_dir, "--keyspace", keyspace, "create"}));
  ASSERT_OK(RunBackupCommand(
      {"--backup_location", backup_dir, "--keyspace", "new_" + keyspace, "restore"}));

  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

void YBBackupTest::DoTestYSQLKeyspaceBackup(helpers::TableOp tableOp) {
  ASSERT_NO_FATALS(CreateTable("CREATE TABLE mytbl (k INT PRIMARY KEY, v TEXT)"));
  ASSERT_NO_FATALS(InsertOneRow("INSERT INTO mytbl (k, v) VALUES (100, 'foo')"));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT k, v FROM mytbl ORDER BY k",
      R"#(
          k  |  v
        -----+-----
         100 | foo
        (1 row)
      )#"
  ));

  const string backup_dir = GetTempDir("backup");

  // There is no YCQL keyspace 'yugabyte'.
  ASSERT_NOK(RunBackupCommand(
      {"--backup_location", backup_dir, "--keyspace", "yugabyte", "create"}));

  ASSERT_OK(RunBackupCommand(
      {"--backup_location", backup_dir, "--keyspace", "ysql.yugabyte", "create"}));

  ASSERT_NO_FATALS(InsertOneRow("INSERT INTO mytbl (k, v) VALUES (200, 'bar')"));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT k, v FROM mytbl ORDER BY k",
      R"#(
          k  |  v
        -----+-----
         100 | foo
         200 | bar
        (2 rows)
      )#"
  ));

  if (tableOp == helpers::TableOp::kDropTable) {
    // Validate that the DB restoration works even if we have deleted tables with the same name.
    ASSERT_NO_FATALS(RunPsqlCommand("DROP TABLE mytbl", "DROP TABLE"));
  }

  // Restore into the original "ysql.yugabyte" YSQL DB.
  ASSERT_OK(RunBackupCommand({"--backup_location", backup_dir, "restore"}));

  // Check the table data.
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT k, v FROM mytbl ORDER BY k",
      R"#(
          k  |  v
        -----+-----
         100 | foo
        (1 row)
      )#"
  ));
}

TEST_F(YBBackupTest, YB_DISABLE_TEST_IN_SANITIZERS(TestYSQLKeyspaceBackup)) {
  DoTestYSQLKeyspaceBackup(helpers::TableOp::kKeepTable);
  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

TEST_F(YBBackupTest, YB_DISABLE_TEST_IN_SANITIZERS(TestYSQLKeyspaceBackupWithDropTable)) {
  DoTestYSQLKeyspaceBackup(helpers::TableOp::kDropTable);
  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

TEST_F(YBBackupTest, YB_DISABLE_TEST_IN_SANITIZERS(TestYSQLRestoreBackupToNewKeyspace)) {
  // Test hash-table.
  ASSERT_NO_FATALS(CreateTable("CREATE TABLE hashtbl(k INT PRIMARY KEY, v TEXT)"));
  // Test single shard range-table.
  ASSERT_NO_FATALS(CreateTable("CREATE TABLE rangetbl(k INT, PRIMARY KEY(k ASC))"));
  // Test table containing serial column.
  ASSERT_NO_FATALS(CreateTable("CREATE TABLE serialtbl(k SERIAL PRIMARY KEY, v TEXT)"));

  ASSERT_NO_FATALS(CreateTable("CREATE TABLE vendors(v_code INT PRIMARY KEY, v_name TEXT)"));
  // Test Index.
  ASSERT_NO_FATALS(CreateIndex("CREATE UNIQUE INDEX ON vendors(v_name)"));
  // Test View.
  ASSERT_NO_FATALS(CreateView("CREATE VIEW vendors_view AS "
                              "SELECT * FROM vendors WHERE v_name = 'foo'"));
  // Test stored procedure.
  ASSERT_NO_FATALS(CreateProcedure(
      "CREATE PROCEDURE proc(n INT) LANGUAGE PLPGSQL AS $$ DECLARE c INT := 0; BEGIN WHILE c < n "
      "LOOP c := c + 1; INSERT INTO vendors (v_code) VALUES(c + 10); END LOOP; END; $$"));

  ASSERT_NO_FATALS(CreateTable("CREATE TABLE items(i_code INT, i_name TEXT, "
                               "price numeric(10,2), PRIMARY KEY(i_code, i_name))"));
  // Test Foreign Key for 1 column and for 2 columns.
  ASSERT_NO_FATALS(CreateTable("CREATE TABLE orders(o_no INT PRIMARY KEY, o_date date, "
                               "v_code INT REFERENCES vendors, i_code INT, i_name TEXT, "
                               "FOREIGN KEY(i_code, i_name) REFERENCES items(i_code, i_name))"));

  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT schemaname, indexname FROM pg_indexes WHERE tablename = 'vendors'",
      R"#(
         schemaname |     indexname
        ------------+--------------------
         public     | vendors_pkey
         public     | vendors_v_name_idx
        (2 rows)
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "\\d vendors_view",
      R"#(
                       View "public.vendors_view"
         Column |  Type   | Collation | Nullable | Default
        --------+---------+-----------+----------+---------
         v_code | integer |           |          |
         v_name | text    |           |          |
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "\\d orders",
      R"#(
                       Table "public.orders"
         Column |  Type   | Collation | Nullable | Default
        --------+---------+-----------+----------+---------
         o_no   | integer |           | not null |
         o_date | date    |           |          |
         v_code | integer |           |          |
         i_code | integer |           |          |
         i_name | text    |           |          |
        Indexes:
            "orders_pkey" PRIMARY KEY, lsm (o_no HASH)
        Foreign-key constraints:
            "orders_i_code_fkey" FOREIGN KEY (i_code, i_name) REFERENCES items(i_code, i_name)
            "orders_v_code_fkey" FOREIGN KEY (v_code) REFERENCES vendors(v_code)
      )#"
  ));

  ASSERT_NO_FATALS(InsertOneRow("INSERT INTO vendors (v_code, v_name) VALUES (100, 'foo')"));

  const string backup_dir = GetTempDir("backup");

  ASSERT_OK(RunBackupCommand(
      {"--backup_location", backup_dir, "--keyspace", "ysql.yugabyte", "create"}));

  ASSERT_NO_FATALS(InsertOneRow("INSERT INTO vendors (v_code, v_name) VALUES (200, 'bar')"));

  // Restore into new "ysql.yugabyte2" YSQL DB.
  ASSERT_OK(RunBackupCommand(
      {"--backup_location", backup_dir, "--keyspace", "ysql.yugabyte2", "restore"}));

  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT v_code, v_name FROM vendors ORDER BY v_code",
      R"#(
         v_code | v_name
        --------+--------
            100 | foo
            200 | bar
        (2 rows)
      )#"
  ));

  SetDbName("yugabyte2"); // Connecting to the second DB from the moment.

  // Check the tables.
  ASSERT_NO_FATALS(RunPsqlCommand(
      "\\dt",
      R"#(
                  List of relations
        Schema |   Name    | Type  |  Owner
       --------+-----------+-------+----------
        public | hashtbl   | table | yugabyte
        public | items     | table | yugabyte
        public | orders    | table | yugabyte
        public | rangetbl  | table | yugabyte
        public | serialtbl | table | yugabyte
        public | vendors   | table | yugabyte
       (6 rows)
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "\\d rangetbl",
      R"#(
                     Table "public.rangetbl"
        Column |  Type   | Collation | Nullable | Default
       --------+---------+-----------+----------+---------
        k      | integer |           | not null |
       Indexes:
           "rangetbl_pkey" PRIMARY KEY, lsm (k)
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "\\d serialtbl",
      R"#(
                                   Table "public.serialtbl"
        Column |  Type   | Collation | Nullable |               Default
       --------+---------+-----------+----------+--------------------------------------
        k      | integer |           | not null | nextval('serialtbl_k_seq'::regclass)
        v      | text    |           |          |
       Indexes:
           "serialtbl_pkey" PRIMARY KEY, lsm (k HASH)
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "\\d vendors",
      R"#(
                     Table "public.vendors"
        Column |  Type   | Collation | Nullable | Default
       --------+---------+-----------+----------+---------
        v_code | integer |           | not null |
        v_name | text    |           |          |
       Indexes:
           "vendors_pkey" PRIMARY KEY, lsm (v_code HASH)
           "vendors_v_name_idx" UNIQUE, lsm (v_name HASH)
       Referenced by:
      )#"
      "     TABLE \"orders\" CONSTRAINT \"orders_v_code_fkey\" FOREIGN KEY (v_code) "
      "REFERENCES vendors(v_code)"
  ));
  // Check the table data.
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT v_code, v_name FROM vendors ORDER BY v_code",
      R"#(
         v_code | v_name
        --------+--------
            100 | foo
        (1 row)
      )#"
  ));
  // Check the index data.
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT schemaname, indexname FROM pg_indexes WHERE tablename = 'vendors'",
      R"#(
         schemaname |     indexname
        ------------+--------------------
         public     | vendors_pkey
         public     | vendors_v_name_idx
        (2 rows)
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "EXPLAIN SELECT v_name FROM vendors WHERE v_name = 'foo'",
      R"#(
                                               QUERY PLAN
        ----------------------------------------------------------------------------------------
         Index Only Scan using vendors_v_name_idx on vendors  (cost=0.00..4.11 rows=1 width=32)
           Index Cond: (v_name = 'foo'::text)
        (2 rows)
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT v_name FROM vendors WHERE v_name = 'foo'",
      R"#(
         v_name
        --------
         foo
        (1 row)
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "EXPLAIN SELECT * FROM vendors WHERE v_name = 'foo'",
      R"#(
                                            QUERY PLAN
        -----------------------------------------------------------------------------------
         Index Scan using vendors_v_name_idx on vendors  (cost=0.00..4.12 rows=1 width=36)
           Index Cond: (v_name = 'foo'::text)
        (2 rows)
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT * FROM vendors WHERE v_name = 'foo'",
      R"#(
         v_code | v_name
        --------+--------
            100 | foo
        (1 row)
      )#"
  ));
  // Check the view.
  ASSERT_NO_FATALS(RunPsqlCommand(
      "\\d vendors_view",
      R"#(
                       View "public.vendors_view"
         Column |  Type   | Collation | Nullable | Default
        --------+---------+-----------+----------+---------
         v_code | integer |           |          |
         v_name | text    |           |          |
      )#"
  ));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT * FROM vendors_view",
      R"#(
         v_code | v_name
        --------+--------
            100 | foo
        (1 row)
      )#"
  ));
  // Check the foreign keys.
  ASSERT_NO_FATALS(RunPsqlCommand(
      "\\d orders",
      R"#(
                       Table "public.orders"
         Column |  Type   | Collation | Nullable | Default
        --------+---------+-----------+----------+---------
         o_no   | integer |           | not null |
         o_date | date    |           |          |
         v_code | integer |           |          |
         i_code | integer |           |          |
         i_name | text    |           |          |
        Indexes:
            "orders_pkey" PRIMARY KEY, lsm (o_no HASH)
        Foreign-key constraints:
            "orders_i_code_fkey" FOREIGN KEY (i_code, i_name) REFERENCES items(i_code, i_name)
            "orders_v_code_fkey" FOREIGN KEY (v_code) REFERENCES vendors(v_code)
      )#"
  ));
  // Check the stored procedure.
  ASSERT_NO_FATALS(Call("CALL proc(3)"));
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT v_code, v_name FROM vendors ORDER BY v_code",
      R"#(
         v_code | v_name
        --------+--------
             11 |
             12 |
             13 |
            100 | foo
        (4 rows)
      )#"
  ));

  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

TEST_F(YBBackupTest, YB_DISABLE_TEST_IN_SANITIZERS(TestYBBackupWrongUsage)) {
  client::KeyValueTableTest::CreateTable(
      client::Transactional::kTrue, CalcNumTablets(3), client_.get(), &table_);
  const string& keyspace = table_.name().namespace_name();
  const string& table = table_.name().table_name();
  const string backup_dir = GetTempDir("backup");

  // No 'create' or 'restore' argument.
  ASSERT_NOK(RunBackupCommand({}));

  // No '--keyspace' argument.
  ASSERT_NOK(RunBackupCommand({"--backup_location", backup_dir, "create"}));
  ASSERT_NOK(RunBackupCommand({"--backup_location", backup_dir, "--table", table, "create"}));

  ASSERT_OK(RunBackupCommand({"--backup_location", backup_dir, "--keyspace", keyspace, "create"}));

  // No '--keyspace' argument, but there is '--table'.
  ASSERT_NOK(RunBackupCommand(
      {"--backup_location", backup_dir, "--table", "new_" + table, "restore"}));

  ASSERT_OK(RunBackupCommand({"--backup_location", backup_dir, "restore"}));

  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

}  // namespace tools
}  // namespace yb
