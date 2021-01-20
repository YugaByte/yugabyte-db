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

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <chrono>
#include <boost/assign.hpp>
#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "yb/common/wire_protocol.h"
#include "yb/common/schema.h"

#include "yb/cdc/cdc_service.h"
#include "yb/cdc/cdc_service.pb.h"
#include "yb/cdc/cdc_service.proxy.h"
#include "yb/client/client.h"
#include "yb/client/client-test-util.h"
#include "yb/client/meta_cache.h"
#include "yb/client/schema.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"
#include "yb/client/transaction.h"
#include "yb/client/yb_op.h"

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/cdc_test_util.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/twodc_test_base.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/master/mini_master.h"
#include "yb/master/master.h"
#include "yb/master/master.pb.h"
#include "yb/master/master-test-util.h"

#include "yb/master/cdc_consumer_registry_service.h"
#include "yb/server/hybrid_clock.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/tserver/cdc_consumer.h"
#include "yb/util/atomic.h"
#include "yb/util/faststring.h"
#include "yb/util/random.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"

using namespace std::literals;

DECLARE_int32(replication_factor);
DECLARE_bool(TEST_twodc_write_hybrid_time);
DECLARE_int32(cdc_wal_retention_time_secs);
DECLARE_int32(replication_failure_delay_exponent);
DECLARE_double(TEST_respond_write_failed_probability);
DECLARE_int32(cdc_max_apply_batch_num_records);
DECLARE_int32(async_replication_idle_delay_ms);
DECLARE_int32(async_replication_max_idle_wait);
DECLARE_int32(external_intent_cleanup_secs);

namespace yb {

using client::YBClient;
using client::YBClientBuilder;
using client::YBColumnSchema;
using client::YBError;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBSession;
using client::YBTable;
using client::YBTableAlterer;
using client::YBTableCreator;
using client::YBTableType;
using client::YBTableName;
using master::MiniMaster;
using tserver::MiniTabletServer;
using tserver::enterprise::CDCConsumer;

namespace enterprise {

using SessionTransactionPair = std::pair<client::YBSessionPtr, client::YBTransactionPtr>;

class TwoDCTest : public TwoDCTestBase, public testing::WithParamInterface<TwoDCTestParams> {
 public:
  Result<std::vector<std::shared_ptr<client::YBTable>>> SetUpWithParams(
      const std::vector<uint32_t>& num_consumer_tablets,
      const std::vector<uint32_t>& num_producer_tablets,
      uint32_t replication_factor,
      uint32_t num_masters = 1) {
    FLAGS_enable_ysql = false;
    TwoDCTestBase::SetUp();
    FLAGS_cdc_max_apply_batch_num_records = GetParam().batch_size;
    FLAGS_cdc_enable_replicate_intents = GetParam().enable_replicate_intents;

    MiniClusterOptions opts;
    opts.num_tablet_servers = replication_factor;
    opts.num_masters = num_masters;
    FLAGS_replication_factor = replication_factor;
    opts.cluster_id = "producer";
    producer_cluster_.mini_cluster_ = std::make_unique<MiniCluster>(Env::Default(), opts);
    RETURN_NOT_OK(producer_cluster()->StartSync());
    RETURN_NOT_OK(producer_cluster()->WaitForTabletServerCount(replication_factor));

    opts.cluster_id = "consumer";
    consumer_cluster_.mini_cluster_ = std::make_unique<MiniCluster>(Env::Default(), opts);
    RETURN_NOT_OK(consumer_cluster()->StartSync());
    RETURN_NOT_OK(consumer_cluster()->WaitForTabletServerCount(replication_factor));

    producer_cluster_.client_ = VERIFY_RESULT(producer_cluster()->CreateClient());
    consumer_cluster_.client_ = VERIFY_RESULT(consumer_cluster()->CreateClient());

    RETURN_NOT_OK(clock_->Init());
    producer_cluster_.txn_mgr_.emplace(producer_client(), clock_, client::LocalTabletFilter());
    consumer_cluster_.txn_mgr_.emplace(consumer_client(), clock_, client::LocalTabletFilter());

    YBSchemaBuilder b;
    b.AddColumn("c0")->Type(INT32)->NotNull()->HashPrimaryKey();

    // Create transactional table.
    TableProperties table_properties;
    table_properties.SetTransactional(true);
    b.SetTableProperties(table_properties);
    CHECK_OK(b.Build(&schema_));

    YBSchema consumer_schema;
    table_properties.SetDefaultTimeToLive(0);
    b.SetTableProperties(table_properties);
    CHECK_OK(b.Build(&consumer_schema));

    if (num_consumer_tablets.size() != num_producer_tablets.size()) {
      return STATUS(IllegalState,
                    Format("Num consumer tables: $0 num producer tables: $1 must be equal.",
                           num_consumer_tablets.size(), num_producer_tablets.size()));
    }

    std::vector<YBTableName> tables;
    std::vector<std::shared_ptr<client::YBTable>> yb_tables;
    for (int i = 0; i < num_consumer_tablets.size(); i++) {
      RETURN_NOT_OK(CreateTable(i, num_producer_tablets[i], producer_client(), &tables));
      std::shared_ptr<client::YBTable> producer_table;
      RETURN_NOT_OK(producer_client()->OpenTable(tables[i * 2], &producer_table));
      yb_tables.push_back(producer_table);

      RETURN_NOT_OK(CreateTable(i, num_consumer_tablets[i], consumer_client(),
                                consumer_schema, &tables));
      std::shared_ptr<client::YBTable> consumer_table;
      RETURN_NOT_OK(consumer_client()->OpenTable(tables[(i * 2) + 1], &consumer_table));
      yb_tables.push_back(consumer_table);
    }

    return yb_tables;
  }

  Result<YBTableName> CreateTable(YBClient* client, const std::string& namespace_name,
                                  const std::string& table_name, uint32_t num_tablets,
                                  const YBSchema* schema = nullptr) {
    YBTableName table(YQL_DATABASE_CQL, namespace_name, table_name);
    RETURN_NOT_OK(client->CreateNamespaceIfNotExists(table.namespace_name(),
                                                     table.namespace_type()));

    if (!schema) {
      schema = &schema_;
    }
    // Add a table, make sure it reports itself.
    std::unique_ptr<YBTableCreator> table_creator(client->NewTableCreator());
        RETURN_NOT_OK(table_creator->table_name(table)
                          .schema(schema)
                          .table_type(YBTableType::YQL_TABLE_TYPE)
                          .num_tablets(num_tablets)
                          .Create());
    return table;
  }

  Status CreateTable(
      uint32_t idx, uint32_t num_tablets, YBClient* client, std::vector<YBTableName>* tables) {
    auto table = VERIFY_RESULT(CreateTable(client, kNamespaceName, Format("test_table_$0", idx),
                                           num_tablets));
    tables->push_back(table);
    return Status::OK();
  }

