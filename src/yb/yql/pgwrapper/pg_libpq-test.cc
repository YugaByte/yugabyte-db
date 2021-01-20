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

#include <thread>

#include "yb/gutil/strings/join.h"
#include "yb/util/backoff_waiter.h"
#include "yb/util/barrier.h"
#include "yb/util/monotime.h"
#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"
#include "yb/util/stol_utils.h"

#include "yb/yql/pgwrapper/libpq_test_base.h"
#include "yb/yql/pgwrapper/libpq_utils.h"

#include "yb/client/client_fwd.h"
#include "yb/common/common.pb.h"
#include "yb/common/pgsql_error.h"
#include "yb/master/catalog_manager.h"
#include "yb/tserver/tserver.pb.h"

using namespace std::literals;

DECLARE_int64(external_mini_cluster_max_log_bytes);

METRIC_DECLARE_entity(tablet);
METRIC_DECLARE_counter(transaction_not_found);

METRIC_DECLARE_entity(server);
METRIC_DECLARE_counter(rpc_inbound_calls_created);

namespace yb {
namespace pgwrapper {

class PgLibPqTest : public LibPqTestBase {
 protected:
  void TestUriAuth();

  void TestMultiBankAccount(IsolationLevel isolation);

  void DoIncrement(int key, int num_increments, IsolationLevel isolation);

  void TestParallelCounter(IsolationLevel isolation);

  void TestConcurrentCounter(IsolationLevel isolation);

  void TestOnConflict(bool kill_master, const MonoDelta& duration);

  void TestCacheRefreshRetry(const bool is_retry_disabled);

  const std::vector<std::string> names{
    "uppercase:P",
    "space: ",
    "symbol:#",
    "single_quote:'",
    "double_quote:\"",
    "backslash:\\",
    "mixed:P #'\"\\",
  };
};

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(Simple)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT, value TEXT)"));
  ASSERT_OK(conn.Execute("INSERT INTO t (key, value) VALUES (1, 'hello')"));

  auto res = ASSERT_RESULT(conn.Fetch("SELECT * FROM t"));

  {
    auto lines = PQntuples(res.get());
    ASSERT_EQ(1, lines);

    auto columns = PQnfields(res.get());
    ASSERT_EQ(2, columns);

    auto key = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(key, 1);
    auto value = ASSERT_RESULT(GetString(res.get(), 0, 1));
    ASSERT_EQ(value, "hello");
  }
}

// Test libpq connection to various database names.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(DatabaseNames)) {
  PGConn conn = ASSERT_RESULT(Connect());

  for (const std::string& db_name : names) {
    ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", PqEscapeIdentifier(db_name)));
    ASSERT_OK(ConnectToDB(db_name));
  }
}

// Test libpq connection to various user names.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(UserNames)) {
  PGConn conn = ASSERT_RESULT(Connect());

  for (const std::string& user_name : names) {
    ASSERT_OK(conn.ExecuteFormat("CREATE USER $0", PqEscapeIdentifier(user_name)));
    ASSERT_OK(ConnectToDBAsUser("" /* db_name */, user_name));
  }
}

// Test libpq connection using URI connection string.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(Uri)) {
  const std::string& host = pg_ts->bind_host();
  const uint16_t port = pg_ts->pgsql_rpc_port();
  {
    const std::string& conn_str = Format("postgres://yugabyte@$0:$1", host, port);
    LOG(INFO) << "Connecting using string: " << conn_str;
    PGConn conn = ASSERT_RESULT(ConnectUsingString(conn_str));
    {
      auto res = ASSERT_RESULT(conn.Fetch("select current_database()"));
      auto answer = ASSERT_RESULT(GetString(res.get(), 0, 0));
      ASSERT_EQ(answer, "yugabyte");
    }
    {
      auto res = ASSERT_RESULT(conn.Fetch("select current_user"));
      auto answer = ASSERT_RESULT(GetString(res.get(), 0, 0));
      ASSERT_EQ(answer, "yugabyte");
    }
    {
      auto res = ASSERT_RESULT(conn.Fetch("show listen_addresses"));
      auto answer = ASSERT_RESULT(GetString(res.get(), 0, 0));
      ASSERT_EQ(answer, host);
    }
    {
      auto res = ASSERT_RESULT(conn.Fetch("show port"));
      auto answer = ASSERT_RESULT(GetString(res.get(), 0, 0));
      ASSERT_EQ(answer, std::to_string(port));
    }
  }
  // Supply database name.
  {
    const std::string& conn_str = Format("postgres://yugabyte@$0:$1/template1", host, port);
    LOG(INFO) << "Connecting using string: " << conn_str;
    PGConn conn = ASSERT_RESULT(ConnectUsingString(conn_str));
    {
      auto res = ASSERT_RESULT(conn.Fetch("select current_database()"));
      auto answer = ASSERT_RESULT(GetString(res.get(), 0, 0));
      ASSERT_EQ(answer, "template1");
    }
  }
  // Supply an incorrect password.  Since HBA config gives the yugabyte user trust access, postgres
  // won't request a password, our client won't send this password, and the authentication should
  // succeed.
  {
    const std::string& conn_str = Format("postgres://yugabyte:monkey123@$0:$1", host, port);
    LOG(INFO) << "Connecting using string: " << conn_str;
    ASSERT_OK(ConnectUsingString(conn_str));
  }
}

void PgLibPqTest::TestUriAuth() {
  const std::string& host = pg_ts->bind_host();
  const uint16_t port = pg_ts->pgsql_rpc_port();
  // Don't supply password.
  {
    const std::string& conn_str = Format("postgres://yugabyte@$0:$1", host, port);
    LOG(INFO) << "Connecting using string: " << conn_str;
    Result<PGConn> result = ConnectUsingString(
        conn_str,
        CoarseMonoClock::Now() + 2s /* deadline */);
    ASSERT_NOK(result);
    ASSERT_TRUE(result.status().IsNetworkError());
    ASSERT_TRUE(result.status().message().ToBuffer().find("Connect failed") != std::string::npos)
        << result.status();
  }
  // Supply an incorrect password.
  {
    const std::string& conn_str = Format("postgres://yugabyte:monkey123@$0:$1", host, port);
    LOG(INFO) << "Connecting using string: " << conn_str;
    Result<PGConn> result = ConnectUsingString(
        conn_str,
        CoarseMonoClock::Now() + 2s /* deadline */);
    ASSERT_NOK(result);
    ASSERT_TRUE(result.status().IsNetworkError());
    ASSERT_TRUE(result.status().message().ToBuffer().find("Connect failed") != std::string::npos)
        << result.status();
  }
  // Supply the correct password.
  {
    const std::string& conn_str = Format("postgres://yugabyte:yugabyte@$0:$1", host, port);
    LOG(INFO) << "Connecting using string: " << conn_str;
    ASSERT_OK(ConnectUsingString(conn_str));
  }
}

// Enable authentication using password.  This scheme requests the plain password.  You may still
// use SSL for encryption on the wire.
class PgLibPqTestAuthPassword : public PgLibPqTest {
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->extra_tserver_flags.push_back("--ysql_hba_conf_csv=host all all samehost password");
  }
};

TEST_F_EX(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(UriPassword), PgLibPqTestAuthPassword) {
  TestUriAuth();
}

// Enable authentication using md5.  This scheme is a challenge and response, so the plain password
// isn't sent.
class PgLibPqTestAuthMd5 : public PgLibPqTest {
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->extra_tserver_flags.push_back("--ysql_hba_conf_csv=host all all samehost md5");
  }
};

TEST_F_EX(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(UriMd5), PgLibPqTestAuthMd5) {
  TestUriAuth();
}

// Test that repeats example from this article:
// https://blogs.msdn.microsoft.com/craigfr/2007/05/16/serializable-vs-snapshot-isolation-level/
//
// Multiple rows with values 0 and 1 are stored in table.
// Two concurrent transaction fetches all rows from table and does the following.
// First transaction changes value of all rows with value 0 to 1.
// Second transaction changes value of all rows with value 1 to 0.
// As outcome we should have rows with the same value.
//
// The described prodecure is repeated multiple times to increase probability of catching bug,
// w/o running test multiple times.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(SerializableColoring)) {
  static const std::string kTryAgain = "Try again.";
  constexpr auto kKeys = RegularBuildVsSanitizers(10, 20);
  constexpr auto kColors = 2;
  constexpr auto kIterations = 20;

  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY, color INT)"));

  auto iterations_left = kIterations;

  for (int iteration = 0; iterations_left > 0; ++iteration) {
    auto iteration_title = Format("Iteration: $0", iteration);
    SCOPED_TRACE(iteration_title);
    LOG(INFO) << iteration_title;

    auto status = conn.Execute("DELETE FROM t");
    if (!status.ok()) {
      ASSERT_STR_CONTAINS(status.ToString(), kTryAgain);
      continue;
    }
    for (int k = 0; k != kKeys; ++k) {
      int32_t color = RandomUniformInt(0, kColors - 1);
      ASSERT_OK(conn.ExecuteFormat("INSERT INTO t (key, color) VALUES ($0, $1)", k, color));
    }

    std::atomic<int> complete{ 0 };
    std::vector<std::thread> threads;
    for (int i = 0; i != kColors; ++i) {
      int32_t color = i;
      threads.emplace_back([this, color, kKeys, &complete] {
        auto conn = ASSERT_RESULT(Connect());

        ASSERT_OK(conn.Execute("BEGIN"));
        ASSERT_OK(conn.Execute("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"));

        auto res = conn.Fetch("SELECT * FROM t");
        if (!res.ok()) {
          auto msg = res.status().message().ToBuffer();
          ASSERT_STR_CONTAINS(res.status().ToString(), kTryAgain);
          return;
        }
        auto columns = PQnfields(res->get());
        ASSERT_EQ(2, columns);

        auto lines = PQntuples(res->get());
        ASSERT_EQ(kKeys, lines);
        for (int i = 0; i != lines; ++i) {
          if (ASSERT_RESULT(GetInt32(res->get(), i, 1)) == color) {
            continue;
          }

          auto key = ASSERT_RESULT(GetInt32(res->get(), i, 0));
          auto status = conn.ExecuteFormat("UPDATE t SET color = $1 WHERE key = $0", key, color);
          if (!status.ok()) {
            auto msg = status.message().ToBuffer();
            // Missing metadata means that transaction was aborted and cleaned.
            ASSERT_TRUE(msg.find("Try again.") != std::string::npos ||
                        msg.find("Missing metadata") != std::string::npos) << status;
            break;
          }
        }

        auto status = conn.Execute("COMMIT");
        if (!status.ok()) {
          auto msg = status.message().ToBuffer();
          ASSERT_TRUE(msg.find("Operation expired") != std::string::npos) << status;
          return;
        }

        ++complete;
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    if (complete == 0) {
      continue;
    }

    auto res = ASSERT_RESULT(conn.Fetch("SELECT * FROM t"));
    auto columns = PQnfields(res.get());
    ASSERT_EQ(2, columns);

    auto lines = PQntuples(res.get());
    ASSERT_EQ(kKeys, lines);

    std::vector<int32_t> zeroes, ones;
    for (int i = 0; i != lines; ++i) {
      auto key = ASSERT_RESULT(GetInt32(res.get(), i, 0));
      auto current = ASSERT_RESULT(GetInt32(res.get(), i, 1));
      if (current == 0) {
        zeroes.push_back(key);
      } else {
        ones.push_back(key);
      }
    }

    std::sort(ones.begin(), ones.end());
    std::sort(zeroes.begin(), zeroes.end());

    LOG(INFO) << "Zeroes: " << yb::ToString(zeroes) << ", ones: " << yb::ToString(ones);
    ASSERT_TRUE(zeroes.empty() || ones.empty());

    --iterations_left;
  }
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(SerializableReadWriteConflict)) {
  const auto kKeys = RegularBuildVsSanitizers(20, 5);
  const auto kNumTries = RegularBuildVsSanitizers(4, 1);
  auto tries = 1;
  for (; tries <= kNumTries; ++tries) {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_OK(conn.Execute("DROP TABLE IF EXISTS t"));
    ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY)"));

    size_t reads_won = 0, writes_won = 0;
    for (int i = 0; i != kKeys; ++i) {
      auto read_conn = ASSERT_RESULT(Connect());
      ASSERT_OK(read_conn.Execute("BEGIN ISOLATION LEVEL SERIALIZABLE"));
      auto res = read_conn.FetchFormat("SELECT * FROM t WHERE key = $0", i);
      auto read_status = ResultToStatus(res);

      auto write_conn = ASSERT_RESULT(Connect());
      ASSERT_OK(write_conn.Execute("BEGIN ISOLATION LEVEL SERIALIZABLE"));
      auto write_status = write_conn.ExecuteFormat("INSERT INTO t (key) VALUES ($0)", i);

      std::thread read_commit_thread([&read_conn, &read_status] {
        if (read_status.ok()) {
          read_status = read_conn.Execute("COMMIT");
        }
      });

      std::thread write_commit_thread([&write_conn, &write_status] {
        if (write_status.ok()) {
          write_status = write_conn.Execute("COMMIT");
        }
      });

      read_commit_thread.join();
      write_commit_thread.join();

      LOG(INFO) << "Read: " << read_status << ", write: " << write_status;

      if (!read_status.ok()) {
        ASSERT_OK(write_status);
        ++writes_won;
      } else {
        ASSERT_NOK(write_status);
        ++reads_won;
      }
    }

    LOG(INFO) << "Reads won: " << reads_won << ", writes won: " << writes_won
              << " (" << tries << "/" << kNumTries << ")";
    // always pass for TSAN, we're just looking for memory issues
    if (RegularBuildVsSanitizers(false, true)) {
      break;
    }
    // break (succeed) if we hit 25% on our "coin toss" transaction conflict above
    if (reads_won >= kKeys / 4 && writes_won >= kKeys / 4) {
      break;
    }
    // otherwise, retry and see if this is consistent behavior
  }
  ASSERT_LE(tries, kNumTries);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(ReadRestart)) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY)"));

  std::atomic<bool> stop(false);
  std::atomic<int> last_written(0);

  std::thread write_thread([this, &stop, &last_written] {
    auto write_conn = ASSERT_RESULT(Connect());
    int write_key = 1;
    while (!stop.load(std::memory_order_acquire)) {
      SCOPED_TRACE(Format("Writing: $0", write_key));

      ASSERT_OK(write_conn.Execute("BEGIN"));
      auto status = write_conn.ExecuteFormat("INSERT INTO t (key) VALUES ($0)", write_key);
      if (status.ok()) {
        status = write_conn.Execute("COMMIT");
      }
      if (status.ok()) {
        last_written.store(write_key, std::memory_order_release);
        ++write_key;
      } else {
        LOG(INFO) << "Write " << write_key << " failed: " << status;
      }
    }
  });

  auto se = ScopeExit([&stop, &write_thread] {
    stop.store(true, std::memory_order_release);
    write_thread.join();
  });

  auto deadline = CoarseMonoClock::now() + 30s;

  while (CoarseMonoClock::now() < deadline) {
    int read_key = last_written.load(std::memory_order_acquire);
    if (read_key == 0) {
      std::this_thread::sleep_for(100ms);
      continue;
    }

    SCOPED_TRACE(Format("Reading: $0", read_key));

    ASSERT_OK(conn.Execute("BEGIN"));

    auto res = ASSERT_RESULT(conn.FetchFormat("SELECT * FROM t WHERE key = $0", read_key));
    auto columns = PQnfields(res.get());
    ASSERT_EQ(1, columns);

    auto lines = PQntuples(res.get());
    ASSERT_EQ(1, lines);

    auto key = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(key, read_key);

    ASSERT_OK(conn.Execute("ROLLBACK"));
  }

  ASSERT_GE(last_written.load(std::memory_order_acquire), 100);
}

