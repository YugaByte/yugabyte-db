//
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
//

#include <shared_mutex>
#include <thread>

#include <boost/optional/optional.hpp>
#include <boost/optional/optional_io.hpp>

#include "yb/client/client-test-util.h"
#include "yb/client/ql-dml-test-base.h"
#include "yb/client/session.h"
#include "yb/client/table_handle.h"

#include "yb/common/ql_value.h"

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/docdb/consensus_frontier.h"

#include "yb/integration-tests/test_workload.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_retention_policy.h"

#include "yb/server/skewed_clock.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/shared_lock.h"
#include "yb/util/stopwatch.h"

#include "yb/yql/cql/ql/util/statement_result.h"

using namespace std::literals; // NOLINT

DECLARE_uint64(initial_seqno);
DECLARE_int32(leader_lease_duration_ms);
DECLARE_int64(db_write_buffer_size);
DECLARE_string(time_source);
DECLARE_int32(TEST_delay_execute_async_ms);
DECLARE_int32(retryable_request_timeout_secs);
DECLARE_bool(enable_lease_revocation);
DECLARE_bool(rocksdb_disable_compactions);
DECLARE_int32(rocksdb_level0_slowdown_writes_trigger);
DECLARE_int32(rocksdb_level0_stop_writes_trigger);
DECLARE_bool(flush_rocksdb_on_shutdown);
DECLARE_int32(memstore_size_mb);
DECLARE_int64(global_memstore_size_mb_max);
DECLARE_bool(TEST_allow_stop_writes);
DECLARE_int32(yb_num_shards_per_tserver);
DECLARE_int32(TEST_tablet_inject_latency_on_apply_write_txn_ms);
DECLARE_bool(TEST_log_cache_skip_eviction);
DECLARE_uint64(sst_files_hard_limit);
DECLARE_uint64(sst_files_soft_limit);
DECLARE_int32(timestamp_history_retention_interval_sec);
DECLARE_int32(raft_heartbeat_interval_ms);
DECLARE_int32(history_cutoff_propagation_interval_ms);

namespace yb {
namespace client {

using ql::RowsResult;

namespace {

const std::string kKeyColumn = "key"s;
const std::string kRangeKey1Column = "range_key1"s;
const std::string kRangeKey2Column = "range_key2"s;
const std::string kValueColumn = "int_val"s;
const YBTableName kTable1Name(YQL_DATABASE_CQL, "my_keyspace", "ql_client_test_table1");
const YBTableName kTable2Name(YQL_DATABASE_CQL, "my_keyspace", "ql_client_test_table2");

int32_t ValueForKey(int32_t key) {
  return key * 2;
}

const int kTotalKeys = 250;
const int kBigSeqNo = 100500;

} // namespace

class QLTabletTest : public QLDmlTestBase {
 protected:
  void SetUp() override {
    server::SkewedClock::Register();
    FLAGS_time_source = server::SkewedClock::kName;
    QLDmlTestBase::SetUp();
  }

  void CreateTables(uint64_t initial_seqno1, uint64_t initial_seqno2) {
    google::FlagSaver saver;
    FLAGS_initial_seqno = initial_seqno1;
    CreateTable(kTable1Name, &table1_);
    FLAGS_initial_seqno = initial_seqno2;
    CreateTable(kTable2Name, &table2_);
  }

  void SetValue(const YBSessionPtr& session, int32_t key, int32_t value, const TableHandle& table) {
    const auto op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, key);
    table.AddInt32ColumnValue(req, kValueColumn, value);
    ASSERT_OK(session->ApplyAndFlush(op));
    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status());
  }

  boost::optional<int32_t> GetValue(
      const YBSessionPtr& session, int32_t key, const TableHandle& table) {
    const auto op = CreateReadOp(key, table);
    EXPECT_OK(session->ApplyAndFlush(op));
    auto rowblock = RowsResult(op.get()).GetRowBlock();
    if (rowblock->row_count() == 0) {
      return boost::none;
    }
    EXPECT_EQ(1, rowblock->row_count());
    const auto& value = rowblock->row(0).column(0);
    EXPECT_TRUE(value.value().has_int32_value()) << "Value: " << value.value().ShortDebugString();
    return value.int32_value();
  }

  std::shared_ptr<YBqlReadOp> CreateReadOp(int32_t key, const TableHandle& table) {
    return client::CreateReadOp(key, table, kValueColumn);
  }

  void CreateTable(const YBTableName& table_name, TableHandle* table, int num_tablets = 0) {
    YBSchemaBuilder builder;
    builder.AddColumn(kKeyColumn)->Type(INT32)->HashPrimaryKey()->NotNull();
    builder.AddColumn(kValueColumn)->Type(INT32);

    if (num_tablets == 0) {
      num_tablets = CalcNumTablets(3);
    }
    ASSERT_OK(table->Create(table_name, num_tablets, client_.get(), &builder));
  }

  YBSessionPtr CreateSession() {
    auto session = client_->NewSession();
    session->SetTimeout(15s);
    return session;
  }

  void FillTable(int begin, int end, const TableHandle& table) {
    {
      auto session = CreateSession();
      for (int i = begin; i != end; ++i) {
        SetValue(session, i, ValueForKey(i), table);
      }
    }
    VerifyTable(begin, end, table);
    ASSERT_OK(WaitSync(begin, end, table));
  }

  void VerifyTable(int begin, int end, const TableHandle& table) {
    auto session = CreateSession();
    for (int i = begin; i != end; ++i) {
      auto value = GetValue(session, i, table);
      ASSERT_TRUE(value.is_initialized()) << "i: " << i << ", table: " << table->name().ToString();
      ASSERT_EQ(ValueForKey(i), *value) << "i: " << i << ", table: " << table->name().ToString();
    }
  }

  CHECKED_STATUS WaitSync(int begin, int end, const TableHandle& table) {
    auto deadline = MonoTime::Now() + MonoDelta::FromSeconds(30);

    master::GetTableLocationsRequestPB req;
    master::GetTableLocationsResponsePB resp;
    req.set_max_returned_locations(std::numeric_limits<uint32_t>::max());
    table->name().SetIntoTableIdentifierPB(req.mutable_table());
    RETURN_NOT_OK(
        cluster_->mini_master()->master()->catalog_manager()->GetTableLocations(&req, &resp));
    std::vector<master::TabletLocationsPB> tablets;
    std::unordered_set<std::string> replicas;
    for (const auto& tablet : resp.tablet_locations()) {
      tablets.emplace_back(tablet);
      for (const auto& replica : tablet.replicas()) {
        replicas.insert(replica.ts_info().permanent_uuid());
      }
    }
    for (const auto& replica : replicas) {
      RETURN_NOT_OK(DoWaitSync(deadline, tablets, replica, begin, end, table));
    }
    return Status::OK();
  }