  Status CreateTable(uint32_t idx, uint32_t num_tablets, YBClient* client, YBSchema schema,
                     std::vector<YBTableName>* tables) {
    auto table = VERIFY_RESULT(CreateTable(client, kNamespaceName, Format("test_table_$0", idx),
                                           num_tablets, &schema));
    tables->push_back(table);
    return Status::OK();
  }

  void WriteWorkload(uint32_t start, uint32_t end, YBClient* client, const YBTableName& table,
                     bool delete_op = false) {
    auto session = client->NewSession();
    client::TableHandle table_handle;
    ASSERT_OK(table_handle.Open(table, client));
    std::vector<std::shared_ptr<client::YBqlOp>> ops;

    LOG(INFO) << "Writing " << end-start << (delete_op ? " deletes" : " inserts");
    for (uint32_t i = start; i < end; i++) {
      auto op = delete_op ? table_handle.NewDeleteOp() : table_handle.NewInsertOp();
      int32_t key = i;
      auto req = op->mutable_request();
      QLAddInt32HashValue(req, key);
      ASSERT_OK(session->ApplyAndFlush(op));
    }
  }

  void DeleteWorkload(uint32_t start, uint32_t end, YBClient* client, const YBTableName& table) {
    WriteWorkload(start, end, client, table, true /* delete_op */);
  }

  std::vector<string> ScanToStrings(const YBTableName& table_name, YBClient* client) {
    client::TableHandle table;
    EXPECT_OK(table.Open(table_name, client));
    auto result = ScanTableToStrings(table);
    std::sort(result.begin(), result.end());
    return result;
  }

  Status VerifyWrittenRecords(const YBTableName& producer_table,
                              const YBTableName& consumer_table,
                              int timeout_secs = kRpcTimeout) {
    return LoggedWaitFor([=]() -> Result<bool> {
      auto producer_results = ScanToStrings(producer_table, producer_client());
      auto consumer_results = ScanToStrings(consumer_table, consumer_client());
      return producer_results == consumer_results;
    }, MonoDelta::FromSeconds(timeout_secs), "Verify written records");
  }

  Status VerifyNumRecords(const YBTableName& table, YBClient* client, int expected_size) {
    return LoggedWaitFor([=]() -> Result<bool> {
      auto results = ScanToStrings(table, client);
      return results.size() == expected_size;
    }, MonoDelta::FromSeconds(kRpcTimeout), "Verify number of records");
  }

  Result<SessionTransactionPair> CreateSessionWithTransaction(
      YBClient* client, client::TransactionManager* txn_mgr) {
    auto session = client->NewSession();
    auto transaction = std::make_shared<client::YBTransaction>(txn_mgr);
    ReadHybridTime read_time;
    RETURN_NOT_OK(transaction->Init(IsolationLevel::SNAPSHOT_ISOLATION, read_time));
    session->SetTransaction(transaction);
    return std::make_pair(session, transaction);
  }

  void WriteIntents(uint32_t start, uint32_t end, YBClient* client,
                    const std::shared_ptr<YBSession>& session, const YBTableName& table,
                    bool delete_op = false) {
    client::TableHandle table_handle;
    ASSERT_OK(table_handle.Open(table, client));
    std::vector<std::shared_ptr<client::YBqlOp>> ops;

    for (uint32_t i = start; i < end; i++) {
      auto op = delete_op ? table_handle.NewDeleteOp() : table_handle.NewInsertOp();
      int32_t key = i;
      auto req = op->mutable_request();
      QLAddInt32HashValue(req, key);
      ASSERT_OK(session->ApplyAndFlush(op));
    }
  }

  void WriteTransactionalWorkload(uint32_t start, uint32_t end, YBClient* client,
                                  client::TransactionManager* txn_mgr, const YBTableName& table,
                                  bool delete_op = false) {
    auto pair = ASSERT_RESULT(CreateSessionWithTransaction(client, txn_mgr));
    ASSERT_NO_FATALS(WriteIntents(start, end, client, pair.first, table, delete_op));
    pair.second->CommitFuture().get();
  }

 private:
  server::ClockPtr clock_{new server::HybridClock()};

  YBSchema schema_;
};

INSTANTIATE_TEST_CASE_P(TwoDCTestParams, TwoDCTest,
                        ::testing::Values(TwoDCTestParams(1, true), TwoDCTestParams(1, false),
                                          TwoDCTestParams(0, true), TwoDCTestParams(0, false)));

TEST_P(TwoDCTest, SetupUniverseReplication) {
  auto tables = ASSERT_RESULT(SetUpWithParams({8, 4}, {6, 6}, 3));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer tables from the list.
  producer_tables.reserve(tables.size() / 2);
  for (int i = 0; i < tables.size(); i += 2) {
    producer_tables.push_back(tables[i]);
  }
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // Verify that universe was setup on consumer.
  master::GetUniverseReplicationResponsePB resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, &resp));
  ASSERT_EQ(resp.entry().producer_id(), kUniverseId);
  ASSERT_EQ(resp.entry().tables_size(), producer_tables.size());
  for (int i = 0; i < producer_tables.size(); i++) {
    ASSERT_EQ(resp.entry().tables(i), producer_tables[i]->id());
  }

