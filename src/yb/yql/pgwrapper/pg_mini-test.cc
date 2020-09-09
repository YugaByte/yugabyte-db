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

#include "yb/yql/pgwrapper/pg_mini_test_base.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/mini_master.h"
#include "yb/master/sys_catalog_constants.h"

#include "yb/util/logging.h"
#include "yb/yql/pggate/pggate_flags.h"

#include "yb/common/pgsql_error.h"
#include "yb/common/row_mark.h"
#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"

using namespace std::literals;

DECLARE_bool(flush_rocksdb_on_shutdown);
DECLARE_bool(TEST_force_master_leader_resolution);
DECLARE_bool(ysql_enable_manual_sys_table_txn_ctl);
DECLARE_double(TEST_respond_write_failed_probability);
DECLARE_double(TEST_transaction_ignore_applying_probability_in_tests);
DECLARE_int32(history_cutoff_propagation_interval_ms);
DECLARE_int32(timestamp_history_retention_interval_sec);
DECLARE_int32(txn_max_apply_batch_records);
DECLARE_int64(apply_intents_task_injected_delay_ms);
DECLARE_uint64(max_clock_skew_usec);
DECLARE_int64(db_write_buffer_size);
DECLARE_bool(ysql_enable_manual_sys_table_txn_ctl);
DECLARE_bool(rocksdb_use_logging_iterator);

namespace yb {
namespace pgwrapper {

class PgMiniTest : public PgMiniTestBase {
 protected:
  // Have several threads doing updates and several threads doing large scans in parallel.
  // If deferrable is true, then the scans are in deferrable transactions, so no read restarts are
  // expected.
  // Otherwise, the scans are in transactions with snapshot isolation, but we still don't expect any
  // read restarts to be observer because they should be transparently handled on the postgres side.
  void TestReadRestart(bool deferrable = true);

  // Run interleaved INSERT, SELECT with specified isolation level and row mark.  Possible isolation
  // levels are SNAPSHOT_ISOLATION and SERIALIZABLE_ISOLATION.  Possible row marks are
  // ROW_MARK_KEYSHARE, ROW_MARK_SHARE, ROW_MARK_NOKEYEXCLUSIVE, and ROW_MARK_EXCLUSIVE.
  void TestInsertSelectRowLock(IsolationLevel isolation, RowMarkType row_mark);

  // Run interleaved DELETE, SELECT with specified isolation level and row mark.  Possible isolation
  // levels are SNAPSHOT_ISOLATION and SERIALIZABLE_ISOLATION.  Possible row marks are
  // ROW_MARK_KEYSHARE, ROW_MARK_SHARE, ROW_MARK_NOKEYEXCLUSIVE, and ROW_MARK_EXCLUSIVE.
  void TestDeleteSelectRowLock(IsolationLevel isolation, RowMarkType row_mark);

  // Test the row lock conflict matrix across a grid of the following parameters
  // * 4 row marks for session A
  // * 4 row marks for session B
  // * 2 isolation levels
  // This totals 4 x 4 x 2 = 32 situations.
  void TestRowLockConflictMatrix();

  void TestForeignKey(IsolationLevel isolation);

  void TestBigInsert(bool restart);

  void FlushAndCompactTablets() {
    FLAGS_timestamp_history_retention_interval_sec = 0;
    FLAGS_history_cutoff_propagation_interval_ms = 1;
    ASSERT_OK(cluster_->FlushTablets(tablet::FlushMode::kSync));
    const auto compaction_start = MonoTime::Now();
    ASSERT_OK(cluster_->CompactTablets());
    const auto compaction_finish = MonoTime::Now();
    const double compaction_elapsed_time_sec = (compaction_finish - compaction_start).ToSeconds();
    LOG(INFO) << "Compaction duration: " << compaction_elapsed_time_sec << " s";
  }
};

class PgMiniSingleTServerTest : public PgMiniTest {
 public:
  int NumTabletServers() override {
    return 1;
  }
};

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(Simple)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY, value TEXT)"));
  ASSERT_OK(conn.Execute("INSERT INTO t (key, value) VALUES (1, 'hello')"));

  auto value = ASSERT_RESULT(conn.FetchValue<std::string>("SELECT value FROM t WHERE key = 1"));
  ASSERT_EQ(value, "hello");
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(WriteRetry)) {
  constexpr int kKeys = 100;
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY)"));

  SetAtomicFlag(0.25, &FLAGS_TEST_respond_write_failed_probability);

  LOG(INFO) << "Insert " << kKeys << " keys";
  for (int key = 0; key != kKeys; ++key) {
    auto status = conn.ExecuteFormat("INSERT INTO t (key) VALUES ($0)", key);
    ASSERT_TRUE(status.ok() || PgsqlError(status) == YBPgErrorCode::YB_PG_UNIQUE_VIOLATION ||
                status.ToString().find("Already present: Duplicate request") != std::string::npos)
        << status;
  }

  SetAtomicFlag(0, &FLAGS_TEST_respond_write_failed_probability);

  auto result = ASSERT_RESULT(conn.FetchMatrix("SELECT * FROM t ORDER BY key", kKeys, 1));
  for (int key = 0; key != kKeys; ++key) {
    auto fetched_key = ASSERT_RESULT(GetInt32(result.get(), key, 0));
    ASSERT_EQ(fetched_key, key);
  }

  LOG(INFO) << "Insert duplicate key";
  auto status = conn.Execute("INSERT INTO t (key) VALUES (1)");
  ASSERT_EQ(PgsqlError(status), YBPgErrorCode::YB_PG_UNIQUE_VIOLATION) << status;
  ASSERT_STR_CONTAINS(status.ToString(), "duplicate key value violates unique constraint");
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(With)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE test (k int PRIMARY KEY, v int)"));

  ASSERT_OK(conn.Execute(
      "WITH test2 AS (UPDATE test SET v = 2 WHERE k = 1) "
      "UPDATE test SET v = 3 WHERE k = 1"));
}

