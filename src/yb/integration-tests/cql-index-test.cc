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

#include "yb/integration-tests/cql_test_base.h"

#include "yb/client/table_info.h"

#include "yb/docdb/deadline_info.h"

#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/external_mini_cluster_validator.h"
#include "yb/integration-tests/mini_cluster_utils.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/mini_master.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/transaction_participant.h"
#include "yb/tablet/write_query.h"

#include "yb/util/atomic.h"
#include "yb/util/backoff_waiter.h"
#include "yb/util/logging.h"
#include "yb/util/random_util.h"
#include "yb/util/status_log.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_thread_holder.h"
#include "yb/util/test_util.h"
#include "yb/util/tsan_util.h"

using namespace std::literals;

DECLARE_bool(allow_index_table_read_write);
DECLARE_int32(client_read_write_timeout_ms);
DECLARE_int32(cql_prepare_child_threshold_ms);
DECLARE_bool(disable_index_backfill);
DECLARE_int32(rpc_high_priority_workers_limit);
DECLARE_int32(rpc_workers_limit);
DECLARE_int64(transaction_abort_check_interval_ms);
DECLARE_uint64(transaction_manager_workers_limit);
DECLARE_bool(transactions_poll_check_aborted);

DECLARE_uint32(TEST_abort_create_table);
DECLARE_bool(TEST_disable_proactive_txn_cleanup_on_abort);
DECLARE_int32(TEST_fetch_next_delay_ms);
DECLARE_bool(TEST_simulate_crash_after_table_marked_deleting);
DECLARE_uint64(TEST_inject_txn_get_status_delay_ms);
DECLARE_bool(TEST_writequery_stuck_from_callback_leak);
DECLARE_bool(TEST_simulate_cannot_enable_compactions);

namespace yb {

class CqlIndexTest : public CqlTestBase<MiniCluster> {
 public:
  virtual ~CqlIndexTest() = default;

  void TestTxnCleanup(size_t max_remaining_txns_per_tablet);
  void TestConcurrentModify2Columns(const std::string& expr);
};

YB_STRONGLY_TYPED_BOOL(UniqueIndex);

Status CreateIndexedTable(
    CassandraSession* session, UniqueIndex unique_index = UniqueIndex::kFalse) {
  RETURN_NOT_OK(
      session->ExecuteQuery("CREATE TABLE IF NOT EXISTS t (key INT PRIMARY KEY, value INT) WITH "
                            "transactions = { 'enabled' : true }"));
  return session->ExecuteQuery(
      Format("CREATE $0 INDEX IF NOT EXISTS idx ON T (value)", unique_index ? "UNIQUE" : ""));
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

TEST_F(CqlIndexTest, EmptyIndex) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_disable_index_backfill) = false;
  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

  WARN_NOT_OK(session.ExecuteQuery(
      "CREATE TABLE t (key INT PRIMARY KEY, value INT) WITH transactions = { 'enabled' : true }"),
      "Create table failed.");
  auto future = session.ExecuteGetFuture(
      "CREATE INDEX idx ON T (value) WITH transactions = { 'enabled' : true }");

  constexpr auto kNamespace = "test";
  const client::YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "t");
  const client::YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace, "idx");

  ASSERT_OK(future.Wait());
  auto perm = ASSERT_RESULT(client_->WaitUntilIndexPermissionsAtLeast(
      table_name, index_table_name, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE));
  CHECK_EQ(perm, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  auto index = ASSERT_RESULT(client_->GetYBTableInfo(index_table_name));
  ASSERT_FALSE(index.schema.table_properties().retain_delete_markers());
}