  CHECKED_STATUS DoWaitSync(const MonoTime& deadline,
                            const std::vector<master::TabletLocationsPB>& tablets,
                            const std::string& replica,
                            int begin,
                            int end,
                            const TableHandle& table) {
    auto tserver = cluster_->find_tablet_server(replica);
    if (!tserver) {
      return STATUS_FORMAT(NotFound, "Tablet server for $0 not found", replica);
    }
    auto endpoint = tserver->server()->rpc_server()->GetBoundAddresses().front();
    auto proxy = std::make_unique<tserver::TabletServerServiceProxy>(
        &tserver->server()->proxy_cache(), HostPort::FromBoundEndpoint(endpoint));

    auto condition = [&]() -> Result<bool> {
      for (int i = begin; i != end; ++i) {
        bool found = false;
        for (const auto& tablet : tablets) {
          tserver::ReadRequestPB req;
          {
            std::string partition_key;
            auto op = CreateReadOp(i, table);
            RETURN_NOT_OK(op->GetPartitionKey(&partition_key));
            auto* ql_batch = req.add_ql_batch();
            *ql_batch = op->request();
            const auto& hash_code = PartitionSchema::DecodeMultiColumnHashValue(partition_key);
            ql_batch->set_hash_code(hash_code);
            ql_batch->set_max_hash_code(hash_code);
          }

          tserver::ReadResponsePB resp;
          rpc::RpcController controller;
          controller.set_timeout(MonoDelta::FromSeconds(1));
          req.set_tablet_id(tablet.tablet_id());
          req.set_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
          proxy->Read(req, &resp, &controller);

          const auto& ql_batch = resp.ql_batch(0);
          if (ql_batch.status() != QLResponsePB_QLStatus_YQL_STATUS_OK) {
            return STATUS_FORMAT(RemoteError,
                                 "Bad resp status: $0",
                                 QLResponsePB_QLStatus_Name(ql_batch.status()));
          }
          std::shared_ptr<std::vector<ColumnSchema>>
            columns = std::make_shared<std::vector<ColumnSchema>>(table->schema().columns());
          Slice data = VERIFY_RESULT(controller.GetSidecar(ql_batch.rows_data_sidecar()));
          yb::ql::RowsResult result(table->name(), columns, data.ToBuffer());
          auto row_block = result.GetRowBlock();
          if (row_block->row_count() == 1) {
            if (found) {
              return STATUS_FORMAT(Corruption, "Key found twice: $0", i);
            }
            auto value = row_block->row(0).column(0).int32_value();
            if (value != ValueForKey(i)) {
              return STATUS_FORMAT(Corruption,
                                   "Wrong value for key: $0, expected: $1",
                                   value,
                                   ValueForKey(i));
            }
            found = true;
          }
        }
        if (!found) {
          return STATUS_FORMAT(NotFound, "Key not found: $0", i);
        }
      }
      return true;
    };

    return Wait(condition, deadline, "Waiting for replication");
  }

  CHECKED_STATUS Import() {
    std::this_thread::sleep_for(1s); // Wait until all tablets a synced and flushed.
    EXPECT_OK(cluster_->FlushTablets());

    auto source_infos = GetTabletInfos(kTable1Name);
    auto dest_infos = GetTabletInfos(kTable2Name);
    EXPECT_EQ(source_infos.size(), dest_infos.size());
    for (size_t i = 0; i != source_infos.size(); ++i) {
      std::string start1, end1, start2, end2;
      {
        auto& metadata = source_infos[i]->metadata();
        SharedLock<std::remove_reference<decltype(metadata)>::type> lock(metadata);
        const auto& partition = metadata.state().pb.partition();
        start1 = partition.partition_key_start();
        end1 = partition.partition_key_end();
      }
      {
        auto& metadata = dest_infos[i]->metadata();
        SharedLock<std::remove_reference<decltype(metadata)>::type> lock(metadata);
        const auto& partition = metadata.state().pb.partition();
        start2 = partition.partition_key_start();
        end2 = partition.partition_key_end();
      }
      EXPECT_EQ(start1, start2);
      EXPECT_EQ(end1, end2);
    }
    for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
      auto* tablet_manager = cluster_->mini_tablet_server(i)->server()->tablet_manager();
      for (size_t j = 0; j != source_infos.size(); ++j) {
        tablet::TabletPeerPtr source_peer, dest_peer;
        tablet_manager->LookupTablet(source_infos[j]->id(), &source_peer);
        EXPECT_NE(nullptr, source_peer);
        auto source_dir = source_peer->tablet()->metadata()->rocksdb_dir();
        tablet_manager->LookupTablet(dest_infos[j]->id(), &dest_peer);
        EXPECT_NE(nullptr, dest_peer);
        auto status = dest_peer->tablet()->ImportData(source_dir);
        if (!status.ok() && !status.IsNotFound()) {
          return status;
        }
      }
    }
    return Status::OK();
  }

  scoped_refptr<master::TableInfo> GetTableInfo(const YBTableName& table_name) {
    auto* catalog_manager = cluster_->leader_mini_master()->master()->catalog_manager();
    std::vector<scoped_refptr<master::TableInfo>> all_tables;
    catalog_manager->GetAllTables(&all_tables);
    scoped_refptr<master::TableInfo> table_info;
    for (auto& table : all_tables) {
      if (table->name() == table_name.table_name()) {
        return table;
      }
    }
    return nullptr;
  }

  std::vector<scoped_refptr<master::TabletInfo>> GetTabletInfos(const YBTableName& table_name) {
    auto table_info = GetTableInfo(table_name);
    EXPECT_NE(nullptr, table_info);
    std::vector<scoped_refptr<master::TabletInfo>> tablets;
    table_info->GetAllTablets(&tablets);
    return tablets;
  }

  Status WaitForTableCreation(const YBTableName& table_name,
                              master::IsCreateTableDoneResponsePB *resp) {
    return LoggedWaitFor([=]() -> Result<bool> {
      master::IsCreateTableDoneRequestPB req;
      req.mutable_table()->set_table_name(table_name.table_name());
      req.mutable_table()->mutable_namespace_()->set_name(table_name.namespace_name());
      resp->Clear();

      auto master_proxy = std::make_shared<master::MasterServiceProxy>(
          &client_->proxy_cache(),
          cluster_->leader_mini_master()->bound_rpc_addr());
      rpc::RpcController rpc;
      rpc.set_timeout(MonoDelta::FromSeconds(30));

      Status s = master_proxy->IsCreateTableDone(req, resp, &rpc);
      return s.ok() && !resp->has_error();
    }, MonoDelta::FromSeconds(30), "Table Creation");
  }

  void TestDeletePartialKey(int num_range_keys_in_delete);

  TableHandle table1_;
  TableHandle table2_;
};

