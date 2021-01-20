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

#include "yb/yql/pgwrapper/pg_wrapper.h"

#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"
#include "yb/util/result.h"
#include "yb/util/path_util.h"
#include "yb/util/net/net_util.h"
#include "yb/util/subprocess.h"
#include "yb/util/enums.h"
#include "yb/common/wire_protocol.h"

#include "yb/master/master.pb.h"
#include "yb/master/master.proxy.h"
#include "yb/client/client.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_table_name.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"

#include "yb/yql/pgwrapper/pg_wrapper_test_base.h"

using yb::master::FlushTablesRequestPB;
using yb::master::FlushTablesResponsePB;
using yb::master::IsFlushTablesDoneRequestPB;
using yb::master::IsFlushTablesDoneResponsePB;
using yb::master::MasterServiceProxy;
using yb::StatusFromPB;

using std::string;
using std::vector;
using std::unique_ptr;

using yb::client::YBTableName;
using yb::rpc::RpcController;

using namespace std::literals;

namespace yb {
namespace pgwrapper {
namespace {

template<bool Auth, bool Encrypted>
struct ConnectionStrategy {
  static const bool UseAuth = Auth;
  static const bool EncryptConnection = Encrypted;
};

template<class Strategy>
class PgWrapperTestHelper: public PgCommandTestBase {
 protected:
  PgWrapperTestHelper() : PgCommandTestBase(Strategy::UseAuth, Strategy::EncryptConnection) {}
};

} // namespace


YB_DEFINE_ENUM(FlushOrCompaction, (kFlush)(kCompaction));

class PgWrapperTest : public PgWrapperTestHelper<ConnectionStrategy<false, false>> {
 protected:
  void FlushOrCompact(string table_id, FlushOrCompaction flush_or_compaction) {
    RpcController rpc;
    auto master_proxy = cluster_->master_proxy();

    FlushTablesResponsePB flush_tables_resp;
    FlushTablesRequestPB compaction_req;
    compaction_req.add_tables()->set_table_id(table_id);
    compaction_req.set_is_compaction(flush_or_compaction == FlushOrCompaction::kCompaction);
    LOG(INFO) << "Initiating a " << flush_or_compaction << " request for table " << table_id;
    ASSERT_OK(master_proxy->FlushTables(compaction_req, &flush_tables_resp, &rpc));
    LOG(INFO) << "Initiated a " << flush_or_compaction << " request for table " << table_id;

    if (flush_tables_resp.has_error()) {
      FAIL() << flush_tables_resp.error().ShortDebugString();
    }

    IsFlushTablesDoneRequestPB wait_req;
    IsFlushTablesDoneResponsePB wait_resp;
    wait_req.set_flush_request_id(flush_tables_resp.flush_request_id());

    ASSERT_OK(WaitFor(
        [&]() -> Result<bool> {
          rpc.Reset();
          RETURN_NOT_OK(master_proxy->IsFlushTablesDone(wait_req, &wait_resp, &rpc));

          if (wait_resp.has_error()) {
            if (wait_resp.error().status().code() == AppStatusPB::NOT_FOUND) {
              return STATUS_FORMAT(
                  NotFound, "$0 request was deleted: $1",
                  flush_or_compaction, flush_tables_resp.flush_request_id());
            }

            return StatusFromPB(wait_resp.error().status());
          }

          if (wait_resp.done()) {
            if (!wait_resp.success()) {
              return STATUS_FORMAT(
                  InternalError, "$0 request failed: $1", flush_or_compaction,
                  wait_resp.ShortDebugString());
            }
            return true;
          }
          return false;
        },
        MonoDelta::FromSeconds(20),
        "Compaction"
    ));
    LOG(INFO) << "Table " << table_id << " " << flush_or_compaction << " finished";
  }
};

using PgWrapperTestAuth = PgWrapperTestHelper<ConnectionStrategy<true, false>>;
using PgWrapperTestSecure = PgWrapperTestHelper<ConnectionStrategy<false, true>>;
using PgWrapperTestAuthSecure = PgWrapperTestHelper<ConnectionStrategy<true, true>>;

TEST_F(PgWrapperTestAuth, YB_DISABLE_TEST_IN_TSAN(TestConnectionAuth)) {
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT clientdn FROM pg_stat_ssl WHERE ssl=true",
      R"#(
         clientdn
        ----------
        (0 rows)
      )#"
  ));
}