void PgMiniTest::TestReadRestart(const bool deferrable) {
  constexpr CoarseDuration kWaitTime = 60s;
  constexpr int kKeys = 100;
  constexpr int kNumReadThreads = 8;
  constexpr int kNumUpdateThreads = 8;
  constexpr int kRequiredNumReads = 500;
  constexpr std::chrono::milliseconds kClockSkew = -100ms;
  std::atomic<int> num_read_restarts(0);
  std::atomic<int> num_read_successes(0);
  TestThreadHolder thread_holder;

  SetAtomicFlag(250000ULL, &FLAGS_max_clock_skew_usec);

  // Set up table
  auto setup_conn = ASSERT_RESULT(Connect());
  ASSERT_OK(setup_conn.Execute("CREATE TABLE t (key INT PRIMARY KEY, value INT)"));
  for (int key = 0; key != kKeys; ++key) {
    ASSERT_OK(setup_conn.Execute(Format("INSERT INTO t (key, value) VALUES ($0, 0)", key)));
  }

  // Introduce clock skew
  auto delta_changers = SkewClocks(cluster_.get(), kClockSkew);

  // Start read threads
  for (int i = 0; i < kNumReadThreads; ++i) {
    thread_holder.AddThreadFunctor([this, deferrable, &num_read_restarts, &num_read_successes,
                                    &stop = thread_holder.stop_flag()] {
      auto read_conn = ASSERT_RESULT(Connect());
      while (!stop.load(std::memory_order_acquire)) {
        if (deferrable) {
          ASSERT_OK(read_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY, "
                                      "DEFERRABLE"));
        } else {
          ASSERT_OK(read_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
        }
        auto result = read_conn.FetchMatrix("SELECT * FROM t", kKeys, 2);
        if (!result.ok()) {
          ASSERT_TRUE(result.status().IsNetworkError()) << result.status();
          ASSERT_EQ(PgsqlError(result.status()), YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE)
              << result.status();
          ASSERT_STR_CONTAINS(result.status().ToString(), "Restart read");
          ++num_read_restarts;
          ASSERT_OK(read_conn.Execute("ABORT"));
        } else {
          ASSERT_OK(read_conn.Execute("COMMIT"));
          ++num_read_successes;
        }
      }
    });
  }

  // Start update threads
  for (int i = 0; i < kNumUpdateThreads; ++i) {
    thread_holder.AddThreadFunctor([this, i, &stop = thread_holder.stop_flag()] {
      auto update_conn = ASSERT_RESULT(Connect());
      while (!stop.load(std::memory_order_acquire)) {
        for (int key = i; key < kKeys; key += kNumUpdateThreads) {
          ASSERT_OK(update_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
          ASSERT_OK(update_conn.Execute(
              Format("UPDATE t SET value = value + 1 WHERE key = $0", key)));
          ASSERT_OK(update_conn.Execute("COMMIT"));
        }
      }
    });
  }

  // Stop threads after a while
  thread_holder.WaitAndStop(kWaitTime);

  // Count successful reads
  int num_reads = (num_read_restarts.load(std::memory_order_acquire)
                   + num_read_successes.load(std::memory_order_acquire));
  LOG(INFO) << "Successful reads: " << num_read_successes.load(std::memory_order_acquire) << "/"
      << num_reads;
  ASSERT_EQ(num_read_restarts.load(std::memory_order_acquire), 0);
  ASSERT_GT(num_read_successes.load(std::memory_order_acquire), kRequiredNumReads);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(ReadRestartSerializableDeferrable)) {
  TestReadRestart(true /* deferrable */);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(ReadRestartSnapshot)) {
  TestReadRestart(false /* deferrable */);
}

void PgMiniTest::TestInsertSelectRowLock(IsolationLevel isolation, RowMarkType row_mark) {
  const std::string isolation_str = (
      isolation == IsolationLevel::SNAPSHOT_ISOLATION ? "REPEATABLE READ" : "SERIALIZABLE");
  const std::string row_mark_str = RowMarkTypeToPgsqlString(row_mark);
  constexpr auto kSleepTime = 1s;
  constexpr int kKeys = 3;
  PGConn read_conn = ASSERT_RESULT(Connect());
  PGConn misc_conn = ASSERT_RESULT(Connect());
  PGConn write_conn = ASSERT_RESULT(Connect());

  // Set up table
  ASSERT_OK(misc_conn.Execute("CREATE TABLE t (i INT PRIMARY KEY, j INT)"));
  // TODO: remove this when issue #2857 is fixed.
  std::this_thread::sleep_for(kSleepTime);
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_OK(misc_conn.ExecuteFormat("INSERT INTO t (i, j) VALUES ($0, $0)", i));
  }

  ASSERT_OK(read_conn.ExecuteFormat("BEGIN TRANSACTION ISOLATION LEVEL $0", isolation_str));
  ASSERT_OK(read_conn.Fetch("SELECT '(setting read point)'"));
  ASSERT_OK(write_conn.ExecuteFormat("INSERT INTO t (i, j) VALUES ($0, $0)", kKeys));
  auto result = read_conn.FetchFormat("SELECT * FROM t FOR $0", row_mark_str);
  if (isolation == IsolationLevel::SNAPSHOT_ISOLATION) {
    // TODO: change to ASSERT_OK and expect kKeys rows when issue #2809 is fixed.
    ASSERT_NOK(result);
    ASSERT_TRUE(result.status().IsNetworkError()) << result.status();
    ASSERT_EQ(PgsqlError(result.status()), YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE)
        << result.status();
    ASSERT_STR_CONTAINS(result.status().ToString(), "Value write after transaction start");
    ASSERT_OK(read_conn.Execute("ABORT"));
  } else {
    ASSERT_OK(result);
    // NOTE: vanilla PostgreSQL expects kKeys rows, but kKeys + 1 rows are expected for Yugabyte.
    ASSERT_EQ(PQntuples(result.get().get()), kKeys + 1);
    ASSERT_OK(read_conn.Execute("COMMIT"));
  }
}

