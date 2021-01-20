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

#include <chrono>
#include <thread>
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

#include "yb/docdb/doc_key.h"
#include "yb/docdb/docdb_rocksdb_util.h"

#include "yb/gutil/dynamic_annotations.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/test_workload.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/master.pb.h"
#include "yb/master/master_error.h"

#include "yb/util/tsan_util.h"
#include "yb/yql/cql/ql/util/statement_result.h"

#include "yb/rpc/messenger.h"

#include "yb/tablet/tablet_metadata.h"

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

DECLARE_int64(db_block_size_bytes);
DECLARE_int64(db_write_buffer_size);
DECLARE_int32(rocksdb_level0_file_num_compaction_trigger);
DECLARE_bool(TEST_do_not_start_election_test_only);
DECLARE_int32(TEST_apply_tablet_split_inject_delay_ms);
DECLARE_int32(heartbeat_interval_ms);
DECLARE_int32(leader_lease_duration_ms);
DECLARE_int32(raft_heartbeat_interval_ms);
DECLARE_double(leader_failure_max_missed_heartbeat_periods);
DECLARE_int32(cleanup_split_tablets_interval_sec);
DECLARE_bool(TEST_skip_deleting_split_tablets);
DECLARE_int32(replication_factor);
DECLARE_int32(tablet_split_limit_per_table);
DECLARE_bool(TEST_pause_before_post_split_compation);