TEST_F(QLTabletTest, ImportToEmpty) {
  CreateTables(0, kBigSeqNo);

  FillTable(0, kTotalKeys, table1_);
  ASSERT_OK(Import());
  VerifyTable(0, kTotalKeys, table1_);
  VerifyTable(0, kTotalKeys, table2_);
}

TEST_F(QLTabletTest, ImportToNonEmpty) {
  CreateTables(0, kBigSeqNo);

  FillTable(0, kTotalKeys, table1_);
  FillTable(kTotalKeys, 2 * kTotalKeys, table2_);
  ASSERT_OK(Import());
  VerifyTable(0, 2 * kTotalKeys, table2_);
}

TEST_F(QLTabletTest, ImportToEmptyAndRestart) {
  CreateTables(0, kBigSeqNo);

  FillTable(0, kTotalKeys, table1_);
  ASSERT_OK(Import());
  VerifyTable(0, kTotalKeys, table2_);

  ASSERT_OK(cluster_->RestartSync());
  VerifyTable(0, kTotalKeys, table1_);
  VerifyTable(0, kTotalKeys, table2_);
}

TEST_F(QLTabletTest, ImportToNonEmptyAndRestart) {
  CreateTables(0, kBigSeqNo);

  FillTable(0, kTotalKeys, table1_);
  FillTable(kTotalKeys, 2 * kTotalKeys, table2_);

  ASSERT_OK(Import());
  VerifyTable(0, 2 * kTotalKeys, table2_);

  ASSERT_OK(cluster_->RestartSync());
  VerifyTable(0, kTotalKeys, table1_);
  VerifyTable(0, 2 * kTotalKeys, table2_);
}

TEST_F(QLTabletTest, LateImport) {
  CreateTables(kBigSeqNo, 0);

  FillTable(0, kTotalKeys, table1_);
  ASSERT_NOK(Import());
}

TEST_F(QLTabletTest, OverlappedImport) {
  CreateTables(kBigSeqNo - 2, kBigSeqNo);

  FillTable(0, kTotalKeys, table1_);
  FillTable(kTotalKeys, 2 * kTotalKeys, table2_);
  ASSERT_NOK(Import());
}

// Test expected number of tablets for transactions table - added for #2293.
TEST_F(QLTabletTest, TransactionsTableTablets) {
  YBSchemaBuilder builder;
  builder.AddColumn(kKeyColumn)->Type(INT32)->HashPrimaryKey()->NotNull();
  builder.AddColumn(kValueColumn)->Type(INT32);

  // Create transactional table.
  TableProperties table_properties;
  table_properties.SetTransactional(true);
  builder.SetTableProperties(table_properties);

  TableHandle table;
  ASSERT_OK(table.Create(kTable1Name, 8, client_.get(), &builder));

  // Wait for transactions table to be created.
  YBTableName table_name(YQL_DATABASE_CQL, master::kSystemNamespaceName, kTransactionsTableName);
  master::IsCreateTableDoneResponsePB resp;
  ASSERT_OK(WaitForTableCreation(table_name, &resp));
  ASSERT_TRUE(resp.done());

  auto tablets = GetTabletInfos(table_name);
  ASSERT_EQ(tablets.size(), cluster_->num_tablet_servers() * FLAGS_yb_num_shards_per_tserver);
}

void DoStepDowns(MiniCluster* cluster) {
  for (int j = 0; j != 5; ++j) {
    StepDownAllTablets(cluster);
    std::this_thread::sleep_for(5s);
  }
}

void VerifyLogIndicies(MiniCluster* cluster) {
  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    std::vector<tablet::TabletPeerPtr> peers;
    cluster->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers(&peers);

    for (const auto& peer : peers) {
      int64_t index = ASSERT_RESULT(peer->GetEarliestNeededLogIndex());
      ASSERT_EQ(peer->consensus()->GetLastCommittedOpId().index, index);
    }
  }
}

namespace {

constexpr auto kRetryableRequestTimeoutSecs = 4;

} // namespace

TEST_F(QLTabletTest, GCLogWithoutWrites) {
  SetAtomicFlag(kRetryableRequestTimeoutSecs, &FLAGS_retryable_request_timeout_secs);

  TableHandle table;
  CreateTable(kTable1Name, &table);

  FillTable(0, kTotalKeys, table);

  std::this_thread::sleep_for(1s * (kRetryableRequestTimeoutSecs + 1));
  ASSERT_OK(cluster_->FlushTablets());
  DoStepDowns(cluster_.get());
  VerifyLogIndicies(cluster_.get());
}

TEST_F(QLTabletTest, GCLogWithRestartWithoutWrites) {
  SetAtomicFlag(kRetryableRequestTimeoutSecs, &FLAGS_retryable_request_timeout_secs);

  TableHandle table;
  CreateTable(kTable1Name, &table);

  FillTable(0, kTotalKeys, table);

  std::this_thread::sleep_for(1s * (kRetryableRequestTimeoutSecs + 1));
  ASSERT_OK(cluster_->FlushTablets());

  ASSERT_OK(cluster_->RestartSync());

  DoStepDowns(cluster_.get());
  VerifyLogIndicies(cluster_.get());
}

TEST_F(QLTabletTest, LeaderLease) {
  SetAtomicFlag(false, &FLAGS_enable_lease_revocation);

  TableHandle table;
  CreateTable(kTable1Name, &table);

  LOG(INFO) << "Filling table";
  FillTable(0, kTotalKeys, table);

  auto old_lease_ms = GetAtomicFlag(&FLAGS_leader_lease_duration_ms);
  SetAtomicFlag(60 * 1000, &FLAGS_leader_lease_duration_ms);
  // Wait for lease to sync.
  std::this_thread::sleep_for(2ms * old_lease_ms);

  LOG(INFO) << "Step down";
  StepDownAllTablets(cluster_.get());

  LOG(INFO) << "Write value";
  auto session = CreateSession();
  const auto op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
  auto* const req = op->mutable_request();
  QLAddInt32HashValue(req, 1);
  table.AddInt32ColumnValue(req, kValueColumn, 1);
  auto status = session->ApplyAndFlush(op);
  ASSERT_TRUE(status.IsIOError()) << "Status: " << status;
}