  // Verify that CDC streams were created on producer for all tables.
  for (int i = 0; i < producer_tables.size(); i++) {
    master::ListCDCStreamsResponsePB stream_resp;
    ASSERT_OK(GetCDCStreamForTable(producer_tables[i]->id(), &stream_resp));
    ASSERT_EQ(stream_resp.streams_size(), 1);
    ASSERT_EQ(stream_resp.streams(0).table_id(), producer_tables[i]->id());
  }

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, SetupUniverseReplicationWithProducerBootstrapId) {
  constexpr int kNTabletsPerTable = 1;
  std::vector<uint32_t> tables_vector = {kNTabletsPerTable, kNTabletsPerTable};
  auto tables = ASSERT_RESULT(SetUpWithParams(tables_vector, tables_vector, 3));
  auto producer_master_proxy = std::make_shared<master::MasterServiceProxy>(
      &producer_client()->proxy_cache(),
      producer_cluster()->leader_mini_master()->bound_rpc_addr());

  std::unique_ptr<client::YBClient> client;
  std::unique_ptr<cdc::CDCServiceProxy> producer_cdc_proxy;
  client = ASSERT_RESULT(consumer_cluster()->CreateClient());
  producer_cdc_proxy = std::make_unique<cdc::CDCServiceProxy>(
      &client->proxy_cache(),
      HostPort::FromBoundEndpoint(producer_cluster()->mini_tablet_server(0)->bound_rpc_addr()));

  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer tables from the list.
  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  std::vector<std::shared_ptr<client::YBTable>> consumer_tables;
  producer_tables.reserve(tables.size() / 2);
  consumer_tables.reserve(tables.size() / 2);
  for (int i = 0; i < tables.size(); i ++) {
    if (i % 2 == 0) {
      producer_tables.push_back(tables[i]);
    } else {
      consumer_tables.push_back(tables[i]);
    }
  }

  // 1. Write some data so that we can verify that only new records get replicated
  for (const auto& producer_table : producer_tables) {
    LOG(INFO) << "Writing records for table " << producer_table->name().ToString();
    WriteWorkload(0, 100, producer_client(), producer_table->name());
  }

  SleepFor(MonoDelta::FromSeconds(10));
  cdc::BootstrapProducerRequestPB req;
  cdc::BootstrapProducerResponsePB resp;

  for (const auto& producer_table : producer_tables) {
    req.add_table_ids(producer_table->id());
  }

  rpc::RpcController rpc;
  producer_cdc_proxy->BootstrapProducer(req, &resp, &rpc);
  ASSERT_FALSE(resp.has_error());

  ASSERT_EQ(resp.cdc_bootstrap_ids().size(), producer_tables.size());

  int table_idx = 0;
  for (const auto& bootstrap_id : resp.cdc_bootstrap_ids()) {
    LOG(INFO) << "Got bootstrap id " << bootstrap_id
              << " for table " << producer_tables[table_idx++]->name().table_name();
  }

  std::unordered_map<std::string, int> tablet_bootstraps;

  // Verify that for each of the table's tablets, a new row in cdc_state table with the returned
  // id was inserted.
  client::TableHandle table;
  client::YBTableName cdc_state_table(
      YQL_DATABASE_CQL, master::kSystemNamespaceName, master::kCdcStateTableName);
  ASSERT_OK(table.Open(cdc_state_table, producer_client()));

  // 2 tables with 8 tablets each.
  ASSERT_EQ(tables_vector.size() * kNTabletsPerTable, boost::size(client::TableRange(table)));
  int nrows = 0;
  for (const auto& row : client::TableRange(table)) {
    nrows++;
    string stream_id = row.column(0).string_value();
    tablet_bootstraps[stream_id]++;

    string checkpoint = row.column(2).string_value();
    auto s = OpId::FromString(checkpoint);
    ASSERT_OK(s);
    OpId op_id = *s;
    ASSERT_GT(op_id.index, 0);

    LOG(INFO) << "Bootstrap id " << stream_id
              << " for tablet " << row.column(1).string_value();
  }

  ASSERT_EQ(tablet_bootstraps.size(), producer_tables.size());
  // Check that each bootstrap id has 8 tablets.
  for (const auto& e : tablet_bootstraps) {
    ASSERT_EQ(e.second, kNTabletsPerTable);
  }

  // Map table -> bootstrap_id. We will need when setting up replication.
  std::unordered_map<TableId, std::string> table_bootstrap_ids;
  for (int i = 0; i < resp.cdc_bootstrap_ids_size(); i++) {
    table_bootstrap_ids[req.table_ids(i)] = resp.cdc_bootstrap_ids(i);
  }

  // 2. Setup replication.
  master::SetupUniverseReplicationRequestPB setup_universe_req;
  master::SetupUniverseReplicationResponsePB setup_universe_resp;
  setup_universe_req.set_producer_id(kUniverseId);
  string master_addr = producer_cluster()->GetMasterAddresses();
  auto hp_vec = ASSERT_RESULT(HostPort::ParseStrings(master_addr, 0));
  HostPortsToPBs(hp_vec, setup_universe_req.mutable_producer_master_addresses());

  setup_universe_req.mutable_producer_table_ids()->Reserve(producer_tables.size());
  for (const auto& producer_table : producer_tables) {
    setup_universe_req.add_producer_table_ids(producer_table->id());
    const auto& iter = table_bootstrap_ids.find(producer_table->id());
    ASSERT_NE(iter, table_bootstrap_ids.end());
    setup_universe_req.add_producer_bootstrap_ids(iter->second);
  }

  auto master_proxy = std::make_shared<master::MasterServiceProxy>(
      &consumer_client()->proxy_cache(),
      consumer_cluster()->leader_mini_master()->bound_rpc_addr());

  rpc.Reset();
  rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
  ASSERT_OK(master_proxy->SetupUniverseReplication(setup_universe_req, &setup_universe_resp, &rpc));
  ASSERT_FALSE(setup_universe_resp.has_error());

  // 3. Verify everything is setup correctly.
  master::GetUniverseReplicationResponsePB get_universe_replication_resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId,
      &get_universe_replication_resp));
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(),
                                       tables_vector.size() * kNTabletsPerTable));

  // 4. Write more data.
  for (const auto& producer_table : producer_tables) {
    WriteWorkload(1000, 1005, producer_client(), producer_table->name());
  }

  // 5. Verify that only new writes get replicated to consumer since we bootstrapped the producer
  // after we had already written some data, therefore the old data (whatever was there before we
  // bootstrapped the producer) should not be replicated.
  auto data_replicated_correctly = [&]() {
    for (const auto& consumer_table : consumer_tables) {
      LOG(INFO) << "Checking records for table " << consumer_table->name().ToString();
      std::vector<std::string> expected_results;
      for (int key = 1000; key < 1005; key++) {
        expected_results.emplace_back("{ int32:" + std::to_string(key) + " }");
      }
      std::sort(expected_results.begin(), expected_results.end());

      auto consumer_results = ScanToStrings(consumer_table->name(), consumer_client());
      std::sort(consumer_results.begin(), consumer_results.end());

      if (expected_results.size() != consumer_results.size()) {
        return false;
      }

      for (int idx = 0; idx < expected_results.size(); idx++) {
        if (expected_results[idx] != consumer_results[idx]) {
          return false;
        }
      }
    }
    return true;
  };
  ASSERT_OK(WaitFor([&]() { return data_replicated_correctly(); },
                    MonoDelta::FromSeconds(20), "IsDataReplicatedCorrectly"));
}

