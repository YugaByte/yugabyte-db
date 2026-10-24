// Copyright (c) YugabyteDB, Inc.
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

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <utility>

#include "yb/client/client_fwd.h"
#include "yb/client/snapshot_test_util.h"
#include "yb/client/table_info.h"
#include "yb/client/yb_table_name.h"
#include "yb/client/client-test-util.h"

#include "yb/common/common.pb.h"
#include "yb/common/ddl_mode-test-util.h"
#include "yb/common/pgsql_error.h"
#include "yb/common/schema.h"
#include "yb/common/wire_protocol.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/master_admin.pb.h"
#include "yb/master/master_admin.proxy.h"
#include "yb/master/master_client.pb.h"
#include "yb/master/master_ddl.pb.h"
#include "yb/master/master_ddl.proxy.h"
#include "yb/master/mini_master.h"
#include "yb/master/sys_catalog.h"

#include "yb/rpc/rpc_controller.h"

#include "yb/tserver/tserver_service.pb.h"

#include "yb/util/async_util.h"
#include "yb/util/backoff_waiter.h"
#include "yb/util/countdown_latch.h"
#include "yb/util/monotime.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status_format.h"
#include "yb/util/string_util.h"
#include "yb/util/sync_point.h"
#include "yb/util/test_thread_holder.h"
#include "yb/util/timestamp.h"
#include "yb/util/tsan_util.h"

#include "yb/yql/pgwrapper/libpq_test_base.h"
#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_ddl_atomicity_test_base.h"
#include "yb/yql/pgwrapper/pg_test_utils.h"

#include "yb/yql/pgwrapper/pg_mini_test_base.h"

using namespace std::literals;
using std::string;
using std::vector;
using namespace std::literals;

using yb::tserver::ListTabletsForTabletServerResponsePB;

DECLARE_string(allowed_preview_flags_csv);
DECLARE_bool(report_ysql_ddl_txn_status_to_master);
DECLARE_uint32(TEST_abort_create_table);
DECLARE_string(TEST_block_alter_table);
DECLARE_bool(TEST_fail_alter_table_after_commit);
DECLARE_bool(TEST_simulate_crash_after_table_marked_deleting);
DECLARE_bool(TEST_disable_ysql_ddl_txn_verification);
DECLARE_bool(ysql_yb_enable_ddl_savepoint_support);
DECLARE_bool(TEST_hang_on_ddl_verification_progress);
DECLARE_bool(TEST_pause_ddl_rollback);
DECLARE_bool(TEST_ysql_disable_transparent_cache_refresh_retry);
DECLARE_double(TEST_ysql_ddl_transaction_verification_failure_probability);
DECLARE_bool(ysql_ddl_transaction_wait_for_ddl_verification);
DECLARE_int32(ysql_ddl_post_processing_failed_verification_retry_secs);

namespace yb {
namespace pgwrapper {

class PgDdlAtomicityTest : public PgDdlAtomicityTestBase {
 public:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->extra_master_flags.push_back("--ysql_transaction_bg_task_wait_ms=5000");
    options->extra_tserver_flags.push_back("--ysql_pg_conf_csv=log_statement=all");
    ToggleDDLMode(options->extra_tserver_flags, LegacyDDLMode());
    ToggleDDLMode(options->extra_master_flags, LegacyDDLMode());
  }

  // Object locking, concurrent DDL and transactional DDL are enabled/ disabled together, per the
  // cross-flag validators in common_flags.cc, so a single knob drives all three.
  virtual bool LegacyDDLMode() const {
    // The tests assert for transaction verification errors in case of concurrent/conflicting DDLs.
    // But with object locks, conflicting DDLs get serialized, and the latter one times out with
    // various potential errors. Hence disabling table locking for this test.
    return true;
  }

  void CreateTable(const string& tablename) {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.Execute(CreateTableStmt(tablename)));
  }

  // After the master is restarted, it will continue the rollback operations of ongoing
  // DDL transactions. This will increment the table schema version and then propagate
  // to all tablet leaders via AlterSchema RPC. The new master leader needs some time to
  // learn who are the tablet leaders. If the new master does not know who is the leader
  // of a tablet, AlterSchema RPC will fail and the tablet leader can have a stale schema
  // version, leading to schema version mismatch error like: expected 3, got 4.
  // This function adds a delay that is proportional to the number of tablets found in the
  // cluster.
  void WaitForMasterToLearnAllTabletLeaders() const {
    std::unordered_set<std::string> tablet_id_set;
    for (size_t i = 0; i < cluster_->num_tablet_servers(); ++i) {
      const auto ts = cluster_->tablet_server(i);
      const auto tablets = CHECK_RESULT(cluster_->GetTablets(ts));
      for (const auto& tablet : tablets) {
        tablet_id_set.insert(tablet.tablet_id());
      }
    }
    SleepFor(300ms * tablet_id_set.size() * RegularBuildVsDebugVsSanitizers(1, 3, 3));
  }

  void RestartMaster() {
    LOG(INFO) << "Restarting Master";
    auto master = cluster_->GetLeaderMaster();
    master->Shutdown();
    ASSERT_OK(master->Restart());
    ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      auto s = cluster_->GetIsMasterLeaderServiceReady(master);
      return s.ok();
    }, MonoDelta::FromSeconds(60), "Wait for Master to be ready."));
    WaitForMasterToLearnAllTabletLeaders();
    LOG(INFO) << "Restarted Master";
  }

  void SetFlagOnAllProcessesWithRollingRestart(const string& flag) {
    LOG(INFO) << "Restart the cluster and set " << flag;
    for (size_t i = 0; i != cluster_->num_masters(); ++i) {
      cluster_->master(i)->mutable_flags()->push_back(flag);
    }

    RestartMaster();

    for (size_t i = 0; i != cluster_->num_tablet_servers(); ++i) {
      cluster_->tablet_server(i)->mutable_flags()->push_back(flag);
    }
    for (auto* tserver : cluster_->tserver_daemons()) {
      tserver->Shutdown();
      ASSERT_OK(tserver->Restart());
      SleepFor(5s);
    }
  }
};

class PgDdlAtomicityMasterFailoverTest : public PgDdlAtomicityTest {
 protected:
  int GetNumMasters() const override { return 3; }

  bool TableLocksEnabled() const override { return true; }

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgDdlAtomicityTest::UpdateMiniClusterOptions(options);
    options->bind_to_unique_loopback_addresses = false;
    options->extra_tserver_flags.push_back("--ysql_oid_cache_prefetch_size=1");
  }
};

TEST_F(PgDdlAtomicityMasterFailoverTest, CreateVectorExtensionDuringLeaderRestart) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE DATABASE db_1"));

  auto blocker_conn = ASSERT_RESULT(ConnectToDB("db_1"));
  auto observer_conn = ASSERT_RESULT(ConnectToDB("db_1"));
  ASSERT_OK(blocker_conn.Execute(R"sql(
    CREATE FUNCTION block_vector_opclass() RETURNS event_trigger AS $$
    BEGIN
      RAISE LOG 'blocking vector operator class for master failover';
      PERFORM pg_advisory_lock(32385);
    END;
    $$ LANGUAGE plpgsql;

    CREATE EVENT TRIGGER block_vector_opclass
      ON ddl_command_end
      WHEN TAG IN ('CREATE OPERATOR CLASS')
      EXECUTE FUNCTION block_vector_opclass();
  )sql"));

  const auto baseline_amop_count =
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>("SELECT count(*) FROM pg_amop"));
  const auto db_oid = ASSERT_RESULT(GetDatabaseOid(&observer_conn, "db_1"));
  const auto catalog_version_query =
      Format("SELECT current_version FROM pg_yb_catalog_version WHERE db_oid = $0", db_oid);
  const auto baseline_catalog_version =
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>(catalog_version_query));
  ASSERT_OK(blocker_conn.Fetch("SELECT pg_advisory_lock(32385)"));

  LogWaiter trigger_waiter(pg_ts, "blocking vector operator class for master failover");
  auto extension_future = std::async(std::launch::async, [&]() -> Status {
    auto extension_conn = VERIFY_RESULT(ConnectToDB("db_1"));
    RETURN_NOT_OK(
        extension_conn.ExecuteFormat("SET statement_timeout = '$0s'", 120 * kTimeMultiplier));
    return extension_conn.Execute("CREATE EXTENSION IF NOT EXISTS vector");
  });

  bool blocker_lock_held = true;
  auto unlock_blocker = ScopeExit([&] {
    if (blocker_lock_held) {
      WARN_NOT_OK(
          blocker_conn.Fetch("SELECT pg_advisory_unlock(32385)"),
          "Failed to release extension test advisory lock");
    }
  });

  ASSERT_OK(trigger_waiter.WaitFor(30s * kTimeMultiplier));
  ASSERT_EQ(extension_future.wait_for(0s), std::future_status::timeout);

  ASSERT_EQ(
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>(
          "SELECT count(*) FROM pg_extension WHERE extname = 'vector'")),
      0);
  ASSERT_EQ(
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>("SELECT count(*) FROM pg_amop")),
      baseline_amop_count);
  ASSERT_EQ(
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>(catalog_version_query)),
      baseline_catalog_version);

  const auto old_leader_idx = ASSERT_RESULT(cluster_->GetLeaderMasterIndex());
  auto* old_leader = cluster_->master(old_leader_idx);
  bool old_leader_down = false;
  auto restart_old_leader = ScopeExit([&] {
    if (old_leader_down) {
      WARN_NOT_OK(old_leader->Restart(), "Failed to restart old master leader");
    }
  });

  old_leader->Shutdown();
  old_leader_down = true;
  size_t new_leader_idx = old_leader_idx;
  ASSERT_OK(LoggedWaitFor(
      [&]() -> Result<bool> {
        const auto leader_idx = cluster_->GetLeaderMasterIndex();
        if (!leader_idx.ok()) {
          return false;
        }
        new_leader_idx = *leader_idx;
        return new_leader_idx != old_leader_idx;
      },
      60s * kTimeMultiplier, "Wait for a new master leader"));
  ASSERT_OK(old_leader->Restart());
  old_leader_down = false;

  ASSERT_TRUE(ASSERT_RESULT(blocker_conn.FetchRow<bool>("SELECT pg_advisory_unlock(32385)")));
  blocker_lock_held = false;

  ASSERT_EQ(extension_future.wait_for(120s * kTimeMultiplier), std::future_status::ready);
  ASSERT_OK(extension_future.get());

  ASSERT_EQ(
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>(
          "SELECT count(*) FROM pg_extension WHERE extname = 'vector'")),
      1);
  ASSERT_EQ(
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>(catalog_version_query)),
      baseline_catalog_version + 1);
  const auto final_amop_count =
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>("SELECT count(*) FROM pg_amop"));
  ASSERT_GT(final_amop_count, baseline_amop_count);
  ASSERT_EQ(
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>("SELECT count(DISTINCT oid) FROM pg_amop")),
      final_amop_count);
  ASSERT_EQ(
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>(R"sql(
              SELECT count(*)
              FROM pg_amop amop
              LEFT JOIN pg_opfamily opfamily ON opfamily.oid = amop.amopfamily
              LEFT JOIN pg_operator op ON op.oid = amop.amopopr
              WHERE opfamily.oid IS NULL OR op.oid IS NULL
            )sql")),
      0);
  ASSERT_OK(observer_conn.Fetch("SELECT '[1,2,3]'::vector <-> '[1,2,4]'::vector"));

  ASSERT_OK(observer_conn.Execute("DROP EVENT TRIGGER block_vector_opclass"));
  ASSERT_OK(observer_conn.Execute("DROP FUNCTION block_vector_opclass()"));
  ASSERT_OK(observer_conn.Execute("DROP EXTENSION vector"));
  ASSERT_EQ(
      ASSERT_RESULT(observer_conn.FetchRow<PGUint64>("SELECT count(*) FROM pg_amop")),
      baseline_amop_count);
  ASSERT_OK(observer_conn.Execute("CREATE EXTENSION vector"));
  ASSERT_OK(observer_conn.Fetch("SELECT '[1,2,3]'::vector <-> '[1,2,4]'::vector"));
}

TEST_F(PgDdlAtomicityTest, TestDatabaseGC) {
  TableName test_name = "test_pgsql";
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.TestFailDdl("CREATE DATABASE " + test_name));

  // Verify DocDB Database creation, even though it failed in PG layer.
  // 'ysql_transaction_bg_task_wait_ms' setting ensures we can finish this before the GC.
  VerifyNamespaceExists(client.get(), test_name);

  // After bg_task_wait, DocDB will notice the PG layer failure because the transaction aborts.
  // Confirm that DocDB async deletes the namespace.
  VerifyNamespaceNotExists(client.get(), test_name);
}

TEST_F(PgDdlAtomicityTest, TestCreateDbAndRestartGC) {
  NamespaceName test_name = "test_pgsql";
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.TestFailDdl("CREATE DATABASE " + test_name));

  // Verify DocDB Database creation, even though it fails in PG layer.
  // 'ysql_transaction_bg_task_wait_ms' setting ensures we can finish this before the GC.
  VerifyNamespaceExists(client.get(), test_name);

  // Restart the master before the BG task can kick in and GC the failed transaction.
  RestartMaster();

  // Re-init client after restart.
  client = ASSERT_RESULT(cluster_->CreateClient());

  // Confirm that Catalog Loader deletes the namespace on master restart.
  VerifyNamespaceNotExists(client.get(), test_name);
}

TEST_F(PgDdlAtomicityTest, TestIndexTableGC) {
  TableName test_name = "test_pgsql_table";
  TableName test_name_idx = test_name + "_idx";

  auto client = ASSERT_RESULT(cluster_->CreateClient());

  // Lower the delays so we successfully create this first table.
  ASSERT_OK(cluster_->SetFlagOnMasters("ysql_transaction_bg_task_wait_ms", "10"));

  // Create Table that Index will be set on.
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute(CreateTableStmt(test_name)));

  // After successfully creating the first table, set flags to delay the background task.
  // But do not let PG wait for the ddl verification to complete otherwise it will defeat the
  // purpopse of the following 13000ms delay to start the ddl verification task: TestFailDdl
  // does not finish until 13000ms have passed and the ddl verification has completed. On
  // ddl verification completion master rolls back the index table and the next VerifyTableExists
  // fails to see the table. Do not set report_ysql_ddl_txn_status_to_master for similar
  // reason: on receiving PG reported status without polling master rolls back the index
  // table.
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "ysql_ddl_transaction_wait_for_ddl_verification", "false"));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "report_ysql_ddl_txn_status_to_master", "false"));
  ASSERT_OK(cluster_->SetFlagOnMasters("ysql_transaction_bg_task_wait_ms", "13000"));
  ASSERT_OK(conn.TestFailDdl(CreateIndexStmt(test_name_idx, test_name, "key")));

  // Wait for DocDB index creation, even though it will fail in PG layer.
  // 'ysql_transaction_bg_task_wait_ms' setting ensures we can finish this before the GC.
  VerifyTableExists(client.get(), kDatabase, test_name_idx, 10);

  // DocDB will notice the PG layer failure because the transaction aborts.
  // Confirm that DocDB async deletes the index.
  VerifyTableNotExists(client.get(), kDatabase, test_name_idx, 40);
}

TEST_F(
    PgDdlAtomicityTest, TestRaceIndexDeletionAndReadQueryOnColocatedDB) {
  TableName table_name = "test_pgsql_table";
  TableName index_name = Format("$0_idx", table_name);
  NamespaceName db_name = "test_db";

  ASSERT_OK(
      ASSERT_RESULT(Connect()).ExecuteFormat("CREATE DATABASE $0 WITH colocated = true", db_name));

  auto conn = ASSERT_RESULT(ConnectToDB("test_db"));
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (key INT PRIMARY KEY, value TEXT)", table_name));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0 ON test_pgsql_table(value)", index_name));
  for (int i = 0; i < 5000; ++i) {
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES($1, '$2')", table_name, i, i));
  }
  TestThreadHolder threads;
  std::mutex m;
  std::condition_variable cv;
  bool cache_warm = false;
  bool index_deletion_sent = false;
  threads.AddThreadFunctor([this, &m, &cv, &cache_warm, &index_deletion_sent, &table_name] {
    auto conn = ASSERT_RESULT(ConnectToDB("test_db"));
    ASSERT_RESULT(
        conn.FetchRow<int32_t>(Format("SELECT key FROM $0 where value = '1000'", table_name)));
    LOG(INFO) << "queryer - setting cache to be warm";
    {
      std::unique_lock lk(m);
      cache_warm = true;
    }
    cv.notify_one();
    LOG(INFO) << "queryer - waiting for index deletion to be sent";
    {
      std::unique_lock lk(m);
      cv.wait(lk, [&index_deletion_sent] { return index_deletion_sent; });
    }
    auto result =
        conn.FetchRow<int32_t>(Format("SELECT key FROM $0 where value = '3000'", table_name));
    if (!result.ok() &&
        !(result.status().IsNetworkError() &&
          result.status().ToString().find("OBJECT_NOT_FOUND") != std::string::npos)) {
      FAIL() << "Expected ok or network error from query, received: " << result.status().ToString();
    }
    LOG(INFO) << "queryer - complete";
  });
  threads.AddThreadFunctor([this, &m, &cv, &cache_warm, &index_deletion_sent, &index_name] {
    auto conn = ASSERT_RESULT(ConnectToDB("test_db"));
    LOG(INFO) << "index deleter - waiting for cache to be warm";
    {
      std::unique_lock lk(m);
      cv.wait(lk, [&cache_warm] { return cache_warm; });
    }
    ASSERT_OK(conn.ExecuteFormat("DROP INDEX $0", index_name));
    LOG(INFO) << "index deleter - setting index deletion request sent to true";
    {
      std::unique_lock lk(m);
      index_deletion_sent = true;
    }
    cv.notify_one();
    LOG(INFO) << "index deleter - finished";
  });
  threads.JoinAll();
  threads.Stop();
}