// This test tries to catch situation when some entries were applied and flushed in RocksDB,
// but is not present in persistent logs.
//
// If that happens than we would get situation that after restart some node has records
// in RocksDB, but does not have log records for it. And would not be able to restore last
// hybrid time, also this node would not be able to remotely bootstrap other nodes.
//
// So we just delay one of follower logs and write a random key.
// Checking that flushed op id in RocksDB does not exceed last op id in logs.
TEST_F(QLTabletTest, WaitFlush) {
  google::FlagSaver saver;

  constexpr int kNumTablets = 1; // Use single tablet to increase chance of bad scenario.
  FLAGS_db_write_buffer_size = 10; // Use small memtable to induce background flush on each write.

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTable1Name);
  workload.set_write_timeout_millis(30000);
  workload.set_num_tablets(kNumTablets);
  workload.set_num_write_threads(1);
  workload.set_write_batch_size(1);
  workload.set_payload_bytes(128);
  workload.Setup();
  workload.Start();

  std::vector<tablet::TabletPeerPtr> peers;

  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    std::vector<tablet::TabletPeerPtr> tserver_peers;
    cluster_->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers(&tserver_peers);
    ASSERT_EQ(tserver_peers.size(), 1);
    peers.push_back(tserver_peers.front());
  }

  bool leader_found = false;
  while (!leader_found) {
    for (size_t i = 0; i != peers.size(); ++i) {
      if (peers[i]->LeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY) {
        peers[(i + 1) % peers.size()]->log()->TEST_SetSleepDuration(500ms);
        leader_found = true;
        break;
      }
    }
  }

  auto deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() <= deadline) {
    for (const auto& peer : peers) {
      auto flushed_op_id = ASSERT_RESULT(peer->tablet()->MaxPersistentOpId()).regular;
      auto latest_entry_op_id = peer->log()->GetLatestEntryOpId();
      ASSERT_LE(flushed_op_id.index, latest_entry_op_id.index);
    }
  }

  for (const auto& peer : peers) {
    auto flushed_op_id = ASSERT_RESULT(peer->tablet()->MaxPersistentOpId()).regular;
    ASSERT_GE(flushed_op_id.index, 100);
  }

  workload.StopAndJoin();
}


TEST_F(QLTabletTest, BoundaryValues) {
  constexpr size_t kTotalThreads = 8;
  constexpr size_t kTotalRows = 10000;

  TableHandle table;
  CreateTable(kTable1Name, &table, 1);

  std::vector<std::thread> threads;
  std::atomic<int32_t> idx(0);
  for (size_t t = 0; t != kTotalThreads; ++t) {
    threads.emplace_back([this, &idx, &table] {
      auto session = CreateSession();
      for (;;) {
        int32_t i = idx++;
        if (i >= kTotalRows) {
          break;
        }

        SetValue(session, i, -i, table);
      }
    });
  }
  const auto kSleepTime = NonTsanVsTsan(5s, 1s);
  std::this_thread::sleep_for(kSleepTime);
  LOG(INFO) << "Flushing tablets";
  ASSERT_OK(cluster_->FlushTablets());
  std::this_thread::sleep_for(kSleepTime);
  LOG(INFO) << "GC logs";
  ASSERT_OK(cluster_->CleanTabletLogs());
  LOG(INFO) << "Wait for threads";
  for (auto& thread : threads) {
    thread.join();
  }
  std::this_thread::sleep_for(kSleepTime * 5);
  ASSERT_OK(cluster_->RestartSync());

  size_t total_rows = 0;
  for (const auto& row : TableRange(table)) {
    EXPECT_EQ(row.column(0).int32_value(), -row.column(1).int32_value());
    ++total_rows;
  }
  ASSERT_EQ(kTotalRows, total_rows);

  ASSERT_OK(cluster_->FlushTablets());
  std::this_thread::sleep_for(kSleepTime);

  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    std::vector<tablet::TabletPeerPtr> peers;
    cluster_->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers(&peers);
    ASSERT_EQ(1, peers.size());
    auto& peer = *peers[0];
    auto op_id = peer.log()->GetLatestEntryOpId();
    auto* db = peer.tablet()->TEST_db();
    int64_t max_index = 0;
    int64_t min_index = std::numeric_limits<int64_t>::max();
    for (const auto& file : db->GetLiveFilesMetaData()) {
      LOG(INFO) << "File: " << yb::ToString(file);
      max_index = std::max(
          max_index,
          down_cast<docdb::ConsensusFrontier&>(*file.largest.user_frontier).op_id().index);
      min_index = std::min(
          min_index,
          down_cast<docdb::ConsensusFrontier&>(*file.smallest.user_frontier).op_id().index);
    }

    // Allow several entries for non write ops.
    ASSERT_GE(max_index, op_id.index - 5);
    ASSERT_LE(min_index, 5);
  }
}

// There was bug with MvccManager when clocks were skewed.
// Client tries to read from follower and max safe time is requested w/o any limits,
// so new operations could be added with HT lower than returned.
TEST_F(QLTabletTest, SkewedClocks) {
  google::FlagSaver saver;

  auto delta_changers = SkewClocks(cluster_.get(), 50ms);

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTable1Name);
  workload.set_write_timeout_millis(30000);
  workload.set_num_tablets(12);
  workload.set_num_write_threads(2);
  workload.set_write_batch_size(1);
  workload.set_payload_bytes(128);
  workload.Setup();
  workload.Start();

  while (workload.rows_inserted() < 100) {
    std::this_thread::sleep_for(10ms);
  }

  TableHandle table;
  ASSERT_OK(table.Open(kTable1Name, client_.get()));
  auto session = CreateSession();

  for (int i = 0; i != 1000; ++i) {
    auto op = table.NewReadOp();
    auto req = op->mutable_request();
    QLAddInt32HashValue(req, i);
    auto value_column_id = table.ColumnId(kValueColumn);
    req->add_selected_exprs()->set_column_id(value_column_id);
    req->mutable_column_refs()->add_ids(value_column_id);

    QLRSColDescPB *rscol_desc = req->mutable_rsrow_desc()->add_rscol_descs();
    rscol_desc->set_name(kValueColumn);
    table.ColumnType(kValueColumn)->ToQLTypePB(rscol_desc->mutable_ql_type());
    op->set_yb_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
    ASSERT_OK(session->ApplyAndFlush(op));
    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status());
  }

  workload.StopAndJoin();

  cluster_->Shutdown(); // Need to shutdown cluster before resetting clock back.
  cluster_.reset();
}

