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
//

#include "yb/client/transaction.h"
#include "yb/client/transaction_pool.h"
#include "yb/common/common_flags.h"
#include "yb/common/ddl_mode-test-util.h"
#include "yb/docdb/object_lock_shared_state_manager.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_local_lock_manager.h"
#include "yb/tserver/tserver_service.pb.h"
#include "yb/tserver/tserver_shared_mem.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/mini_master.h"
#include "yb/master/object_lock_info_manager.h"

#include "yb/util/async_util.h"
#include "yb/util/backoff_waiter.h"
#include "yb/util/logging_test_util.h"
#include "yb/util/monotime.h"
#include "yb/util/sync_point.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_thread_holder.h"
#include "yb/util/tsan_util.h"

#include "yb/yql/pgwrapper/geo_transactions_test_base.h"
#include "yb/yql/pgwrapper/libpq_test_base.h"
#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_mini_test_base.h"
#include "yb/yql/pgwrapper/pg_test_utils.h"

DECLARE_bool(auto_create_local_transaction_tables);
DECLARE_bool(auto_promote_nonlocal_transactions_to_global);
DECLARE_bool(enable_object_lock_fastpath);
DECLARE_bool(enable_object_locking_for_table_locks);
DECLARE_bool(enable_tablespace_based_transaction_placement);
DECLARE_bool(force_global_transactions);
DECLARE_bool(pg_client_use_shared_memory);
DECLARE_bool(report_ysql_ddl_txn_status_to_master);
DECLARE_bool(use_tablespace_based_transaction_placement);
DECLARE_bool(ysql_ddl_transaction_wait_for_ddl_verification);
DECLARE_bool(ysql_enable_concurrent_ddl);
DECLARE_bool(ysql_yb_ddl_transaction_block_enabled);
DECLARE_bool(TEST_allow_wait_for_alter_table_to_finish);
DECLARE_bool(TEST_check_broadcast_address);
DECLARE_bool(TEST_consider_all_local_transaction_tables_local);
DECLARE_bool(TEST_make_global_lock_release_async);
DECLARE_bool(TEST_perform_ignore_pg_is_region_local);
DECLARE_bool(TEST_track_last_transaction);
DECLARE_string(TEST_block_alter_table);

DECLARE_uint64(pg_client_session_expiration_ms);
DECLARE_uint64(pg_client_heartbeat_interval_ms);
DECLARE_uint64(refresh_waiter_timeout_ms);
DECLARE_uint64(transaction_heartbeat_usec);
DECLARE_int32(transaction_table_num_tablets);
DECLARE_uint64(master_ysql_operation_lease_ttl_ms);
DECLARE_bool(TEST_tserver_enable_ysql_lease_refresh);
DECLARE_int64(olm_poll_interval_ms);
DECLARE_string(vmodule);
DECLARE_bool(ysql_enable_auto_analyze);
DECLARE_int32(pg_client_extra_timeout_ms);
DECLARE_int32(ysql_client_read_write_timeout_ms);
DECLARE_int32(ysql_tablespace_info_refresh_secs);
DECLARE_bool(TEST_olm_serve_redundant_lock);
DECLARE_uint64(TEST_delay_release_locks_ms);
DECLARE_int32(master_ts_rpc_timeout_ms);
DECLARE_uint32(ysql_max_invalidation_message_queue_size);
DECLARE_bool(TEST_pause_session_lock_before_release);
DECLARE_bool(TEST_pause_session_lock_after_release);

METRIC_DECLARE_counter(object_locking_lock_acquires);
METRIC_DECLARE_counter(object_locking_lock_releases);
METRIC_DECLARE_gauge_uint64(object_locking_fastpath_pg_acquires);
METRIC_DECLARE_gauge_uint64(object_locking_fastpath_pg_releases);
METRIC_DECLARE_gauge_uint64(object_locking_fastpath_tserver_acquires);
METRIC_DECLARE_gauge_uint64(object_locking_fastpath_tserver_releases);

using namespace std::literals;

namespace yb::pgwrapper {

constexpr uint64_t kDefaultMasterYSQLLeaseTTLMilli = 5 * 1000;
constexpr uint64_t kDefaultYSQLLeaseRefreshIntervalMilli = 500;
constexpr uint64_t kDefaultLockManagerPollIntervalMs = 100;

class PgObjectLocksTestRF1 : public PgMiniTestBase {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_vmodule) = "libpq_utils=1";
    // Set reasonable high poll interavl to disable polling in tests., would help catch issues
    // with signaling logic if any.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_olm_poll_interval_ms) = 1000000;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_master_ysql_operation_lease_ttl_ms) =
        kDefaultMasterYSQLLeaseTTLMilli;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_tserver_enable_ysql_lease_refresh) =
        kDefaultYSQLLeaseRefreshIntervalMilli;
    ToggleDDLMode(/* use_legacy = */ false);
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_check_broadcast_address) = false;  // GH #26281
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_auto_analyze) = false;
    PgMiniTestBase::SetUp();
    Init();
  }

  void Init() {
    ts_lock_manager_ = cluster_->mini_tablet_server(0)->server()->ts_local_lock_manager();
    master_lock_manager_ = cluster_->mini_master()
                               ->master()
                               ->catalog_manager_impl()
                               ->object_lock_info_manager()
                               ->TEST_ts_local_lock_manager();
  }

  size_t NumTabletServers() override {
    return 1;
  }

  size_t NumGrantedLocks() const {
    return ts_lock_manager_->TEST_GrantedLocksSize();
  }

  size_t NumWaitingLocks() const {
    return ts_lock_manager_->TEST_WaitingLocksSize();
  }

  size_t NumGrantedLocksOnMaster() const {
    return master_lock_manager_->TEST_GrantedLocksSize();
  }

  size_t NumWaitingLocksOnMaster() const {
    return master_lock_manager_->TEST_WaitingLocksSize();
  }

  Status AssertNumLocks(size_t granted_locks, size_t waiting_locks) {
    SCHECK_EQ(NumGrantedLocks(), granted_locks, IllegalState,
              Format("Found num granted locks != Expected num granted locks"));
    SCHECK_EQ(NumWaitingLocks(), waiting_locks, IllegalState,
              Format("Found num waiting locks != Expected num waiting locks"));
    return Status::OK();
  }

  void CreateTestTable() {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
    ASSERT_OK(conn.Execute("INSERT INTO test SELECT generate_series(1, 10), 0"));
  }

  void TestAllBlockingPairs(bool test_pg_locks) {
    static const std::vector<std::pair<std::string, std::string>> kBlockingPairs = {
      {"ACCESS EXCLUSIVE", "ACCESS SHARE"},
      {"ACCESS EXCLUSIVE", "ROW SHARE"},
      {"ACCESS EXCLUSIVE", "ROW EXCLUSIVE"},
      {"ACCESS EXCLUSIVE", "SHARE UPDATE EXCLUSIVE"},
      {"ACCESS EXCLUSIVE", "SHARE"},
      {"ACCESS EXCLUSIVE", "SHARE ROW EXCLUSIVE"},
      {"ACCESS EXCLUSIVE", "EXCLUSIVE"},
      {"ACCESS EXCLUSIVE", "ACCESS EXCLUSIVE"},
      {"EXCLUSIVE", "ROW SHARE"},
      {"EXCLUSIVE", "ROW EXCLUSIVE"},
      {"EXCLUSIVE", "SHARE UPDATE EXCLUSIVE"},
      {"EXCLUSIVE", "SHARE"},
      {"EXCLUSIVE", "SHARE ROW EXCLUSIVE"},
      {"EXCLUSIVE", "EXCLUSIVE"},
      {"SHARE ROW EXCLUSIVE", "ROW EXCLUSIVE"},
      {"SHARE ROW EXCLUSIVE", "SHARE UPDATE EXCLUSIVE"},
      {"SHARE ROW EXCLUSIVE", "SHARE"},
      {"SHARE ROW EXCLUSIVE", "SHARE ROW EXCLUSIVE"},
      {"SHARE", "ROW EXCLUSIVE"},
      {"SHARE", "SHARE UPDATE EXCLUSIVE"},
      {"SHARE UPDATE EXCLUSIVE", "SHARE UPDATE EXCLUSIVE"},
    };

    CreateTestTable();
    auto conn1 = ASSERT_RESULT(Connect());
    ASSERT_OK(conn1.ExecuteFormat("SET yb_locks_min_txn_age='0s'"));
    auto conn2 = ASSERT_RESULT(Connect());
    ASSERT_OK(conn2.ExecuteFormat("SET yb_locks_min_txn_age='0s'"));
    for (const auto& lock_types : kBlockingPairs) {
      VerifyBlockingBehavior(conn1, conn2, lock_types.first, lock_types.second, test_pg_locks);
      VerifyBlockingBehavior(conn1, conn2, lock_types.second, lock_types.first, test_pg_locks);
    }
  }

  std::string ToMode(const std::string& table_lock_type_str) {
    static const std::map<std::string, std::string> kLockModeMap = {
      {"ACCESS SHARE", "AccessShareLock"},
      {"ROW SHARE", "RowShareLock"},
      {"ROW EXCLUSIVE", "RowExclusiveLock"},
      {"SHARE UPDATE EXCLUSIVE", "ShareUpdateExclusiveLock"},
      {"SHARE", "ShareLock"},
      {"SHARE ROW EXCLUSIVE", "ShareRowExclusiveLock"},
      {"EXCLUSIVE", "ExclusiveLock"},
      {"ACCESS EXCLUSIVE", "AccessExclusiveLock"}
    };
    auto mode_it = kLockModeMap.find(table_lock_type_str);
    if (mode_it == kLockModeMap.end()) {
      return "UnknownLockMode";
    }
    return mode_it->second;
  }

  void VerifyBlockingBehavior(
      PGConn& conn1, PGConn& conn2, const std::string& holder_lock_type,
      const std::string& waiter_lock_type, bool test_pg_locks) {
    google::SetVLOGLevel("object_lock*", 1);
    LOG(INFO) << "Checking blocking behavior: " << holder_lock_type << " and " << waiter_lock_type;
    static const std::string lock_query = "LOCK TABLE test IN $0 MODE";
    ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
    ASSERT_OK(conn1.ExecuteFormat(lock_query, holder_lock_type));

    auto log_waiter = StringWaiterLogSink("added to wait-queue on");
    auto conn2_lock_future = std::async(std::launch::async, [&]() -> Status {
      RETURN_NOT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
      RETURN_NOT_OK(conn2.ExecuteFormat(lock_query, waiter_lock_type));

      if (test_pg_locks) {
        SleepFor(500ms * kTimeMultiplier);
        auto granted_lock_mode = VERIFY_RESULT(conn2.FetchRow<std::string>(
            "SELECT mode FROM pg_locks WHERE granted = true AND relation = 'test'::regclass"));
        // The waiter should have acquired the lock at this point.
        SCHECK_EQ(granted_lock_mode, ToMode(waiter_lock_type),
            IllegalState, "Expected the waiter to be unblocked");
      }

      RETURN_NOT_OK(conn2.Execute("INSERT INTO test (k, v) VALUES (11, 1)"));
      RETURN_NOT_OK(conn2.CommitTransaction());
      return Status::OK();
    });
    ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(5 * kTimeMultiplier)));

    if (test_pg_locks) {
      SleepFor(500ms * kTimeMultiplier);
      auto granted_lock_mode = ASSERT_RESULT(conn1.FetchRow<std::string>(
          "SELECT mode FROM pg_locks WHERE granted = true AND relation = 'test'::regclass"));
      ASSERT_EQ(granted_lock_mode, ToMode(holder_lock_type));
      auto waiting_lock_mode = ASSERT_RESULT(conn1.FetchRow<std::string>(
          "SELECT mode FROM pg_locks WHERE granted = false AND relation = 'test'::regclass"));
      ASSERT_EQ(waiting_lock_mode, ToMode(waiter_lock_type));
    }

    ASSERT_EQ(ASSERT_RESULT(conn1.FetchRow<PGUint64>("SELECT COUNT(*) FROM test WHERE v = 1")), 0);

    // After conn1 releases the lock, conn2 should be able to acquire it.
    ASSERT_OK(conn1.CommitTransaction());
    ASSERT_OK(conn2_lock_future.get());
    ASSERT_EQ(ASSERT_RESULT(conn1.FetchRow<PGUint64>("SELECT COUNT(*) FROM test WHERE v = 1")), 1);
    ASSERT_OK(conn2.Execute("DELETE FROM test WHERE v = 1"));
  }

  tserver::TSLocalLockManagerPtr ts_lock_manager_;
  tserver::TSLocalLockManagerPtr master_lock_manager_;
};

TEST_F(PgObjectLocksTestRF1, TestLockTuple) {
  auto setup_conn = ASSERT_RESULT(Connect());
  ASSERT_OK(setup_conn.Execute(
      "CREATE OR REPLACE FUNCTION add1(integer, integer) RETURNS integer "
      "AS 'select $1 + $2 + 2;' LANGUAGE SQL IMMUTABLE RETURNS NULL ON NULL INPUT"));

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn.Execute("ALTER FUNCTION add1(int, int) OWNER TO postgres"));

  auto status_future = std::async(std::launch::async, [&]() -> Status {
    auto conn = VERIFY_RESULT(Connect());
    RETURN_NOT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
    RETURN_NOT_OK(conn.Execute("ALTER FUNCTION add1(int, int) OWNER TO postgres"));
    RETURN_NOT_OK(conn.CommitTransaction());
    return Status::OK();
  });

  EXPECT_OK(WaitFor([&]() {
    return NumWaitingLocks() >= 1 || NumWaitingLocksOnMaster() >= 1;
  }, 5s * kTimeMultiplier, "Timed out waiting for num waiting locks >= 1"));
  ASSERT_OK(conn.RollbackTransaction());
  ASSERT_OK(status_future.get());
}

TEST_F(PgObjectLocksTestRF1, TestSanity) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(AssertNumLocks(0 /* granted locks*/, 0 /* waiting locks */));

  // A DDL shouldn't leave behind any locks on the local TS (since it is RF1).
  ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));

  // An auto commit distributed transaction shouldn't leave behind any locks.
  ASSERT_OK(conn.Execute("INSERT INTO test SELECT generate_series(1, 10), 0"));
  ASSERT_OK(AssertNumLocks(0 /* granted locks*/, 0 /* waiting locks */));

  // A fast-path transaction shouldn't leave behing any locks.
  ASSERT_OK(conn.Execute("UPDATE test SET v=1 where k=1"));
  ASSERT_OK(AssertNumLocks(0 /* granted locks*/, 0 /* waiting locks */));

  // A read-only transaction shouldn't leave behind any locks.
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn.Fetch("SELECT * FROM test"));
  ASSERT_GE(NumGrantedLocks(), 1);
  ASSERT_OK(conn.CommitTransaction());
  ASSERT_OK(AssertNumLocks(0 /* granted locks*/, 0 /* waiting locks */));

  // An explicit distributed transaction shouldn't leave behind any locks.
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn.Execute("UPDATE test SET v=v+1 WHERE k>=1"));
  ASSERT_GE(NumGrantedLocks(), 1);
  ASSERT_OK(conn.CommitTransaction());
  ASSERT_OK(AssertNumLocks(0 /* granted locks*/, 0 /* waiting locks */));
}

TEST_F(PgObjectLocksTestRF1, TestWaitingOnConflictingLocks) {
  CreateTestTable();

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn.Fetch("SELECT * FROM test"));

  auto status_future = std::async(std::launch::async, [&]() -> Status {
    auto conn = VERIFY_RESULT(Connect());
    RETURN_NOT_OK(conn.Execute("ALTER TABLE test ADD COLUMN v1 INT"));
    return Status::OK();
  });

  EXPECT_OK(WaitFor([&]() {
    return NumWaitingLocks() >= 1;
  }, 5s * kTimeMultiplier, "Timed out waiting for num waiting locks == 1"));
  ASSERT_OK(conn.CommitTransaction());
  ASSERT_OK(status_future.get());
  ASSERT_OK(AssertNumLocks(0 /* granted locks*/, 0 /* waiting locks */));
}

// A DDL blocked on a conflicting object lock used to fail with a transport timeout naming neither
// the lock nor the holder. This only happens with lock_timeout at its default of 0; setting it arms
// postgres' timer, which already reports the conflict correctly, so the test shortens the fallback
// deadline instead.
class PgObjectLocksTestShortDeadline : public PgObjectLocksTestRF1 {
 protected:
  // Reproducing the field failure needs the shared memory exchange, off by default on macOS.
  void BeforePgProcessStart() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_object_lock_fastpath) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_pg_client_use_shared_memory) = true;
  }

  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_client_read_write_timeout_ms) = 1000;
    PgObjectLocksTestRF1::SetUp();
  }
};

TEST_F_EX(PgObjectLocksTestRF1, DdlBlockedOnObjectLockReportsLockConflict,
          PgObjectLocksTestShortDeadline) {
  CreateTestTable();

  auto blocker = ASSERT_RESULT(Connect());
  ASSERT_OK(blocker.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(blocker.Execute("LOCK TABLE test IN SHARE UPDATE EXCLUSIVE MODE"));

  auto conn = ASSERT_RESULT(Connect());
  // Setting lock_timeout would arm postgres' timer directly and hide the bug.
  ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<std::string>("SHOW lock_timeout")), "0");

  const auto start = MonoTime::Now();
  const auto status = conn.Execute("DROP TABLE test");
  const auto elapsed = MonoTime::Now() - start;
  LOG(INFO) << "DROP TABLE returned after " << elapsed << ": " << status;

  ASSERT_NOK(status);
  ASSERT_LT(elapsed, MonoDelta::FromSeconds(60) * kTimeMultiplier);
  ASSERT_STR_CONTAINS(
      status.ToString(), "canceling statement due to lock timeout (pgsql error 55P03)");

  ASSERT_OK(blocker.CommitTransaction());
}

TEST_F(PgObjectLocksTestRF1, VerifyTableLockBlockingBehavior) {
  TestAllBlockingPairs(/*test_pg_locks=*/false);
}

TEST_F(PgObjectLocksTestRF1, TestPgLocks) {
  TestAllBlockingPairs(/*test_pg_locks=*/true);
}

// Verifies that when an object lock request waits on a
// conflicting granted object lock, pg_locks surfaces the blocking
// transaction id via ybdetails->'blocked_by' for the waiting object lock.
// See issue: https://github.com/yugabyte/yugabyte-db/issues/27398