void PgMiniTest::TestDeleteSelectRowLock(IsolationLevel isolation, RowMarkType row_mark) {
  const std::string isolation_str = (
      isolation == IsolationLevel::SNAPSHOT_ISOLATION ? "REPEATABLE READ" : "SERIALIZABLE");
  const std::string row_mark_str = RowMarkTypeToPgsqlString(row_mark);
  constexpr auto kSleepTime = 1s;
  constexpr int kKeys = 3;
  PGConn read_conn = ASSERT_RESULT(Connect());
  PGConn misc_conn = ASSERT_RESULT(Connect());
  PGConn write_conn = ASSERT_RESULT(Connect());

  // Set up table
  ASSERT_OK(misc_conn.Execute("CREATE TABLE t (i INT PRIMARY KEY, j INT)"));
  // TODO: remove this when issue #2857 is fixed.
  std::this_thread::sleep_for(kSleepTime);
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_OK(misc_conn.ExecuteFormat("INSERT INTO t (i, j) VALUES ($0, $0)", i));
  }

  ASSERT_OK(read_conn.ExecuteFormat("BEGIN TRANSACTION ISOLATION LEVEL $0", isolation_str));
  ASSERT_OK(read_conn.Fetch("SELECT '(setting read point)'"));
  ASSERT_OK(write_conn.ExecuteFormat("DELETE FROM t WHERE i = $0", RandomUniformInt(0, kKeys - 1)));
  auto result = read_conn.FetchFormat("SELECT * FROM t FOR $0", row_mark_str);
  if (isolation == IsolationLevel::SNAPSHOT_ISOLATION) {
    ASSERT_NOK(result);
    ASSERT_TRUE(result.status().IsNetworkError()) << result.status();
    ASSERT_EQ(PgsqlError(result.status()), YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE)
        << result.status();
    ASSERT_STR_CONTAINS(result.status().ToString(), "Value write after transaction start");
    ASSERT_OK(read_conn.Execute("ABORT"));
  } else {
    ASSERT_OK(result);
    // NOTE: vanilla PostgreSQL expects kKeys rows, but kKeys - 1 rows are expected for Yugabyte.
    ASSERT_EQ(PQntuples(result.get().get()), kKeys - 1);
    ASSERT_OK(read_conn.Execute("COMMIT"));
  }
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SnapshotInsertForUpdate)) {
  TestInsertSelectRowLock(IsolationLevel::SNAPSHOT_ISOLATION, RowMarkType::ROW_MARK_EXCLUSIVE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableInsertForUpdate)) {
  TestInsertSelectRowLock(IsolationLevel::SERIALIZABLE_ISOLATION, RowMarkType::ROW_MARK_EXCLUSIVE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SnapshotInsertForNoKeyUpdate)) {
  TestInsertSelectRowLock(IsolationLevel::SNAPSHOT_ISOLATION,
                          RowMarkType::ROW_MARK_NOKEYEXCLUSIVE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableInsertForNoKeyUpdate)) {
  TestInsertSelectRowLock(IsolationLevel::SERIALIZABLE_ISOLATION,
                          RowMarkType::ROW_MARK_NOKEYEXCLUSIVE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SnapshotInsertForShare)) {
  TestInsertSelectRowLock(IsolationLevel::SNAPSHOT_ISOLATION, RowMarkType::ROW_MARK_SHARE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableInsertForShare)) {
  TestInsertSelectRowLock(IsolationLevel::SERIALIZABLE_ISOLATION, RowMarkType::ROW_MARK_SHARE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SnapshotInsertForKeyShare)) {
  TestInsertSelectRowLock(IsolationLevel::SNAPSHOT_ISOLATION, RowMarkType::ROW_MARK_KEYSHARE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableInsertForKeyShare)) {
  TestInsertSelectRowLock(IsolationLevel::SERIALIZABLE_ISOLATION, RowMarkType::ROW_MARK_KEYSHARE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SnapshotDeleteForUpdate)) {
  TestDeleteSelectRowLock(IsolationLevel::SNAPSHOT_ISOLATION, RowMarkType::ROW_MARK_EXCLUSIVE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableDeleteForUpdate)) {
  TestDeleteSelectRowLock(IsolationLevel::SERIALIZABLE_ISOLATION, RowMarkType::ROW_MARK_EXCLUSIVE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SnapshotDeleteForNoKeyUpdate)) {
  TestDeleteSelectRowLock(IsolationLevel::SNAPSHOT_ISOLATION,
                          RowMarkType::ROW_MARK_NOKEYEXCLUSIVE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableDeleteForNoKeyUpdate)) {
  TestDeleteSelectRowLock(IsolationLevel::SERIALIZABLE_ISOLATION,
                          RowMarkType::ROW_MARK_NOKEYEXCLUSIVE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SnapshotDeleteForShare)) {
  TestDeleteSelectRowLock(IsolationLevel::SNAPSHOT_ISOLATION, RowMarkType::ROW_MARK_SHARE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableDeleteForShare)) {
  TestDeleteSelectRowLock(IsolationLevel::SERIALIZABLE_ISOLATION, RowMarkType::ROW_MARK_SHARE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SnapshotDeleteForKeyShare)) {
  TestDeleteSelectRowLock(IsolationLevel::SNAPSHOT_ISOLATION, RowMarkType::ROW_MARK_KEYSHARE);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableDeleteForKeyShare)) {
  TestDeleteSelectRowLock(IsolationLevel::SERIALIZABLE_ISOLATION, RowMarkType::ROW_MARK_KEYSHARE);
}