TEST_F(QLTabletTest, LeaderChange) {
  const int32_t kKey = 1;
  const int32_t kValue1 = 2;
  const int32_t kValue2 = 3;
  const int32_t kValue3 = 4;
  const int kNumTablets = 1;

  TableHandle table;
  CreateTable(kTable1Name, &table, kNumTablets);
  auto session = client_->NewSession();
  session->SetTimeout(60s);

  // Write kValue1
  SetValue(session, kKey, kValue1, table);

  std::string leader_id;
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto server = cluster_->mini_tablet_server(i)->server();
    auto peers = server->tablet_manager()->GetTabletPeers();
    for (const auto& peer : peers) {
      if (peer->LeaderStatus() != consensus::LeaderStatus::NOT_LEADER) {
        leader_id = server->permanent_uuid();
        break;
      }
    }
  }

  LOG(INFO) << "Current leader: " << leader_id;

  ASSERT_NE(leader_id, "");

  LOG(INFO) << "CAS " << kValue1 << " => " << kValue2;
  const auto write_op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
  auto* const req = write_op->mutable_request();
  QLAddInt32HashValue(req, kKey);
  table.AddInt32ColumnValue(req, kValueColumn, kValue2);

  table.SetColumn(req->add_column_values(), kValueColumn);
  table.SetInt32Condition(
    req->mutable_if_expr()->mutable_condition(), kValueColumn, QL_OP_EQUAL, kValue1);
  req->mutable_column_refs()->add_ids(table.ColumnId(kValueColumn));
  ASSERT_OK(session->Apply(write_op));

  SetAtomicFlag(30000, &FLAGS_TEST_delay_execute_async_ms);
  auto flush_future = session->FlushFuture();
  std::this_thread::sleep_for(2s);

  SetAtomicFlag(0, &FLAGS_TEST_delay_execute_async_ms);

  LOG(INFO) << "Step down old leader";
  StepDownAllTablets(cluster_.get());

  // Write other key to refresh leader cache.
  // Otherwise we would hang of locking the key.
  LOG(INFO) << "Write other key";
  SetValue(session, kKey + 1, kValue1, table);

  LOG(INFO) << "Write " << kValue3;
  SetValue(session, kKey, kValue3, table);

  ASSERT_EQ(GetValue(session, kKey, table), kValue3);

  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto server = cluster_->mini_tablet_server(i)->server();
    auto peers = server->tablet_manager()->GetTabletPeers();
    bool found = false;
    for (const auto& peer : peers) {
      if (peer->LeaderStatus() != consensus::LeaderStatus::NOT_LEADER) {
        LOG(INFO) << "Request step down: " << server->permanent_uuid() << " => " << leader_id;
        consensus::LeaderStepDownRequestPB req;
        req.set_tablet_id(peer->tablet_id());
        req.set_new_leader_uuid(leader_id);
        consensus::LeaderStepDownResponsePB resp;
        ASSERT_OK(peer->consensus()->StepDown(&req, &resp));
        found = true;
        break;
      }
    }
    if (found) {
      break;
    }
  }

  ASSERT_OK(flush_future.get());
  ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, write_op->response().status());

  ASSERT_EQ(GetValue(session, kKey, table), kValue3);
}

void QLTabletTest::TestDeletePartialKey(int num_range_keys_in_delete) {
  YBSchemaBuilder builder;
  builder.AddColumn(kKeyColumn)->Type(INT32)->HashPrimaryKey()->NotNull();
  builder.AddColumn(kRangeKey1Column)->Type(INT32)->PrimaryKey()->NotNull();
  builder.AddColumn(kRangeKey2Column)->Type(INT32)->PrimaryKey()->NotNull();
  builder.AddColumn(kValueColumn)->Type(INT32);

  TableHandle table;
  ASSERT_OK(table.Create(kTable1Name, 1 /* num_tablets */, client_.get(), &builder));

  const auto kValue1 = 2;
  const auto kValue2 = 3;
  const auto kTotalKeys = 200;

  auto session1 = CreateSession();
  auto session2 = CreateSession();
  for (int key = 1; key != kTotalKeys; ++key) {
    {
      const auto op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
      auto* const req = op->mutable_request();
      QLAddInt32HashValue(req, key);
      QLAddInt32RangeValue(req, key);
      QLAddInt32RangeValue(req, key);
      table.AddInt32ColumnValue(req, kValueColumn, kValue1);
      ASSERT_OK(session1->ApplyAndFlush(op));
      ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status());
    }

    const auto op_del = table.NewWriteOp(QLWriteRequestPB::QL_STMT_DELETE);
    {
      auto* const req = op_del->mutable_request();
      QLAddInt32HashValue(req, key);
      for (int i = 0; i != num_range_keys_in_delete; ++i) {
        QLAddInt32RangeValue(req, key);
      }
      ASSERT_OK(session1->Apply(op_del));
    }

    const auto op_update = table.NewWriteOp(QLWriteRequestPB::QL_STMT_UPDATE);
    {
      auto* const req = op_update->mutable_request();
      QLAddInt32HashValue(req, key);
      QLAddInt32RangeValue(req, key);
      QLAddInt32RangeValue(req, key);
      table.AddInt32ColumnValue(req, kValueColumn, kValue2);
      req->mutable_if_expr()->mutable_condition()->set_op(QL_OP_EXISTS);
      ASSERT_OK(session2->Apply(op_update));
    }
    auto future_del = session1->FlushFuture();
    auto future_update = session2->FlushFuture();
    ASSERT_OK(future_del.get());
    ASSERT_OK(future_update.get());
    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op_del->response().status());
    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op_update->response().status());

    auto stored_value = GetValue(session1, key, table);
    ASSERT_TRUE(!stored_value) << "Key: " << key << ", value: " << *stored_value;
  }
}

TEST_F(QLTabletTest, DeleteByHashKey) {
  TestDeletePartialKey(0);
}