TEST_F(PgDdlAtomicityTest, FailureRecoveryTestWithAbortedTxn) {
  // Make TransactionParticipant::Impl::CheckForAbortedTransactions and TabletLoader::Visit deadlock
  // on the mutex. GH issue #15849.

  // Temporarily disable abort cleanup. This flag will be reset when we RestartMaster.
  ASSERT_OK(cluster_->SetFlagOnMasters("transactions_poll_check_aborted", "true"));

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute(CreateTableStmt(kDropTable)));

  // Create an aborted transaction so that TransactionParticipant::Impl::CheckForAbortedTransactions
  // has something to do.
  ASSERT_OK(conn.TestFailDdl(CreateTableStmt(kCreateTable)));

  // Crash in the middle of a DDL so that TabletLoader::Visit will perform some writes to
  // sys_catalog on CatalogManager startup.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_simulate_crash_after_table_marked_deleting", "true"));
  // Set pause rollback flag.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  ASSERT_OK(conn.Execute(DropTableStmt(kDropTable)));

  ASSERT_EQ(cluster_->master_daemons().size(), 1);
  // Give enough time for CheckForAbortedTransactions to start and get stuck.
  cluster_->GetLeaderMaster()->mutable_flags()->push_back(
      "--TEST_delay_sys_catalog_reload_secs=10");

  RestartMaster();

  VerifyTableNotExists(client.get(), kDatabase, kDropTable, 40);
}

// Class for sanity test.
class PgDdlAtomicitySanityTest : public PgDdlAtomicityTest {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgDdlAtomicityTest::UpdateMiniClusterOptions(options);
    // TODO (#19975): Enable read committed isolation
    options->extra_tserver_flags.push_back("--yb_enable_read_committed_isolation=false");
  }

  bool LegacyDDLMode() const override {
    // Tests toggle ddl atomicity flag, which transactional ddl depends on.
    return true;
  }
};

TEST_F(PgDdlAtomicitySanityTest, BasicTest) {
  auto conn = ASSERT_RESULT(Connect());
  const int num_rows = 5;
  ASSERT_OK(SetupTablesForAllDdls(&conn, num_rows));

  ASSERT_OK(RunAllDdlsWithErrorInjection(&conn));

  // Wait for rollback to happen.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(WaitForDdlVerificationAfterDdlFailure(client.get(), kDatabase));

  // Verify that all the above operations are rolled back.
  ASSERT_OK(VerifyAllFailingDdlsRolledBack(&conn, client.get(), kDatabase));

  // Verify that data is still intact.
  ASSERT_OK(VerifyRowsAfterDdlErrorInjection(&conn, num_rows));

  // Now test successful DDLs.
  ASSERT_OK(RunAllDdls(&conn));
  ASSERT_OK(VerifyAllSuccessfulDdls(&conn, client.get(), kDatabase));

  // Verify that data is still intact.
  ASSERT_OK(VerifyRowsAfterDdlSuccess(&conn, num_rows));
}

TEST_F(PgDdlAtomicitySanityTest, BasicTest1) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute(CreateTableStmt(kCreateTable)));
  ASSERT_OK(conn.TestFailDdl(DropColumnStmt(kCreateTable)));
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kCreateTable, {"key", "value", "num"}));
}

TEST_F(PgDdlAtomicitySanityTest, CreateFailureRollback) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(cluster_->SetFlagOnMasters(
      "TEST_fail_table_creation_at_preparing_state", "true"));
  ASSERT_NOK(conn.Execute(CreateTableStmt(kCreateTable)));
  VerifyTableNotExists(client.get(), kDatabase, kCreateTable, 20);
}

TEST_F(PgDdlAtomicitySanityTest, TestMultiRewriteAlterTable) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());

  // First create a table with some data.
  const string table_name = "test_table";
  ASSERT_OK(conn.Execute(CreateTableStmt(table_name)));
  const int num_rows = 5;
  for (int i = 0; i < num_rows; ++i) {
    ASSERT_OK(conn.ExecuteFormat(
        "INSERT INTO $0 VALUES ($1, 'value$1', 1.1)", table_name, i));
  }

  // Test failure of alter statement with multiple subcommands.
  ASSERT_OK(conn.TestFailDdl("ALTER TABLE test_table ALTER COLUMN key TYPE text USING key::text,"
      " ALTER COLUMN num TYPE text USING num::text"));
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      if (VERIFY_RESULT(client->ListTables(table_name)).size() == 1)
        return true;
      return false;
  }, MonoDelta::FromSeconds(60), "Wait for new DocDB table to be dropped."));
  ASSERT_OK(VerifySchema(client.get(), "yugabyte", table_name, {"key", "value", "num"}));

  // Verify that the data and schema is intact.
  ASSERT_OK(conn.FetchMatrix(Format("SELECT * FROM $0", table_name), num_rows, 3));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (11, 'value11', 11.11)", table_name));

  // Verify successful execution of the above alter commands.
  ASSERT_OK(conn.Execute("ALTER TABLE test_table ALTER COLUMN key TYPE text USING key::text,"
      " ALTER COLUMN num TYPE text USING num::text"));
  ASSERT_OK(VerifySchema(client.get(), "yugabyte", table_name, {"key", "value", "num"}));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES ('keytext', 'value$1', '2.231')", table_name));

  // We added two new rows.
  ASSERT_OK(conn.FetchMatrix(Format("SELECT * FROM $0", table_name), num_rows + 2, 3));
}

TEST_F(PgDdlAtomicitySanityTest, TestChangedPkColOrder) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(cluster_->SetFlagOnTServers("report_ysql_ddl_txn_status_to_master", "false"));

  // First create a table with some data.
  const string table_name = "test_table";
  ASSERT_OK(conn.TestFailDdl(Format("CREATE TABLE $0 (key int, value text, value2 float, "
      "PRIMARY KEY(value2, key))", table_name)));
  VerifyTableNotExists(client.get(), "yugabyte", table_name, 10);

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (key int, value text, value2 float, "
      "PRIMARY KEY(value2, key))", table_name));
  VerifyTableExists(client.get(), "yugabyte", table_name, 10);

  // Verify Alter Table with switched PK order.
  const string alter_test = "alter_test";
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (key INT, value TEXT, value2 float)", alter_test));

  // Verify failure case.
  ASSERT_OK(conn.TestFailDdl(Format("ALTER TABLE $0 ADD PRIMARY KEY(value, key)", alter_test)));
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      if (VERIFY_RESULT(client->ListTables(alter_test)).size() == 1)
        return true;
      return false;
  }, MonoDelta::FromSeconds(60), "Wait for new DocDB table to be dropped."));
  // Insert duplicate rows.
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES (1, 'value1', 1.1), (1, 'value1', 1.1)",
                               alter_test));
  ASSERT_OK(conn.ExecuteFormat("DELETE FROM $0 WHERE key = 1", alter_test));

  // Verify success case.
  ASSERT_OK(conn.Execute(Format("ALTER TABLE $0 ADD PRIMARY KEY(value, key)", alter_test)));
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      if (VERIFY_RESULT(client->ListTables(alter_test)).size() == 1)
        return true;
      return false;
  }, MonoDelta::FromSeconds(60), "Wait for old DocDB table to be dropped."));
  ASSERT_NOK(conn.ExecuteFormat("INSERT INTO $0 VALUES (1, 'value1', 1.1), (1, 'value1', 1.1)",
                                alter_test));
}

TEST_F(PgDdlAtomicitySanityTest, IndexRollback) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());

  const TableName& table = "indexed_table";
  ASSERT_OK(conn.Execute(CreateTableStmt(table)));

  // Test rollback of CREATE INDEX statement.
  ASSERT_OK(conn.TestFailDdl(CreateIndexStmt(kCreateIndex, table)));
  VerifyTableNotExists(client.get(), kDatabase, kCreateIndex, 20);

  // Test rollback of CREATE INDEX NONCONCURRENTLY statement.
  ASSERT_OK(conn.TestFailDdl(
    Format("CREATE INDEX NONCONCURRENTLY $0 ON $1(value)", kCreateIndex, table)));
  VerifyTableNotExists(client.get(), kDatabase, kCreateIndex, 20);

  // Test rollback of CREATE TABLE with index statement.
  const TableName& unique_table = "unique_table";
  const string unique_idx_col = "col";
  ASSERT_OK(conn.TestFailDdl(Format("CREATE TABLE $0($1 text UNIQUE)",
                             unique_table, unique_idx_col)));
  VerifyTableNotExists(client.get(), kDatabase, unique_table, 40);
  VerifyTableNotExists(client.get(),
                       kDatabase,
                       Format("$0_$1_key", unique_table, unique_idx_col),
                       40);

  // Test rollback after failure at different backfill stages.
  const TableName& backfill_idx = "backfill_index_";
  vector<string> backfill_index_stages = {"indisready", "postbackfill"};
  for (const string& idx_stage : backfill_index_stages) {
    ASSERT_OK(conn.ExecuteFormat("SET yb_test_fail_index_state_change TO $0", idx_stage));
    const string idx = backfill_idx + idx_stage;
    ASSERT_NOK(conn.Execute(CreateIndexStmt(idx, table)));
    // If index backfill fails, then the indexes should not be cleaned up.
    // This is because the transaction that creates the index is separate from the transaction that
    // backfills the index.
    VerifyTableExists(client.get(), kDatabase, idx, 20);
  }
  ASSERT_OK(conn.ExecuteFormat("RESET yb_test_fail_index_state_change"));

  // Test failure of alter table add index.
  ASSERT_OK(conn.TestFailDdl(Format("ALTER TABLE $0 ADD UNIQUE(value)", table)));
  VerifyTableNotExists(client.get(), kDatabase, Format("$0_$1_key", table, "value"), 20);

  // Test rollback of Alter Table DROP index
  ASSERT_OK(conn.Execute(Format("ALTER TABLE $0 ADD UNIQUE(key)", table)));
  ASSERT_OK(conn.TestFailDdl(Format("ALTER TABLE $0 DROP CONSTRAINT $0_key_key", table)));
  VerifyTableExists(client.get(), kDatabase, Format("$0_key_key", table), 20);

  // Test rollback of DROP INDEX
  const string drop_idx_test = "drop_idx_test";
  ASSERT_OK(conn.Execute(CreateIndexStmt(drop_idx_test, table)));
  ASSERT_OK(conn.TestFailDdl(DropIndexStmt(drop_idx_test)));
  VerifyTableExists(client.get(), kDatabase, drop_idx_test, 20);

  // Test rollback of DROP TABLE which causes drop of index.
  const TableName drop_test = "drop_unique_idx_table";
  const string drop_col = "col";
  ASSERT_OK(conn.Execute(Format("CREATE TABLE $0($1 text UNIQUE, value text)",
                                drop_test, drop_col)));
  ASSERT_OK(conn.TestFailDdl(DropTableStmt(drop_test)));
  VerifyTableExists(client.get(), kDatabase, drop_test, 20);
  VerifyTableExists(client.get(), kDatabase, Format("$0_$1_key", drop_test, drop_col), 20);

  // Test rollback of rename index.
  const TableName& rename_unique_idx = "rename_unique_idx";
  ASSERT_OK(conn.Execute(CreateIndexStmt(rename_unique_idx, drop_test)));
  ASSERT_OK(conn.TestFailDdl(Format("ALTER INDEX $0 RENAME TO foobar", rename_unique_idx)));
  VerifyTableExists(client.get(), kDatabase, rename_unique_idx, 20);

  // Test successful DROP TABLE with index.
  ASSERT_OK(conn.Execute(DropTableStmt(drop_test)));
  VerifyTableNotExists(client.get(), kDatabase, drop_test, 20);
  VerifyTableNotExists(client.get(), kDatabase, Format("$0_$1_key", drop_test, drop_col), 20);
}

TEST_F(PgDdlAtomicitySanityTest, StressTestTableWithIndexRollback) {
  // If we drop a table with index, YSQL will issue drop requests for both of them separately.
  // However YB-Master will also drop the index if the table is being dropped. This can cause the
  // drop of index to be generated from two places. With DDL Atomicity, both the DROP requests can
  // be generated in any order in quick succession. Stress test dropping a table with index with
  // minuscule random sleep to ensure that there are no races.
  constexpr size_t kNumIterations = 25;
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_ysql_max_random_delay_before_ddl_verification_usecs",
                                       "1000"));
  auto conn = ASSERT_RESULT(Connect());

  // Test failure of creating a table with index.
  for (size_t i = 0; i < kNumIterations; ++i) {
    ASSERT_OK(conn.TestFailDdl("CREATE TABLE testable(col text UNIQUE, value text)"));
  }

  // Create tables with indexes.
  for (size_t i = 0; i < kNumIterations; ++i) {
    ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE $0_$1(col text UNIQUE, value text)", kIndexedTable, i));
  }

  // Drop all tables with indexes.
  for (size_t i = 0; i < kNumIterations; ++i) {
    ASSERT_OK(conn.ExecuteFormat(DropTableStmt(Format("$0_$1", kIndexedTable, i))));
  }

  // Verify that all the tables have been cleared.
  for (size_t i = 0; i < kNumIterations; ++i) {
    auto client = ASSERT_RESULT(cluster_->CreateClient());
    VerifyTableNotExists(client.get(), kDatabase, Format("$0_$1", kIndexedTable, i), 20);
    VerifyTableNotExists(client.get(), kDatabase, Format("$0_$1_col_key", kIndexedTable, i), 20);
  }
}

TEST_F(PgDdlAtomicitySanityTest, DdlRollbackMasterRestart) {
  auto conn = ASSERT_RESULT(Connect());
  const int num_rows = 5;
  ASSERT_OK(SetupTablesForAllDdls(&conn, num_rows));

  // Pause DDL rollback.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  ASSERT_OK(RunAllDdlsWithErrorInjection(&conn));

  // Verify that for now all the incomplete DDLs have taken effect.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(VerifyAllFailingDdlsNotRolledBack(client.get(), kDatabase));

  // Restart the master before the BG task can kick in and GC the failed transaction.
  RestartMaster();

  // Verify that rollback reflected on all the tables after restart.
  client = ASSERT_RESULT(cluster_->CreateClient()); // Reinit the YBClient after restart.

  ASSERT_OK(VerifyAllFailingDdlsRolledBack(&conn, client.get(), kDatabase));
  ASSERT_OK(VerifyRowsAfterDdlErrorInjection(&conn, num_rows));
}

TEST_F(PgDdlAtomicitySanityTest, TestYsqlTxnStatusReporting) {
  auto conn = ASSERT_RESULT(Connect());
  const int num_rows_to_insert = 5;
  ASSERT_OK(SetupTablesForAllDdls(&conn, num_rows_to_insert));

  // Disable YB-Master's background task.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_disable_ysql_ddl_txn_verification", "true"));
  // Run some failing Ddl transactions
  ASSERT_OK(RunAllDdlsWithErrorInjection(&conn));

  // The rollback should have succeeded anyway because YSQL would have reported the
  // status.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(VerifyAllFailingDdlsRolledBack(&conn, client.get(), kDatabase));

  // Now test successful DDLs.
  ASSERT_OK(RunAllDdls(&conn));

  // Verify that these DDLs were not rolled back.
  ASSERT_OK(VerifyAllSuccessfulDdls(&conn, client.get(), kDatabase));

  // Verify that rows are still intact.
  ASSERT_OK(VerifyRowsAfterDdlSuccess(&conn, num_rows_to_insert));
}

class PgDdlAtomicityParallelDdlTest : public PgDdlAtomicitySanityTest {
 public:
  template<class... Args>
  Result<bool> RunDdlHelper(const std::string& format, Args&&... args) {
    return DoRunDdlHelper(Format(format, std::forward<Args>(args)...));
  }

 private:
  Result<bool> DoRunDdlHelper(const std::string& ddl) {
    auto conn = VERIFY_RESULT(Connect());
    auto s = conn.Execute(ddl);
    if (s.ok()) {
      return true;
    }

    const auto msg = s.message().ToBuffer();
    static const auto allowed_msgs = {
      "Catalog Version Mismatch"sv,
      SerializeAccessErrorMessageSubstring(),
      "Restart read required"sv,
      "Transaction aborted"sv,
      "Transaction metadata missing"sv,
      "Unknown transaction, could be recently aborted"sv,
      "Flush: Value write after transaction start"sv,
      kDdlVerificationError
    };
    if (HasSubstring(msg, allowed_msgs)) {
      return false;
    }
    LOG(ERROR) << "Unexpected failure status " << s;
    return s;
  }
};