void PgMiniTest::TestRowLockConflictMatrix() {
  constexpr auto kSleepTime = 1s;
  constexpr int kKeys = 3;
  constexpr int kNumIsolationLevels = 2;
  constexpr int kNumRowMarkTypes = 4;
  PGConn conn_a = ASSERT_RESULT(Connect());
  PGConn conn_b = ASSERT_RESULT(Connect());
  PGConn conn_misc = ASSERT_RESULT(Connect());

  const std::array<RowMarkType, kNumRowMarkTypes> row_mark_types = {{
    RowMarkType::ROW_MARK_EXCLUSIVE,
    RowMarkType::ROW_MARK_NOKEYEXCLUSIVE,
    RowMarkType::ROW_MARK_SHARE,
    RowMarkType::ROW_MARK_KEYSHARE,
  }};
  const std::array<std::string, kNumIsolationLevels> isolation_strs = {{
    "REPEATABLE READ",
    "SERIALIZABLE",
  }};

  // Set up table
  ASSERT_OK(conn_misc.Execute("CREATE TABLE t (i INT PRIMARY KEY, j INT)"));
  // TODO: remove this sleep when issue #2857 is fixed.
  std::this_thread::sleep_for(kSleepTime);
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_OK(conn_misc.ExecuteFormat("INSERT INTO t (i, j) VALUES ($0, $0)", i));
  }

  for (const auto& row_mark_type_a : row_mark_types) {
    for (const auto& row_mark_type_b : row_mark_types) {
      for (const auto& isolation_str : isolation_strs) {
        const std::string row_mark_str_a = RowMarkTypeToPgsqlString(row_mark_type_a);
        const std::string row_mark_str_b = RowMarkTypeToPgsqlString(row_mark_type_b);
        LOG(INFO) << "Testing " << row_mark_str_a << " vs " << row_mark_str_b << " with "
                  << isolation_str << " isolation transactions.";

        ASSERT_OK(conn_a.ExecuteFormat("BEGIN TRANSACTION ISOLATION LEVEL $0", isolation_str));
        ASSERT_RESULT(conn_a.Fetch("SELECT '(setting read point)'"));
        ASSERT_OK(conn_b.ExecuteFormat("BEGIN TRANSACTION ISOLATION LEVEL $0", isolation_str));
        ASSERT_RESULT(conn_b.Fetch("SELECT '(setting read point)'"));
        ASSERT_RESULT(conn_a.FetchFormat("SELECT * FROM t FOR $0", row_mark_str_a));
        Result<PGResultPtr> result_select = conn_b.FetchFormat("SELECT * FROM t FOR $0",
                                                               row_mark_str_b);
        // TODO: remove this sleep when issue #2910 is fixed.
        std::this_thread::sleep_for(kSleepTime);
        Status status_commit = conn_a.Execute("COMMIT");
        ASSERT_OK(conn_b.Execute("COMMIT"));
        if (AreConflictingRowMarkTypes(row_mark_type_a, row_mark_type_b)) {
          if (result_select.ok()) {
            // Should conflict on COMMIT only.
            ASSERT_NOK(status_commit);
            ASSERT_TRUE(status_commit.IsNetworkError()) << status_commit;
            ASSERT_EQ(PgsqlError(status_commit), YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE)
                << status_commit;
          } else {
            // Should conflict on SELECT only.
            ASSERT_OK(status_commit);
            ASSERT_TRUE(result_select.status().IsNetworkError()) << result_select.status();
            ASSERT_EQ(PgsqlError(result_select.status()),
                      YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE)
                << result_select.status();
            ASSERT_STR_CONTAINS(result_select.status().ToString(),
                                "Conflicts with higher priority transaction");
          }
        } else {
          ASSERT_OK(result_select);
          ASSERT_OK(status_commit);
        }
      }
    }
  }
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(RowLockConflictMatrix)) {
  TestRowLockConflictMatrix();
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SerializableReadOnly)) {
  PGConn read_conn = ASSERT_RESULT(Connect());
  PGConn setup_conn = ASSERT_RESULT(Connect());
  PGConn write_conn = ASSERT_RESULT(Connect());

  // Set up table
  ASSERT_OK(setup_conn.Execute("CREATE TABLE t (i INT)"));
  ASSERT_OK(setup_conn.Execute("INSERT INTO t (i) VALUES (0)"));

  // SERIALIZABLE, READ ONLY should use snapshot isolation
  ASSERT_OK(read_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY"));
  ASSERT_OK(write_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ WRITE"));
  ASSERT_OK(write_conn.Execute("UPDATE t SET i = i + 1"));
  ASSERT_OK(read_conn.Fetch("SELECT * FROM t"));
  ASSERT_OK(read_conn.Execute("COMMIT"));
  ASSERT_OK(write_conn.Execute("COMMIT"));

  // READ ONLY, SERIALIZABLE should use snapshot isolation
  ASSERT_OK(read_conn.Execute("BEGIN TRANSACTION READ ONLY, ISOLATION LEVEL SERIALIZABLE"));
  ASSERT_OK(write_conn.Execute("BEGIN TRANSACTION READ WRITE, ISOLATION LEVEL SERIALIZABLE"));
  ASSERT_OK(read_conn.Fetch("SELECT * FROM t"));
  ASSERT_OK(write_conn.Execute("UPDATE t SET i = i + 1"));
  ASSERT_OK(read_conn.Execute("COMMIT"));
  ASSERT_OK(write_conn.Execute("COMMIT"));

  // SHOW for READ ONLY should show serializable
  ASSERT_OK(read_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY"));
  Result<PGResultPtr> result = read_conn.Fetch("SHOW transaction_isolation");
  ASSERT_TRUE(result.ok()) << result.status();
  string value = ASSERT_RESULT(GetString(result.get().get(), 0, 0));
  ASSERT_EQ(value, "serializable");
  ASSERT_OK(read_conn.Execute("COMMIT"));

  // SHOW for READ WRITE to READ ONLY should show serializable and read_only
  ASSERT_OK(write_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ WRITE"));
  ASSERT_OK(write_conn.Execute("SET TRANSACTION READ ONLY"));
  result = write_conn.Fetch("SHOW transaction_isolation");
  ASSERT_TRUE(result.ok()) << result.status();
  value = ASSERT_RESULT(GetString(result.get().get(), 0, 0));
  ASSERT_EQ(value, "serializable");
  result = write_conn.Fetch("SHOW transaction_read_only");
  ASSERT_TRUE(result.ok()) << result.status();
  value = ASSERT_RESULT(GetString(result.get().get(), 0, 0));
  ASSERT_EQ(value, "on");
  ASSERT_OK(write_conn.Execute("COMMIT"));

  // SERIALIZABLE, READ ONLY to READ WRITE should not use snapshot isolation
  ASSERT_OK(read_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY"));
  ASSERT_OK(write_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ WRITE"));
  ASSERT_OK(read_conn.Execute("SET TRANSACTION READ WRITE"));
  ASSERT_OK(write_conn.Execute("UPDATE t SET i = i + 1"));
  // The result of the following statement is probabilistic.  If it does not fail now, then it
  // should fail during COMMIT.
  result = read_conn.Fetch("SELECT * FROM t");
  if (result.ok()) {
    ASSERT_OK(read_conn.Execute("COMMIT"));
    Status status = write_conn.Execute("COMMIT");
    ASSERT_NOK(status);
    ASSERT_TRUE(status.IsNetworkError()) << status;
    ASSERT_EQ(PgsqlError(status), YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE) << status;
  } else {
    ASSERT_TRUE(result.status().IsNetworkError()) << result.status();
    ASSERT_EQ(PgsqlError(result.status()), YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE)
        << result.status();
    ASSERT_STR_CONTAINS(result.status().ToString(), "Conflicts with higher priority transaction");
  }
}

void AssertAborted(const Status& status) {
  ASSERT_NOK(status);
  ASSERT_STR_CONTAINS(status.ToString(), "Transaction aborted");
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_SANITIZERS(SelectModifySelect)) {
  {
    auto read_conn = ASSERT_RESULT(Connect());
    auto write_conn = ASSERT_RESULT(Connect());

    ASSERT_OK(read_conn.Execute("CREATE TABLE t (i INT)"));
    ASSERT_OK(read_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE"));
    ASSERT_RESULT(read_conn.FetchMatrix("SELECT * FROM t", 0, 1));
    ASSERT_OK(write_conn.Execute("INSERT INTO t VALUES (1)"));
    ASSERT_NO_FATALS(AssertAborted(ResultToStatus(read_conn.Fetch("SELECT * FROM t"))));
  }
  {
    auto read_conn = ASSERT_RESULT(Connect());
    auto write_conn = ASSERT_RESULT(Connect());

    ASSERT_OK(read_conn.Execute("CREATE TABLE t2 (i INT PRIMARY KEY)"));
    ASSERT_OK(read_conn.Execute("INSERT INTO t2 VALUES (1)"));

    ASSERT_OK(read_conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE"));
    ASSERT_RESULT(read_conn.FetchMatrix("SELECT * FROM t2", 1, 1));
    ASSERT_OK(write_conn.Execute("DELETE FROM t2 WHERE i = 1"));
    ASSERT_NO_FATALS(AssertAborted(ResultToStatus(read_conn.Fetch("SELECT * FROM t2"))));
  }
}

class PgMiniSmallWriteBufferTest : public PgMiniTest {
 public:
  void SetUp() override {
    FLAGS_db_write_buffer_size = 256_KB;
    PgMiniTest::SetUp();
  }
};

TEST_F_EX(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(BulkCopyWithRestart), PgMiniSmallWriteBufferTest) {
  const std::string kTableName = "key_value";
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE $0 (key INTEGER NOT NULL PRIMARY KEY, value VARCHAR)",
      kTableName));

  TestThreadHolder thread_holder;
  constexpr int kTotalBatches = RegularBuildVsSanitizers(50, 5);
  constexpr int kBatchSize = 1000;
  constexpr int kValueSize = 128;

  std::atomic<int> key(0);

  thread_holder.AddThreadFunctor([this, &kTableName, &stop = thread_holder.stop_flag(), &key] {
    SetFlagOnExit set_flag(&stop);
    auto conn = ASSERT_RESULT(Connect());

    auto se = ScopeExit([&key] {
      LOG(INFO) << "Total keys: " << key;
    });

    while (!stop.load(std::memory_order_acquire) && key < kBatchSize * kTotalBatches) {
      ASSERT_OK(conn.CopyBegin(Format("COPY $0 FROM STDIN WITH BINARY", kTableName)));
      for (int j = 0; j != kBatchSize; ++j) {
        conn.CopyStartRow(2);
        conn.CopyPutInt32(++key);
        conn.CopyPutString(RandomHumanReadableString(kValueSize));
      }

      ASSERT_OK(conn.CopyEnd());
    }
  });

  thread_holder.AddThread(RestartsThread(cluster_.get(), 5s, &thread_holder.stop_flag()));

  thread_holder.WaitAndStop(120s); // Actually will stop when enough batches were copied

  ASSERT_EQ(key.load(std::memory_order_relaxed), kTotalBatches * kBatchSize);

  LOG(INFO) << "Restarting cluster";
  ASSERT_OK(cluster_->RestartSync());

  ASSERT_OK(WaitFor([this, &conn, &key, &kTableName] {
    auto intents_count = CountIntents(cluster_.get());
    LOG(INFO) << "Intents count: " << intents_count;

    if (intents_count <= 5000) {
      return true;
    }

    // We cleanup only transactions that were completely aborted/applied before last replication
    // happens.
    // So we could get into situation when intents of the last transactions are not cleaned.
    // To avoid such scenario in this test we write one more row to allow cleanup.
    EXPECT_OK(conn.ExecuteFormat(
        "INSERT INTO $0 VALUES ($1, '$2')", kTableName, ++key,
        RandomHumanReadableString(kValueSize)));

    return false;
  }, 5s, "Intents cleanup", 200ms));
}

void PgMiniTest::TestForeignKey(IsolationLevel isolation_level) {
  const std::string kDataTable = "data";
  const std::string kReferenceTable = "reference";
  constexpr int kRows = 10;
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE $0 (id int NOT NULL, name VARCHAR, PRIMARY KEY (id))",
      kReferenceTable));
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE $0 (ref_id INTEGER, data_id INTEGER, name VARCHAR, "
          "PRIMARY KEY (ref_id, data_id))",
      kDataTable));
  ASSERT_OK(conn.ExecuteFormat(
      "ALTER TABLE $0 ADD CONSTRAINT fk FOREIGN KEY(ref_id) REFERENCES $1(id) "
          "ON DELETE CASCADE",
      kDataTable, kReferenceTable));

  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES ($1, 'reference_$1')", kReferenceTable, 1));

  for (int i = 1; i <= kRows; ++i) {
    ASSERT_OK(conn.StartTransaction(isolation_level));
    ASSERT_OK(conn.ExecuteFormat(
        "INSERT INTO $0 VALUES ($1, $2, 'data_$2')", kDataTable, 1, i));
    ASSERT_OK(conn.CommitTransaction());
  }

  ASSERT_OK(WaitFor([this] {
    return CountIntents(cluster_.get()) == 0;
  }, 15s, "Intents cleanup"));
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(ForeignKeySerializable)) {
  TestForeignKey(IsolationLevel::SERIALIZABLE_ISOLATION);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(ForeignKeySnapshot)) {
  TestForeignKey(IsolationLevel::SNAPSHOT_ISOLATION);
}

// ------------------------------------------------------------------------------------------------
// A test performing manual transaction control on system tables.
// ------------------------------------------------------------------------------------------------

class PgMiniTestManualSysTableTxn : public PgMiniTest {
  virtual void BeforePgProcessStart() {
    // Enable manual transaction control for operations on system tables. Otherwise, they would
    // execute non-transactionally.
    FLAGS_ysql_enable_manual_sys_table_txn_ctl = true;
  }
};

TEST_F_EX(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(SystemTableTxnTest), PgMiniTestManualSysTableTxn) {

  // Resolving conflicts between transactions on a system table.
  //
  // postgres=# \d pg_ts_dict;
  //
  //              Table "pg_catalog.pg_ts_dict"
  //      Column     | Type | Collation | Nullable | Default
  // ----------------+------+-----------+----------+---------
  //  dictname       | name |           | not null |
  //  dictnamespace  | oid  |           | not null |
  //  dictowner      | oid  |           | not null |
  //  dicttemplate   | oid  |           | not null |
  //  dictinitoption | text |           |          |
  // Indexes:
  //     "pg_ts_dict_oid_index" PRIMARY KEY, lsm (oid)
  //     "pg_ts_dict_dictname_index" UNIQUE, lsm (dictname, dictnamespace)

  auto conn1 = ASSERT_RESULT(Connect());
  auto conn2 = ASSERT_RESULT(Connect());

  size_t commit1_fail_count = 0;
  size_t commit2_fail_count = 0;
  size_t insert2_fail_count = 0;

  const auto kStartTxnStatementStr = "START TRANSACTION ISOLATION LEVEL REPEATABLE READ";
  for (int i = 1; i <= 100; ++i) {
    std::string dictname = Format("contendedkey$0", i);
    const int dictnamespace = i;
    ASSERT_OK(conn1.Execute(kStartTxnStatementStr));
    ASSERT_OK(conn2.Execute(kStartTxnStatementStr));

    // Insert a row in each transaction. The first insert should always succeed.
    ASSERT_OK(conn1.Execute(
        Format("INSERT INTO pg_ts_dict VALUES ('$0', $1, 1, 2, 'b')", dictname, dictnamespace)));
    Status insert_status2 = conn2.Execute(
        Format("INSERT INTO pg_ts_dict VALUES ('$0', $1, 3, 4, 'c')", dictname, dictnamespace));
    if (!insert_status2.ok()) {
      LOG(INFO) << "MUST BE A CONFLICT: Insert failed: " << insert_status2;
      insert2_fail_count++;
    }

    Status commit_status1;
    Status commit_status2;
    if (RandomUniformBool()) {
      commit_status1 = conn1.Execute("COMMIT");
      commit_status2 = conn2.Execute("COMMIT");
    } else {
      commit_status2 = conn2.Execute("COMMIT");
      commit_status1 = conn1.Execute("COMMIT");
    }
    if (!commit_status1.ok()) {
      commit1_fail_count++;
    }
    if (!commit_status2.ok()) {
      commit2_fail_count++;
    }

    auto get_commit_statuses_str = [&commit_status1, &commit_status2]() {
      return Format("commit_status1=$0, commit_status2=$1", commit_status1, commit_status2);
    };

    bool succeeded1 = commit_status1.ok();
    bool succeeded2 = insert_status2.ok() && commit_status2.ok();

    ASSERT_TRUE(!succeeded1 || !succeeded2)
        << "Both transactions can't commit. " << get_commit_statuses_str();
    ASSERT_TRUE(succeeded1 || succeeded2)
        << "We expect one of the two transactions to succeed. " << get_commit_statuses_str();
    if (!commit_status1.ok()) {
      ASSERT_OK(conn1.Execute("ROLLBACK"));
    }
    if (!commit_status2.ok()) {
      ASSERT_OK(conn2.Execute("ROLLBACK"));
    }

    if (RandomUniformBool()) {
      std::swap(conn1, conn2);
    }
  }
  LOG(INFO) << "Test stats: "
            << EXPR_VALUE_FOR_LOG(commit1_fail_count) << ", "
            << EXPR_VALUE_FOR_LOG(insert2_fail_count) << ", "
            << EXPR_VALUE_FOR_LOG(commit2_fail_count);
  ASSERT_GE(commit1_fail_count, 25);
  ASSERT_GE(insert2_fail_count, 25);
  ASSERT_EQ(commit2_fail_count, 0);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBUpdateSysTablet)) {
  const std::string kDatabaseName = "testdb";
  master::CatalogManager *catalog_manager =
      cluster_->leader_mini_master()->master()->catalog_manager();
  PGConn conn = ASSERT_RESULT(Connect());
  scoped_refptr<master::TabletInfo> sys_tablet;
  std::array<int, 4> num_tables;

  {
    SharedLock<master::CatalogManager::LockType> catalog_lock(catalog_manager->lock_);
    sys_tablet = catalog_manager->tablet_map_->find(master::kSysCatalogTabletId)->second;
  }
  {
    auto tablet_lock = sys_tablet->LockForWrite();
    num_tables[0] = tablet_lock->data().pb.table_ids_size();
  }
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", kDatabaseName));
  {
    auto tablet_lock = sys_tablet->LockForWrite();
    num_tables[1] = tablet_lock->data().pb.table_ids_size();
  }
  ASSERT_OK(conn.ExecuteFormat("DROP DATABASE $0", kDatabaseName));
  {
    auto tablet_lock = sys_tablet->LockForWrite();
    num_tables[2] = tablet_lock->data().pb.table_ids_size();
  }
  // Make sure that the system catalog tablet table_ids is persisted.
  ASSERT_OK(cluster_->RestartSync());
  {
    // Refresh stale local variables after RestartSync.
    catalog_manager = cluster_->leader_mini_master()->master()->catalog_manager();
    SharedLock<master::CatalogManager::LockType> catalog_lock(catalog_manager->lock_);
    sys_tablet = catalog_manager->tablet_map_->find(master::kSysCatalogTabletId)->second;
  }
  {
    auto tablet_lock = sys_tablet->LockForWrite();
    num_tables[3] = tablet_lock->data().pb.table_ids_size();
  }
  ASSERT_LT(num_tables[0], num_tables[1]);
  ASSERT_EQ(num_tables[0], num_tables[2]);
  ASSERT_EQ(num_tables[0], num_tables[3]);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBMarkDeleted)) {
  const std::string kDatabaseName = "testdb";
  constexpr auto kSleepTime = 500ms;
  constexpr int kMaxNumSleeps = 20;
  master::CatalogManager *catalog_manager =
      cluster_->leader_mini_master()->master()->catalog_manager();
  PGConn conn = ASSERT_RESULT(Connect());

  ASSERT_FALSE(catalog_manager->AreTablesDeleting());
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", kDatabaseName));
  ASSERT_OK(conn.ExecuteFormat("DROP DATABASE $0", kDatabaseName));
  // System tables should be deleting then deleted.
  int num_sleeps = 0;
  while (catalog_manager->AreTablesDeleting() && (num_sleeps++ != kMaxNumSleeps)) {
    LOG(INFO) << "Tables are deleting...";
    std::this_thread::sleep_for(kSleepTime);
  }
  ASSERT_FALSE(catalog_manager->AreTablesDeleting()) << "Tables should have finished deleting";
  // Make sure that the table deletions are persisted.
  ASSERT_OK(cluster_->RestartSync());
  // Refresh stale local variable after RestartSync.
  catalog_manager = cluster_->leader_mini_master()->master()->catalog_manager();
  ASSERT_FALSE(catalog_manager->AreTablesDeleting());
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBWithTables)) {
  const std::string kDatabaseName = "testdb";
  const std::string kTablePrefix = "testt";
  constexpr auto kSleepTime = 500ms;
  constexpr int kMaxNumSleeps = 20;
  int num_tables_before, num_tables_after;
  master::CatalogManager *catalog_manager =
      cluster_->leader_mini_master()->master()->catalog_manager();
  PGConn conn = ASSERT_RESULT(Connect());
  scoped_refptr<master::TabletInfo> sys_tablet;

  {
    SharedLock<master::CatalogManager::LockType> catalog_lock(catalog_manager->lock_);
    sys_tablet = catalog_manager->tablet_map_->find(master::kSysCatalogTabletId)->second;
  }
  {
    auto tablet_lock = sys_tablet->LockForWrite();
    num_tables_before = tablet_lock->data().pb.table_ids_size();
  }
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", kDatabaseName));
  {
    PGConn conn_new = ASSERT_RESULT(ConnectToDB(kDatabaseName));
    for (int i = 0; i < 10; ++i) {
      ASSERT_OK(conn_new.ExecuteFormat("CREATE TABLE $0$1 (i int)", kTablePrefix, i));
    }
    ASSERT_OK(conn_new.ExecuteFormat("INSERT INTO $0$1 (i) VALUES (1), (2), (3)", kTablePrefix, 5));
  }
  ASSERT_OK(conn.ExecuteFormat("DROP DATABASE $0", kDatabaseName));
  // User and system tables should be deleting then deleted.
  int num_sleeps = 0;
  while (catalog_manager->AreTablesDeleting() && (num_sleeps++ != kMaxNumSleeps)) {
    LOG(INFO) << "Tables are deleting...";
    std::this_thread::sleep_for(kSleepTime);
  }
  ASSERT_FALSE(catalog_manager->AreTablesDeleting()) << "Tables should have finished deleting";
  // Make sure that the table deletions are persisted.
  ASSERT_OK(cluster_->RestartSync());
  {
    // Refresh stale local variables after RestartSync.
    catalog_manager = cluster_->leader_mini_master()->master()->catalog_manager();
    SharedLock<master::CatalogManager::LockType> catalog_lock(catalog_manager->lock_);
    sys_tablet = catalog_manager->tablet_map_->find(master::kSysCatalogTabletId)->second;
  }
  ASSERT_FALSE(catalog_manager->AreTablesDeleting());
  {
    auto tablet_lock = sys_tablet->LockForWrite();
    num_tables_after = tablet_lock->data().pb.table_ids_size();
  }
  ASSERT_EQ(num_tables_before, num_tables_after);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(BigSelect)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY, value TEXT)"));

  constexpr size_t kRows = 400;
  constexpr size_t kValueSize = RegularBuildVsSanitizers(256_KB, 4_KB);

  for (size_t i = 0; i != kRows; ++i) {
    ASSERT_OK(conn.ExecuteFormat(
        "INSERT INTO t VALUES ($0, '$1')", i, RandomHumanReadableString(kValueSize)));
  }

  auto start = MonoTime::Now();
  auto res = ASSERT_RESULT(conn.FetchValue<int64_t>("SELECT COUNT(DISTINCT(value)) FROM t"));
  auto finish = MonoTime::Now();
  LOG(INFO) << "Time: " << finish - start;
  ASSERT_EQ(res, kRows);
}