TEST_F(CqlIndexTest, EmptyIndexRecovery) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_cannot_enable_compactions) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_disable_index_backfill) = false;
  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

  WARN_NOT_OK(session.ExecuteQuery(
      "CREATE TABLE ttt(key INT PRIMARY KEY, value INT) WITH transactions = { 'enabled' : true }"),
      "Create table failed.");
  auto future = session.ExecuteGetFuture(
      "CREATE INDEX ttt_idx ON ttt(value) WITH transactions = { 'enabled' : true }");

  constexpr auto kNamespace = "test";
  const client::YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "ttt");
  const client::YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace, "ttt_idx");

  ASSERT_OK(future.Wait());
  {
    auto perm = ASSERT_RESULT(client_->WaitUntilIndexPermissionsAtLeast(
        table_name, index_table_name, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE));
    CHECK_EQ(perm, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
    auto index = ASSERT_RESULT(client_->GetYBTableInfo(index_table_name));
    ASSERT_TRUE(index.schema.table_properties().retain_delete_markers());
  }

  // Restart and make sure retain delete markers value has been fixed (is unset now).
  ASSERT_OK(RestartCluster());
  LOG(INFO) << "Cluster restarted, checking table properties";
  {
    auto index = ASSERT_RESULT(client_->GetYBTableInfo(index_table_name));
    ASSERT_FALSE(index.schema.table_properties().retain_delete_markers());
  }
}

TEST_F(CqlIndexTest, MultipleIndex) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_disable_index_backfill) = false;
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

// Restarts the master after a DROP TABLE on a table with indexes is written to the sys catalog and
// before it is applied in memory. Expects the table and all of its indexes to be deleted.
TEST_F(CqlIndexTest, DropTableWithIndexesIsAtomic) {
  constexpr auto kNamespace = "test";

  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
  ASSERT_OK(session.ExecuteQuery(
      "CREATE TABLE t (key INT PRIMARY KEY, v1 INT, v2 INT) WITH transactions = "
      "{ 'enabled' : true }"));
  ASSERT_OK(session.ExecuteQuery("CREATE INDEX idx1 ON t (v1)"));
  ASSERT_OK(session.ExecuteQuery("CREATE INDEX idx2 ON t (v2)"));

  std::vector<TableId> dropped_table_ids;
  {
    auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager_impl();
    for (const auto* name : {"t", "idx1", "idx2"}) {
      auto table = catalog_manager.GetTableInfoFromNamespaceNameAndTableName(
          YQL_DATABASE_CQL, kNamespace, name);
      ASSERT_NE(table, nullptr) << name;
      dropped_table_ids.push_back(table->id());
    }
  }

  // The failed drop leaves the in-memory table unchanged, so a backfill still updating it would
  // write it back to the sys catalog over the persisted deletion.
  const client::YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "t");
  for (const auto* index_name : {"idx1", "idx2"}) {
    ASSERT_EQ(ASSERT_RESULT(client_->WaitUntilIndexPermissionsAtLeast(
        table_name, client::YBTableName(YQL_DATABASE_CQL, kNamespace, index_name),
        IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE)),
        IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  }
  ASSERT_OK(WaitFor([this, &dropped_table_ids]() -> Result<bool> {
    auto& catalog_manager = VERIFY_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager_impl();
    auto table = catalog_manager.GetTableInfo(dropped_table_ids.front());
    SCHECK(table, NotFound, "Table t not found");
    return !table->IsBackfilling() &&
           table->LockForRead()->pb.state() == master::SysTablesEntryPB::RUNNING;
  }, 60s * kTimeMultiplier, "The table is done altering"));

  // The master persists the deletion and then fails before it applies it in memory.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = true;
  auto drop_status = session.ExecuteQuery("DROP TABLE t");
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_simulate_crash_after_table_marked_deleting) = false;
  ASSERT_NOK(drop_status);

  ASSERT_OK(ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->Restart(
      /* wait_until_catalog_manager_is_leader = */ true));

  auto& catalog_manager = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager_impl();
  ASSERT_OK(WaitFor([&catalog_manager, &dropped_table_ids]() -> Result<bool> {
    for (const auto& table_id : dropped_table_ids) {
      auto table = catalog_manager.GetTableInfo(table_id);
      if (table && !table->LockForRead()->started_deleting()) {
        LOG(INFO) << "Table " << table->ToString() << " is not deleted";
        return false;
      }
    }
    return true;
  }, 60s * kTimeMultiplier, "The dropped table and all its indexes are deleted"));
}