// Test for #2250 to verify that replication for tables with the same prefix gets set up correctly.
TEST_P(TwoDCTest, SetupUniverseReplicationMultipleTables) {
  // Setup the two clusters without any tables.
  auto tables = ASSERT_RESULT(SetUpWithParams({}, {}, 1));

  // Create tables with the same prefix.
  std::string table_names[2] = {"table", "table_index"};

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  for (int i = 0; i < 2; i++) {
    auto t = ASSERT_RESULT(CreateTable(producer_client(), kNamespaceName, table_names[i], 3));
    std::shared_ptr<client::YBTable> producer_table;
    ASSERT_OK(producer_client()->OpenTable(t, &producer_table));
    producer_tables.push_back(producer_table);
  }

  for (int i = 0; i < 2; i++) {
    ASSERT_RESULT(CreateTable(consumer_client(), kNamespaceName, table_names[i], 3));
  }

  // Setup universe replication on both these tables.
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // Verify that universe was setup on consumer.
  master::GetUniverseReplicationResponsePB resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, &resp));
  ASSERT_EQ(resp.entry().producer_id(), kUniverseId);
  ASSERT_EQ(resp.entry().tables_size(), producer_tables.size());
  for (int i = 0; i < producer_tables.size(); i++) {
    ASSERT_EQ(resp.entry().tables(i), producer_tables[i]->id());
  }

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, PollWithConsumerRestart) {
  // Avoid long delays with node failures so we can run with more aggressive test timing
  FLAGS_replication_failure_delay_exponent = 7; // 2^7 == 128ms

  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({4}, {4}, replication_factor));

  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId,
      {tables[0]} /* all producer tables */));

  // After creating the cluster, make sure all tablets being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));

  consumer_cluster()->mini_tablet_server(0)->Shutdown();

  // After shutting down a single consumer node, the other consumers should pick up the slack.
  if (replication_factor > 1) {
    ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));
  }

  ASSERT_OK(consumer_cluster()->mini_tablet_server(0)->Start());

  // After restarting the node.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));

  ASSERT_OK(consumer_cluster()->RestartSync());

  // After consumer cluster restart.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, PollWithProducerNodesRestart) {
  // Avoid long delays with node failures so we can run with more aggressive test timing
  FLAGS_replication_failure_delay_exponent = 7; // 2^7 == 128ms

  uint32_t replication_factor = 3, tablet_count = 4, master_count = 3;
  auto tables = ASSERT_RESULT(
      SetUpWithParams({tablet_count}, {tablet_count}, replication_factor,  master_count));

  ASSERT_OK(SetupUniverseReplication(
    producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId,
    {tables[0]} /* all producer tables */, false /* leader_only */));

  // After creating the cluster, make sure all tablets being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));

  // Stop the Master and wait for failover.
  LOG(INFO) << "Failover to new Master";
  MiniMaster* old_master = producer_cluster()->leader_mini_master();
  ASSERT_OK(old_master->WaitUntilCatalogManagerIsLeaderAndReadyForTests());
  producer_cluster()->leader_mini_master()->Shutdown();
  MiniMaster* new_master = producer_cluster()->leader_mini_master();
  ASSERT_NE(nullptr, new_master);
  ASSERT_NE(old_master, new_master);
  ASSERT_OK(producer_cluster()->WaitForAllTabletServers());

  // Stop a TServer on the Producer after failing its master.
  producer_cluster()->mini_tablet_server(0)->Shutdown();
  // This Verifies:
  // 1. Consumer successfully transitions over to using the new master for Tablet lookup.
  // 2. Consumer cluster has rebalanced all the CDC Pollers
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));
  WriteWorkload(0, 5, producer_client(), tables[0]->name());
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Restart the Producer TServer and verify that rebalancing happens.
  ASSERT_OK(old_master->Start());
  ASSERT_OK(producer_cluster()->mini_tablet_server(0)->Start());
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));
  WriteWorkload(6, 10, producer_client(), tables[0]->name());
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Cleanup.
  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, PollWithProducerClusterRestart) {
  // Avoid long delays with node failures so we can run with more aggressive test timing
  FLAGS_replication_failure_delay_exponent = 7; // 2^7 == 128ms

  uint32_t replication_factor = 3, tablet_count = 4;
  auto tables = ASSERT_RESULT(
      SetUpWithParams({tablet_count}, {tablet_count}, replication_factor));

  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId,
      {tables[0]} /* all producer tables */));

  // After creating the cluster, make sure all tablets being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));

  // Restart the ENTIRE Producer cluster.
  ASSERT_OK(producer_cluster()->RestartSync());

  // After producer cluster restart.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 4));
  WriteWorkload(0, 5, producer_client(), tables[0]->name());
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Cleanup.
  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}


TEST_P(TwoDCTest, PollAndObserveIdleDampening) {
  uint32_t replication_factor = 3, tablet_count = 1, master_count = 1;
  auto tables = ASSERT_RESULT(
      SetUpWithParams({tablet_count}, {tablet_count}, replication_factor,  master_count));

  ASSERT_OK(SetupUniverseReplication(producer_cluster(), consumer_cluster(), consumer_client(),
                                     kUniverseId, {tables[0]} , false ));

  // After creating the cluster, make sure all tablets being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 1));

  // Write some Info and query GetChanges to setup the CDCTabletMetrics.
  WriteWorkload(0, 5, producer_client(), tables[0]->name());
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  /*****************************************************************
   * Find the CDC Tablet Metrics, which we will use for this test. *
   *****************************************************************/
  // Find the stream.
  master::ListCDCStreamsResponsePB stream_resp;
  ASSERT_OK(GetCDCStreamForTable(tables[0]->id(), &stream_resp));
  ASSERT_EQ(stream_resp.streams_size(), 1);
  ASSERT_EQ(stream_resp.streams(0).table_id(), tables[0]->id());
  auto stream_id = stream_resp.streams(0).stream_id();

  // Find the tablet id for the stream.
  TabletId tablet_id;
  {
    yb::cdc::ListTabletsRequestPB tablets_req;
    yb::cdc::ListTabletsResponsePB tablets_resp;
    rpc::RpcController rpc;
    tablets_req.set_stream_id(stream_id);

    auto producer_cdc_proxy = std::make_unique<cdc::CDCServiceProxy>(
        &producer_client()->proxy_cache(),
        HostPort::FromBoundEndpoint(producer_cluster()->mini_tablet_server(0)->bound_rpc_addr()));
    ASSERT_OK(producer_cdc_proxy->ListTablets(tablets_req, &tablets_resp, &rpc));
    ASSERT_FALSE(tablets_resp.has_error());
    ASSERT_EQ(tablets_resp.tablets_size(), 1);
    tablet_id = tablets_resp.tablets(0).tablet_id();
  }

  // Find the TServer that is hosting this tablet.
  tserver::TabletServer* cdc_ts = nullptr;
  std::string ts_uuid;
  std::mutex data_mutex;
  {
    ASSERT_OK(WaitFor([this, &tablet_id, &table = tables[0], &ts_uuid, &data_mutex] {
        producer_client()->LookupTabletById(
            tablet_id, table, CoarseMonoClock::Now() + MonoDelta::FromSeconds(3),
            [&ts_uuid, &data_mutex](const Result<client::internal::RemoteTabletPtr>& result) {
              if (result.ok()) {
                std::lock_guard<std::mutex> l(data_mutex);
                ts_uuid = (*result)->LeaderTServer()->permanent_uuid();
              }
            },
            client::UseCache::kFalse);
        std::lock_guard<std::mutex> l(data_mutex);
        return !ts_uuid.empty();
      }, MonoDelta::FromSeconds(10), "Get TS for Tablet"));

    for (auto ts : producer_cluster()->mini_tablet_servers()) {
      if (ts->server()->permanent_uuid() == ts_uuid) {
        cdc_ts = ts->server();
        break;
      }
    }
  }
  ASSERT_NOTNULL(cdc_ts);

  // Find the CDCTabletMetric associated with the above pair.
  auto cdc_service = dynamic_cast<cdc::CDCServiceImpl*>(
    cdc_ts->rpc_server()->service_pool("yb.cdc.CDCService")->TEST_get_service().get());
  std::shared_ptr<cdc::CDCTabletMetrics> metrics =
      cdc_service->GetCDCTabletMetrics({"", stream_id, tablet_id});

  /***********************************
   * Setup Complete.  Starting test. *
   ***********************************/
  // Log the first heartbeat count for baseline
  auto first_heartbeat_count = metrics->rpc_heartbeats_responded->value();
  LOG(INFO) << "first_heartbeat_count = " << first_heartbeat_count;

  // Write some Info to the producer, which should be consumed quickly by GetChanges.
  WriteWorkload(6, 10, producer_client(), tables[0]->name());
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Sleep for the idle timeout.
  SleepFor(MonoDelta::FromMilliseconds(FLAGS_async_replication_idle_delay_ms));
  auto active_heartbeat_count = metrics->rpc_heartbeats_responded->value();
  LOG(INFO) << "active_heartbeat_count  = " << active_heartbeat_count;
  // The new heartbeat count should be at least 3 (idle_wait)
  ASSERT_GE(active_heartbeat_count - first_heartbeat_count, FLAGS_async_replication_max_idle_wait);

  // Now, wait past update request frequency, so we should be using idle timing.
  auto multiplier = 2;
  SleepFor(MonoDelta::FromMilliseconds(FLAGS_async_replication_idle_delay_ms * multiplier));
  auto idle_heartbeat_count = metrics->rpc_heartbeats_responded->value();
  ASSERT_LE(idle_heartbeat_count - active_heartbeat_count, multiplier + 1 /*allow subtle race*/);
  LOG(INFO) << "idle_heartbeat_count = " << idle_heartbeat_count;

  // Write some more data to the producer and call GetChanges with some real data.
  WriteWorkload(11, 15, producer_client(), tables[0]->name());
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Sleep for the idle timeout and Verify that the idle behavior ended now that we have new data.
  SleepFor(MonoDelta::FromMilliseconds(FLAGS_async_replication_idle_delay_ms));
  active_heartbeat_count = metrics->rpc_heartbeats_responded->value();
  LOG(INFO) << "active_heartbeat_count  = " << active_heartbeat_count;
  // The new heartbeat count should be at least 3 (idle_wait)
  ASSERT_GE(active_heartbeat_count - idle_heartbeat_count, FLAGS_async_replication_max_idle_wait);

  // Cleanup.
  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, ApplyOperations) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  // Use just one tablet here to more easily catch lower-level write issues with this test.
  auto tables = ASSERT_RESULT(SetUpWithParams({1}, {1}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 1));

  WriteWorkload(0, 5, producer_client(), tables[0]->name());

  // Check that all tablets continue to be polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 1));

  // Verify that both clusters have the same records.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, ApplyOperationsWithTransactions) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({2}, {2}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));

  // Write some transactional rows.
  WriteTransactionalWorkload(0, 5, producer_client(), producer_txn_mgr(), tables[0]->name());

  // Write some non-transactional rows.
  WriteWorkload(6, 10, producer_client(), tables[0]->name());

  // Check that all tablets continue to be polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));

  // Verify that both clusters have the same records.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