TEST_F_EX(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(ManyRowsInsert), PgMiniSingleTServerTest) {
  constexpr int kRows = 100000;
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY)"));

  auto start = MonoTime::Now();
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO t SELECT generate_series(1, $0)", kRows));
  auto finish = MonoTime::Now();
  LOG(INFO) << "Time: " << finish - start;
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(MoveMaster)) {
  ShutdownAllMasters(cluster_.get());
  cluster_->mini_master(0)->set_pass_master_addresses(false);
  ASSERT_OK(StartAllMasters(cluster_.get()));

  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(WaitFor([&conn] {
    auto status = conn.Execute("CREATE TABLE t (key INT PRIMARY KEY)");
    WARN_NOT_OK(status, "Failed to create table");
    return status.ok();
  }, 15s, "Create table"));
}

class PgMiniBigPrefetchTest : public PgMiniSingleTServerTest {
 protected:
  void SetUp() override {
    FLAGS_ysql_prefetch_limit = 20000000;
    PgMiniTest::SetUp();
  }

  void Run(int rows, int block_size, int reads, bool compact = false) {
    auto conn = ASSERT_RESULT(Connect());

    ASSERT_OK(conn.Execute("CREATE TABLE t (a int PRIMARY KEY) SPLIT INTO 1 TABLETS"));
    auto last_row = 0;
    while (last_row < rows) {
      auto first_row = last_row + 1;
      last_row = std::min(rows, last_row + block_size);
      ASSERT_OK(conn.ExecuteFormat(
          "INSERT INTO t SELECT generate_series($0, $1)", first_row, last_row));
    }

    auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
    for (const auto& peer : peers) {
      auto tp = peer->tablet()->transaction_participant();
      if (tp) {
        LOG(INFO) << peer->LogPrefix() << "Intents: " << tp->TEST_CountIntents().first;
      }
    }

    if (compact) {
      FlushAndCompactTablets();
    }

    LOG(INFO) << "Perform read";

    if (VLOG_IS_ON(4)) {
      google::SetVLOGLevel("intent_aware_iterator", 4);
      google::SetVLOGLevel("docdb_rocksdb_util", 4);
      google::SetVLOGLevel("docdb", 4);
    }

    for (int i = 0; i != reads; ++i) {
      auto start = MonoTime::Now();
      auto fetched_rows = ASSERT_RESULT(conn.FetchValue<int64_t>("SELECT count(*) FROM t"));
      auto finish = MonoTime::Now();
      ASSERT_EQ(rows, fetched_rows);
      LOG(INFO) << i << ") Full Time: " << finish - start;
    }
  }
};