// Fails a CREATE INDEX after the index and the indexed table that references it are persisted,
// then restarts the master. Expects the index to exist and the table to list it, before and after
// the restart.
TEST_F(CqlIndexTest, CreateIndexFailureAfterPersistKeepsIndex) {
  constexpr auto kNamespace = "test";
  // The create table abort flag only applies to tables with this name prefix.
  constexpr auto kIndexName = "test_create_abort_idx";

  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
  ASSERT_OK(session.ExecuteQuery(
      "CREATE TABLE t (key INT PRIMARY KEY, value INT) WITH transactions = { 'enabled' : true }"));

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_abort_create_table) = 2;
  auto create_status = session.ExecuteQuery(Format("CREATE INDEX $0 ON t (value)", kIndexName));
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_abort_create_table) = 0;
  ASSERT_NOK(create_status);

  auto check_index_listed = [this]() -> Status {
    auto& catalog_manager = VERIFY_RESULT(cluster_->GetLeaderMiniMaster())->catalog_manager_impl();
    auto index = catalog_manager.GetTableInfoFromNamespaceNameAndTableName(
        YQL_DATABASE_CQL, kNamespace, kIndexName);
    SCHECK(index, NotFound, "Index not found");
    auto indexed_table = catalog_manager.GetTableInfoFromNamespaceNameAndTableName(
        YQL_DATABASE_CQL, kNamespace, "t");
    SCHECK(indexed_table, NotFound, "Indexed table not found");
    SCHECK(!indexed_table->GetIndexInfo(index->id()).table_id().empty(), IllegalState,
           "Indexed table does not list the index");
    return Status::OK();
  };

  ASSERT_OK(check_index_listed());
  ASSERT_OK(ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->Restart(
      /* wait_until_catalog_manager_is_leader = */ true));
  ASSERT_OK(check_index_listed());
}

TEST_F(CqlIndexTest, RecreateIndex) {
  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
  auto validate = [&](const int value) {
    auto result =
        ASSERT_RESULT(session.ExecuteWithResult(Format("SELECT * FROM t WHERE value = $0", value)));
    auto iter = result.CreateIterator();
    ASSERT_TRUE(iter.Next());
    auto row = iter.Row();
    ASSERT_EQ(row.Value(0).As<cass_int32_t>(), value);
    ASSERT_EQ(row.Value(1).As<cass_int32_t>(), value);
    ASSERT_FALSE(iter.Next());
  };

  ASSERT_OK(session.ExecuteQuery(
      "CREATE TABLE T (key INT PRIMARY KEY, value INT) WITH transactions = { 'enabled' : true }"));
  ASSERT_OK(session.ExecuteQuery("CREATE INDEX idx1 ON T (value)"));
  for (int j = 1; j <= 5; ++j) {
    ASSERT_OK(session.ExecuteQuery(Format("INSERT INTO T (key, value) VALUES ($0, $0)", j)));
  }

  // Validate the index based query.
  validate(2);

  ASSERT_OK(session.ExecuteQuery("DROP INDEX idx1"));
  ASSERT_OK(session.ExecuteQuery("CREATE INDEX idx1 ON T (value)"));

  // wait for backfill.
  std::this_thread::sleep_for(100ms);

  for (int j = 6; j <= 10; ++j) {
    ASSERT_OK(session.ExecuteQueryFormat("INSERT INTO T (key, value) VALUES ($0, $0)", j));
  }

  // Validate the index based query.
  for (int value : {7, 2}) {
    validate(value);
  }
}

class CqlIndexSmallWorkersTest : public CqlIndexTest {
 public:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_rpc_workers_limit) = 4;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_transaction_manager_workers_limit) = 4;
    // The starved pool must be the one serving writes. Leaving consensus on 4 workers too queues
    // UpdateConsensus/RequestConsensusVote for seconds, so no leader can commit its NoOp before
    // the election timeout and the cluster livelocks re-electing instead of exercising the
    // conflict resolution path this test covers.
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_rpc_high_priority_workers_limit) = 256;
    CqlIndexTest::SetUp();
  }
};