class TwoDCTestWithEnableIntentsReplication : public TwoDCTest {
};

INSTANTIATE_TEST_CASE_P(TwoDCTestParams, TwoDCTestWithEnableIntentsReplication,
                        ::testing::Values(TwoDCTestParams(0, true), TwoDCTestParams(1, true)));

TEST_P(TwoDCTestWithEnableIntentsReplication, UpdateWithinTransaction) {
  constexpr int kNumTablets = 1;
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({kNumTablets}, {kNumTablets}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), kNumTablets));

  auto txn = ASSERT_RESULT(CreateSessionWithTransaction(producer_client(), producer_txn_mgr()));
  for (bool del : {false, true}) {
    WriteIntents(1, 5, producer_client(), txn.first, tables[0]->name(), del);
  }
  ASSERT_OK(txn.second->CommitFuture().get());

  txn.first->SetTransaction(nullptr);
  client::TableHandle table_handle;
  ASSERT_OK(table_handle.Open(tables[0]->name(), producer_client()));
  auto op = table_handle.NewInsertOp();
  auto req = op->mutable_request();
  QLAddInt32HashValue(req, 0);
  ASSERT_OK(txn.first->ApplyAndFlush(op));

  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Check that all tablets continue to be polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), kNumTablets));

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTestWithEnableIntentsReplication, TransactionsWithRestart) {
  auto tables = ASSERT_RESULT(SetUpWithParams({2}, {2}, 3));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables = { tables[0] };
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));

  auto txn = ASSERT_RESULT(CreateSessionWithTransaction(producer_client(), producer_txn_mgr()));
  // Write some transactional rows.
  WriteTransactionalWorkload(
      0, 5, producer_client(), producer_txn_mgr(), tables[0]->name(), /* delete_op */ false);

  WriteWorkload(6, 10, producer_client(), tables[0]->name());

  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));
  std::this_thread::sleep_for(5s);
  ASSERT_OK(consumer_cluster()->FlushTablets(
      tablet::FlushMode::kSync, tablet::FlushFlags::kRegular));
  LOG(INFO) << "Restart";
  ASSERT_OK(consumer_cluster()->RestartSync());
  std::this_thread::sleep_for(5s);
  LOG(INFO) << "Commit";
  ASSERT_OK(txn.second->CommitFuture().get());

  // Verify that both clusters have the same records.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTestWithEnableIntentsReplication, MultipleTransactions) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({1}, {1}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 1));

  auto txn_0 = ASSERT_RESULT(CreateSessionWithTransaction(producer_client(), producer_txn_mgr()));
  auto txn_1 = ASSERT_RESULT(CreateSessionWithTransaction(producer_client(), producer_txn_mgr()));

  ASSERT_NO_FATALS(WriteIntents(0, 5, producer_client(), txn_0.first, tables[0]->name()));
  ASSERT_NO_FATALS(WriteIntents(5, 10, producer_client(), txn_0.first, tables[0]->name()));
  ASSERT_NO_FATALS(WriteIntents(10, 15, producer_client(), txn_1.first, tables[0]->name()));
  ASSERT_NO_FATALS(WriteIntents(10, 20, producer_client(), txn_1.first, tables[0]->name()));

  ASSERT_OK(WaitFor([&]() {
    return CountIntents(consumer_cluster()) > 0;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Consumer cluster replicated intents"));

  // Make sure that none of the intents replicated have been committed.
  auto consumer_results = ScanToStrings(tables[1]->name(), consumer_client());
  ASSERT_EQ(consumer_results.size(), 0);

  txn_0.second->CommitFuture().get();
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  txn_1.second->CommitFuture().get();
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));
  ASSERT_OK(WaitFor([&]() {
    return CountIntents(consumer_cluster()) == 0;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Consumer cluster cleaned up intents"));
}

TEST_P(TwoDCTestWithEnableIntentsReplication, CleanupAbortedTransactions) {
  static const int kNumRecordsPerBatch = 5;
  const uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({1 /* num_consumer_tablets */},
                                              {1 /* num_producer_tablets */},
                                              replication_factor));
  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));
  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 1 /* num_producer_tablets */));
  auto txn_0 = ASSERT_RESULT(CreateSessionWithTransaction(producer_client(), producer_txn_mgr()));
  ASSERT_NO_FATALS(WriteIntents(0, kNumRecordsPerBatch, producer_client(), txn_0.first,
                                tables[0]->name()));
  // Wait for records to be replicated.
  ASSERT_OK(WaitFor([&]() {
    return CountIntents(consumer_cluster()) == kNumRecordsPerBatch * replication_factor;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Consumer cluster created intents"));
  ASSERT_OK(consumer_cluster()->FlushTablets());
  // Then, set timeout to 0 and make sure we do cleanup on the next compaction.
  SetAtomicFlag(0, &FLAGS_external_intent_cleanup_secs);
  ASSERT_NO_FATALS(WriteIntents(kNumRecordsPerBatch, kNumRecordsPerBatch * 2, producer_client(),
                                txn_0.first, tables[0]->name()));
  // Wait for records to be replicated.
  ASSERT_OK(WaitFor([&]() {
    return CountIntents(consumer_cluster()) == 2 * kNumRecordsPerBatch * replication_factor;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Consumer cluster created intents"));
  ASSERT_OK(consumer_cluster()->CompactTablets());
  ASSERT_OK(WaitFor([&]() {
    return CountIntents(consumer_cluster()) == 0;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Consumer cluster cleaned up intents"));
}

// Make sure when we compact a tablet, we retain intents.
TEST_P(TwoDCTestWithEnableIntentsReplication, NoCleanupOfFlushedFiles) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({1}, {1}, replication_factor));
  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 1));
  auto txn_0 = ASSERT_RESULT(CreateSessionWithTransaction(producer_client(), producer_txn_mgr()));
  ASSERT_NO_FATALS(WriteIntents(0, 5, producer_client(), txn_0.first, tables[0]->name()));
  auto consumer_results = ScanToStrings(tables[1]->name(), consumer_client());
  ASSERT_EQ(consumer_results.size(), 0);
  ASSERT_OK(consumer_cluster()->FlushTablets());
  ASSERT_NO_FATALS(WriteIntents(5, 10, producer_client(), txn_0.first, tables[0]->name()));
  ASSERT_OK(consumer_cluster()->FlushTablets());
  ASSERT_OK(consumer_cluster()->CompactTablets());
  // Wait for 5 seconds to make sure background CleanupIntents thread doesn't cleanup intents on the
  // consumer.
  SleepFor(MonoDelta::FromSeconds(5));
  txn_0.second->CommitFuture().get();
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));
  ASSERT_OK(WaitFor([&]() {
    return CountIntents(consumer_cluster()) == 0;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Consumer cluster cleaned up intents"));
}


TEST_P(TwoDCTestWithEnableIntentsReplication, ManyToOneTabletMapping) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({2}, {5}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 5));

  WriteTransactionalWorkload(0, 100, producer_client(), producer_txn_mgr(), tables[0]->name());
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name(), 60 /* timeout_secs */));
}

TEST_P(TwoDCTestWithEnableIntentsReplication, OneToManyTabletMapping) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({5}, {2}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));
  WriteTransactionalWorkload(0, 50, producer_client(), producer_txn_mgr(), tables[0]->name());
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name(), 60 /* timeout_secs */));
}