TEST_F(PgDdlAtomicityParallelDdlTest, TestParallelDdl) {
  constexpr size_t kNumIterations = 10;
  const auto tablename = "test_table"s;
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute(CreateTableStmt(tablename)));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1))",
      tablename,
      kNumIterations));

  // Add some columns.
  for (size_t i = 0; i < kNumIterations; ++i) {
    ASSERT_OK(ExecuteWithRetry(&conn,
      Format("ALTER TABLE $0 ADD COLUMN col_$1 TEXT", tablename, i)));
  }

  ASSERT_OK(cluster_->SetFlagOnMasters("ysql_transaction_bg_task_wait_ms", "0"));

  // Add columns in the first thread.
  TestThreadHolder thread_holder;
  thread_holder.AddThreadFunctor([this, &tablename] {
    for (size_t i = kNumIterations; i < kNumIterations * 2;) {
      if (ASSERT_RESULT(RunDdlHelper("ALTER TABLE $0 ADD COLUMN col_$1 TEXT", tablename, i))) {
       ++i;
      }
    }
  });

  // Rename columns in the second thread.
  thread_holder.AddThreadFunctor([this, &tablename] {
    for (size_t i = 0; i < kNumIterations;) {
      if (ASSERT_RESULT(RunDdlHelper("ALTER TABLE $0 RENAME COLUMN col_$1 TO renamedcol_$2",
                                     tablename, i, i))) {
        ++i;
      }
    }
  });

  thread_holder.JoinAll();
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  vector<string> expected_cols;
  expected_cols.reserve(kNumIterations * 2);
  expected_cols.emplace_back("key");
  expected_cols.emplace_back("value");
  expected_cols.emplace_back("num");
  for (size_t i = 0;  i < kNumIterations * 2; ++i) {
    expected_cols.push_back(Format(i < kNumIterations ? "renamedcol_$0" : "col_$0", i));
  }
  ASSERT_OK(VerifySchema(client.get(), kDatabase, tablename, expected_cols));
}

// TODO (deepthi) : This test is flaky because of #14995. Re-enable it back after #14995 is fixed.
TEST_F(PgDdlAtomicitySanityTest, YB_DISABLE_TEST(FailureRecoveryTest)) {
  const auto tables_to_create = {kRenameTable, kRenameCol, kAddCol, kDropTable};

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  for (const auto& table : tables_to_create) {
    ASSERT_OK(conn.Execute(CreateTableStmt(table)));
  }

  // Set ysql_transaction_bg_task_wait_ms so high that table rollback is nearly
  // disabled.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_delay_ysql_ddl_rollback_secs", "100"));
  ASSERT_OK(conn.TestFailDdl(RenameTableStmt(kRenameTable)));
  ASSERT_OK(conn.TestFailDdl(RenameColumnStmt(kRenameCol)));
  ASSERT_OK(conn.TestFailDdl(AddColumnStmt(kAddCol)));
  ASSERT_OK(conn.TestFailDdl(DropTableStmt(kDropTable)));

  // Verify that table was created on DocDB.
  VerifyTableExists(client.get(), kDatabase, kDropTable, 10);

  // Verify that the tables were altered on DocDB.
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kRenamedTable, {"key"}));
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kRenameCol, {"key2"}));
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kAddCol, {"key", "value"}));

  // Disable DDL rollback.
  ASSERT_OK(cluster_->SetFlagOnTServers("ysql_yb_ddl_rollback_enabled", "false"));

  // Verify that best effort rollback works when ysql_ddl_rollback is disabled.
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES (1)", kAddCol));
  // The following DDL fails because the table already contains data, so it is not possible to
  // add a new column with a not null constraint.
  ASSERT_NOK(conn.ExecuteFormat("ALTER TABLE $0 ADD COLUMN value2 TEXT NOT NULL"));
  // Verify that the above DDL was rolled back by YSQL.
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kAddCol, {"key", "value"}));

  // Restart the cluster with DDL rollback disabled.
  SetFlagOnAllProcessesWithRollingRestart("--ysql_yb_ddl_rollback_enabled=false");
  // Verify that rollback did not occur even after the restart.
  client = ASSERT_RESULT(cluster_->CreateClient());
  VerifyTableExists(client.get(), kDatabase, kDropTable, 10);
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kRenamedTable, {"key"}));
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kRenameCol, {"key2"}));
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kAddCol, {"key", "value"}));

  // Verify that it is still possible to run DDL on an affected table.
  // Tables having unverified transaction state on them can still be altered if the DDL rollback
  // is not enabled.
  ASSERT_OK(conn.Execute(RenameTableStmt(kRenameTable, "foobar2")));
  ASSERT_OK(conn.Execute(AddColumnStmt(kAddCol, "value2")));

  // Re-enable DDL rollback properly with restart.
  SetFlagOnAllProcessesWithRollingRestart("--ysql_yb_ddl_rollback_enabled=true");

  client = ASSERT_RESULT(cluster_->CreateClient());
  conn = ASSERT_RESULT(Connect());
  // The tables with the transaction state on them must have had their state cleared upon restart
  // since the flag is now enabled again.
  ASSERT_OK(conn.Execute(RenameColumnStmt(kRenameCol)));
  ASSERT_OK(conn.Execute(DropTableStmt(kDropTable)));

  // However add_col_test will be corrupted because we never performed transaction rollback on it.
  // It should ideally have only one added column "value2", but we never rolled back the addition of
  // "value".
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kAddCol, {"key", "value", "value2"}));

  // Add a column to add_col_test which is now corrupted.
  ASSERT_OK(conn.Execute(AddColumnStmt(kAddCol, "value3")));
  // Wait for transaction verification to run.

  // Future DDLs still succeed even though the schema is corrupted. This is because we do not need
  // to compare schemas to determine whether the transaction is a success. PG backend tells the
  // YB-Master the status of the transaction.
  ASSERT_OK(ExecuteWithRetry(&conn, AddColumnStmt(kAddCol, "value4")));

  // Disable PG reporting to Yb-Master.
  ASSERT_OK(cluster_->SetFlagOnTServers("report_ysql_ddl_txn_status_to_master", "false"));

  // Run a DDL on the corrupted table.
  ASSERT_OK(conn.Execute(RenameTableStmt(kAddCol, "foobar3")));
  // However since the PG schema and DocDB schema have become out-of-sync, the above DDL cannot
  // be verified. All future DDL operations will now fail.
  ASSERT_NOK(conn.Execute(RenameColumnStmt(kAddCol)));
  ASSERT_NOK(conn.Execute(DropTableStmt(kAddCol)));
}

TEST_F(PgDdlAtomicitySanityTest, AddReplicaIdentityTest) {
  ASSERT_OK(cluster_->SetFlagOnMasters("ysql_yb_enable_replica_identity", "true"));
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("set yb_enable_replica_identity = true"));

  CreateTable("test_table");
  string ddl_statement = "ALTER TABLE test_table REPLICA IDENTITY FULL;";
  ASSERT_OK(conn.TestFailDdl(ddl_statement));

  // Table should exist with old (default) replica identity intact.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(VerifyReplicaIdentityMatches(
      client.get(), kDatabase, "test_table", PgReplicaIdentity::CHANGE));
}

class PgDdlAtomicityConcurrentDdlTest : public PgDdlAtomicitySanityTest {
 protected:
  bool testFailedDueToTxnVerification(PGConn *conn, const string& cmd) {
    Status s = conn->Execute(cmd);
    if (s.ok()) {
      LOG(ERROR) << "Command " << cmd << " executed successfully when failure expected";
      return false;
    }
    return IsDdlVerificationError(s.ToString());
  }

  void testConcurrentDDL(const string& stmt1, const string& stmt2) {
    // Test that until transaction verification is complete, another DDL operating on
    // the same object cannot go through.
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(ExecuteWithRetry(&conn, stmt1));
    // The first DDL was successful, but the second should fail because rollback has
    // been paused using 'TEST_pause_ddl_rollback'.
    auto conn2 = ASSERT_RESULT(Connect());
    ASSERT_TRUE(testFailedDueToTxnVerification(&conn2, stmt2));
  }

  void testConcurrentFailedDDL(const string& stmt1, const string& stmt2) {
    // Same as 'testConcurrentDDL' but here the first DDL statement is a failure. However
    // other DDLs still cannot happen unless rollback is complete.
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.TestFailDdl(stmt1));
    auto conn2 = ASSERT_RESULT(Connect());
    ASSERT_TRUE(testFailedDueToTxnVerification(&conn2, stmt2));
  }

  const string& table() const {
    return table_;
  }

 private:
  const string database_name_ = "yugabyte";
  const string table_ = "test";
};

TEST_F(PgDdlAtomicityConcurrentDdlTest, ConcurrentDdl) {
  const string kCreateAndAlter = "create_and_alter_test";
  const string kCreateAndDrop = "create_and_drop_test";
  const string kDropAndAlter = "drop_and_alter_test";
  const string kAlterAndAlter = "alter_and_alter_test";
  const string kAlterAndDrop = "alter_and_drop_test";

  const auto tables_to_create = {kDropAndAlter, kAlterAndAlter, kAlterAndDrop};

  auto conn = ASSERT_RESULT(Connect());
  for (const auto& table : tables_to_create) {
    ASSERT_OK(conn.Execute(CreateTableStmt(table)));
  }

  // Test that we can't run a second DDL on a table until the YsqlTxnVerifierState on the first
  // table is cleared.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  testConcurrentDDL(CreateTableStmt(kCreateAndAlter), AddColumnStmt(kCreateAndAlter));
  testConcurrentDDL(CreateTableStmt(kCreateAndDrop), DropTableStmt(kCreateAndDrop));
  testConcurrentDDL(AddColumnStmt(kAlterAndDrop), DropTableStmt(kAlterAndDrop));
  testConcurrentDDL(RenameColumnStmt(kAlterAndAlter), RenameTableStmt(kAlterAndAlter));

  // Test that we can't run a second DDL on a table until the YsqlTxnVerifierState on the first
  // table is cleared even if the first DDL was a failure.
  testConcurrentFailedDDL(DropTableStmt(kDropAndAlter), RenameColumnStmt(kDropAndAlter));
}

class PgDdlAtomicityTxnTest : public PgDdlAtomicitySanityTest {
 public:
  void RunFailingDdlTransaction(const string& ddl_statements) {
    // Normally every DDL auto-commits, and thus we usually have only one DDL statement in a
    // transaction. However, when DDLs are invoked in a function, all the DDLs will be executed
    // in a single atomic transaction if the function itself was invoked as part of a DDL statement.
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.ExecuteFormat(
        "CREATE OR REPLACE FUNCTION ddl_txn_func() RETURNS TABLE (k INT) AS $$ "
        "BEGIN " + ddl_statements +
        " CREATE TABLE t AS SELECT (1) AS k;"
        // The following statement will fail due to NOT NULL constraint.
        " ALTER TABLE t ADD COLUMN v3 int not null;"
        " RETURN QUERY SELECT k FROM t;"
        " END $$ LANGUAGE plpgsql"));
    ASSERT_NOK(conn.Execute("CREATE TABLE txntest AS SELECT k FROM ddl_txn_func()"));
  }

  // Run all the statements in 'ddl_statements' in a single transaction. This will happen if
  // the DDL statements are invoked in a function that is executed by a DDL statement.
  Status RunTransaction(const string& ddl_statements) {
    auto conn = VERIFY_RESULT(Connect());
    RETURN_NOT_OK(conn.ExecuteFormat(
        "CREATE OR REPLACE FUNCTION ddl_txn_func() RETURNS TABLE (k INT) AS $$ "
        "BEGIN " + ddl_statements + " RETURN QUERY SELECT (1) AS k;" +
        " END $$ LANGUAGE plpgsql"));
    return conn.Execute("CREATE TABLE txntest AS SELECT k FROM ddl_txn_func()");
  }

  string table() {
    return "test_table";
  }
};

TEST_F(PgDdlAtomicityTxnTest, CreateAlterDropTest) {
  string ddl_statements = CreateTableStmt(table()) + "; " +
                          CreateIndexStmt("idx", table()) + "; " +
                          AddColumnStmt(table()) +  "; " +
                          RenameColumnStmt(table(), "value", "value_renamed") + "; " +
                          DropColumnStmt(table(), "value_renamed") + "; " +
                          RenameTableStmt(table()) + "; " +
                          DropTableStmt(kRenamedTable) + ";";
  RunFailingDdlTransaction(ddl_statements);
  // Table should not exist.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  VerifyTableNotExists(client.get(), kDatabase, table(), 10);
  VerifyTableNotExists(client.get(), kDatabase, "idx", 10);
}

TEST_F(PgDdlAtomicityTxnTest, CreateDropTest) {
  string ddl_statements = CreateTableStmt(table()) + "; " +
                          DropTableStmt(table()) + "; ";
  RunFailingDdlTransaction(ddl_statements);
  // Table should not exist.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  VerifyTableNotExists(client.get(), kDatabase, table(), 10);
}

TEST_F(PgDdlAtomicityTxnTest, CreateDropColTest) {
  string ddl_statements = CreateTableStmt(table()) + "; " +
                          DropColumnStmt(table()) + "; ";
  RunFailingDdlTransaction(ddl_statements);
  // Table should not exist.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  VerifyTableNotExists(client.get(), kDatabase, table(), 10);
}

TEST_F(PgDdlAtomicityTxnTest, CreateAlterTest) {
  string ddl_statements = CreateTableStmt(table()) + "; " +
                          AddColumnStmt(table()) +  "; " +
                          RenameColumnStmt(table()) + "; " +
                          RenameTableStmt(table()) + "; ";
  RunFailingDdlTransaction(ddl_statements);
  // Table should not exist.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  VerifyTableNotExists(client.get(), kDatabase, table(), 10);
}

TEST_F(PgDdlAtomicityTxnTest, AlterDropTest) {
  CreateTable(table());
  string ddl_statements = RenameColumnStmt(table()) + "; " + DropTableStmt(table()) + "; ";
  RunFailingDdlTransaction(ddl_statements);

  // Table should exist with old schema intact.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(VerifySchema(client.get(), kDatabase, table(), {"key", "value", "num"}));
}

TEST_F(PgDdlAtomicityTxnTest, AddColRenameColTest) {
  CreateTable(table());
  string ddl_statements = AddColumnStmt(table()) + "; " + RenameColumnStmt(table()) + "; ";
  RunFailingDdlTransaction(ddl_statements);

  // Table should exist with old schema intact.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(VerifySchema(client.get(), kDatabase, table(), {"key", "value", "num"}));
}

TEST_F(PgDdlAtomicityTxnTest, AddColDropColTest) {
  CreateTable(table());
  // Insert some rows.
  auto conn = ASSERT_RESULT(Connect());
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES($1, '$2')", table(), i, i));
  }
  const string drop_col_stmt = "ALTER TABLE " + table() + " DROP COLUMN value2";
  string ddl_statements = AddColumnStmt(table()) + "; " + drop_col_stmt + "; ";
  RunFailingDdlTransaction(ddl_statements);

  // Table should exist with old schema intact.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(VerifySchema(client.get(), kDatabase, table(), {"key", "value", "num"}));
  ASSERT_OK(conn.FetchMatrix(Format("SELECT value FROM $0", table()), 5, 1));
}

TEST_F(PgDdlAtomicityTxnTest, AddDropColWithSameNameTest) {
  CreateTable(table());
  // Test that adding and dropping a column with the same name in a single transaction is
  // unsupported.
  Status s = RunTransaction(DropColumnStmt(table()) + ";" + AddColumnStmt(table(), "value") + ";");
  ASSERT_TRUE(s.ToString().find("column value is still in process of deletion") != string::npos);
}

TEST_F(PgDdlAtomicityTxnTest, CreateDropIndexTxn) {
  CreateTable(kIndexedTable);
  string ddl_stmts = CreateIndexStmt(kCreateIndex, kIndexedTable) + "; " +
                     DropIndexStmt(kCreateIndex) + "; ";
  // Create and drop index in the same failing transaction.
  RunFailingDdlTransaction(ddl_stmts);
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  VerifyTableNotExists(client.get(), kDatabase, kCreateIndex, 10);
  // Create and drop index in the same successful transaction.
  ASSERT_OK(RunTransaction(ddl_stmts));
  VerifyTableNotExists(client.get(), kDatabase, kCreateIndex, 10);
}