TEST_F_EX(CqlIndexTest, ConcurrentIndexUpdate, CqlIndexSmallWorkersTest) {
  constexpr int kThreads = 8;
  constexpr cass_int32_t kKeys = kThreads / 2;
  constexpr cass_int32_t kValues = kKeys;
  constexpr int kNumInserts = kThreads * 5;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_client_read_write_timeout_ms) = 10000 * kTimeMultiplier;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_inject_txn_get_status_delay_ms) = 1000;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_transaction_abort_check_interval_ms) = 100000;

  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

  ASSERT_OK(LoggedWaitFor(
      [&session]() { return CreateIndexedTable(&session).ok(); }, 60s, "create table", 12s));
  constexpr auto kNamespace = "test";
  const client::YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "t");
  const client::YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace, "idx");
  ASSERT_OK(LoggedWaitFor(
      [this, table_name, index_table_name]() {
        auto result = client_->WaitUntilIndexPermissionsAtLeast(
            table_name, index_table_name, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
        return result.ok() && *result == IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE;
      },
      90s,
      "wait for create index to complete",
      12s));

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
          // Errors reported without a round trip (no host available) would otherwise turn this
          // into a busy loop that starves the in-process cluster and the driver IO threads.
          std::this_thread::sleep_for(10ms);
        }
      }
    });
  }

  while (!thread_holder.stop_flag().load(std::memory_order_acquire)) {
    auto num_inserts = inserts.load(std::memory_order_acquire);
    if (num_inserts >= kNumInserts) {
      break;
    }
    YB_LOG_EVERY_N_SECS(INFO, 5) << "Num inserts " << num_inserts << " of " << kNumInserts;
    std::this_thread::sleep_for(100ms);
  }

  thread_holder.Stop();

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_inject_txn_get_status_delay_ms) = 0;
}

int64_t GetFailedBatchLockNum(MiniCluster* cluster)  {
  int64_t failed_batch_lock = 0;
  auto list = ListTabletPeers(cluster, ListPeersFilter::kAll);
  for (const auto& peer : list) {
    auto tablet = peer->shared_tablet_maybe_null();
    if (tablet && tablet->metadata()->table_name() == "t") {
      auto metrics = tablet->metrics();
      if (metrics) {
        failed_batch_lock += metrics->Get(tablet::TabletCounters::kFailedBatchLock);
      }
    }
  }
  return failed_batch_lock;
}

TEST_F(CqlIndexTest, WriteQueryStuckAndUpdateOnSameKey) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_writequery_stuck_from_callback_leak) = true;
  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
  ASSERT_OK(session.ExecuteQuery(
      "CREATE TABLE t(id INT PRIMARY KEY, s TEXT) WITH transactions = { 'enabled' : true };"));
  ASSERT_OK(session.ExecuteQuery("Create index idx on t(id) WHERE id < 0;"));
  std::this_thread::sleep_for(5000ms);
  // Trigger callback leak, which means a WriteQuery never get destroyed.
  ASSERT_NOK(session.ExecuteQuery("INSERT INTO t(id, s) values(-1, 'test');"));
  // Validate that the stuck WriteQuery object block the followup update on same key
  // due to batch lock fail.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_client_read_write_timeout_ms) =
      narrow_cast<uint32_t>(kCassandraTimeOut.ToMilliseconds());
  int64_t failed_batch_lock = GetFailedBatchLockNum(cluster_.get());
  ASSERT_NOK(session.ExecuteQuery("UPDATE t SET s = 'txn' WHERE id = -1;"));
  std::this_thread::sleep_for(1000ms);
  ASSERT_EQ(failed_batch_lock+1, GetFailedBatchLockNum(cluster_.get()));
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_writequery_stuck_from_callback_leak) = false;
}

TEST_F(CqlIndexTest, WriteQueryStuckAndVerifyTxnCleanup) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_writequery_stuck_from_callback_leak) = true;
  const size_t kNumTxns = 100;
  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
  ASSERT_OK(session.ExecuteQuery(
      "CREATE TABLE t(id INT PRIMARY KEY, s TEXT) WITH transactions = { 'enabled' : true };"));
  ASSERT_OK(session.ExecuteQuery("Create index idx on t(id) WHERE id < 0;"));
  std::this_thread::sleep_for(5000ms);
  // Trigger callback leak, which means a WriteQuery never get destroyed.
  ASSERT_NOK(session.ExecuteQuery("INSERT INTO t(id, s) values(-1, 'test');"));
  // Verify the stuck WriteQuery object doesn't block transaction clean up.
  ASSERT_OK(session.ExecuteQuery("INSERT INTO t(id, s) values(0, 'test');"));
  for (size_t i = 0; i < kNumTxns; i++) {
    ASSERT_OK(session.ExecuteQuery(Format("START TRANSACTION;"
                                          " UPDATE t SET s = 'txn$i' WHERE id = 0;"
                                          "COMMIT;", i)));
  }
  std::this_thread::sleep_for(5000ms);
  size_t total_txns = 0;
  auto list = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
  for (const auto& peer : list) {
    auto tablet = ASSERT_RESULT(peer->shared_tablet());
    if (tablet->metadata()->table_name() == "t") {
      auto* participant = tablet->transaction_participant();
      if (participant) {
        total_txns += participant->GetNumRunningTransactions();
      }
    }
  }
  ASSERT_EQ(0, total_txns);
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_writequery_stuck_from_callback_leak) = false;
}