namespace yb {

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
      for (auto& error : session->GetAndClearPendingErrors()) {
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

class TabletSplitITest : public client::TransactionTestBase<MiniCluster> {
 public:
  void SetUp() override {
    FLAGS_cleanup_split_tablets_interval_sec = 1;
    mini_cluster_opt_.num_tablet_servers = 3;
    create_table_ = false;
    TransactionTestBase::SetUp();
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
  Result<std::pair<docdb::DocKeyHash, docdb::DocKeyHash>> WriteRows(
      size_t num_rows, size_t start_key);

  Result<docdb::DocKeyHash> WriteRowsAndGetMiddleHashCode(size_t num_rows) {
    auto min_max_hash_code = VERIFY_RESULT(WriteRows(num_rows, 1));
    const auto split_hash_code = (min_max_hash_code.first + min_max_hash_code.second) / 2;
    LOG(INFO) << "Split hash code: " << split_hash_code;

    RETURN_NOT_OK(CheckRowsCount(num_rows));

    return split_hash_code;
  }

  master::CatalogManager* catalog_manager() {
    return CHECK_NOTNULL(cluster_->leader_mini_master()->master())->catalog_manager();
  }

  Result<scoped_refptr<master::TabletInfo>> GetSingleTestTabletInfo(
      master::CatalogManager* catalog_manager);

  void CreateSingleTablet() {
    SetNumTablets(1);
    CreateTable();
  }

  Result<TabletId> SplitTabletAndValidate(docdb::DocKeyHash split_hash_code, size_t num_rows) {
    auto* catalog_mgr = catalog_manager();

    auto source_tablet_info = VERIFY_RESULT(GetSingleTestTabletInfo(catalog_mgr));
    const auto source_tablet_id = source_tablet_info->id();

    RETURN_NOT_OK(catalog_mgr->TEST_SplitTablet(source_tablet_info, split_hash_code));

    const auto expected_split_tablets = FLAGS_TEST_skip_deleting_split_tablets ? 1 : 0;

    WaitForTabletSplitCompletion(/* expected_non_split_tablets =*/ 2, expected_split_tablets);

    RETURN_NOT_OK(CheckPostSplitTabletReplicasData(num_rows));

    if (expected_split_tablets > 0) {
      RETURN_NOT_OK(CheckSourceTabletAfterSplit(source_tablet_id));
    }

    return source_tablet_id;
  }

  Result<TabletId> CreateSingleTabletAndSplit(size_t num_rows) {
    CreateSingleTablet();
    const auto split_hash_code = VERIFY_RESULT(WriteRowsAndGetMiddleHashCode(num_rows));
    return SplitTabletAndValidate(split_hash_code, num_rows);
  }

  CHECKED_STATUS CheckRowsCount(size_t expected_num_rows) {
    auto rows_count = VERIFY_RESULT(SelectRowsCount(NewSession(), table_));
    SCHECK_EQ(rows_count, expected_num_rows, InternalError, "Got unexpected rows count");
    return Status::OK();
  }

  // By default we wait until all split tablets are cleanup. expected_split_tablets could be
  // overridden if needed to test behaviour of split tablet when its deletion is disabled.
  // If num_replicas_online is 0, uses replication factor.
  void WaitForTabletSplitCompletion(
      const size_t expected_non_split_tablets, const size_t expected_split_tablets = 0,
      size_t num_replicas_online = 0);

  // Wait for all peers to complete post-split compaction.
  void WaitForTestTableTabletsCompactionFinish(MonoDelta timeout);

  // Returns all tablet peers in the cluster which are marked as being in
  // TABLET_DATA_SPLIT_COMPLETED state. In most of the test cases below, this corresponds to the
  // post-split parent/source tablet peers.
  Result<std::vector<tablet::TabletPeerPtr>> ListSplitCompleteTabletPeers();

  // Returns all tablet peers in the cluster which are not part of a transaction table and which are
  // not in TABLET_DATA_SPLIT_COMPLETED state. In most of the test cases below, this corresponds to
  // post-split children tablet peers.
  Result<std::vector<tablet::TabletPeerPtr>> ListPostSplitChildrenTabletPeers();

  // Verify that the db has queued, is running, or did complete a post split compaction.
  void VerifyTriggeredPostSplitCompaction(int num_peers);

  // Returns the bytes read at the RocksDB layer by each split child tablet.
  Result<uint64_t> GetActiveTabletsBytesRead();

  // Returns the bytes written at the RocksDB layer by the split parent tablet.
  Result<uint64_t> GetInactiveTabletsBytesWritten();

  // Checks all tablet replicas expect ones which have been split to have all rows from 1 to
  // `num_rows` and nothing else.
  // If num_replicas_online is 0, uses replication factor.
  CHECKED_STATUS CheckPostSplitTabletReplicasData(size_t num_rows, size_t num_replicas_online = 0);

  // Checks source tablet behaviour after split:
  // - It should reject reads and writes.
  CHECKED_STATUS CheckSourceTabletAfterSplit(const TabletId& source_tablet_id);

  // Make sure table contains only keys 1...num_keys without gaps.
  void CheckTableKeysInRange(const size_t num_keys);

  // Tests appropriate client requests structure update at YBClient side.
  // split_depth specifies how deep should we split original tablet until trying to write again.
  void SplitClientRequestsIds(int split_depth);

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
    const size_t num_rows, const size_t start_key) {
  auto min_hash_code = std::numeric_limits<docdb::DocKeyHash>::max();
  auto max_hash_code = std::numeric_limits<docdb::DocKeyHash>::min();

  LOG(INFO) << "Writing " << num_rows << " rows...";

  auto txn = CreateTransaction();
  auto session = CreateSession();
  for (auto i = start_key; i < start_key + num_rows; ++i) {
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

  LOG(INFO) << num_rows << " rows has been written";
  LOG(INFO) << "min_hash_code = " << min_hash_code;
  LOG(INFO) << "max_hash_code = " << max_hash_code;
  return std::make_pair(min_hash_code, max_hash_code);
}

void TabletSplitITest::WaitForTabletSplitCompletion(
    const size_t expected_non_split_tablets, const size_t expected_split_tablets,
    size_t num_replicas_online) {
  if (num_replicas_online == 0) {
    num_replicas_online = FLAGS_replication_factor;
  }

  LOG(INFO) << "Waiting for tablet split to be completed... ";
  LOG(INFO) << "expected_non_split_tablets: " << expected_non_split_tablets;
  LOG(INFO) << "expected_split_tablets: " << expected_split_tablets;

  const auto expected_total_tablets = expected_non_split_tablets + expected_split_tablets;
  LOG(INFO) << "expected_total_tablets: " << expected_total_tablets;

  std::vector<tablet::TabletPeerPtr> peers;
  auto s = WaitFor([&] {
      peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
      size_t num_peers_running = 0;
      size_t num_peers_split = 0;
      size_t num_peers_leader_ready = 0;
      for (const auto& peer : peers) {
        const auto tablet = peer->shared_tablet();
        const auto consensus = peer->shared_consensus();
        if (!tablet || !consensus) {
          break;
        }
        if (tablet->table_type() == TRANSACTION_STATUS_TABLE_TYPE) {
          continue;
        }
        const auto raft_group_state = peer->state();
        const auto tablet_data_state = tablet->metadata()->tablet_data_state();
        const auto leader_status = consensus->GetLeaderStatus(/* allow_stale =*/true);
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

    return num_peers_running == num_replicas_online * expected_total_tablets &&
           num_peers_split == num_replicas_online * expected_split_tablets &&
           num_peers_leader_ready == expected_total_tablets;
  }, 20s * kTimeMultiplier, "Wait for tablet split to be completed");
  if (!s.ok()) {
    for (const auto& peer : peers) {
      const auto tablet = peer->shared_tablet();
      const auto consensus = peer->shared_consensus();
      if (!tablet || !consensus) {
        LOG(INFO) << consensus::MakeTabletLogPrefix(peer->tablet_id(), peer->permanent_uuid())
                  << "no tablet";
        continue;
      }
      if (tablet->table_type() == TRANSACTION_STATUS_TABLE_TYPE) {
        continue;
      }
      LOG(INFO) << consensus::MakeTabletLogPrefix(peer->tablet_id(), peer->permanent_uuid())
                << "raft_group_state: " << AsString(peer->state())
                << " tablet_data_state: "
                << TabletDataState_Name(tablet->metadata()->tablet_data_state())
                << " leader status: "
                << AsString(consensus->GetLeaderStatus(/* allow_stale =*/true));
    }
    LOG(INFO) << "Crashing test to avoid waiting on deadlock...";
    raise(SIGSEGV);
  }

  DumpTableLocations(catalog_manager(), client::kTableName);
}

void TabletSplitITest::WaitForTestTableTabletsCompactionFinish(MonoDelta timeout) {
  for (auto peer : ASSERT_RESULT(ListPostSplitChildrenTabletPeers())) {
    ASSERT_OK(WaitFor([&peer] {
      return peer->tablet()->metadata()->has_been_fully_compacted();
    }, timeout * kTimeMultiplier, "Wait for post tablet split compaction to be completed"));
  }
}

Result<std::vector<tablet::TabletPeerPtr>> TabletSplitITest::ListSplitCompleteTabletPeers() {
  auto test_table_id = VERIFY_RESULT(client_->GetYBTableInfo(client::kTableName)).table_id;
  return ListTableInactiveSplitTabletPeers(cluster_.get(), test_table_id);
}

Result<std::vector<tablet::TabletPeerPtr>> TabletSplitITest::ListPostSplitChildrenTabletPeers() {
  auto test_table_id = VERIFY_RESULT(client_->GetYBTableInfo(client::kTableName)).table_id;
  return ListTableActiveTabletPeers(cluster_.get(), test_table_id);
}

bool HasPendingCompaction(rocksdb::DB* db) {
  uint64_t compaction_pending = 0;
  db->GetIntProperty("rocksdb.compaction-pending", &compaction_pending);
  return compaction_pending > 0;
}

bool HasRunningCompaction(rocksdb::DB* db) {
  uint64_t running_compactions = 0;
  db->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
  return running_compactions > 0;
}

void TabletSplitITest::VerifyTriggeredPostSplitCompaction(int num_peers) {
  for (auto peer : ASSERT_RESULT(ListPostSplitChildrenTabletPeers())) {
    const auto* tablet = peer->tablet();
    auto* db = tablet->TEST_db();
    auto has_triggered_compaction = HasPendingCompaction(db) ||
        HasRunningCompaction(db) ||
        tablet->metadata()->has_been_fully_compacted();
    EXPECT_TRUE(has_triggered_compaction);
    num_peers--;
  }
  EXPECT_EQ(num_peers, 0);
}

Result<uint64_t> TabletSplitITest::GetActiveTabletsBytesRead() {
  uint64_t read_bytes_1 = 0, read_bytes_2 = 0;
  for (auto peer : VERIFY_RESULT(ListPostSplitChildrenTabletPeers())) {
    auto this_peer_read_bytes = peer->tablet()->regulardb_statistics()->getTickerCount(
        rocksdb::Tickers::COMPACT_READ_BYTES);
    if (read_bytes_1 == 0) {
      read_bytes_1 = this_peer_read_bytes;
    } else if (read_bytes_2 == 0) {
      read_bytes_2 = this_peer_read_bytes;
    } else {
      if (this_peer_read_bytes != read_bytes_1 && this_peer_read_bytes != read_bytes_2) {
        return STATUS_FORMAT(IllegalState,
            "Expected this peer's read bytes ($0) to equal one of the existing peer's read bytes "
            "($1 or $2)",
            this_peer_read_bytes, read_bytes_1, read_bytes_2);
      }
    }
  }
  if (read_bytes_1 <= 0 || read_bytes_2 <= 0) {
    return STATUS_FORMAT(IllegalState,
        "Peer's read bytes should be greater than zero. Found $0 and $1",
        read_bytes_1, read_bytes_2);
  }
  return read_bytes_1 + read_bytes_2;
}

Result<uint64_t> TabletSplitITest::GetInactiveTabletsBytesWritten() {
  uint64_t write_bytes = 0;
  for (auto peer : VERIFY_RESULT(ListSplitCompleteTabletPeers())) {
    auto this_peer_written_bytes = peer->tablet()->regulardb_statistics()->getTickerCount(
        rocksdb::Tickers::COMPACT_WRITE_BYTES);
    if (write_bytes == 0) write_bytes = this_peer_written_bytes;
    if (write_bytes != this_peer_written_bytes) {
      return STATUS_FORMAT(IllegalState,
          "Expected the number of written bytes at each peer to be the same. Found one with $0 and "
          "another with $1",
          write_bytes, this_peer_written_bytes);
    }
  }
  return write_bytes;
}

Status TabletSplitITest::CheckPostSplitTabletReplicasData(
    size_t num_rows, size_t num_replicas_online) {
  LOG(INFO) << "Checking post-split tablet replicas data...";

  if (num_replicas_online == 0) {
      num_replicas_online = FLAGS_replication_factor;
    }

  std::vector<size_t> keys(num_rows, num_replicas_online);
  const auto key_column_id = table_.ColumnId(kKeyColumn);
  const auto value_column_id = table_.ColumnId(kValueColumn);
  for (auto peer : VERIFY_RESULT(ListPostSplitChildrenTabletPeers())) {
    const auto* shared_tablet = peer->tablet();
    const SchemaPtr schema = shared_tablet->metadata()->schema();
    auto client_schema = schema->CopyWithoutColumnIds();
    auto iter = VERIFY_RESULT(shared_tablet->NewRowIterator(client_schema, boost::none));
    QLTableRow row;
    std::unordered_set<size_t> tablet_keys;
    while (VERIFY_RESULT(iter->HasNext())) {
      RETURN_NOT_OK(iter->NextRow(&row));
      auto key_opt = row.GetValue(key_column_id);
      SCHECK(key_opt.is_initialized(), InternalError, "Key is not initialized");
      SCHECK_EQ(key_opt, row.GetValue(value_column_id), InternalError, "Wrong value for key");
      auto key = key_opt->int32_value();
      SCHECK(
          tablet_keys.insert(key).second,
          InternalError,
          Format("Duplicate key $0 in tablet $1", key, shared_tablet->tablet_id()));
      SCHECK_GT(
          keys[key - 1]--,
          0,
          InternalError,
          Format("Extra key $0 in tablet $1", key, shared_tablet->tablet_id()));
    }
  }
  for (auto key = 1; key <= num_rows; ++key) {
    SCHECK_EQ(keys[key - 1], 0, InternalError, Format("Missing key: $0", key));
  }
  return Status::OK();
}

Status TabletSplitITest::CheckSourceTabletAfterSplit(const TabletId& source_tablet_id) {
  LOG(INFO) << "Checking source tablet behavior after split...";
  google::FlagSaver saver;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_do_not_start_election_test_only) = true;

  size_t tablet_split_insert_error_count = 0;
  size_t not_the_leader_insert_error_count = 0;
  size_t ts_online_count = 0;
  for (auto mini_ts : cluster_->mini_tablet_servers()) {
    if (!mini_ts->is_started()) {
      continue;
    }
    ++ts_online_count;
    auto ts_service_proxy = std::make_unique<tserver::TabletServerServiceProxy>(
        proxy_cache_.get(), HostPort::FromBoundEndpoint(mini_ts->bound_rpc_addr()));

    {
      tserver::ReadRequestPB req = VERIFY_RESULT(CreateReadRequest(source_tablet_id, 1 /* key */));

      rpc::RpcController controller;
      controller.set_timeout(kRpcTimeout);
      tserver::ReadResponsePB resp;
      ts_service_proxy->Read(req, &resp, &controller);

      SCHECK(resp.has_error(), InternalError, "Expected error on read from split tablet");
      SCHECK_EQ(
          resp.error().code(),
          tserver::TabletServerErrorPB::TABLET_SPLIT,
          InternalError,
          "Expected error on read from split tablet to be "
          "tserver::TabletServerErrorPB::TABLET_SPLIT");
    }

    {
      tserver::WriteRequestPB req =
          CreateInsertRequest(source_tablet_id, 0 /* key */, 0 /* value */);

      rpc::RpcController controller;
      controller.set_timeout(kRpcTimeout);
      tserver::WriteResponsePB resp;
      ts_service_proxy->Write(req, &resp, &controller);

      SCHECK(resp.has_error(), InternalError, "Expected error on write to split tablet");
      LOG(INFO) << "Error: " << AsString(resp.error());
      switch (resp.error().code()) {
        case tserver::TabletServerErrorPB::TABLET_SPLIT:
          SCHECK_EQ(
              resp.error().status().code(),
              AppStatusPB::ILLEGAL_STATE,
              InternalError,
              "tserver::TabletServerErrorPB::TABLET_SPLIT error should have "
              "AppStatusPB::ILLEGAL_STATE on write to split tablet");
          tablet_split_insert_error_count++;
          break;
        case tserver::TabletServerErrorPB::NOT_THE_LEADER:
          not_the_leader_insert_error_count++;
          break;
        default:
          return STATUS_FORMAT(InternalError, "Unexpected error: $0", resp.error());
      }
    }
  }
  SCHECK_EQ(
      tablet_split_insert_error_count, 1, InternalError,
      "Leader should return \"try again\" error on insert.");
  SCHECK_EQ(
      not_the_leader_insert_error_count, ts_online_count - 1, InternalError,
      "Followers should return \"not the leader\" error.");
  return Status::OK();
}

Result<scoped_refptr<master::TabletInfo>> TabletSplitITest::GetSingleTestTabletInfo(
    master::CatalogManager* catalog_mgr) {
  std::vector<scoped_refptr<master::TabletInfo>> tablet_infos;
  catalog_mgr->GetTableInfo(table_->id())->GetAllTablets(&tablet_infos);

  SCHECK_EQ(tablet_infos.size(), 1, IllegalState, "Expect test table to have only 1 tablet");
  return tablet_infos.front();
}

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
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_skip_deleting_split_tablets) = true;

  // TODO(tsplit): add delay of applying part of intents after tablet is split.
  // TODO(tsplit): test split during long-running transactions.

  constexpr auto kNumRows = 500;

  const auto source_tablet_id = ASSERT_RESULT(CreateSingleTabletAndSplit(kNumRows));

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_skip_deleting_split_tablets) = false;

  ASSERT_NO_FATALS(WaitForTabletSplitCompletion(/* expected_non_split_tablets =*/ 2));

  ASSERT_OK(CheckRowsCount(kNumRows));

  ASSERT_OK(WriteRows(kNumRows, kNumRows + 1));

  ASSERT_OK(cluster_->RestartSync());

  ASSERT_OK(CheckPostSplitTabletReplicasData(kNumRows * 2));
}

TEST_F(TabletSplitITest, SplitTabletIsAsync) {
  constexpr auto kNumRows = 500;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_before_post_split_compation) = true;

