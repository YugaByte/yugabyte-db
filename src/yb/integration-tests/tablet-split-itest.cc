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

#include "yb/client/client-test-util.h"
#include "yb/client/error.h"
#include "yb/client/ql-dml-test-base.h"
#include "yb/client/session.h"
#include "yb/client/transaction.h"
#include "yb/client/txn-test-base.h"

#include "yb/common/ql_expr.h"
#include "yb/common/ql_value.h"

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus_util.h"

#include "yb/integration-tests/test_workload.h"

#include "yb/master/catalog_manager.h"

#include "yb/yql/cql/ql/util/statement_result.h"

#include "yb/rpc/messenger.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/tserver_admin.proxy.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/protobuf_util.h"
#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"

using namespace std::literals;  // NOLINT

DECLARE_int64(db_write_buffer_size);
DECLARE_int32(rocksdb_level0_file_num_compaction_trigger);
DECLARE_bool(TEST_do_not_start_election_test_only);
DECLARE_int32(TEST_apply_tablet_split_inject_delay_ms);
DECLARE_int32(heartbeat_interval_ms);
DECLARE_int32(leader_lease_duration_ms);
DECLARE_int32(raft_heartbeat_interval_ms);
DECLARE_double(leader_failure_max_missed_heartbeat_periods);

namespace yb {

class TabletSplitITest : public client::TransactionTestBase {
 public:
  void SetUp() override {
    mini_cluster_opt_.num_tablet_servers = 3;
    create_table_ = false;
    client::TransactionTestBase::SetUp();
    proxy_cache_ = std::make_unique<rpc::ProxyCache>(client_->messenger());
  }

  // Creates read request for tablet_id which reflects following query (see
  // client::KeyValueTableTest for schema and kXxx constants):
  // SELECT `kValueColumn` FROM `kTableName` WHERE `kKeyColumn` = `key`;
  // Uses YBConsistencyLevel::CONSISTENT_PREFIX as this is default for YQL clients.
  Result<tserver::ReadRequestPB> CreateReadRequest(const TabletId& tablet_id, int32_t key);

  // Creates write request for tablet_id which reflects following query (see
  // client::KeyValueTableTest for schema and kXxx constants):
  // INSERT INTO `kTableName`(`kValueColumn`) VALUES (`value`);
  tserver::WriteRequestPB CreateInsertRequest(
      const TabletId& tablet_id, int32_t key, int32_t value);

  // Writes `num_rows` rows into test table using `CreateInsertRequest`.
  // Returns a pair with min and max hash code written.
  Result<std::pair<docdb::DocKeyHash, docdb::DocKeyHash>> WriteRows(size_t num_rows);

  Result<docdb::DocKeyHash> WriteRowsAndGetMiddleHashCode(size_t num_rows) {
    auto min_max_hash_code = VERIFY_RESULT(WriteRows(num_rows));
    const auto split_hash_code = (min_max_hash_code.first + min_max_hash_code.second) / 2;
    LOG(INFO) << "Split hash code: " << split_hash_code;
    return split_hash_code;
  }

  Result<scoped_refptr<master::TabletInfo>> GetSingleTestTabletInfo(
      const master::Master& leader_master);

  void WaitForTabletSplitCompletion(
      const size_t num_tablets_before_split = 1, const size_t num_tablets_to_split = 1);

  // Checks all tablet replicas expect ones which have been split to have all rows from 1 to
  // `num_rows` and nothing else.
  void CheckPostSplitTabletReplicasData(size_t num_rows);

  // Checks source tablet behaviour after split:
  // - It should reject reads and writes.
  void CheckSourceTabletAfterSplit(const TabletId& source_tablet_id);

  // Make sure table contains only keys 1...num_keys without gaps.
  void CheckTableKeysInRange(const size_t num_keys);