// Concurrently insert records into tables with foreign key relationship while truncating both.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(ConcurrentInsertTruncateForeignKey)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("DROP TABLE IF EXISTS t2"));
  ASSERT_OK(conn.Execute("DROP TABLE IF EXISTS t1"));
  ASSERT_OK(conn.Execute("CREATE TABLE t1 (k int primary key, v int)"));
  ASSERT_OK(conn.Execute(
        "CREATE TABLE t2 (k int primary key, t1_k int, FOREIGN KEY (t1_k) REFERENCES t1 (k))"));

  const int kMaxKeys = 1 << 20;

  constexpr auto kWriteThreads = 4;
  constexpr auto kTruncateThreads = 2;

  TestThreadHolder thread_holder;
  for (int i = 0; i != kWriteThreads; ++i) {
    thread_holder.AddThreadFunctor([this, &stop = thread_holder.stop_flag()] {
      auto write_conn = ASSERT_RESULT(Connect());
      while (!stop.load(std::memory_order_acquire)) {
        int t1_k = RandomUniformInt(0, kMaxKeys - 1);
        int t1_v = RandomUniformInt(0, kMaxKeys - 1);
        auto status = write_conn.ExecuteFormat("INSERT INTO t1 VALUES ($0, $1)", t1_k, t1_v);
        int t2_k = RandomUniformInt(0, kMaxKeys - 1);
        status = write_conn.ExecuteFormat("INSERT INTO t2 VALUES ($0, $1)", t2_k, t1_k);
      }
    });
  }

  for (int i = 0; i != kTruncateThreads; ++i) {
    thread_holder.AddThreadFunctor([this, &stop = thread_holder.stop_flag()] {
      auto truncate_conn = ASSERT_RESULT(Connect());
      int idx = 0;
      while (!stop.load(std::memory_order_acquire)) {
        auto status = truncate_conn.Execute("TRUNCATE TABLE t1, t2 CASCADE");
        ++idx;
        std::this_thread::sleep_for(100ms);
      }
    });
  }

  thread_holder.WaitAndStop(30s);
}

// Concurrently insert records to table with index.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(ConcurrentIndexInsert)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute(
      "CREATE TABLE IF NOT EXISTS users(id text, ename text, age int, PRIMARY KEY(id))"));

  ASSERT_OK(conn.Execute(
      "CREATE INDEX IF NOT EXISTS name_idx ON users(ename)"));

  constexpr auto kWriteThreads = 4;

  std::atomic<bool> stop(false);
  std::vector<std::thread> write_threads;

  while (write_threads.size() != kWriteThreads) {
    write_threads.emplace_back([this, &stop] {
      auto write_conn = ASSERT_RESULT(Connect());
      auto this_thread_id = std::this_thread::get_id();
      auto tid = std::hash<decltype(this_thread_id)>()(this_thread_id);
      int idx = 0;
      while (!stop.load(std::memory_order_acquire)) {
        ASSERT_OK(write_conn.ExecuteFormat(
            "INSERT INTO users (id, ename, age) VALUES ('user-$0-$1', 'name-$1', $2)",
            tid, idx, 20 + (idx % 50)));
        ++idx;
      }
    });
  }

  auto se = ScopeExit([&stop, &write_threads] {
    stop.store(true, std::memory_order_release);
    for (auto& thread : write_threads) {
      thread.join();
    }
  });

  std::this_thread::sleep_for(30s);
}

Result<int64_t> ReadSumBalance(
    PGConn* conn, int accounts, IsolationLevel isolation,
    std::atomic<int>* counter) {
  RETURN_NOT_OK(conn->StartTransaction(isolation));
  bool failed = true;
  auto se = ScopeExit([conn, &failed] {
    if (failed) {
      EXPECT_OK(conn->Execute("ROLLBACK"));
    }
  });

  std::string query = "";
  for (int i = 1; i <= accounts; ++i) {
    if (!query.empty()) {
      query += " UNION ";
    }
    query += Format("SELECT balance, id FROM account_$0 WHERE id = $0", i);
  }

  auto res = VERIFY_RESULT(conn->FetchMatrix(query, accounts, 2));
  int64_t sum = 0;
  for (int i = 0; i != accounts; ++i) {
    sum += VERIFY_RESULT(GetValue<int64_t>(res.get(), i, 0));
  }

  failed = false;
  RETURN_NOT_OK(conn->Execute("COMMIT"));
  return sum;
}

void PgLibPqTest::TestMultiBankAccount(IsolationLevel isolation) {
  constexpr int kAccounts = RegularBuildVsSanitizers(20, 10);
  constexpr int64_t kInitialBalance = 100;

#ifndef NDEBUG
  const auto kTimeout = 180s;
  constexpr int kThreads = RegularBuildVsSanitizers(12, 5);
#else
  const auto kTimeout = 60s;
  constexpr int kThreads = 5;
#endif

  PGConn conn = ASSERT_RESULT(Connect());
  std::vector<PGConn> thread_connections;
  for (int i = 0; i < kThreads; ++i) {
    thread_connections.push_back(ASSERT_RESULT(Connect()));
  }

  for (int i = 1; i <= kAccounts; ++i) {
    ASSERT_OK(conn.ExecuteFormat(
        "CREATE TABLE account_$0 (id int, balance bigint, PRIMARY KEY(id))", i));
    ASSERT_OK(conn.ExecuteFormat(
        "INSERT INTO account_$0 (id, balance) VALUES ($0, $1)", i, kInitialBalance));
  }

  std::atomic<int> writes(0);
  std::atomic<int> reads(0);

  constexpr auto kRequiredReads = RegularBuildVsSanitizers(5, 2);
  constexpr auto kRequiredWrites = RegularBuildVsSanitizers(1000, 500);

  std::atomic<int> counter(100000);
  TestThreadHolder thread_holder;
  for (int i = 1; i <= kThreads; ++i) {
    thread_holder.AddThreadFunctor(
        [&conn = thread_connections[i - 1], &writes, &isolation,
         &stop_flag = thread_holder.stop_flag()]() {
      while (!stop_flag.load(std::memory_order_acquire)) {
        int from = RandomUniformInt(1, kAccounts);
        int to = RandomUniformInt(1, kAccounts - 1);
        if (to >= from) {
          ++to;
        }
        int64_t amount = RandomUniformInt(1, 10);
        ASSERT_OK(conn.StartTransaction(isolation));
        auto status = conn.ExecuteFormat(
              "UPDATE account_$0 SET balance = balance - $1 WHERE id = $0", from, amount);
        if (status.ok()) {
          status = conn.ExecuteFormat(
              "UPDATE account_$0 SET balance = balance + $1 WHERE id = $0", to, amount);
        }
        if (status.ok()) {
          status = conn.Execute("COMMIT;");
        } else {
          ASSERT_OK(conn.Execute("ROLLBACK;"));
        }
        if (!status.ok()) {
          ASSERT_TRUE(TransactionalFailure(status)) << status;
        } else {
          LOG(INFO) << "Updated: " << from << " => " << to << " by " << amount;
          ++writes;
        }
      }
    });
  }

  thread_holder.AddThreadFunctor(
      [this, &counter, &reads, &writes, isolation, &stop_flag = thread_holder.stop_flag()]() {
    SetFlagOnExit set_flag_on_exit(&stop_flag);
    auto conn = ASSERT_RESULT(Connect());
    auto failures_in_row = 0;
    while (!stop_flag.load(std::memory_order_acquire)) {
      if (isolation == IsolationLevel::SERIALIZABLE_ISOLATION) {
        auto lower_bound = reads.load() * kRequiredWrites < writes.load() * kRequiredReads
            ? 1.0 - 1.0 / (1ULL << failures_in_row) : 0.0;
        ASSERT_OK(conn.ExecuteFormat("SET yb_transaction_priority_lower_bound = $0", lower_bound));
      }
      auto sum = ReadSumBalance(&conn, kAccounts, isolation, &counter);
      if (!sum.ok()) {
        // Do not overflow long when doing bitshift above.
        failures_in_row = std::min(failures_in_row + 1, 63);
        ASSERT_TRUE(TransactionalFailure(sum.status())) << sum.status();
      } else {
        failures_in_row = 0;
        ASSERT_EQ(*sum, kAccounts * kInitialBalance);
        ++reads;
      }
    }
  });

  auto wait_status = WaitFor([&reads, &writes, &stop = thread_holder.stop_flag()] {
    return stop.load() || (writes.load() >= kRequiredWrites && reads.load() >= kRequiredReads);
  }, kTimeout, Format("At least $0 reads and $1 writes", kRequiredReads, kRequiredWrites));

  LOG(INFO) << "Writes: " << writes.load() << ", reads: " << reads.load();

  ASSERT_OK(wait_status);

  thread_holder.Stop();

  ASSERT_OK(WaitFor([&conn, isolation, &counter]() -> Result<bool> {
    auto sum = ReadSumBalance(&conn, kAccounts, isolation, &counter);
    if (!sum.ok()) {
      if (!TransactionalFailure(sum.status())) {
        return sum.status();
      }
      return false;
    }
    EXPECT_EQ(*sum, kAccounts * kInitialBalance);
    return true;
  }, 10s, "Final read"));

  auto total_not_found = 0;
  for (auto* tserver : cluster_->tserver_daemons()) {
    auto tablets = ASSERT_RESULT(cluster_->GetTabletIds(tserver));
    for (const auto& tablet : tablets) {
      auto result = tserver->GetInt64Metric(
          &METRIC_ENTITY_tablet, tablet.c_str(), &METRIC_transaction_not_found, "value");
      if (result.ok()) {
        total_not_found += *result;
      } else {
        ASSERT_TRUE(result.status().IsNotFound()) << result.status();
      }
    }
  }

  LOG(INFO) << "Total not found: " << total_not_found;
  // Check that total not found is not too big.
  ASSERT_LE(total_not_found, 200);
}

class PgLibPqSmallClockSkewTest : public PgLibPqTest {
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    // Use small clock skew, to decrease number of read restarts.
    options->extra_tserver_flags.push_back("--max_clock_skew_usec=5000");
  }
};

TEST_F_EX(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(MultiBankAccountSnapshot),
          PgLibPqSmallClockSkewTest) {
  TestMultiBankAccount(IsolationLevel::SNAPSHOT_ISOLATION);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(MultiBankAccountSerializable)) {
  TestMultiBankAccount(IsolationLevel::SERIALIZABLE_ISOLATION);
}

void PgLibPqTest::DoIncrement(int key, int num_increments, IsolationLevel isolation) {
  auto conn = ASSERT_RESULT(Connect());

  // Perform increments
  int succeeded_incs = 0;
  while (succeeded_incs < num_increments) {
    ASSERT_OK(conn.StartTransaction(isolation));
    bool committed = false;
    auto exec_status = conn.ExecuteFormat("UPDATE t SET value = value + 1 WHERE key = $0", key);
    if (exec_status.ok()) {
      auto commit_status = conn.Execute("COMMIT");
      if (commit_status.ok()) {
        succeeded_incs++;
        committed = true;
      }
    }
    if (!committed) {
      ASSERT_OK(conn.Execute("ROLLBACK"));
    }
  }
}

void PgLibPqTest::TestParallelCounter(IsolationLevel isolation) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT, value INT)"));

  const auto kThreads = RegularBuildVsSanitizers(3, 2);
  const auto kIncrements = RegularBuildVsSanitizers(100, 20);

  // Make a counter for each thread and have each thread increment it
  std::vector<std::thread> threads;
  while (threads.size() != kThreads) {
    int key = threads.size();
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO t (key, value) VALUES ($0, 0)", key));

    threads.emplace_back([this, key, isolation] {
      DoIncrement(key, kIncrements, isolation);
    });
  }

  // Wait for completion
  for (auto& thread : threads) {
    thread.join();
  }

  // Check each counter
  for (int i = 0; i < kThreads; i++) {
    auto res = ASSERT_RESULT(conn.FetchFormat("SELECT value FROM t WHERE key = $0", i));

    auto row_val = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(row_val, kIncrements);
  }
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestParallelCounterSerializable)) {
  TestParallelCounter(IsolationLevel::SERIALIZABLE_ISOLATION);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestParallelCounterRepeatableRead)) {
  TestParallelCounter(IsolationLevel::SNAPSHOT_ISOLATION);
}

void PgLibPqTest::TestConcurrentCounter(IsolationLevel isolation) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT, value INT)"));

  ASSERT_OK(conn.Execute("INSERT INTO t (key, value) VALUES (0, 0)"));

  const auto kThreads = RegularBuildVsSanitizers(3, 2);
  const auto kIncrements = RegularBuildVsSanitizers(100, 20);

  // Have each thread increment the same already-created counter
  std::vector<std::thread> threads;
  while (threads.size() != kThreads) {
    threads.emplace_back([this, isolation] {
      DoIncrement(0, kIncrements, isolation);
    });
  }

  // Wait for completion
  for (auto& thread : threads) {
    thread.join();
  }

  // Check that we incremented exactly the desired number of times
  auto res = ASSERT_RESULT(conn.Fetch("SELECT value FROM t WHERE key = 0"));

  auto row_val = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
  ASSERT_EQ(row_val, kThreads * kIncrements);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestConcurrentCounterSerializable)) {
  TestConcurrentCounter(IsolationLevel::SERIALIZABLE_ISOLATION);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestConcurrentCounterRepeatableRead)) {
  TestConcurrentCounter(IsolationLevel::SNAPSHOT_ISOLATION);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(SecondaryIndexInsertSelect)) {
  constexpr int kThreads = 4;

  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (a INT PRIMARY KEY, b INT)"));
  ASSERT_OK(conn.Execute("CREATE INDEX ON t (b, a)"));

  TestThreadHolder holder;
  std::array<std::atomic<int>, kThreads> written;
  for (auto& w : written) {
    w.store(0, std::memory_order_release);
  }

  for (int i = 0; i != kThreads; ++i) {
    holder.AddThread([this, i, &stop = holder.stop_flag(), &written] {
      auto conn = ASSERT_RESULT(Connect());
      SetFlagOnExit set_flag_on_exit(&stop);
      int key = 0;

      while (!stop.load(std::memory_order_acquire)) {
        if (RandomUniformBool()) {
          int a = i * 1000000 + key;
          int b = key;
          ASSERT_OK(conn.ExecuteFormat("INSERT INTO t (a, b) VALUES ($0, $1)", a, b));
          written[i].store(++key, std::memory_order_release);
        } else {
          int writer_index = RandomUniformInt(0, kThreads - 1);
          int num_written = written[writer_index].load(std::memory_order_acquire);
          if (num_written == 0) {
            continue;
          }
          int read_key = num_written - 1;
          int b = read_key;
          int read_a = ASSERT_RESULT(conn.FetchValue<int32_t>(
              Format("SELECT a FROM t WHERE b = $0 LIMIT 1", b)));
          ASSERT_EQ(read_a % 1000000, read_key);
        }
      }
    });
  }

  holder.WaitAndStop(60s);
}