  ASSERT_OK(CreateSingleTabletAndSplit(kNumRows));

  for (auto peer : ASSERT_RESULT(ListPostSplitChildrenTabletPeers())) {
    EXPECT_FALSE(peer->tablet()->metadata()->has_been_fully_compacted());
  }
  std::this_thread::sleep_for(1s * kTimeMultiplier);
  for (auto peer : ASSERT_RESULT(ListPostSplitChildrenTabletPeers())) {
    EXPECT_FALSE(peer->tablet()->metadata()->has_been_fully_compacted());
  }
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_before_post_split_compation) = false;
  ASSERT_NO_FATALS(WaitForTestTableTabletsCompactionFinish(5s * kTimeMultiplier));
}

TEST_F(TabletSplitITest, ParentTabletCleanup) {
  constexpr auto kNumRows = 500;

  ASSERT_OK(CreateSingleTabletAndSplit(kNumRows));

  // This will make client first try to access deleted tablet and that should be handled correctly.
  ASSERT_OK(CheckRowsCount(kNumRows));
}

TEST_F(TabletSplitITest, TestInitiatesCompactionAfterSplit) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_skip_deleting_split_tablets) = true;
  constexpr auto kNumRows = 10000;

  CreateSingleTablet();

  const auto split_hash_code = ASSERT_RESULT(WriteRowsAndGetMiddleHashCode(kNumRows));

  ASSERT_OK(SplitTabletAndValidate(split_hash_code, kNumRows));

  auto pre_split_bytes_written = ASSERT_RESULT(GetInactiveTabletsBytesWritten());
  auto post_split_bytes_read = ASSERT_RESULT((GetActiveTabletsBytesRead()));
  ASSERT_GE(pre_split_bytes_written, post_split_bytes_read);
}