TEST_F_EX(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(BigRead), PgMiniBigPrefetchTest) {
  constexpr int kRows = RegularBuildVsSanitizers(1000000, 10000);
  constexpr int kBlockSize = 1000;
  constexpr int kReads = 3;

  Run(kRows, kBlockSize, kReads);
}

TEST_F_EX(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(BigReadWithCompaction), PgMiniBigPrefetchTest) {
  constexpr int kRows = RegularBuildVsSanitizers(1000000, 10000);
  constexpr int kBlockSize = 1000;
  constexpr int kReads = 3;

  Run(kRows, kBlockSize, kReads, /* compact= */ true);
}

TEST_F_EX(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(SmallRead), PgMiniBigPrefetchTest) {
  constexpr int kRows = 10;
  constexpr int kBlockSize = kRows;
  constexpr int kReads = 1;

  Run(kRows, kBlockSize, kReads);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DDLWithRestart)) {
  SetAtomicFlag(1.0, &FLAGS_TEST_transaction_ignore_applying_probability_in_tests);
  FLAGS_TEST_force_master_leader_resolution = true;

  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.StartTransaction(IsolationLevel::SERIALIZABLE_ISOLATION));
  ASSERT_OK(conn.Execute("CREATE TABLE t (a int PRIMARY KEY)"));
  ASSERT_OK(conn.CommitTransaction());

  ShutdownAllMasters(cluster_.get());

  LOG(INFO) << "Start masters";
  ASSERT_OK(StartAllMasters(cluster_.get()));

  auto res = ASSERT_RESULT(conn.FetchValue<int64_t>("SELECT COUNT(*) FROM t"));
  ASSERT_EQ(res, 0);
}