 protected:
  std::unique_ptr<rpc::ProxyCache> proxy_cache_;
};

class TabletSplitITestWithIsolationLevel : public TabletSplitITest,
                                           public testing::WithParamInterface<IsolationLevel> {
 public:
  void SetUp() override {
    SetIsolationLevel(GetParam());
    TabletSplitITest::SetUp();
  }
};

namespace {

static constexpr auto kRpcTimeout = 60s * kTimeMultiplier;

} // namespace

Result<tserver::ReadRequestPB> TabletSplitITest::CreateReadRequest(
    const TabletId& tablet_id, int32_t key) {
  tserver::ReadRequestPB req;
  auto op = client::CreateReadOp(key, table_, kValueColumn);
  auto* ql_batch = req.add_ql_batch();
  *ql_batch = op->request();

  std::string partition_key;
  RETURN_NOT_OK(op->GetPartitionKey(&partition_key));
  const auto& hash_code = PartitionSchema::DecodeMultiColumnHashValue(partition_key);
  ql_batch->set_hash_code(hash_code);
  ql_batch->set_max_hash_code(hash_code);
  req.set_tablet_id(tablet_id);
  req.set_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
  return req;
}

tserver::WriteRequestPB TabletSplitITest::CreateInsertRequest(
    const TabletId& tablet_id, int32_t key, int32_t value) {
  tserver::WriteRequestPB req;
  auto op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);

  {
    auto op_req = op->mutable_request();
    QLAddInt32HashValue(op_req, key);
    table_.AddInt32ColumnValue(op_req, kValueColumn, value);
  }

  auto* ql_batch = req.add_ql_write_batch();
  *ql_batch = op->request();

  std::string partition_key;
  EXPECT_OK(op->GetPartitionKey(&partition_key));
  const auto& hash_code = PartitionSchema::DecodeMultiColumnHashValue(partition_key);
  ql_batch->set_hash_code(hash_code);
  req.set_tablet_id(tablet_id);
  return req;
}

Result<std::pair<docdb::DocKeyHash, docdb::DocKeyHash>> TabletSplitITest::WriteRows(
    size_t num_rows) {
  auto min_hash_code = std::numeric_limits<docdb::DocKeyHash>::max();
  auto max_hash_code = std::numeric_limits<docdb::DocKeyHash>::min();

  LOG(INFO) << "Writing data...";

  auto txn = CreateTransaction();
  auto session = CreateSession();
  for (auto i = 1; i <= num_rows; ++i) {
    client::YBqlWriteOpPtr op =
        VERIFY_RESULT(WriteRow(session, i /* key */, i /* value */, client::WriteOpType::INSERT));
    const auto hash_code = op->GetHashCode();
    min_hash_code = std::min(min_hash_code, hash_code);
    max_hash_code = std::max(max_hash_code, hash_code);
    YB_LOG_EVERY_N_SECS(INFO, 10) << "Rows written: " << i;
  }
  if (txn) {
    RETURN_NOT_OK(txn->CommitFuture().get());
    LOG(INFO) << "Committed: " << txn->id();
  }

  LOG(INFO) << "Data has been written";
  LOG(INFO) << "min_hash_code = " << min_hash_code;
  LOG(INFO) << "max_hash_code = " << max_hash_code;
  return std::make_pair(min_hash_code, max_hash_code);
}