void AssertRows(PGConn *conn, int expected_num_rows) {
  auto res = ASSERT_RESULT(conn->Fetch("SELECT * FROM test"));
  ASSERT_EQ(PQntuples(res.get()), expected_num_rows);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(InTxnDelete)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE test (pk int PRIMARY KEY)"));
  ASSERT_OK(conn.Execute("BEGIN"));
  ASSERT_OK(conn.Execute("INSERT INTO test VALUES (1)"));
  ASSERT_NO_FATALS(AssertRows(&conn, 1));
  ASSERT_OK(conn.Execute("DELETE FROM test"));
  ASSERT_NO_FATALS(AssertRows(&conn, 0));
  ASSERT_OK(conn.Execute("INSERT INTO test VALUES (1)"));
  ASSERT_NO_FATALS(AssertRows(&conn, 1));
  ASSERT_OK(conn.Execute("COMMIT"));

  ASSERT_NO_FATALS(AssertRows(&conn, 1));
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(CompoundKeyColumnOrder)) {
  const string table_name = "test";
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE $0 (r2 int, r1 int, h int, v2 int, v1 int, primary key (h, r1, r2))",
      table_name));
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  yb::client::YBSchema schema;
  PartitionSchema partition_schema;
  bool table_found = false;
  // TODO(dmitry): Find table by name instead of checking all the tables when catalog_mangager
  // will be able to find YSQL tables
  const auto tables = ASSERT_RESULT(client->ListTables());
  for (const auto& t : tables) {
    if (t.namespace_type() == YQLDatabase::YQL_DATABASE_PGSQL && t.table_name() == table_name) {
      table_found = true;
      ASSERT_OK(client->GetTableSchema(t, &schema, &partition_schema));
      const auto& columns = schema.columns();
      std::array<string, 5> expected_column_names{"h", "r1", "r2", "v2", "v1"};
      ASSERT_EQ(expected_column_names.size(), columns.size());
      for (size_t i = 0; i < expected_column_names.size(); ++i) {
        ASSERT_EQ(columns[i].name(), expected_column_names[i]);
      }
      break;
    }
  }
  ASSERT_TRUE(table_found);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(BulkCopy)) {
  const std::string kTableName = "customer";
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat(
      "CREATE TABLE CUSTOMER ( CUSTKEY     INTEGER NOT NULL PRIMARY KEY,\n"
      "                        NAME        VARCHAR(25) NOT NULL,\n"
      "                        ADDRESS     VARCHAR(40) NOT NULL,\n"
      "                        NATIONKEY   INTEGER NOT NULL,\n"
      "                        PHONE       CHAR(15) NOT NULL,\n"
      "                        MKTSEGMENT  CHAR(10) NOT NULL,\n"
      "                        COMMENT     VARCHAR(117) NOT NULL);",
      kTableName));

  constexpr int kNumBatches = 10;
  constexpr int kBatchSize = 1000;

  int customer_key = 0;
  for (int i = 0; i != kNumBatches; ++i) {
    ASSERT_OK(conn.CopyBegin(Format("COPY $0 FROM STDIN WITH BINARY", kTableName)));
    for (int j = 0; j != kBatchSize; ++j) {
      conn.CopyStartRow(7);
      conn.CopyPutInt32(++customer_key);
      conn.CopyPutString(Format("Name $0 $1", i, j));
      conn.CopyPutString(Format("Address $0 $1", i, j));
      conn.CopyPutInt32(i);
      conn.CopyPutString(std::to_string(999999876543210 + customer_key));
      conn.CopyPutString(std::to_string(9876543210 + customer_key));
      conn.CopyPutString(Format("Comment $0 $1", i, j));
    }

    ASSERT_OK(conn.CopyEnd());
  }

  LOG(INFO) << "Finished copy";
  for (;;) {
    auto result = conn.FetchFormat("SELECT COUNT(*) FROM $0", kTableName);
    if (result.ok()) {
      LogResult(result->get());
      auto count = ASSERT_RESULT(GetInt64(result->get(), 0, 0));
      LOG(INFO) << "Total count: " << count;
      ASSERT_EQ(count, kNumBatches * kBatchSize);
      break;
    } else {
      auto message = result.status().ToString();
      ASSERT_TRUE(message.find("Snaphost too old") != std::string::npos) << result.status();
    }
  }
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(CatalogManagerMapsTest)) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE DATABASE test_db"));
  {
    auto test_conn = ASSERT_RESULT(ConnectToDB("test_db"));
    ASSERT_OK(test_conn.Execute("CREATE TABLE foo (a int PRIMARY KEY)"));
    ASSERT_OK(test_conn.Execute("ALTER TABLE foo RENAME TO bar"));
    ASSERT_OK(test_conn.Execute("ALTER TABLE bar RENAME COLUMN a to b"));
  }
  ASSERT_OK(conn.Execute("ALTER DATABASE test_db RENAME TO test_db_renamed"));

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  Result<bool> result(false);
  result = client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, "test_db_renamed", "bar"));
  ASSERT_OK(result);
  ASSERT_TRUE(result.get());
  result = client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, "test_db_renamed", "foo"));
  ASSERT_OK(result);
  ASSERT_FALSE(result.get());
  result = client->NamespaceExists("test_db_renamed", YQL_DATABASE_PGSQL);
  ASSERT_OK(result);
  ASSERT_TRUE(result.get());
  result = client->NamespaceExists("test_db", YQL_DATABASE_PGSQL);
  ASSERT_OK(result);
  ASSERT_FALSE(result.get());

  string ns_id;
  auto list_result = client->ListNamespaces(YQL_DATABASE_PGSQL);
  ASSERT_OK(list_result);
  for (const auto& ns : list_result.get()) {
    if (ns.name() == "test_db_renamed") {
      ns_id = ns.id();
    }
  }

  client::YBSchema schema;
  PartitionSchema partition_schema;
  ASSERT_OK(client->GetTableSchema(
      {YQL_DATABASE_PGSQL, ns_id, "test_db_renamed", "bar"}, &schema, &partition_schema));
  ASSERT_EQ(schema.num_columns(), 1);
  ASSERT_EQ(schema.Column(0).name(), "b");
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestSystemTableRollback)) {
  auto conn1 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn1.Execute("CREATE TABLE pktable (ptest1 int PRIMARY KEY);"));
  Status s = conn1.Execute("CREATE TABLE fktable (ftest1 inet REFERENCES pktable);");
  LOG(INFO) << "Status of second table creation: " << s;
  auto res = ASSERT_RESULT(conn1.Fetch("SELECT * FROM pg_class WHERE relname='fktable'"));
  ASSERT_EQ(0, PQntuples(res.get()));
}

namespace {
Result<master::TabletLocationsPB> GetColocatedTabletLocations(
    client::YBClient* client,
    std::string database_name,
    MonoDelta timeout) {
  std::string ns_id;
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;

  bool exists = VERIFY_RESULT(client->NamespaceExists(database_name, YQL_DATABASE_PGSQL));
  if (!exists) {
    return STATUS(NotFound, "namespace does not exist");
  }

  // Get namespace id.
  for (const auto& ns : VERIFY_RESULT(client->ListNamespaces(YQL_DATABASE_PGSQL))) {
    if (ns.name() == database_name) {
      ns_id = ns.id();
      break;
    }
  }
  if (ns_id.empty()) {
    return STATUS(NotFound, "namespace not found");
  }

  // Get TabletLocations for the colocated tablet.
  RETURN_NOT_OK(WaitFor(
      [&]() -> Result<bool> {
        Status s = client->GetTabletsFromTableId(
            ns_id + master::kColocatedParentTableIdSuffix,
            0 /* max_tablets */,
            &tablets);
        if (s.ok()) {
          return tablets.size() == 1;
        } else if (s.IsNotFound()) {
          return false;
        } else {
          return s;
        }
      },
      timeout,
      "wait for colocated parent tablet"));

  return tablets[0];
}

const TableId GetTableGroupTableId(const std::string& tablegroup_id) {
  return tablegroup_id + master::kTablegroupParentTableIdSuffix;
}

Result<master::TabletLocationsPB> GetTablegroupTabletLocations(
    client::YBClient* client,
    std::string database_name,
    std::string tablegroup_id,
    MonoDelta timeout) {
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;

  bool exists = VERIFY_RESULT(client->TablegroupExists(database_name, tablegroup_id));
  if (!exists) {
    return STATUS(NotFound, "tablegroup does not exist");
  }

  // Get TabletLocations for the tablegroup tablet.
  RETURN_NOT_OK(WaitFor(
      [&]() -> Result<bool> {
        Status s = client->GetTabletsFromTableId(
            GetTableGroupTableId(tablegroup_id),
            0 /* max_tablets */,
            &tablets);
        if (s.ok()) {
          return tablets.size() == 1;
        } else if (s.IsNotFound()) {
          return false;
        } else {
          return s;
        }
      },
      timeout,
      "wait for tablegroup parent tablet"));

  return tablets[0];
}

Result<string> GetTableIdByTableName(
    client::YBClient* client, const string& namespace_name, const string& table_name) {
  const auto tables = VERIFY_RESULT(client->ListTables());
  for (const auto& t : tables) {
    if (t.namespace_name() == namespace_name && t.table_name() == table_name) {
      return t.table_id();
    }
  }
  return STATUS(NotFound, "The table does not exist");
}

} // namespace

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TableColocation)) {
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  const string kDatabaseName = "test_db";
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets_bar_index;
  string ns_id;

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0 WITH colocated = true", kDatabaseName));
  conn = ASSERT_RESULT(ConnectToDB(kDatabaseName));

  // A parent table with one tablet should be created when the database is created.
  const auto colocated_tablet_locations = ASSERT_RESULT(GetColocatedTabletLocations(
      client.get(),
      kDatabaseName,
      30s));
  const auto colocated_tablet_id = colocated_tablet_locations.tablet_id();
  const auto colocated_table = ASSERT_RESULT(client->OpenTable(
      colocated_tablet_locations.table_id()));

  // Create a range partition table, the table should share the tablet with the parent table.
  ASSERT_OK(conn.Execute("CREATE TABLE foo (a INT, PRIMARY KEY (a ASC))"));
  auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "foo"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);
  ASSERT_EQ(tablets[0].tablet_id(), colocated_tablet_id);

  // Create a colocated index table.
  ASSERT_OK(conn.Execute("CREATE INDEX foo_index1 ON foo (a)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "foo_index1"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);
  ASSERT_EQ(tablets[0].tablet_id(), colocated_tablet_id);

  // Create a hash partition table and opt out of using the parent tablet.
  ASSERT_OK(conn.Execute("CREATE TABLE bar (a INT) WITH (colocated = false)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "bar"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  for (auto& tablet : tablets) {
    ASSERT_NE(tablet.tablet_id(), colocated_tablet_id);
  }

  // Create an index on the non-colocated table. The index should follow the table and opt out of
  // colocation.
  ASSERT_OK(conn.Execute("CREATE INDEX bar_index ON bar (a)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "bar_index"));
  const auto table_bar_index = ASSERT_RESULT(client->OpenTable(table_id));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  for (auto& tablet : tablets) {
    ASSERT_NE(tablet.tablet_id(), colocated_tablet_id);
  }
  tablets_bar_index.Swap(&tablets);

  // Create a range partition table without specifying primary key.
  ASSERT_OK(conn.Execute("CREATE TABLE baz (a INT)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "baz"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);
  ASSERT_EQ(tablets[0].tablet_id(), colocated_tablet_id);

  // Create another table and index.
  ASSERT_OK(conn.Execute("CREATE TABLE qux (a INT, PRIMARY KEY (a ASC)) WITH (colocated = true)"));
  ASSERT_OK(conn.Execute("CREATE INDEX qux_index ON qux (a)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "qux_index"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));

  // Drop a table in the parent tablet.
  ASSERT_OK(conn.Execute("DROP TABLE qux"));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "qux"))));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "qux_index"))));

  // Drop a table that is opted out.
  ASSERT_OK(conn.Execute("DROP TABLE bar"));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "bar"))));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "bar_index"))));

  // The tablets for bar_index should be deleted.
  std::vector<bool> tablet_founds(tablets_bar_index.size(), true);
  ASSERT_OK(WaitFor(
      [&] {
        for (int i = 0; i < tablets_bar_index.size(); ++i) {
          client->LookupTabletById(
              tablets_bar_index[i].tablet_id(),
              table_bar_index,
              CoarseMonoClock::Now() + 30s,
              [&, i](const Result<client::internal::RemoteTabletPtr>& result) {
                tablet_founds[i] = result.ok();
              },
              client::UseCache::kFalse);
        }
        return std::all_of(
            tablet_founds.cbegin(),
            tablet_founds.cend(),
            [](bool tablet_found) {
              return !tablet_found;
            });
      },
      30s, "Drop table opted out of colocation"));

  // Drop the database.
  conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("DROP DATABASE $0", kDatabaseName));
  ASSERT_FALSE(ASSERT_RESULT(client->NamespaceExists(kDatabaseName, YQL_DATABASE_PGSQL)));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "foo"))));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "foo_index1"))));

  // The colocation tablet should be deleted.
  bool tablet_found = true;
  int rpc_calls = 0;
  ASSERT_OK(WaitFor(
      [&] {
        rpc_calls++;
        client->LookupTabletById(
            colocated_tablet_id,
            colocated_table,
            CoarseMonoClock::Now() + 30s,
            [&](const Result<client::internal::RemoteTabletPtr>& result) {
              tablet_found = result.ok();
              rpc_calls--;
            },
            client::UseCache::kFalse);
        return !tablet_found;
      },
      30s, "Drop colocated database"));
  // To prevent an "AddressSanitizer: stack-use-after-scope", do not return from this function until
  // all callbacks are done.
  ASSERT_OK(WaitFor(
      [&rpc_calls] {
        LOG(INFO) << "Waiting for " << rpc_calls << " RPCs to run callbacks";
        return rpc_calls == 0;
      },
      30s, "Drop colocated database (wait for RPCs to finish)"));
}

// Test for ensuring that transaction conflicts work as expected for colocated tables.
// Related to https://github.com/yugabyte/yugabyte-db/issues/3251.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TxnConflictsForColocatedTables)) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE DATABASE test_db WITH colocated = true"));

  auto conn1 = ASSERT_RESULT(ConnectToDB("test_db"));
  auto conn2 = ASSERT_RESULT(ConnectToDB("test_db"));

  ASSERT_OK(conn1.Execute("CREATE TABLE t (a INT, PRIMARY KEY (a ASC))"));
  ASSERT_OK(conn1.Execute("INSERT INTO t(a) VALUES(1)"));

  // From conn1, select the row in UPDATE row lock mode. From conn2, delete the row.
  // Ensure that conn1's transaction will detect a conflict at the time of commit.
  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SERIALIZABLE_ISOLATION));
  auto res = ASSERT_RESULT(conn1.Fetch("SELECT * FROM t FOR UPDATE"));
  ASSERT_EQ(PQntuples(res.get()), 1);

  auto status = conn2.Execute("DELETE FROM t WHERE a = 1");
  ASSERT_FALSE(status.ok());
  ASSERT_EQ(PgsqlError(status), YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE) << status;
  ASSERT_STR_CONTAINS(status.ToString(), "Conflicts with higher priority transaction");

  ASSERT_OK(conn1.CommitTransaction());

  // Ensure that reads to separate tables in a colocated database do not conflict.
  ASSERT_OK(conn1.Execute("CREATE TABLE t2 (a INT, PRIMARY KEY (a ASC))"));
  ASSERT_OK(conn1.Execute("INSERT INTO t2(a) VALUES(1)"));

  ASSERT_OK(conn1.StartTransaction(IsolationLevel::SERIALIZABLE_ISOLATION));
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SERIALIZABLE_ISOLATION));

  res = ASSERT_RESULT(conn1.Fetch("SELECT * FROM t FOR UPDATE"));
  ASSERT_EQ(PQntuples(res.get()), 1);
  res = ASSERT_RESULT(conn2.Fetch("SELECT * FROM t2 FOR UPDATE"));
  ASSERT_EQ(PQntuples(res.get()), 1);

  ASSERT_OK(conn1.CommitTransaction());
  ASSERT_OK(conn2.CommitTransaction());
}

class PgLibPqTablegroupTest : public PgLibPqTest {
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    // Enable tablegroup beta feature
    options->extra_tserver_flags.push_back("--ysql_beta_feature_tablegroup=true");
    options->extra_master_flags.push_back("--ysql_beta_feature_tablegroup=true");
  }
};