TEST_F(PgDdlAtomicityTxnTest,
       YB_DISABLE_TEST_IN_TSAN(TestDropColumnSkippingAlterSchema)) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE $0 (a int, b int, PRIMARY KEY (a ASC))", table()));
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 ADD COLUMN c INT", table()));
  const auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(VerifySchema(client.get(), "yugabyte", table(), {"a", "b", "c"}));
  ASSERT_OK(cluster_->SetFlagOnMasters(
      "TEST_ysql_ddl_rollback_failure_probability", "1.0"));
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 DROP COLUMN c", table()));
  ASSERT_OK(VerifySchema(client.get(), "yugabyte", table(), {"a", "b"}));
  ASSERT_OK(conn.ExecuteFormat("UPDATE $0 SET b = 2 WHERE a = 1", table()));
}

class PgDdlAtomicitySanityTestWithTableLocks : public PgDdlAtomicitySanityTest,
                                               public ::testing::WithParamInterface<bool> {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgDdlAtomicitySanityTest::UpdateMiniClusterOptions(options);
    ToggleDDLMode(options->extra_tserver_flags, LegacyDDLMode());
    ToggleDDLMode(options->extra_master_flags, LegacyDDLMode());
  }

  bool LegacyDDLMode() const override { return !GetParam(); }
};

TEST_P(PgDdlAtomicitySanityTestWithTableLocks, DmlWithAddColTest) {
  // Disable object locking for table locks so that we can test the DML with add-column.
  // With table locks, the add-column operation will be blocked until the DML transaction
  // is committed. This is because the add-column operation acquires an EXCLUSIVE lock on the table
  // and the DML transaction acquires an ACCESS_SHARE lock on the table (at ts-1).
  const int kTimeoutSec = 3;
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  const string table = "dml_with_add_col_test";
  CreateTable(table);
  auto conn1 = ASSERT_RESULT(Connect());
  auto conn2 = ASSERT_RESULT(Connect());

  // Conn1: Begin write to the table.
  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.Execute("INSERT INTO " + table + " VALUES (1)"));

  // Conn2: Initiate rollback of the alter.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  ASSERT_OK(conn2.Execute("SET statement_timeout = '" + std::to_string(kTimeoutSec) + "s'"));

  if (!LegacyDDLMode()) {
    // Conn2 will have to fail because it cannot get the table locks held by conn1.
    ASSERT_NOK(conn2.TestFailDdl(AddColumnStmt(table)));
    ASSERT_OK(conn1.Execute("ABORT"));
  } else {
    ASSERT_OK(conn2.TestFailDdl(AddColumnStmt(table)));

    // Conn1: Since we parallely added a column to the table, the add-column operation would have
    // detected the distributed transaction locks acquired by this transaction on its tablets and
    // aborted it. Add-column operation aborts all ongoing transactions on the table without
    // exception as the transaction could now be in an erroneous state having used the schema
    // without the newly added column.
    ASSERT_NOK(conn1.Execute("COMMIT"));
  }

  // Conn1: Non-transactional insert retry due to Schema version mismatch,
  // refreshing the table cache.
  ASSERT_OK(conn1.Execute("INSERT INTO " + table + " VALUES (1)"));

  // Conn1: Start new transaction.
  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.Execute("INSERT INTO " + table + " VALUES (2)"));

  // Rollback happens while the transaction is in progress.
  // Wait for the rollback to complete.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "false"));
  ASSERT_OK(VerifySchema(client.get(), kDatabase, table, {"key", "value", "num"}));

  // Transaction at conn1 succeeds. Normally, drop-column operation would also have aborted all
  // ongoing transactions on this table. However this is a drop-column operation initiated as part
  // of rollback. The column being dropped by this operation was added as part of an uncommitted
  // transaction, so this column would not have been visible to any transaction. It is safe for
  // this drop-column operation to operate silently without aborting any transaction. Therefore this
  // transaction succeeds.
  ASSERT_OK(conn1.Execute("COMMIT"));
}

INSTANTIATE_TEST_CASE_P(
    DmlAddColTestWithTableLocks, PgDdlAtomicitySanityTestWithTableLocks, ::testing::Bool(),
    ::testing::PrintToStringParamName());

// Test that DML transactions concurrent with an aborted DROP TABLE transaction
// can commit successfully (both before and after the rollback is complete).`
TEST_F(PgDdlAtomicitySanityTest, DmlWithDropTableTest) {
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  const string table = "dml_with_drop_test";
  CreateTable(table);
  auto conn1 = ASSERT_RESULT(Connect());
  auto conn2 = ASSERT_RESULT(Connect());
  auto conn3 = ASSERT_RESULT(Connect());

  // Conn1: Begin write to the table.
  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.Execute("INSERT INTO " + table + " VALUES (1)"));

  // Conn2: Also begin write to the table.
  ASSERT_OK(conn2.Execute("BEGIN"));
  ASSERT_OK(conn2.Execute("INSERT INTO " + table + " VALUES (2)"));

  // Conn3: Initiate rollback of DROP table.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  ASSERT_OK(conn3.TestFailDdl(DropTableStmt(table)));

  // Conn1: This should succeed.
  ASSERT_OK(conn1.Execute("INSERT INTO " + table + " VALUES (3)"));
  ASSERT_OK(conn1.Execute("COMMIT"));

  // Wait for rollback to complete.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "false"));
  ASSERT_OK(WaitForDdlVerification(client.get(), kDatabase, table));

  // Conn2 must also commit successfully.
  ASSERT_OK(conn2.Execute("INSERT INTO " + table + " VALUES (4)"));
  ASSERT_OK(conn2.Execute("COMMIT"));
}

// Test that we are able to rollback DROP COLUMN on a column with a missing
// default value.
TEST_F(PgDdlAtomicitySanityTest, DropColumnWithMissingDefaultTest) {
  static const std::string kTable = "drop_column_missing_default_test";
  CreateTable(kTable);
  auto conn = ASSERT_RESULT(Connect());
  // Write some rows to the table.
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0(key) VALUES (generate_series(1, 3))", kTable));
  // Add column with missing default value.
  ASSERT_OK(conn.Execute(Format(AddColumnStmt(kTable, "value2", "default"))));
  // Insert rows with the new column set to null.
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0(key, value2) VALUES (generate_series(4, 6), null)", kTable));
  // Insert rows with the new column set to a non-default value.
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0(key, value2) VALUES (generate_series(7, 9), 'not default')", kTable));
  auto check_table_rows = [&conn] {
    auto rows = ASSERT_RESULT((conn.FetchRows<int32_t, std::optional<std::string>>(Format(
        "SELECT key, value2 FROM $0 ORDER BY key", kTable))));
    size_t idx = 0;
    for (const auto& [key, value] : rows) {
      ASSERT_EQ(key, idx + 1);
      switch(key) {
        case 4: [[fallthrough]];
        case 5: [[fallthrough]];
        case 6:
          ASSERT_EQ(value, std::nullopt);
          break;
        case 7: [[fallthrough]];
        case 8: [[fallthrough]];
        case 9:
          ASSERT_EQ(value, "not default");
          break;
        default:
          ASSERT_EQ(value, "default");
          break;
      }
      ++idx;
    }
  };
  check_table_rows();
  // Fail drop column.
  ASSERT_OK(conn.TestFailDdl(DropColumnStmt(kTable)));
  // Insert more rows.
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0(key) VALUES (generate_series(10, 12))", kTable));
  check_table_rows();
}

class PgDdlAtomicityColocatedTestBase : public PgDdlAtomicitySanityTest {
 protected:
  void SetUp() override {
    LibPqTestBase::SetUp();

    conn_ = std::make_unique<PGConn>(ASSERT_RESULT(ConnectToDB(kDatabaseName)));
  }

  // Test function.
  void sanityTest(const string& tablegroup = "");

  std::unique_ptr<PGConn> conn_;
  string kDatabaseName = "yugabyte";
};

void PgDdlAtomicityColocatedTestBase::sanityTest(const string& tablegroup) {
  const int num_rows = 5;
  ASSERT_OK(SetupTablesForAllDdls(conn_.get(), num_rows, tablegroup));

  ASSERT_OK(RunAllDdlsWithErrorInjection(conn_.get()));

  // Wait for rollback to happen.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(WaitForDdlVerificationAfterDdlFailure(client.get(), kDatabaseName));

  // Verify that all the above operations are rolled back.
  ASSERT_OK(VerifyAllFailingDdlsRolledBack(conn_.get(), client.get(), kDatabaseName));
  ASSERT_OK(VerifyRowsAfterDdlErrorInjection(conn_.get(), num_rows));
}

class PgDdlAtomicityColocatedDbTest : public PgDdlAtomicityColocatedTestBase {
  void SetUp() override {
    LibPqTestBase::SetUp();

    kDatabaseName = "colocateddbtest";
    PGConn conn_init = ASSERT_RESULT(Connect());
    ASSERT_OK(conn_init.ExecuteFormat("CREATE DATABASE $0 WITH colocated = true", kDatabaseName));

    conn_ = std::make_unique<PGConn>(ASSERT_RESULT(ConnectToDB(kDatabaseName)));
  }
};

TEST_F(PgDdlAtomicityColocatedDbTest, ColocatedTest) {
  sanityTest();
}

class PgDdlAtomicityTablegroupTest : public PgDdlAtomicityColocatedTestBase {
  void SetUp() override {
    LibPqTestBase::SetUp();

    conn_ = std::make_unique<PGConn>(ASSERT_RESULT(Connect()));
    ASSERT_OK(conn_->ExecuteFormat("CREATE TABLEGROUP $0", kTablegroup));
  }

 public:
  Result<int> NumTablegroupParentTables (client::YBClient* client) {
    const auto tables = VERIFY_RESULT(client->ListTables());
    int count = 0;
    for (const auto& t : tables) {
      if (t.table_name().find("tablegroup.parent") != string::npos) {
        ++count;
      }
    }
    return count;
  }

  const string kTablegroup = "test_tgroup";
};

TEST_F(PgDdlAtomicityTablegroupTest, TablegroupTableTest) {
  sanityTest(kTablegroup);
}

TEST_F(PgDdlAtomicityTablegroupTest, TablegroupTest) {
  const string tablegroup = "test_tgrp";
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  // Test that CREATE TABLEGROUP is rolled back.
  ASSERT_OK(conn_->TestFailDdl(Format("CREATE TABLEGROUP $0", tablegroup)));
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    return VERIFY_RESULT(NumTablegroupParentTables(client.get())) == 1;
  }, MonoDelta::FromSeconds(10), "Verify number of tablegroups"));

  // Test that failed DROP TABLEGROUP is rolled back.
  ASSERT_OK(conn_->TestFailDdl(Format("DROP TABLEGROUP $0", kTablegroup)));
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    return VERIFY_RESULT(NumTablegroupParentTables(client.get())) == 1;
  }, MonoDelta::FromSeconds(10), "Verify number of tablegroups"));

  // Create many tables in the tablegroup.
  const int num_tables = 10;
  for (int i = 0; i < num_tables; ++i) {
    ASSERT_OK(conn_->ExecuteFormat("CREATE TABLE test_$0 (key INT PRIMARY KEY) TABLEGROUP $1",
                                    i, kTablegroup));
  }

  // Wait for DDL verification to complete.
  SleepFor(5 * 1s);

  // Verify that DROP TABLEGROUP CASCADE can get rolled back successfully.
  ASSERT_OK(conn_->TestFailDdl(Format("DROP TABLEGROUP $0 CASCADE", kTablegroup)));
  // Wait to verify that the drop actually did not go through.
  SleepFor(5 * 1s);
  ASSERT_EQ(ASSERT_RESULT(NumTablegroupParentTables(client.get())), 1);
  for (int i = 0; i < num_tables; ++i) {
    VerifyTableExists(client.get(), kDatabase, Format("test_$0", i), 20);
  }

  // Verify that DROP TABLEGROUP CASCADE can finish successfully.
  ASSERT_OK(conn_->ExecuteFormat("DROP TABLEGROUP $0 CASCADE", kTablegroup));
  // Wait for DDL verification.
  SleepFor(5 * 1s);
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    return VERIFY_RESULT(NumTablegroupParentTables(client.get())) == 0;
  }, MonoDelta::FromSeconds(30), "Verify number of tablegroups"));

  for (int i = 0; i < num_tables; ++i) {
    VerifyTableNotExists(client.get(), kDatabase, Format("test_$0", i), 20);
  }
}

class PgDdlAtomicitySnapshotTest : public PgDdlAtomicitySanityTest {
  void SetUp() override {
    LibPqTestBase::SetUp();

    snapshot_util_ = std::make_unique<client::SnapshotTestUtil>();
    client_ = ASSERT_RESULT(cluster_->CreateClient());
    snapshot_util_->SetProxy(&client_->proxy_cache());
    snapshot_util_->SetCluster(cluster_.get());
  }

 public:
  std::unique_ptr<client::SnapshotTestUtil> snapshot_util_;
  std::unique_ptr<client::YBClient> client_;

  YB_STRONGLY_TYPED_BOOL(ExpectSuccess);
  Status testListSnapshots(yb::pgwrapper::PGConn *conn, DdlErrorInjection inject_error,
      const string& ddl, const TxnSnapshotId& snapshot_id,
      ExpectSuccess expect_success = ExpectSuccess::kTrue) {

    RETURN_NOT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
    if (inject_error) {
      RETURN_NOT_OK(conn->TestFailDdl(ddl));
    } else {
      RETURN_NOT_OK(conn->ExecuteFormat(ddl));
    }
    auto res = snapshot_util_->ListSnapshots(
        snapshot_id, client::ListDeleted::kFalse, client::PrepareForBackup::kTrue);
    if (expect_success) {
      RETURN_NOT_OK(res);
    } else if (res.ok()) {
      return STATUS_FORMAT(IllegalState, "ListSnapshots should have failed for DDL $0", ddl);
    }
    return cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "false");
  }

  Status ListSnapshotTest(DdlErrorInjection inject_error);
};

TEST_F(PgDdlAtomicitySnapshotTest, SnapshotTest) {
  // Create requisite tables.
  const auto tables_to_create = {kAddCol, kDropTable, kDropCol};

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  for (const auto& table : tables_to_create) {
    ASSERT_OK(conn.Execute(CreateTableStmt(table)));
  }

  const int snapshot_interval_secs = 1;
  // Create a snapshot schedule before running all the failed DDLs.
  auto schedule_id = ASSERT_RESULT(
    snapshot_util_->CreateSchedule(kDatabase,
                                   client::WaitSnapshot::kTrue,
                                   MonoDelta::FromSeconds(snapshot_interval_secs)));

  // Run all the failed DDLs.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  ASSERT_OK(conn.TestFailDdl(CreateTableStmt(kCreateTable)));
  ASSERT_OK(conn.TestFailDdl(AddColumnStmt(kAddCol)));
  ASSERT_OK(conn.TestFailDdl(DropTableStmt(kDropTable)));
  ASSERT_OK(conn.TestFailDdl(DropColumnStmt(kDropCol)));

  // Get the hybrid time before the rollback can happen.
  Timestamp current_time(ASSERT_RESULT(WallClock()->Now()).time_point);
  HybridTime hybrid_time_before_rollback = HybridTime::FromMicros(current_time.ToInt64());

  // Ensure that at least one snapshot is taken.
  SleepFor(snapshot_interval_secs * 5s * kTimeMultiplier);

  // Verify that rollback for Alter and Create has indeed not happened yet.
  VerifyTableExists(client.get(), kDatabase, kCreateTable, 10);
  VerifyTableExists(client.get(), kDatabase, kDropTable, 10);
  ASSERT_OK(VerifySchema(client.get(), kDatabase, kAddCol, {"key", "value", "num", "value2"}));

  // Unpause DDL rollback and verify that rollback has happened.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "false"));
  VerifyTableNotExists(client.get(), kDatabase, kCreateTable, 60);

  // Verify all the other tables are unchanged by all of the DDLs.
  for (const auto& table : tables_to_create) {
    ASSERT_OK(VerifySchema(client.get(), kDatabase, table, {"key", "value", "num"}));
  }

  // Run different failing DDL operations on the tables.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  ASSERT_OK(conn.TestFailDdl(RenameTableStmt(kAddCol)));
  ASSERT_OK(conn.TestFailDdl(RenameColumnStmt(kDropTable)));
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "false"));

  // Restore to before rollback.
  LOG(INFO) << "Start restoration to timestamp " << hybrid_time_before_rollback;
  auto snapshot_id =
    ASSERT_RESULT(snapshot_util_->PickSuitableSnapshot(schedule_id, hybrid_time_before_rollback));
  ASSERT_OK(snapshot_util_->RestoreSnapshot(snapshot_id, hybrid_time_before_rollback));

  LOG(INFO) << "Restoration complete";

  // We restored to a point before the first rollback occurred. That DDL transaction should have
  // been re-detected to be a failure and rolled back again.
  VerifyTableNotExists(client.get(), kDatabase, kCreateTable, 60);
  for (const string& table : tables_to_create) {
    ASSERT_OK(VerifySchema(client.get(), kDatabase, table, {"key", "value", "num"}));
  }
}

