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

#include "yb/integration-tests/cql_test_base.h"

#include "yb/util/random_util.h"
#include "yb/util/test_util.h"

using namespace std::literals;

DECLARE_bool(disable_index_backfill);
DECLARE_int32(client_read_write_timeout_ms);
DECLARE_int32(rpc_workers_limit);
DECLARE_uint64(transaction_manager_workers_limit);
DECLARE_uint64(TEST_inject_txn_get_status_delay_ms);

namespace yb {

class CqlIndexTest : public CqlTestBase {
 public:
  virtual ~CqlIndexTest() = default;
};

CHECKED_STATUS CreateIndexedTable(CassandraSession* session) {
  RETURN_NOT_OK(
      session->ExecuteQuery("CREATE TABLE IF NOT EXISTS t (key INT PRIMARY KEY, value INT) WITH "
                            "transactions = { 'enabled' : true }"));
  return session->ExecuteQuery("CREATE INDEX IF NOT EXISTS idx ON T (value)");
}

TEST_F(CqlIndexTest, Simple) {
  constexpr int kKey = 1;
  constexpr int kValue = 2;

  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

  ASSERT_OK(CreateIndexedTable(&session));

  ASSERT_OK(session.ExecuteQuery("INSERT INTO t (key, value) VALUES (1, 2)"));
  auto result = ASSERT_RESULT(session.ExecuteWithResult("SELECT * FROM t WHERE value = 2"));
  auto iter = result.CreateIterator();
  ASSERT_TRUE(iter.Next());
  auto row = iter.Row();
  ASSERT_EQ(row.Value(0).As<cass_int32_t>(), kKey);
  ASSERT_EQ(row.Value(1).As<cass_int32_t>(), kValue);
  ASSERT_FALSE(iter.Next());
}

TEST_F(CqlIndexTest, MultipleIndex) {
  FLAGS_disable_index_backfill = false;
  auto session1 = ASSERT_RESULT(EstablishSession(driver_.get()));
  auto session2 = ASSERT_RESULT(EstablishSession(driver_.get()));

  WARN_NOT_OK(session1.ExecuteQuery(
      "CREATE TABLE t (key INT PRIMARY KEY, value INT) WITH transactions = { 'enabled' : true }"),
          "Create table failed.");
  auto future1 = session1.ExecuteGetFuture("CREATE INDEX idx1 ON T (value)");
  auto future2 = session2.ExecuteGetFuture("CREATE INDEX idx2 ON T (value)");

  constexpr auto kNamespace = "test";
  const client::YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "t");
  const client::YBTableName index_table_name1(YQL_DATABASE_CQL, kNamespace, "idx1");
  const client::YBTableName index_table_name2(YQL_DATABASE_CQL, kNamespace, "idx2");

  LOG(INFO) << "Waiting for idx1 got " << future1.Wait();
  auto perm1 = ASSERT_RESULT(client_->WaitUntilIndexPermissionsAtLeast(
      table_name, index_table_name1, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE));
  CHECK_EQ(perm1, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  LOG(INFO) << "Waiting for idx2 got " << future2.Wait();
  auto perm2 = ASSERT_RESULT(client_->WaitUntilIndexPermissionsAtLeast(
      table_name, index_table_name2, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE));
  CHECK_EQ(perm2, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
}

class CqlIndexSmallWorkersTest : public CqlIndexTest {
 public:
  void SetUp() override {
    FLAGS_rpc_workers_limit = 4;
    FLAGS_transaction_manager_workers_limit = 4;
    CqlIndexTest::SetUp();
  }
};

TEST_F_EX(CqlIndexTest, ConcurrentIndexUpdate, CqlIndexSmallWorkersTest) {
  constexpr int kThreads = 8;
  constexpr cass_int32_t kKeys = kThreads / 2;
  constexpr cass_int32_t kValues = kKeys;
  constexpr int kNumInserts = kThreads * 5;

  FLAGS_client_read_write_timeout_ms = 10000;
  SetAtomicFlag(1000, &FLAGS_TEST_inject_txn_get_status_delay_ms);

  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

  AssertLoggedWaitFor(
      [&session]() { return CreateIndexedTable(&session).ok(); }, 60s, "create table", 12s);
  constexpr auto kNamespace = "test";
  const client::YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "t");
  const client::YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace, "idx");
  AssertLoggedWaitFor(
      [this, table_name, index_table_name]() {
        auto result = client_->WaitUntilIndexPermissionsAtLeast(
            table_name, index_table_name, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
        return result.ok() && *result == IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE;
      },
      90s,
      "wait for create index to complete",
      12s);

  TestThreadHolder thread_holder;
  std::atomic<int> inserts(0);
  for (int i = 0; i != kThreads; ++i) {
    thread_holder.AddThreadFunctor([this, &inserts, &stop = thread_holder.stop_flag()] {
      auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
      auto prepared = ASSERT_RESULT(session.Prepare("INSERT INTO t (key, value) VALUES (?, ?)"));
      while (!stop.load(std::memory_order_acquire)) {
        auto stmt = prepared.Bind();
        stmt.Bind(0, RandomUniformInt<cass_int32_t>(1, kKeys));
        stmt.Bind(1, RandomUniformInt<cass_int32_t>(1, kValues));
        auto status = session.Execute(stmt);
        if (status.ok()) {
          ++inserts;
        } else {
          LOG(INFO) << "Insert failed: " << status;
        }
      }
    });
  }

  while (!thread_holder.stop_flag().load(std::memory_order_acquire)) {
    if (inserts.load(std::memory_order_acquire) >= kNumInserts) {
      break;
    }
  }

  thread_holder.Stop();

  SetAtomicFlag(0, &FLAGS_TEST_inject_txn_get_status_delay_ms);
}

} // namespace yb