void TabletSplitITest::WaitForTabletSplitCompletion(
    const size_t num_tablets_before_split, const size_t num_tablets_to_split) {
  LOG(INFO) << "Waiting for tablet split to be completed...";
  std::vector<tablet::TabletPeerPtr> peers;
  auto s = WaitFor([this, &peers, &num_tablets_before_split, &num_tablets_to_split] {
      peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
      size_t num_peers_running = 0;
      size_t num_peers_split = 0;
      size_t num_peers_leader_ready = 0;
      for (const auto& peer : peers) {
        if (!peer->tablet()) {
          break;
        }
        if (peer->tablet()->table_type() == TRANSACTION_STATUS_TABLE_TYPE) {
          continue;
        }
        const auto raft_group_state = peer->state();
        const auto tablet_data_state = peer->tablet()->metadata()->tablet_data_state();
        const auto leader_status = peer->consensus()->GetLeaderStatus(/* allow_stale =*/ true);
        if (raft_group_state == tablet::RaftGroupStatePB::RUNNING) {
          ++num_peers_running;
        } else {
          return false;
        }
        num_peers_leader_ready += leader_status == consensus::LeaderStatus::LEADER_AND_READY;
        num_peers_split +=
            tablet_data_state == tablet::TabletDataState::TABLET_DATA_SPLIT_COMPLETED;
      }
      VLOG(1) << "num_peers_running: " << num_peers_running;
      VLOG(1) << "num_peers_split: " << num_peers_split;
      VLOG(1) << "num_peers_leader_ready: " << num_peers_leader_ready;
      const auto replication_factor = cluster_->num_tablet_servers();

      const auto expected_num_tablets = num_tablets_before_split + 2 * num_tablets_to_split;
      return num_peers_running == replication_factor * expected_num_tablets &&
             num_peers_split == replication_factor * num_tablets_to_split &&
             num_peers_leader_ready == expected_num_tablets;
    }, 20s * kTimeMultiplier, "Wait for tablet split to be completed");
  if (!s.ok()) {
    for (const auto& peer : peers) {
      if (!peer->tablet()) {
        LOG(INFO) << consensus::MakeTabletLogPrefix(peer->tablet_id(), peer->permanent_uuid())
                  << "no tablet";
        continue;
      }
      if (peer->tablet()->table_type() == TRANSACTION_STATUS_TABLE_TYPE) {
        continue;
      }
      LOG(INFO) << consensus::MakeTabletLogPrefix(peer->tablet_id(), peer->permanent_uuid())
                << "raft_group_state: " << AsString(peer->state())
                << " tablet_data_state: "
                << AsString(peer->tablet()->metadata()->tablet_data_state())
                << " leader status: "
                << AsString(peer->consensus()->GetLeaderStatus(/* allow_stale =*/true));
    }
    LOG(INFO) << "Crashing test to avoid waiting on deadlock...";
    raise(SIGSEGV);
  }
}

void TabletSplitITest::CheckPostSplitTabletReplicasData(size_t num_rows) {
  LOG(INFO) << "Checking post-split tablet replicas data...";

  const auto replication_factor = cluster_->num_tablet_servers();

  std::vector<size_t> keys(num_rows, replication_factor);
  const auto key_column_id = table_.ColumnId(kKeyColumn);
  const auto value_column_id = table_.ColumnId(kValueColumn);
  for (auto peer : ListTabletPeers(cluster_.get(), ListPeersFilter::kAll)) {
    const auto* tablet = peer->tablet();
    if (tablet->table_type() == TRANSACTION_STATUS_TABLE_TYPE ||
        tablet->metadata()->tablet_data_state() ==
            tablet::TabletDataState::TABLET_DATA_SPLIT_COMPLETED) {
      continue;
    }

    const SchemaPtr schema = tablet->metadata()->schema();
    auto client_schema = schema->CopyWithoutColumnIds();
    auto iter = ASSERT_RESULT(tablet->NewRowIterator(client_schema, boost::none));
    QLTableRow row;
    std::unordered_set<size_t> tablet_keys;
    while (ASSERT_RESULT(iter->HasNext())) {
      ASSERT_OK(iter->NextRow(&row));
      auto key_opt = row.GetValue(key_column_id);
      ASSERT_TRUE(key_opt.is_initialized());
      ASSERT_EQ(key_opt, row.GetValue(value_column_id));
      auto key = key_opt->int32_value();
      ASSERT_TRUE(tablet_keys.insert(key).second)
          << "Duplicate key " << key << " in tablet " << tablet->tablet_id();
      ASSERT_GT(keys[key - 1]--, 0)
          << "Extra key " << key << " in tablet " << tablet->tablet_id();
    }
  }
  for (auto key = 1; key <= num_rows; ++key) {
    ASSERT_EQ(keys[key - 1], 0) << "Missing key: " << key;
  }
}