TEST_F(QLTabletTest, DeleteByHashAndPartialRangeKey) {
  TestDeletePartialKey(1);
}

TEST_F(QLTabletTest, ManySstFilesBootstrap) {
  FLAGS_flush_rocksdb_on_shutdown = false;

  int key = 0;
  {
    google::FlagSaver flag_saver;

    auto original_rocksdb_level0_stop_writes_trigger = 48;
    FLAGS_sst_files_hard_limit = std::numeric_limits<uint64_t>::max() / 4;
    FLAGS_sst_files_soft_limit = FLAGS_sst_files_hard_limit;
    FLAGS_rocksdb_level0_stop_writes_trigger = 10000;
    FLAGS_rocksdb_level0_slowdown_writes_trigger = 10000;
    FLAGS_rocksdb_disable_compactions = true;
    CreateTable(kTable1Name, &table1_, 1);

    auto session = CreateSession();
    auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kLeaders);
    ASSERT_EQ(peers.size(), 1);
    LOG(INFO) << "Leader: " << peers[0]->permanent_uuid();
    int stop_key = 0;
    for (;;) {
      auto meta = peers[0]->tablet()->TEST_db()->GetLiveFilesMetaData();
      LOG(INFO) << "Total files: " << meta.size();

      ++key;
      SetValue(session, key, ValueForKey(key), table1_);
      if (meta.size() <= original_rocksdb_level0_stop_writes_trigger) {
        ASSERT_OK(peers[0]->tablet()->Flush(tablet::FlushMode::kSync));
        stop_key = key + 10;
      } else if (key >= stop_key) {
        break;
      }
    }
  }

  cluster_->Shutdown();

  LOG(INFO) << "Starting cluster";

  ASSERT_OK(cluster_->StartSync());

  LOG(INFO) << "Verify table";

  VerifyTable(1, key, table1_);
}

class QLTabletTestSmallMemstore : public QLTabletTest {
 public:
  void SetUp() override {
    FLAGS_memstore_size_mb = 1;
    FLAGS_global_memstore_size_mb_max = 1;
    QLTabletTest::SetUp();
  }
};

TEST_F_EX(QLTabletTest, DoubleFlush, QLTabletTestSmallMemstore) {
  SetAtomicFlag(false, &FLAGS_TEST_allow_stop_writes);

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTable1Name);
  workload.set_write_timeout_millis(30000);
  workload.set_num_tablets(1);
  workload.set_num_write_threads(10);
  workload.set_write_batch_size(1);
  workload.set_payload_bytes(1_KB);
  workload.Setup();
  workload.Start();

  while (workload.rows_inserted() < RegularBuildVsSanitizers(75000, 20000)) {
    std::this_thread::sleep_for(10ms);
  }

  workload.StopAndJoin();

  // Flush on rocksdb shutdown could produce second immutable memtable, that will stop writes.
  SetAtomicFlag(true, &FLAGS_TEST_allow_stop_writes);
  cluster_->Shutdown(); // Need to shutdown cluster before resetting clock back.
  cluster_.reset();
}

TEST_F(QLTabletTest, OperationMemTracking) {
  FLAGS_TEST_log_cache_skip_eviction = true;

  constexpr size_t kValueSize = 64_KB;
  const auto kWaitInterval = 50ms;

  YBSchemaBuilder builder;
  builder.AddColumn(kKeyColumn)->Type(INT32)->HashPrimaryKey()->NotNull();
  builder.AddColumn(kValueColumn)->Type(STRING);

  TableHandle table;
  ASSERT_OK(table.Create(kTable1Name, CalcNumTablets(3), client_.get(), &builder));

  FLAGS_TEST_tablet_inject_latency_on_apply_write_txn_ms = 1000;

  const auto op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
  auto* const req = op->mutable_request();
  QLAddInt32HashValue(req, 42);
  auto session = CreateSession();
  table.AddStringColumnValue(req, kValueColumn, std::string(kValueSize, 'X'));
  ASSERT_OK(session->Apply(op));
  auto future = session->FlushFuture();
  auto server_tracker = MemTracker::GetRootTracker()->FindChild("server 1");
  auto tablets_tracker = server_tracker->FindChild("Tablets");
  auto log_tracker = server_tracker->FindChild("log_cache");

  std::chrono::steady_clock::time_point deadline;
  bool tracked_by_tablets = false;
  bool tracked_by_log_cache = false;
  for (;;) {
    // The consumption get order is important, otherwise we could get into situation where
    // mem tracking changed between gets.
    auto log_cache_consumption = log_tracker->consumption();
    tracked_by_log_cache = tracked_by_log_cache || log_cache_consumption >= kValueSize;
    int64_t operation_tracker_consumption = 0;
    for (auto& child : tablets_tracker->ListChildren()) {
      operation_tracker_consumption += child->FindChild("operation_tracker")->consumption();
    }

    tracked_by_tablets = tracked_by_tablets || operation_tracker_consumption >= kValueSize;
    LOG(INFO) << "Operation tracker consumption: " << operation_tracker_consumption
              << ", log cache consumption: " << log_cache_consumption;
    // We have overhead in both log cache and tablets.
    // So if value is double tracked then sum consumption will be higher than double value size.
    ASSERT_LE(operation_tracker_consumption + log_cache_consumption, kValueSize * 2)
        << DumpMemoryUsage();
    if (std::chrono::steady_clock::time_point() == deadline) { // operation did not finish yet
      if (future.wait_for(kWaitInterval) == std::future_status::ready) {
        LOG(INFO) << "Value written";
        deadline = std::chrono::steady_clock::now() + 3s;
        ASSERT_OK(future.get());
        ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status());
      }
    } else if (deadline < std::chrono::steady_clock::now() || tracked_by_log_cache) {
      break;
    } else {
      std::this_thread::sleep_for(kWaitInterval);
    }
  }

  ASSERT_TRUE(tracked_by_tablets);
  ASSERT_TRUE(tracked_by_log_cache);
}