Status PgDdlAtomicitySnapshotTest::ListSnapshotTest(DdlErrorInjection inject_error) {
  auto conn = VERIFY_RESULT(Connect());
  RETURN_NOT_OK(SetupTablesForAllDdls(&conn, 0 /* num_rows_to_insert */));

  const int snapshot_interval_secs = 1;
  // Create a snapshot schedule before running all the failed DDLs.
  auto schedule_id = VERIFY_RESULT(
      snapshot_util_->CreateSchedule(kDatabase,
                                     client::WaitSnapshot::kTrue,
                                     MonoDelta::FromSeconds(snapshot_interval_secs)));

  // Get the hybrid time before the rollback can happen.
  Timestamp current_time(VERIFY_RESULT(WallClock()->Now()).time_point);
  HybridTime hybrid_time_before_rollback = HybridTime::FromMicros(current_time.ToInt64());

  SleepFor(snapshot_interval_secs * 5s * kTimeMultiplier);
  auto snapshot_id =
      VERIFY_RESULT(snapshot_util_->PickSuitableSnapshot(schedule_id, hybrid_time_before_rollback));

  // Verify that newly created tables do not cause the snapshot to fail as they are not part
  // of the original snapshot.
  RETURN_NOT_OK(testListSnapshots(&conn, inject_error, CreateTableStmt(kCreateTable), snapshot_id));
  RETURN_NOT_OK(testListSnapshots(&conn, inject_error, CreateIndexStmt(kCreateIndex, kIndexedTable),
      snapshot_id));

  vector<string> ddls = {RenameColumnStmt(kRenameCol), AddColumnStmt(kAddCol),
      DropColumnStmt(kDropCol), AddPkStmt(kAddPk), DropPkStmt(kDropPk),
      RenameTableStmt(kRenameTable), RenameIndexStmt(kRenameIndex, kRenamedIndex)};
  vector<string> tables = {kRenameCol, kAddCol, kDropCol, kAddPk, kDropPk,
      inject_error ? kRenameTable : kRenamedTable, inject_error ? kRenameIndex : kRenamedIndex};

  DCHECK_EQ(ddls.size(), tables.size());

  if (inject_error) {
    // Drop is handled separately below for successful Ddl case.
    ddls.insert(ddls.end(), {DropTableStmt(kDropTable), DropIndexStmt(kDropIndex)});
    tables.insert(tables.end(), {kDropTable, kDropIndex});
  }

  auto client = VERIFY_RESULT(cluster_->CreateClient());
  for (size_t i = 0; i< ddls.size(); ++i) {
    RETURN_NOT_OK(testListSnapshots(&conn, inject_error, ddls[i], snapshot_id,
        ExpectSuccess::kFalse));
    // Wait for Ddl verification to complete so that the ddl state on this table does not affect
    // the list snapshot test for the next ddl.
    RETURN_NOT_OK(WaitForDdlVerification(client.get(), kDatabase, tables[i]));
  }

  if (inject_error) {
    return Status::OK();
  }

  // If there is a table marked for deletion, then the snapshot should fail.
  RETURN_NOT_OK(testListSnapshots(&conn, inject_error, DropTableStmt(kDropTable), snapshot_id,
      ExpectSuccess::kFalse));
  // Wait for the table to be deleted before the next test.
  client::VerifyTableNotExists(client.get(), kDatabase, kDropTable, 40);

  // Verify that an index marked for deletion causes ListSnapshots to fail.
  return testListSnapshots(&conn, inject_error, DropIndexStmt(kDropIndex), snapshot_id,
      ExpectSuccess::kFalse);
}

TEST_F(PgDdlAtomicitySnapshotTest, SuccessfulDdlListSnapshotTest) {
  ASSERT_OK(ListSnapshotTest(DdlErrorInjection::kFalse));
}

TEST_F(PgDdlAtomicitySnapshotTest, DdlRollbackListSnapshotTest) {
  ASSERT_OK(ListSnapshotTest(DdlErrorInjection::kTrue));
}

class PgLibPqMatviewTest: public PgDdlAtomicitySanityTest {
 public:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
  }
 protected:
  void MatviewTest();
};

void PgLibPqMatviewTest::MatviewTest() {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE t(id int)"));

  // Test matview creation failure.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(conn.TestFailDdl("CREATE MATERIALIZED VIEW mv AS SELECT * FROM t"));
  // Verify matview creation is rolled back.
  VerifyTableNotExists(client.get(), kDatabase, "mv", 10);

  // Verify successful materialized view creation.
  const string rename_test = "rename_mv";
  const string renamed_name = "foobar";
  const string rename_col_test = "rename_col_mv";

  // Verify alter materialized view fails.
  ASSERT_OK(conn.ExecuteFormat("CREATE MATERIALIZED VIEW $0 AS SELECT * FROM t", rename_test));
  ASSERT_OK(conn.ExecuteFormat("CREATE MATERIALIZED VIEW $0 AS SELECT * FROM t", rename_col_test));
  ASSERT_OK(conn.TestFailDdl(Format("ALTER MATERIALIZED VIEW $0 RENAME TO $1",
      rename_test, renamed_name)));
  ASSERT_OK(conn.TestFailDdl(Format("ALTER MATERIALIZED VIEW $0 RENAME $1 TO $2", rename_col_test,
      "id", "foobar")));
  VerifyTableNotExists(client.get(), kDatabase, renamed_name, 10);
  VerifyTableExists(client.get(), kDatabase, rename_test, 10);
  ASSERT_OK(VerifySchema(client.get(), kDatabase, rename_col_test, {"ybrowid", "id"}));

  // Verify alter materialized view success.
  ASSERT_OK(conn.ExecuteFormat("ALTER MATERIALIZED VIEW $0 RENAME TO $1",
      rename_test, renamed_name));
  ASSERT_OK(conn.ExecuteFormat("ALTER MATERIALIZED VIEW $0 RENAME $1 TO $2", rename_col_test,
      "id", renamed_name));
  VerifyTableNotExists(client.get(), kDatabase, rename_test, 10);
  VerifyTableExists(client.get(), kDatabase, renamed_name, 10);
  ASSERT_OK(VerifySchema(client.get(), kDatabase, rename_col_test, {"ybrowid", renamed_name}));

  // Verify refresh failure.
  ASSERT_OK(conn.Execute("CREATE MATERIALIZED VIEW mv AS SELECT * FROM t"));
  ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1), (2)"));
  ASSERT_OK(conn.TestFailDdl(Format("REFRESH MATERIALIZED VIEW mv")));
  // Wait for rollback to complete.
  SleepFor(2s);
  auto curr_rows = ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT COUNT(*) FROM mv"));
  ASSERT_EQ(curr_rows, 0);

  // Verify refresh success.
  ASSERT_OK(conn.Execute("REFRESH MATERIALIZED VIEW mv"));
  curr_rows = ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT COUNT(*) FROM mv"));
  ASSERT_EQ(curr_rows, 2);

  // Perform another refresh to verify the case where relfilenode of both old and new table no
  // longer matches the oid.
  ASSERT_OK(conn.Execute("INSERT INTO t VALUES (3), (4)"));
  ASSERT_OK(conn.Execute("REFRESH MATERIALIZED VIEW mv"));
  curr_rows = ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT COUNT(*) FROM mv"));
  ASSERT_EQ(curr_rows, 4);
}

TEST_F(PgLibPqMatviewTest, MatviewTestWithoutPgOptimization) {
  ASSERT_OK(cluster_->SetFlagOnTServers("report_ysql_ddl_txn_status_to_master", "false"));
  MatviewTest();
}

TEST_F(PgLibPqMatviewTest, MatviewTest) {
  ASSERT_OK(cluster_->SetFlagOnTServers("report_ysql_ddl_txn_status_to_master", "true"));
  MatviewTest();
}

YB_STRONGLY_TYPED_BOOL(EnableDDLAtomicity);

// TODO(deepthi): Remove the tests for txn GC after 'ysql_yb_ddl_rollback_enabled' is set to true
// by default.
class PgLibPqTableRewrite:
  public PgDdlAtomicitySanityTest,
  public ::testing::WithParamInterface<EnableDDLAtomicity> {
 public:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    PgDdlAtomicitySanityTest::UpdateMiniClusterOptions(options);
    options->extra_tserver_flags.push_back("--ysql_enable_reindex=true");
    if (!GetParam()) {
      options->extra_master_flags.push_back("--ysql_transaction_bg_task_wait_ms=10000");
      // Disable the current version of DDL rollback so that we can test the
      // transaction GC framework.
      options->extra_tserver_flags.push_back("--ysql_yb_ddl_rollback_enabled=false");
    } else {
      options->extra_tserver_flags.push_back("--ysql_yb_ddl_rollback_enabled=true");
    }
  }

 protected:
  void SetupTestData() {
    auto conn = ASSERT_RESULT(Connect());
    for (auto table_name : {kTable, kTable2}) {
      // Create the table.
      ASSERT_OK(conn.ExecuteFormat(
          "CREATE TABLE $0 (a int, b int, PRIMARY KEY (a ASC))", table_name));
      // Execute some rewrites so that the oid and relfilenode don't match.
      ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 DROP CONSTRAINT $0_pkey", kTable));
      ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 ADD PRIMARY KEY (a ASC)", kTable));
      // Insert some data.
      ASSERT_OK(conn.ExecuteFormat(
          "INSERT INTO $0 (a, b) VALUES (generate_series(1, 5), generate_series(1, 5))",
          table_name));
      // Create an index.
      ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0 ON $1 (b DESC)",
          table_name == kTable ? kIndex : "idx2", table_name));
    }
    // Create a materialized view.
    ASSERT_OK(conn.ExecuteFormat(
        "CREATE MATERIALIZED VIEW $0 AS SELECT * FROM $1", kMaterializedView, kTable));
    // Insert some more data into the materialized view's base table.
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 (a, b) VALUES (6, 6)", kTable));
    // Set the index as invalid (so that we can test reindex).
    ASSERT_OK(conn.ExecuteFormat(
        "UPDATE pg_index SET indisvalid='f' WHERE indexrelid = '$0'::regclass", kIndex));
  }
  Status WaitForDroppedTablesCleanup() {
    auto client = VERIFY_RESULT(cluster_->CreateClient());
    return LoggedWaitFor([this, &client]() -> Result<bool> {
      for (auto table_name : {kTable, kTable2, kIndex, kMaterializedView}) {
        if (VERIFY_RESULT(client->ListTables(table_name)).size() != 1) {
          return false;
        }
      }
      return true;
    }, MonoDelta::FromSeconds(60), "Verify that we dropped the stale DocDB tables");
  }
  const std::string kTable = "t1";
  const std::string kTable2 = "t2";
  const std::string kIndex = "idx1";
  const std::string kMaterializedView = "mv";
};

INSTANTIATE_TEST_CASE_P(bool, PgLibPqTableRewrite,
    ::testing::Values(EnableDDLAtomicity::kFalse, EnableDDLAtomicity::kTrue));

// Test that orphaned tables left after failed rewrites are cleaned up.
// TODO(deepthi): Re-enable both these tests in TSAN after #16055 is fixed.
TEST_P(PgLibPqTableRewrite,
       YB_DISABLE_TEST_IN_TSAN(TestTableRewriteRollback)) {
  SetupTestData();
  if (GetParam()) {
    ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  }

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("SET yb_test_fail_table_rewrite_after_creation=true"));
  ASSERT_NOK(conn.ExecuteFormat("REINDEX INDEX $0", kIndex));
  ASSERT_NOK(conn.ExecuteFormat("ALTER TABLE $0 ADD COLUMN c SERIAL", kTable));
  ASSERT_NOK(conn.ExecuteFormat("REFRESH MATERIALIZED VIEW $0", kMaterializedView));
  ASSERT_NOK(conn.ExecuteFormat("TRUNCATE $0", kTable2));

  // Verify that we created orphaned DocDB tables.
  const auto client = ASSERT_RESULT(cluster_->CreateClient());
  for (auto table_name : {kTable, kTable2, kIndex, kMaterializedView}) {
    ASSERT_EQ(ASSERT_RESULT(client->ListTables(table_name)).size(), 2);
  }

  // Verify that we drop the new DocDB tables after failed rewrite operations.
  if (GetParam()) {
    ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "false"));
  }
  ASSERT_OK(WaitForDroppedTablesCleanup());

  // Because rollback was paused, the failed DDLs returned before master reverted the DocDB
  // metadata on the surviving tables. Those reverts are AlterTables that bump the tablet schema
  // version and may run after the orphan drops, so a read can hit a retryable "schema version
  // mismatch" (40001). Wait until reads stop seeing it.
  for (const auto& table_name : {kTable, kTable2}) {
    ASSERT_OK(LoggedWaitFor([&conn, &table_name]() -> Result<bool> {
      const auto res = conn.FetchRows<int32_t, int32_t>(Format("SELECT * FROM $0", table_name));
      if (res.ok()) {
        return true;
      }
      if (IsRetryable(res.status())) {
        return false;
      }
      return res.status();
    }, MonoDelta::FromSeconds(60), Format("Wait for $0 schema version to converge", table_name)));
  }

  // Verify the data.
  ASSERT_OK(conn.ExecuteFormat(
      "UPDATE pg_index SET indisvalid = 't' WHERE indexrelid = '$0'::regclass", kIndex));
  ASSERT_NOK(conn.ExecuteFormat("SELECT c FROM $0", kTable));
  auto rows = ASSERT_RESULT((conn.FetchRows<int32_t, int32_t>(
      Format("SELECT * FROM $0", kTable))));
  ASSERT_EQ(rows, (decltype(rows){{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}}));
  rows = ASSERT_RESULT((conn.FetchRows<int32_t, int32_t>(Format(
      "SELECT * FROM $0 WHERE b = 1", kTable))));
  ASSERT_EQ(rows, (decltype(rows){{1, 1}}));
  rows = ASSERT_RESULT((conn.FetchRows<int32_t, int32_t>(Format(
      "SELECT * FROM $0 ORDER BY a", kMaterializedView))));
  ASSERT_EQ(rows, (decltype(rows){{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}}));
  rows = ASSERT_RESULT((conn.FetchRows<int32_t, int32_t>(Format(
      "SELECT * FROM $0", kTable2))));
  ASSERT_EQ(rows, (decltype(rows){{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}}));
  rows = ASSERT_RESULT((conn.FetchRows<int32_t, int32_t>(Format(
      "SELECT * FROM $0 WHERE b = 1", kTable2))));
  ASSERT_EQ(rows, (decltype(rows){{1, 1}}));
}

// Test that orphaned tables left after successful rewrites are cleaned up.
TEST_P(PgLibPqTableRewrite,
       YB_DISABLE_TEST_IN_TSAN(TestTableRewriteSuccess)) {
  SetupTestData();

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("REINDEX INDEX $0", kIndex));
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 ADD COLUMN c SERIAL", kTable));
  ASSERT_OK(conn.ExecuteFormat("REFRESH MATERIALIZED VIEW $0", kMaterializedView));

  // Verify that we drop the old DocDB tables after successful rewrite operations.
  ASSERT_OK(WaitForDroppedTablesCleanup());

  // Sanity check to ensure we can perform ALTERs on the rewritten tables/materialized view.
  const auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 ADD COLUMN d int", kTable));
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 RENAME COLUMN c TO c_serial", kTable));
  ASSERT_OK(VerifySchema(client.get(), "yugabyte", kTable, {"a", "b", "c_serial", "d"}));
  ASSERT_OK(conn.ExecuteFormat("ALTER MATERIALIZED VIEW $0 RENAME COLUMN a TO a_renamed",
      kMaterializedView));
  ASSERT_OK(VerifySchema(client.get(), "yugabyte", kMaterializedView,
      {"ybrowid", "a_renamed", "b"}));
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 ADD COLUMN c int", kTable2));
  ASSERT_OK(VerifySchema(client.get(), "yugabyte", kTable2, {"a", "b", "c"}));
}