void TabletSplitITest::CheckSourceTabletAfterSplit(const TabletId& source_tablet_id) {
  LOG(INFO) << "Checking source tablet behavior after split...";
  google::FlagSaver saver;
  FLAGS_TEST_do_not_start_election_test_only = true;

  size_t tablet_split_insert_error_count = 0;
  size_t not_the_leader_insert_error_count = 0;
  for (auto mini_ts : cluster_->mini_tablet_servers()) {
    auto ts_service_proxy = std::make_unique<tserver::TabletServerServiceProxy>(
        proxy_cache_.get(), HostPort::FromBoundEndpoint(mini_ts->bound_rpc_addr()));

    {
      tserver::ReadRequestPB req = ASSERT_RESULT(CreateReadRequest(source_tablet_id, 1 /* key */));

      rpc::RpcController controller;
      controller.set_timeout(kRpcTimeout);
      tserver::ReadResponsePB resp;
      ts_service_proxy->Read(req, &resp, &controller);

      ASSERT_TRUE(resp.has_error());
      ASSERT_EQ(resp.error().code(), tserver::TabletServerErrorPB::TABLET_SPLIT);
    }

    {
      tserver::WriteRequestPB req =
          CreateInsertRequest(source_tablet_id, 0 /* key */, 0 /* value */);

      rpc::RpcController controller;
      controller.set_timeout(kRpcTimeout);
      tserver::WriteResponsePB resp;
      ts_service_proxy->Write(req, &resp, &controller);

      ASSERT_TRUE(resp.has_error());
      LOG(INFO) << "Error: " << AsString(resp.error());
      switch (resp.error().code()) {
        case tserver::TabletServerErrorPB::TABLET_SPLIT:
          ASSERT_EQ(resp.error().status().code(), AppStatusPB::ILLEGAL_STATE);
          tablet_split_insert_error_count++;
          break;
        case tserver::TabletServerErrorPB::NOT_THE_LEADER:
          not_the_leader_insert_error_count++;
          break;
        default:
          FAIL() << "Unexpected error: " << AsString(resp.error());
      }
    }
  }
  // Leader should return "try again" error on insert.
  ASSERT_EQ(tablet_split_insert_error_count, 1);
  // Followers should return "not the leader" error.
  ASSERT_EQ(not_the_leader_insert_error_count, cluster_->num_tablet_servers() - 1);
}

Result<scoped_refptr<master::TabletInfo>> TabletSplitITest::GetSingleTestTabletInfo(
    const master::Master& leader_master) {
  std::vector<scoped_refptr<master::TabletInfo>> tablet_infos;
  leader_master.catalog_manager()->GetTableInfo(table_->id())->GetAllTablets(&tablet_infos);

  SCHECK_EQ(tablet_infos.size(), 1, IllegalState, "Expect test table to have only 1 tablet");
  return tablet_infos.front();
}