TEST_F(CqlIndexTest, TestSaturatedWorkers) {
  /*
   * (#11258) We set a very short timeout to force failure if child transaction
   * is not created quickly enough.

   * TODO: when switching to a fully asynchronous model, this failure will disappear.
   */
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_cql_prepare_child_threshold_ms) = 0;

  constexpr int kKeys = 10;
  constexpr int kMaxRound = 10;
  Status status;

  for (int round = 0; round < kMaxRound; ++round) {
    auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

    const std::string tbl_name = "tbl" + AsString(round);
    ASSERT_OK(session.ExecuteQuery(
        "CREATE TABLE " + tbl_name + " (key INT PRIMARY KEY, v1 INT, v2 INT) WITH "
        "transactions = { 'enabled' : true }"));
    ASSERT_OK(session.ExecuteQuery(
        "CREATE INDEX " + tbl_name + "_idx1 ON " + tbl_name + "(key, v1) WITH "
        "transactions = { 'enabled' : true }"));
    ASSERT_OK(session.ExecuteQuery(
        "CREATE INDEX " + tbl_name + "_idx2 ON " + tbl_name + "(key, v2) WITH "
        "transactions = { 'enabled' : true }"));

    const client::YBTableName table_name(YQL_DATABASE_CQL, kCqlTestKeyspace, tbl_name);
    const client::YBTableName index_name1(YQL_DATABASE_CQL, kCqlTestKeyspace, tbl_name + "_idx1");
    const client::YBTableName index_name2(YQL_DATABASE_CQL, kCqlTestKeyspace, tbl_name + "_idx2");

    auto perm_idx1 = ASSERT_RESULT(client_->WaitUntilIndexPermissionsAtLeast(
        table_name, index_name1, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE));
    CHECK_EQ(perm_idx1, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
    auto perm_idx2 = ASSERT_RESULT(client_->WaitUntilIndexPermissionsAtLeast(
        table_name, index_name2, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE));
    CHECK_EQ(perm_idx2, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);

    std::string expr = "BEGIN TRANSACTION ";
    for (int i = 0; i < kKeys; i++) {
      expr += Format("INSERT INTO " + tbl_name + " (key, v1, v2) VALUES ($0, $1, $2); ", i, i, i);
    }
    expr += "END TRANSACTION;";

    // We should expect to see timed out error.
    status = session.ExecuteQuery(expr, 120000); // 2 minutes maximum
    LOG(INFO) << "Round " << round << ": Result = " << status;

    if (status.ok() || status.message().ToBuffer().find("Execution Error. ") != std::string::npos) {
      // Ignore the successful OR "Execution Error" results & retry the test once again.
      // During the testing the following rare (1-5%) unexpected results were faced:
      // 1. Successful result.
      // 2. 'Query error' like:
      //    "Execution Error. Write RPC (request call id ID) to IP:PORT timed out after 0.008s"
      // 3. 'Query error' like:
      //    "Execution Error. LookupTabletByKey attempted after deadline expired, "
      //    "passed since deadline: 0.000s"
      SleepFor(MonoDelta::FromMilliseconds(200)); // And continue the loop.
    } else {
      break; // Got the expected error - stop the loop & check the error.
    }
  }

  static const char expected_error[] = "Timed out waiting for prepare child status";
  if (!status.ok()) {
    // Got the expected error - check the error message.
    ASSERT_NE(status.message().ToBuffer().find(expected_error), std::string::npos) << status;
  } else {
    ASSERT_TRUE(false) << "Unexpected case: '" << expected_error << "' error was not detected "
                       << "after " << kMaxRound << " rounds: status = " << status;
  }
}