// Checks history cutoff for cluster against previous state.
// Committed history cutoff should not go backward.
// Updates committed_history_cutoff with current state.
void VerifyHistoryCutoff(MiniCluster* cluster, HybridTime* prev_committed,
                         const std::string& trace) {
  SCOPED_TRACE(trace);
  const auto base_delta_us =
      -FLAGS_timestamp_history_retention_interval_sec * MonoTime::kMicrosecondsPerSecond;
  constexpr auto kExtraDeltaMs = 200;
  // Allow one 2 Raft rounds + processing delta to replicate operation, update committed and
  // propagate it.
  const auto committed_delta_us =
      base_delta_us -
      (FLAGS_raft_heartbeat_interval_ms * 2 + kExtraDeltaMs) * MonoTime::kMicrosecondsPerMillisecond
          * kTimeMultiplier;

  HybridTime committed = HybridTime::kMin;
  auto deadline = CoarseMonoClock::now() + 5s * kTimeMultiplier;
  for (;;) {
    ASSERT_LE(CoarseMonoClock::now(), deadline);
    auto peers = ListTabletPeers(cluster, ListPeersFilter::kAll);
    std::sort(peers.begin(), peers.end(), [](const auto& lhs, const auto& rhs) {
      return lhs->permanent_uuid() < rhs->permanent_uuid();
    });
    if (peers.size() != cluster->num_tablet_servers()) {
      std::this_thread::sleep_for(100ms);
      continue;
    }
    bool complete = false;
    for (int i = 0; i < peers.size(); ++i) {
      auto peer = peers[i];
      SCOPED_TRACE(Format("Peer: $0", peer->permanent_uuid()));
      if (peer->state() != tablet::RaftGroupStatePB::RUNNING) {
        complete = false;
        break;
      }
      auto peer_history_cutoff =
          peer->tablet()->RetentionPolicy()->GetRetentionDirective().history_cutoff;
      committed = std::max(peer_history_cutoff, committed);
      auto min_allowed = std::min(peer->clock_ptr()->Now().AddMicroseconds(committed_delta_us),
                                  peer->tablet()->mvcc_manager()->LastReplicatedHybridTime());
      if (peer_history_cutoff < min_allowed) {
        LOG(INFO) << "Committed did not catch up for " << peer->permanent_uuid() << ": "
                  << peer_history_cutoff << " vs " << min_allowed;
        complete = false;
        break;
      }
      if (peer->consensus()->GetLeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY) {
        complete = true;
      }
    }
    if (complete) {
      break;
    }
    std::this_thread::sleep_for(100ms);
  }
  ASSERT_GE(committed, *prev_committed);
  *prev_committed = committed;
}

// Basic check for history cutoff evolution
TEST_F(QLTabletTest, HistoryCutoff) {
  FLAGS_timestamp_history_retention_interval_sec = kTimeMultiplier;
  FLAGS_history_cutoff_propagation_interval_ms = 100;

  CreateTable(kTable1Name, &table1_, /* num_tablets= */ 1);
  HybridTime committed_history_cutoff = HybridTime::kMin;
  FillTable(0, 10, table1_);
  ASSERT_NO_FATALS(VerifyHistoryCutoff(cluster_.get(), &committed_history_cutoff, "After write"));

  // Check that we restore committed state after restart.
  std::array<HybridTime, 3> peer_committed;
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto peers = cluster_->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers();
    ASSERT_EQ(peers.size(), 1);
    peer_committed[i] =
        peers[0]->tablet()->RetentionPolicy()->GetRetentionDirective().history_cutoff;
    LOG(INFO) << "Peer: " << peers[0]->permanent_uuid() << ", index: " << i
              << ", committed: " << peer_committed[i];
    cluster_->mini_tablet_server(i)->Shutdown();
  }

  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    ASSERT_OK(cluster_->mini_tablet_server(i)->Start());
    for (;;) {
      auto peers = cluster_->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers();
      ASSERT_LE(peers.size(), 1);
      if (peers.empty() || peers[0]->state() != tablet::RaftGroupStatePB::RUNNING) {
        std::this_thread::sleep_for(100ms);
        continue;
      }
      SCOPED_TRACE(Format("Peer: $0, index: $1", peers[0]->permanent_uuid(), i));
      ASSERT_GE(peers[0]->tablet()->RetentionPolicy()->GetRetentionDirective().history_cutoff,
                peer_committed[i]);
      break;
    }
    cluster_->mini_tablet_server(i)->Shutdown();
  }

  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    ASSERT_OK(cluster_->mini_tablet_server(i)->Start());
  }
  ASSERT_NO_FATALS(VerifyHistoryCutoff(cluster_.get(), &committed_history_cutoff, "After restart"));

  // Wait to check history cutoff advance w/o operations.
  std::this_thread::sleep_for(
      FLAGS_timestamp_history_retention_interval_sec * 1s +
      FLAGS_history_cutoff_propagation_interval_ms * 3ms);
  ASSERT_NO_FATALS(VerifyHistoryCutoff(cluster_.get(), &committed_history_cutoff, "Final"));
}

class QLTabletRf1Test : public QLTabletTest {
 public:
  void SetUp() override {
    mini_cluster_opt_.num_masters = 1;
    mini_cluster_opt_.num_tablet_servers = 1;
    QLTabletTest::SetUp();
  }
};