// Two compatible holders (ROW EXCLUSIVE) block one ACCESS EXCLUSIVE waiter, so blocked_by must
// list more than one transaction id.
TEST_F(PgObjectLocksTestRF1, TestPgLocksBlockedByMultipleTransactions) {
  CreateTestTable();

  auto blocker1_conn = ASSERT_RESULT(Connect());
  auto blocker2_conn = ASSERT_RESULT(Connect());
  auto observer_conn = ASSERT_RESULT(Connect());
  ASSERT_OK(observer_conn.Execute("SET yb_locks_min_txn_age='0s'"));
  // Ensure both blockers and the waiter can appear together in pg_locks.
  ASSERT_OK(observer_conn.Execute("SET yb_locks_max_transactions=16"));

  // ROW EXCLUSIVE locks do not conflict with each other, so both holders are granted.
  ASSERT_OK(blocker1_conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(blocker1_conn.Execute("INSERT INTO test VALUES (100, 100)"));
  ASSERT_OK(blocker2_conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(blocker2_conn.Execute("INSERT INTO test VALUES (101, 101)"));

  // ACCESS EXCLUSIVE conflicts with both ROW EXCLUSIVE holders and must wait on both.
  auto waiter_future = std::async(std::launch::async, [&]() -> Status {
    auto waiter_conn = VERIFY_RESULT(Connect());
    RETURN_NOT_OK(waiter_conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
    RETURN_NOT_OK(waiter_conn.Execute("ALTER TABLE test ADD COLUMN v1 INT"));
    return waiter_conn.CommitTransaction();
  });
  ASSERT_OK(WaitFor([&]() {
    return NumWaitingLocks() >= 1;
  }, 15s * kTimeMultiplier, "Timed out waiting for the ALTER to enqueue a waiting object lock"));

  std::string blocked_by;
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    const auto dump = VERIFY_RESULT(observer_conn.FetchAllAsString(
        "SELECT granted, mode, ybdetails->>'transactionid' AS txn,"
        " (ybdetails->'blocked_by')::text AS blocked_by FROM pg_locks"
        " WHERE relation = 'test'::regclass AND locktype = 'relation' ORDER BY granted, mode"));
    LOG(INFO) << "object locks on test:\n" << dump;
    // Require at least two blocker txn ids, both matching the granted RowExclusiveLock holders.
    // The check and the blocked_by value must come from one query: separate round trips see
    // different pg_locks snapshots, so the value could still be empty on the iteration where
    // the check first passes. MATERIALIZED keeps the correlated subquery on the same snapshot.
    blocked_by = VERIFY_RESULT(observer_conn.FetchRow<std::string>(
        "WITH locks AS MATERIALIZED ("
        "  SELECT granted, mode, ybdetails FROM pg_locks"
        "  WHERE relation = 'test'::regclass AND locktype = 'relation')"
        "SELECT COALESCE("
        "  (SELECT w.ybdetails->'blocked_by' FROM locks w"
        "   WHERE NOT w.granted AND w.mode = 'AccessExclusiveLock'"
        "     AND w.ybdetails->'blocked_by' IS NOT NULL"
        "     AND jsonb_array_length(w.ybdetails->'blocked_by') >= 2"
        "     AND (SELECT count(DISTINCT g.ybdetails->>'transactionid')"
        "          FROM locks g"
        "          WHERE g.granted AND g.mode = 'RowExclusiveLock'"
        "            AND g.ybdetails->>'transactionid' IS NOT NULL"
        "            AND w.ybdetails->'blocked_by' @> to_jsonb(g.ybdetails->>'transactionid')"
        "         ) >= 2"
        "   LIMIT 1), '[]'::jsonb)::text"));
    return blocked_by != "[]" && !blocked_by.empty();
  }, 30s * kTimeMultiplier,
     "Timed out waiting for blocked_by to list multiple blocking transactions"));

  EXPECT_NE(blocked_by, "[]") << "blocked_by was empty";
  LOG(INFO) << "blocked_by with multiple transactions: " << blocked_by;

  // Both blockers must be released before the ACCESS EXCLUSIVE waiter can proceed.
  ASSERT_OK(blocker1_conn.CommitTransaction());
  ASSERT_OK(blocker2_conn.CommitTransaction());
  ASSERT_OK(waiter_future.get());
}

class PgObjectLocksTestRF1Deadlock : public PgObjectLocksTestRF1 {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_master_ts_rpc_timeout_ms) = kRefreshWaiterTimeoutMs;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_refresh_waiter_timeout_ms) = kRefreshWaiterTimeoutMs;
    PgObjectLocksTestRF1::SetUp();
  }

 private:
  uint32_t kRefreshWaiterTimeoutMs = 10000 * kTimeMultiplier;
};

TEST_F(PgObjectLocksTestRF1Deadlock, YB_DISABLE_TEST_IN_SANITIZERS(TestDeadlockSimple)) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_olm_poll_interval_ms) = kDefaultLockManagerPollIntervalMs * 10;
  ASSERT_OK(cluster_->RestartSync());
  Init();

  auto conn1 = ASSERT_RESULT(Connect());
  auto conn2 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT)"));
  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.Execute("LOCK TABLE test IN ACCESS SHARE MODE"));

  ASSERT_OK(conn2.Execute("BEGIN;"));
  auto status_future_1 = std::async(std::launch::async, [&]() -> Status {
    RETURN_NOT_OK(conn2.Execute("LOCK TABLE test IN ACCESS EXCLUSIVE MODE"));
    return Status::OK();
  });
  ASSERT_OK(WaitFor([&]() {
    return NumWaitingLocks() >= 1;
  }, 5s * kTimeMultiplier, "Timed out waiting for num waiting locks to be >= 1"));
  // At this point both txns have triggered SetStartTimeIfNecessary via the object-lock path, so
  // their local start_ values are set. Wait for the next periodic transaction heartbeat to carry
  // those values to each status-tablet coordinator before forming the deadlock cycle, so the
  // deadlock detector sees real txn_start_us values instead of the stale first_touch_=0 left by
  // the pool-prebuilt CREATED heartbeat.
  SleepFor(FLAGS_transaction_heartbeat_usec * 1us * kTimeMultiplier * 2);
  auto status_future_2 = std::async(std::launch::async, [&]() -> Status {
    RETURN_NOT_OK(conn1.Execute("LOCK TABLE test IN ACCESS EXCLUSIVE MODE"));
    return Status::OK();
  });
  // The deadlock detector aborts exactly one of the two lock requests. The aborted transaction,
  // however, retains the locks it acquired in earlier statements (e.g. conn1's ACCESS SHARE) until
  // it is rolled back, matching PostgreSQL semantics where a failed statement leaves the
  // transaction aborted but holding its locks. Hence the surviving request cannot make progress
  // until the aborted transaction is rolled back. Roll back each transaction as soon as its request
  // aborts so the survivor is unblocked.
  std::optional<Status> s1, s2;
  ASSERT_OK(WaitFor(
      [&]() {
        if (!s1 && status_future_1.wait_for(0s) == std::future_status::ready) {
          s1 = status_future_1.get();
          if (!s1->ok()) {
            EXPECT_OK(conn2.RollbackTransaction());
          }
        }
        if (!s2 && status_future_2.wait_for(0s) == std::future_status::ready) {
          s2 = status_future_2.get();
          if (!s2->ok()) {
            EXPECT_OK(conn1.RollbackTransaction());
          }
        }
        return s1.has_value() && s2.has_value();
      },
      60s * kTimeMultiplier, "Timed out waiting for status futures to complete"));
  // One of the two connections should have been aborted due to deadlock.
  ASSERT_TRUE(s1->ok() ^ s2->ok());
  // Roll back the surviving transaction; the aborted one was already rolled back above.
  ASSERT_OK(s1->ok() ? conn2.RollbackTransaction() : conn1.RollbackTransaction());
}

class PgObjectLocksTestRF1SessionExpiry : public PgObjectLocksTestRF1 {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_pg_client_session_expiration_ms) = kSessionTimeoutMs;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_pg_client_heartbeat_interval_ms) = kSessionTimeoutMs / 3;
    PgObjectLocksTestRF1::SetUp();

    conn_ = ASSERT_RESULT(Connect());
    LOG(INFO) << "Creating test table";
    ASSERT_OK(conn_->Execute("CREATE TABLE test_table (id INT PRIMARY KEY, value TEXT);"));
  }

  Status WaitToReleaseAllLocks(MonoDelta timeout) {
    LOG(INFO) << "Waiting for locks to be cleaned up";
    return LoggedWaitFor(
        [this]() -> bool {
          auto granted_locks = NumGrantedLocks();
          auto granted_master_locks = NumGrantedLocksOnMaster();
          LOG(INFO) << "Num granted locks: " << granted_locks;
          LOG(INFO) << "Num granted master locks: " << granted_master_locks;
          return granted_locks == 0 && granted_master_locks == 0;
        },
        timeout, "wait for locks to be cleaned up", timeout / 10);
  }

  void ExpireSession(pgwrapper::PGConn& target_conn) {
    auto pid = ASSERT_RESULT(target_conn.FetchRow<int32_t>("SELECT pg_backend_pid();"));
    ExpireSessionWithPid(pid);
  }

  void ExpireSessionWithPid(int32_t pid) {
    LOG(INFO) << "Killing pid " << pid;
    auto res =
        ASSERT_RESULT(conn_->FetchRowAsString(yb::Format("SELECT pg_terminate_backend($0);", pid)));
    LOG(INFO) << "Got " << res;
  }

  uint64_t kSessionTimeoutMs = 6000 * kTimeMultiplier;
  MonoDelta kSessionTimeout = MonoDelta::FromMilliseconds(kSessionTimeoutMs);

 private:
  std::optional<PGConn> conn_;
};

TEST_F(PgObjectLocksTestRF1SessionExpiry, CleanupSharedLocksOnExpiredSessions) {
  auto conn = ASSERT_RESULT(Connect());
  // Clean up a shared lock from the local TServer.
  LOG(INFO) << "Acquiring shared lock on test table in another session";
  ASSERT_OK(conn.Execute("BEGIN TRANSACTION;"));
  ASSERT_OK(conn.Execute("INSERT INTO test_table VALUES (1, 'good');"));
  ASSERT_GE(NumGrantedLocks(), 1);
  LOG(INFO) << "Expiring connection";
  ExpireSession(conn);
  ASSERT_OK(WaitToReleaseAllLocks(kSessionTimeout * 2));
}

TEST_F(PgObjectLocksTestRF1SessionExpiry, CleanupExclusiveLocksOnExpiredSessions) {
  auto conn = ASSERT_RESULT(Connect());
  // Clean up an exclusive lock from the master and local TServer.
  LOG(INFO) << "Acquiring exclusive lock on test table";
  ASSERT_OK(conn.Execute("BEGIN TRANSACTION;"));
  ASSERT_OK(conn.Execute("LOCK TABLE test_table IN ACCESS EXCLUSIVE MODE;"));
  ASSERT_GE(NumGrantedLocksOnMaster(), 1);
  ASSERT_GE(NumGrantedLocks(), 1);
  LOG(INFO) << "Expiring connection";
  ExpireSession(conn);
  ASSERT_OK(WaitToReleaseAllLocks(kSessionTimeout * 2));
}

TEST_F(PgObjectLocksTestRF1SessionExpiry, CleanupReadOnlyLocksOnExpiredSessions) {
  auto conn = ASSERT_RESULT(Connect());
  // Clean up a shared lock from the local TServer.
  LOG(INFO) << "Acquiring shared lock on test table in another session";
  ASSERT_OK(conn.Execute("BEGIN TRANSACTION;"));
  ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT count(*) from test_table;"));
  ASSERT_GE(NumGrantedLocks(), 1);
  LOG(INFO) << "Expiring connection";
  ExpireSession(conn);
  ASSERT_OK(WaitToReleaseAllLocks(kSessionTimeout * 2));
}

class PgObjectLocksTestRF1SessionExpiryWithDdl
    : public PgObjectLocksTestRF1SessionExpiry,
      public ::testing::WithParamInterface<bool> {
 protected:
  void SetUp() override {
    allow_ddl_verification_task_ = GetParam();
    PgObjectLocksTestRF1SessionExpiry::SetUp();
  }
  bool allow_ddl_verification_task_;
};

TEST_P(PgObjectLocksTestRF1SessionExpiryWithDdl, CleanupDdlLocksOnExpiredSessions) {
  auto conn = ASSERT_RESULT(Connect());
  auto pid = ASSERT_RESULT(conn.FetchRow<int32_t>("SELECT pg_backend_pid();"));
  TestThreadHolder thread_holder;
  // Cause the alter to hang, so that we can kill the session while it is holding the locks.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_block_alter_table) =
      (allow_ddl_verification_task_ ? "completion" : "alter_schema");
  thread_holder.AddThreadFunctor([&conn]() {
    LOG(INFO) << "Acquiring exclusive lock on test table";
    ASSERT_OK(conn.Execute("BEGIN TRANSACTION;"));
    auto res = conn.Execute("ALTER TABLE test_table ADD COLUMN bar TEXT");
    LOG(INFO) << "Got " << res;
    // Expect the Alter to fail because we are going to kill the session.
    ASSERT_NOK(res);
  });
  ASSERT_OK(WaitFor(
      [this]() { return NumGrantedLocks() >= 1; }, 5s * kTimeMultiplier,
      "Timed out waiting for num granted locks == 1"));
  LOG(INFO) << "Expiring connection 1";
  SleepFor(1s * kTimeMultiplier);
  ExpireSessionWithPid(pid);
  if (allow_ddl_verification_task_) {
    // Let the Alter proceed, so it creates the verifier task.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_block_alter_table) = "";
    // Release request will be ignored.
    // ddl verifier will kick in after the txn expires.
    ASSERT_OK(WaitToReleaseAllLocks(60s));
  } else {
    ASSERT_OK(WaitToReleaseAllLocks(kSessionTimeout * 2));
    // Alter may proceed, only after the locks are released.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_block_alter_table) = "";
  }
}

std::string DdlVerificationTaskToName(
    const ::testing::TestParamInfo<bool>& info) {
  return yb::Format("$0", info.param ? "ReleaseByDdlVerifier" : "ReleaseByLocalTServer");
}

INSTANTIATE_TEST_CASE_P(
    VerifyTxnAtMaster, PgObjectLocksTestRF1SessionExpiryWithDdl,
    ::testing::Values(
      false,
      true
    ),
    DdlVerificationTaskToName);

// Test that the exclusive locks are released only after the DDL schema changes have been applied
// at the docdb layer.
#ifndef NDEBUG
TEST_F(PgObjectLocksTestRF1, ExclusiveLocksRemovedAfterDocDBSchemaChange) {
  CreateTestTable();
  // Force the alter table txn to just return success on commit, and not really wait for
  // docdb to apply the schema changes.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_report_ysql_ddl_txn_status_to_master) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_ddl_transaction_wait_for_ddl_verification) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_allow_wait_for_alter_table_to_finish) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_make_global_lock_release_async) = true;

  yb::SyncPoint::GetInstance()->LoadDependency({
    {"ExclusiveLocksRemovedAfterDocDBSchemaChange", "DoReleaseObjectLocksIfNecessary"}});
  yb::SyncPoint::GetInstance()->ClearTrace();
  yb::SyncPoint::GetInstance()->EnableProcessing();

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("ALTER TABLE test ADD COLUMN v1 INT"));

  auto status_future = std::async(std::launch::async, [&conn]() -> Status {
    RETURN_NOT_OK(conn.Fetch("SELECT * FROM test WHERE k=1"));
    return Status::OK();
  });

  EXPECT_OK(WaitFor([&]() {
    return NumWaitingLocks() >= 1;
  }, 5s * kTimeMultiplier, "Timed out waiting for num waiting locks == 1"));
  DEBUG_ONLY_TEST_SYNC_POINT("ExclusiveLocksRemovedAfterDocDBSchemaChange");
  ASSERT_OK(status_future.get());
}
#endif