namespace {

Result<size_t> SelectRowsCount(
    const client::YBSessionPtr& session, const client::TableHandle& table) {
  LOG(INFO) << "Running full scan on test table...";
  session->SetTimeout(5s);
  QLPagingStatePB paging_state;
  size_t row_count = 0;
  for (;;) {
    const auto op = table.NewReadOp();
    auto* const req = op->mutable_request();
    req->set_return_paging_state(true);
    if (paging_state.has_table_id()) {
      if (paging_state.has_read_time()) {
        ReadHybridTime read_time = ReadHybridTime::FromPB(paging_state.read_time());
        if (read_time) {
          session->SetReadPoint(read_time);
        }
      }
      session->SetForceConsistentRead(client::ForceConsistentRead::kTrue);
      *req->mutable_paging_state() = std::move(paging_state);
    }
    Status s;
    RETURN_NOT_OK(WaitFor([&] {
      s = session->ApplyAndFlush(op);
      if (s.ok()) {
        return true;
      }
      for (auto& error : session->GetPendingErrors()) {
        if (error->status().IsTryAgain()) {
          return false;
        }
      }
      return true;
    }, 15s, "Waiting for session flush"));
    RETURN_NOT_OK(s);
    auto rowblock = ql::RowsResult(op.get()).GetRowBlock();
    row_count += rowblock->row_count();
    if (!op->response().has_paging_state()) {
      break;
    }
    paging_state = op->response().paging_state();
  }
  return row_count;
}

void DumpTableLocations(
    master::CatalogManager* catalog_mgr, const client::YBTableName& table_name) {
  master::GetTableLocationsResponsePB resp;
  master::GetTableLocationsRequestPB req;
  table_name.SetIntoTableIdentifierPB(req.mutable_table());
  req.set_max_returned_locations(std::numeric_limits<int32_t>::max());
  ASSERT_OK(catalog_mgr->GetTableLocations(&req, &resp));
  LOG(INFO) << "Table locations:";
  for (auto& tablet : resp.tablet_locations()) {
    LOG(INFO) << "Tablet: " << tablet.tablet_id()
              << " partition: " << tablet.partition().ShortDebugString();
  }
}

} // namespace

// Tests splitting of the single tablet in following steps:
// - Create single-tablet table and populates it with specified number of rows.
// - Do full scan using `select count(*)`.
// - Send SplitTablet RPC to the tablet leader.
// - After tablet split is completed - check that new tablets have exactly the same rows.
// - Check that source tablet is rejecting reads and writes.
// - Do full scan using `select count(*)`.
// - Restart cluster.
// - ClusterVerifier will check cluster integrity at the end of the test.

TEST_P(TabletSplitITestWithIsolationLevel, SplitSingleTablet) {
  constexpr auto kNumRows = 500;

  SetNumTablets(1);
  CreateTable();

  // TODO(tsplit): add delay of applying part of intents after tablet is split.
  // TODO(tsplit): test split during read/write workload.
  // TODO(tsplit): test split during long-running transactions.

  const auto split_hash_code = ASSERT_RESULT(WriteRowsAndGetMiddleHashCode(kNumRows));

  auto rows_count = ASSERT_RESULT(SelectRowsCount(NewSession(), table_));
  ASSERT_EQ(rows_count, kNumRows);

  auto& leader_master = *ASSERT_NOTNULL(cluster_->leader_mini_master()->master());

  auto source_tablet_info = ASSERT_RESULT(GetSingleTestTabletInfo(leader_master));
  const auto source_tablet_id = source_tablet_info->id();

  auto* catalog_mgr = leader_master.catalog_manager();

  ASSERT_OK(catalog_mgr->TEST_SplitTablet(source_tablet_info, split_hash_code));

  ASSERT_NO_FATALS(WaitForTabletSplitCompletion());

  ASSERT_NO_FATALS(CheckPostSplitTabletReplicasData(kNumRows));

  ASSERT_NO_FATALS(CheckSourceTabletAfterSplit(source_tablet_id));

  DumpTableLocations(catalog_mgr, client::kTableName);

  rows_count = ASSERT_RESULT(SelectRowsCount(NewSession(), table_));
  ASSERT_EQ(rows_count, kNumRows);

  ASSERT_OK(WriteRows(kNumRows));

  ASSERT_OK(cluster_->RestartSync());
}