YB_STRONGLY_TYPED_BOOL(CheckReady);

void CleanFutures(std::deque<CassandraFuture>* futures, CheckReady check_ready) {
  while (!futures->empty() && (!check_ready || futures->front().Ready())) {
    auto status = futures->front().Wait();
    if (!status.ok() && !status.IsTimedOut()) {
      auto msg = status.message().ToBuffer();
      if (msg.find("Duplicate value disallowed by unique index") == std::string::npos) {
        ASSERT_OK(status);
      }
    }
    futures->pop_front();
  }
}

void CqlIndexTest::TestTxnCleanup(size_t max_remaining_txns_per_tablet) {
  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

  ASSERT_OK(CreateIndexedTable(&session, UniqueIndex::kTrue));
  std::deque<CassandraFuture> futures;

  auto prepared = ASSERT_RESULT(session.Prepare("INSERT INTO t (key, value) VALUES (?, ?)"));

  for (int i = 0; i != RegularBuildVsSanitizers(100, 30); ++i) {
    ASSERT_NO_FATALS(CleanFutures(&futures, CheckReady::kTrue));

    auto stmt = prepared.Bind();
    stmt.Bind(0, i);
    stmt.Bind(1, RandomUniformInt<cass_int32_t>(1, 10));
    futures.push_back(session.ExecuteGetFuture(stmt));
  }

  ASSERT_NO_FATALS(CleanFutures(&futures, CheckReady::kFalse));

  AssertRunningTransactionsCountLessOrEqualTo(cluster_.get(), max_remaining_txns_per_tablet);
}

// Test proactive aborted transactions cleanup.
TEST_F(CqlIndexTest, TxnCleanup) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_transactions_poll_check_aborted) = false;

  TestTxnCleanup(/* max_remaining_txns_per_tablet= */ 5);
}

// Test poll based aborted transactions cleanup.
TEST_F(CqlIndexTest, TxnPollCleanup) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_proactive_txn_cleanup_on_abort) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_transaction_abort_check_interval_ms) = 1000;

  TestTxnCleanup(/* max_remaining_txns_per_tablet= */ 0);
}

void CqlIndexTest::TestConcurrentModify2Columns(const std::string& expr) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_allow_index_table_read_write) = true;

  constexpr int kKeys = RegularBuildVsSanitizers(50, 10);

  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
  ASSERT_OK(session.ExecuteQuery(
      "CREATE TABLE t (key INT PRIMARY KEY, v1 INT, v2 INT) WITH "
      "transactions = { 'enabled' : true }"));
  ASSERT_OK(session.ExecuteQuery("CREATE INDEX v1_idx ON t (v1)"));

  // Wait for the table alterations to complete.
  std::this_thread::sleep_for(5000ms);

  auto prepared1 = ASSERT_RESULT(session.Prepare(Format(expr, "v1")));
  auto prepared2 = ASSERT_RESULT(session.Prepare(Format(expr, "v2")));

  std::vector<CassandraFuture> futures;

  for (int i = 0; i != kKeys; ++i) {
    for (auto* prepared : {&prepared1, &prepared2}) {
      auto statement = prepared->Bind();
      statement.Bind(0, i);
      statement.Bind(1, i);
      futures.push_back(session.ExecuteGetFuture(statement));
    }
  }

  int good = 0;
  int bad = 0;
  for (auto& future : futures) {
    auto status = future.Wait();
    if (status.ok()) {
      ++good;
    } else {
      ASSERT_TRUE(status.IsTimedOut()) << status;
      ++bad;
    }
  }

  ASSERT_GE(good, bad * 4);

  std::vector<bool> present(kKeys);

  int read_keys = 0;
  auto result = ASSERT_RESULT(session.ExecuteWithResult("SELECT * FROM v1_idx"));
  auto iter = result.CreateIterator();
  while (iter.Next()) {
    auto row = iter.Row();
    auto key = row.Value(0).As<int32_t>();
    auto value = row.Value(1);
    LOG(INFO) << key << ", " << value.ToString();

    ASSERT_FALSE(present[key]) << key;
    present[key] = true;
    ++read_keys;
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(key, value.As<int32_t>());
  }

  ASSERT_EQ(read_keys, kKeys);
}