class PgDdlAtomicityMiniClusterTest : public PgMiniTestBase {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_ysql_disable_transparent_cache_refresh_retry) = true;
    pgwrapper::PgMiniTestBase::SetUp();
  }

  Status WaitForDdlVerificationToFinish(client::YBClient* client, const std::string& table_id) {
    return LoggedWaitFor([&]() -> Result<bool> {
      std::shared_ptr<client::YBTableInfo> table_info = std::make_shared<client::YBTableInfo>();
      Synchronizer sync;
      RETURN_NOT_OK(client->GetTableSchemaById(table_id, table_info, sync.AsStatusCallback()));
      RETURN_NOT_OK(sync.Wait());
      bool has_state = table_info->ysql_ddl_txn_verifier_state.has_value() &&
             table_info->ysql_ddl_txn_verifier_state->size() > 0;
      return !has_state;
    }, MonoDelta::FromSeconds(60) * kTimeMultiplier,
        "Wait for DDL verification to finish for table " + table_id);
  }

  // Returns the tables and indexes whose name contains name_filter that the master has not
  // deleted. Tables that are not running yet, or that no longer are, are included: a table left
  // behind by a failed DDL is not necessarily in the RUNNING state.
  Result<std::vector<master::ListTablesResponsePB::TableInfo>> ListTablesNotDeleted(
      const std::string& name_filter) {
    master::ListTablesRequestPB req;
    master::ListTablesResponsePB resp;
    req.set_name_filter(name_filter);
    req.set_include_not_running(true);
    rpc::RpcController controller;
    controller.set_timeout(MonoDelta::FromSeconds(30) * kTimeMultiplier);
    auto proxy = VERIFY_RESULT(cluster_->GetLeaderMasterProxy<master::MasterDdlProxy>());
    RETURN_NOT_OK(proxy.ListTables(req, &resp, &controller));
    RETURN_NOT_OK(ResponseStatus(resp));

    std::vector<master::ListTablesResponsePB::TableInfo> tables;
    for (const auto& table : resp.tables()) {
      if (table.state() != master::SysTablesEntryPB::DELETING &&
          table.state() != master::SysTablesEntryPB::DELETED) {
        tables.push_back(table);
      }
    }
    return tables;
  }

  // Returns whether the sys catalog holds a DDL log entry of the given action for the table. The
  // entry is written together with the change it describes, so it tells whether that change was
  // persisted, which a change that is not applied in memory does not.
  Result<bool> HasDdlLogEntry(const std::string& table_id, const std::string& action) {
    master::DdlLogRequestPB req;
    master::DdlLogResponsePB resp;
    rpc::RpcController controller;
    controller.set_timeout(MonoDelta::FromSeconds(30) * kTimeMultiplier);
    auto proxy = VERIFY_RESULT(cluster_->GetLeaderMasterProxy<master::MasterAdminProxy>());
    RETURN_NOT_OK(proxy.DdlLog(req, &resp, &controller));
    RETURN_NOT_OK(ResponseStatus(resp));

    for (const auto& entry : resp.entries()) {
      if (entry.table_id() == table_id && entry.action() == action) {
        LOG(INFO) << "Found DDL log entry: " << entry.ShortDebugString();
        return true;
      }
    }
    return false;
  }

  Status WaitForNoTablesLeftBehind(const std::string& name_filter, const std::string& description) {
    return LoggedWaitFor([this, &name_filter]() -> Result<bool> {
      auto tables = VERIFY_RESULT(ListTablesNotDeleted(name_filter));
      for (const auto& table : tables) {
        LOG(INFO) << "Table left behind: " << table.ShortDebugString();
      }
      return tables.empty();
    }, MonoDelta::FromSeconds(120) * kTimeMultiplier, description);
  }

  // Removes the index from the index list of its indexed table, in memory and in the sys catalog.
  Status UnlinkIndexFromTable(const TableId& indexed_table_id, const TableId& index_id) {
    auto& catalog_manager = VERIFY_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager_impl();
    auto indexed_table = catalog_manager.GetTableInfo(indexed_table_id);
    SCHECK(indexed_table, NotFound, Format("Table $0 not found", indexed_table_id));
    auto l = indexed_table->LockForWrite();
    auto* indexes = l.mutable_data()->pb.mutable_indexes();
    auto it = std::find_if(indexes->begin(), indexes->end(), [&index_id](const auto& index) {
      return index.table_id() == index_id;
    });
    SCHECK(it != indexes->end(), IllegalState,
           Format("Index $0 not listed in table $1", index_id, indexed_table_id));
    indexes->erase(it);
    RETURN_NOT_OK(catalog_manager.sys_catalog()->Upsert(
        catalog_manager.GetLeaderEpochInternal(), indexed_table));
    l.Commit();
    return Status::OK();
  }

  // Writes the table to the sys catalog in the given state, leaving the in-memory state as is. For
  // DELETED, its tablets are written as DELETED too, and the master does not load the table once it
  // reloads the sys catalog.
  Status WriteTableStateToSysCatalog(
      const TableId& table_id, master::SysTablesEntryPB::State state) {
    auto& catalog_manager = VERIFY_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager_impl();
    auto table = catalog_manager.GetTableInfo(table_id);
    SCHECK(table, NotFound, Format("Table $0 not found", table_id));
    master::TabletInfos tablets;
    if (state == master::SysTablesEntryPB::DELETED) {
      tablets = VERIFY_RESULT(table->GetTablets());
    }
    auto l = table->LockForWrite();
    l.mutable_data()->set_state(state, "Set by test");
    std::vector<master::TabletInfo::WriteLock> tablet_locks;
    for (const auto& tablet : tablets) {
      tablet_locks.push_back(tablet->LockForWrite());
      tablet_locks.back().mutable_data()->set_state(
          master::SysTabletsEntryPB::DELETED, "Deleted by test");
    }
    // The locks are released without committing, so only the sys catalog has the change.
    return catalog_manager.sys_catalog()->Upsert(
        catalog_manager.GetLeaderEpochInternal(), table, tablets);
  }

  // Commits a DROP TABLE on a new table with an index while DDL verification is paused, so the
  // table and the index stay running with the drop pending. Returns the table and index ids.
  Result<std::pair<TableId, TableId>> DropTableWithIndexPendingVerification(
      const std::string& table_name) {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_transaction_wait_for_ddl_verification) = false;
    auto client = VERIFY_RESULT(cluster_->CreateClient());
    auto conn = VERIFY_RESULT(Connect());
    RETURN_NOT_OK(conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v INT)", table_name));
    RETURN_NOT_OK(conn.ExecuteFormat("CREATE INDEX $0_idx ON $0 (v)", table_name));
    const auto table_id =
        VERIFY_RESULT(GetTableIdByTableName(client.get(), "yugabyte", table_name));
    const auto index_id =
        VERIFY_RESULT(GetTableIdByTableName(client.get(), "yugabyte", table_name + "_idx"));
    RETURN_NOT_OK(WaitForDdlVerificationToFinish(client.get(), table_id));
    RETURN_NOT_OK(WaitForDdlVerificationToFinish(client.get(), index_id));

    ANNOTATE_UNPROTECTED_WRITE(FLAGS_report_ysql_ddl_txn_status_to_master) = false;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_ysql_ddl_txn_verification) = true;
    RETURN_NOT_OK(conn.ExecuteFormat("DROP TABLE $0", table_name));

    auto& catalog_manager = VERIFY_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    auto table = catalog_manager.GetTableInfo(table_id);
    auto index = catalog_manager.GetTableInfo(index_id);
    SCHECK(table && table->LockForRead()->is_running(), IllegalState, "Table is not running");
    SCHECK(index && index->LockForRead()->is_running(), IllegalState, "Index is not running");
    SCHECK(index->LockForRead()->has_ysql_ddl_txn_verifier_state(), IllegalState,
           "Index has no pending DDL verification");
    return std::make_pair(table_id, index_id);
  }
};

TEST_F(PgDdlAtomicityMiniClusterTest, TestWaitForRollbackWithMasterRestart) {
  SyncPoint::GetInstance()->LoadDependency(
      {{"YsqlDdlHandler::IsYsqlDdlVerificationDone:Fail",
        "PgDdlAtomicitySanityTest::TestWaitForRollbackWithMasterRestart:WaitForFail"}});
  SyncPoint::GetInstance()->EnableProcessing();

  auto conn = ASSERT_RESULT(Connect());
  const auto kDropCol = "drop_col";
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (key INT PRIMARY KEY, value TEXT, num real)",
                               kDropCol));

  TestThreadHolder thread_holder;

  // Fetch the table id before starting the thread.
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kDropCol));
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_hang_on_ddl_verification_progress) = true;

  // Start the thread to drop the column.
  thread_holder.AddThreadFunctor([&stop = thread_holder.stop_flag(), &conn, kDropCol] {
    ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 DROP COLUMN value", kDropCol));
  });

  // Wait until the alter operation hits the sync point in ysql_ddl_handler.
  TEST_SYNC_POINT("PgDdlAtomicitySanityTest::TestWaitForRollbackWithMasterRestart:WaitForFail");

  // Restart master to simulate the case where IsYsqlDdlVerificationDone poller in YSQL spans a
  // master restart.
  ASSERT_OK(RestartMaster());

  // Allow verification to proceed and wait for thread to finish.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_hang_on_ddl_verification_progress) = false;
  thread_holder.JoinAll();

  // Verify that alter table was successful.
  std::shared_ptr<client::YBTableInfo> table_info = std::make_shared<client::YBTableInfo>();
  Synchronizer sync;
  ASSERT_OK(client->GetTableSchemaById(table_id, table_info, sync.AsStatusCallback()));
  ASSERT_OK(sync.Wait());

  const auto& columns = table_info->schema.columns();
  ASSERT_EQ(columns.size(), 2);
  ASSERT_EQ(columns[0].name(), "key");
  ASSERT_EQ(columns[1].name(), "num");
}

// Test that the ALTER TABLE operation is rolled back if a master crash occurs in the middle of the
// ALTER TABLE operation (after the new DocDB schema is committed in-memory).
TEST_F(PgDdlAtomicityMiniClusterTest, AlterTableRollbackOnMasterCrash) {
  SyncPoint::GetInstance()->LoadDependency(
      {{"YBBackupTestWithColocationParam::AlterTableDocDBTableCommitted",
        "PgDdlAtomicitySanityTest::MasterRestart"}});
  SyncPoint::GetInstance()->EnableProcessing();
  // Prevent tserver from reporting the DDL txn status or waiting for verification
  // before we restart the master leader.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_report_ysql_ddl_txn_status_to_master) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_transaction_wait_for_ddl_verification) = false;
  auto conn = ASSERT_RESULT(Connect());
  const auto kTableName = "drop_col";
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (key INT PRIMARY KEY, value INT)", kTableName));

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));

  // Wait for DDL verification of the previous create table operation to finish before starting the
  // alter table operation. This is needed because we disabled
  // ysql_ddl_transaction_wait_for_ddl_verification flag.
  ASSERT_OK(WaitForDdlVerificationToFinish(client.get(), table_id));
  TestThreadHolder thread_holder;

  // Inject master-side failure right after the new DocDB schema is committed in-memory.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_fail_alter_table_after_commit) = true;

  // Start the thread to add a new column.
  thread_holder.AddThreadFunctor([&stop = thread_holder.stop_flag(), &conn, kTableName] {
    ASSERT_NOK(conn.ExecuteFormat("ALTER TABLE $0 ADD COLUMN num REAL", kTableName));
  });
  thread_holder.JoinAll();
  // Wait until the alter operation commits the new DocDB schema.
  TEST_SYNC_POINT("PgDdlAtomicitySanityTest::MasterRestart");
  // Pause DDL rollback so we can check the reloaded TableInfo still has the
  // ysql_ddl_txn_verifier_state before it is cleared by a rollback.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_ddl_rollback) = true;

  // Restart master to simulate the case where a new master leader should rollback the alter table
  // operation (after the new DocDB schema is committed in-memory).
  ASSERT_OK(RestartMaster());
  // Ensure verifier state exists
  {
    std::shared_ptr<client::YBTableInfo> info = std::make_shared<client::YBTableInfo>();
    Synchronizer s;
    ASSERT_OK(client->GetTableSchemaById(table_id, info, s.AsStatusCallback()));
    ASSERT_OK(s.Wait());
    ASSERT_TRUE(info->ysql_ddl_txn_verifier_state.has_value());
    ASSERT_GT(info->ysql_ddl_txn_verifier_state->size(), 0);
  }

  // Unpause DDL rollback.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_ddl_rollback) = false;
  // Wait for DDL transaction verification to complete after the new master reloads
  // syscatalog (i.e. the verifier state is cleared).
  ASSERT_OK(WaitForDdlVerificationToFinish(client.get(), table_id));
  // Verify that alter table was rolled back.
  std::shared_ptr<client::YBTableInfo> table_info = std::make_shared<client::YBTableInfo>();
  Synchronizer sync;
  ASSERT_OK(client->GetTableSchemaById(table_id, table_info, sync.AsStatusCallback()));
  ASSERT_OK(sync.Wait());
  LOG(INFO) << "table_info after rollback: " << table_info->schema.ToString();
  const auto& columns = table_info->schema.columns();
  ASSERT_EQ(columns.size(), 2);
  ASSERT_EQ(columns[0].name(), "key");
  ASSERT_EQ(columns[1].name(), "value");
}

// Fails a CREATE TABLE with a unique index after the index and the table that references it are
// persisted. Expects neither the table nor the index to be left behind, before or after a master
// restart.
TEST_F(PgDdlAtomicityMiniClusterTest, CreateTableWithIndexFailure) {
  // The create table abort flag only applies to tables with this name prefix.
  const auto kTableName = "test_create_abort_t"s;
  auto conn = ASSERT_RESULT(Connect());

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_abort_create_table) = 2;
  auto s = conn.ExecuteFormat("CREATE TABLE $0 (col TEXT UNIQUE, value TEXT)", kTableName);
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_abort_create_table) = 0;
  ASSERT_NOK(s);
  ASSERT_STR_CONTAINS(s.ToString(), "TEST: Aborting due to FLAGS_TEST_abort_create_table");

  // The rollback of the create table must leave neither the table nor its index behind, also after
  // the master rebuilds its state from the sys catalog.
  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the create table rollback to complete"));
  ASSERT_OK(RestartMaster());
  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the reloaded catalog to have nothing left behind"));
}

// Restarts the master after a DROP TABLE on a table with indexes is written to the sys catalog and
// before it is applied in memory. Expects the table and both indexes to be deleted.
TEST_F(PgDdlAtomicityMiniClusterTest, DropTableWithIndexesOnMasterCrash) {
  const auto kTableName = "drop_with_indexes"s;
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v1 INT, v2 INT)", kTableName));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0_idx1 ON $0 (v1)", kTableName));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0_idx2 ON $0 (v2)", kTableName));
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));
  const auto index1_id =
      ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName + "_idx1"));
  const auto index2_id =
      ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName + "_idx2"));
  const std::vector<TableId> dropped_table_ids = {table_id, index1_id, index2_id};

  // The master persists the deletion and then fails before it applies it in memory. Rollback stays
  // paused across the restart, so the check sees only what the failed master persisted. The failed
  // master does not retry the drop, so no rollback is paused while it shuts down.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_transaction_wait_for_ddl_verification) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_post_processing_failed_verification_retry_secs) = 0;
  auto se = ScopeExit([] {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = false;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_ddl_rollback) = false;
  });
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = true;
  ASSERT_OK(conn.ExecuteFormat("DROP TABLE $0", kTableName));

  ASSERT_OK(LoggedWaitFor([this, &table_id]() -> Result<bool> {
    return HasDdlLogEntry(table_id, "Drop");
  }, MonoDelta::FromSeconds(120) * kTimeMultiplier,
      "Wait for the deletion to be written to the sys catalog"));

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_ddl_rollback) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = false;
  ASSERT_OK(RestartMaster());
  {
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    for (const auto& id : dropped_table_ids) {
      auto table = catalog_manager.GetTableInfo(id);
      ASSERT_TRUE(table == nullptr || table->LockForRead()->started_deleting()) << id;
    }
  }
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_ddl_rollback) = false;

  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the table and its indexes to be deleted"));
}

// Restarts the master after a DROP INDEX is written to the sys catalog and before it is applied in
// memory. Expects the index to be deleted and the indexed table to no longer list it.
TEST_F(PgDdlAtomicityMiniClusterTest, DropIndexOnMasterCrash) {
  const auto kTableName = "drop_index_crash"s;
  const auto kIndexName = kTableName + "_idx";
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v INT)", kTableName));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0 ON $1 (v)", kIndexName, kTableName));
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));
  const auto index_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kIndexName));
  ASSERT_OK(WaitForDdlVerificationToFinish(client.get(), table_id));
  ASSERT_OK(WaitForDdlVerificationToFinish(client.get(), index_id));

  auto lists_index = [&index_id](const master::TableInfo& table) {
    return !table.GetIndexInfo(index_id).table_id().empty();
  };
  uint32_t version_before_drop;
  {
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    auto table = catalog_manager.GetTableInfo(table_id);
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(lists_index(*table));
    version_before_drop = table->LockForRead()->pb.version();
  }

  // The master persists the deletion and then fails before it applies it in memory.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_transaction_wait_for_ddl_verification) = false;
  auto se = ScopeExit([] {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = false;
  });
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = true;
  ASSERT_OK(conn.ExecuteFormat("DROP INDEX $0", kIndexName));

  // The DDL log entry of an index drop is recorded under the indexed table.
  ASSERT_OK(LoggedWaitFor([this, &table_id, &kIndexName]() -> Result<bool> {
    return HasDdlLogEntry(table_id, "Drop index " + kIndexName);
  }, MonoDelta::FromSeconds(120) * kTimeMultiplier,
      "Wait for the deletion to be written to the sys catalog"));

  {
    // The failed master did not apply the deletion in memory.
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    auto index = catalog_manager.GetTableInfo(index_id);
    ASSERT_NE(index, nullptr);
    ASSERT_TRUE(index->LockForRead()->is_running());
    auto table = catalog_manager.GetTableInfo(table_id);
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(lists_index(*table));
  }
  ASSERT_OK(RestartMaster());
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = false;
  {
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    auto index = catalog_manager.GetTableInfo(index_id);
    ASSERT_TRUE(index == nullptr || index->LockForRead()->started_deleting());
    auto table = catalog_manager.GetTableInfo(table_id);
    ASSERT_NE(table, nullptr);
    ASSERT_FALSE(table->LockForRead()->started_deleting());
    ASSERT_FALSE(lists_index(*table));
    ASSERT_GT(table->LockForRead()->pb.version(), version_before_drop);
  }
  ASSERT_OK(LoggedWaitFor([this, &index_id]() -> Result<bool> {
    auto* mini_master = VERIFY_RESULT(cluster_->GetLeaderMiniMaster());
    auto index = mini_master->catalog_manager().GetTableInfo(index_id);
    return index == nullptr || index->LockForRead()->is_deleted();
  }, MonoDelta::FromSeconds(120) * kTimeMultiplier, "Wait for the index to be deleted"));
  auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
  auto table = catalog_manager.GetTableInfo(table_id);
  ASSERT_NE(table, nullptr);
  ASSERT_FALSE(table->LockForRead()->started_deleting());
  ASSERT_FALSE(lists_index(*table));

  // The indexed table finishes the alter that removes the index and stays usable.
  ASSERT_OK(LoggedWaitFor([&table]() -> Result<bool> {
    return table->LockForRead()->pb.state() == master::SysTablesEntryPB::RUNNING;
  }, MonoDelta::FromSeconds(120) * kTimeMultiplier, "Wait for the indexed table to be running"));
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES (1, 10)", kTableName));
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int32_t>(
      Format("SELECT v FROM $0 WHERE k = 1", kTableName))), 10);
}