// Test for https://github.com/yugabyte/yugabyte-db/issues/4312 reproducing a deadlock
// between TSTabletManager::ApplyTabletSplit and Heartbeater::Thread::TryHeartbeat.
TEST_F(TabletSplitITest, SlowSplitSingleTablet) {
  const auto leader_failure_timeout = FLAGS_leader_failure_max_missed_heartbeat_periods *
        FLAGS_raft_heartbeat_interval_ms;

  FLAGS_TEST_apply_tablet_split_inject_delay_ms = 200 * kTimeMultiplier;
  // We want heartbeater to be called during tablet split apply to reproduce deadlock bug.
  FLAGS_heartbeat_interval_ms = FLAGS_TEST_apply_tablet_split_inject_delay_ms / 3;
  // We reduce FLAGS_leader_lease_duration_ms for ReplicaState::GetLeaderState to avoid always
  // reusing results from cache on heartbeat, otherwise it won't lock ReplicaState mutex.
  FLAGS_leader_lease_duration_ms = FLAGS_TEST_apply_tablet_split_inject_delay_ms / 2;
  // Reduce raft_heartbeat_interval_ms for leader lease to be reliably replicated.
  FLAGS_raft_heartbeat_interval_ms = FLAGS_leader_lease_duration_ms / 2;
  // Keep leader failure timeout the same to avoid flaky losses of leader with short heartbeats.
  FLAGS_leader_failure_max_missed_heartbeat_periods =
      leader_failure_timeout / FLAGS_raft_heartbeat_interval_ms;

  constexpr auto kNumRows = 50;

  SetNumTablets(1);
  CreateTable();

  const auto split_hash_code = ASSERT_RESULT(WriteRowsAndGetMiddleHashCode(kNumRows));

  auto& leader_master = *ASSERT_NOTNULL(cluster_->leader_mini_master()->master());
  auto source_tablet_info = ASSERT_RESULT(GetSingleTestTabletInfo(leader_master));
  auto* catalog_mgr = leader_master.catalog_manager();
  ASSERT_OK(catalog_mgr->TEST_SplitTablet(source_tablet_info, split_hash_code));

  ASSERT_NO_FATALS(WaitForTabletSplitCompletion());
}

void TabletSplitITest::CheckTableKeysInRange(const size_t num_keys) {
  client::TableHandle table;
  ASSERT_OK(table.Open(client::kTableName, client_.get()));

  std::vector<int32> keys;
  for (const auto& row : client::TableRange(table)) {
    keys.push_back(row.column(0).int32_value());
  }

  LOG(INFO) << "Total rows read: " << keys.size();

  std::sort(keys.begin(), keys.end());
  int32 prev_key = 0;
  for (const auto& key : keys) {
    if (key != prev_key + 1) {
      LOG(ERROR) << "Keys missed: " << prev_key + 1 << "..." << key - 1;
    }
    prev_key = key;
  }
  LOG(INFO) << "Last key: " << prev_key;

  ASSERT_EQ(prev_key, num_keys);
  ASSERT_EQ(keys.size(), num_keys);
}

namespace {

void DumpWorkloadStats(const TestWorkload& workload) {
  LOG(INFO) << "Rows inserted: " << workload.rows_inserted();
  LOG(INFO) << "Rows insert failed: " << workload.rows_insert_failed();
  LOG(INFO) << "Rows read ok: " << workload.rows_read_ok();
  LOG(INFO) << "Rows read empty: " << workload.rows_read_empty();
  LOG(INFO) << "Rows read error: " << workload.rows_read_error();
  LOG(INFO) << "Rows read try again: " << workload.rows_read_try_again();
}

} // namespace