namespace {

struct TableGroupInfo {
  int oid;
  std::string id;
  TabletId tablet_id;
  std::shared_ptr<client::YBTable> table;
};

Result<TableGroupInfo> SelectTableGroup(
    client::YBClient* client, PGConn* conn, const std::string& database_name,
    const std::string& group_name) {
  TableGroupInfo group_info;
  auto res = VERIFY_RESULT(
      conn->FetchFormat("SELECT oid FROM pg_database WHERE datname=\'$0\'", database_name));
  const int database_oid = VERIFY_RESULT(GetInt32(res.get(), 0, 0));
  res = VERIFY_RESULT(
      conn->FetchFormat("SELECT oid FROM pg_tablegroup WHERE grpname=\'$0\'", group_name));
  group_info.oid = VERIFY_RESULT(GetInt32(res.get(), 0, 0));

  group_info.id = GetPgsqlTablegroupId(database_oid, group_info.oid);
  group_info.tablet_id = VERIFY_RESULT(GetTablegroupTabletLocations(
      client,
      database_name,
      group_info.id,
      30s))
    .tablet_id();
  group_info.table = VERIFY_RESULT(client->OpenTable(GetTableGroupTableId(group_info.id)));
  return group_info;
}

} // namespace

TEST_F_EX(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(ColocatedTablegroups),
          PgLibPqTablegroupTest) {
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  const string kDatabaseName ="tgroup_test_db";
  const string kTablegroupName ="test_tgroup";
  const string kTablegroupAltName ="test_alt_tgroup";
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets_bar_index;

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", kDatabaseName));
  conn = ASSERT_RESULT(ConnectToDB(kDatabaseName));
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLEGROUP $0", kTablegroupName));

  // A parent table with one tablet should be created when the tablegroup is created.
  const auto tablegroup = ASSERT_RESULT(SelectTableGroup(
      client.get(), &conn, kDatabaseName, kTablegroupName));

  // Create a range partition table, the table should share the tablet with the parent table.
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE foo (a INT, PRIMARY KEY (a ASC)) TABLEGROUP $0",
                               kTablegroupName));
  auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "foo"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);
  ASSERT_EQ(tablets[0].tablet_id(), tablegroup.tablet_id);

  // Create a index table that uses the tablegroup by default.
  ASSERT_OK(conn.Execute("CREATE INDEX foo_index1 ON foo (a)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "foo_index1"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);
  ASSERT_EQ(tablets[0].tablet_id(), tablegroup.tablet_id);

  // Create a hash partition table and dont use tablegroup.
  ASSERT_OK(conn.Execute("CREATE TABLE bar (a INT)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "bar"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  for (auto& tablet : tablets) {
    ASSERT_NE(tablet.tablet_id(), tablegroup.tablet_id);
  }

  // Create an index on the table not in a tablegroup. The index should follow the table
  // and opt out of the tablegroup.
  ASSERT_OK(conn.Execute("CREATE INDEX bar_index ON bar (a)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "bar_index"));
  const auto table_bar_index = ASSERT_RESULT(client->OpenTable(table_id));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  for (auto& tablet : tablets) {
    ASSERT_NE(tablet.tablet_id(), tablegroup.tablet_id);
  }
  tablets_bar_index.Swap(&tablets);

  // Create a range partition table without specifying primary key.
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE baz (a INT) TABLEGROUP $0", kTablegroupName));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "baz"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);
  ASSERT_EQ(tablets[0].tablet_id(), tablegroup.tablet_id);

  // Create another table and index.
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE qux (a INT, PRIMARY KEY (a ASC)) TABLEGROUP $0",
                               kTablegroupName));
  ASSERT_OK(conn.Execute("CREATE INDEX qux_index ON qux (a)"));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "qux"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets[0].tablet_id(), tablegroup.tablet_id);
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "qux_index"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets[0].tablet_id(), tablegroup.tablet_id);

  // Now create a second tablegroup.
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLEGROUP $0", kTablegroupAltName));

  // A parent table with one tablet should be created when the tablegroup is created.
  auto tablegroup_alt = ASSERT_RESULT(SelectTableGroup(
      client.get(), &conn, kDatabaseName, kTablegroupAltName));

  // Create another range partition table - should be part of the second tablegroup
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE quuz (a INT, PRIMARY KEY (a ASC)) TABLEGROUP $0",
                               kTablegroupAltName));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "quuz"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);
  ASSERT_EQ(tablets[0].tablet_id(), tablegroup_alt.tablet_id);

    // Drop a table in the parent tablet.
  ASSERT_OK(conn.Execute("DROP TABLE quuz"));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "quuz"))));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "quuz_index"))));

  // Drop a table that is opted out.
  ASSERT_OK(conn.Execute("DROP TABLE bar"));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "bar"))));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "bar_index"))));

  // The tablets for bar_index should be deleted.
  std::vector<bool> tablet_founds(tablets_bar_index.size(), true);
  ASSERT_OK(WaitFor(
      [&] {
        for (int i = 0; i < tablets_bar_index.size(); ++i) {
          client->LookupTabletById(
              tablets_bar_index[i].tablet_id(),
              table_bar_index,
              CoarseMonoClock::Now() + 30s,
              [&, i](const Result<client::internal::RemoteTabletPtr>& result) {
                tablet_founds[i] = result.ok();
              },
              client::UseCache::kFalse);
        }
        return std::all_of(
            tablet_founds.cbegin(),
            tablet_founds.cend(),
            [](bool tablet_found) {
              return !tablet_found;
            });
      },
      30s, "Drop table did not use tablegroups"));

  // Drop a tablegroup.
  ASSERT_OK(conn.ExecuteFormat("DROP TABLEGROUP $0", kTablegroupAltName));
  ASSERT_FALSE(ASSERT_RESULT(client->TablegroupExists(kDatabaseName, kTablegroupAltName)));

  // The alt tablegroup tablet should be deleted after dropping the tablegroup.
  bool alt_tablet_found = true;
  int rpc_calls = 0;
  ASSERT_OK(WaitFor(
      [&] {
        rpc_calls++;
        client->LookupTabletById(
            tablegroup_alt.tablet_id,
            tablegroup_alt.table,
            CoarseMonoClock::Now() + 30s,
            [&](const Result<client::internal::RemoteTabletPtr>& result) {
              alt_tablet_found = result.ok();
              rpc_calls--;
            },
            client::UseCache::kFalse);
        return !alt_tablet_found;
      },
      30s, "Drop tablegroup"));

  // Recreate that tablegroup. Being able to recreate it and add tables to it tests that it was
  // properly cleaned up from catalog manager maps and postgres metadata at time of DROP.
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLEGROUP $0", kTablegroupAltName));

  // A parent table with one tablet should be created when the tablegroup is created.
  tablegroup_alt = ASSERT_RESULT(SelectTableGroup(
        client.get(), &conn, kDatabaseName, kTablegroupAltName));

  // Add a table back in and ensure that it is part of the recreated tablegroup.
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE quuz (a INT, PRIMARY KEY (a ASC)) TABLEGROUP $0",
                               kTablegroupAltName));
  table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "quuz"));
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 1);
  ASSERT_EQ(tablets[0].tablet_id(), tablegroup_alt.tablet_id);

  // Drop the database.
  conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("DROP DATABASE $0", kDatabaseName));
  ASSERT_FALSE(ASSERT_RESULT(client->NamespaceExists(kDatabaseName, YQL_DATABASE_PGSQL)));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "foo"))));
  ASSERT_FALSE(ASSERT_RESULT(
        client->TableExists(client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, "foo_index1"))));

  // The original tablegroup tablet should be deleted after dropping the database.
  bool orig_tablet_found = true;
  ASSERT_OK(WaitFor(
      [&] {
        rpc_calls++;
        client->LookupTabletById(
            tablegroup.tablet_id,
            tablegroup.table,
            CoarseMonoClock::Now() + 30s,
            [&](const Result<client::internal::RemoteTabletPtr>& result) {
              orig_tablet_found = result.ok();
              rpc_calls--;
            },
            client::UseCache::kFalse);
        return !orig_tablet_found;
      },
      30s, "Drop database with tablegroup"));

  // The second tablegroup tablet should also be deleted after dropping the database.
  bool second_tablet_found = true;
  ASSERT_OK(WaitFor(
      [&] {
        rpc_calls++;
        client->LookupTabletById(
            tablegroup_alt.tablet_id,
            tablegroup_alt.table,
            CoarseMonoClock::Now() + 30s,
            [&](const Result<client::internal::RemoteTabletPtr>& result) {
              second_tablet_found = result.ok();
              rpc_calls--;
            },
            client::UseCache::kFalse);
        return !second_tablet_found;
      },
      30s, "Drop database with tablegroup"));

  // To prevent an "AddressSanitizer: stack-use-after-scope", do not return from this function until
  // all callbacks are done.
  ASSERT_OK(WaitFor(
      [&rpc_calls] {
        LOG(INFO) << "Waiting for " << rpc_calls << " RPCs to run callbacks";
        return rpc_calls == 0;
      },
      30s, "Drop database with tablegroup (wait for RPCs to finish)"));
}

// Test that the number of RPCs sent to master upon first connection is not too high.
// See https://github.com/yugabyte/yugabyte-db/issues/3049
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(NumberOfInitialRpcs)) {
  auto get_master_inbound_rpcs_created = [&cluster_ = this->cluster_]() -> Result<int64_t> {
    int64_t m_in_created = 0;
    for (auto* master : cluster_->master_daemons()) {
      m_in_created += VERIFY_RESULT(master->GetInt64Metric(
          &METRIC_ENTITY_server, "yb.master", &METRIC_rpc_inbound_calls_created, "value"));
    }
    return m_in_created;
  };

  int64_t rpcs_before = ASSERT_RESULT(get_master_inbound_rpcs_created());
  ASSERT_RESULT(Connect());
  int64_t rpcs_after  = ASSERT_RESULT(get_master_inbound_rpcs_created());
  int64_t rpcs_during = rpcs_after - rpcs_before;

  // Real-world numbers (debug build, local Mac): 328 RPCs before, 95 after the fix for #3049
  LOG(INFO) << "Master inbound RPC during connection: " << rpcs_during;
  // RPC counter is affected no only by table read/write operations but also by heartbeat mechanism.
  // As far as ASAN/TSAN builds are slower they can receive more heartbeats while
  // processing requests. As a result RPC count might be higher in comparison to other build types.
  ASSERT_LT(rpcs_during, RegularBuildVsSanitizers(150, 200));
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(RangePresplit)) {
  const string kDatabaseName ="yugabyte";
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  string ns_id;

  auto conn = ASSERT_RESULT(ConnectToDB(kDatabaseName));
  ASSERT_OK(conn.Execute("CREATE TABLE range(a int, PRIMARY KEY(a ASC)) " \
      "SPLIT AT VALUES ((100), (1000))"));

  // Get database and table IDs
  for (const auto& ns : ASSERT_RESULT(client->ListNamespaces(YQL_DATABASE_PGSQL))) {
    if (ns.name() == kDatabaseName) {
      ns_id = ns.id();
      break;
    }
  }
  ASSERT_FALSE(ns_id.empty());

  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kDatabaseName, "range"));

  // Validate that number of tablets created is 3.
  ASSERT_OK(client->GetTabletsFromTableId(table_id, 0, &tablets));
  ASSERT_EQ(tablets.size(), 3);
}

// Override the base test to start a cluster that kicks out unresponsive tservers faster.
class PgLibPqTestSmallTSTimeout : public PgLibPqTest {
 public:
  PgLibPqTestSmallTSTimeout() {
    more_master_flags.push_back("--tserver_unresponsive_timeout_ms=8000");
    more_master_flags.push_back("--unresponsive_ts_rpc_timeout_ms=10000");
    more_tserver_flags.push_back("--follower_unavailable_considered_failed_sec=10");
  }

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->extra_master_flags.insert(
        std::end(options->extra_master_flags),
        std::begin(more_master_flags),
        std::end(more_master_flags));
    options->extra_tserver_flags.insert(
        std::end(options->extra_tserver_flags),
        std::begin(more_tserver_flags),
        std::end(more_tserver_flags));
  }

 protected:
  std::vector<std::string> more_master_flags;
  std::vector<std::string> more_tserver_flags;
};

// Test that adding a tserver and removing a tserver causes the colocation tablet to adjust raft
// configuration off the old tserver and onto the new tserver.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(LoadBalanceSingleColocatedDB),
          PgLibPqTestSmallTSTimeout) {
  const std::string kDatabaseName = "co";
  const auto kTimeout = 60s;
  const int starting_num_tablet_servers = cluster_->num_tablet_servers();
  std::string ns_id;
  std::map<std::string, int> ts_loads;

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0 WITH colocated = true", kDatabaseName));

  // Collect colocation tablet replica locations.
  {
    master::TabletLocationsPB tablet_locations = ASSERT_RESULT(GetColocatedTabletLocations(
        client.get(),
        kDatabaseName,
        kTimeout));
    for (const auto& replica : tablet_locations.replicas()) {
      ts_loads[replica.ts_info().permanent_uuid()]++;
    }
  }

  // Ensure each tserver has exactly one colocation tablet replica.
  ASSERT_EQ(ts_loads.size(), starting_num_tablet_servers);
  for (const auto& entry : ts_loads) {
    ASSERT_NOTNULL(cluster_->tablet_server_by_uuid(entry.first));
    ASSERT_EQ(entry.second, 1);
    LOG(INFO) << "found ts " << entry.first << " has " << entry.second << " replicas";
  }

  // Add a tablet server.
  ASSERT_OK(cluster_->AddTabletServer(ExternalMiniClusterOptions::kDefaultStartCqlProxy,
                                      more_tserver_flags));
  ASSERT_OK(cluster_->WaitForTabletServerCount(starting_num_tablet_servers + 1, kTimeout));

  // Wait for load balancing.  This should move some tablet-peers (e.g. of the colocation tablet,
  // system.transactions tablets) to the new tserver.
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        bool is_idle = VERIFY_RESULT(client->IsLoadBalancerIdle());
        return !is_idle;
      },
      kTimeout,
      "wait for load balancer to be active"));
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return client->IsLoadBalancerIdle();
      },
      kTimeout,
      "wait for load balancer to be idle"));

  // Remove a tablet server.
  cluster_->tablet_server(0)->Shutdown();

  // Wait for load balancing.  This should move the remaining tablet-peers off the dead tserver.
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        bool is_idle = VERIFY_RESULT(client->IsLoadBalancerIdle());
        return !is_idle;
      },
      kTimeout,
      "wait for load balancer to be active"));
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return client->IsLoadBalancerIdle();
      },
      kTimeout,
      "wait for load balancer to be idle"));

  // Collect colocation tablet replica locations.
  {
    master::TabletLocationsPB tablet_locations = ASSERT_RESULT(GetColocatedTabletLocations(
        client.get(),
        kDatabaseName,
        kTimeout));
    ts_loads.clear();
    for (const auto& replica : tablet_locations.replicas()) {
      ts_loads[replica.ts_info().permanent_uuid()]++;
    }
  }

  // Ensure each colocation tablet replica is on the three tablet servers excluding the first one,
  // which is shut down.
  ASSERT_EQ(ts_loads.size(), starting_num_tablet_servers);
  for (const auto& entry : ts_loads) {
    ExternalTabletServer* ts = cluster_->tablet_server_by_uuid(entry.first);
    ASSERT_NOTNULL(ts);
    ASSERT_NE(ts, cluster_->tablet_server(0));
    ASSERT_EQ(entry.second, 1);
    LOG(INFO) << "found ts " << entry.first << " has " << entry.second << " replicas";
  }
}