TEST_F(TabletSplitITest, TestHeartbeatAfterSplit) {
  constexpr auto kNumRows = 10000;

  // Keep tablets without compaction after split.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_before_post_split_compation) = true;

  CreateSingleTablet();

  const auto split_hash_code = ASSERT_RESULT(WriteRowsAndGetMiddleHashCode(kNumRows));

  ASSERT_OK(SplitTabletAndValidate(split_hash_code, kNumRows));

  auto test_table_id = ASSERT_RESULT(client_->GetYBTableInfo(client::kTableName)).table_id;
  // Verify that heartbeat contains flag processing_parent_data for all tablets of the test
  // table on each tserver to have after split
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto server = cluster_->mini_tablet_server(i)->server();
    if (!server) { // Server is shut down.
      continue;
    }
    auto* ts_manager = server->tablet_manager();
    std::set<TabletId> tablets;
    for (auto& peer : ts_manager->GetTabletPeers()) {
      if (peer->tablet_metadata()->table_id() == test_table_id) {
        tablets.insert(peer->tablet()->tablet_id());
      }
    }

    master::TabletReportPB report;
    ts_manager->GenerateTabletReport(&report);
    for (const auto& reported_tablet : report.updated_tablets()) {
      if (tablets.find(reported_tablet.tablet_id()) == tablets.end()) {
        continue;
      }
      EXPECT_TRUE(reported_tablet.processing_parent_data());
    }
  }

  // Wait for the flag processing_parent_data to be propagated to master through heartbeat
  SleepFor(MonoDelta::FromMilliseconds(2 * FLAGS_heartbeat_interval_ms));

  // Add new tserver in to force load balancer moves.
  auto newts = cluster_->num_tablet_servers();
  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(newts + 1));
  const auto newts_uuid = cluster_->mini_tablet_server(newts)->server()->permanent_uuid();

  // Verify none test table replica on the new tserver
  scoped_refptr<master::TableInfo> tbl_info = catalog_manager()->GetTableInfo(test_table_id);
  vector<scoped_refptr<master::TabletInfo>> tablets;
  tbl_info->GetAllTablets(&tablets);
  bool foundReplica = false;
  for (const auto& tablet : tablets) {
    auto replica_map = tablet->GetReplicaLocations();
    if (replica_map->find(newts_uuid) != replica_map->end()) {
      foundReplica = true;
      break;
    }
  }
  EXPECT_FALSE(foundReplica);

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_pause_before_post_split_compation) = false;

  ASSERT_NO_FATALS(WaitForTestTableTabletsCompactionFinish(5s * kTimeMultiplier));

  // Wait for test table replica on the new tserver
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
                        tbl_info = catalog_manager()->GetTableInfo(test_table_id);
                        tbl_info->GetAllTablets(&tablets);
                        foundReplica = false;
                        for (const auto& tablet : tablets) {
                          auto replica_map = tablet->GetReplicaLocations();
                          if (replica_map->find(newts_uuid) != replica_map->end()) {
                            foundReplica = true;
                            break;
                          }
                      }
                      return foundReplica;
                    }, MonoDelta::FromMilliseconds(30000 * 2), "WaitForLBToBeProcessed"));

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

  ASSERT_OK(CreateSingleTabletAndSplit(kNumRows));
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