TEST_F(CqlIndexTest, ConcurrentInsert2Columns) {
  TestConcurrentModify2Columns("INSERT INTO t ($0, key) VALUES (?, ?)");
}

TEST_F(CqlIndexTest, ConcurrentUpdate2Columns) {
  TestConcurrentModify2Columns("UPDATE t SET $0 = ? WHERE key = ?");
}

// Deletes a whole partition of an indexed table with a single range delete (hash key given, range
// key omitted), and checks that all the rows and all their index entries are gone.
TEST_F(CqlIndexTest, RangeDeleteWithIndex) {
  constexpr auto kNumRows = 16;
  constexpr auto kNamespace = "test";

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_allow_index_table_read_write) = true;

  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
  ASSERT_OK(session.ExecuteQuery(
      "CREATE TABLE t (h INT, r INT, value INT, PRIMARY KEY ((h), r)) WITH "
      "transactions = { 'enabled' : true }"));
  ASSERT_OK(session.ExecuteQuery("CREATE INDEX idx ON t (value)"));
  ASSERT_OK(client_->WaitUntilIndexPermissionsAtLeast(
      client::YBTableName(YQL_DATABASE_CQL, kNamespace, "t"),
      client::YBTableName(YQL_DATABASE_CQL, kNamespace, "idx"),
      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE));

  for (int r = 1; r <= kNumRows; ++r) {
    ASSERT_OK(session.ExecuteQuery(Format("INSERT INTO t (h, r, value) VALUES (1, $0, $0)", r)));
  }
  ASSERT_EQ(ASSERT_RESULT(session.FetchValue<int64_t>("SELECT COUNT(*) FROM idx")), kNumRows);

  ASSERT_OK(session.ExecuteQuery("DELETE FROM t WHERE h = 1"));

  ASSERT_EQ(ASSERT_RESULT(session.FetchValue<int64_t>("SELECT COUNT(*) FROM t")), 0);
  ASSERT_EQ(ASSERT_RESULT(session.FetchValue<int64_t>("SELECT COUNT(*) FROM idx")), 0);
}

TEST_F(CqlIndexTest, SlowIndexResponse) {
  constexpr auto kNumKeys = NonTsanVsTsan(3000, 1500);
  constexpr auto kWriteBatchSize = NonTsanVsTsan(100, 30);
  constexpr auto kNumWriteThreads = 8;
  constexpr auto kFixedCategory = 0;
  constexpr auto kTestReadDelayMs = 1;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_client_read_write_timeout_ms) = 100000 * kTimeMultiplier;

  {
    auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

    ASSERT_OK(session.ExecuteQuery(
        "CREATE TABLE test_table (key INT PRIMARY KEY, category INT, value INT) WITH "
        "transactions = { 'enabled' : true }"));
    ASSERT_OK(session.ExecuteQuery(
        "CREATE INDEX category_idx ON test_table (category) WITH TABLETS = 1"));

    std::atomic<int> num_rows = 0;

    auto writer = [&num_rows, kFixedCategory, this]() -> void {
      auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
      auto prepared = ASSERT_RESULT(
          session.Prepare("INSERT INTO test_table (key, category, value) VALUES (?, ?, ?)"));
      while (num_rows < kNumKeys) {
        CassandraBatch batch(CassBatchType::CASS_BATCH_TYPE_LOGGED);
        for (int i = 0; i < kWriteBatchSize; ++i) {
          const auto prev_num_rows = num_rows.fetch_add(1);
          if (prev_num_rows == kNumKeys) {
            num_rows.fetch_sub(1);
            break;
          }
          auto stmt = prepared.Bind();
          stmt.Bind(0, prev_num_rows);
          stmt.Bind(1, kFixedCategory);
          stmt.Bind(2, prev_num_rows);
          batch.Add(&stmt);
        }

        ASSERT_OK(session.ExecuteBatch(batch));
        YB_LOG_EVERY_N_SECS(INFO, 5) << "Inserted " << num_rows << " rows";
      }
    };

    TestThreadHolder writers;
    for (int i = 0; i < kNumWriteThreads; ++i) {
      writers.AddThreadFunctor(writer);
    }
    writers.JoinAll();

    LOG(INFO) << "Inserted " << num_rows << " rows";

    NO_PENDING_FATALS();
  }

  ASSERT_OK(WaitForAllIntentsApplied(cluster_.get()));
  ASSERT_OK(cluster_->FlushTablets());

  // Restart cluster so MetaCache is cleared.
  ASSERT_OK(RestartCluster());

  auto session = ASSERT_RESULT(EstablishSession(driver_.get()));

  LOG(INFO) << "Running SELECT";

  // We want index tablet scan to take >= FLAGS_client_read_write_timeout_ms in order to reproduce
  // scenario where MetaCache is trying to lookup tablets based on keys returned by index after
  // hitting deadline.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_client_read_write_timeout_ms) = kNumKeys * kTestReadDelayMs;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_fetch_next_delay_ms) = kTestReadDelayMs;

  CountDownLatch latch(1);
  TestThreadHolder select_thread;
  select_thread.AddThreadFunctor([&session, &latch, kFixedCategory]{
    Stopwatch sw;
    sw.start();
    auto result = session.ExecuteWithResult(
        Format("SELECT * FROM test_table WHERE category = $0", kFixedCategory));
    sw.stop();
    latch.CountDown();
    ASSERT_NOK(result) << "Expected SELECT to fail due to time out, but got: " << [&result]() {
      auto iter = result->CreateIterator();
      return iter.Next() ? AsString(iter.Row().Value(0).As<int64>()) : "<none>";
    }();
    ASSERT_GE(sw.elapsed().wall_millis(), FLAGS_client_read_write_timeout_ms)
        << "SELECT failed too early";
  });

  const auto timeout_limit_ms =
      FLAGS_client_read_write_timeout_ms +
      narrow_cast<int32_t>(kTestReadDelayMs * docdb::kDeadlineCheckGranularity * 1.1);
  const auto latch_done = latch.WaitFor(timeout_limit_ms * 1ms);
  LOG(INFO) << "Latch done: " << latch_done;
  ASSERT_TRUE(latch_done) << "SELECT hasn't completed within " << timeout_limit_ms << " ms";

  select_thread.JoinAll();
}