// Test that adding a tserver causes colocation tablets to offload tablet-peers to the new tserver.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(LoadBalanceMultipleColocatedDB)) {
  constexpr int kNumDatabases = 3;
  const auto kTimeout = 60s;
  const int starting_num_tablet_servers = cluster_->num_tablet_servers();
  const std::string kDatabasePrefix = "co";
  std::map<std::string, int> ts_loads;

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(Connect());

  for (int i = 0; i < kNumDatabases; ++i) {
    ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0$1 WITH colocated = true", kDatabasePrefix, i));
  }

  // Add a tablet server.
  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(starting_num_tablet_servers + 1, kTimeout));

  // Wait for load balancing.  This should move some tablet-peers (e.g. of the colocation tablets,
  // system.transactions tablets) to the new tserver.
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        bool is_idle = VERIFY_RESULT(client->IsLoadBalancerIdle());
        return !is_idle;
      },
      kTimeout,
      "wait for load balancer to be active"));
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return client->IsLoadBalancerIdle();
      },
      kTimeout,
      "wait for load balancer to be idle"));

  // Collect colocation tablets' replica locations.
  for (int i = 0; i < kNumDatabases; ++i) {
    master::TabletLocationsPB tablet_locations = ASSERT_RESULT(GetColocatedTabletLocations(
        client.get(),
        Format("$0$1", kDatabasePrefix, i),
        kTimeout));
    for (const auto& replica : tablet_locations.replicas()) {
      ts_loads[replica.ts_info().permanent_uuid()]++;
    }
  }

  // Ensure that the load is properly distributed.
  int min_load = kNumDatabases;
  int max_load = 0;
  for (const auto& entry : ts_loads) {
    if (entry.second < min_load) {
      min_load = entry.second;
    } else if (entry.second > max_load) {
      max_load = entry.second;
    }
  }
  LOG(INFO) << "Found max_load on a TS = " << max_load << ", and min_load on a ts = " << min_load;
  ASSERT_LT(max_load - min_load, 2);
  ASSERT_EQ(ts_loads.size(), kNumDatabases + 1);
}

// Override the base test to start a cluster with index backfill enabled.
class PgLibPqTestIndexBackfill : public PgLibPqTest {
 public:
  PgLibPqTestIndexBackfill() {
    more_master_flags.push_back("--ysql_disable_index_backfill=false");
    more_tserver_flags.push_back("--ysql_disable_index_backfill=false");
  }

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->extra_master_flags.insert(
        std::end(options->extra_master_flags),
        std::begin(more_master_flags),
        std::end(more_master_flags));
    options->extra_tserver_flags.insert(
        std::end(options->extra_tserver_flags),
        std::begin(more_tserver_flags),
        std::end(more_tserver_flags));
  }

 protected:
  std::vector<std::string> more_master_flags;
  std::vector<std::string> more_tserver_flags;
};

// Make sure that backfill works.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillSimple),
          PgLibPqTestIndexBackfill) {
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (c char, i int, p point)", kTableName));
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES ('a', 0, '(1, 2)')", kTableName));
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES ('y', -5, '(0, -2)')", kTableName));
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES ('b', 100, '(868, 9843)')", kTableName));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX ON $0 (c ASC)", kTableName));

  // Index scan to verify contents of index table.
  const std::string query = Format("SELECT * FROM $0 ORDER BY c", kTableName);
  ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
  auto res = ASSERT_RESULT(conn.Fetch(query));
  ASSERT_EQ(PQntuples(res.get()), 3);
  ASSERT_EQ(PQnfields(res.get()), 3);
  std::array<int, 3> values = {
    ASSERT_RESULT(GetInt32(res.get(), 0, 1)),
    ASSERT_RESULT(GetInt32(res.get(), 1, 1)),
    ASSERT_RESULT(GetInt32(res.get(), 2, 1)),
  };
  ASSERT_EQ(values[0], 0);
  ASSERT_EQ(values[1], 100);
  ASSERT_EQ(values[2], -5);
}

// Make sure that partial indexes work for index backfill.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillPartial),
          PgLibPqTestIndexBackfill) {
  constexpr int kNumRows = 7;
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j int)", kTableName));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1), generate_series(-1, -$1, -1))",
      kTableName,
      kNumRows));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX ON $0 (i ASC) WHERE j > -5", kTableName));

  // Index scan to verify contents of index table.
  {
    const std::string query = Format("SELECT j FROM $0 WHERE j > -3 ORDER BY i", kTableName);
    ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
    auto res = ASSERT_RESULT(conn.Fetch(query));
    ASSERT_EQ(PQntuples(res.get()), 2);
    ASSERT_EQ(PQnfields(res.get()), 1);
    std::array<int, 2> values = {
      ASSERT_RESULT(GetInt32(res.get(), 0, 0)),
      ASSERT_RESULT(GetInt32(res.get(), 1, 0)),
    };
    ASSERT_EQ(values[0], -1);
    ASSERT_EQ(values[1], -2);
  }
  {
    const std::string query = Format(
        "SELECT i FROM $0 WHERE j > -5 ORDER BY i DESC LIMIT 2",
        kTableName);
    ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
    auto res = ASSERT_RESULT(conn.Fetch(query));
    ASSERT_EQ(PQntuples(res.get()), 2);
    ASSERT_EQ(PQnfields(res.get()), 1);
    std::array<int, 2> values = {
      ASSERT_RESULT(GetInt32(res.get(), 0, 0)),
      ASSERT_RESULT(GetInt32(res.get(), 1, 0)),
    };
    ASSERT_EQ(values[0], 4);
    ASSERT_EQ(values[1], 3);
  }
}

// Make sure that expression indexes work for index backfill.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillExpression),
          PgLibPqTestIndexBackfill) {
  constexpr int kNumRows = 9;
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j int)", kTableName));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1), generate_series(11, 10 + $1))",
      kTableName,
      kNumRows));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX ON $0 ((j % i))", kTableName));

  // Index scan to verify contents of index table.
  const std::string query = Format(
      "SELECT j, i, j % i as mod FROM $0 WHERE j % i = 2 ORDER BY i",
      kTableName);
  ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
  auto res = ASSERT_RESULT(conn.Fetch(query));
  ASSERT_EQ(PQntuples(res.get()), 2);
  ASSERT_EQ(PQnfields(res.get()), 3);
  std::array<std::array<int, 3>, 2> values = {{
    {
      ASSERT_RESULT(GetInt32(res.get(), 0, 0)),
      ASSERT_RESULT(GetInt32(res.get(), 0, 1)),
      ASSERT_RESULT(GetInt32(res.get(), 0, 2)),
    },
    {
      ASSERT_RESULT(GetInt32(res.get(), 1, 0)),
      ASSERT_RESULT(GetInt32(res.get(), 1, 1)),
      ASSERT_RESULT(GetInt32(res.get(), 1, 2)),
    },
  }};
  ASSERT_EQ(values[0][0], 14);
  ASSERT_EQ(values[0][1], 4);
  ASSERT_EQ(values[0][2], 2);
  ASSERT_EQ(values[1][0], 18);
  ASSERT_EQ(values[1][1], 8);
  ASSERT_EQ(values[1][2], 2);
}

// Make sure that unique indexes work when index backfill is enabled.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillUnique),
          PgLibPqTestIndexBackfill) {
  constexpr int kNumRows = 3;
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j int)", kTableName));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1), generate_series(11, 10 + $1))",
      kTableName,
      kNumRows));
  // Add row that would make j not unique.
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (99, 11)",
      kTableName,
      kNumRows));

  // Create unique index without failure.
  ASSERT_OK(conn.ExecuteFormat("CREATE UNIQUE INDEX ON $0 (i ASC)", kTableName));
  // Index scan to verify contents of index table.
  const std::string query = Format(
      "SELECT * FROM $0 ORDER BY i",
      kTableName);
  ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
  auto res = ASSERT_RESULT(conn.Fetch(query));
  ASSERT_EQ(PQntuples(res.get()), 4);
  ASSERT_EQ(PQnfields(res.get()), 2);

  // Create unique index with failure.
  Status status = conn.ExecuteFormat("CREATE UNIQUE INDEX ON $0 (j ASC)", kTableName);
  ASSERT_NOK(status);
  auto msg = status.message().ToBuffer();
  ASSERT_TRUE(msg.find("duplicate key value violates unique constraint") != std::string::npos)
      << status;
}

// Make sure that indexes created in postgres nested DDL work and skip backfill (optimization).
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillNestedDdl),
          PgLibPqTestIndexBackfill) {
  constexpr int kNumRows = 3;
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j int, UNIQUE (j))", kTableName));

  // Make sure that the index create was not multi-stage.
  std::string table_id =
      ASSERT_RESULT(GetTableIdByTableName(client.get(), kNamespaceName, kTableName));
  std::shared_ptr<client::YBTableInfo> table_info = std::make_shared<client::YBTableInfo>();
  Synchronizer sync;
  ASSERT_OK(client->GetTableSchemaById(table_id, table_info, sync.AsStatusCallback()));
  ASSERT_OK(sync.Wait());
  ASSERT_EQ(table_info->schema.version(), 1);

  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1), generate_series(11, 10 + $1))",
      kTableName,
      kNumRows));

  // Add row that violates unique constraint on j.
  Status status = conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (99, 11)",
      kTableName,
      kNumRows);
  ASSERT_NOK(status);
  auto msg = status.message().ToBuffer();
  ASSERT_TRUE(msg.find("duplicate key value") != std::string::npos) << status;
}

// Make sure that drop index works when index backfill is enabled (skips online schema migration for
// now)
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillDrop),
          PgLibPqTestIndexBackfill) {
  constexpr int kNumRows = 5;
  const std::string kNamespaceName = "yugabyte";
  const std::string kIndexName = "i";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j int)", kTableName));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1), generate_series(11, 10 + $1))",
      kTableName,
      kNumRows));

  // Create index.
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0 ON $1 (i ASC)", kIndexName, kTableName));

  // Drop index.
  ASSERT_OK(conn.ExecuteFormat("DROP INDEX $0", kIndexName));

  // Ensure index is not used for scan.
  const std::string query = Format(
      "SELECT * FROM $0 ORDER BY i",
      kTableName);
  ASSERT_FALSE(ASSERT_RESULT(conn.HasIndexScan(query)));
}

// Make sure deletes to nonexistent rows look like noops to clients.  This may seem too obvious to
// necessitate a test, but logic for backfill is special in that it wants nonexistent index deletes
// to be applied for the backfill process to use them.  This test guards against that logic being
// implemented incorrectly.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillNonexistentDelete),
          PgLibPqTestIndexBackfill) {
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int PRIMARY KEY)", kTableName));

  // Delete to nonexistent row should return no rows.
  auto res = ASSERT_RESULT(conn.FetchFormat("DELETE FROM $0 WHERE i = 1 RETURNING i", kTableName));
  ASSERT_EQ(PQntuples(res.get()), 0);
  ASSERT_EQ(PQnfields(res.get()), 1);
}

// Make sure that index backfill on large tables backfills all data.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillLarge),
          PgLibPqTestIndexBackfill) {
  constexpr int kNumRows = 10000;
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));

  // Insert bunch of rows.
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1))",
      kTableName,
      kNumRows));

  // Create index.
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX ON $0 (i ASC)", kTableName));

  // All rows should be in the index.
  const std::string query = Format(
      "SELECT COUNT(*) FROM $0 WHERE i > 0",
      kTableName);
  ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
  auto res = ASSERT_RESULT(conn.Fetch(query));
  ASSERT_EQ(PQntuples(res.get()), 1);
  ASSERT_EQ(PQnfields(res.get()), 1);
  int actual_num_rows = ASSERT_RESULT(GetInt64(res.get(), 0, 0));
  ASSERT_EQ(actual_num_rows, kNumRows);
}

// Make sure that CREATE INDEX NONCONCURRENTLY doesn't use backfill.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillNonconcurrent),
          PgLibPqTestIndexBackfill) {
  const std::string kIndexName = "x";
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));
  auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kNamespaceName, kTableName));

  // To determine whether the index uses backfill or not, look at the table schema version before
  // and after.  We can't look at the DocDB index permissions because
  // - if backfill is skipped, index_permissions is unset, and the default value is
  //   INDEX_PERM_READ_WRITE_AND_DELETE
  // - if backfill is used, index_permissions is INDEX_PERM_READ_WRITE_AND_DELETE
  // - GetTableSchemaById offers no way to see whether the default value for index permissions is
  //   set
  std::shared_ptr<client::YBTableInfo> info = std::make_shared<client::YBTableInfo>();
  {
    Synchronizer sync;
    ASSERT_OK(client->GetTableSchemaById(table_id, info, sync.AsStatusCallback()));
    ASSERT_OK(sync.Wait());
  }
  ASSERT_EQ(info->schema.version(), 0);

  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX NONCONCURRENTLY $0 ON $1 (i)",
                               kIndexName,
                               kTableName));

  // If the index used backfill, it would have incremented the table schema version by two or three:
  // - add index info with INDEX_PERM_DELETE_ONLY
  // - update to INDEX_PERM_DO_BACKFILL (as part of issue #6218)
  // - update to INDEX_PERM_READ_WRITE_AND_DELETE
  // If the index did not use backfill, it would have incremented the table schema version by one:
  // - add index info with no DocDB permission (default INDEX_PERM_READ_WRITE_AND_DELETE)
  // Expect that it did not use backfill.
  {
    Synchronizer sync;
    ASSERT_OK(client->GetTableSchemaById(table_id, info, sync.AsStatusCallback()));
    ASSERT_OK(sync.Wait());
  }
  ASSERT_EQ(info->schema.version(), 1);
}

// Test simultaneous CREATE INDEX.
// TODO(jason): update this when closing issue #6269.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(CreateIndexSimultaneously),
          PgLibPqTestIndexBackfill) {
  constexpr int kNumRows = 10;
  constexpr int kNumThreads = 5;
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";
  const std::string query = Format("SELECT * FROM $0 WHERE i = $1", kTableName, 7);

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1))",
      kTableName,
      kNumRows));

  std::vector<std::thread> threads;
  std::vector<Status> statuses;
  LOG(INFO) << "Starting threads";
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&] {
      auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
      statuses.emplace_back(conn.ExecuteFormat("CREATE INDEX ON $0 (i)", kTableName));
    });
  }
  LOG(INFO) << "Joining threads";
  for (auto& thread : threads) {
    thread.join();
  }

  LOG(INFO) << "Inspecting statuses";
  int num_ok = 0;
  ASSERT_EQ(statuses.size(), kNumThreads);
  for (auto& status : statuses) {
    LOG(INFO) << "status: " << status;
    if (status.ok()) {
      num_ok++;
    } else {
      ASSERT_TRUE(status.IsNetworkError()) << status;
      const std::string msg = status.message().ToBuffer();
      ASSERT_TRUE(
          (msg.find("Catalog Version Mismatch") != std::string::npos)
          || (msg.find("Conflicts with higher priority transaction") != std::string::npos)
          || (msg.find("Transaction aborted") != std::string::npos)
          || (msg.find("Unknown transaction, could be recently aborted") != std::string::npos)
          || (msg.find("Transaction metadata missing") != std::string::npos))
        << status.message().ToBuffer();
    }
  }
  ASSERT_EQ(num_ok, 1);

  LOG(INFO) << "Checking postgres schema";
  {
    // Check number of indexes.
    auto res = ASSERT_RESULT(conn.FetchFormat(
        "SELECT indexname FROM pg_indexes WHERE tablename = '$0'", kTableName));
    ASSERT_EQ(PQntuples(res.get()), 1);
    const std::string actual = ASSERT_RESULT(GetString(res.get(), 0, 0));
    const std::string expected = Format("$0_i_idx", kTableName);
    ASSERT_EQ(actual, expected);

    // Check whether index is public using index scan.
    ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
  }
  LOG(INFO) << "Checking DocDB schema";
  std::vector<TableId> orphaned_docdb_index_ids;
  {
    auto client = ASSERT_RESULT(cluster_->CreateClient());
    std::string table_id =
        ASSERT_RESULT(GetTableIdByTableName(client.get(), kNamespaceName, kTableName));
    std::shared_ptr<client::YBTableInfo> table_info = std::make_shared<client::YBTableInfo>();
    Synchronizer sync;
    ASSERT_OK(client->GetTableSchemaById(table_id, table_info, sync.AsStatusCallback()));
    ASSERT_OK(sync.Wait());

    // Check schema version.
    //   kNumThreads (INDEX_PERM_WRITE_AND_DELETE)
    // + 1 (INDEX_PERM_READ_WRITE_AND_DELETE)
    // = kNumThreads + 1
    // TODO(jason): change this when closing #6218 because DO_BACKFILL permission will add another
    // schema version.
    ASSERT_EQ(table_info->schema.version(), kNumThreads + 1);

    // Check number of indexes.
    ASSERT_EQ(table_info->index_map.size(), kNumThreads);

    // Check index permissions.  Also collect orphaned DocDB indexes.
    int num_rwd = 0;
    for (auto& pair : table_info->index_map) {
      VLOG(1) << "table id: " << pair.first;
      IndexPermissions perm = pair.second.index_permissions();
      if (perm == IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE) {
        num_rwd++;
      } else {
        ASSERT_EQ(perm, IndexPermissions::INDEX_PERM_WRITE_AND_DELETE);
        orphaned_docdb_index_ids.emplace_back(pair.first);
      }
    }
    ASSERT_EQ(num_rwd, 1);
  }

  LOG(INFO) << "Removing orphaned DocDB indexes";
  {
    auto client = ASSERT_RESULT(cluster_->CreateClient());
    for (TableId& index_id : orphaned_docdb_index_ids) {
      client::YBTableName indexed_table_name;
      ASSERT_OK(client->DeleteIndexTable(index_id, &indexed_table_name));
    }
  }

  LOG(INFO) << "Checking if index still works";
  {
    ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
    auto res = ASSERT_RESULT(conn.Fetch(query));
    ASSERT_EQ(PQntuples(res.get()), 1);
    auto value = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(value, 7);
  }
}