TEST_F(PgWrapperTestSecure, YB_DISABLE_TEST_IN_TSAN(TestConnectionTLS)) {
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT clientdn FROM pg_stat_ssl WHERE ssl=true",
      R"#(
                clientdn
        -------------------------
         /O=YugaByte/CN=yugabyte
        (1 row)
      )#"
  ));
}


TEST_F(PgWrapperTestAuthSecure, YB_DISABLE_TEST_IN_TSAN(TestConnectionAuthTLS)) {
  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT clientdn FROM pg_stat_ssl WHERE ssl=true",
      R"#(
                clientdn
        -------------------------
         /O=YugaByte/CN=yugabyte
        (1 row)
      )#"
  ));
}

TEST_F(PgWrapperTest, YB_DISABLE_TEST_IN_TSAN(TestStartStop)) {
  ASSERT_NO_FATALS(CreateTable("CREATE TABLE mytbl (k INT PRIMARY KEY, v TEXT)"));
  ASSERT_NO_FATALS(InsertOneRow("INSERT INTO mytbl (k, v) VALUES (100, 'foo')"));
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
}

TEST_F(PgWrapperTest, YB_DISABLE_TEST_IN_TSAN(TestCompactHistoryWithTxn)) {
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(60));
  ASSERT_NO_FATALS(CreateTable("CREATE TABLE mytbl (k INT PRIMARY KEY, v TEXT)"));

  ASSERT_NO_FATALS(InsertOneRow("INSERT INTO mytbl (k, v) VALUES (100, 'value1')"));
  string table_id;
  LOG(INFO) << "Preparing to force a compaction on the table we created";
  {
    auto client = ASSERT_RESULT(cluster_->CreateClient());

    const auto tables = ASSERT_RESULT(client->ListTables());
    for (const auto& table : tables) {
      if (table.has_table() && table.table_name() == "mytbl") {
        table_id = table.table_id();
        break;
      }
    }
  }
  ASSERT_TRUE(!table_id.empty());

  for (int i = 2; i <= 5; ++i) {
    ASSERT_NO_FATALS(UpdateOneRow(
        Format("UPDATE mytbl SET v = 'value$0' WHERE k = 100", i)));
    ASSERT_NO_FATALS(FlushOrCompact(table_id, FlushOrCompaction::kFlush));
  }

  ASSERT_NO_FATALS(RunPsqlCommand(
      "SELECT k, v FROM mytbl WHERE k = 100",
      R"#(
          k  |   v
        -----+--------
         100 | value5
        (1 row)
      )#"
  ));

  std::thread read_txn_thread([this]{
    LOG(INFO) << "Starting transaction to read data and wait";
    RunPsqlCommand(
        "BEGIN; "
        "SELECT * FROM mytbl WHERE k = 100; "
        "SELECT pg_sleep(30); "
        "SELECT * FROM mytbl WHERE k = 100; "
        "COMMIT; "
        "SELECT * FROM mytbl WHERE k = 100;",
        R"#(
          BEGIN
            k  |   v
          -----+--------
           100 | value5
          (1 row)

           pg_sleep
          ----------

          (1 row)

          ROLLBACK
            k  |   v
          -----+--------
           100 | value7
          (1 row)
        )#");
    LOG(INFO) << "Transaction finished";
  });

  // Give our transaction a chance to start up the backend and perform the first read.
  std::this_thread::sleep_for(5s);

  // Generate a few more files.
  for (int i = 6; i <= 7; ++i) {
    ASSERT_NO_FATALS(UpdateOneRow(
        Format("UPDATE mytbl SET v = 'value$0' WHERE k = 100", i)));
    ASSERT_NO_FATALS(FlushOrCompact(table_id, FlushOrCompaction::kFlush));
  }

  ASSERT_NO_FATALS(FlushOrCompact(table_id, FlushOrCompaction::kCompaction));

  read_txn_thread.join();
}

TEST_F(PgWrapperTest, YB_DISABLE_TEST_IN_TSAN(InsertSelect)) {
  ASSERT_NO_FATALS(CreateTable("CREATE TABLE mytbl (k INT, v TEXT)"));

  ASSERT_NO_FATALS(InsertOneRow("INSERT INTO mytbl (k, v) VALUES (1, 'abc')"));
  for (size_t i = 0; i != RegularBuildVsSanitizers(7, 1); ++i) {
     ASSERT_NO_FATALS(InsertRows(
         "INSERT INTO mytbl SELECT * FROM mytbl", 1 << i /* expected_rows */));
  }
}