class PgObjectLocksTest : public LibPqTestBase {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* opts) override {
    LibPqTestBase::UpdateMiniClusterOptions(opts);
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_vmodule) =
        yb::Format("libpq_utils=1,ts_local_lock_manager=2,$0", FLAGS_vmodule);
    ToggleDDLMode(opts->extra_tserver_flags, /* use_legacy = */ !EnableTableLocks());
    ToggleDDLMode(opts->extra_master_flags, /* use_legacy = */ !EnableTableLocks());
    opts->extra_tserver_flags.emplace_back("--enable_ysql_operation_lease=true");
    opts->extra_tserver_flags.emplace_back("--TEST_tserver_enable_ysql_lease_refresh=true");
    opts->extra_tserver_flags.emplace_back(
        Format("--ysql_lease_refresher_interval_ms=$0", kDefaultYSQLLeaseRefreshIntervalMilli));
    // yb_user_ddls_preempt_auto_analyze works only if enable_object_locking_for_table_locks is off,
    // so disable auto analyze in this test suite.
    opts->extra_tserver_flags.emplace_back("--ysql_enable_auto_analyze=false");
    opts->extra_tserver_flags.emplace_back("--log_ysql_catalog_versions=true");
    opts->extra_tserver_flags.emplace_back("--ysql_log_statement=all");
    opts->extra_tserver_flags.emplace_back(
        Format("--pg_client_session_expiration_ms=$0", kSessionTimeoutMs));
    opts->extra_tserver_flags.emplace_back(
        Format("--pg_client_heartbeat_interval_ms=$0", kSessionTimeoutMs / 3));

    opts->extra_master_flags.emplace_back("--enable_ysql_operation_lease=true");
    opts->extra_master_flags.emplace_back(
        Format("--master_ysql_operation_lease_ttl_ms=$0", MasterYSQLLeaseTTLMilli()));
  }

  int GetNumTabletServers() const override {
    return 3;
  }

  virtual uint64_t MasterYSQLLeaseTTLMilli() const {
    return kDefaultMasterYSQLLeaseTTLMilli;
  }

  virtual bool EnableTableLocks() const {
    return true;
  }

  void testConcurrentAlterSelect(bool fresh_catalog_cache) {
    const auto ts1_idx = 0;
    const auto ts2_idx = 1;
    auto* ts1 = cluster_->tablet_server(ts1_idx);
    auto* ts2 = cluster_->tablet_server(ts2_idx);

    auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
    auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));

    ASSERT_OK(conn1.Execute("CREATE TABLE t1(c1 INT, c2 INT)"));
    ASSERT_OK(conn2.Execute("INSERT INTO t1 VALUES (1, 2)"));

    if (fresh_catalog_cache) {
      // Reset to new conns so we don't retain any catalog cache entries
      conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
      conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));
    } else {
      auto row1 = ASSERT_RESULT(conn2.FetchRowAsString("SELECT * FROM t1"));
      LOG(INFO) << "Row was " << row1;
    }
    ASSERT_OK(cluster_->SetFlag(
        ts2,
        "TEST_tserver_disable_catalog_refresh_on_heartbeat",
        "true"));

    ASSERT_OK(conn1.Execute("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
    ASSERT_OK(conn2.Execute("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
    LOG(INFO) << "conn1: Acquiring exclusive lock on test table";
    ASSERT_OK(conn1.Execute("ALTER TABLE t1 DROP COLUMN c2"));

    TestThreadHolder thread_holder;
    thread_holder.AddThreadFunctor([&conn2]() {
      LOG(INFO) << "conn2: going to wait on conn1's table lock";
      // without propagation of inval msgs on lock acquire, this statement would hit a
      // schema mismatch error
      auto row2 = ASSERT_RESULT(conn2.FetchRowAsString("SELECT * FROM t1"));
      LOG(INFO) << "Row was " << row2;
    });

    ASSERT_OK(conn1.Execute("COMMIT"));
    thread_holder.JoinAll();
    ASSERT_OK(conn2.Execute("COMMIT"));

    ASSERT_OK(cluster_->SetFlag(
        ts2,
        "TEST_tserver_disable_catalog_refresh_on_heartbeat",
        "false"));
    LOG(INFO) << "Verifying new conns go through";
    auto conn4 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));
    ASSERT_OK(conn4.Fetch("SELECT * FROM t1"));
  }

  void testSyncReleaseForGlobalLocksAndDdls() {
    auto* ts1 = cluster_->tablet_server(0);
    auto* ts2 = cluster_->tablet_server(1);

    constexpr auto kRole = "test_role";
    constexpr auto kTable = "test_table";

    auto admin_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
    const auto db_name =
        ASSERT_RESULT(admin_conn.FetchRow<std::string>("SELECT current_database()"));

    ASSERT_OK(admin_conn.ExecuteFormat("CREATE TABLE $0 (k INT PRIMARY KEY, v INT)", kTable));
    ASSERT_OK(admin_conn.ExecuteFormat("INSERT INTO $0 VALUES (1, 10)", kTable));
    ASSERT_OK(admin_conn.ExecuteFormat("CREATE ROLE $0 LOGIN", kRole));
    ASSERT_OK(admin_conn.ExecuteFormat(
        "GRANT CONNECT ON DATABASE $0 TO $1",
        PqEscapeIdentifier(db_name),
        PqEscapeIdentifier(kRole)));
    ASSERT_OK(admin_conn.ExecuteFormat("GRANT USAGE ON SCHEMA public TO $0", kRole));
    ASSERT_OK(admin_conn.ExecuteFormat(
        "GRANT ALL PRIVILEGES ON TABLE $0 TO $1", kTable, kRole));

    auto role_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTsAsUser(*ts2, kRole));
    ASSERT_OK(role_conn.FetchMatrix(
        Format("SELECT * FROM $0 WHERE k = 1", kTable), 1 /* rows */, 2 /* columns */));

    ASSERT_OK(cluster_->SetFlag(
        ts2,
        "TEST_tserver_disable_catalog_refresh_on_heartbeat",
        "true"));

    // Delay the release of locks, so that the test fails if lock release is made async.
    ASSERT_OK(cluster_->SetFlagOnTServers("TEST_delay_release_locks_ms", "3000"));
    ASSERT_OK(admin_conn.ExecuteFormat("REVOKE ALL PRIVILEGES ON TABLE $0 FROM $1", kTable, kRole));

    ASSERT_NOK_STR_CONTAINS(
        role_conn.FetchMatrix(
            Format("SELECT * FROM $0 WHERE k = 1", kTable), 1 /* rows */, 2 /* columns */),
        "permission denied");

    ASSERT_OK(cluster_->SetFlag(
        ts2,
        "TEST_tserver_disable_catalog_refresh_on_heartbeat",
        "false"));
    ASSERT_OK(cluster_->SetFlagOnTServers("TEST_delay_release_locks_ms", "0"));
  }

 private:
  const uint64_t kSessionTimeoutMs = 10000 * kTimeMultiplier;
};

class PgObjectLocksTestAbortTxns : public PgObjectLocksTest,
                                   public ::testing::WithParamInterface<bool> {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* opts) override {
    PgObjectLocksTest::UpdateMiniClusterOptions(opts);
    opts->extra_master_flags.emplace_back("--ysql_colocate_database_by_default=true");
    opts->extra_tserver_flags.emplace_back("--ysql_colocate_database_by_default=true");
  }

  bool EnableTableLocks() const override {
    return GetParam();
  }
};

TEST_P(PgObjectLocksTestAbortTxns, TestDDLAbortsTxns) {
  auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
  ASSERT_OK(conn.Execute("CREATE DATABASE testdb with colocation=true"));

  auto conn1 = ASSERT_RESULT(ConnectToDB("testdb"));
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT) with (colocated=true)"));
  ASSERT_OK(conn1.Execute("CREATE TABLE test2(k INT PRIMARY KEY, v INT) with (colocated=true)"));

  ASSERT_OK(conn1.Execute("BEGIN TRANSACTION"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));

  auto conn2 = ASSERT_RESULT(ConnectToDB("testdb"));
  ASSERT_OK(conn2.Execute("BEGIN TRANSACTION"));
  ASSERT_OK(conn2.Execute("ALTER TABLE test2 ADD COLUMN v1 INT"));
  ASSERT_OK(conn2.Execute("COMMIT"));

  if (EnableTableLocks()) {
    ASSERT_OK(conn1.Execute("COMMIT"));
  } else {
    ASSERT_NOK(conn1.Execute("COMMIT"));
  }
}

INSTANTIATE_TEST_CASE_P(
    TableLocksEnabled, PgObjectLocksTestAbortTxns, ::testing::Bool(),
    ::testing::PrintToStringParamName());

// The cluster starts in the mode GetParam() asks for and TServer 1 is restarted into the opposite
// mode below, so the two connections used by the test run against nodes in different modes.
class PgObjectLocksTestAbortTxnsInMixedMode : public PgObjectLocksTestAbortTxns {};

// We create connections to 2 different nodes.
// And try combinations where the nodes may or may not be using table locks.
TEST_P(PgObjectLocksTestAbortTxnsInMixedMode, TestDDLAbortsTxnsInMixedMode) {
  {
    auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
    ASSERT_OK(conn.Execute("CREATE DATABASE testdb with colocation=true"));
    conn = ASSERT_RESULT(ConnectToDB("testdb"));
    ASSERT_OK(conn.Execute("CREATE TABLE test1(k INT PRIMARY KEY, v INT) with (colocated=true)"));
    ASSERT_OK(conn.Execute("CREATE TABLE test2(k INT PRIMARY KEY, v INT) with (colocated=true)"));
  }

  const bool ddl_using_table_locks = GetParam();
  const bool ts1_should_use_table_locks = !ddl_using_table_locks;

  auto* ts1 = cluster_->tablet_server(0);
  // Restart TServer 1. To start with table locks in the opposite mode of ddl_using_table_locks.
  ts1->Shutdown();
  LogWaiter log_waiter(ts1, "Received new lease epoch");
  // Append the flags here rather than pass them as an argument to Restart to ensure that these
  // take precedence over the value set by GetParam()
  ToggleDDLMode(*ts1->mutable_flags(), /* use_legacy = */ !ts1_should_use_table_locks);
  ASSERT_OK(ts1->Restart(ExternalMiniClusterOptions::kDefaultStartCqlProxy));
  ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts1, "testdb"));
  ASSERT_OK(conn1.Execute("BEGIN TRANSACTION"));
  ASSERT_OK(conn1.Execute("INSERT INTO test1 SELECT generate_series(1,11), 0"));

  auto* ts2 = cluster_->tablet_server(1);
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts2, "testdb"));
  ASSERT_OK(conn2.Execute("BEGIN TRANSACTION"));
  ASSERT_OK(conn2.Execute("ALTER TABLE test2 ADD COLUMN v1 INT"));
  ASSERT_OK(conn2.Execute("COMMIT"));

  // The DML txn should be aborted because at least one of the DDL/DML is not
  // using table locks.
  ASSERT_NOK(conn1.Execute("COMMIT"));
}

INSTANTIATE_TEST_CASE_P(
    TableLocksEnabled, PgObjectLocksTestAbortTxnsInMixedMode, ::testing::Bool(),
    ::testing::PrintToStringParamName());

class PgObjectLocksTestMixModeDuringPromotion : public PgObjectLocksTest {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* opts) override {
    PgObjectLocksTest::UpdateMiniClusterOptions(opts);
    // Start off with the flag disabled.
    opts->extra_tserver_flags.emplace_back("--ysql_enable_object_locking_infra=false");
    // ysql_enable_concurrent_ddl defaults to kEnableDdlTransactionBlocks, which is only true in
    // release builds. YBCIsLegacyModeForCatalogOps() is pinned to legacy without it, which would
    // leave the promotion unobservable in the catalog op mode, so pin it on for every build type.
    opts->extra_tserver_flags.emplace_back("--ysql_enable_concurrent_ddl=true");
  }
};

TEST_F(PgObjectLocksTestMixModeDuringPromotion, TestMixModeDuringPromotion) {
  {
    auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
    ASSERT_OK(conn.Execute("CREATE DATABASE testdb;"));
    conn = ASSERT_RESULT(ConnectToDB("testdb"));
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  }

  // Promote ts1 to use table locks before the DDL.
  auto* ts1 = cluster_->tablet_server(0);
  ASSERT_OK(cluster_->SetFlag(ts1, "ysql_enable_object_locking_infra", "true"));

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts1, "testdb"));
  ASSERT_OK(conn1.Execute("BEGIN TRANSACTION"));
  ASSERT_OK(conn1.Execute("ALTER TABLE test ADD COLUMN v1 INT"));

  auto* ts2 = cluster_->tablet_server(1);
  ASSERT_OK(cluster_->SetFlag(ts2, "ysql_enable_object_locking_infra", "true"));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts2, "testdb"));
  const auto kStatementTimeoutSec = 2;
  ASSERT_OK(conn2.ExecuteFormat("SET statement_timeout = '$0s';", kStatementTimeoutSec));

  // The DML should fail because it cannot get the table lock.
  ASSERT_NOK(conn2.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));

  ASSERT_OK(conn1.Execute("COMMIT"));

  // The DML should now succeed.
  ASSERT_OK(conn2.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));
}

TEST_F(
    PgObjectLocksTestMixModeDuringPromotion, TestLocksTakenAfterPromotionWithExistingConnections) {
  {
    auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
    ASSERT_OK(conn.Execute("CREATE DATABASE testdb;"));
    conn = ASSERT_RESULT(ConnectToDB("testdb"));
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  }

  // Promote ts1 to use table locks before the DDL.
  auto* ts1 = cluster_->tablet_server(0);
  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts1, "testdb"));
  auto* ts2 = cluster_->tablet_server(1);
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts2, "testdb"));

  ASSERT_OK(cluster_->SetFlag(ts1, "ysql_enable_object_locking_infra", "true"));
  ASSERT_OK(cluster_->SetFlag(ts2, "ysql_enable_object_locking_infra", "true"));

  ASSERT_OK(conn1.Execute("BEGIN TRANSACTION"));
  ASSERT_OK(conn1.Execute("ALTER TABLE test ADD COLUMN v1 INT"));

  const auto kStatementTimeoutSec = 2;
  ASSERT_OK(conn2.ExecuteFormat("SET statement_timeout = '$0s';", kStatementTimeoutSec));

  // The DML should fail because it cannot get the table lock.
  ASSERT_NOK(conn2.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));
}

TEST_F(PgObjectLocksTestMixModeDuringPromotion, TestConcurrentTxnsMayNotTakeTableLocks) {
  {
    auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
    ASSERT_OK(conn.Execute("CREATE DATABASE testdb;"));
    conn = ASSERT_RESULT(ConnectToDB("testdb"));
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  }

  // Promote ts1 to use table locks before the DDL.
  auto* ts1 = cluster_->tablet_server(0);
  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts1, "testdb"));
  auto* ts2 = cluster_->tablet_server(1);
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts2, "testdb"));

  ASSERT_OK(conn1.Execute("BEGIN TRANSACTION"));

  ASSERT_OK(cluster_->SetFlag(ts1, "ysql_enable_object_locking_infra", "true"));
  ASSERT_OK(cluster_->SetFlag(ts2, "ysql_enable_object_locking_infra", "true"));

  ASSERT_OK(conn1.Execute("ALTER TABLE test ADD COLUMN v1 INT"));

  const auto kStatementTimeoutSec = 2;
  ASSERT_OK(conn2.ExecuteFormat("SET statement_timeout = '$0s';", kStatementTimeoutSec));

  // The DML should NOT fail because it can get the table lock.
  ASSERT_OK(conn2.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));
}

// The object locking infra auto flag is latched once per transaction. A transaction that began
// before the promotion must not switch to the object locking catalog op mode partway through:
// catalog reads would move from the legacy catalog session to the transactional session with a
// catalog snapshot read time, changing catalog visibility under an open transaction.
//
// The switch is observed through the read point that PgSession::UpdateReadPointForCatalogOps()
// logs under yb_debug_log_snapshot_mgmt, which runs only on the non-legacy path.
TEST_F(PgObjectLocksTestMixModeDuringPromotion, TestCatalogOpModeStableAcrossPromotion) {
  const std::string kCatalogSnapshotLogLine = "Using catalog snapshot read time serial number";
  {
    auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
    ASSERT_OK(conn.Execute("CREATE DATABASE testdb;"));
    conn = ASSERT_RESULT(ConnectToDB("testdb"));
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  }

  auto* ts1 = cluster_->tablet_server(0);
  auto admin = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts1, "testdb"));
  auto conn = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts1, "testdb"));
  ASSERT_OK(conn.Execute("SET yb_debug_log_snapshot_mgmt = true"));

  {
    LogWaiter log_waiter(ts1, kCatalogSnapshotLogLine);

    // This transaction latches the pre-promotion value of the auto flag.
    ASSERT_OK(conn.Execute("BEGIN TRANSACTION"));
    ASSERT_OK(conn.FetchAllAsString("SELECT * FROM test"));

    ASSERT_OK(cluster_->SetFlag(ts1, "ysql_enable_object_locking_infra", "true"));

    // Catalog work after the promotion, including a relation this backend has never seen, so the
    // lookup cannot be served from its caches and has to issue a catalog read.
    ASSERT_OK(admin.Execute("CREATE TABLE cold(k INT PRIMARY KEY, v INT)"));
    ASSERT_OK(conn.FetchAllAsString("SELECT * FROM cold"));
    ASSERT_OK(conn.Execute("INSERT INTO test VALUES (1, 1)"));
    ASSERT_OK(conn.Execute("COMMIT"));

    ASSERT_FALSE(log_waiter.IsEventOccurred())
        << "Transaction switched to the object locking catalog op mode after it had already "
        << "started: " << log_waiter.matched_log_line();
  }

  // The next transaction latches the promoted value and does use the new catalog op mode. It needs
  // a catalog read of its own, so read another relation this backend has not seen before.
  {
    ASSERT_OK(admin.Execute("CREATE TABLE cold2(k INT PRIMARY KEY, v INT)"));
    LogWaiter log_waiter(ts1, kCatalogSnapshotLogLine);
    ASSERT_OK(conn.Execute("BEGIN TRANSACTION"));
    ASSERT_OK(conn.FetchAllAsString("SELECT * FROM cold2"));
    ASSERT_OK(conn.Execute("COMMIT"));
    ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 30)));
  }
}

// Transactional DDL is part of the object locking feature, so it follows the object locking infra
// auto flag that the transaction latched when it began. A transaction that started before the
// promotion must keep running DDLs in an autonomous transaction, so its CREATE TABLE survives a
// rollback; the next transaction runs the DDL inside the transaction block, so its CREATE TABLE is
// rolled back with it.
TEST_F(PgObjectLocksTestMixModeDuringPromotion, TestDdlTxnBlockModeStableAcrossPromotion) {
  {
    auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
    ASSERT_OK(conn.Execute("CREATE DATABASE testdb;"));
  }

  auto* ts1 = cluster_->tablet_server(0);
  auto conn = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts1, "testdb"));

  // This transaction latches the pre-promotion value of the auto flag.
  ASSERT_OK(conn.Execute("BEGIN TRANSACTION"));
  ASSERT_OK(conn.Execute("CREATE TABLE pre_promotion(k INT PRIMARY KEY)"));

  ASSERT_OK(cluster_->SetFlag(ts1, "ysql_enable_object_locking_infra", "true"));

  ASSERT_OK(conn.Execute("ROLLBACK"));
  // The DDL ran in an autonomous transaction, so the table is still there.
  ASSERT_OK(conn.FetchAllAsString("SELECT * FROM pre_promotion"));

  // The next transaction latches the promoted value and runs the DDL inside the transaction block.
  ASSERT_OK(conn.Execute("BEGIN TRANSACTION"));
  ASSERT_OK(conn.Execute("CREATE TABLE post_promotion(k INT PRIMARY KEY)"));
  ASSERT_OK(conn.Execute("ROLLBACK"));
  ASSERT_NOK(conn.FetchAllAsString("SELECT * FROM post_promotion"));
}