CHECKED_STATUS SplitTablet(master::CatalogManager* catalog_mgr, const tablet::Tablet& tablet) {
  const auto& tablet_id = tablet.tablet_id();
  LOG(INFO) << "Tablet: " << tablet_id;
  LOG(INFO) << "Number of SST files: " << tablet.TEST_db()->GetCurrentVersionNumSSTFiles();
  std::string properties;
  tablet.TEST_db()->GetProperty(rocksdb::DB::Properties::kAggregatedTableProperties, &properties);
  LOG(INFO) << "DB properties: " << properties;

  const auto encoded_split_key = VERIFY_RESULT(tablet.GetEncodedMiddleSplitKey());
  const auto doc_key_hash = VERIFY_RESULT(docdb::DecodeDocKeyHash(encoded_split_key)).value();
  LOG(INFO) << "Middle hash key: " << doc_key_hash;
  const auto partition_split_key = PartitionSchema::EncodeMultiColumnHashValue(doc_key_hash);

  return catalog_mgr->SplitTablet(tablet_id, encoded_split_key, partition_split_key);
}

} // namespace

TEST_F(TabletSplitITest, SplitTabletDuringReadWriteLoad) {
  constexpr auto kNumTablets = 3;

  FLAGS_db_write_buffer_size = 100_KB;

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

  const auto test_table_id = ASSERT_RESULT(client_->GetYBTableInfo(client::kTableName)).table_id;

  std::vector<tablet::TabletPeerPtr> peers;
  ASSERT_OK(LoggedWaitFor([&] {
    peers = ListTableTabletLeadersPeers(cluster_.get(), test_table_id);
    return peers.size() == kNumTablets;
  }, 60s, "Waiting for leaders ..."));

  LOG(INFO) << "Starting workload ...";
  workload.Start();

  for (const auto& peer : peers) {
    ASSERT_OK(LoggedWaitFor(
        [&peer] {
          const auto data_size =
              peer->tablet()->TEST_db()->GetCurrentVersionSstFilesUncompressedSize();
          YB_LOG_EVERY_N_SECS(INFO, 5) << "Data written: " << data_size;
          return data_size > (FLAGS_rocksdb_level0_file_num_compaction_trigger + 1) *
                                 FLAGS_db_write_buffer_size;
        },
        60s * kTimeMultiplier, Format("Writing data to split (tablet $0) ...", peer->tablet_id())));
  }

  DumpWorkloadStats(workload);

  auto* catalog_mgr = catalog_manager();

  for (const auto& peer : peers) {
    const auto& source_tablet = *ASSERT_NOTNULL(peer->tablet());
    ASSERT_OK(SplitTablet(catalog_mgr, source_tablet));
  }

  ASSERT_NO_FATALS(WaitForTabletSplitCompletion(/* expected_non_split_tablets =*/ kNumTablets * 2));

  DumpTableLocations(catalog_mgr, client::kTableName);

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

void TabletSplitITest::SplitClientRequestsIds(int split_depth) {
  // Set data block size low enough, so we have enough data blocks for middle key
  // detection to work correctly.
  FLAGS_db_block_size_bytes = 1_KB;
  const auto kNumRows = 50 * (1 << split_depth);

  SetNumTablets(1);
  CreateTable();

  ASSERT_OK(WriteRows(kNumRows, 1));

  ASSERT_OK(CheckRowsCount(kNumRows));

  auto* catalog_mgr = catalog_manager();

  for (int i = 0; i < split_depth; ++i) {
    auto peers = ListTableTabletLeadersPeers(cluster_.get(), table_->id());
    ASSERT_EQ(peers.size(), 1 << i);
    for (const auto& peer : peers) {
      const auto tablet = peer->shared_tablet();
      ASSERT_OK(tablet->Flush(tablet::FlushMode::kSync));
      tablet->ForceRocksDBCompactInTest();
      ASSERT_OK(SplitTablet(catalog_mgr, *tablet));
    }

    ASSERT_NO_FATALS(WaitForTabletSplitCompletion(
        /* expected_non_split_tablets =*/ 1 << (i + 1)));
  }

  Status s;
  ASSERT_OK(WaitFor([&] {
    s = ResultToStatus(WriteRows(1, 1));
    return !s.IsTryAgain();
  }, 60s, "Waiting for successful write"));
  ASSERT_OK(s);
}

// Test for https://github.com/yugabyte/yugabyte-db/issues/5415.
// Client knows about split parent for final tablets.
TEST_F(TabletSplitITest, SplitClientRequestsIdsDepth1) {
  SplitClientRequestsIds(1);
}

// Test for https://github.com/yugabyte/yugabyte-db/issues/5415.
// Client doesn't know about split parent for final tablets.
TEST_F(TabletSplitITest, SplitClientRequestsIdsDepth2) {
  SplitClientRequestsIds(2);
}

TEST_F(TabletSplitITest, SplitSingleTabletWithLimit) {
  FLAGS_db_block_size_bytes = 1_KB;

  const auto kSplitDepth = 3;
  const auto kNumRows = 50 * (1 << kSplitDepth);
  FLAGS_tablet_split_limit_per_table = (1 << kSplitDepth) - 1;

  CreateSingleTablet();
  ASSERT_OK(WriteRows(kNumRows, 1));
  ASSERT_OK(CheckRowsCount(kNumRows));

  auto* catalog_mgr = catalog_manager();

  master::TableIdentifierPB table_id_pb;
  table_id_pb.set_table_id(table_->id());
  bool reached_split_limit = false;

  for (int i = 0; i < kSplitDepth; ++i) {
    auto peers = ListTableTabletLeadersPeers(cluster_.get(), table_->id());
    bool expect_split = false;
    for (const auto& peer : peers) {
      const auto tablet = peer->shared_tablet();
      ASSERT_OK(tablet->Flush(tablet::FlushMode::kSync));
      tablet->ForceRocksDBCompactInTest();
      scoped_refptr<master::TableInfo> table_info;
      ASSERT_OK(catalog_mgr->FindTable(table_id_pb, &table_info));

      expect_split = table_info->NumTablets() < FLAGS_tablet_split_limit_per_table;

      if (expect_split) {
        ASSERT_OK(SplitTablet(catalog_mgr, *tablet));
      } else {
        const auto split_status = SplitTablet(catalog_mgr, *tablet);
        ASSERT_EQ(master::MasterError(split_status),
                  master::MasterErrorPB::REACHED_SPLIT_LIMIT);
        reached_split_limit = true;
      }
    }
    if (expect_split) {
      ASSERT_NO_FATALS(WaitForTabletSplitCompletion(
          /* expected_non_split_tablets =*/1 << (i + 1)));
    }
  }

  ASSERT_TRUE(reached_split_limit);

  Status s;
  ASSERT_OK(WaitFor([&] {
    s = ResultToStatus(WriteRows(1, 1));
    return !s.IsTryAgain();
  }, 60s, "Waiting for successful write"));

  scoped_refptr<master::TableInfo> table_info;
  ASSERT_OK(catalog_mgr->FindTable(table_id_pb, &table_info));
  ASSERT_EQ(table_info->NumTablets(), FLAGS_tablet_split_limit_per_table);
}

TEST_F(TabletSplitITest, SplitDuringReplicaOffline) {
  constexpr auto kNumRows = 500;

  SetNumTablets(1);
  CreateTable();

  const auto split_hash_code = ASSERT_RESULT(WriteRowsAndGetMiddleHashCode(kNumRows));

  auto rows_count = ASSERT_RESULT(SelectRowsCount(NewSession(), table_));
  ASSERT_EQ(rows_count, kNumRows);

  auto* catalog_mgr = catalog_manager();

  auto source_tablet_info = ASSERT_RESULT(GetSingleTestTabletInfo(catalog_mgr));
  const auto source_tablet_id = source_tablet_info->id();

  cluster_->mini_tablet_server(0)->Shutdown();

  LOG(INFO) << "Stopped TS-1";

  ASSERT_OK(catalog_mgr->TEST_SplitTablet(source_tablet_info, split_hash_code));

  ASSERT_NO_FATALS(WaitForTabletSplitCompletion(
      /* expected_non_split_tablets =*/ 2, /* expected_split_tablets =*/ 1,
      /* num_replicas_online =*/ 2));

  ASSERT_OK(CheckPostSplitTabletReplicasData(kNumRows, 2));

  ASSERT_OK(CheckSourceTabletAfterSplit(source_tablet_id));

  DumpTableLocations(catalog_mgr, client::kTableName);

  rows_count = ASSERT_RESULT(SelectRowsCount(NewSession(), table_));
  ASSERT_EQ(rows_count, kNumRows);

  ASSERT_OK(WriteRows(kNumRows, kNumRows + 1));

  LOG(INFO) << "Starting TS-1";

  ASSERT_OK(cluster_->mini_tablet_server(0)->Start());

  // This time we expect all replicas to be online.
  ASSERT_NO_FATALS(WaitForTabletSplitCompletion(
      /* expected_non_split_tablets =*/ 2, /* expected_split_tablets =*/ 0));

  Status s;
  ASSERT_OK_PREPEND(LoggedWaitFor([&] {
      s = CheckPostSplitTabletReplicasData(kNumRows * 2);
      return s.IsOk();
    }, 30s * kTimeMultiplier, "Waiting for TS-1 to catch up ..."), AsString(s));
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