class PgMiniRocksDbIteratorLoggingTest : public PgMiniSingleTServerTest {
 public:
  struct IteratorLoggingTestConfig {
    int num_non_pk_columns;
    int num_rows;
    int num_overwrites;
    int first_row_to_scan;
    int last_row_to_scan;
  };

  void RunIteratorLoggingTest(const IteratorLoggingTestConfig& config) {
    auto conn = ASSERT_RESULT(Connect());

    std::string non_pk_columns_schema;
    std::string non_pk_column_names;
    for (int i = 0; i < config.num_non_pk_columns; ++i) {
      non_pk_columns_schema += Format(", $0 TEXT", GetNonPkColName(i));
      non_pk_column_names += Format(", $0", GetNonPkColName(i));
    }
    ASSERT_OK(conn.ExecuteFormat("CREATE TABLE t (pk TEXT, PRIMARY KEY (pk ASC)$0)",
                                 non_pk_columns_schema));
    // Delete and overwrite every row multiple times.
    for (int overwrite_index = 0; overwrite_index < config.num_overwrites; ++overwrite_index) {
      for (int row_index = 0; row_index < config.num_rows; ++row_index) {
        string non_pk_values;
        for (int non_pk_col_index = 0;
             non_pk_col_index < config.num_non_pk_columns;
             ++non_pk_col_index) {
          non_pk_values += Format(", '$0'", GetNonPkColValue(
              non_pk_col_index, row_index, overwrite_index));
        }

        const auto pk_value = GetPkForRow(row_index);
        ASSERT_OK(conn.ExecuteFormat(
            "INSERT INTO t(pk$0) VALUES('$1'$2)", non_pk_column_names, pk_value, non_pk_values));
        if (overwrite_index != config.num_overwrites - 1) {
          ASSERT_OK(conn.ExecuteFormat("DELETE FROM t WHERE pk = '$0'", pk_value));
        }
      }
    }
    const auto first_pk_to_scan = GetPkForRow(config.first_row_to_scan);
    const auto last_pk_to_scan = GetPkForRow(config.last_row_to_scan);
    auto count_stmt_str = Format(
        "SELECT COUNT(*) FROM t WHERE pk >= '$0' AND pk <= '$1'",
        first_pk_to_scan,
        last_pk_to_scan);
    // Do the same scan twice, and only turn on iterator logging on the second scan.
    // This way we won't be logging system table operations needed to fetch PostgreSQL metadata.
    for (bool is_warmup : {true, false}) {
      if (!is_warmup) {
        SetAtomicFlag(true, &FLAGS_rocksdb_use_logging_iterator);
      }
      auto count_result = ASSERT_RESULT(conn.Fetch(count_stmt_str));
      ASSERT_EQ(PQntuples(count_result.get()), 1);

      auto actual_num_rows = ASSERT_RESULT(GetInt64(count_result.get(), 0, 0));
      const int expected_num_rows = config.last_row_to_scan - config.first_row_to_scan + 1;
      ASSERT_EQ(expected_num_rows, actual_num_rows);
    }
    SetAtomicFlag(false, &FLAGS_rocksdb_use_logging_iterator);
  }