// A statement that commits intermediate transactions (ANALYZE on multiple tables here) re-latches
// the object locking infra auto flag at every one of those commits. If the flag is promoted while
// such a statement is in flight, the rest of the statement would run in the new mode while the
// state it already built belongs to the old one, so the statement must fail instead.
// TODO(#34635): Pin the latched value for the statement and replace this with an assertion that
// the statement succeeds.
TEST_F(PgObjectLocksTestMixModeDuringPromotion, TestModeChangeDuringStatementFails) {
  {
    auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
    ASSERT_OK(conn.Execute("CREATE DATABASE testdb;"));
    conn = ASSERT_RESULT(ConnectToDB("testdb"));
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
    ASSERT_OK(conn.Execute("CREATE TABLE test2(k INT PRIMARY KEY, v INT)"));
  }

  auto* ts1 = cluster_->tablet_server(0);
  // Used to observe that the promotion has reached this tserver's backends.
  auto observer = ASSERT_RESULT(LibPqTestBase::ConnectToTsForDB(*ts1, "testdb"));

  // The pipeline mode keeps Parse and Execute in separate protocol messages of one open
  // transaction: Parse starts the transaction and latches the auto flag, and the backend applies
  // the promotion in its message loop before it runs Execute.
  const auto conn_str = Format(
      "host=$0 port=$1 dbname=testdb user=$2", ts1->bind_host(), ts1->ysql_port(),
      PGConnSettings::kDefaultUser);
  PGConnPtr conn(PQconnectdb(conn_str.c_str()));
  ASSERT_EQ(PQstatus(conn.get()), CONNECTION_OK) << PQerrorMessage(conn.get());
  ASSERT_EQ(PQenterPipelineMode(conn.get()), 1) << PQerrorMessage(conn.get());

  // ANALYZE of more than one relation processes each in its own transaction, committing the
  // previous one through YbCommitTransactionCommandIntermediate() (vacuum.c).
  ASSERT_EQ(
      PQsendPrepare(conn.get(), "analyze_stmt", "ANALYZE test, test2", 0, nullptr), 1)
      << PQerrorMessage(conn.get());
  ASSERT_EQ(PQsendFlushRequest(conn.get()), 1) << PQerrorMessage(conn.get());
  ASSERT_EQ(PQflush(conn.get()), 0) << PQerrorMessage(conn.get());
  {
    PGResultPtr res(PQgetResult(conn.get()));
    ASSERT_EQ(PQresultStatus(res.get()), PGRES_COMMAND_OK) << PQresultErrorMessage(res.get());
  }

  ASSERT_OK(cluster_->SetFlag(ts1, "ysql_enable_object_locking_infra", "true"));
  // The flag reaches a backend as a SIGHUP that it applies in its message loop. Wait for another
  // backend on this tserver to have applied it, which means the signal has been delivered.
  ASSERT_OK(LoggedWaitFor(
      [&observer]() -> Result<bool> {
        return VERIFY_RESULT(observer.FetchRow<std::string>(
                   "SHOW enable_object_locking_infra")) == "on";
      },
      MonoDelta::FromSeconds(kTimeMultiplier * 60), "promotion to reach the backends"));

  ASSERT_EQ(
      PQsendQueryPrepared(conn.get(), "analyze_stmt", 0, nullptr, nullptr, nullptr, 0), 1)
      << PQerrorMessage(conn.get());
  ASSERT_EQ(PQpipelineSync(conn.get()), 1) << PQerrorMessage(conn.get());
  ASSERT_EQ(PQflush(conn.get()), 0) << PQerrorMessage(conn.get());

  // A NULL result only delimits one pipelined command's results from the next, so read on until
  // the sync point that ends the pipeline.
  std::string error_message;
  for (bool done = false; !done;) {
    PGResultPtr res(PQgetResult(conn.get()));
    if (!res) {
      ASSERT_NE(PQstatus(conn.get()), CONNECTION_BAD) << PQerrorMessage(conn.get());
      continue;
    }
    const auto status = PQresultStatus(res.get());
    if (status == PGRES_PIPELINE_SYNC) {
      done = true;
    } else if (status == PGRES_FATAL_ERROR) {
      error_message = PQresultErrorMessage(res.get());
    }
  }

  LOG(INFO) << "ANALYZE returned: " << error_message;
  ASSERT_STR_CONTAINS(error_message, "transactional DDL was enabled while the statement was "
                                     "running");
}

// Test for #32224.
//
// A transaction that started before the object locking infra auto flag was promoted keeps running
// in the legacy mode: it takes no object locks, so the tserver catalog version check is what stops
// it from using a catalog cache that a breaking DDL (REVOKE here) invalidated. The check must be
// skipped based on the mode of the request's transaction, not on the tserver's flags.
TEST_F(PgObjectLocksTestMixModeDuringPromotion, LegacyTxnKeepsCatalogVersionCheck) {
  constexpr auto kInvalidatedMsg =
      "The catalog snapshot used for this transaction has been invalidated";
  auto* ts1 = cluster_->tablet_server(0);
  auto* ts2 = cluster_->tablet_server(1);
  {
    auto conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
    ASSERT_OK(conn.Execute("CREATE ROLE r"));
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
    ASSERT_OK(conn.Execute("INSERT INTO test SELECT generate_series(1, 10), 0"));
    ASSERT_OK(conn.Execute("GRANT SELECT ON test TO r"));
    ASSERT_OK(conn.Execute("CREATE TABLE unrelated(k INT PRIMARY KEY, v INT)"));
    ASSERT_OK(conn.Execute("CREATE INDEX unrelated_idx ON unrelated(v)"));
  }

  auto legacy_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(legacy_conn.Execute("SET ROLE r"));
  ASSERT_OK(legacy_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
  ASSERT_EQ(ASSERT_RESULT(legacy_conn.FetchRow<int64_t>("SELECT COUNT(*) FROM test")), 10);

  for (size_t i = 0; i < cluster_->num_tablet_servers(); ++i) {
    ASSERT_OK(cluster_->SetFlag(
        cluster_->tablet_server(i), "ysql_enable_object_locking_infra", "true"));
  }

  // A new mode transaction open across a breaking DDL must not be subjected to the check.
  auto new_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(new_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
  ASSERT_EQ(ASSERT_RESULT(new_conn.FetchRow<int64_t>("SELECT COUNT(*) FROM test")), 10);

  auto ddl_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));
  ASSERT_OK(ddl_conn.Execute("ALTER INDEX unrelated_idx RENAME TO unrelated_idx2"));
  ASSERT_OK(ddl_conn.Execute("REVOKE SELECT ON test FROM r"));
  WaitForCatalogVersionToPropagate();

  ASSERT_EQ(ASSERT_RESULT(new_conn.FetchRow<int64_t>("SELECT COUNT(*) FROM test")), 10);
  ASSERT_OK(new_conn.Execute("COMMIT"));

  // Before the fix, the legacy transaction kept reading the table with the revoked privilege.
  auto result = legacy_conn.FetchRow<int64_t>("SELECT COUNT(*) FROM test");
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.status().ToString(), kInvalidatedMsg);
  ASSERT_OK(legacy_conn.Execute("ROLLBACK"));

  // The next transaction on the same connection runs in the new mode and sees the REVOKE.
  result = legacy_conn.FetchRow<int64_t>("SELECT COUNT(*) FROM test");
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.status().ToString(), "permission denied");
}

TEST_F(PgObjectLocksTest, ExclusiveLockReleaseInvalidatesCatalogCache) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  ts2->Shutdown();
  LogWaiter log_waiter(ts2, "Received new lease epoch");
  ASSERT_OK(ts2->Restart(
      ExternalMiniClusterOptions::kDefaultStartCqlProxy,
      {std::make_pair("TEST_ysql_disable_transparent_cache_refresh_retry", "true")}));
  ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));

  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));

  ASSERT_OK(conn2.FetchMatrix("SELECT * FROM test WHERE k=1", 1 /* rows */, 2 /* columns */));

  // Disable catalog cache invalidation on tserver-master heartbeat path. Set it after the tserver
  // boots up since setting it as part of initialization seems to error.
  ASSERT_OK(cluster_->SetFlag(
      ts2,
      "TEST_tserver_disable_catalog_refresh_on_heartbeat",
      "true"));

  // Release of exclusive locks of the below DDL should invalidate catalog cache on all tservers.
  ASSERT_OK(conn1.Execute("ALTER TABLE test ADD COLUMN v1 INT"));
  // The DML should now see the updated schema and not hit a catalog cache/schema version mismatch.
  ASSERT_OK(conn2.FetchMatrix("SELECT * FROM test WHERE k=1", 1 /* rows */, 3 /* columns */));
}

TEST_F(PgObjectLocksTest, ConcurrentAlterSelectFreshCache) {
  testConcurrentAlterSelect(true);
}

TEST_F(PgObjectLocksTest, ConcurrentAlterSelect) {
  testConcurrentAlterSelect(false);
}

// Uses a longer YSQL operation lease TTL. On oversubscribed hosts a transient stall of the tserver
// lease refresher can otherwise expire the lease and kill an in-flight PG backend mid-statement.
class PgObjectLocksTestLongLease : public PgObjectLocksTest {
 protected:
  uint64_t MasterYSQLLeaseTTLMilli() const override {
    return 20 * 1000;
  }
};

TEST_F_EX(PgObjectLocksTest, BackfillIndexSanityTest, PgObjectLocksTestLongLease) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));

  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test select generate_series(1, 100000)"));
  ASSERT_OK(cluster_->SetFlag(
      ts2,
      "TEST_tserver_disable_catalog_refresh_on_heartbeat",
      "true"));

  ASSERT_OK(conn1.Execute("CREATE INDEX test_k ON test(k)"));

  std::string select_query = "SELECT * FROM test WHERE k=1";
  auto analyze_query = Format("EXPLAIN ANALYZE $0", Format(select_query));
  auto values = ASSERT_RESULT(conn2.FetchRows<std::string>(analyze_query));
  const auto index_only_scan_test = "Index Only Scan using test_k on test";
  const auto index_cond_text = "Index Cond: (k = 1)";
  bool found_index_only_scan = false;
  bool found_index_cond = false;
  for (const auto& value : values) {
    if (value.find(index_only_scan_test) != std::string::npos) {
      found_index_only_scan = true;
    } else if (value.find(index_cond_text) != std::string::npos) {
      found_index_cond = true;
    }
  }
  ASSERT_TRUE(found_index_only_scan && found_index_cond)
      << "Expected following text - \n"
      << "1. " << index_only_scan_test << "\n"
      << "2. " << index_cond_text << "\n"
      << "in " << analyze_query;
  ASSERT_OK(conn2.Execute("INSERT INTO test values(200000)"));
}

// Concurrent index creation is done in 4 phases/txns (with object locking enabled) - briefly,
// creation of index table, updating index perms to indisready=true, backfill, and finally
// updating the index to indisvalid=true. At the end of the first, phase/txn a session lock
// of type ShareUpdateExclusiveLock is taken on the parent relation to prevent other schema
// changes from creeping in before the lock is taken again in the next phase(s). This test
// asserts that the session lock is honored in YB.
TEST_F(PgObjectLocksTest, ConcurrentIndexCreationTakesSessionLock) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));

  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test select generate_series(1, 10000)"));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "ysql_yb_disable_wait_for_backends_catalog_version", "false"));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "TEST_pause_wait_for_lockers", "true"));

  LogWaiter log_waiter(ts1,
                       "Pausing due to flag TEST_pause_wait_for_lockers");
  TestThreadHolder thread_holder;
  thread_holder.AddThreadFunctor([&]() {
    ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 30)));

    // Alter should fail since it requests AccessExclusive lock on the base table, which
    // conflicts with active session lock of type ShareUpdateExclusive held by create index.
    ASSERT_OK(conn2.Execute("SET statement_timeout = '5s';"));
    ASSERT_NOK(conn2.Execute("ALTER TABLE test ADD COLUMN v1 INT DEFAULT 0"));
    // Insert would go through since RowExclusive doesn't conflict with ShareUpdateExclusive.
    ASSERT_OK(conn2.Execute("INSERT INTO test values(200000)"));

    ASSERT_OK(cluster_->SetFlagOnTServers(
        "TEST_pause_wait_for_lockers", "false"));
  });

  ASSERT_OK(conn1.Execute("CREATE INDEX test_k ON test(k)"));
  thread_holder.JoinAll();
  // Alter should go through now since the session lock should have been released.
  ASSERT_OK(conn2.Execute("ALTER TABLE test ADD COLUMN v1 INT DEFAULT 0"));
}

TEST_F(PgObjectLocksTest, YB_DISABLE_TEST_IN_TSAN(SessionLocksAreReleasedOnBackendErrorsOrKill)) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));

  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test select generate_series(1, 10000)"));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "ysql_yb_disable_wait_for_backends_catalog_version", "false"));
  // Force the create index to fail so that the backend would trigger the abort path.
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "TEST_fail_wait_for_lockers", "true"));

  ASSERT_NOK(conn1.Execute("CREATE INDEX test_k ON test(k)"));
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "TEST_fail_wait_for_lockers", "false"));
  // Alter should go through since the backend should have released the lock on error.
  ASSERT_OK(conn2.Execute("ALTER TABLE test ADD COLUMN v1 INT DEFAULT 0"));

  // Stall the create index after acquiring the session lock and kill the backend.
  ASSERT_OK(cluster_->SetFlagOnTServers(
      "TEST_pause_wait_for_lockers", "true"));
  LogWaiter log_waiter(ts1,
                       "Pausing due to flag TEST_pause_wait_for_lockers");
  TestThreadHolder thread_holder;
  auto conn1_pid = ASSERT_RESULT(conn1.FetchRow<int32_t>("SELECT pg_backend_pid();"));
  thread_holder.AddThreadFunctor([&]() {
    ASSERT_NOK(conn1.Execute("CREATE INDEX test_k_1 ON test(k)"));
  });
  ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 30)));
  auto conn3 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));

  LOG(INFO) << "Killing pid " << conn1_pid;
  ASSERT_EQ(
      ASSERT_RESULT(
          conn3.FetchRowAsString(yb::Format("SELECT pg_terminate_backend($0);", conn1_pid))),
      "1");

  ASSERT_OK(cluster_->SetFlagOnTServers(
      "TEST_pause_wait_for_lockers", "false"));
  // Alter should go through since the tserver cleans up locks of expired backends.
  ASSERT_OK(conn2.Execute("ALTER TABLE test ADD COLUMN v2 INT DEFAULT 0"));
}

TEST_F(PgObjectLocksTest, ReleaseExpiredLocksInvalidatesCatalogCache) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  ts2->Shutdown();
  {
    LogWaiter log_waiter(ts2, "Received new lease epoch");
    ASSERT_OK(ts2->Restart(
        ExternalMiniClusterOptions::kDefaultStartCqlProxy,
        {std::make_pair("TEST_ysql_disable_transparent_cache_refresh_retry", "true")}));
    ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));
  }

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));

  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));

  ASSERT_OK(conn2.FetchMatrix("SELECT * FROM test WHERE k=1", 1 /* rows */, 2 /* columns */));

  ASSERT_OK(cluster_->SetFlag(
      ts2,
      "TEST_tserver_disable_catalog_refresh_on_heartbeat",
      "true"));
  auto master_leader = cluster_->GetLeaderMaster();
  ASSERT_OK(cluster_->SetFlag(
      master_leader,
      "TEST_disable_release_object_locks_on_ddl_verification",
      "true"));

  {
    LogWaiter log_waiter(ts2, "Invalidating PgTableCache with filter=");
    ASSERT_OK(conn1.Execute("ALTER TABLE test ADD COLUMN v1 INT"));
    ts1->Shutdown();
    ASSERT_OK(log_waiter.WaitFor(2s * kTimeMultiplier * kDefaultMasterYSQLLeaseTTLMilli));
  }
  ASSERT_OK(conn2.FetchMatrix("SELECT * FROM test WHERE k=1", 1 /* rows */, 3 /* columns */));
  ASSERT_OK(ts1->Restart());
}

TEST_F(PgObjectLocksTest, CreateIndexWaitsForInProgressWriteTxn) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test VALUES (0,0)"));

  LogWaiter log_waiter(ts1, "to wait-for-lockers tracker");
  ASSERT_OK(cluster_->SetFlag(
      ts1,
      "TEST_pause_wait_for_lockers",
      "true"));
  auto status_future = std::async(std::launch::async, [&]() {
    auto conn2 = VERIFY_RESULT(LibPqTestBase::ConnectToTs(*ts2));
    return conn2.Execute("CREATE INDEX idx on test(v)");
  });
  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.Execute("INSERT INTO test VALUES (1,1)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test VALUES (2,2)"));
  ASSERT_OK(cluster_->SetFlag(
      ts1,
      "vmodule",
      "object_lock_manager*=1"));
  ASSERT_OK(cluster_->SetFlag(
      ts1,
      "TEST_pause_wait_for_lockers",
      "false"));
  ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));
  ASSERT_OK(cluster_->SetFlag(
      ts1,
      "vmodule",
      "object_lock_manager*=0"));
  bool create_index_waits_for_in_progress_txn = true;
  if (status_future.wait_for(3s * kTimeMultiplier) == std::future_status::ready) {
    LOG(WARNING) << "CREATE INDEX didn't wait for in progress transaction";
    create_index_waits_for_in_progress_txn = false;
  }
  ASSERT_OK(conn1.Execute("COMMIT"));
  ASSERT_EQ(status_future.wait_for(30s * kTimeMultiplier), std::future_status::ready);

  auto values = ASSERT_RESULT(
      conn1.FetchRows<std::string>("EXPLAIN SELECT * FROM test WHERE v=1"));
  const auto scan_type_index_only = "Index Scan using idx on test";
  bool is_index_only_scan = false;
  for (const auto& value : values) {
    if (value.find(scan_type_index_only) != std::string::npos) {
      is_index_only_scan = true;
    }
  }
  ASSERT_TRUE(is_index_only_scan);
  ASSERT_EQ(ASSERT_RESULT(conn1.FetchRow<PGUint64>("SELECT COUNT(*) FROM test WHERE v=2")), 1);
  ASSERT_EQ(ASSERT_RESULT(conn1.FetchRow<PGUint64>("SELECT COUNT(*) FROM test WHERE v=1")), 1);
  ASSERT_EQ(ASSERT_RESULT(conn1.FetchRow<PGUint64>("SELECT COUNT(*) FROM test WHERE v=0")), 1);
  ASSERT_TRUE(create_index_waits_for_in_progress_txn);
}

TEST_F(PgObjectLocksTest, WaitForLockersSeesFastpathDmlLocks) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test VALUES (0, 0)"));

  ASSERT_OK(cluster_->SetFlagOnTServers("ysql_yb_test_block_index_phase", "backfill"));
  LogWaiter log_waiter_block(ts2, "blocking concurrent index backfill");
  auto create_index_future = std::async(std::launch::async, [&]() {
    auto conn2 = VERIFY_RESULT(LibPqTestBase::ConnectToTs(*ts2));
    return conn2.Execute("CREATE INDEX idx ON test(v)");
  });
  CancelableScopeExit cleanup([&]() {
    WARN_NOT_OK(
        cluster_->SetFlagOnTServers("ysql_yb_test_block_index_phase", "none"),
        "Failed to unblock CREATE INDEX");
  });
  ASSERT_OK(log_waiter_block.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 30)));

  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.Execute("UPDATE test SET v = 1 WHERE k = 0"));

  ASSERT_OK(cluster_->SetFlag(ts1, "vmodule", "object_lock_manager*=1"));
  LogWaiter log_waiter_conflict(ts1, "to wait-for-lockers tracker");
  cleanup.Cancel();
  ASSERT_OK(cluster_->SetFlagOnTServers("ysql_yb_test_block_index_phase", "none"));
  ASSERT_OK(log_waiter_conflict.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));
  ASSERT_EQ(create_index_future.wait_for(3s * kTimeMultiplier), std::future_status::timeout)
      << "CREATE INDEX did not wait for the in-progress UPDATE";

  ASSERT_OK(conn1.Execute("COMMIT"));
  ASSERT_OK(create_index_future.get());
}