TEST_P(TwoDCTest, TestExternalWriteHybridTime) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({2}, {2}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));

  // Write 2 rows.
  WriteWorkload(0, 2, producer_client(), tables[0]->name());

  // Ensure that records can be read.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Delete 1 record.
  DeleteWorkload(0, 1, producer_client(), tables[0]->name());

  // Ensure that record is deleted on both universes.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Delete 2nd record but replicate at a low timestamp (timestamp lower than insertion timestamp).
  FLAGS_TEST_twodc_write_hybrid_time = true;
  DeleteWorkload(1, 2, producer_client(), tables[0]->name());

  // Verify that record exists on consumer universe, but is deleted from producer universe.
  ASSERT_OK(VerifyNumRecords(tables[0]->name(), producer_client(), 0));
  ASSERT_OK(VerifyNumRecords(tables[1]->name(), consumer_client(), 1));

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTestWithEnableIntentsReplication, BiDirectionalWrites) {
  auto tables = ASSERT_RESULT(SetUpWithParams({2}, {2}, 1));

  // Setup bi-directional replication.
  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables_reverse;
  producer_tables_reverse.push_back(tables[1]);
  ASSERT_OK(SetupUniverseReplication(
      consumer_cluster(), producer_cluster(), producer_client(), kUniverseId,
      producer_tables_reverse));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));
  ASSERT_OK(CorrectlyPollingAllTablets(producer_cluster(), 2));

  // Write non-conflicting rows on both clusters.
  WriteWorkload(0, 5, producer_client(), tables[0]->name());
  WriteWorkload(5, 10, consumer_client(), tables[1]->name());

  // Ensure that records are the same on both clusters.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));
  // Ensure that both universes have all 10 records.
  ASSERT_OK(VerifyNumRecords(tables[0]->name(), producer_client(), 10));

  // Write conflicting records on both clusters (1 clusters adds key, another deletes key).
  std::vector<std::thread> threads;
  for (int i = 0; i < 2; ++i) {
    auto client = i == 0 ? producer_client() : consumer_client();
    int index = i;
    bool is_delete = i == 0;
    threads.emplace_back([this, client, index, tables, is_delete] {
      WriteWorkload(10, 20, client, tables[index]->name(), is_delete);
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // Ensure that same records exist on both universes.
  VerifyWrittenRecords(tables[0]->name(), tables[1]->name());

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, AlterUniverseReplicationMasters) {
  // Tablets = Servers + 1 to stay simple but ensure round robin gives a tablet to everyone.
  uint32_t t_count = 2, master_count = 3;
  auto tables = ASSERT_RESULT(SetUpWithParams(
      {t_count, t_count}, {t_count, t_count}, 1,  master_count));

  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  std::vector<std::shared_ptr<client::YBTable>> producer_tables{tables[0], tables[2]},
    initial_tables{tables[0]};

  // SetupUniverseReplication only utilizes 1 master.
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, initial_tables));

  master::GetUniverseReplicationResponsePB v_resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, &v_resp));
  ASSERT_EQ(v_resp.entry().producer_master_addresses_size(), 1);
  ASSERT_EQ(HostPortFromPB(v_resp.entry().producer_master_addresses(0)),
            producer_cluster()->leader_mini_master()->bound_rpc_addr());

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), t_count));

  LOG(INFO) << "Alter Replication to include all Masters";
  // Alter Replication to include the other masters.
  {
    master::AlterUniverseReplicationRequestPB alter_req;
    master::AlterUniverseReplicationResponsePB alter_resp;
    alter_req.set_producer_id(kUniverseId);

    // GetMasterAddresses returns 3 masters.
    string master_addr = producer_cluster()->GetMasterAddresses();
    auto hp_vec = ASSERT_RESULT(HostPort::ParseStrings(master_addr, 0));
    HostPortsToPBs(hp_vec, alter_req.mutable_producer_master_addresses());

    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));

    auto master_proxy = std::make_shared<master::MasterServiceProxy>(
        &consumer_client()->proxy_cache(),
        consumer_cluster()->leader_mini_master()->bound_rpc_addr());
    ASSERT_OK(master_proxy->AlterUniverseReplication(alter_req, &alter_resp, &rpc));
    ASSERT_FALSE(alter_resp.has_error());

    // Verify that the consumer now has all masters.
    ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      master::GetUniverseReplicationResponsePB tmp_resp;
      return VerifyUniverseReplication(consumer_cluster(), consumer_client(),
          kUniverseId, &tmp_resp).ok() &&
          tmp_resp.entry().producer_master_addresses_size() == master_count;
    }, MonoDelta::FromSeconds(kRpcTimeout), "Verify master count increased."));
    ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), t_count));
  }

  // Stop the old master.
  LOG(INFO) << "Failover to new Master";
  MiniMaster* old_master = producer_cluster()->leader_mini_master();
  producer_cluster()->leader_mini_master()->Shutdown();
  MiniMaster* new_master = producer_cluster()->leader_mini_master();
  ASSERT_NE(nullptr, new_master);
  ASSERT_NE(old_master, new_master);
  ASSERT_OK(producer_cluster()->WaitForAllTabletServers());

  LOG(INFO) << "Add Table after Master Failover";
  // Add a new table to replication and ensure that it can read using the new master config.
  {
    master::AlterUniverseReplicationRequestPB alter_req;
    master::AlterUniverseReplicationResponsePB alter_resp;
    alter_req.set_producer_id(kUniverseId);
    alter_req.add_producer_table_ids_to_add(producer_tables[1]->id());
    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));

    auto master_proxy = std::make_shared<master::MasterServiceProxy>(
        &consumer_client()->proxy_cache(),
        consumer_cluster()->leader_mini_master()->bound_rpc_addr());
    ASSERT_OK(master_proxy->AlterUniverseReplication(alter_req, &alter_resp, &rpc));
    ASSERT_FALSE(alter_resp.has_error());

    // Verify that the consumer now has both tables in the universe.
    ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      master::GetUniverseReplicationResponsePB tmp_resp;
      return VerifyUniverseReplication(consumer_cluster(), consumer_client(),
          kUniverseId, &tmp_resp).ok() &&
          tmp_resp.entry().tables_size() == 2;
    }, MonoDelta::FromSeconds(kRpcTimeout), "Verify table created with alter."));
    ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), t_count * 2));
  }

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, AlterUniverseReplicationTables) {
  // Setup the consumer and producer cluster.
  auto tables = ASSERT_RESULT(SetUpWithParams({3, 3}, {3, 3}, 1));
  std::vector<std::shared_ptr<client::YBTable>> producer_tables{tables[0], tables[2]};
  std::vector<std::shared_ptr<client::YBTable>> consumer_tables{tables[1], tables[3]};

  // Setup universe replication on the first table.
  auto initial_table = { producer_tables[0] };
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, initial_table));

  // Verify that universe was setup on consumer.
  master::GetUniverseReplicationResponsePB v_resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, &v_resp));
  ASSERT_EQ(v_resp.entry().producer_id(), kUniverseId);
  ASSERT_EQ(v_resp.entry().tables_size(), 1);
  ASSERT_EQ(v_resp.entry().tables(0), producer_tables[0]->id());

  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 3));

  // 'add_table'. Add the next table with the alter command.
  {
    master::AlterUniverseReplicationRequestPB alter_req;
    master::AlterUniverseReplicationResponsePB alter_resp;
    alter_req.set_producer_id(kUniverseId);
    alter_req.add_producer_table_ids_to_add(producer_tables[1]->id());
    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));

    auto master_proxy = std::make_shared<master::MasterServiceProxy>(
        &consumer_client()->proxy_cache(),
        consumer_cluster()->leader_mini_master()->bound_rpc_addr());
    ASSERT_OK(master_proxy->AlterUniverseReplication(alter_req, &alter_resp, &rpc));
    ASSERT_FALSE(alter_resp.has_error());

    // Verify that the consumer now has both tables in the universe.
    ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      master::GetUniverseReplicationResponsePB tmp_resp;
      return VerifyUniverseReplication(consumer_cluster(), consumer_client(),
                                          kUniverseId, &tmp_resp).ok() &&
             tmp_resp.entry().tables_size() == 2;
    }, MonoDelta::FromSeconds(kRpcTimeout), "Verify table created with alter."));
    ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 6));
  }

  // Write some rows to the new table on the Producer. Ensure that the Consumer gets it.
  WriteWorkload(6, 10, producer_client(), producer_tables[1]->name());
  ASSERT_OK(VerifyWrittenRecords(producer_tables[1]->name(), consumer_tables[1]->name()));

  // 'remove_table'. Remove the original table, leaving only the new one.
  {
    master::AlterUniverseReplicationRequestPB alter_req;
    master::AlterUniverseReplicationResponsePB alter_resp;
    alter_req.set_producer_id(kUniverseId);
    alter_req.add_producer_table_ids_to_remove(producer_tables[0]->id());
    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));

    auto master_proxy = std::make_shared<master::MasterServiceProxy>(
        &consumer_client()->proxy_cache(),
        consumer_cluster()->leader_mini_master()->bound_rpc_addr());
    ASSERT_OK(master_proxy->AlterUniverseReplication(alter_req, &alter_resp, &rpc));
    ASSERT_FALSE(alter_resp.has_error());

    // Verify that the consumer now has only the new table created by the previous alter.
    ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
      return VerifyUniverseReplication(consumer_cluster(), consumer_client(),
          kUniverseId, &v_resp).ok() &&
          v_resp.entry().tables_size() == 1;
    }, MonoDelta::FromSeconds(kRpcTimeout), "Verify table removed with alter."));
    ASSERT_EQ(v_resp.entry().tables(0), producer_tables[1]->id());
    ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 3));
  }

  LOG(INFO) << "All alter tests passed.  Tearing down...";

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));
  Destroy();
}