// Restarts the master after the rollback of a CREATE TABLE with a unique index is written to the
// sys catalog and before it is applied in memory. Expects the table and index to be deleted.
TEST_F(PgDdlAtomicityMiniClusterTest, CreateTableWithIndexRollbackOnMasterCrash) {
  const auto kTableName = "rollback_with_index"s;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_report_ysql_ddl_txn_status_to_master) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_transaction_wait_for_ddl_verification) = false;
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());

  // The failed master does not retry the rollback, so none is paused while it shuts down.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_post_processing_failed_verification_retry_secs) = 0;
  auto se = ScopeExit([] {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = false;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_ddl_rollback) = false;
  });
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = true;
  ASSERT_OK(conn.TestFailDdl(
      Format("CREATE TABLE $0 (a INT, b INT, CONSTRAINT $0_uniq UNIQUE (b))", kTableName)));
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));
  const auto index_id =
      ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName + "_uniq"));

  ASSERT_OK(LoggedWaitFor([this, &table_id]() -> Result<bool> {
    return HasDdlLogEntry(table_id, "Drop");
  }, MonoDelta::FromSeconds(120) * kTimeMultiplier,
      "Wait for the rollback to be written to the sys catalog"));

  // Rollback stays paused across the restart, so the check sees only what the failed master
  // persisted.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_ddl_rollback) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = false;
  ASSERT_OK(RestartMaster());
  {
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    for (const auto& id : {table_id, index_id}) {
      auto table = catalog_manager.GetTableInfo(id);
      ASSERT_TRUE(table == nullptr || table->LockForRead()->started_deleting()) << id;
    }
  }
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_ddl_rollback) = false;

  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the table and its index to be deleted"));
}

// Restarts the master with an aborted CREATE TABLE whose unique index is not listed in the
// table's index list. Expects the table and the index to be deleted.
TEST_F(PgDdlAtomicityMiniClusterTest, AbortedCreateTableWithUnlistedIndex) {
  const auto kTableName = "unlisted_index"s;
  const auto kIndexName = kTableName + "_uniq";
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_report_ysql_ddl_txn_status_to_master) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_transaction_wait_for_ddl_verification) = false;
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());

  auto se = ScopeExit([] {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_ysql_ddl_txn_verification) = false;
  });
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_ysql_ddl_txn_verification) = true;
  ASSERT_OK(conn.TestFailDdl(
      Format("CREATE TABLE $0 (a INT, b INT, CONSTRAINT $1 UNIQUE (b))", kTableName, kIndexName)));
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));
  const auto index_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kIndexName));
  ASSERT_OK(UnlinkIndexFromTable(table_id, index_id));

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_ysql_ddl_txn_verification) = false;
  ASSERT_OK(RestartMaster());

  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the table and its index to be deleted"));
}

// Drops a colocated index that is not listed in its indexed table's index list while a CREATE
// INDEX on the same table runs. Expects both to complete.
TEST_F(PgDdlAtomicityMiniClusterTest, DropUnlistedIndexConcurrentWithCreateIndex) {
  const auto kDbName = "colo_db"s;
  const auto kTableName = "unlisted_colocated_index"s;
  const auto kIndexName = kTableName + "_idx1";
  {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0 WITH colocation = true", kDbName));
  }
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(ConnectToDB(kDbName));
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v1 INT, v2 INT)", kTableName));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0 ON $1 (v1)", kIndexName, kTableName));
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDbName, kTableName));
  const auto index_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDbName, kIndexName));
  ASSERT_OK(WaitForDdlVerificationToFinish(client.get(), table_id));
  ASSERT_OK(WaitForDdlVerificationToFinish(client.get(), index_id));
  ASSERT_OK(UnlinkIndexFromTable(table_id, index_id));

  auto create_conn = ASSERT_RESULT(ConnectToDB(kDbName));
  // The drop pauses after its change is applied in memory, until the CREATE INDEX has locked the
  // indexed table, which the drop leaves unchanged.
  CountDownLatch drop_paused(1);
  CountDownLatch resume_drop(1);
  CountDownLatch indexed_table_locked(1);
  SyncPoint::GetInstance()->SetCallBack(
      "PersistDeleteTableOperation::TableCommitted", [&](void* arg) {
        if (static_cast<master::TableInfo*>(arg)->id() == index_id) {
          drop_paused.CountDown();
          resume_drop.WaitFor(MonoDelta::FromSeconds(60) * kTimeMultiplier);
        }
      });
  SyncPoint::GetInstance()->SetCallBack("CreateTable::IndexedTableLocked", [&](void* arg) {
    if (static_cast<master::TableInfo*>(arg)->id() == table_id) {
      indexed_table_locked.CountDown();
    }
  });
  SyncPoint::GetInstance()->EnableProcessing();

  std::atomic<bool> drop_done{false};
  std::atomic<bool> create_done{false};
  TestThreadHolder thread_holder;
  // Resumes the drop and removes the callback on every exit, before the threads are joined.
  auto se = ScopeExit([&resume_drop] {
    resume_drop.CountDown();
    SyncPoint::GetInstance()->DisableProcessing();
    SyncPoint::GetInstance()->ClearAllCallBacks();
  });
  thread_holder.AddThreadFunctor([&client, &index_id, &drop_done] {
    ASSERT_OK(client->DeleteIndexTable(
        index_id, nullptr /* indexed_table_name */, false /* wait */));
    drop_done = true;
  });
  ASSERT_TRUE(drop_paused.WaitFor(MonoDelta::FromSeconds(60) * kTimeMultiplier));
  {
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    auto indexed_table = catalog_manager.GetTableInfo(table_id);
    ASSERT_NE(indexed_table, nullptr);
    auto probe_lock = ASSERT_RESULT(indexed_table->TryLockForWrite(
        CoarseMonoClock::now() + MonoDelta::FromSeconds(10) * kTimeMultiplier));
  }

  thread_holder.AddThreadFunctor([&create_conn, &kTableName, &create_done] {
    ASSERT_OK(create_conn.ExecuteFormat("CREATE INDEX $0_idx2 ON $0 (v2)", kTableName));
    create_done = true;
  });
  ASSERT_TRUE(indexed_table_locked.WaitFor(MonoDelta::FromSeconds(60) * kTimeMultiplier));
  resume_drop.CountDown();

  thread_holder.JoinAll();
  ASSERT_TRUE(drop_done);
  ASSERT_TRUE(create_done);
}

// Restarts the master with a committed DROP TABLE whose indexed table is already deleted from the
// sys catalog while its index is still running. Expects the index to be deleted.
TEST_F(PgDdlAtomicityMiniClusterTest, DropTableWithIndexAfterTableDeleted) {
  const auto kTableName = "deleted_indexed_table"s;
  const auto [table_id, index_id] =
      ASSERT_RESULT(DropTableWithIndexPendingVerification(kTableName));
  std::vector<TabletId> tablet_ids;
  {
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    auto table = catalog_manager.GetTableInfo(table_id);
    ASSERT_NE(table, nullptr);
    for (const auto& tablet : ASSERT_RESULT(table->GetTablets())) {
      tablet_ids.push_back(tablet->id());
    }
  }
  ASSERT_OK(WriteTableStateToSysCatalog(table_id, master::SysTablesEntryPB::DELETED));

  // Failed verifications are retried every second after the restart.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_post_processing_failed_verification_retry_secs) = 1;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_ysql_ddl_txn_verification) = false;
  ASSERT_OK(RestartMaster());
  {
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
    ASSERT_EQ(catalog_manager.GetTableInfo(table_id), nullptr);
    for (const auto& tablet_id : tablet_ids) {
      auto tablet = catalog_manager.GetTabletInfo(tablet_id);
      if (tablet.ok()) {
        ASSERT_TRUE((*tablet)->LockForRead()->is_deleted()) << tablet_id;
      } else {
        ASSERT_TRUE(tablet.status().IsNotFound() || tablet.status().IsDeleted()) << tablet.status();
      }
    }
  }

  ASSERT_OK(WaitForNoTablesLeftBehind(kTableName, "Wait for the index to be deleted"));
}

// Restarts the master with a committed DROP TABLE whose indexed table is DELETING in the sys
// catalog while its index is still running. Expects the index to be deleted and the table to stay
// deleted.
TEST_F(PgDdlAtomicityMiniClusterTest, DropTableWithIndexAfterTableMarkedDeleting) {
  const auto kTableName = "deleting_indexed_table"s;
  const auto [table_id, index_id] =
      ASSERT_RESULT(DropTableWithIndexPendingVerification(kTableName));
  ASSERT_OK(WriteTableStateToSysCatalog(table_id, master::SysTablesEntryPB::DELETING));

  // Failed verifications are retried every second after the restart.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_post_processing_failed_verification_retry_secs) = 1;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_ysql_ddl_txn_verification) = false;
  ASSERT_OK(RestartMaster());

  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the index to be deleted"));
  auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();
  auto table = catalog_manager.GetTableInfo(table_id);
  ASSERT_TRUE(table == nullptr || table->LockForRead()->started_deleting());
}

class PgDdlAtomicityTxnBlockMiniClusterTest : public PgDdlAtomicityMiniClusterTest {
 protected:
  void SetUp() override {
    ToggleDDLMode(/* use_legacy = */ false);
    PgDdlAtomicityMiniClusterTest::SetUp();
  }
};

// Commits a transaction that alters a table with an index and then drops it, so the table comes
// before the index in the transaction's DDL verification and the index is verified while the drop
// is being applied. Expects the index's verification to find the index already being deleted, and
// no table's verification to fail.
TEST_F(PgDdlAtomicityTxnBlockMiniClusterTest, DropTableWithIndexAfterAlterInSameTransaction) {
  const auto kTableName = "altered_then_dropped"s;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_transaction_wait_for_ddl_verification) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_post_processing_failed_verification_retry_secs) = 0;
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v INT)", kTableName));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0_idx ON $0 (v)", kTableName));
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));
  const auto index_id =
      ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName + "_idx"));
  ASSERT_OK(WaitForDdlVerificationToFinish(client.get(), table_id));
  ASSERT_OK(WaitForDdlVerificationToFinish(client.get(), index_id));

  CountDownLatch table_committed(1);
  CountDownLatch index_state_read(1);
  // Counted down when the index's verification finds the index deleting or any verification fails.
  CountDownLatch index_verification_finished(1);
  // The first index check's result: 1 if the table's drop was committed by then, 0 if not.
  std::atomic<int> first_index_check_result{-1};
  std::atomic<bool> index_state_read_before_commit{false};
  std::atomic<bool> table_verification_failed{false};
  std::atomic<bool> index_already_deleting{false};
  // Set before COMMIT so the verification callbacks can ignore other transactions.
  TransactionId txn_id = TransactionId::Nil();
  std::atomic<bool> txn_id_known{false};
  auto* sync_point = SyncPoint::GetInstance();
  sync_point->SetCallBack("PersistDeleteTableOperation::BeforeCommit", [&](void*) {
    if (txn_id_known) {
      index_state_read_before_commit =
          index_state_read.WaitFor(MonoDelta::FromSeconds(60) * kTimeMultiplier);
    }
  });
  sync_point->SetCallBack("PersistDeleteTableOperation::TableCommitted", [&](void* arg) {
    if (static_cast<master::TableInfo*>(arg)->id() == table_id) {
      table_committed.CountDown();
    }
  });
  sync_point->SetCallBack("YsqlDdlTxnCompleteCallback::AfterIndexStateRead", [&](void*) {
    if (!txn_id_known) {
      return;
    }
    index_state_read.CountDown();
    const int result =
        table_committed.WaitFor(MonoDelta::FromSeconds(60) * kTimeMultiplier) ? 1 : 0;
    int expected = -1;
    first_index_check_result.compare_exchange_strong(expected, result);
  });
  sync_point->SetCallBack("YsqlDdlTxnCompleteCallback::TableVerificationDone", [&](void* arg) {
    const auto& verification =
        *static_cast<std::tuple<master::TableInfo*, const TransactionId*, Status*>*>(arg);
    const auto* txn = std::get<1>(verification);
    const auto* status = std::get<2>(verification);
    if (!txn_id_known || *txn != txn_id) {
      return;
    }
    if (!status->ok()) {
      table_verification_failed = true;
      index_verification_finished.CountDown();
    }
  });
  sync_point->SetCallBack("YsqlDdlTxnCompleteCallbackInternal::AlreadyDeleting", [&](void* arg) {
    if (txn_id_known && static_cast<master::TableInfo*>(arg)->id() == index_id) {
      index_already_deleting = true;
      index_verification_finished.CountDown();
    }
  });
  // Keeps the index from being marked DELETED before its verification finishes.
  sync_point->SetCallBack("CatalogManager::PrepareTableDeletion", [&](void* arg) {
    if (txn_id_known && static_cast<master::TableInfo*>(arg)->id() == index_id) {
      index_verification_finished.WaitFor(MonoDelta::FromSeconds(60) * kTimeMultiplier);
    }
  });
  sync_point->EnableProcessing();
  auto se = ScopeExit([&] {
    table_committed.CountDown();
    index_state_read.CountDown();
    index_verification_finished.CountDown();
    sync_point->DisableProcessing();
    sync_point->ClearAllCallBacks();
  });

  ASSERT_OK(conn.Execute("BEGIN"));
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE $0 ADD COLUMN v2 INT", kTableName));
  ASSERT_OK(conn.ExecuteFormat("DROP TABLE $0", kTableName));
  auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager_impl();
  txn_id = ASSERT_RESULT(FullyDecodeTransactionId(
      catalog_manager.GetTableInfo(table_id)->LockForRead()->pb_transaction_id()));
  txn_id_known = true;
  ASSERT_OK(conn.Execute("COMMIT"));

  ASSERT_TRUE(index_verification_finished.WaitFor(MonoDelta::FromSeconds(60) * kTimeMultiplier));
  ASSERT_FALSE(table_verification_failed);
  ASSERT_TRUE(index_already_deleting);
  ASSERT_EQ(first_index_check_result.load(), 1);
  ASSERT_TRUE(index_state_read_before_commit);
  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the table and its index to be deleted"));
  ASSERT_OK(LoggedWaitFor(
      [&catalog_manager, &txn_id] {
        return !catalog_manager.TEST_GetYsqlDdlVerificationState(txn_id).has_value();
      },
      MonoDelta::FromSeconds(30) * kTimeMultiplier,
      "Wait for the DDL verification of the transaction to complete"));
}

class PgDdlAtomicitySavepointMiniClusterTest : public PgDdlAtomicityMiniClusterTest {
 protected:
  void SetUp() override {
    ToggleDDLMode(/* use_legacy = */ false);
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_yb_enable_ddl_savepoint_support) = true;
    PgDdlAtomicityMiniClusterTest::SetUp();
  }
};

// Rolls back to a savepoint taken before a CREATE TABLE whose unique index is not listed in the
// table's index list. Expects the table and the index to be deleted.
TEST_F(PgDdlAtomicitySavepointMiniClusterTest, RollbackToSavepointWithUnlistedIndex) {
  const auto kTableName = "savepoint_unlisted_index"s;
  const auto kIndexName = kTableName + "_uniq";
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("BEGIN"));
  ASSERT_OK(conn.Execute("SAVEPOINT a"));
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE $0 (a INT, b INT, CONSTRAINT $1 UNIQUE (b))", kTableName, kIndexName));
  const auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));
  const auto index_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kIndexName));
  ASSERT_OK(UnlinkIndexFromTable(table_id, index_id));

  ASSERT_OK(conn.Execute("ROLLBACK TO SAVEPOINT a"));
  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the rollback to savepoint to delete the table and its index"));
  ASSERT_OK(conn.Execute("COMMIT"));

  ASSERT_OK(WaitForNoTablesLeftBehind(
      kTableName, "Wait for the table and its index to be deleted"));
}