 private:
  std::string GetNonPkColName(int non_pk_col_index) {
    return Format("non_pk_col$0", non_pk_col_index);
  }

  std::string GetPkForRow(int row_index) {
    return Format("PrimaryKeyForRow$0", row_index);
  }

  std::string GetNonPkColValue(int non_pk_col_index, int row_index, int overwrite_index) {
    return Format("NonPkCol$0ValueForRow$1Overwrite$2",
                  non_pk_col_index, row_index, overwrite_index);
  }
};

TEST_F_EX(PgMiniTest,
          YB_DISABLE_TEST_IN_TSAN(IteratorLogPkOnly), PgMiniRocksDbIteratorLoggingTest) {
  RunIteratorLoggingTest({
    .num_non_pk_columns = 0,
    .num_rows = 5,
    .num_overwrites = 100,
    .first_row_to_scan = 1,  // 0-based
    .last_row_to_scan = 3,
  });
}

TEST_F_EX(PgMiniTest,
          YB_DISABLE_TEST_IN_TSAN(IteratorLogTwoNonPkCols), PgMiniRocksDbIteratorLoggingTest) {
  RunIteratorLoggingTest({
    .num_non_pk_columns = 2,
    .num_rows = 5,
    .num_overwrites = 100,
    .first_row_to_scan = 1,  // 0-based
    .last_row_to_scan = 3,
  });
}

// ------------------------------------------------------------------------------------------------
// Backward scan on an index
// ------------------------------------------------------------------------------------------------

class PgMiniBackwardIndexScanTest : public PgMiniSingleTServerTest {
 protected:
  void BackwardIndexScanTest(bool uncommitted_intents) {
    auto conn = ASSERT_RESULT(Connect());

    ASSERT_OK(conn.Execute(R"#(
        create table events_backwardscan (

          log       text not null,
          src       text not null,
          inserted  timestamp(3) without time zone not null,
          created   timestamp(3) without time zone not null,
          data      jsonb not null,

          primary key (log, src, created)
        );
      )#"));
    ASSERT_OK(conn.Execute("create index on events_backwardscan (inserted asc);"));

    ASSERT_OK(conn.Execute(R"#(
        insert into events_backwardscan

        select
          'log',
          'src',
          t,
          t,
          '{}'

        from generate_series(
          timestamp '2020-01-01',
          timestamp '2020-02-01',
          interval  '1 minute'
        )

        as t(day);
    )#"));

    boost::optional<PGConn> uncommitted_intents_conn;
    if (uncommitted_intents) {
      uncommitted_intents_conn = ASSERT_RESULT(Connect());
      ASSERT_OK(uncommitted_intents_conn->Execute("BEGIN"));
      auto ts = "1970-01-01 00:00:00";
      ASSERT_OK(uncommitted_intents_conn->ExecuteFormat(
          "insert into events_backwardscan values ('log', 'src', '$0', '$0', '{}')", ts, ts));
    }

    auto count = ASSERT_RESULT(
        conn.FetchValue<int64_t>("SELECT COUNT(*) FROM events_backwardscan"));
    LOG(INFO) << "Total rows inserted: " << count;

    auto select_result = ASSERT_RESULT(conn.Fetch(
        "select * from events_backwardscan order by inserted desc limit 100"
    ));
    ASSERT_EQ(PQntuples(select_result.get()), 100);

    if (uncommitted_intents) {
      ASSERT_OK(uncommitted_intents_conn->Execute("ROLLBACK"));
    }
  }
};

TEST_F_EX(PgMiniTest,
          YB_DISABLE_TEST_IN_TSAN(BackwardIndexScanNoIntents),
          PgMiniBackwardIndexScanTest) {
  BackwardIndexScanTest(/* uncommitted_intents */ false);
}

TEST_F_EX(PgMiniTest,
          YB_DISABLE_TEST_IN_TSAN(BackwardIndexScanWithIntents),
          PgMiniBackwardIndexScanTest) {
  BackwardIndexScanTest(/* uncommitted_intents */ true);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(CreateDatabase)) {
  FLAGS_flush_rocksdb_on_shutdown = false;
  auto conn = ASSERT_RESULT(Connect());
  const std::string kDatabaseName = "testdb";
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", kDatabaseName));
  ASSERT_OK(cluster_->RestartSync());
}

void PgMiniTest::TestBigInsert(bool restart) {
  constexpr int64_t kNumRows = RegularBuildVsSanitizers(100000, 10000);
  FLAGS_txn_max_apply_batch_records = kNumRows / 10;

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (a int PRIMARY KEY) SPLIT INTO 1 TABLETS"));
  ASSERT_OK(conn.Execute("INSERT INTO t VALUES (0)"));

  TestThreadHolder thread_holder;

  std::atomic<int> post_insert_reads{0};
  thread_holder.AddThreadFunctor([this, &stop = thread_holder.stop_flag(), &post_insert_reads] {
    auto conn = ASSERT_RESULT(Connect());
    while (!stop.load(std::memory_order_acquire)) {
      auto res = ASSERT_RESULT(conn.FetchValue<int64_t>("SELECT SUM(a) FROM t"));

      // We should see zero or full sum only.
      if (res) {
        ASSERT_EQ(res, kNumRows * (kNumRows + 1) / 2);
        ++post_insert_reads;
      }
    }
  });

  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO t SELECT generate_series(1, $0)", kNumRows));

  if (restart) {
    LOG(INFO) << "Restart cluster";
    ASSERT_OK(cluster_->RestartSync());
  }

  ASSERT_OK(WaitFor([this] {
    auto intents_count = CountIntents(cluster_.get());
    LOG(INFO) << "Intents count: " << intents_count;

    return intents_count == 0;
  }, 60s * kTimeMultiplier, "Intents cleanup", 200ms));

  thread_holder.Stop();

  ASSERT_GT(post_insert_reads.load(std::memory_order_acquire), 0);

  FlushAndCompactTablets();

  auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
  for (const auto& peer : peers) {
    auto db = peer->tablet()->TEST_db();
    if (!db) {
      continue;
    }
    rocksdb::ReadOptions read_opts;
    read_opts.query_id = rocksdb::kDefaultQueryId;
    std::unique_ptr<rocksdb::Iterator> iter(db->NewIterator(read_opts));

    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      Slice key = iter->key();
      ASSERT_FALSE(key.TryConsumeByte(docdb::ValueTypeAsChar::kTransactionApplyState))
          << "Key: " << iter->key().ToDebugString() << ", value: " << iter->value().ToDebugString();
    }
  }
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(BigInsert)) {
  TestBigInsert(/* restart= */ false);
}

TEST_F(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(BigInsertWithRestart)) {
  FLAGS_apply_intents_task_injected_delay_ms = 200;
  TestBigInsert(/* restart= */ true);
}

} // namespace pgwrapper
} // namespace yb