TEST_F(PgObjectLocksTest, RetryExclusiveLockOnTserverLeaseRefresh) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));
  ASSERT_OK(conn2.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn2.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn2.Execute("LOCK TABLE test IN ACCESS SHARE MODE"));

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(
      cluster_->SetFlag(ts2, "vmodule", yb::Format("object_lock_manager=1,$0", FLAGS_vmodule)));
  LogWaiter log_waiter(ts2, "added to wait-queue");
  auto status_future = std::async(std::launch::async, [&]() -> Status {
    return conn1.Execute("LOCK TABLE test IN ACCESS EXCLUSIVE MODE");
  });
  ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));

  // The exclusive lock request should still go through and not error due to membership changes.
  ts2->Shutdown(SafeShutdown::kTrue);
  ASSERT_EQ(
      status_future.wait_for(2s * kTimeMultiplier * kDefaultMasterYSQLLeaseTTLMilli),
      std::future_status::ready);
  ASSERT_OK(status_future.get());
  ASSERT_OK(conn1.CommitTransaction());
}

TEST_F(PgObjectLocksTest, VerifyLockTimeout) {
  constexpr int kLockTimeoutDurationMs = 3000;
  constexpr int kExtraTimeoutMs = 1000;

  auto setup_conn = ASSERT_RESULT(Connect());
  ASSERT_OK(setup_conn.Execute("CREATE TABLE t (k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(setup_conn.Execute("INSERT INTO t VALUES (1, 1)"));

  auto conn1 = ASSERT_RESULT(Connect());
  auto conn2 = ASSERT_RESULT(Connect());

  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn1.Execute("LOCK TABLE t IN ACCESS EXCLUSIVE MODE;"));

  ASSERT_OK(conn2.ExecuteFormat("SET lock_timeout='$0ms';", kLockTimeoutDurationMs));
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));

  std::future<Status> conn2_lock_future = std::async(std::launch::async, [&]() -> Status {
    return conn2.Execute("LOCK TABLE t IN ACCESS EXCLUSIVE MODE;");
  });

  // Wait briefly to confirm that conn2 is actually blocking on the lock.
  ASSERT_EQ(conn2_lock_future.wait_for(
      std::chrono::seconds(1)), std::future_status::timeout);

  auto expected_timeout_ms =
      kLockTimeoutDurationMs + FLAGS_pg_client_extra_timeout_ms + kExtraTimeoutMs;
  ASSERT_EQ(conn2_lock_future.wait_for(
      std::chrono::milliseconds(expected_timeout_ms * kTimeMultiplier)), std::future_status::ready);

  // Verify the lock attempt fails with lock timeout error.
  Status result = conn2_lock_future.get();
  ASSERT_NOK(result);
  ASSERT_STR_CONTAINS(result.ToString(), "canceling statement due to lock timeout");

  // Conn1 should still be able to commit
  ASSERT_OK(conn1.CommitTransaction());
}

TEST_F(PgObjectLocksTest, BootstrapLocksHasStatusTabetIdForTxns) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));

  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn1.Execute("ALTER TABLE test ADD COLUMN v1 INT DEFAULT 0"));

  ts2->Shutdown();
  {
    LogWaiter log_waiter(ts2, "BootstrapDdlObjectLocks: success");
    ASSERT_OK(ts2->Restart());
    ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));
  }

  ASSERT_OK(conn1.CommitTransaction());

  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));
  ASSERT_OK(conn2.FetchMatrix("SELECT * FROM test WHERE k=1", 1 /* rows */, 3 /* columns */));
}

TEST_F(PgObjectLocksTest, TestGlobalReleaseForFailedDdlsBeforeMetadataIsSet) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(1,11), 0"));

  ts2->Shutdown();
  {
    LogWaiter log_waiter(ts2, "Received new lease epoch");
    ASSERT_OK(ts2->Restart(
        ExternalMiniClusterOptions::kDefaultStartCqlProxy,
        {std::make_pair("TEST_fail_create_table_rpc", "true")}));
    ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));
  }
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn2.Execute("LOCK TABLE test in ACCESS EXCLUSIVE mode"));
  ASSERT_NOK(conn2.Execute("CREATE TABLE test_fail(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn2.RollbackTransaction());

  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn1.Execute("LOCK TABLE test in ACCESS EXCLUSIVE mode"));
  ASSERT_OK(conn1.Execute("CREATE TABLE test_fail(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.CommitTransaction());
}

TEST_F(PgObjectLocksTest, TestSyncReleaseForGlobalLocksAndDdls) {
  testSyncReleaseForGlobalLocksAndDdls();
}

// Multi-node variant: verifies that pg_locks.ybdetails.blocked_by is populated for a waiting object
// lock even when the blocker, the waiter, and the observer are all on different nodes. This
// exercises the cross-node blocker aggregation in PgClientService::GetObjectLockStatus (the blocker
// holds its lock on ts0, the DDL waits there via the global lock, and the observer reads from ts2).
TEST_F(PgObjectLocksTest, TestPgLocksBlockedByForObjectLocksMultiNode) {
  auto* ts_blocker = cluster_->tablet_server(0);
  auto* ts_waiter = cluster_->tablet_server(1);
  auto* ts_observer = cluster_->tablet_server(2);

  auto setup_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts_blocker));
  ASSERT_OK(setup_conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(setup_conn.Execute("INSERT INTO test SELECT generate_series(1, 10), 0"));

  auto observer_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts_observer));
  ASSERT_OK(observer_conn.Execute("SET yb_locks_min_txn_age='0s'"));

  // Blocker on ts0 holds a ROW EXCLUSIVE lock via a write in an explicit transaction. Using a write
  // (rather than a read-only ACCESS SHARE) ensures the blocker's transaction is registered at a
  // status tablet and therefore visible in pg_locks with its transaction id.
  auto blocker_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts_blocker));
  ASSERT_OK(blocker_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
  ASSERT_OK(blocker_conn.Execute("INSERT INTO test VALUES (100, 100)"));

  // Waiter on ts1 issues a DDL needing ACCESS EXCLUSIVE (a global lock), which conflicts with the
  // blocker's ROW EXCLUSIVE lock held on ts0 and must wait.
  auto waiter_future = std::async(std::launch::async, [&]() -> Status {
    auto waiter_conn = VERIFY_RESULT(LibPqTestBase::ConnectToTs(*ts_waiter));
    return waiter_conn.Execute("ALTER TABLE test ADD COLUMN v1 INT");
  });

  // Poll pg_locks from the observer (ts2) until the waiting object lock reports a blocker that is
  // also visible as a granted holder. Both facts come from independent nodes, so the waiter can
  // show up first.
  std::string blocked_by;
  bool references_blocker = false;
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    const auto dump = VERIFY_RESULT(observer_conn.FetchAllAsString(
        "SELECT granted, mode, ybdetails->>'transactionid' AS txn,"
        " (ybdetails->'blocked_by')::text AS blocked_by FROM pg_locks"
        " WHERE relation = 'test'::regclass AND locktype = 'relation' ORDER BY granted"));
    LOG(INFO) << "object locks on test:\n" << dump;
    blocked_by = VERIFY_RESULT(observer_conn.FetchRow<std::string>(
        "SELECT COALESCE("
        "  (SELECT ybdetails->'blocked_by' FROM pg_locks"
        "     WHERE NOT granted AND relation = 'test'::regclass AND locktype = 'relation'"
        "       AND ybdetails->'blocked_by' IS NOT NULL"
        "     LIMIT 1), '[]'::jsonb)::text"));
    if (blocked_by == "[]" || blocked_by.empty()) {
      return false;
    }
    references_blocker = VERIFY_RESULT(observer_conn.FetchRow<bool>(
        "SELECT EXISTS ("
        "  SELECT 1 FROM pg_locks w JOIN pg_locks g"
        "    ON g.granted AND g.relation = 'test'::regclass AND g.locktype = 'relation'"
        "       AND g.ybdetails->>'transactionid' IS NOT NULL"
        "  WHERE NOT w.granted AND w.relation = 'test'::regclass AND w.locktype = 'relation'"
        "    AND w.ybdetails->'blocked_by' @> to_jsonb(g.ybdetails->>'transactionid'))"));
    return references_blocker;
  }, 60s * kTimeMultiplier, "Timed out waiting for blocked_by to reference the granted holder"));

  EXPECT_TRUE(references_blocker)
      << "blocked_by did not reference the granted holder; blocked_by=" << blocked_by;

  // Releasing the blocker should let the waiter's DDL complete.
  ASSERT_OK(blocker_conn.Execute("COMMIT"));
  ASSERT_OK(waiter_future.get());
}

YB_STRONGLY_TYPED_BOOL(DoMasterFailover);
YB_STRONGLY_TYPED_BOOL(UseExplicitLocksInsteadOfDdl);
class PgObjecLocksTestOutOfOrderMessageHandling
    : public PgObjectLocksTest,
      public ::testing::WithParamInterface<
          std::tuple<DoMasterFailover, UseExplicitLocksInsteadOfDdl>> {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* opts) override {
    PgObjectLocksTest::UpdateMiniClusterOptions(opts);
    opts->num_masters = (ShouldDoMasterFailover() ? 3 : 1);
    opts->extra_master_flags.emplace_back(
        yb::Format("--pg_client_extra_timeout_ms=$0", kPgClientExtraTimeoutMs));
    opts->extra_tserver_flags.emplace_back(
        yb::Format("--vmodule=ts_local_lock_manager=2,$0", FLAGS_vmodule));
  }

  DoMasterFailover ShouldDoMasterFailover() const {
    return std::get<0>(GetParam());
  }

  UseExplicitLocksInsteadOfDdl ShouldUseExplicitLocksInsteadOfDdl() const {
    return std::get<1>(GetParam());
  }

  static constexpr auto kPgClientExtraTimeoutMs = 2000;
};

TEST_P(PgObjecLocksTestOutOfOrderMessageHandling, TestOutOfOrderMessageHandling) {
  if (!ShouldUseExplicitLocksInsteadOfDdl()) {
    // TODO(#33361): the UseDdlForLocks variants fail with concurrent DDL enabled. Concurrent DDL
    // can no longer be turned off independently of object locking, so skip these variants until
    // the test is fixed to work in that mode.
    GTEST_SKIP() << "Skipping UseDdlForLocks variant, see #33361";
  }
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));

  LOG(INFO) << "Creating test table";
  ASSERT_OK(conn.Execute("CREATE TABLE test_table (id INT PRIMARY KEY, value TEXT);"));

  const auto kStatementTimeoutSec = 8;
  ASSERT_OK(conn.ExecuteFormat("SET statement_timeout = '$0s';", kStatementTimeoutSec));

  ASSERT_OK(cluster_->SetFlag(ts2, "TEST_block_acquires_to_simulate_out_of_order", "true"));
  LogWaiter log_waiter(ts2, "Blocking acquire request to simulate out-of-order requests");

  if (!ShouldDoMasterFailover()) {
    // This will cause the master to timeout the Acquire Rpc to the tserver, and retry
    // the Acquire request to simulate out-of-order requests, along with the original blocked
    // request.
    ASSERT_OK(cluster_->SetFlagOnMasters(
      "refresh_waiter_timeout_ms", yb::Format("$0", (kStatementTimeoutSec * 1000 / 10))));
    ASSERT_OK(cluster_->SetFlagOnMasters(
        "master_ts_rpc_timeout_ms", yb::Format("$0", (kStatementTimeoutSec * 1000 / 10))));
  }

  ASSERT_OK(conn.Execute("BEGIN TRANSACTION;"));
  auto lock_request_start = CoarseMonoClock::Now();
  auto status_future = std::async(std::launch::async, [&]() -> Status {
    if (ShouldUseExplicitLocksInsteadOfDdl()) {
      LOG(INFO) << "Acquiring EXCLUSIVE lock on test_table using explicit locks";
      RETURN_NOT_OK(conn.Execute("LOCK TABLE test_table IN ACCESS EXCLUSIVE MODE;"));
      LOG(INFO) << "Releasing EXCLUSIVE lock on test_table";
    } else {
      LOG(INFO) << "Acquiring EXCLUSIVE lock on test_table using a DDL -- Alter Table";
      RETURN_NOT_OK(conn.Execute("ALTER TABLE test_table ADD COLUMN dummy TEXT;"));
    }
    return conn.Execute("COMMIT;");
  });

  const auto kTimeout = MonoDelta::FromSeconds(3 * kTimeMultiplier);
  ASSERT_OK(log_waiter.WaitFor(kTimeout));

  LOG(INFO) << "Have at least one Tserver blocking the acquire request";
  ASSERT_OK(cluster_->SetFlag(ts2, "TEST_block_acquires_to_simulate_out_of_order", "false"));
  LOG(INFO) << "Will no longer block acquires at TServers";

  if (ShouldDoMasterFailover()) {
    auto old_master = cluster_->GetLeaderMaster();
    LOG(INFO) << "Stepping down master leader";
    ASSERT_OK(cluster_->StepDownMasterLeaderAndWaitForNewLeader());
    LOG(INFO) << "Killing old master to reset the network connection";
    old_master->Shutdown();
  }

  LOG(INFO) << "Waiting for LOCK TABLE to succeed and release it";
  ASSERT_OK(status_future.get());

  LOG(INFO) << "Taking a SHARED lock to ensure that the asynchronous release is infact done.";
  ASSERT_OK(conn2.Execute("BEGIN TRANSACTION;"));
  ASSERT_OK(conn2.Execute("LOCK TABLE test_table IN ACCESS SHARE MODE;"));
  ASSERT_OK(conn2.Execute("COMMIT;"));
  LOG(INFO) << "Acquired and Released shared lock. So the exclusive lock release was processed by "
               "the TServer.";

  LogWaiter log_waiter2(ts2, "Unblocking acquire request.");
  LOG(INFO) << "Allowing blocked acquire request to now proceed.";
  ASSERT_OK(
      cluster_->SetFlag(ts2, "TEST_release_blocked_acquires_to_simulate_out_of_order", "true"));
  ASSERT_OK(log_waiter2.WaitFor(kTimeout));

  auto get_locks_count_on_tserver = [&ts2, this]() -> Result<int64_t> {
    return VERIFY_RESULT(cluster_->GetObjectLockStatus(*ts2)).object_lock_infos_size();
  };

  auto kGracePeriod = MonoDelta::FromMilliseconds(500);
  ASSERT_LE(CoarseMonoClock::Now() + kGracePeriod, lock_request_start + kStatementTimeoutSec * 1s);
  ASSERT_OK(LoggedWaitFor(
      [&]() -> Result<bool> {
        auto locks_count = VERIFY_RESULT(get_locks_count_on_tserver());
        LOG(INFO) << "Number of locks on test_table: " << locks_count;
        return locks_count > 0;
      },
      kGracePeriod, "Wait for unblocked acquire request to complete"));

  auto expected_cleanup_time = lock_request_start + MonoDelta::FromSeconds(kStatementTimeoutSec) +
                               kPgClientExtraTimeoutMs * 1ms;
  ASSERT_OK(LoggedWait(
      [&]() -> Result<bool> {
        auto locks_count = VERIFY_RESULT(get_locks_count_on_tserver());
        LOG(INFO) << "Number of locks on test_table: " << locks_count;
        return locks_count == 0;
      },
      expected_cleanup_time + kGracePeriod, "Wait for the out-of-order acquire to get cleaned up"));

  LOG(INFO) << "Re-Acquiring an exclusive lock on test table";
  ASSERT_OK(conn.Execute("BEGIN TRANSACTION;"));
  ASSERT_OK(conn.Execute("LOCK TABLE test_table IN ACCESS EXCLUSIVE MODE;"));
  ASSERT_OK(conn.Execute("COMMIT;"));
  LOG(INFO) << "Re-acquire succeeded. Done";
}

INSTANTIATE_TEST_SUITE_P(
    , PgObjecLocksTestOutOfOrderMessageHandling,
    ::testing::Values(
        std::make_tuple(
            DoMasterFailover::kTrue, UseExplicitLocksInsteadOfDdl::kTrue),
        std::make_tuple(
            DoMasterFailover::kFalse, UseExplicitLocksInsteadOfDdl::kTrue),
        std::make_tuple(
            DoMasterFailover::kTrue, UseExplicitLocksInsteadOfDdl::kFalse),
        std::make_tuple(
            DoMasterFailover::kFalse, UseExplicitLocksInsteadOfDdl::kFalse)),
    [](const ::testing::TestParamInfo<
        std::tuple<DoMasterFailover, UseExplicitLocksInsteadOfDdl>>& info) {
      return Format("$0_$1",
          (std::get<0>(info.param) ? "WithMasterFailover" : "NoMasterFailover"),
          (std::get<1>(info.param) ? "UseExplicitLocks" : "UseDdlForLocks"));
    });