// Override the index backfill test to disable transparent retries on cache version mismatch.
class PgLibPqTestIndexBackfillNoRetry : public PgLibPqTestIndexBackfill {
 public:
  PgLibPqTestIndexBackfillNoRetry() {
    more_tserver_flags.push_back("--TEST_ysql_disable_transparent_cache_refresh_retry=true");
  }
};

TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillDropNoRetry),
          PgLibPqTestIndexBackfillNoRetry) {
  constexpr int kNumRows = 5;
  const std::string kNamespaceName = "yugabyte";
  const std::string kIndexName = "i";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j int)", kTableName));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO $0 VALUES (generate_series(1, $1), generate_series(11, 10 + $1))",
      kTableName,
      kNumRows));

  // Create index.
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0 ON $1 (i ASC)", kIndexName, kTableName));

  // Update the table cache entry for the indexed table.
  ASSERT_OK(conn.FetchFormat("SELECT * FROM $0", kTableName));

  // Drop index.
  ASSERT_OK(conn.ExecuteFormat("DROP INDEX $0", kIndexName));

  // Ensure that there is no schema version mismatch for the indexed table.  This is because the
  // above `DROP INDEX` should have invalidated the corresponding table cache entry.  (There also
  // should be no catalog version mismatch because it is updated for the same session after DDL.)
  ASSERT_OK(conn.FetchFormat("SELECT * FROM $0", kTableName));
}

// Override the index backfill test to do alter slowly.
class PgLibPqTestIndexBackfillAlterSlowly : public PgLibPqTestIndexBackfill {
 public:
  PgLibPqTestIndexBackfillAlterSlowly() {
    more_tserver_flags.push_back("--TEST_alter_schema_delay_ms=10000");
  }
};

// Test whether IsCreateTableDone works when creating an index with backfill enabled.  See issue
// #6234.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillIsCreateTableDone),
          PgLibPqTestIndexBackfillAlterSlowly) {
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));
  ASSERT_OK(conn.ExecuteFormat("CREATE INDEX ON $0 (i)", kTableName));
}

namespace {

Result<bool> IsAtTargetIndexStateFlags(
    PGConn* conn,
    const std::string& index_name,
    const std::map<std::string, bool>& target_index_state_flags) {
  constexpr int kNumIndexStateFlags = 3;

  // Check target_index_state_flags.
  SCHECK_EQ(
      target_index_state_flags.size(),
      kNumIndexStateFlags,
      InvalidArgument,
      Format("$0 should have $1 state flags", "target_index_state_flags", kNumIndexStateFlags));
  SCHECK(
      target_index_state_flags.find("indislive") != target_index_state_flags.end(),
      InvalidArgument,
      Format("$0 should have $1 state flag", "target_index_state_flags", "indislive"));
  SCHECK(
      target_index_state_flags.find("indisready") != target_index_state_flags.end(),
      InvalidArgument,
      Format("$0 should have $1 state flag", "target_index_state_flags", "indisready"));
  SCHECK(
      target_index_state_flags.find("indisvalid") != target_index_state_flags.end(),
      InvalidArgument,
      Format("$0 should have $1 state flag", "target_index_state_flags", "indisvalid"));

  // Get actual index state flags.
  auto res = VERIFY_RESULT(conn->FetchFormat(
      "SELECT $0"
      " FROM pg_class INNER JOIN pg_index ON pg_class.oid = pg_index.indexrelid"
      " WHERE pg_class.relname = '$1'",
      JoinKeysIterator(target_index_state_flags.begin(), target_index_state_flags.end(), ", "),
      index_name));
  if (PQntuples(res.get()) == 0) {
    LOG(WARNING) << index_name << " not found in system tables";
    return false;
  }

  // Check index state flags.
  for (const auto& target_index_state_flag : target_index_state_flags) {
    bool actual_value = VERIFY_RESULT(GetBool(
        res.get(),
        0,
        std::distance(
            target_index_state_flags.begin(),
            target_index_state_flags.find(target_index_state_flag.first))));
    bool expected_value = target_index_state_flag.second;
    if (actual_value < expected_value) {
      LOG(INFO) << index_name
                << " not yet at target index state flag "
                << target_index_state_flag.first;
      return false;
    } else if (actual_value > expected_value) {
      return STATUS(RuntimeError,
                    Format("$0 exceeded target index state flag $1",
                           index_name,
                           target_index_state_flag.first));
    }
  }
  return true;
}

CHECKED_STATUS WaitForBackfillStage(
    PGConn* conn,
    const std::string& index_name,
    const MonoDelta& index_state_flags_update_delay) {
  const MonoTime start_time = MonoTime::Now();
  const std::map<std::string, bool> index_state_flags{
    {"indislive", true},
    {"indisready", true},
    {"indisvalid", false},
  };

  LOG(INFO) << "Waiting for pg_index indisready to be true";
  RETURN_NOT_OK(WaitFor(
      std::bind(IsAtTargetIndexStateFlags, conn, index_name, index_state_flags),
      index_state_flags_update_delay * 2,
      Format("Wait for index state flags to hit target: $0", index_state_flags)));

  LOG(INFO) << "Waiting till (approx) the end of the delay after committing indisready true";
  SleepFor(
      (index_state_flags_update_delay * 2)
      - (MonoTime::Now() - start_time));

  return Status::OK();
}

} // namespace

// Override the index backfill test to have delays for testing snapshot too old.
class PgLibPqTestIndexBackfillSnapshotTooOld : public PgLibPqTestIndexBackfill {
 public:
  PgLibPqTestIndexBackfillSnapshotTooOld() {
    more_tserver_flags.push_back("--TEST_slowdown_backfill_by_ms=10000");
    more_tserver_flags.push_back("--TEST_ysql_index_state_flags_update_delay_ms=0");
    more_tserver_flags.push_back("--timestamp_history_retention_interval_sec=3");
  }
};

// Make sure that index backfill doesn't care about snapshot too old.  Force a situation where the
// indexed table scan for backfill would occur after the committed history cutoff.  A compaction is
// needed to update this committed history cutoff, and the retention period needs to be low enough
// so that the cutoff is ahead of backfill's safe read time.  See issue #6333.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillSnapshotTooOld),
          PgLibPqTestIndexBackfillSnapshotTooOld) {
  const std::string kIndexName = "i";
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";
  constexpr int kTimeoutSec = 3;

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  // (Make it one tablet for simplicity.)
  LOG(INFO) << "Create table...";
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (c char) SPLIT INTO 1 TABLETS", kTableName));

  LOG(INFO) << "Get table id for indexed table...";
  auto table_id = ASSERT_RESULT(GetTableIdByTableName(client.get(), kNamespaceName, kTableName));

  // Insert something so that reading it would trigger snapshot too old.
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES ('s')", kTableName));

  std::thread create_index_thread([&] {
    LOG(INFO) << "Create index...";
    Status s = conn.ExecuteFormat("CREATE INDEX $0 ON $1 (c)", kIndexName, kTableName);
    if (!s.ok()) {
      // We are doomed to fail the test.  Before that, let's see if it turns out to be "snapshot too
      // old" or some other unexpected error.
      ASSERT_TRUE(s.IsNetworkError()) << "got unexpected error: " << s;
      ASSERT_TRUE(s.message().ToBuffer().find("Snapshot too old") != std::string::npos)
          << "got unexpected error: " << s;
      // It is "snapshot too old".  Fail now.
      FAIL() << "got snapshot too old: " << s;
    }
  });

  // Sleep until we are in the interval
  //   (read_time + history_retention_interval, read_time + slowdown_backfill)
  // = (read_time + 3s, read_time + 10s)
  // Choose read_time + 5s.
  LOG(INFO) << "Sleep...";
  SleepFor(1s); // approximate setup time before getting to the backfill stage
  SleepFor(5s);

  LOG(INFO) << "Flush and compact indexed table...";
  ASSERT_OK(client->FlushTables(
      {table_id},
      false /* add_indexes */,
      kTimeoutSec,
      false /* is_compaction */));
  ASSERT_OK(client->FlushTables(
      {table_id},
      false /* add_indexes */,
      kTimeoutSec,
      true /* is_compaction */));

  LOG(INFO) << "Waiting for create index thread to finish...";
  create_index_thread.join();
}

// Override the index backfill test to have slower backfill-related operations
class PgLibPqTestIndexBackfillSlow : public PgLibPqTestIndexBackfill {
 public:
  PgLibPqTestIndexBackfillSlow() {
    more_master_flags.push_back("--TEST_slowdown_backfill_alter_table_rpcs_ms=7000");
    more_tserver_flags.push_back("--TEST_ysql_index_state_flags_update_delay_ms=7000");
    more_tserver_flags.push_back("--TEST_slowdown_backfill_by_ms=7000");
  }
};

// Make sure that read time (and write time) for backfill works.  Simulate this situation:
//   Session A                                    Session B
//   --------------------------                   ---------------------------------
//   CREATE INDEX
//   - indislive
//   - indisready
//   - backfill
//     - get safe time for read
//                                                UPDATE a row of the indexed table
//     - do the actual backfill
//   - indisvalid
// The backfill should use the values before update when writing to the index.  The update should
// write and delete to the index because of permissions.  Since backfill writes with an ancient
// timestamp, the update should appear to have happened after the backfill.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillReadTime),
          PgLibPqTestIndexBackfillSlow) {
  const MonoDelta& kIndexStateFlagsUpdateDelay = MonoDelta::FromMilliseconds(
      ASSERT_RESULT(CheckedStoi(ASSERT_RESULT(
        cluster_->tserver_daemons()[0]->GetFlag("TEST_ysql_index_state_flags_update_delay_ms")))));
  const MonoDelta& kSlowDownBackfillDelay = MonoDelta::FromMilliseconds(
      ASSERT_RESULT(CheckedStoi(ASSERT_RESULT(
        cluster_->tserver_daemons()[0]->GetFlag("TEST_slowdown_backfill_by_ms")))));
  const std::map<std::string, bool> index_state_flags{
    {"indislive", true},
    {"indisready", true},
    {"indisvalid", false},
  };
  const std::string kIndexName = "rn_idx";
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "rn";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j int, PRIMARY KEY (i ASC))", kTableName));
  for (auto pair = std::make_pair(0, 10); pair.first < 6; ++pair.first, ++pair.second) {
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES ($1, $2)",
                                 kTableName,
                                 pair.first,
                                 pair.second));
  }

  std::vector<std::thread> threads;
  threads.emplace_back([&] {
    auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
    ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0 ON $1 (j ASC)", kIndexName, kTableName));
    {
      // Index scan to verify contents of index table.
      const std::string query = Format("SELECT * FROM $0 WHERE j = 113", kTableName);
      ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
      auto res = ASSERT_RESULT(conn.Fetch(query));
      auto lines = PQntuples(res.get());
      ASSERT_EQ(1, lines);
      auto columns = PQnfields(res.get());
      ASSERT_EQ(2, columns);
      auto key = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
      ASSERT_EQ(key, 3);
      // Make sure that the update is visible.
      auto value = ASSERT_RESULT(GetInt32(res.get(), 0, 1));
      ASSERT_EQ(value, 113);
    }
  });
  threads.emplace_back([&] {
    ASSERT_OK(WaitForBackfillStage(&conn, kIndexName, kIndexStateFlagsUpdateDelay));

    // Give the backfill stage enough time to get a read time.
    // TODO(jason): come up with some way to wait until the read time is chosen rather than relying
    // on a brittle sleep.
    LOG(INFO) << "Waiting out half the delay of executing backfill so that we're hopefully after "
              << "getting the safe read time and before executing backfill";
    SleepFor(kSlowDownBackfillDelay / 2);

    {
      auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
      LOG(INFO) << "Updating row";
      ASSERT_OK(conn.ExecuteFormat("UPDATE $0 SET j = j + 100 WHERE i = 3", kTableName));
      LOG(INFO) << "Done updating row";
    }

    // It should still be in the backfill stage, hopefully before the actual backfill started.
    {
      auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
      ASSERT_TRUE(ASSERT_RESULT(IsAtTargetIndexStateFlags(&conn, kIndexName, index_state_flags)));
    }
  });

  for (auto& thread : threads) {
    thread.join();
  }
}

// Make sure that updates at each stage of multi-stage CREATE INDEX work.  Simulate this situation:
//   Session A                                    Session B
//   --------------------------                   ---------------------------------
//   CREATE INDEX
//   - indislive
//                                                UPDATE a row of the indexed table
//   - indisready
//                                                UPDATE a row of the indexed table
//   - indisvalid
//                                                UPDATE a row of the indexed table
// Updates should succeed and get written to the index.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillPermissions),
          PgLibPqTestIndexBackfillSlow) {
  const auto kIndexStateFlagsWaitTime = 10s;
  const auto kThreadWaitTime = 60s;
  const std::array<std::pair<std::map<std::string, bool>, int>, 3> index_state_flags_key_pairs = {
    std::make_pair(std::map<std::string, bool>{
        {"indislive", true},
        {"indisready", false},
        {"indisvalid", false},
      }, 2),
    std::make_pair(std::map<std::string, bool>{
        {"indislive", true},
        {"indisready", true},
        {"indisvalid", false},
      }, 3),
    std::make_pair(std::map<std::string, bool>{
        {"indislive", true},
        {"indisready", true},
        {"indisvalid", true},
      }, 4),
  };
  const std::string kIndexName = "rn_idx";
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "rn";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j int, PRIMARY KEY (i ASC))", kTableName));
  for (auto pair = std::make_pair(0, 10); pair.first < 6; ++pair.first, ++pair.second) {
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES ($1, $2)",
                                 kTableName,
                                 pair.first,
                                 pair.second));
  }

  std::atomic<int> updates(0);
  TestThreadHolder thread_holder;
  thread_holder.AddThreadFunctor([&] {
    LOG(INFO) << "Begin create thread";
    auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
    ASSERT_OK(conn.ExecuteFormat("CREATE INDEX $0 ON $1 (j ASC)", kIndexName, kTableName));
  });
  thread_holder.AddThreadFunctor([&] {
    LOG(INFO) << "Begin update thread";
    auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
    for (auto pair : index_state_flags_key_pairs) {
      std::map<std::string, bool>& index_state_flags = pair.first;
      int key = pair.second;

      ASSERT_OK(WaitFor(
          std::bind(IsAtTargetIndexStateFlags, &conn, kIndexName, index_state_flags),
          kIndexStateFlagsWaitTime * (index_state_flags["indisvalid"] ? 6 : 1),
          Format("Wait for index state flags to hit target: $0", index_state_flags)));
      LOG(INFO) << "running UPDATE on i = " << key;
      ASSERT_OK(conn.ExecuteFormat("UPDATE $0 SET j = j + 100 WHERE i = $1", kTableName, key));
      LOG(INFO) << "done running UPDATE on i = " << key;

      // Make sure permission didn't change yet.
      ASSERT_TRUE(ASSERT_RESULT(IsAtTargetIndexStateFlags(&conn, kIndexName, index_state_flags)));
      updates++;
    }
  });

  thread_holder.WaitAndStop(kThreadWaitTime);

  ASSERT_EQ(updates.load(std::memory_order_acquire), index_state_flags_key_pairs.size());

  for (auto pair : index_state_flags_key_pairs) {
    int key = pair.second;

    // Verify contents of index table.
    const std::string query = Format(
        "WITH j_idx AS (SELECT * FROM $0 ORDER BY j) SELECT j FROM j_idx WHERE i = $1",
        kTableName,
        key);
    ASSERT_TRUE(ASSERT_RESULT(conn.HasIndexScan(query)));
    auto res = ASSERT_RESULT(conn.Fetch(query));
    int lines = PQntuples(res.get());
    ASSERT_EQ(1, lines);
    int columns = PQnfields(res.get());
    ASSERT_EQ(1, columns);
    // Make sure that the update is visible.
    int value = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(value, key + 110);
  }
}