TEST_F(TabletSplitITest, SplitTabletDuringReadWriteLoad) {
  constexpr auto kNumTablets = 3;

  FLAGS_db_write_buffer_size = 20_KB;

  TestWorkload workload(cluster_.get());
  workload.set_table_name(client::kTableName);
  workload.set_write_timeout_millis(MonoDelta(kRpcTimeout).ToMilliseconds());
  workload.set_num_tablets(kNumTablets);
  workload.set_num_read_threads(4);
  workload.set_num_write_threads(2);
  workload.set_write_batch_size(50);
  workload.set_payload_bytes(16);
  workload.set_sequential_write(true);
  workload.set_retry_on_restart_required_error(true);
  workload.set_read_only_written_keys(true);
  workload.Setup();

  std::vector<tablet::TabletPeerPtr> peers;
  AssertLoggedWaitFor([this, &peers] {
    peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kLeaders);
    return peers.size() == kNumTablets;
  }, 60s, "Waiting for leaders ...");

  LOG(INFO) << "Starting workload ...";
  workload.Start();

  for (const auto& peer : peers) {
    AssertLoggedWaitFor(
        [&peer] {
          return peer->tablet()->TEST_db()->GetCurrentVersionDataSstFilesSize() >
                 15 * FLAGS_db_write_buffer_size;
    }, 40s * kTimeMultiplier, Format("Writing data to split (tablet $0) ...", peer->tablet_id()));
  }

  DumpWorkloadStats(workload);

  auto& leader_master = *ASSERT_NOTNULL(cluster_->leader_mini_master()->master());
  auto& catalog_mgr = *ASSERT_NOTNULL(leader_master.catalog_manager());

  for (const auto& peer : peers) {
    const auto& source_tablet = *ASSERT_NOTNULL(peer->tablet());
    const auto& source_tablet_id = source_tablet.tablet_id();

    LOG(INFO) << "Tablet: " << source_tablet_id;
    LOG(INFO) << "Number of SST files: " << source_tablet.TEST_db()->GetCurrentVersionNumSSTFiles();

    const auto encoded_split_key = ASSERT_RESULT(source_tablet.GetEncodedMiddleSplitKey());
    const auto doc_key_hash = ASSERT_RESULT(docdb::DecodeDocKeyHash(encoded_split_key)).value();
    LOG(INFO) << "Middle hash key: " << doc_key_hash;
    const auto partition_split_key = PartitionSchema::EncodeMultiColumnHashValue(doc_key_hash);

    ASSERT_OK(catalog_mgr.SplitTablet(source_tablet_id, encoded_split_key, partition_split_key));
  }

  ASSERT_NO_FATALS(WaitForTabletSplitCompletion(kNumTablets, kNumTablets));

  DumpTableLocations(&catalog_mgr, client::kTableName);

  // Generate some more read/write traffic after tablets are split and after that we check data
  // for consistency and that failures rates are acceptable.
  std::this_thread::sleep_for(5s);

  LOG(INFO) << "Stopping workload ...";
  workload.StopAndJoin();

  DumpWorkloadStats(workload);

  ASSERT_NO_FATALS(CheckTableKeysInRange(workload.rows_inserted()));

  const auto insert_failure_rate = 1.0 * workload.rows_insert_failed() / workload.rows_inserted();
  const auto read_failure_rate = 1.0 * workload.rows_read_error() / workload.rows_read_ok();
  const auto read_try_again_rate = 1.0 * workload.rows_read_try_again() / workload.rows_read_ok();

  ASSERT_LT(insert_failure_rate, 0.01);
  ASSERT_LT(read_failure_rate, 0.01);
  // TODO(tsplit): lower this threshold as internal (without reaching client app) read retries
  //  implemented for split tablets.
  ASSERT_LT(read_try_again_rate, 0.1);
  ASSERT_EQ(workload.rows_read_empty(), 0);

  // TODO(tsplit): Check with different isolation levels.
  // TODO(tsplit): Add more splits during writes, so we have tablets with split_depth > 1.

  ASSERT_OK(cluster_->RestartSync());
}

namespace {

PB_ENUM_FORMATTERS(IsolationLevel);

std::string TestParamToString(const testing::TestParamInfo<IsolationLevel>& isolation_level) {
  return ToString(isolation_level.param);
}

} // namespace

INSTANTIATE_TEST_CASE_P(
    TabletSplitITest,
    TabletSplitITestWithIsolationLevel,
    ::testing::ValuesIn(GetAllPbEnumValues<IsolationLevel>()),
    TestParamToString);

}  // namespace yb