class PgWrapperOneNodeClusterTest : public YBMiniClusterTestBase<ExternalMiniCluster> {
 public:
  void SetUp() {
    YBMiniClusterTestBase::SetUp();

    ExternalMiniClusterOptions opts;
    opts.enable_ysql = true;
    opts.num_tablet_servers = 1;

    cluster_.reset(new ExternalMiniCluster(opts));
    ASSERT_OK(cluster_->Start());

    pg_ts_ = cluster_->tablet_server(0);

    // TODO: fix cluster verification for PostgreSQL tables.
    DontVerifyClusterBeforeNextTearDown();
  }

 protected:
  ExternalTabletServer* pg_ts_ = nullptr;

};

TEST_F(PgWrapperOneNodeClusterTest, YB_DISABLE_TEST_IN_TSAN(TestPostgresPid)) {
  MonoDelta timeout = 15s;
  int tserver_count = 1;

  std::string pid_file = JoinPathSegments(pg_ts_->GetDataDir(), "pg_data", "postmaster.pid");
  // Wait for postgres server to start and setup postmaster.pid file
  AssertLoggedWaitFor(
      [this, &pid_file] {
        return env_->FileExists(pid_file);
      }, timeout, "Waiting for postgres server to create postmaster.pid file");
  ASSERT_TRUE(env_->FileExists(pid_file));

  // Shutdown tserver and wait for postgres server to shut down and delete postmaster.pid file
  pg_ts_->Shutdown();
  AssertLoggedWaitFor(
      [this, &pid_file] {
        return !env_->FileExists(pid_file);
      }, timeout, "Waiting for postgres server to shutdown");
  ASSERT_FALSE(env_->FileExists(pid_file));

  // Create empty postmaster.pid file and ensure that tserver can start up
  // Use sync_on_close flag to ensure that the file is flushed to disk when tserver tries to read it
  std::unique_ptr<RWFile> file;
  RWFileOptions opts;
  opts.sync_on_close = true;
  opts.mode = Env::CREATE_IF_NON_EXISTING_TRUNCATE;

  ASSERT_OK(env_->NewRWFile(opts, pid_file, &file));
  ASSERT_OK(pg_ts_->Start(false /* start_cql_proxy */));
  ASSERT_OK(cluster_->WaitForTabletServerCount(tserver_count, timeout));

  // Shutdown tserver and wait for postgres server to shutdown and delete postmaster.pid file
  pg_ts_->Shutdown();
  AssertLoggedWaitFor(
      [this, &pid_file] {
        return !env_->FileExists(pid_file);
      }, timeout, "Waiting for postgres server to shutdown", 100ms);
  ASSERT_FALSE(env_->FileExists(pid_file));

  // Create postmaster.pid file with string pid (invalid) and ensure that tserver can start up
  ASSERT_OK(env_->NewRWFile(opts, pid_file, &file));
  ASSERT_OK(file->Write(0, "abcde\n" + pid_file));
  ASSERT_OK(file->Close());

  ASSERT_OK(pg_ts_->Start(false /* start_cql_proxy */));
  ASSERT_OK(cluster_->WaitForTabletServerCount(tserver_count, timeout));

  // Shutdown tserver and wait for postgres server to shutdown and delete postmaster.pid file
  pg_ts_->Shutdown();
  AssertLoggedWaitFor(
      [this, &pid_file] {
        return !env_->FileExists(pid_file);
      }, timeout, "Waiting for postgres server to shutdown", 100ms);
  ASSERT_FALSE(env_->FileExists(pid_file));

  // Create postgres pid file with integer pid (valid) and ensure that tserver can start up
  ASSERT_OK(env_->NewRWFile(opts, pid_file, &file));
  ASSERT_OK(file->Write(0, "1002\n" + pid_file));
  ASSERT_OK(file->Close());

  ASSERT_OK(pg_ts_->Start(false /* start_cql_proxy */));
  ASSERT_OK(cluster_->WaitForTabletServerCount(tserver_count, timeout));
}

}  // namespace pgwrapper
}  // namespace yb