// For this test we don't need actually RF3 setup which also makes test flaky because of
// https://github.com/yugabyte/yugabyte-db/issues/4663.
TEST_F_EX(QLTabletTest, GetMiddleKey, QLTabletRf1Test) {
  FLAGS_db_write_buffer_size = 20_KB;

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTable1Name);
  workload.set_write_timeout_millis(30000);
  workload.set_num_tablets(1);
  workload.set_num_write_threads(2);
  workload.set_write_batch_size(1);
  workload.set_payload_bytes(16);
  workload.Setup();

  LOG(INFO) << "Starting workload ...";
  Stopwatch s(Stopwatch::ALL_THREADS);
  s.start();
  workload.Start();

  const auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kLeaders);
  ASSERT_EQ(peers.size(), 1);
  const auto& tablet = *ASSERT_NOTNULL(peers[0]->tablet());

  // We want some compactions to happen, so largest SST file will become large enough for its
  // approximate middle key to roughly split the whole tablet into two parts that are close in size.
  while (tablet.TEST_db()->GetCurrentVersionDataSstFilesSize() < 20 * FLAGS_db_write_buffer_size) {
    std::this_thread::sleep_for(100ms);
  }

  workload.StopAndJoin();
  s.stop();
  LOG(INFO) << "Workload stopped, it took: " << AsString(s.elapsed());

  LOG(INFO) << "Rows inserted: " << workload.rows_inserted();
  LOG(INFO) << "Number of SST files: " << tablet.TEST_db()->GetCurrentVersionNumSSTFiles();

  ASSERT_OK(cluster_->FlushTablets());

  const auto encoded_split_key = ASSERT_RESULT(tablet.GetEncodedMiddleSplitKey());
  LOG(INFO) << "Encoded split key: " << Slice(encoded_split_key).ToDebugString();

  if (tablet.metadata()->partition_schema()->IsHashPartitioning()) {
    docdb::DocKey split_key;
    Slice key_slice = encoded_split_key;
    ASSERT_OK(split_key.DecodeFrom(&key_slice, docdb::DocKeyPart::kUpToHashCode));
    ASSERT_TRUE(key_slice.empty()) << "Extra bytes after decoding: " << key_slice.ToDebugString();
    ASSERT_EQ(split_key.hashed_group().size() + split_key.range_group().size(), 0)
        << "Hash-based partition: middle key should only have encoded hash code";
    LOG(INFO) << "Split key: " << AsString(split_key);
  } else {
    docdb::SubDocKey split_key;
    ASSERT_OK(split_key.FullyDecodeFrom(encoded_split_key, docdb::HybridTimeRequired::kFalse));
    ASSERT_EQ(split_key.num_subkeys(), 0)
        << "Range-based partition: middle doc key should not have sub doc key components";
    LOG(INFO) << "Split key: " << AsString(split_key);
  }

  // Checking number of keys less/bigger than the approximate middle key.
  size_t total_keys = 0;
  size_t num_keys_less = 0;

  rocksdb::ReadOptions read_opts;
  read_opts.query_id = rocksdb::kDefaultQueryId;
  std::unique_ptr<rocksdb::Iterator> iter(tablet.TEST_db()->NewIterator(read_opts));

  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    Slice key = iter->key();
    if (key.Less(encoded_split_key)) {
      ++num_keys_less;
    }
    ++total_keys;
  }

  LOG(INFO) << "Total keys: " << total_keys;
  LOG(INFO) << "Number of keys less than approximate middle key: " << num_keys_less;
  const auto num_keys_less_percent = 100 * num_keys_less / total_keys;

  LOG(INFO) << Format(
      "Number of keys less than approximate middle key: $0 ($1%)", num_keys_less,
      num_keys_less_percent);

  ASSERT_GE(num_keys_less_percent, 40);
  ASSERT_LE(num_keys_less_percent, 60);
}

namespace {

std::vector<OpId> GetLastAppliedOpIds(const std::vector<tablet::TabletPeerPtr>& peers) {
  std::vector<OpId> last_applied_op_ids;
  for (auto& peer : peers) {
    const auto last_applied_op_id = peer->consensus()->GetLastAppliedOpId();
    VLOG(1) << "Peer: " << AsString(peer->permanent_uuid())
            << ", last applied op ID: " << AsString(last_applied_op_id);
    last_applied_op_ids.push_back(last_applied_op_id);
  }
  return last_applied_op_ids;
}

Result<OpId> GetAllAppliedOpId(const std::vector<tablet::TabletPeerPtr>& peers) {
  for (auto& peer : peers) {
    if (peer->LeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY) {
      return peer->raft_consensus()->TEST_GetAllAppliedOpId();
    }
  }
  return STATUS(NotFound, "No leader found");
}

Status WaitForAppliedOpIdsStabilized(
    const std::vector<tablet::TabletPeerPtr>& peers, const MonoDelta& timeout) {
  std::vector<OpId> prev_last_applied_op_ids;
  return WaitFor(
      [&]() {
        std::vector<OpId> last_applied_op_ids = GetLastAppliedOpIds(peers);
        return last_applied_op_ids == prev_last_applied_op_ids;
      },
      timeout, "Waiting for applied op IDs to stabilize", 500ms * kTimeMultiplier, 1);
}

} // namespace

TEST_F(QLTabletTest, LastAppliedOpIdTracking) {
  constexpr auto kAppliesTimeout = 5s * kTimeMultiplier;

  TableHandle table;
  CreateTable(kTable1Name, &table, /* num_tablets =*/1);
  auto session = client_->NewSession();
  session->SetTimeout(60s);

  LOG(INFO) << "Writing data...";
  int key = 0;
  for (; key < 10; ++key) {
    SetValue(session, key, key, table);
  }
  LOG(INFO) << "Writing completed";

  auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);

  WaitForAppliedOpIdsStabilized(peers, kAppliesTimeout);
  auto last_applied_op_ids = GetLastAppliedOpIds(peers);
  auto all_applied_op_id = ASSERT_RESULT(GetAllAppliedOpId(peers));
  for (const auto& last_applied_op_id : last_applied_op_ids) {
    ASSERT_EQ(last_applied_op_id, all_applied_op_id);
  }

  LOG(INFO) << "Shutting down TS-0";
  cluster_->mini_tablet_server(0)->Shutdown();

  peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);

  LOG(INFO) << "Writing more data...";
  for (; key < 20; ++key) {
    SetValue(session, key, key, table);
  }
  LOG(INFO) << "Writing completed";

  WaitForAppliedOpIdsStabilized(peers, kAppliesTimeout);
  auto new_all_applied_op_id = ASSERT_RESULT(GetAllAppliedOpId(peers));
  // We expect turned off TS to lag behind and not let all applied OP ids to advance.
  // In case TS-0 was leader, all_applied_op_id will be 0 on a new leader until it hears from TS-0.
  ASSERT_TRUE(new_all_applied_op_id == all_applied_op_id || new_all_applied_op_id.empty());

  // Remember max applied op ID.
  last_applied_op_ids = GetLastAppliedOpIds(peers);
  auto max_applied_op_id = OpId::Min();
  for (const auto& last_applied_op_id : last_applied_op_ids) {
    max_applied_op_id = std::max(max_applied_op_id, last_applied_op_id);
  }
  ASSERT_GT(max_applied_op_id, all_applied_op_id);

  LOG(INFO) << "Restarting TS-0";
  ASSERT_OK(cluster_->mini_tablet_server(0)->Start());

  // TS-0 should catch up on applied ops.
  ASSERT_OK(WaitFor(
      [&]() -> Result<bool> {
        return VERIFY_RESULT(GetAllAppliedOpId(peers)) == max_applied_op_id;
      },
      kAppliesTimeout, "Waiting for all ops to apply"));
  last_applied_op_ids = GetLastAppliedOpIds(peers);
  for (const auto& last_applied_op_id : last_applied_op_ids) {
    ASSERT_EQ(last_applied_op_id, max_applied_op_id);
  }
}

} // namespace client
} // namespace yb