class CqlIndexExternalMiniClusterTest : public CqlTestBase<ExternalMiniCluster> {
 protected:
  void TestRetainDeleteMarkersRecovery(bool use_multiple_requests) {
    ASSERT_OK(EnsureClientCreated());
    auto validator =
        CqlRetainDeleteMarkersValidator{ cluster_.get(), client_.get(), driver_.get() };
    validator.TestRecovery(use_multiple_requests);
  }

 private:
  class CqlRetainDeleteMarkersValidator final : public itest::RetainDeleteMarkersValidator {
    using Base = itest::RetainDeleteMarkersValidator;

   public:
    CqlRetainDeleteMarkersValidator(
        ExternalMiniCluster* cluster, client::YBClient* client, CppCassandraDriver* driver)
        : Base(cluster, client, kCqlTestKeyspace), driver_(*CHECK_NOTNULL(driver)) {
    }

   private:
    Status RestartCluster() override {
      RETURN_NOT_OK(Base::RestartCluster());
      session_ = VERIFY_RESULT(EstablishSession(&driver_));
      return Status::OK();
    }

    Status CreateIndex(const std::string &index_name, const std::string &table_name) override {
      return session_.ExecuteQueryFormat(
          "CREATE INDEX $0 ON $1(value) WITH transactions = { 'enabled' : true }",
          index_name, table_name);
    }

    Status CreateTable(const std::string &table_name) override {
      return session_.ExecuteQueryFormat(
          "CREATE TABLE $0 (key INT PRIMARY KEY, value INT) "
          "WITH transactions = { 'enabled' : true }", table_name);
    }

    CppCassandraDriver& driver_;
    CassandraSession session_;
  };
};

// Test for https://github.com/yugabyte/yugabyte-db/issues/19731.
TEST_F(CqlIndexExternalMiniClusterTest, RetainDeleteMarkersRecovery) {
  TestRetainDeleteMarkersRecovery(false /* use_multiple_requests */);
}

// Test for https://github.com/yugabyte/yugabyte-db/issues/19731.
TEST_F(CqlIndexExternalMiniClusterTest, RetainDeleteMarkersRecoveryViaSeveralRequests) {
  TestRetainDeleteMarkersRecovery(true /* use_multiple_requests */);
}

} // namespace yb