// This test relies on the OLM poller to run frequently and timedout waiters, in order to
// check for deadlocks/transaction aborts and retry from pg_client_session if necessary.
TEST_F(PgObjectLocksTestRF1, YB_DISABLE_TEST_IN_TSAN(TestDeadlock)) {
  // Restart the cluster_ with the poll interval set to default.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_olm_poll_interval_ms) = kDefaultLockManagerPollIntervalMs;
  ASSERT_OK(cluster_->RestartSync());
  Init();

  auto conn1 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(0, 10), 0"));
  ASSERT_OK(conn1.Execute("CREATE TABLE test1(k INT)"));

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_refresh_waiter_timeout_ms) = 5000;
  auto conn2 = ASSERT_RESULT(Connect());

  // Deadlock at the tserver's object lock manager
  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));

  ASSERT_OK(conn1.Execute("LOCK TABLE test in ACCESS SHARE mode"));
  ASSERT_OK(conn2.Execute("LOCK TABLE test1 in ACCESS EXCLUSIVE mode"));

  auto status_future = std::async(std::launch::async, [&]() -> Status {
    auto s = conn1.Execute("LOCK TABLE test1 in ACCESS SHARE mode");
    EXPECT_OK(conn1.RollbackTransaction());
    return s;
  });

  auto s = conn2.Execute("LOCK TABLE test in ACCESS EXCLUSIVE mode");
  ASSERT_OK(conn2.RollbackTransaction());
  if (s.ok()) {
    s = status_future.get();
  } else {
    ASSERT_OK(status_future.get());
  }
  ASSERT_STR_CONTAINS(s.ToString(), "aborted due to a deadlock");

  // Deadlock between row-level locks and object locks.
  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));

  ASSERT_OK(conn1.Execute("UPDATE test SET v=v+1 where k=1"));
  ASSERT_OK(conn2.Execute("LOCK TABLE test1 in ACCESS EXCLUSIVE mode"));

  status_future = std::async(std::launch::async, [&]() -> Status {
    auto s = conn1.Execute("LOCK TABLE test1 in ACCESS SHARE mode");
    SleepFor(FLAGS_transaction_heartbeat_usec * 1us * kTimeMultiplier * 2);
    if (s.ok()) {
      s = conn1.Execute("UPDATE test SET v=v+1 where k=1");
    }
    EXPECT_OK(conn1.RollbackTransaction());
    return s;
  });
  s = conn2.Execute("UPDATE test SET v=v+1 where k=1");
  if (s.ok()) {
    // Give time for the transaction heartheat to realize the abort. That way, conn1's
    // lock request would fail. If not, it will falsely report success, but fail the
    // subsequent statements.
    SleepFor(FLAGS_transaction_heartbeat_usec * 1us * kTimeMultiplier * 2);
    s = conn2.Execute("UPDATE test SET v=v+1 where k=1");
  }
  ASSERT_OK(conn2.RollbackTransaction());
  if (s.ok()) {
    s = status_future.get();
  } else {
    ASSERT_OK(status_future.get());
  }
  ASSERT_STR_CONTAINS(s.ToString(), "aborted due to a deadlock");

  // Deadlock at the master's object lock manager
  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));

  ASSERT_OK(conn1.Execute("LOCK TABLE test in ACCESS EXCLUSIVE mode"));
  ASSERT_OK(conn2.Execute("LOCK TABLE test1 in ACCESS EXCLUSIVE mode"));
  status_future = std::async(std::launch::async, [&]() -> Status {
    auto s = conn1.Execute("LOCK TABLE test1 in ACCESS EXCLUSIVE mode");
    EXPECT_OK(conn1.RollbackTransaction());
    return s;
  });
  s = conn2.Execute("LOCK TABLE test in ACCESS EXCLUSIVE mode");
  ASSERT_OK(conn2.RollbackTransaction());
  if (s.ok()) {
    s = status_future.get();
  } else {
    ASSERT_OK(status_future.get());
  }
  ASSERT_STR_CONTAINS(s.ToString(), "aborted due to a deadlock");
}

TEST_F(PgObjectLocksTestRF1, TestShutdownWithWaiters) {
  constexpr auto kNumWaiters = 3;
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn.Execute("INSERT INTO test SELECT generate_series(0, 10), 0"));
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn.Execute("LOCK TABLE test in ACCESS EXCLUSIVE mode"));

  TestThreadHolder thread_holder;
  for (int i = 0 ; i < kNumWaiters; i++) {
    thread_holder.AddThreadFunctor([&]() {
      auto conn = ASSERT_RESULT(Connect());
      ASSERT_OK(conn.ExecuteFormat("SET statement_timeout=$0", 1000 * kTimeMultiplier));
      ASSERT_NOK(conn.Execute("UPDATE test SET v=v+1 WHERE k=1"));
    });
  }
  auto log_waiter = StringWaiterLogSink("has just lost its lease");
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_tserver_enable_ysql_lease_refresh) = false;
  ASSERT_OK(log_waiter.WaitFor(
      MonoDelta::FromMilliseconds(2 * kDefaultMasterYSQLLeaseTTLMilli * kTimeMultiplier)));
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_tserver_enable_ysql_lease_refresh) = true;
  thread_holder.JoinAll();
}

TEST_F(PgObjectLocksTestRF1, YB_DISABLE_TEST_IN_TSAN(TestDeadlockFastPath)) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn.Execute("INSERT INTO test SELECT generate_series(0, 10), 0"));

  google::SetVLOGLevel("conflict_resolution*", 3);
  {
    RegexWaiterLogSink log_waiter(R"#(.*ResolveTransactionConflicts.*object_locking_txn_meta.*)#");
    ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
    ASSERT_OK(conn.Execute("UPDATE test SET v=v+1 WHERE k=1"));
    ASSERT_FALSE(log_waiter.IsEventOccurred());
  }

  SyncPoint::GetInstance()->LoadDependency(
      {{"WaitQueue::Impl::SetupWaiterUnlocked:1", "TestDeadlockFastPath"}});
  SyncPoint::GetInstance()->ClearTrace();
  SyncPoint::GetInstance()->EnableProcessing();

  RegexWaiterLogSink log_waiter(R"#(.*ResolveOperationConflicts.*object_locking_txn_meta.*)#");
  auto status_future = std::async(std::launch::async, [&]() -> Status {
    auto conn = VERIFY_RESULT(Connect());
    return conn.Execute("UPDATE test SET v=v+1 WHERE k=1");
  });
  TEST_SYNC_POINT("TestDeadlockFastPath");
  ASSERT_TRUE(log_waiter.IsEventOccurred());
  auto s = conn.Execute("ALTER TABLE test ADD COLUMN v1 INT");
  if (s.ok()) {
    s = conn.Execute("UPDATE test SET v=v+1 WHERE k=1");
  }
  if (s.ok()) {
    ASSERT_NOK(status_future.get());
    ASSERT_OK(conn.CommitTransaction());
  } else {
    ASSERT_OK(status_future.get());
    ASSERT_OK(conn.RollbackTransaction());
  }
}

TEST_F(PgObjectLocksTestRF1, TestDisableReuseAbortedPlainTxn) {
  constexpr auto kStatementTimeoutMs = 1000 * kTimeMultiplier;
  // Restart the cluster_ with the poll interval set to 5x of statment timeout
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_olm_poll_interval_ms) = 5 * kStatementTimeoutMs;
  google::SetVLOGLevel("pg_client_session*", 1);
  ASSERT_OK(cluster_->RestartSync());
  Init();

  auto conn1 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(1, 10), 0"));
  ASSERT_OK(conn1.Execute("CREATE TABLE test1(k INT)"));

  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn1.Execute("LOCK TABLE test in ACCESS EXCLUSIVE mode"));

  auto conn2 = ASSERT_RESULT(Connect());
  // TODO(#26792): Change to LOCK_TIMEOUT once GH is addressed.
  ASSERT_OK(conn2.ExecuteFormat("SET statement_timeout=$0", kStatementTimeoutMs));
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  auto log_waiter = StringWaiterLogSink("Consuming re-usable kPlain txn");
  ASSERT_OK(conn2.Execute("LOCK TABLE test1 in ACCESS SHARE mode"));
  // Would timeout before the waiting lock at the OLM is released.
  ASSERT_NOK(conn2.Execute("LOCK TABLE test in ACCESS SHARE mode"));
  ASSERT_OK(conn2.RollbackTransaction());
  ASSERT_OK(log_waiter.WaitFor(MonoDelta::FromSeconds(2 * kTimeMultiplier)));
  ASSERT_OK(conn1.Execute("COMMIT"));
  ASSERT_EQ(ASSERT_RESULT(conn2.FetchRow<PGUint64>("SELECT COUNT(*) FROM test")), 10);
  ASSERT_OK(AssertNumLocks(0, 0));
}

#ifndef NDEBUG
TEST_F(PgObjectLocksTestRF1, TestDisableReuseOfFailedTxn) {
  google::SetVLOGLevel("pg_client_session*", 1);
  google::SetVLOGLevel("transaction*", 1);
  auto conn1 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(1, 10), 0"));

  TransactionId txn_id = TransactionId::Nil();
  yb::SyncPoint::GetInstance()->SetCallBack(
      "PgClientSession::Impl::DoAcquireObjectLock",
      [&](void* arg) { txn_id = *(static_cast<TransactionId*>(arg)); });
  SyncPoint::GetInstance()->ClearTrace();
  SyncPoint::GetInstance()->EnableProcessing();

  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn1.Execute("LOCK TABLE test in ACCESS SHARE mode"));
  ASSERT_TRUE(!txn_id.Nil());
  SyncPoint::GetInstance()->DisableProcessing();

  auto conn2 = ASSERT_RESULT(Connect());
  auto log_waiter1 = RegexWaiterLogSink(Format(".*$0.*Heartbeat failed.*", txn_id));
  auto log_waiter2 = StringWaiterLogSink("Clearing lock owner registration for remote aborted txn");
  ASSERT_TRUE(ASSERT_RESULT(
      conn2.FetchRow<bool>(Format("SELECT yb_cancel_transaction('$0')", txn_id))));
  ASSERT_OK(log_waiter1.WaitFor(MonoDelta::FromSeconds(5 * kTimeMultiplier)));
  ASSERT_OK(conn1.Execute("COMMIT"));

  ASSERT_OK(log_waiter2.WaitFor(MonoDelta::FromSeconds(2 * kTimeMultiplier)));
  ASSERT_EQ(ASSERT_RESULT(conn1.FetchRow<PGUint64>("SELECT COUNT(*) FROM test")), 10);
  ASSERT_OK(AssertNumLocks(0, 0));
}

TEST_F(PgObjectLocksTestRF1, TestDisableReuseOfFailedTxnImplicit) {
  google::SetVLOGLevel("pg_client_session*", 1);
  google::SetVLOGLevel("transaction*", 1);
  {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
    ASSERT_OK(conn.Execute("INSERT INTO test SELECT generate_series(1, 10), 0"));
  }

  TransactionId txn_id = TransactionId::Nil();
  yb::SyncPoint::GetInstance()->SetCallBack(
      "TransactionProvider::NextTxnMetaForPlain",
      [&](void* arg) { txn_id = *(static_cast<TransactionId*>(arg)); });
  SyncPoint::GetInstance()->ClearTrace();
  SyncPoint::GetInstance()->EnableProcessing();

  auto conn1 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn1.Fetch("SELECT * FROM test"));
  ASSERT_TRUE(!txn_id.Nil());
  SyncPoint::GetInstance()->DisableProcessing();

  auto conn2 = ASSERT_RESULT(Connect());
  auto log_waiter1 = RegexWaiterLogSink(Format(".*$0.*Heartbeat failed.*", txn_id));
  auto log_waiter2 = StringWaiterLogSink("Clearing lock owner registration for remote aborted txn");
  ASSERT_TRUE(ASSERT_RESULT(
      conn2.FetchRow<bool>(Format("SELECT yb_cancel_transaction('$0')", txn_id))));
  ASSERT_OK(log_waiter1.WaitFor(MonoDelta::FromSeconds(5 * kTimeMultiplier)));
  ASSERT_OK(log_waiter2.WaitFor(MonoDelta::FromSeconds(2 * kTimeMultiplier)));

  TransactionId old_txn_id = txn_id;
  SyncPoint::GetInstance()->ClearTrace();
  SyncPoint::GetInstance()->EnableProcessing();

  auto log_waiter3 = StringWaiterLogSink("Consuming re-usable kPlain txn");
  // TODO(#33274): This query will fail, because we try to use the aborted transaction, and do not
  // transparently retry.
  (void) conn1.FetchRow<PGUint64>("SELECT COUNT(*) FROM test");
  ASSERT_OK(log_waiter3.WaitFor(-10s));
  ASSERT_EQ(ASSERT_RESULT(conn1.FetchRow<PGUint64>("SELECT COUNT(*) FROM test")), 10);
  ASSERT_NE(old_txn_id, txn_id);
  ASSERT_OK(AssertNumLocks(0, 0));
}
#endif

TEST_F(PgObjectLocksTestRF1, TestDisableReuseOfBlockerTxn) {
  google::SetVLOGLevel("object_lock_manager*", 1);
  google::SetVLOGLevel("pg_client_session*", 1);
  auto conn1 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test SELECT generate_series(1, 10), 0"));

  ASSERT_OK(conn1.Execute("BEGIN"));
  ASSERT_OK(conn1.Fetch("SELECT * FROM test"));

  auto conn2 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn2.Execute("BEGIN"));
  auto log_waiter1 = StringWaiterLogSink("added to wait-queue on");
  auto status_future1 = std::async(std::launch::async, [&] -> Status {
    return conn2.Execute("ALTER TABLE test ADD COLUMN v1 INT DEFAULT 0");
  });
  ASSERT_OK(log_waiter1.WaitFor(MonoDelta::FromSeconds(10 * kTimeMultiplier)));

  auto log_waiter2 = StringWaiterLogSink("Consuming re-usable kPlain txn");
  ASSERT_OK(conn1.Execute("COMMIT"));
  ASSERT_OK(log_waiter2.WaitFor(MonoDelta::FromSeconds(10 * kTimeMultiplier)));
  ASSERT_OK(status_future1.get());

  auto log_waiter3 = StringWaiterLogSink("added to wait-queue on");
  auto status_future2 = std::async(std::launch::async, [&] -> Status {
    return ResultToStatus(conn1.Fetch("SELECT * FROM test"));
  });
  ASSERT_OK(log_waiter3.WaitFor(MonoDelta::FromSeconds(10 * kTimeMultiplier)));
  SleepFor(2s * kTimeMultiplier);

  ASSERT_OK(conn2.Execute("COMMIT"));
  ASSERT_OK(status_future2.get());
  ASSERT_OK(AssertNumLocks(0, 0));
}

TEST_F(PgObjectLocksTestRF1, TestRedundantLockIsServedLocallyInYsql) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_olm_serve_redundant_lock) = true;
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE pk(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn.Execute("INSERT INTO pk SELECT generate_series(1, 1000), 0"));

  // Assert redundant table_open on pk is skipped.
  ASSERT_OK(conn.Execute("BEGIN"));
  ASSERT_OK(conn.Execute("INSERT INTO pk VALUES(2000, 0)"));
  auto num_initial_locks = NumGrantedLocks();
  ASSERT_OK(conn.Execute("INSERT INTO pk VALUES(3000, 0)"));
  ASSERT_EQ(num_initial_locks, NumGrantedLocks());
  ASSERT_OK(conn.Execute("COMMIT"));
}

class PgObjectLocksBothPathsTest : public PgObjectLocksTestRF1,
                                   public ::testing::WithParamInterface<bool> {
 protected:
  void BeforePgProcessStart() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_object_lock_fastpath) = GetParam();
  }

  Status WaitForLockManagerStatusTablet(
      TransactionId txn_id, const TabletId& expected_tablet) {
    return WaitFor([&] {
      auto olm_status_tablet = ts_lock_manager_->TEST_GetStatusTabletForTxn(txn_id);
      if (olm_status_tablet == expected_tablet) {
        return true;
      }
      LOG(INFO) << "Transaction status tablet changed to " << expected_tablet << " but "
                << "ObjectLockManager has " << olm_status_tablet;
      return false;
    }, 5s * kTimeMultiplier, "Waiting for status tablet to change");
  }
};

TEST_P(PgObjectLocksBothPathsTest, TestLocalLockTransactionPromotion) {
  auto conn = ASSERT_RESULT(Connect());

  // Tablespace-based placement allows for testing promotion in a RF1 setup.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_use_tablespace_based_transaction_placement) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_consider_all_local_transaction_tables_local) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_track_last_transaction) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_tablespace_info_refresh_secs) = 1;

  auto log_waiter = RegexWaiterLogSink(Format(
      ".*Received status tablets \\(version bump from .+ to $0\\).*",
      2 * (1 + FLAGS_transaction_table_num_tablets)));
  ASSERT_OK(conn.Execute(
      R"#(CREATE TABLESPACE ts WITH (replica_placement = '{
        "num_replicas": 1,
        "placement_blocks": [{
          "cloud": "cloud1",
          "region": "region1",
          "zone": "zone",
          "min_num_replicas": 1
        }]
      }'))#"));
  ASSERT_OK(conn.Execute("CREATE TABLE test_local1(x int) TABLESPACE ts"));
  ASSERT_OK(conn.Execute("CREATE TABLE test_local2(x int) TABLESPACE ts"));
  ASSERT_OK(conn.Execute("CREATE TABLE test_global(x int)"));
  ASSERT_OK(log_waiter.WaitFor(5s * kTimeMultiplier));

  ASSERT_OK(conn.StartTransaction(IsolationLevel::SERIALIZABLE_ISOLATION));
  ASSERT_OK(conn.Execute("INSERT INTO test_local1 VALUES(0)"));
  ASSERT_OK(conn.Execute("INSERT INTO test_local2 VALUES(0)"));
  ASSERT_OK(conn.RollbackTransaction());

  ASSERT_OK(conn.StartTransaction(IsolationLevel::SERIALIZABLE_ISOLATION));
  ASSERT_OK(conn.Execute("INSERT INTO test_local1 VALUES(0)"));

  auto txn =
      cluster_->mini_tablet_server(0)->server()->TransactionPool().TEST_GetLastTransaction();
  auto initial_metadata = ASSERT_RESULT(txn->metadata());
  auto txn_id = initial_metadata.transaction_id;
  auto old_status_tablet = initial_metadata.status_tablet;
  ASSERT_OK(conn.Execute("INSERT INTO test_global VALUES(0)"));

  auto final_metadata = ASSERT_RESULT(txn->metadata());
  auto new_status_tablet = final_metadata.status_tablet;
  ASSERT_NE(old_status_tablet, new_status_tablet);

  if (!FLAGS_enable_object_lock_fastpath) {
    ASSERT_OK(WaitForLockManagerStatusTablet(txn_id, new_status_tablet));
  }

  ASSERT_OK(conn.Execute("INSERT INTO test_local2 VALUES(0)"));
  if (FLAGS_enable_object_lock_fastpath) {
    ts_lock_manager_->TEST_ConsumePendingSharedLockRequests();
  }
  ASSERT_OK(WaitForLockManagerStatusTablet(txn_id, new_status_tablet));
  ASSERT_OK(conn.CommitTransaction());
}

INSTANTIATE_TEST_CASE_P(
    Fastpath, PgObjectLocksBothPathsTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
      return info.param ? "Fastpath" : "RPC";
    });

class PgObjectLocksFastpathTest : public PgObjectLocksTestRF1 {
 protected:
  auto TServerSharedObject() {
    return cluster_->mini_tablet_server(0)->server()->shared_object();
  }

  docdb::ObjectLockSharedStateManager& ObjectLockSharedStateManager() {
    return *cluster_->mini_tablet_server(0)->server()->ObjectLockSharedStateManager();
  }