TEST_P(TwoDCTest, ToggleReplicationEnabled) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({2}, {2}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // Verify that universe is now ACTIVE
  master::GetUniverseReplicationResponsePB resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, &resp));

  // After we know the universe is ACTIVE, make sure all tablets are getting polled.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));

  // Disable the replication and ensure no tablets are being polled
  ASSERT_OK(ToggleUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, false));
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 0));

  // Enable replication and ensure that all the tablets start being polled again
  ASSERT_OK(ToggleUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, true));
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));

  Destroy();
}

TEST_P(TwoDCTest, TestDeleteUniverse) {
  uint32_t replication_factor = NonTsanVsTsan(3, 1);

  auto tables = ASSERT_RESULT(SetUpWithParams({8, 4}, {6, 6}, replication_factor));

  ASSERT_OK(SetupUniverseReplication(producer_cluster(), consumer_cluster(), consumer_client(),
      kUniverseId, {tables[0], tables[2]} /* all producer tables */));

  // Verify that universe was setup on consumer.
  master::GetUniverseReplicationResponsePB resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, &resp));

  // After creating the cluster, make sure all tablets being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 12));

  ASSERT_OK(DeleteUniverseReplication(kUniverseId));

  ASSERT_OK(VerifyUniverseReplicationDeleted(consumer_cluster(), consumer_client(), kUniverseId,
      FLAGS_cdc_read_rpc_timeout_ms * 2));

  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 0));

  Destroy();
}