// Test that the table cache is correctly invalidated after transaction verification
// completes for an ALTER TABLE operation that performs a table scan.
TEST_F(PgDdlAtomicityTest, TestTableCacheAfterTxnVerification) {
  // Set report_ysql_ddl_txn_status_to_master to false, so that we can test the schema verification
  // codepaths on master.
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "report_ysql_ddl_txn_status_to_master", "false"));
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE test (key INT PRIMARY KEY, value TEXT, num real, serialcol SERIAL) "
      "PARTITION BY LIST(key)"));
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE test1 PARTITION OF test FOR VALUES IN (1)"));
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE test2 PARTITION OF test FOR VALUES IN (2, 3, 4, 5)"));
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO test VALUES (1, 'value', 1.0), (2, 'value', 2.0)"));
  ASSERT_OK(conn.TestFailDdl(
    "ALTER TABLE test DROP COLUMN value, ADD CONSTRAINT check_num CHECK (num > 0)"));
  // Ensure there is no schema version mismatch after a failed ALTER operation that performs
  // a table scan.
  ASSERT_OK(conn.Execute("BEGIN; INSERT INTO test2 VALUES (3, 'value', 3.0); COMMIT;"));
  ASSERT_OK(conn.ExecuteFormat(
    "ALTER TABLE test DROP COLUMN value, ADD CONSTRAINT check_num CHECK (num > 0)"));
  // Ensure there is no schema version mismatch after a successful ALTER operation that performs
  // a table scan.
  ASSERT_OK(conn.Execute("BEGIN; INSERT INTO test2 VALUES (4, 4.0); COMMIT;"));
  auto rows =
      ASSERT_RESULT((conn.FetchRows<int32_t, float, int32_t>("SELECT * FROM test2 ORDER BY key")));
  ASSERT_EQ(rows, (decltype(rows){{2, 2, 2}, {3, 3, 3}, {4, 4, 4}}));
  // Ensure there is no schema version mismatch for various ALTERs.
  // Alter type (with no rewrite).
  ASSERT_OK(conn.Execute("ALTER TABLE test ALTER COLUMN num TYPE double precision"));
  ASSERT_OK(conn.Execute("BEGIN; INSERT INTO test VALUES (5, 5.0); COMMIT;"));
  // Legacy table rewrite.
  ASSERT_OK(conn.Execute("CREATE TABLE test3 (key INT, value TEXT)"));
  ASSERT_OK(conn.Execute("INSERT INTO test3 VALUES (1, 'value')"));
  ASSERT_OK(conn.Execute("SET yb_enable_alter_table_rewrite = OFF;"));
  ASSERT_OK(conn.Execute(
      "ALTER TABLE test3 ADD PRIMARY KEY (key), ALTER COLUMN value SET NOT NULL"));
  ASSERT_OK(conn.Execute("BEGIN; INSERT INTO test3 VALUES (2, 'value2'); COMMIT;"));
  ASSERT_OK(conn.Execute("ALTER TABLE test3 ALTER COLUMN value TYPE int USING length(value),"
      "ADD CONSTRAINT check_value CHECK (value > 0)"));
  ASSERT_OK(conn.Execute("BEGIN; INSERT INTO test3 VALUES (3, 6); COMMIT;"));
}

// Test that DDL-related metadata in TableInfo objects is cleared on drop.
TEST_F(PgDdlAtomicityMiniClusterTest, ClearTableMetadataOnDrop) {
  const auto kTableName = "test";
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());
  auto& catalog_mgr = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager();

  // DDL-related metadata should be cleared after drop.
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v INT)", kTableName));
  auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));
  auto table = catalog_mgr.GetTableInfo(table_id);
  ASSERT_OK(conn.ExecuteFormat("DROP TABLE $0", kTableName));
  ASSERT_FALSE(table->LockForRead()->has_ysql_ddl_txn_verifier_state());

  // Same test but with the table being hidden instead of deleted.
  auto snapshot_util = std::make_unique<client::SnapshotTestUtil>();
  snapshot_util->SetProxy(&client->proxy_cache());
  snapshot_util->SetCluster(cluster_.get());
  ASSERT_RESULT(snapshot_util->CreateSchedule(
      "yugabyte", client::WaitSnapshot::kTrue, 1min /* snapshot_interval */));
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v INT)", kTableName));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", kTableName));
  table = catalog_mgr.GetTableInfo(table_id);
  ASSERT_OK(conn.ExecuteFormat("DROP TABLE $0", kTableName));
  ASSERT_TRUE(table->LockForRead()->is_hidden_but_not_deleting());
  ASSERT_FALSE(table->LockForRead()->has_ysql_ddl_txn_verifier_state());
}

// Test that the schema verification works correctly for partition tables and its children.
TEST_F(PgDdlAtomicityTest, TestPartitionedTableSchemaVerification) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  // Set report_ysql_ddl_txn_status_to_master to false, so that we can test the schema verification
  // codepaths on master.
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "report_ysql_ddl_txn_status_to_master", "false"));
  // Create a parent partitioned table.
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE test_parent (key INT, value TEXT, num real, serialcol SERIAL) "
      "PARTITION BY LIST(key)"));
  // Create a child partition.
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE test_child PARTITION OF test_parent FOR VALUES IN (1)"));

  // Perform an unsuccessful alter table operation.
  ASSERT_OK(conn.TestFailDdl("ALTER TABLE test_parent DROP COLUMN value"));
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "true"));
  ASSERT_OK(conn.ExecuteFormat("SET yb_test_fail_next_ddl=1"));
  // Perform an unsuccessful alter table rewrite operation.
  ASSERT_NOK(conn.ExecuteFormat("ALTER TABLE test_parent ADD PRIMARY KEY (key)"));

  // Verify that the failed alter table rewrite operation created orphaned tables.
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("test_parent")).size(), 2);
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("test_child")).size(), 2);
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_ddl_rollback", "false"));
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      return VERIFY_RESULT(client->ListTables("test_child")).size() == 1 &&
        VERIFY_RESULT(client->ListTables("test_parent")).size() == 1;
  }, MonoDelta::FromSeconds(60), "Wait for orphaned child table to be cleaned up."));

  // Perform a successful alter table operation.
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE test_parent ADD COLUMN col1 int"));
  // Perform successful alter table rewrite operations.
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE test_parent ADD PRIMARY KEY (key)"));
  ASSERT_OK(conn.ExecuteFormat("ALTER TABLE test_parent ADD COLUMN col2 SERIAL"));

  // Perform a successful drop table operation.
  ASSERT_OK(conn.ExecuteFormat("DROP TABLE test_parent"));
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("test_parent")).size(), 0);
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("test_child")).size(), 0);
}

TEST_F(PgDdlAtomicityTest, TestCreateColocatedTable) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "report_ysql_ddl_txn_status_to_master", "false"));
  ASSERT_OK(conn.Execute("CREATE DATABASE colocated_db colocation = true"));
  conn = ASSERT_RESULT(ConnectToDB("colocated_db"));
  ASSERT_OK(conn.Execute("CREATE TABLE foo(id int)"));
}

TEST_F(PgDdlAtomicityTest, TestAlterTableAddUniqueConstraint) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "report_ysql_ddl_txn_status_to_master", "false"));
  ASSERT_OK(conn.Execute("CREATE TABLE foo(x character varying(20))"));
  // Adding a unique constraint does not change the table schema of the base table foo.
  // If we use schema comparison of foo, we will not be able to tell whether the
  // DDL transaction has committed at PG side or not. In this case we must use schema
  // comparison of the index x_unique instead.
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("x_unique")).size(), 0);
  ASSERT_OK(conn.Execute("ALTER TABLE foo ADD CONSTRAINT x_unique UNIQUE(x)"));
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("x_unique")).size(), 1);

  ASSERT_OK(conn.Execute("CREATE TABLE bar (y character varying(20))"));
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("y_unique")).size(), 0);
  ASSERT_OK(conn.Execute("SET yb_test_fail_next_ddl=1"));
  // As of 2024-09-17, the aborted ALTER TABLE bar statement leaves an orphan index
  // inside DocDB that is not garbage collected. But its existence will not prevent
  // a retry of the same statement to succeed, which will create another index with
  // the same name y_unique (but with a different table id).
  ASSERT_NOK(conn.Execute("ALTER TABLE bar ADD CONSTRAINT y_unique UNIQUE(y)"));
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("y_unique")).size(), 0);
  ASSERT_OK(conn.Execute("ALTER TABLE bar ADD CONSTRAINT y_unique UNIQUE(y)"));
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("y_unique")).size(), 1);
}

TEST_F(PgDdlAtomicityTest, TestAlterTableAddCheckConstraint) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "report_ysql_ddl_txn_status_to_master", "false"));
  ASSERT_OK(conn.Execute("CREATE TABLE foo(id int)"));
  ASSERT_OK(conn.Execute("ALTER TABLE foo ADD CONSTRAINT id_check CHECK (id > 5)"));
  // There is nothing created in DocDB for foo_id_check.
  ASSERT_EQ(ASSERT_RESULT(client->ListTables("foo_id_check")).size(), 0);
  auto foo_table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", "foo"));
  std::shared_ptr<client::YBTableInfo> foo_table_info = std::make_shared<client::YBTableInfo>();
  Synchronizer sync;
  ASSERT_OK(client->GetTableSchemaById(foo_table_id, foo_table_info, sync.AsStatusCallback()));
  ASSERT_OK(sync.Wait());
  ASSERT_EQ(foo_table_info->schema.version(), 1);

  ASSERT_OK(conn.Execute("CREATE TABLE bar(id int)"));
  ASSERT_OK(conn.Execute("SET yb_test_fail_next_ddl=1"));
  ASSERT_NOK(conn.Execute("ALTER TABLE bar ADD CONSTRAINT bar_id_check CHECK (id > 5)"));
  auto bar_table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), "yugabyte", "bar"));
  std::shared_ptr<client::YBTableInfo> bar_table_info = std::make_shared<client::YBTableInfo>();
  sync.Reset();
  ASSERT_OK(client->GetTableSchemaById(bar_table_id, bar_table_info, sync.AsStatusCallback()));
  ASSERT_OK(sync.Wait());
  ASSERT_EQ(bar_table_info->schema.version(), 1);

  ASSERT_OK(conn.Execute("ALTER TABLE bar ADD CONSTRAINT bar_id_check CHECK (id > 5)"));
  bar_table_info = std::make_shared<client::YBTableInfo>();
  sync.Reset();
  ASSERT_OK(client->GetTableSchemaById(bar_table_id, bar_table_info, sync.AsStatusCallback()));
  ASSERT_OK(sync.Wait());
  ASSERT_EQ(bar_table_info->schema.version(), 2);
}

// Issue https://github.com/yugabyte/yugabyte-db/issues/25708
TEST_F(PgDdlAtomicityTest, TestPollTransactionFuilure) {
  auto conn = ASSERT_RESULT(Connect());
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  ASSERT_OK(conn.Execute("CREATE TABLE foo(id int)"));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "report_ysql_ddl_txn_status_to_master", "false"));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "ysql_ddl_transaction_wait_for_ddl_verification", "false"));
  // Setting transaction polling delay to 1000ms.
  ASSERT_OK(cluster_->SetFlagOnMasters("ysql_transaction_bg_task_wait_ms", "1000"));
  // Inject transaction polling failure with 100% probability. This used to
  // cause the verification task to fail and end. However we should not end the
  // verification task on a transaction polling failure.
  ASSERT_OK(cluster_->SetFlagOnMasters(
      "TEST_ysql_ddl_transaction_verification_failure_probability", "100.0"));
  ASSERT_OK(conn.Execute("ALTER TABLE foo ADD CONSTRAINT id_check CHECK (id > 5)"));
  // Sleep enough to simulate several transaction polling failures, each with a delay of 1000ms.
  SleepFor(5s);
  // Disable new verification task auto-spawning.
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_disable_ysql_ddl_txn_verification", "true"));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "ysql_ddl_transaction_wait_for_ddl_verification", "true"));
  // The ADD CONSTRAINT is stucked because of the error injection. Therefore this
  // following DDL fails.
  ASSERT_NOK_STR_CONTAINS(conn.Execute("ALTER TABLE foo ADD COLUMN id2 INT"),
                          "is undergoing DDL transaction verification");
  // Now stop doing error injection so that the stucked DDL can complete.
  ASSERT_OK(cluster_->SetFlagOnMasters(
      "TEST_ysql_ddl_transaction_verification_failure_probability", "0.0"));
  // Wait for the stucked ADD CONSTRAINT DDL to complete.
  SleepFor(5s);
  ASSERT_OK(cluster_->SetFlagOnMasters("TEST_disable_ysql_ddl_txn_verification", "false"));
  // Now a new DDL on table foo can succeed.
  ASSERT_OK(conn.Execute("ALTER TABLE foo ADD COLUMN id3 INT"));
}

// Regression test for GHI #30895.
// When a DDL transaction's commit response is lost (e.g. due to a leader change), the client
// retries the UpdateTransaction(COMMITTED) RPC. If by that time the transaction has been fully
// applied and cleaned up from the coordinator, the retry used to fail with "Transaction expired
// or aborted by a conflict", causing the DDL to be incorrectly considered failed.
// This test verifies that the DDL succeeds despite a simulated lost commit response.
TEST_F(PgDdlAtomicityTest, DdlCommitWithLostResponse) {
  auto conn = ASSERT_RESULT(Connect());

  // Inject a simulated commit response loss on all tservers.
  // The flag auto-clears after the first COMMITTED replication, so only one DDL is affected.
  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_drop_commit_response", "true"));

  // This DDL's transaction commit will be Raft-replicated successfully, but the RPC response
  // will be dropped. The client retries, and with the fix, the retry is acknowledged as a
  // successful commit via the recently_committed_txn_ids_ cache.
  ASSERT_OK(conn.Execute("CREATE TABLE test_lost_commit_response (id INT PRIMARY KEY, v TEXT)"));

  // Verify the table is usable.
  ASSERT_OK(conn.Execute("INSERT INTO test_lost_commit_response VALUES (1, 'hello')"));
  auto val = ASSERT_RESULT(
      conn.FetchRow<std::string>("SELECT v FROM test_lost_commit_response WHERE id = 1"));
  ASSERT_EQ(val, "hello");
}

// Regression test for GHI #30895, leader-change variant.
// Same as DdlCommitWithLostResponse, but additionally forces a leadership change on the
// transaction status tablet after the commit is Raft-replicated. The new leader replays the
// COMMITTED entry from the Raft log, creating the transaction in managed_transactions_.
// The client's retry arrives at the new leader and eventually succeeds through the normal
// lifecycle: IllegalState retries -> transaction applies -> cleanup -> recently_committed cache.
TEST_F(PgDdlAtomicityTest, DdlCommitWithLostResponseAndLeaderChange) {
  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_drop_commit_response", "true"));

  // Run the DDL in a background thread so we can step down leaders while it's in progress.
  TestThreadHolder thread_holder;
  std::atomic<bool> ddl_done{false};
  thread_holder.AddThreadFunctor([this, &ddl_done] {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.Execute(
        "CREATE TABLE test_leader_change (id INT PRIMARY KEY, v TEXT)"));
    ddl_done.store(true);
  });

  // Wait for the commit to be Raft-replicated. The flag auto-clears after the first hit,
  // so by the time we get here the commit is replicated but the response was dropped.
  SleepFor(2s * kTimeMultiplier);

  // Step down transaction status tablet leaders to force retries to reach a new leader.
  // Some step-downs may fail transiently (e.g. a live PRE_VOTER still bootstrapping, or the
  // nominated peer is not caught up yet), which is fine: we just need at least one leadership
  // change.
  int stepped_down = 0;
  for (size_t i = 0; i < cluster_->num_tablet_servers(); ++i) {
    auto tablets = ASSERT_RESULT(cluster_->GetTablets(cluster_->tablet_server(i)));
    for (const auto& tablet : tablets) {
      if (tablet.table_name().find("transactions") != std::string::npos && tablet.is_leader()) {
        LOG(INFO) << "Stepping down leader for transaction status tablet " << tablet.tablet_id()
                  << " on tserver " << i;
        auto s = cluster_->MoveTabletLeader(tablet.tablet_id());
        if (s.ok()) {
          ++stepped_down;
        } else {
          LOG(WARNING) << "Failed to step down leader for " << tablet.tablet_id() << ": " << s;
        }
      }
    }
  }
  LOG(INFO) << "Stepped down " << stepped_down << " transaction status tablet leader(s)";

  thread_holder.WaitAndStop(60s * kTimeMultiplier);
  ASSERT_TRUE(ddl_done.load()) << "DDL did not complete";

  // Verify the table is usable.
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("INSERT INTO test_leader_change VALUES (1, 'hello')"));
  auto val = ASSERT_RESULT(
      conn.FetchRow<std::string>("SELECT v FROM test_leader_change WHERE id = 1"));
  ASSERT_EQ(val, "hello");
}

} // namespace pgwrapper
} // namespace yb