  void BeforePgProcessStart() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_object_lock_fastpath) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_pg_client_use_shared_memory) = true;
  }

  void SetUp() override {
    PgObjectLocksTestRF1::SetUp();
    auto& shared_object = *TServerSharedObject().get();
    ASSERT_OK(shared_object.WaitAllocatorsInitialized());

    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.Execute("CREATE TABLE test(k INT)"));
  }

  template<typename Metric>
  auto ExpectMetricIncrease(const auto& prototype, auto delta) {
    auto& metric_entity = cluster_->mini_tablet_server(0)->metric_entity();
    auto metric = metric_entity.FindOrNull<Metric>(prototype);
    auto initial = metric->value();
    return ScopeExit([initial, delta, metric = std::move(metric)] {
      auto value = metric->value();
      EXPECT_EQ(value - initial, delta)
          << "Expected delta of " << delta << ", actual: " << (value - initial)
          << ", change from " << initial << " to " << value;
    });
  }
};

TEST_F(PgObjectLocksFastpathTest, TestReadOnlyTransaction) {
  auto conn = ASSERT_RESULT(Connect());

  // Get catalog reads out of the way since they acquire a bunch of unrelated locks.
  ASSERT_OK(conn.Fetch("SELECT * FROM test"));

  // Test postgres "read-only" transactions (including single-shard writes) acquire and release
  // entirely on postgres side.

  // Read statement.
  {
    auto g1 = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_acquires, /*delta=*/1);
    auto g2 = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_releases, /*delta=*/1);
    ASSERT_OK(conn.Fetch("SELECT * FROM test"));
  }

  // Single-shard write.
  {
    auto g1 = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_acquires, /*delta=*/2);
    auto g2 = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_releases, /*delta=*/1);
    ASSERT_OK(conn.Execute("INSERT INTO test VALUES(1)"));
  }

  // Transaction that only reads.
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  {
    auto g = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_acquires, /*delta=*/1);
    ASSERT_OK(conn.Fetch("SELECT * FROM test"));
  }
  {
    auto g = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_releases, /*delta=*/1);
    ASSERT_OK(conn.CommitTransaction());
  }
}

TEST_F(PgObjectLocksFastpathTest, TestTServerSharedMemAcquire) {
  auto conn = ASSERT_RESULT(Connect());

  // Consume the plain transaction.
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn.Execute("INSERT INTO test VALUES(1)"));
  ASSERT_OK(conn.CommitTransaction());

  // This needs to go to TServer for a new transaction, but otherwise can use shared memory. It
  // should not prevent postgres-side release.
  {
    auto g1 = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_tserver_acquires, /*delta=*/2);
    auto g2 = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_releases, /*delta=*/1);
    ASSERT_OK(conn.Execute("INSERT INTO test VALUES(1)"));
  }
}

TEST_F(PgObjectLocksFastpathTest, TestTServerSharedMemRelease) {
  auto conn = ASSERT_RESULT(Connect());

  // Get catalog reads out of the way since they acquire a bunch of unrelated locks.
  ASSERT_OK(conn.Fetch("SELECT * FROM test"));

  // Test case where we release as part of FinishTransaction, but do not need to involve
  // ObjectLockManager since all locks are in shared memory.
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  {
    auto g = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_acquires, /*delta=*/2);
    ASSERT_OK(conn.Execute("INSERT INTO test VALUES(1)"));
  }
  {
    auto g = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_tserver_releases, /*delta=*/1);
    ASSERT_OK(conn.CommitTransaction());
  }
}

TEST_F(PgObjectLocksFastpathTest, TestTServerLockManagerRelease) {
  auto conn1 = ASSERT_RESULT(Connect());

  // Get catalog reads out of the way since they acquire a bunch of unrelated locks.
  ASSERT_OK(conn1.Fetch("SELECT * FROM test"));

  // Block conn1 from taking anything greater than ACCESS SHARE.
  auto conn2 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn2.Execute("LOCK TABLE test IN EXCLUSIVE MODE"));

  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  // ACCESS SHARE can use fastpath.
  {
    auto g = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_acquires, /*delta=*/1);
    ASSERT_OK(conn1.Execute("LOCK TABLE test IN ACCESS SHARE MODE"));
  }

  // ROW SHARE should go to ObjectLockManager (and block until conn2 commits).
  {
    std::jthread t([&] {
      std::this_thread::sleep_for(2s);
      ASSERT_OK(conn2.CommitTransaction());
    });
    auto g = ExpectMetricIncrease<Counter>(METRIC_object_locking_lock_acquires, /*delta=*/1);
    ASSERT_OK(conn1.Execute("LOCK TABLE test IN ROW SHARE MODE"));
  }

  // ROW EXCLUSIVE can use fastpath since conn2 has committed.
  {
    auto g = ExpectMetricIncrease<FunctionGauge<uint64_t>>(
        METRIC_object_locking_fastpath_pg_acquires, /*delta=*/2);
    ASSERT_OK(conn1.Execute("LOCK TABLE test IN ROW EXCLUSIVE MODE"));
  }

  // We must release via ObjectLockManager so that the transaction entry is cleaned up properly.
  {
    auto g = ExpectMetricIncrease<Counter>(METRIC_object_locking_lock_releases, /*delta=*/1);
    ASSERT_OK(conn1.CommitTransaction());
  }
}

class PgObjectLocksWithConcurrentDdl : public PgObjectLocksTest {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* opts) override {
    PgObjectLocksTest::UpdateMiniClusterOptions(opts);
    opts->extra_tserver_flags.emplace_back("--ysql_enable_concurrent_ddl=true");
    opts->extra_tserver_flags.emplace_back(
        "--wait_for_ysql_backends_catalog_version_client_master_rpc_timeout_ms=8000");
    opts->extra_tserver_flags.emplace_back(
        "--wait_for_ysql_backends_catalog_version_client_master_rpc_margin_ms=2000");
    opts->extra_tserver_flags.emplace_back(
        "--tablet_validator_retain_delete_markers_validation_period_sec=5");
  }
};

TEST_F_EX(
    PgObjectLocksTest, TestConcurrentDdlSyncReleaseForGlobalLocksAndDdls,
    PgObjectLocksWithConcurrentDdl) {
  testSyncReleaseForGlobalLocksAndDdls();
}

// In YB, CREATE INDEX acquires session locks against the session level txn's by bumping up the
// active subtxn id, and the releases all those locks at the end by rolling back the subtxn.
// The below test asserts that consectuive CREATE INDEX from the same session don't deadlock
// when there's another waiting txn on the create index. Also, it asserts that the session
// advisory locks taken are still honored (as they are tagged to the same session level txn).
TEST_F_EX(
    PgObjectLocksTest, YB_DISABLE_TEST_IN_SANITIZERS(ConsecutiveCreateIndexDontDeadlock),
    PgObjectLocksWithConcurrentDdl) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);
  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));

  ASSERT_OK(conn1.Execute("CREATE TABLE test(k int primary key, v int, v1 int)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test select i,i,i from generate_series(1, 100) as i"));

  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_pause_session_lock_before_release", "true"));
  ASSERT_OK(cluster_->SetFlagOnMasters("vmodule", "object_lock_manager=1"));
  ASSERT_OK(conn1.Fetch("select pg_advisory_lock(1)"));

  LogWaiter log_waiter1(ts1, "Pausing due to flag TEST_pause_session_lock_before_release");
  auto status_future1 = std::async(std::launch::async, [&]() -> Status {
    return conn1.Execute("CREATE INDEX test_idx on test(v)");
  });
  ASSERT_OK(log_waiter1.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 30)));

  LogWaiter log_waiter2(cluster_->GetLeaderMaster(), "added to wait-queue");
  auto status_future2 = std::async(std::launch::async, [&]() -> Status {
    RETURN_NOT_OK(conn2.Execute("begin transaction isolation level repeatable read"));
    return conn2.Execute("LOCK TABLE test in ACCESS EXCLUSIVE mode");
  });
  ASSERT_OK(log_waiter2.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));
  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_pause_session_lock_before_release", "false"));
  ASSERT_OK(status_future1.get());
  ASSERT_OK(conn1.Fetch("select pg_advisory_lock(2)"));

  LogWaiter log_waiter3(cluster_->GetLeaderMaster(), "added to wait-queue");
  auto status_future3 = std::async(std::launch::async, [&]() -> Status {
    return conn1.Execute("CREATE INDEX test_idx2 on test(v1)");
  });
  ASSERT_OK(log_waiter3.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));

  ASSERT_OK(status_future2.get());
  ASSERT_OK(conn2.Execute("ALTER TABLE test add column v2 int"));
  ASSERT_OK(conn2.CommitTransaction());
  ASSERT_OK(status_future3.get());
  ASSERT_OK(conn1.Fetch("select pg_advisory_lock(2)"));
  auto num_locks = ASSERT_RESULT(
      conn1.FetchRow<PGUint64>("select count(*) from pg_locks where locktype=\'advisory\'"));
  ASSERT_EQ(num_locks, 3);
}

// Assert that REFRESH MATERIALIZED VIEW when run in concurrent to CREATE INDEX CONCURRENTLY
// on the same mv relation gets blocked and doesn't progress until the create index commits
// the internal transaction in phase 3. Regression test to catch a bug where refresh mv
// wrongly resumed after the SHARE session lock on the mv relation was released in create
// index phase 3 but before its commit.
TEST_F_EX(
    PgObjectLocksTest, TxnLocksGoToTServerWhenSessionLockIsSoleOwner,
    PgObjectLocksWithConcurrentDdl) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);
  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  auto conn2 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts2));
  auto pg_locks_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));

  ASSERT_OK(conn1.Execute("create table test(k int primary key, v1 int, v2 int)"));
  ASSERT_OK(conn1.Execute("insert into test select i,i,i from generate_series(1, 10) as i"));
  ASSERT_OK(conn1.Execute("create materialized view test_mv as select k, v1 from test"));

  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_pause_session_lock_before_release", "true"));

  LogWaiter log_waiter(ts1, "Pausing due to flag TEST_pause_session_lock_before_release");
  auto status_future1 = std::async(std::launch::async, [&]() -> Status {
    return conn1.Execute("create unique index test_mv_idx on test_mv(v1)");
  });
  ASSERT_OK(log_waiter.WaitFor(20s * kTimeMultiplier));

  auto status_future2 = std::async(std::launch::async, [&]() -> Status {
    return conn2.Execute("refresh materialized view test_mv");
  });

  const auto waiting_locks_query = "select count(*) from pg_locks where not granted";
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    const auto num_waiting_locks =
        VERIFY_RESULT(pg_locks_conn.FetchRow<PGUint64>(waiting_locks_query));
    return num_waiting_locks >= 1;
  }, 10s * kTimeMultiplier, "REFRESH MV should have been blocked, but wasn't???"));

  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_pause_session_lock_after_release", "true"));
  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_pause_session_lock_before_release", "false"));

  const auto granted_locks_query =
      "select count(*) from pg_locks where granted and mode = \'ShareUpdateExclusiveLock\'";
  EXPECT_OK(WaitFor([&]() -> Result<bool> {
    return VERIFY_RESULT(pg_locks_conn.FetchRow<PGUint64>(granted_locks_query)) == 1;
  }, 5s * kTimeMultiplier, "Timed out waiting for session locks to be released"));
  EXPECT_OK(WaitFor([&]() -> Result<bool> {
    return VERIFY_RESULT(pg_locks_conn.FetchRow<PGUint64>(waiting_locks_query)) == 1;
  }, 5s * kTimeMultiplier, "REFRESH MV should still stay blocked, but wasn't???"));
  ASSERT_OK(cluster_->SetFlagOnTServers("TEST_pause_session_lock_after_release", "false"));
  ASSERT_OK(status_future1.get());
  ASSERT_OK(status_future2.get());
}

TEST_F_EX(
    PgObjectLocksTest, CreateIndexDoesntDeadlockWithTruncate, PgObjectLocksWithConcurrentDdl) {
  const auto ts1_idx = 1;
  const auto ts2_idx = 2;
  auto* ts1 = cluster_->tablet_server(ts1_idx);
  auto* ts2 = cluster_->tablet_server(ts2_idx);

  auto conn1 = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(conn1.Execute("CREATE TABLE test(k INT PRIMARY KEY, v INT)"));
  ASSERT_OK(conn1.Execute("INSERT INTO test VALUES (0, 0)"));

  // Pause CREATE INDEX at the first WaitForLockers (start of phase 2).
  ASSERT_OK(cluster_->SetFlag(
      ts2, "TEST_pause_wait_for_lockers", "true"));

  LogWaiter log_waiter1(
      ts2, "Pausing due to flag TEST_pause_wait_for_lockers");
  auto create_index_future = std::async(std::launch::async, [&]() {
    auto conn = VERIFY_RESULT(LibPqTestBase::ConnectToTs(*ts2));
    return conn.Execute("CREATE INDEX idx ON test(v)");
  });
  ASSERT_OK(log_waiter1.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));

  // Refresh conn1's backend catalog version to V1.
  ASSERT_OK(conn1.Fetch("SELECT * FROM test"));

  // TRUNCATE blocks on the ACCESS EXCLUSIVE lock which conflicts with CREATE INDEX's lock.
  // The backend's catalog_version is V1 at this point.
  auto* leader_master = cluster_->GetLeaderMaster();
  ASSERT_OK(cluster_->SetFlag(
    leader_master, "vmodule", "object_lock_manager*=1"));
  LogWaiter log_waiter2(
      leader_master, "added to wait-queue on");
  auto truncate_future = std::async(std::launch::async, [&]() -> Status {
    return conn1.Execute("TRUNCATE test");
  });
  ASSERT_OK(log_waiter2.WaitFor(MonoDelta::FromSeconds(kTimeMultiplier * 10)));

  ASSERT_OK(cluster_->SetFlag(
      ts2, "TEST_pause_wait_for_lockers", "false"));

  ASSERT_OK(create_index_future.get());
  ASSERT_OK(truncate_future.get());
}

TEST_F_EX(
    PgObjectLocksTest, WaitForLockersParticipatesInDeadlockDetection,
    PgObjectLocksWithConcurrentDdl) {
  auto* ts1 = cluster_->tablet_server(1);
  auto* ts2 = cluster_->tablet_server(2);
  ASSERT_OK(cluster_->SetFlagOnTServers("refresh_waiter_timeout_ms", "5000"));

  auto setup_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(setup_conn.Execute("CREATE TABLE parent(k INT PRIMARY KEY)"));
  ASSERT_OK(setup_conn.Execute(
      "CREATE TABLE child(k INT PRIMARY KEY, parent_k INT REFERENCES parent(k))"));
  ASSERT_OK(setup_conn.Execute("INSERT INTO parent VALUES (1)"));
  const auto child_oid =
      ASSERT_RESULT(setup_conn.FetchRow<PGOid>("SELECT 'child'::regclass::oid"));

  ASSERT_OK(cluster_->SetFlag(ts2, "TEST_pause_wait_for_lockers", "true"));
  LogWaiter wait_for_lockers_waiter(
      ts2, "Pausing due to flag TEST_pause_wait_for_lockers");
  auto create_index_future = std::async(std::launch::async, [&]() -> Status {
    auto conn = VERIFY_RESULT(LibPqTestBase::ConnectToTs(*ts2));
    RETURN_NOT_OK(conn.Execute("SET statement_timeout = '60s'"));
    return conn.Execute("CREATE INDEX child_parent_k_idx ON child(parent_k)");
  });
  ASSERT_OK(wait_for_lockers_waiter.WaitFor(10s * kTimeMultiplier));

  auto insert_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(insert_conn.Execute("SET statement_timeout = '30s'"));
  ASSERT_OK(insert_conn.Execute("SET yb_max_query_layer_retries = 0"));
  ASSERT_OK(insert_conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(insert_conn.Fetch("SELECT * FROM child"));

  auto drop_future = std::async(std::launch::async, [&]() -> Status {
    auto conn = VERIFY_RESULT(LibPqTestBase::ConnectToTs(*ts1));
    RETURN_NOT_OK(conn.Execute("SET statement_timeout = '30s'"));
    RETURN_NOT_OK(conn.Execute("SET yb_max_query_layer_retries = 0"));
    return conn.Execute("DROP TABLE parent CASCADE");
  });

  auto observer_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return VERIFY_RESULT(observer_conn.FetchRow<PGUint64>(
        Format("SELECT count(*) FROM pg_locks "
               "WHERE relation = $0 AND NOT granted "
               "AND mode = 'AccessExclusiveLock'", child_oid))) > 0;
  }, 10s * kTimeMultiplier, "DROP should wait for CREATE INDEX's child-table lock"));

  auto insert_future = std::async(std::launch::async, [&]() -> Status {
    auto status = insert_conn.Execute("INSERT INTO child VALUES (1, 1)");
    if (!status.ok()) {
      WARN_NOT_OK(insert_conn.RollbackTransaction(), "Failed to roll back INSERT transaction");
      return status;
    }
    return insert_conn.CommitTransaction();
  });
  SleepFor(1s * kTimeMultiplier);
  ASSERT_EQ(insert_future.wait_for(0s), std::future_status::timeout);

  ASSERT_OK(cluster_->SetFlag(ts2, "TEST_pause_wait_for_lockers", "false"));
  ASSERT_OK(WaitFor([&]() {
    return create_index_future.wait_for(0s) == std::future_status::ready;
  }, 40s * kTimeMultiplier, "CREATE INDEX should complete after deadlock detection"));
  ASSERT_OK(create_index_future.get());
  ASSERT_OK(WaitFor([&]() {
    return drop_future.wait_for(0s) == std::future_status::ready &&
           insert_future.wait_for(0s) == std::future_status::ready;
  }, 20s * kTimeMultiplier, "DROP and INSERT should complete after deadlock detection"));
  auto drop_status = drop_future.get();
  auto insert_status = insert_future.get();
  ASSERT_TRUE(drop_status.ok() ^ insert_status.ok())
      << "drop_status: " << drop_status << ", insert_status: " << insert_status;
  if (!drop_status.ok()) {
    LOG(INFO) << "DROP failed with: " << drop_status;
  }
  if (!insert_status.ok()) {
    LOG(INFO) << "INSERT failed with: " << insert_status;
  }
}

YB_STRONGLY_TYPED_BOOL(Colocated);