// Make sure that writes during CREATE UNIQUE INDEX don't cause unique duplicate row errors to be
// thrown.  Simulate this situation:
//   Session A                                    Session B
//   --------------------------                   ---------------------------------
//                                                INSERT a row to the indexed table
//   CREATE UNIQUE INDEX
//                                                INSERT a row to the indexed table
//   - indislive
//                                                INSERT a row to the indexed table
//   - indisready
//                                                INSERT a row to the indexed table
//   - backfill
//                                                INSERT a row to the indexed table
//   - indisvalid
//                                                INSERT a row to the indexed table
// Particularly pay attention to the insert between indisready and backfill.  The insert
// should cause a write to go to the index.  Backfill should choose a read time after this write, so
// it should try to backfill this same row.  Rather than conflicting when we see the row already
// exists in the index during backfill, check whether the rows match, and don't error if they do.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(CreateUniqueIndexWithOnlineWrites),
          PgLibPqTestIndexBackfillSlow) {
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));

  // Start a thread that continuously inserts distinct values.  The hope is that this would cause
  // inserts to happen at all permissions.
  TestThreadHolder thread_holder;
  thread_holder.AddThreadFunctor([this, kTableName, &stop = thread_holder.stop_flag()] {
    auto insert_conn = ASSERT_RESULT(Connect());
    int i = 0;
    while (!stop.load(std::memory_order_acquire)) {
      Status status = insert_conn.ExecuteFormat("INSERT INTO $0 VALUES ($1)", kTableName, ++i);
      if (!status.ok()) {
        // Schema version mismatches will likely occur when changing index permissions, and we can
        // just ignore them for the purposes of this test.
        // TODO(jason): no longer expect these errors after closing issue #3979.
        ASSERT_TRUE(status.IsNetworkError()) << status;
        std::string msg = status.message().ToBuffer();
        ASSERT_TRUE(msg.find("schema version mismatch") != std::string::npos) << status;
      }
    }
  });

  // Create unique index (should not complain about duplicate row).
  ASSERT_OK(conn.ExecuteFormat("CREATE UNIQUE INDEX ON $0 (i ASC)", kTableName));

  thread_holder.Stop();
}

// Simulate this situation:
//   Session A                                    Session B
//   ------------------------------------         -------------------------------------------
//   CREATE TABLE (i, j, PRIMARY KEY (i))
//                                                INSERT (1, 'a')
//   CREATE UNIQUE INDEX (j)
//   - DELETE_ONLY perm
//                                                DELETE (1, 'a')
//                                                (delete (1, 'a') to index)
//                                                INSERT (2, 'a')
//   - WRITE_DELETE perm
//   - BACKFILL perm
//     - get safe time for read
//                                                INSERT (3, 'a')
//                                                (insert (3, 'a') to index)
//     - do the actual backfill
//                                                (insert (2, 'a') to index--detect conflict)
//   - READ_WRITE_DELETE perm
// This test is for issue #6208.
TEST_F_EX(
    PgLibPqTest,
    YB_DISABLE_TEST_IN_TSAN(CreateUniqueIndexWriteAfterSafeTime),
    PgLibPqTestIndexBackfillSlow) {
  auto client = ASSERT_RESULT(cluster_->CreateClient());
  const std::string& kIndexName = "i";
  const std::string& kNamespaceName = "yugabyte";
  const std::string& kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int, j char, PRIMARY KEY (i))", kTableName));
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES (1, 'a')", kTableName));

  std::thread create_index_thread([&] {
    LOG(INFO) << "Creating index";
    auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
    Status s = conn.ExecuteFormat("CREATE UNIQUE INDEX $0 ON $1 (j ASC)", kIndexName, kTableName);
    ASSERT_NOK(s);
    ASSERT_TRUE(s.IsNetworkError());
    ASSERT_TRUE(s.message().ToBuffer().find("duplicate key value")
                != std::string::npos) << s;
  });

  {
    std::map<std::string, bool> index_state_flags = {
      {"indislive", true},
      {"indisready", false},
      {"indisvalid", false},
    };

    LOG(INFO) << "Wait for indislive index state flag";
    // Deadline is 3s
    ASSERT_OK(WaitFor(
        std::bind(IsAtTargetIndexStateFlags, &conn, kIndexName, index_state_flags),
        3s,
        Format("Wait for index state flags to hit target: $0", index_state_flags)));

    LOG(INFO) << "Do insert and delete";
    ASSERT_OK(conn.ExecuteFormat("DELETE FROM $0 WHERE i = 1", kTableName));
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO $0 VALUES (2, 'a')", kTableName));

    LOG(INFO) << "Check we're not yet at indisready index state flag";
    ASSERT_TRUE(ASSERT_RESULT(IsAtTargetIndexStateFlags(&conn, kIndexName, index_state_flags)));
  }

  {
    std::map<std::string, bool> index_state_flags = {
      {"indislive", true},
      {"indisready", true},
      {"indisvalid", false},
    };

    LOG(INFO) << "Wait for indisready index state flag";
    // Deadline is
    //   7s for sleep between indislive and indisready
    // + 3s for extra
    // = 10s
    ASSERT_OK(WaitFor(
        std::bind(IsAtTargetIndexStateFlags, &conn, kIndexName, index_state_flags),
        10s,
        Format("Wait for index state flags to hit target: $0", index_state_flags)));
  }

  {
    LOG(INFO) << "Wait for backfill (approx)";
    // 7s for sleep between indisready and backfill
    SleepFor(7s);

    LOG(INFO) << "Wait to get safe time for backfill (approx)";
    //   7s for sleep between get safe time and do backfill
    // / 2 to get the middle
    // = 3s
    SleepFor(3s);

    LOG(INFO) << "Do insert";
    // Deadline is
    //   4s for remainder of 7s sleep between get safe time and do backfill
    // + 3s for extra
    // = 7s
    CoarseBackoffWaiter waiter(CoarseMonoClock::Now() + 7s, CoarseMonoClock::Duration::max());
    while (true) {
      Status status = conn.ExecuteFormat("INSERT INTO $0 VALUES (3, 'a')", kTableName);
      LOG(INFO) << "Got " << yb::ToString(status);
      if (status.ok()) {
        break;
      } else {
        ASSERT_FALSE(status.IsIllegalState() &&
                     status.message().ToBuffer().find("Duplicate value") != std::string::npos)
            << "The insert should come before backfill, so it should not cause duplicate conflict.";
        ASSERT_TRUE(waiter.Wait());
      }
    }
  }

  LOG(INFO) << "Wait for CREATE INDEX to finish";
  create_index_thread.join();

  // Check.
  {
    CoarseBackoffWaiter waiter(CoarseMonoClock::Now() + 10s, CoarseMonoClock::Duration::max());
    while (true) {
      auto result = conn.FetchFormat("SELECT count(*) FROM $0", kTableName);
      if (result.ok()) {
        auto res = ASSERT_RESULT(result);
        const int64_t main_table_size = ASSERT_RESULT(GetInt64(res.get(), 0, 0));
        ASSERT_EQ(main_table_size, 2);
        break;
      }
      ASSERT_TRUE(result.status().IsQLError()) << result.status();
      ASSERT_TRUE(result.status().message().ToBuffer().find("schema version mismatch")
                  != std::string::npos) << result.status();
      ASSERT_TRUE(waiter.Wait());
    }
  }
}

// Override the index backfill slow test to have smaller WaitUntilIndexPermissionsAtLeast deadline.
class PgLibPqTestIndexBackfillSlowSmallClientDeadline : public PgLibPqTestIndexBackfillSlow {
 public:
  PgLibPqTestIndexBackfillSlowSmallClientDeadline() {
    more_tserver_flags.push_back("--backfill_index_client_rpc_timeout_ms=3000");
  }
};

// Make sure that the postgres timeout when waiting for backfill to finish causes the index to not
// become public.  Simulate this situation:
//   CREATE INDEX
//   - indislive
//   - indisready
//   - backfill
//     - get safe time for read
//   - (timeout)
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillWaitBackfillTimeout),
          PgLibPqTestIndexBackfillSlowSmallClientDeadline) {
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));
  Status status = conn.ExecuteFormat("CREATE INDEX ON $0 (i)", kTableName);
  ASSERT_TRUE(status.IsNetworkError()) << "Got " << status;
  const std::string msg = status.message().ToBuffer();
  ASSERT_TRUE(msg.find("Timed out waiting for Backfill Index") != std::string::npos)
      << status;

  // Make sure that the index is not public.
  ASSERT_FALSE(ASSERT_RESULT(conn.HasIndexScan(Format(
      "SELECT * FROM $0 WHERE i = 1",
      kTableName))));
}

// Make sure that you can still drop an index that failed to fully create.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillDropAfterFail),
          PgLibPqTestIndexBackfillSlowSmallClientDeadline) {
  const std::string kIndexName = "x";
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;

  auto client = ASSERT_RESULT(cluster_->CreateClient());
  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));
  Status status = conn.ExecuteFormat("CREATE INDEX $0 ON $1 (i)", kIndexName, kTableName);
  ASSERT_TRUE(status.IsNetworkError()) << "Got " << status;
  const std::string msg = status.message().ToBuffer();
  ASSERT_TRUE(msg.find("Timed out waiting for Backfill Index") != std::string::npos)
      << status;

  // Make sure that the index exists in DocDB metadata.
  auto tables = ASSERT_RESULT(client->ListTables());
  bool found = false;
  for (const auto& table : tables) {
    if (table.namespace_name() == kNamespaceName && table.table_name() == kIndexName) {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  ASSERT_OK(conn.ExecuteFormat("DROP INDEX $0", kIndexName));

  // Make sure that the index is gone.
  // Check postgres metadata.
  auto res = ASSERT_RESULT(conn.FetchFormat(
      "SELECT COUNT(*) FROM pg_class WHERE relname = '$0'", kIndexName));
  int value = ASSERT_RESULT(GetInt64(res.get(), 0, 0));
  ASSERT_EQ(value, 0);
  // Check DocDB metadata.
  tables = ASSERT_RESULT(client->ListTables());
  for (const auto& table : tables) {
    ASSERT_FALSE(table.namespace_name() == kNamespaceName && table.table_name() == kIndexName);
  }
}

// Override the index backfill slow test to have more than one master and 30s BackfillIndex client
// timeout.
class PgLibPqTestIndexBackfillSlowMultiMaster : public PgLibPqTestIndexBackfillSlow {
 public:
  PgLibPqTestIndexBackfillSlowMultiMaster() {
    more_tserver_flags.push_back("--backfill_index_client_rpc_timeout_ms=30000");
  }

  int GetNumMasters() const override { return 3; }
};

// Make sure that master leader change during backfill causes the index to not become public and
// doesn't cause any weird hangups or other issues.  Simulate this situation:
//   Thread A                                     Thread B
//   --------------------------                   ----------------------
//   CREATE INDEX
//   - indislive
//   - indisready
//   - backfill
//     - get safe time for read
//                                                master leader stepdown
// TODO(jason): update this test when handling master leader changes during backfill (issue #6218).
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillMasterLeaderStepdown),
          PgLibPqTestIndexBackfillSlowMultiMaster) {
  const MonoDelta& kIndexStateFlagsUpdateDelay = MonoDelta::FromMilliseconds(
      ASSERT_RESULT(CheckedStoi(ASSERT_RESULT(
        cluster_->tserver_daemons()[0]->GetFlag("TEST_ysql_index_state_flags_update_delay_ms")))));
  const MonoDelta& kSlowDownBackfillDelay = MonoDelta::FromMilliseconds(
      ASSERT_RESULT(CheckedStoi(ASSERT_RESULT(
        cluster_->tserver_daemons()[0]->GetFlag("TEST_slowdown_backfill_by_ms")))));
  const std::map<std::string, bool> index_state_flags{
    {"indislive", true},
    {"indisready", true},
    {"indisvalid", false},
  };
  const std::string kIndexName = "x";
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));

  std::thread create_index_thread([&] {
    auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
    // The CREATE INDEX should get master leader change during backfill so that its
    // WaitUntilIndexPermissionsAtLeast call starts querying the new leader.  Since the new leader
    // will be inactive at the WRITE_AND_DELETE docdb permission, it will wait until the deadline,
    // which is set to 30s.
    Status status = conn.ExecuteFormat("CREATE INDEX $0 ON $1 (i)", kIndexName, kTableName);
    ASSERT_TRUE(status.IsNetworkError()) << "Got " << status;
    const std::string msg = status.message().ToBuffer();
    ASSERT_TRUE(msg.find("Timed out waiting for Backfill Index") != std::string::npos)
        << status;
  });

  ASSERT_OK(WaitForBackfillStage(&conn, kIndexName, kIndexStateFlagsUpdateDelay));

  LOG(INFO) << "Waiting out half the delay of executing backfill";
  SleepFor(kSlowDownBackfillDelay / 2);

  LOG(INFO) << "Doing master leader stepdown";
  tserver::TabletServerErrorPB::Code error_code;
  ASSERT_OK(cluster_->StepDownMasterLeader(&error_code));

  // It should still be in the backfill stage.
  ASSERT_TRUE(ASSERT_RESULT(IsAtTargetIndexStateFlags(&conn, kIndexName, index_state_flags)));

  LOG(INFO) << "Waiting for create index thread to complete";
  create_index_thread.join();
}

// Make sure that DROP INDEX during backfill is handled well.  Simulate this situation:
//   Thread A                                     Thread B
//   --------------------------                   ----------------------
//   CREATE INDEX
//   - indislive
//   - indisready
//   - backfill
//     - get safe time for read
//                                                DROP INDEX
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillDropWhileBackfilling),
          PgLibPqTestIndexBackfillSlowMultiMaster) {
  const std::string kIndexName = "x";
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";

  auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));

  ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));

  std::thread create_index_thread([&] {
    auto conn = ASSERT_RESULT(ConnectToDB(kNamespaceName));
    Status status = conn.ExecuteFormat("CREATE INDEX $0 ON $1 (i)", kIndexName, kTableName);
    // Expect timeout because
    // DROP INDEX is currently not online and removes the index info from the indexed table
    // ==> the WaitUntilIndexPermissionsAtLeast will keep failing and retrying GetTableSchema on the
    // index.
    ASSERT_TRUE(status.IsNetworkError()) << "Got " << status;
    const std::string msg = status.message().ToBuffer();
    ASSERT_TRUE(msg.find("Timed out waiting for Backfill Index") != std::string::npos)
        << status;
  });

  // Sleep for
  //   7s (delay after committing pg_index indislive)
  // + 7s (delay after committing pg_index indisready)
  // + 3s (to be somewhere in the middle of the 7s delay before doing backfill)
  // = 17s
  LOG(INFO) << "Waiting 17s";
  std::this_thread::sleep_for(17s);
  LOG(INFO) << "Done waiting 17s";

  auto conn2 = ASSERT_RESULT(ConnectToDB(kNamespaceName));
  ASSERT_OK(conn2.ExecuteFormat("DROP INDEX $0", kIndexName));

  LOG(INFO) << "Waiting for create index thread to complete";
  create_index_thread.join();
}