TEST_P(TwoDCTest, TestWalRetentionSet) {
  FLAGS_cdc_wal_retention_time_secs = 8 * 3600;

  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({8, 4, 4, 12}, {8, 4, 12, 8}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer tables from the list.
  producer_tables.reserve(tables.size() / 2);
  for (int i = 0; i < tables.size(); i += 2) {
    producer_tables.push_back(tables[i]);
  }
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // Verify that universe was setup on consumer.
  master::GetUniverseReplicationResponsePB resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId, &resp));

  // After creating the cluster, make sure all 32 tablets being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 32));

  cdc::VerifyWalRetentionTime(producer_cluster(), "test_table_", FLAGS_cdc_wal_retention_time_secs);

  YBTableName table_name(YQL_DATABASE_CQL, kNamespaceName, "test_table_0");

  // Issue an ALTER TABLE request on the producer to verify that it doesn't crash.
  auto table_alterer = producer_client()->NewTableAlterer(table_name);
  table_alterer->AddColumn("new_col")->Type(INT32);
  ASSERT_OK(table_alterer->timeout(MonoDelta::FromSeconds(kRpcTimeout))->Alter());

  // Verify that the table got altered on the producer.
  YBSchema schema;
  PartitionSchema partition_schema;
  ASSERT_OK(producer_client()->GetTableSchema(table_name, &schema, &partition_schema));

  ASSERT_NE(static_cast<int>(Schema::kColumnNotFound), schema.FindColumn("new_col"));

  Destroy();
}

TEST_P(TwoDCTest, TestProducerUniverseExpansion) {
  // Test that after new node(s) are added to producer universe, we are able to get replicated data
  // from the new node(s).
  auto tables = ASSERT_RESULT(SetUpWithParams({2}, {2}, 1));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));

  WriteWorkload(0, 5, producer_client(), tables[0]->name());

  // Add new node and wait for tablets to be rebalanced.
  // After rebalancing, each node will be leader for 1 tablet.
  ASSERT_OK(producer_cluster()->AddTabletServer());
  ASSERT_OK(producer_cluster()->WaitForTabletServerCount(2));
  ASSERT_OK(WaitFor([&] () { return producer_client()->IsLoadBalanced(2); },
                    MonoDelta::FromSeconds(kRpcTimeout), "IsLoadBalanced"));

  // Check that all tablets continue to be polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 2));

  // Write some more rows. Note that some of these rows will have the new node as the tablet leader.
  WriteWorkload(6, 10, producer_client(), tables[0]->name());

  // Verify that both clusters have the same records.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  Destroy();
}

TEST_P(TwoDCTest, ApplyOperationsRandomFailures) {
  SetAtomicFlag(0.25, &FLAGS_TEST_respond_write_failed_probability);

  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  // Use unequal table count so we have M:N mapping and output to multiple tablets.
  auto tables = ASSERT_RESULT(SetUpWithParams({3}, {5}, replication_factor));

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer table from the list.
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  // Set up bi-directional replication.
  std::vector<std::shared_ptr<client::YBTable>> consumer_tables;
  consumer_tables.reserve(1);
  consumer_tables.push_back(tables[1]);
  ASSERT_OK(SetupUniverseReplication(
      consumer_cluster(), producer_cluster(), producer_client(), kUniverseId, consumer_tables));

  // After creating the cluster, make sure all producer tablets are being polled for.
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), 5));
  ASSERT_OK(CorrectlyPollingAllTablets(producer_cluster(), 3));

  // Write 1000 entries to each cluster.
  std::thread t1([&]() { WriteWorkload(0, 1000, producer_client(), tables[0]->name()); });
  std::thread t2([&]() { WriteWorkload(1000, 2000, consumer_client(), tables[1]->name()); });

  t1.join();
  t2.join();

  // Verify that both clusters have the same records.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  // Stop replication on consumer.
  ASSERT_OK(DeleteUniverseReplication(kUniverseId));

  // Stop replication on producer
  ASSERT_OK(DeleteUniverseReplication(kUniverseId, producer_client(), producer_cluster()));
  Destroy();
}

TEST_P(TwoDCTest, TestInsertDeleteWorkloadWithRestart) {
  // Good test for batching, make sure we can handle operations on the same key with different
  // hybrid times. Then, do a restart and make sure we can successfully bootstrap the batched data.
  // In additional, make sure we write exactly num_total_ops / batch_size batches to the cluster to
  // ensure batching is actually enabled.
  constexpr uint32_t num_ops_per_workload = 100;
  constexpr uint32_t num_runs = 5;

  uint32_t replication_factor = NonTsanVsTsan(3, 1);
  auto tables = ASSERT_RESULT(SetUpWithParams({1}, {1}, replication_factor));

  WriteWorkload(0, num_ops_per_workload, producer_client(), tables[0]->name());
  for (int i = 0; i < num_runs; i++) {
    WriteWorkload(0, num_ops_per_workload, producer_client(), tables[0]->name(), true);
    WriteWorkload(0, num_ops_per_workload, producer_client(), tables[0]->name());
  }

  // Count the number of ops in total, expect 1 batch if the batch flag is set to 0.
  uint32_t expected_num_writes = FLAGS_cdc_max_apply_batch_num_records > 0 ?
      (num_ops_per_workload * (num_runs * 2 + 1)) / FLAGS_cdc_max_apply_batch_num_records : 1;

  LOG(INFO) << "expected num writes: " <<expected_num_writes;

  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  producer_tables.reserve(1);
  producer_tables.push_back(tables[0]);
  ASSERT_OK(SetupUniverseReplication(
      producer_cluster(), consumer_cluster(), consumer_client(), kUniverseId, producer_tables));

  AssertLoggedWaitFor([&]() {
    return GetSuccessfulWriteOps(consumer_cluster()) == expected_num_writes;
  }, MonoDelta::FromSeconds(60), "Wait for all batches to finish.");

  // Verify that both clusters have the same records.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));

  ASSERT_OK(consumer_cluster()->RestartSync());

  // Verify that both clusters have the same records.
  ASSERT_OK(VerifyWrittenRecords(tables[0]->name(), tables[1]->name()));
  // Stop replication on consumer.
  ASSERT_OK(DeleteUniverseReplication(kUniverseId));

  Destroy();
}

} // namespace enterprise
} // namespace yb