// Parameterized over colocated vs non-colocated so we cover both the separate index-read path
// and the embedded index_request path used for colocated secondary indexes.
class PgObjectLocksWaitForBackendsIndexValid
    : public PgObjectLocksWithConcurrentDdl,
      public ::testing::WithParamInterface<Colocated> {
 protected:
  bool IsColocated() const {
    return GetParam();
  }

  const char* TestDatabase() const {
    return IsColocated() ? "colocated_db" : "yugabyte";
  }

  Result<PGConn> ConnectToTestDb(const ExternalTabletServer& ts) {
    return LibPqTestBase::ConnectToTsForDB(ts, TestDatabase());
  }

  void TestWaitForBackendsBeforeMarkingIndexValid() {
    const auto ts1_idx = 1;
    const auto ts2_idx = 2;
    auto* ts1 = cluster_->tablet_server(ts1_idx);
    auto* ts2 = cluster_->tablet_server(ts2_idx);

    if (IsColocated()) {
      auto setup_conn = ASSERT_RESULT(LibPqTestBase::ConnectToTs(*ts1));
      ASSERT_OK(setup_conn.ExecuteFormat(
          "CREATE DATABASE $0 WITH colocation=true", TestDatabase()));
    }

    auto conn1 = ASSERT_RESULT(ConnectToTestDb(*ts1));
    ASSERT_OK(conn1.ExecuteFormat(
        "CREATE TABLE test(k INT PRIMARY KEY, v INT)$0",
        IsColocated() ? " WITH (colocated=true)" : ""));
    ASSERT_OK(conn1.Execute("INSERT INTO test SELECT i,i from generate_series(1, 10) as i"));

    ASSERT_OK(cluster_->SetFlagOnMasters("TEST_pause_compute_safe_time_for_backfill_read", "true"));
    ASSERT_OK(cluster_->SetFlagOnTServers("TEST_skip_metadata_backfill_done", "true"));
    ASSERT_OK(cluster_->SetFlagOnTServers(
        "TEST_tablet_validator_disable_metadata_backfill_done", "true"));

    LogWaiter log_waiter_pause(
        cluster_->GetLeaderMaster(),
        "Pausing due to flag TEST_pause_compute_safe_time_for_backfill_read");
    auto create_index_future = std::async(std::launch::async, [&]() {
      auto conn = VERIFY_RESULT(ConnectToTestDb(*ts2));
      return conn.Execute("CREATE INDEX idx ON test(v)");
    });
    ASSERT_OK(log_waiter_pause.WaitFor(30s * kTimeMultiplier));

    // Start a repeatable read txn to force read time < backfill read safe time
    auto conn_rr = ASSERT_RESULT(ConnectToTestDb(*ts1));
    ASSERT_OK(conn_rr.Execute("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
    ASSERT_OK(conn_rr.FetchMatrix("SELECT * FROM test WHERE v = 1", 1 /* rows */, 2 /* columns */));
    const auto read_time_us = ASSERT_RESULT(conn_rr.FetchRow<PGUint64>(
        "SELECT ((EXTRACT (EPOCH FROM CURRENT_TIMESTAMP))*1000000)::bigint"));

    // Sleep to avoid read restart error on the next select for conn_rr
    SleepFor(1s * kTimeMultiplier);
    // In a different session, delete the key. The backfill won't see this delete if it picks
    // a safe time after this point but the repeatable read txn still has an older snapshot.
    ASSERT_OK(conn1.Execute("DELETE FROM test WHERE k = 1"));

    ASSERT_OK(cluster_->SetFlag(ts1, "vmodule", "tablet_service=4"));
    LogWaiter log_waiter_backends(
        ts1, "Pausing due to flag TEST_pause_wait_for_ysql_backends_catalog_version");
    ASSERT_OK(cluster_->SetFlag(ts1, "TEST_pause_wait_for_ysql_backends_catalog_version", "true"));
    ASSERT_OK(cluster_->SetFlag(
      cluster_->GetLeaderMaster(), "TEST_pause_compute_safe_time_for_backfill_read", "false"));
    ASSERT_OK(log_waiter_backends.WaitFor(10s * kTimeMultiplier));

    // Assert that the read doesn't hit a schema mismatch error.
    ASSERT_OK(conn_rr.FetchMatrix("SELECT * FROM test WHERE v = 1", 1 /* rows */, 2 /* columns */));

    LogWaiter log_lagging_backends(ts1, "Some backends are behind: ");
    ASSERT_OK(cluster_->SetFlag(ts1, "TEST_pause_wait_for_ysql_backends_catalog_version", "false"));

    // The WaitForYsqlBackends call (before marking index valid) should see the repeatable read
    // txn as lagging since its catalog_version is behind the latest version.
    ASSERT_OK(log_lagging_backends.WaitFor(30s * kTimeMultiplier));
    ASSERT_OK(create_index_future.get());

    ASSERT_OK(cluster_->SetFlagOnTServers("vmodule", "tablet_rpc=4"));
    LogWaiter log_waiter_failed_read(ts1, "backfill metadata not yet applied locally");
    auto status_future = std::async(std::launch::async, [&]() {
      return conn_rr.FetchMatrix("SELECT * FROM test WHERE v = 1", 1 /* rows */, 2 /* columns */);
    });
    ASSERT_OK(log_waiter_failed_read.WaitFor(5s * kTimeMultiplier));
    ASSERT_OK(cluster_->SetFlagOnTServers("vmodule", "tablet_rpc=0"));
    ASSERT_OK(cluster_->SetFlagOnTServers("TEST_skip_metadata_backfill_done", "false"));
    ASSERT_OK(cluster_->SetFlagOnTServers(
        "TEST_tablet_validator_disable_metadata_backfill_done", "false"));
    ASSERT_OK(WaitFor([&]() -> Result<bool> {
      return status_future.wait_for(0s) == std::future_status::ready;
    }, 10s * kTimeMultiplier, "Timed out waiting for read to fail"));
    ASSERT_NOK_STR_CONTAINS(status_future.get(), "Query read time < index birth time");
    ASSERT_OK(conn_rr.Execute("ROLLBACK"));

    // Reading as of the pre index creation time should not use the index (catalog/read time is
    // before the index birth time) and should still see the row deleted after that snapshot.
    auto conn_rt = ASSERT_RESULT(ConnectToTestDb(*ts1));
    ASSERT_OK(conn_rt.ExecuteFormat("SET yb_read_time TO $0", read_time_us));
    const auto query = "SELECT * FROM test WHERE v = 1";
    ASSERT_FALSE(ASSERT_RESULT(conn_rt.HasIndexScan(query)));
    ASSERT_OK(conn_rt.FetchMatrix(query, 1 /* rows */, 2 /* columns */));
  }
};

INSTANTIATE_TEST_CASE_P(
    Colocation, PgObjectLocksWaitForBackendsIndexValid,
    ::testing::Values(Colocated::kFalse, Colocated::kTrue),
    [](const ::testing::TestParamInfo<Colocated>& info) {
      return info.param ? "Colocated" : "NonColocated";
    });

TEST_P(PgObjectLocksWaitForBackendsIndexValid, WaitForBackendsBeforeMarkingIndexValid) {
  TestWaitForBackendsBeforeMarkingIndexValid();
}

class PgObjectLocksTestFullInval : public PgObjectLocksTest {
 protected:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* opts) override {
    PgObjectLocksTest::UpdateMiniClusterOptions(opts);
    opts->extra_tserver_flags.emplace_back("--ysql_max_invalidation_message_queue_size=0");
  }
};

TEST_F_EX(PgObjectLocksTest, ConcurrentAlterSelectFullInvalidation, PgObjectLocksTestFullInval) {
  testConcurrentAlterSelect(false);
}

TEST_F_EX(
    PgObjectLocksTest, ConcurrentAlterSelectFreshCacheFullInvalidation,
    PgObjectLocksTestFullInval) {
  testConcurrentAlterSelect(true);
}

TEST_F_EX(PgObjectLocksTest, ConcurrentAlterSelectConcurrentDdl, PgObjectLocksWithConcurrentDdl) {
  testConcurrentAlterSelect(false);
}

TEST_F_EX(
    PgObjectLocksTest, ConcurrentAlterSelectFreshCacheConcurrentDdl,
    PgObjectLocksWithConcurrentDdl) {
  testConcurrentAlterSelect(true);
}

class PgObjectLocksGlobalLockPromotionTest : public client::GeoTransactionsTestBase,
                                             public ::testing::WithParamInterface<bool> {
 protected:
  static constexpr auto kStatementTimeoutMs = 40000;

  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_object_locking_for_table_locks) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_yb_ddl_transaction_block_enabled) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_auto_analyze) = false;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_perform_ignore_pg_is_region_local) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_auto_create_local_transaction_tables) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_auto_promote_nonlocal_transactions_to_global) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_tablespace_based_transaction_placement) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_use_tablespace_based_transaction_placement) = true;
    // Postgres inherits non-default flags at postmaster start.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_object_lock_fastpath) = GetParam();
    client::GeoTransactionsTestBase::SetUp();
  }

  Status SetupPromotionCluster() {
    SetupTablespaces();
    auto conn = VERIFY_RESULT(Connect());
    auto version = GetCurrentVersion();
    RETURN_NOT_OK(conn.Execute(
        "CREATE TABLE t1_1 (k INT PRIMARY KEY, v INT) TABLESPACE tablespace1"));
    RETURN_NOT_OK(conn.Execute(
        "CREATE TABLE t1_2 (k INT PRIMARY KEY, v INT) TABLESPACE tablespace1"));
    version = WaitForStatusTabletsVersionForCreate(version);

    RETURN_NOT_OK(conn.Execute(
        "CREATE TABLE t2_1 (k INT PRIMARY KEY, v INT) TABLESPACE tablespace2"));
    RETURN_NOT_OK(conn.Execute(
        "CREATE TABLE t2_2 (k INT PRIMARY KEY, v INT) TABLESPACE tablespace2"));
    WaitForStatusTabletsVersionForCreate(version);

    RETURN_NOT_OK(conn.Execute("INSERT INTO t1_1 VALUES (1, 0), (2, 0)"));
    RETURN_NOT_OK(conn.Execute("INSERT INTO t1_2 VALUES (1, 0)"));
    RETURN_NOT_OK(conn.Execute("INSERT INTO t2_1 VALUES (1, 0)"));
    RETURN_NOT_OK(conn.Execute("INSERT INTO t2_2 VALUES (1, 0)"));
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_force_global_transactions) = false;
    return Status::OK();
  }

  Status PrepareLocalSession(PGConn& conn) {
    RETURN_NOT_OK(conn.Execute("SET yb_force_tablespace_locality = true"));
    for (const auto* table : {"t1_1", "t1_2", "t2_1", "t2_2"}) {
      RETURN_NOT_OK(WarmupTablespaceCache(conn, table));
    }
    return Status::OK();
  }

  // op_a and op_b run concurrently. Deadlock detection must abort one of them.
  template <typename FnA, typename FnB>
  Status ExpectExactlyOneSucceedsWithDeadlock(
      PGConn& conn_a, PGConn& conn_b, FnA&& op_a, FnB&& op_b,
      MonoDelta deadlock_wait = 10s * kTimeMultiplier) {
    RETURN_NOT_OK(conn_a.ExecuteFormat("SET statement_timeout = $0", kStatementTimeoutMs));
    RETURN_NOT_OK(conn_b.ExecuteFormat("SET statement_timeout = $0", kStatementTimeoutMs));
    google::SetVLOGLevel("deadlock_detector", 1);
    StringWaiterLogSink deadlock_waiter("aborted due to a deadlock");
    auto status_a = std::async(std::launch::async, [&]() -> Status {
      auto s = op_a();
      auto commit_status = conn_a.Execute("COMMIT");
      return s.ok() ? commit_status : s;
    });
    auto status_b = std::async(std::launch::async, [&]() -> Status {
      auto s = op_b();
      auto commit_status = conn_b.Execute("COMMIT");
      return s.ok() ? commit_status : s;
    });
    RETURN_NOT_OK(deadlock_waiter.WaitFor(deadlock_wait));
    RETURN_NOT_OK(WaitFor(
        [&] {
          return status_a.wait_for(0s) == std::future_status::ready &&
                 status_b.wait_for(0s) == std::future_status::ready;
        },
        MonoDelta::FromMilliseconds(kStatementTimeoutMs * 2),
        "Timed out waiting for both statements"));
    const auto result_a = status_a.get();
    const auto result_b = status_b.get();
    if (result_a.ok() == result_b.ok()) {
      return STATUS_FORMAT(
          IllegalState, "expected exactly one success, a: $0, b: $1", result_a, result_b);
    }
    return Status::OK();
  }

  Status ExpectLastTxnStatusTablet(std::string_view tablespace, client::ExpectedLocality locality) {
    auto expected = VERIFY_RESULT(GetStatusTablets(tablespace, locality));
    SCHECK(!expected.empty(), IllegalState, "No status tablets for the expected locality");
    auto last_transaction = transaction_pool_->TEST_GetLastTransaction();
    SCHECK(last_transaction, IllegalState, "No transaction recorded by the pool");
    auto metadata = last_transaction->GetMetadata(TransactionRpcDeadline()).get();
    RETURN_NOT_OK(metadata);
    SCHECK(
        std::find(expected.begin(), expected.end(), metadata->status_tablet) != expected.end(),
        IllegalState, "status tablet $0 is not $1 for $2", metadata->status_tablet, locality,
        tablespace);
    return Status::OK();
  }
};

INSTANTIATE_TEST_SUITE_P(
    , PgObjectLocksGlobalLockPromotionTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
      return info.param ? "Fastpath" : "RpcPath";
    });

TEST_P(
    PgObjectLocksGlobalLockPromotionTest, DeadlockAfterGlobalObjectLock) {
  ASSERT_OK(SetupPromotionCluster());
  auto conn_a = ASSERT_RESULT(Connect());
  auto conn_b = ASSERT_RESULT(Connect());
  ASSERT_OK(PrepareLocalSession(conn_a));
  ASSERT_OK(PrepareLocalSession(conn_b));

  ASSERT_OK(conn_a.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn_b.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  // Both transactions start tablespace-local in region 1.
  ASSERT_OK(conn_a.Execute("UPDATE t1_1 SET v = v + 1 WHERE k = 2"));
  ASSERT_OK(conn_b.Execute("UPDATE t1_1 SET v = v + 1 WHERE k = 1"));
  // Promotes transaction a. t1_2's exclusive lock is what b will wait for.
  ASSERT_OK(conn_a.Execute("LOCK TABLE t1_2 IN ACCESS EXCLUSIVE MODE"));
  ASSERT_OK(conn_a.Execute("UPDATE t2_1 SET v = v + 1 WHERE k = 1"));

  ASSERT_OK(ExpectExactlyOneSucceedsWithDeadlock(
      conn_a, conn_b,
      [&]() -> Status {
        RETURN_NOT_OK(conn_a.Execute("UPDATE t1_1 SET v = v + 1 WHERE k = 1"));
        return conn_a.Execute("UPDATE t2_1 SET v = v + 1 WHERE k = 1");
      },
      [&]() -> Status {
        return conn_b.Execute("LOCK TABLE t1_2 IN ACCESS EXCLUSIVE MODE");
      }));
}

TEST_P(
    PgObjectLocksGlobalLockPromotionTest,
    YB_DISABLE_TEST_IN_TSAN(AccessShareThenCrossExclusiveDeadlock)) {
  ASSERT_OK(SetupPromotionCluster());
  auto conn_a = ASSERT_RESULT(Connect());
  auto conn_b = ASSERT_RESULT(Connect());
  ASSERT_OK(PrepareLocalSession(conn_a));
  ASSERT_OK(PrepareLocalSession(conn_b));

  ASSERT_OK(conn_a.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn_b.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  // A is tablespace-local to region 1, B to region 2. ACCESS SHARE stays on the tserver.
  ASSERT_OK(conn_a.Execute("LOCK TABLE t1_1 IN ACCESS SHARE MODE"));
  ASSERT_OK(conn_b.Execute("LOCK TABLE t2_1 IN ACCESS SHARE MODE"));

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_refresh_waiter_timeout_ms) = 10000;
  ASSERT_OK(ExpectExactlyOneSucceedsWithDeadlock(
      conn_a, conn_b,
      [&]() -> Status {
        return conn_a.Execute("LOCK TABLE t2_1 IN ACCESS EXCLUSIVE MODE");
      },
      [&]() -> Status {
        return conn_b.Execute("LOCK TABLE t1_1 IN ACCESS EXCLUSIVE MODE");
      },
      20s * kTimeMultiplier));
}

TEST_P(PgObjectLocksGlobalLockPromotionTest, CatalogLocksDoNotPromote) {
  ASSERT_OK(SetupPromotionCluster());
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(PrepareLocalSession(conn));
  ASSERT_OK(conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn.Execute("UPDATE t1_1 SET v = v + 1 WHERE k = 1"));
  ASSERT_RESULT(conn.Fetch("SELECT proname FROM pg_proc LIMIT 1"));
  ASSERT_OK(ExpectLastTxnStatusTablet("tablespace1", client::ExpectedLocality::kLocal));
  ASSERT_OK(conn.CommitTransaction());
}

TEST_P(PgObjectLocksGlobalLockPromotionTest, MasterAndTserverOlmDeadlock) {
  ASSERT_OK(SetupPromotionCluster());
  auto conn_a = ASSERT_RESULT(Connect());
  auto conn_b = ASSERT_RESULT(Connect());
  ASSERT_OK(PrepareLocalSession(conn_a));
  ASSERT_OK(PrepareLocalSession(conn_b));

  ASSERT_OK(conn_a.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  ASSERT_OK(conn_b.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  // Both stay tablespace-local. A's ACCESS SHARE is the tserver lock B will block on.
  ASSERT_OK(conn_a.Execute("LOCK TABLE t1_1 IN ACCESS SHARE MODE"));
  ASSERT_OK(conn_b.Execute("LOCK TABLE t2_1 IN ACCESS SHARE MODE"));

  // The exclusive locks are what promote each transaction. B's lock on t2_2 is granted first, so
  // A's request waits on the master's lock manager. B's lock on t1_1 then passes the master and
  // waits for A's ACCESS SHARE on the tserver, which must use A's post-promotion status tablet.
  std::promise<void> b_holds_exclusive;
  auto b_granted = b_holds_exclusive.get_future();
  ASSERT_OK(ExpectExactlyOneSucceedsWithDeadlock(
      conn_a, conn_b,
      [&]() -> Status {
        b_granted.wait();
        return conn_a.Execute("LOCK TABLE t2_2 IN ACCESS EXCLUSIVE MODE");
      },
      [&]() -> Status {
        auto s = conn_b.Execute("LOCK TABLE t2_2 IN ACCESS EXCLUSIVE MODE");
        b_holds_exclusive.set_value();
        RETURN_NOT_OK(s);
        return conn_b.Execute("LOCK TABLE t1_1 IN ACCESS EXCLUSIVE MODE");
      }));
}

}  // namespace yb::pgwrapper