// Override the index backfill test to have different HBA config:
// 1. if any user tries to access the authdb database, enforce md5 auth
// 2. if the postgres user tries to access the yugabyte database, allow it
// 3. if the yugabyte user tries to access the yugabyte database, allow it
// 4. otherwise, disallow it
class PgLibPqTestIndexBackfillAuth : public PgLibPqTestIndexBackfill {
 public:
  PgLibPqTestIndexBackfillAuth() {
    more_tserver_flags.push_back(
        Format(
          "--ysql_hba_conf="
          "host $0 all all md5,"
          "host yugabyte postgres all trust,"
          "host yugabyte yugabyte all trust",
          kAuthDbName));
  }

  const std::string kAuthDbName = "authdb";
};

// Test backfill on clusters where the yugabyte role has authentication enabled.
TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(BackfillAuth),
          PgLibPqTestIndexBackfillAuth) {
  const std::string& kTableName = "t";

  LOG(INFO) << "create " << this->kAuthDbName << " database";
  {
    auto conn = ASSERT_RESULT(ConnectToDB("yugabyte"));
    ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", this->kAuthDbName));
  }

  LOG(INFO) << "backfill table on " << this->kAuthDbName << " database";
  {
    const std::string& host = pg_ts->bind_host();
    const uint16_t port = pg_ts->pgsql_rpc_port();

    auto conn = ASSERT_RESULT(ConnectUsingString(Format(
        "user=$0 password=$1 host=$2 port=$3 dbname=$4",
        "yugabyte",
        "yugabyte",
        host,
        port,
        this->kAuthDbName)));
    ASSERT_OK(conn.ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));
    ASSERT_OK(conn.ExecuteFormat("CREATE INDEX ON $0 (i)", kTableName));
  }
}

// Override the base test to start a cluster with transparent retries on cache version mismatch
// disabled.
class PgLibPqTestNoRetry : public PgLibPqTest {
 public:
  PgLibPqTestNoRetry() {
    more_tserver_flags.push_back("--TEST_ysql_disable_transparent_cache_refresh_retry=true");
  }

  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->extra_master_flags.insert(
        std::end(options->extra_master_flags),
        std::begin(more_master_flags),
        std::end(more_master_flags));
    options->extra_tserver_flags.insert(
        std::end(options->extra_tserver_flags),
        std::begin(more_tserver_flags),
        std::end(more_tserver_flags));
  }

 protected:
  std::vector<std::string> more_master_flags;
  std::vector<std::string> more_tserver_flags;
};

// This test is like "TestPgCacheConsistency#testVersionMismatchWithFailedRetry".  That one gets
// failures because the queries are "parse" message types, and we don't consider retry for those.
// These queries are "simple query" message types, so they should be considered for transparent
// retry.  The last factor is whether `--TEST_ysql_disable_transparent_cache_refresh_retry` is
// specified.
void PgLibPqTest::TestCacheRefreshRetry(const bool is_retry_disabled) {
  constexpr int kNumTries = 5;
  const std::string kNamespaceName = "yugabyte";
  const std::string kTableName = "t";
  int num_successes = 0;
  std::array<PGConn, 2> conns = {
    ASSERT_RESULT(ConnectToDB(kNamespaceName)),
    ASSERT_RESULT(ConnectToDB(kNamespaceName)),
  };

  ASSERT_OK(conns[0].ExecuteFormat("CREATE TABLE $0 (i int)", kTableName));
  // Make the catalog version cache up to date.
  ASSERT_OK(conns[1].FetchFormat("SELECT * FROM $0", kTableName));

  for (int i = 0; i < kNumTries; ++i) {
    ASSERT_OK(conns[0].ExecuteFormat("ALTER TABLE $0 ADD COLUMN j$1 int", kTableName, i));
    auto res = conns[1].FetchFormat("SELECT * FROM $0", kTableName);
    if (is_retry_disabled) {
      // Ensure that we fall under one of two cases:
      // 1. tserver gets updated catalog version before SELECT (rare)
      //    - YBCheckSharedCatalogCacheVersion causes YBRefreshCache
      //    - trying the SELECT requires getting the table schema, but it will be a cache miss since
      //      the whole cache was invalidated, so we get the up-to-date table schema and succeed
      // 2. tserver doesn't get updated catalog version before SELECT (common)
      //    - trying the SELECT causes catalog version mismatch
      if (res.ok()) {
        LOG(WARNING) << "SELECT was ok";
        num_successes++;
        continue;
      }
      auto msg = res.status().message().ToBuffer();
      ASSERT_TRUE(msg.find("Catalog Version Mismatch") != std::string::npos) << res.status();
    } else {
      // Ensure that the request is successful (thanks to retry).
      if (!res.ok()) {
        LOG(WARNING) << "SELECT was not ok: " << res.status();
        continue;
      }
      num_successes++;
    }
    // Make the catalog version cache up to date, if needed.
    ASSERT_OK(conns[1].FetchFormat("SELECT * FROM $0", kTableName));
  }

  LOG(INFO) << "number of successes: " << num_successes << "/" << kNumTries;
  if (is_retry_disabled) {
    // Expect at least half of the tries to fail with catalog version mismatch.  There can be some
    // successes because, between the ALTER and SELECT, the catalog version could have propogated
    // through shared memory (see `YBCheckSharedCatalogCacheVersion`).
    const int num_failures = kNumTries - num_successes;
    ASSERT_GE(num_failures, kNumTries / 2);
  } else {
    // Expect all the tries to succeed.  This is because it is unacceptable to fail when retries are
    // enabled.
    ASSERT_EQ(num_successes, kNumTries);
  }
}

TEST_F_EX(PgLibPqTest,
          YB_DISABLE_TEST_IN_TSAN(CacheRefreshRetryDisabled),
          PgLibPqTestNoRetry) {
  TestCacheRefreshRetry(true /* is_retry_disabled */);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(CacheRefreshRetryEnabled)) {
  TestCacheRefreshRetry(false /* is_retry_disabled */);
}

class PgLibPqDatabaseTimeoutTest : public PgLibPqTest {
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    options->extra_tserver_flags.push_back("--TEST_user_ddl_operation_timeout_sec=1");
    options->extra_master_flags.push_back("--ysql_transaction_bg_task_wait_ms=5000");
  }
};

TEST_F(PgLibPqDatabaseTimeoutTest, YB_DISABLE_TEST_IN_TSAN(TestDatabaseTimeoutGC)) {
  NamespaceName test_name = "test_pgsql";
  auto client = ASSERT_RESULT(cluster_->CreateClient());

  // Create Database: will timeout because the admin setting is lower than the DB create latency.
  {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_NOK(conn.Execute("CREATE DATABASE " + test_name));
  }

  // Verify DocDB Database creation, even though it failed in PG layer.
  // 'ysql_transaction_bg_task_wait_ms' setting ensures we can finish this before the GC.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    Result<bool> ret = client->NamespaceExists(test_name, YQLDatabase::YQL_DATABASE_PGSQL);
    WARN_NOT_OK(ResultToStatus(ret), "" /* prefix */);
    return ret.ok() && ret.get();
  }, MonoDelta::FromSeconds(60),
     "Verify Namespace was created in DocDB"));

  // After bg_task_wait, DocDB will notice the PG layer failure because the transaction aborts.
  // Confirm that DocDB async deletes the namespace.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    Result<bool> ret = client->NamespaceExists(test_name, YQLDatabase::YQL_DATABASE_PGSQL);
    WARN_NOT_OK(ResultToStatus(ret), "ret");
    return ret.ok() && ret.get() == false;
  }, MonoDelta::FromSeconds(20), "Verify Namespace was removed by Transaction GC"));
}

TEST_F(PgLibPqDatabaseTimeoutTest, YB_DISABLE_TEST_IN_TSAN(TestDatabaseTimeoutAndRestartGC)) {
  NamespaceName test_name = "test_pgsql";
  auto client = ASSERT_RESULT(cluster_->CreateClient());

  // Create Database: will timeout because the admin setting is lower than the DB create latency.
  {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_NOK(conn.Execute("CREATE DATABASE " + test_name));
  }

  // Verify DocDB Database creation, even though it fails in PG layer.
  // 'ysql_transaction_bg_task_wait_ms' setting ensures we can finish this before the GC.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    Result<bool> ret = client->NamespaceExists(test_name, YQLDatabase::YQL_DATABASE_PGSQL);
    WARN_NOT_OK(ResultToStatus(ret), "");
    return ret.ok() && ret.get() == true;
  }, MonoDelta::FromSeconds(60),
      "Verify Namespace was created in DocDB"));

  LOG(INFO) << "Restarting Master.";

  // Restart the master before the BG task can kick in and GC the failed transaction.
  auto master = cluster_->GetLeaderMaster();
  master->Shutdown();
  ASSERT_OK(master->Restart());
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    auto s = cluster_->GetIsMasterLeaderServiceReady(master);
    return s.ok();
  }, MonoDelta::FromSeconds(20), "Wait for Master to be ready."));

  // Confirm that Catalog Loader deletes the namespace on master restart.
  client = ASSERT_RESULT(cluster_->CreateClient()); // Reinit the YBClient after restart.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    Result<bool> ret = client->NamespaceExists(test_name, YQLDatabase::YQL_DATABASE_PGSQL);
    WARN_NOT_OK(ResultToStatus(ret), "");
    return ret.ok() && ret.get() == false;
  }, MonoDelta::FromSeconds(20), "Verify Namespace was removed by Transaction GC"));
}

class PgLibPqTableTimeoutTest : public PgLibPqTest {
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* options) override {
    // Use small clock skew, to decrease number of read restarts.
    options->extra_tserver_flags.push_back("--TEST_user_ddl_operation_timeout_sec=1");
    options->extra_master_flags.push_back("--TEST_simulate_slow_table_create_secs=2");
    options->extra_master_flags.push_back("--ysql_transaction_bg_task_wait_ms=3000");
  }
};

TEST_F(PgLibPqTableTimeoutTest, YB_DISABLE_TEST_IN_TSAN(TestTableTimeoutGC)) {
  const string kDatabaseName ="yugabyte";
  NamespaceName test_name = "test_pgsql_table";
  auto client = ASSERT_RESULT(cluster_->CreateClient());

  // Create Database: will timeout because the admin setting is lower than the DB create latency.
  {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_NOK(conn.Execute("CREATE TABLE " + test_name + " (key INT PRIMARY KEY)"));
  }

  // Wait for DocDB Table creation, even though it will fail in PG layer.
  // 'ysql_transaction_bg_task_wait_ms' setting ensures we can finish this before the GC.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    LOG(INFO) << "Requesting TableExists";
    auto ret = client->TableExists(
        client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, test_name));
    WARN_NOT_OK(ResultToStatus(ret), "");
    return ret.ok() && ret.get() == true;
  }, MonoDelta::FromSeconds(20), "Verify Table was created in DocDB"));

  // DocDB will notice the PG layer failure because the transaction aborts.
  // Confirm that DocDB async deletes the namespace.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    auto ret = client->TableExists(
        client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, test_name));
    WARN_NOT_OK(ResultToStatus(ret), "");
    return ret.ok() && ret.get() == false;
  }, MonoDelta::FromSeconds(20), "Verify Table was removed by Transaction GC"));
}

TEST_F(PgLibPqTableTimeoutTest, YB_DISABLE_TEST_IN_TSAN(TestTableTimeoutAndRestartGC)) {
  const string kDatabaseName ="yugabyte";
  NamespaceName test_name = "test_pgsql_table";
  auto client = ASSERT_RESULT(cluster_->CreateClient());

  // Create Database: will timeout because the admin setting is lower than the DB create latency.
  {
    auto conn = ASSERT_RESULT(Connect());
    ASSERT_NOK(conn.Execute("CREATE TABLE " + test_name + " (key INT PRIMARY KEY)"));
  }

  // Wait for DocDB Table creation, even though it will fail in PG layer.
  // 'ysql_transaction_bg_task_wait_ms' setting ensures we can finish this before the GC.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    LOG(INFO) << "Requesting TableExists";
    auto ret = client->TableExists(
        client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, test_name));
    WARN_NOT_OK(ResultToStatus(ret), "");
    return ret.ok() && ret.get() == true;
  }, MonoDelta::FromSeconds(20), "Verify Table was created in DocDB"));

  LOG(INFO) << "Restarting Master.";

  // Restart the master before the BG task can kick in and GC the failed transaction.
  auto master = cluster_->GetLeaderMaster();
  master->Shutdown();
  ASSERT_OK(master->Restart());
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    auto s = cluster_->GetIsMasterLeaderServiceReady(master);
    return s.ok();
  }, MonoDelta::FromSeconds(20), "Wait for Master to be ready."));

  // Confirm that Catalog Loader deletes the namespace on master restart.
  client = ASSERT_RESULT(cluster_->CreateClient()); // Reinit the YBClient after restart.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    auto ret = client->TableExists(
        client::YBTableName(YQL_DATABASE_PGSQL, kDatabaseName, test_name));
    WARN_NOT_OK(ResultToStatus(ret), "");
    return ret.ok() && ret.get() == false;
  }, MonoDelta::FromSeconds(20), "Verify Table was removed by Transaction GC"));
}

namespace {

class CoordinatedRunner {
 public:
  using RepeatableCommand = std::function<Status()>;

  explicit CoordinatedRunner(std::vector<RepeatableCommand> commands)
      : barrier_(commands.size()) {
    threads_.reserve(commands.size());
    for (auto& c : commands) {
      threads_.emplace_back([this, cmd = std::move(c)] () {
        while (!(stop_.load(std::memory_order_acquire) ||
            error_detected_.load(std::memory_order_acquire))) {
          barrier_.Wait();
          const auto status = cmd();
          if (!status.ok()) {
            LOG(ERROR) << "Error detected: " << status;
            error_detected_.store(true, std::memory_order_release);
          }
        }
        barrier_.Detach();
      });
    }
  }

  void Stop() {
    stop_.store(true, std::memory_order_release);
    for (auto& thread : threads_) {
      thread.join();
    }
  }

  bool HasError() {
    return error_detected_.load(std::memory_order_acquire);
  }

 private:
  std::vector<std::thread> threads_;
  Barrier barrier_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> error_detected_{false};
};

bool RetryableError(const Status& status) {
  const auto msg = status.message().ToBuffer();
  const std::string expected_errors[] = {"Try Again",
                                         "Catalog Version Mismatch",
                                         "Restart read required at",
                                         "schema version mismatch for table"};
  for (const auto& expected : expected_errors) {
    if (msg.find(expected) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(PagingReadRestart)) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY)"));
  ASSERT_OK(conn.Execute("INSERT INTO t SELECT generate_series(1, 5000)"));
  const size_t reader_count = 20;
  std::vector<CoordinatedRunner::RepeatableCommand> commands;
  commands.reserve(reader_count + 1);
  commands.emplace_back(
      [connection = std::make_shared<PGConn>(ASSERT_RESULT(Connect()))] () -> Status {
        RETURN_NOT_OK(connection->Execute("ALTER TABLE t ADD COLUMN v INT DEFAULT 100"));
        RETURN_NOT_OK(connection->Execute("ALTER TABLE t DROP COLUMN v"));
        return Status::OK();
  });
  for (size_t i = 0; i < reader_count; ++i) {
    commands.emplace_back(
        [connection = std::make_shared<PGConn>(ASSERT_RESULT(Connect()))] () -> Status {
          const auto res = connection->Fetch("SELECT key FROM t");
          return (res.ok() || RetryableError(res.status())) ? Status::OK() : res.status();
    });
  }
  CoordinatedRunner runner(std::move(commands));
  std::this_thread::sleep_for(10s);
  runner.Stop();
  ASSERT_FALSE(runner.HasError());
}

} // namespace pgwrapper
} // namespace yb
