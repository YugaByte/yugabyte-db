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

#include "yb/common/common.pb.h"
#include "yb/common/entity_ids.h"
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
#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/mini_master.h"
#include "yb/master/master.h"
#include "yb/master/master.pb.h"
#include "yb/master/master-test-util.h"

#include "yb/master/cdc_consumer_registry_service.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/tserver/cdc_consumer.h"
#include "yb/util/atomic.h"
#include "yb/util/faststring.h"
#include "yb/util/format.h"
#include "yb/util/monotime.h"
#include "yb/util/random.h"
#include "yb/util/random_util.h"
#include "yb/util/result.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"
#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"

DECLARE_int32(replication_factor);
DECLARE_int32(cdc_max_apply_batch_num_records);
DECLARE_int32(client_read_write_timeout_ms);
DECLARE_int32(pgsql_proxy_webserver_port);
DECLARE_bool(master_auto_run_initdb);
DECLARE_bool(hide_pg_catalog_table_creation_logs);
DECLARE_int32(pggate_rpc_timeout_secs);

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
using master::GetNamespaceInfoResponsePB;
using master::MiniMaster;
using tserver::MiniTabletServer;
using tserver::enterprise::CDCConsumer;

using pgwrapper::AsString;
using pgwrapper::GetInt32;
using pgwrapper::PGConn;
using pgwrapper::PGResultPtr;
using pgwrapper::PgSupervisor;

namespace enterprise {

constexpr static const char* const kKeyColumnName = "key";

class TwoDCYsqlTest : public TwoDCTestBase, public testing::WithParamInterface<TwoDCTestParams> {
 public:
  Result<std::vector<std::shared_ptr<client::YBTable>>>
      SetUpWithParams(std::vector<uint32_t> num_consumer_tablets,
                      std::vector<uint32_t> num_producer_tablets,
                      uint32_t replication_factor,
                      uint32_t num_masters = 1,
                      bool colocated = false) {
    master::SetDefaultInitialSysCatalogSnapshotFlags();
    TwoDCTestBase::SetUp();
    FLAGS_enable_ysql = true;
    FLAGS_master_auto_run_initdb = true;
    FLAGS_hide_pg_catalog_table_creation_logs = true;
    FLAGS_pggate_rpc_timeout_secs = 120;
    FLAGS_cdc_max_apply_batch_num_records = GetParam().batch_size;
    FLAGS_cdc_enable_replicate_intents = GetParam().enable_replicate_intents;

    MiniClusterOptions opts;
    opts.num_tablet_servers = replication_factor;
    opts.num_masters = num_masters;
    FLAGS_replication_factor = replication_factor;
    opts.cluster_id = "producer";
    producer_cluster_.mini_cluster_ = std::make_unique<MiniCluster>(env_.get(), opts);
    RETURN_NOT_OK(producer_cluster()->StartSync());
    RETURN_NOT_OK(producer_cluster()->WaitForTabletServerCount(replication_factor));
    RETURN_NOT_OK(WaitForInitDb(producer_cluster()));

    opts.cluster_id = "consumer";
    consumer_cluster_.mini_cluster_ = std::make_unique<MiniCluster>(env_.get(), opts);
    RETURN_NOT_OK(consumer_cluster()->StartSync());
    RETURN_NOT_OK(consumer_cluster()->WaitForTabletServerCount(replication_factor));
    RETURN_NOT_OK(WaitForInitDb(consumer_cluster()));

    producer_cluster_.client_ = VERIFY_RESULT(producer_cluster()->CreateClient());
    consumer_cluster_.client_ = VERIFY_RESULT(consumer_cluster()->CreateClient());

    RETURN_NOT_OK(InitPostgres(&producer_cluster_));
    RETURN_NOT_OK(InitPostgres(&consumer_cluster_));

    if (num_consumer_tablets.size() != num_producer_tablets.size()) {
      return STATUS(IllegalState,
                    Format("Num consumer tables: $0 num producer tables: $1 must be equal.",
                           num_consumer_tablets.size(), num_producer_tablets.size()));
    }

    RETURN_NOT_OK(CreateDatabase(&producer_cluster_, kNamespaceName, colocated));
    RETURN_NOT_OK(CreateDatabase(&consumer_cluster_, kNamespaceName, colocated));

    std::vector<YBTableName> tables;
    std::vector<std::shared_ptr<client::YBTable>> yb_tables;
    for (int i = 0; i < num_consumer_tablets.size(); i++) {
      RETURN_NOT_OK(CreateTable(i, num_producer_tablets[i], &producer_cluster_,
                                &tables, colocated));
      std::shared_ptr<client::YBTable> producer_table;
      RETURN_NOT_OK(producer_client()->OpenTable(tables[i * 2], &producer_table));
      yb_tables.push_back(producer_table);

      RETURN_NOT_OK(CreateTable(i, num_consumer_tablets[i], &consumer_cluster_,
                                &tables, colocated));
      std::shared_ptr<client::YBTable> consumer_table;
      RETURN_NOT_OK(consumer_client()->OpenTable(tables[(i * 2) + 1], &consumer_table));
      yb_tables.push_back(consumer_table);
    }

    return yb_tables;
  }

  Status InitPostgres(Cluster* cluster) {
    auto pg_ts = RandomElement(cluster->mini_cluster_->mini_tablet_servers());
    auto port = cluster->mini_cluster_->AllocateFreePort();
    yb::pgwrapper::PgProcessConf pg_process_conf =
        VERIFY_RESULT(yb::pgwrapper::PgProcessConf::CreateValidateAndRunInitDb(
            yb::ToString(Endpoint(pg_ts->bound_rpc_addr().address(), port)),
            pg_ts->options()->fs_opts.data_paths.front() + "/pg_data",
            pg_ts->server()->GetSharedMemoryFd()));
    pg_process_conf.master_addresses = pg_ts->options()->master_addresses_flag;
    pg_process_conf.force_disable_log_file = true;
    FLAGS_pgsql_proxy_webserver_port = cluster->mini_cluster_->AllocateFreePort();

    LOG(INFO) << "Starting PostgreSQL server listening on "
              << pg_process_conf.listen_addresses << ":" << pg_process_conf.pg_port << ", data: "
              << pg_process_conf.data_dir
              << ", pgsql webserver port: " << FLAGS_pgsql_proxy_webserver_port;
    cluster->pg_supervisor_ = std::make_unique<yb::pgwrapper::PgSupervisor>(pg_process_conf);
    RETURN_NOT_OK(cluster->pg_supervisor_->Start());

    cluster->pg_host_port_ = HostPort(pg_process_conf.listen_addresses, pg_process_conf.pg_port);
    return Status::OK();
  }

  Status CreateDatabase(Cluster* cluster,
                        const std::string& namespace_name = kNamespaceName,
                        bool colocated = false) {
    auto conn = EXPECT_RESULT(cluster->Connect());
    EXPECT_OK(conn.ExecuteFormat("CREATE DATABASE $0$1",
                                 namespace_name, colocated ? " colocated = true" : ""));
    return Status::OK();
  }

  Result<string> GetUniverseId(Cluster* cluster) {
    master::GetMasterClusterConfigRequestPB req;
    master::GetMasterClusterConfigResponsePB resp;

    auto master_proxy = std::make_shared<master::MasterServiceProxy>(
        &cluster->client_->proxy_cache(),
        cluster->mini_cluster_->leader_mini_master()->bound_rpc_addr());

    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
    RETURN_NOT_OK(master_proxy->GetMasterClusterConfig(req, &resp, &rpc));
    if (resp.has_error()) {
      return STATUS(IllegalState, "Error getting cluster config");
    }
    return resp.cluster_config().cluster_uuid();
  }

  Result<YBTableName> CreateTable(Cluster* cluster,
                                  const std::string& namespace_name,
                                  const std::string& table_name,
                                  uint32_t num_tablets,
                                  bool colocated = false,
                                  const int table_oid = 0) {
    auto conn = EXPECT_RESULT(cluster->ConnectToDB(namespace_name));
    std::string table_oid_string = "";
    if (table_oid > 0) {
      // Need to turn on session flag to allow for CREATE WITH table_oid.
      EXPECT_OK(conn.Execute("set yb_enable_create_with_table_oid=true"));
      table_oid_string = Format("table_oid = $0,", table_oid);
    }
    EXPECT_OK(conn.ExecuteFormat(
        "CREATE TABLE $0($1 int PRIMARY KEY) WITH ($2colocated = $3) SPLIT INTO $4 TABLETS",
        table_name, kKeyColumnName, table_oid_string, colocated, num_tablets));
    return GetTable(cluster, namespace_name, table_name);
  }

  Status CreateTable(uint32_t idx, uint32_t num_tablets, Cluster* cluster,
                     std::vector<YBTableName>* tables, bool colocated = false) {
    // Generate table_oid based on index so that we have the same table_oid for producer/consumer.
    const int table_oid = colocated ? (idx + 1) * 111111 : 0;
    auto table = VERIFY_RESULT(CreateTable(cluster, kNamespaceName, Format("test_table_$0", idx),
                                           num_tablets, colocated, table_oid));
    tables->push_back(table);
    return Status::OK();
  }

  Result<YBTableName> GetTable(Cluster* cluster,
                               const std::string& namespace_name,
                               const std::string& table_name,
                               bool verify_table_name = true,
                               bool exclude_system_tables = true) {
    master::ListTablesRequestPB req;
    master::ListTablesResponsePB resp;

    req.set_name_filter(table_name);
    req.mutable_namespace_()->set_name(namespace_name);
    req.mutable_namespace_()->set_database_type(YQL_DATABASE_PGSQL);
    if (!exclude_system_tables) {
      req.set_exclude_system_tables(true);
      req.add_relation_type_filter(master::USER_TABLE_RELATION);
    }

    auto master_proxy = std::make_shared<master::MasterServiceProxy>(
        &cluster->client_->proxy_cache(),
        cluster->mini_cluster_->leader_mini_master()->bound_rpc_addr());

    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
    RETURN_NOT_OK(master_proxy->ListTables(req, &resp, &rpc));
    if (resp.has_error()) {
      return STATUS(IllegalState, "Failed listing tables");
    }

    // Now need to find the table and return it.
    for (const auto& table : resp.tables()) {
      // If !verify_table_name, just return the first table.
      if (!verify_table_name ||
          (table.name() == table_name && table.namespace_().name() == namespace_name)) {
        YBTableName yb_table;
        yb_table.set_table_id(table.id());
        yb_table.set_namespace_id(table.namespace_().id());
        return yb_table;
      }
    }
    return STATUS(IllegalState,
                  strings::Substitute("Unable to find table $0 in namespace $1",
                                      table_name, namespace_name));
  }

  void WriteWorkload(uint32_t start, uint32_t end, Cluster* cluster, const YBTableName& table,
                     bool delete_op = false) {
    auto conn = EXPECT_RESULT(cluster->ConnectToDB(table.namespace_name()));

    LOG(INFO) << "Writing " << end-start << (delete_op ? " deletes" : " inserts");
    for (uint32_t i = start; i < end; i++) {
      if (delete_op) {
        EXPECT_OK(conn.ExecuteFormat("DELETE FROM $0 WHERE $1 = $2",
                                     table.table_name(), kKeyColumnName, i));
      } else {
        // TODO(#6582) transactions currently don't work, so don't use ON CONFLICT DO NOTHING now.
        EXPECT_OK(conn.ExecuteFormat("INSERT INTO $0($1) VALUES ($2)", // ON CONFLICT DO NOTHING",
                                     table.table_name(), kKeyColumnName, i));
      }
    }
  }

  void WriteTransactionalWorkload(uint32_t start, uint32_t end, Cluster* cluster,
                                  const YBTableName& table) {
    auto conn = EXPECT_RESULT(cluster->ConnectToDB(table.namespace_name()));
    EXPECT_OK(conn.Execute("BEGIN"));
    for (uint32_t i = start; i < end; i++) {
      EXPECT_OK(conn.ExecuteFormat("INSERT INTO $0($1) VALUES ($2) ON CONFLICT DO NOTHING",
                                   table.table_name(), kKeyColumnName, i));
    }
    EXPECT_OK(conn.Execute("COMMIT"));
  }

  void DeleteWorkload(uint32_t start, uint32_t end, Cluster* cluster, const YBTableName& table) {
    WriteWorkload(start, end, cluster, table, true /* delete_op */);
  }

  PGResultPtr ScanToStrings(const YBTableName& table_name, Cluster* cluster) {
    auto conn = EXPECT_RESULT(cluster->ConnectToDB(table_name.namespace_name()));
    auto result =
        EXPECT_RESULT(conn.FetchFormat("SELECT * FROM $0 ORDER BY key", table_name.table_name()));
    return result;
  }

  Status VerifyWrittenRecords(const YBTableName& producer_table,
                              const YBTableName& consumer_table) {
    return LoggedWaitFor([=]() -> Result<bool> {
      auto producer_results = ScanToStrings(producer_table, &producer_cluster_);
      auto consumer_results = ScanToStrings(consumer_table, &consumer_cluster_);
      if (PQntuples(producer_results.get()) != PQntuples(consumer_results.get())) {
        return false;
      }
      for (int i = 0; i < PQntuples(producer_results.get()); ++i) {
        auto prod_val = EXPECT_RESULT(AsString(producer_results.get(), i, 0));
        auto cons_val = EXPECT_RESULT(AsString(consumer_results.get(), i, 0));
        if (prod_val != cons_val) {
          return false;
        }
      }
      return true;
    }, MonoDelta::FromSeconds(kRpcTimeout), "Verify written records");
  }

  Status VerifyNumRecords(const YBTableName& table, Cluster* cluster, int expected_size) {
    return LoggedWaitFor([=]() -> Result<bool> {
      auto results = ScanToStrings(table, cluster);
      return PQntuples(results.get()) == expected_size;
    }, MonoDelta::FromSeconds(kRpcTimeout), "Verify number of records");
  }
};

INSTANTIATE_TEST_CASE_P(TwoDCTestParams, TwoDCYsqlTest,
                        ::testing::Values(TwoDCTestParams(1, true), TwoDCTestParams(1, false),
                                          TwoDCTestParams(0, true), TwoDCTestParams(0, false)));

TEST_P(TwoDCYsqlTest, YB_DISABLE_TEST_IN_TSAN(SetupUniverseReplication)) {
  auto tables = ASSERT_RESULT(SetUpWithParams({8, 4}, {6, 6}, 3, 1, false /* colocated */));
  const string kUniverseId = ASSERT_RESULT(GetUniverseId(&producer_cluster_));

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

TEST_P(TwoDCYsqlTest, YB_DISABLE_TEST_IN_TSAN(SimpleReplication)) {
  constexpr int kNTabletsPerTable = 1;
  std::vector<uint32_t> tables_vector = {kNTabletsPerTable, kNTabletsPerTable};
  auto tables = ASSERT_RESULT(SetUpWithParams(tables_vector, tables_vector, 1));
  const string kUniverseId = ASSERT_RESULT(GetUniverseId(&producer_cluster_));

  // tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer tables from the list.
  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  std::vector<std::shared_ptr<client::YBTable>> consumer_tables;
  producer_tables.reserve(tables.size() / 2);
  consumer_tables.reserve(tables.size() / 2);
  for (int i = 0; i < tables.size(); ++i) {
    if (i % 2 == 0) {
      producer_tables.push_back(tables[i]);
    } else {
      consumer_tables.push_back(tables[i]);
    }
  }

  // 1. Write some data.
  for (const auto& producer_table : producer_tables) {
    LOG(INFO) << "Writing records for table " << producer_table->name().ToString();
    WriteWorkload(0, 100, &producer_cluster_, producer_table->name());
  }

  // Verify data is written on the producer.
  for (const auto& producer_table : producer_tables) {
    auto producer_results = ScanToStrings(producer_table->name(), &producer_cluster_);
    ASSERT_EQ(100, PQntuples(producer_results.get()));
    int result;
    for (int i = 0; i < 100; ++i) {
      result = ASSERT_RESULT(GetInt32(producer_results.get(), i, 0));
      ASSERT_EQ(i, result);
    }
  }

  // 2. Setup replication.
  ASSERT_OK(SetupUniverseReplication(producer_cluster(), consumer_cluster(), consumer_client(),
                                     kUniverseId, producer_tables));

  // 3. Verify everything is setup correctly.
  master::GetUniverseReplicationResponsePB get_universe_replication_resp;
  ASSERT_OK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId,
      &get_universe_replication_resp));
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(),
                                       tables_vector.size() * kNTabletsPerTable));

  auto data_replicated_correctly = [&](int num_results) -> Result<bool> {
    for (const auto& consumer_table : consumer_tables) {
      LOG(INFO) << "Checking records for table " << consumer_table->name().ToString();
      auto consumer_results = ScanToStrings(consumer_table->name(), &consumer_cluster_);

      if (num_results != PQntuples(consumer_results.get())) {
        return false;
      }
      int result;
      for (int i = 0; i < num_results; ++i) {
        result = VERIFY_RESULT(GetInt32(consumer_results.get(), i, 0));
        if (i != result) {
          return false;
        }
      }
    }
    return true;
  };
  ASSERT_OK(WaitFor([&]() -> Result<bool> { return data_replicated_correctly(100); },
                    MonoDelta::FromSeconds(20), "IsDataReplicatedCorrectly"));

  // 4. Write more data.
  for (const auto& producer_table : producer_tables) {
    WriteWorkload(100, 105, &producer_cluster_, producer_table->name());
  }

  // 5. Make sure this data is also replicated now.
  ASSERT_OK(WaitFor([&]() { return data_replicated_correctly(105); },
                    MonoDelta::FromSeconds(20), "IsDataReplicatedCorrectly"));
  Destroy();
}

TEST_P(TwoDCYsqlTest, YB_DISABLE_TEST_IN_TSAN(SetupUniverseReplicationWithProducerBootstrapId)) {
  constexpr int kNTabletsPerTable = 1;
  std::vector<uint32_t> tables_vector = {kNTabletsPerTable, kNTabletsPerTable};
  auto tables = ASSERT_RESULT(SetUpWithParams(tables_vector, tables_vector, 3));
  const string kUniverseId = ASSERT_RESULT(GetUniverseId(&producer_cluster_));
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
  for (int i = 0; i < tables.size(); ++i) {
    if (i % 2 == 0) {
      producer_tables.push_back(tables[i]);
    } else {
      consumer_tables.push_back(tables[i]);
    }
  }

  // 1. Write some data so that we can verify that only new records get replicated.
  for (const auto& producer_table : producer_tables) {
    LOG(INFO) << "Writing records for table " << producer_table->name().ToString();
    WriteWorkload(0, 100, &producer_cluster_, producer_table->name());
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
    WriteWorkload(1000, 1005, &producer_cluster_, producer_table->name());
  }

  // 5. Verify that only new writes get replicated to consumer since we bootstrapped the producer
  // after we had already written some data, therefore the old data (whatever was there before we
  // bootstrapped the producer) should not be replicated.
  auto data_replicated_correctly = [&]() -> Result<bool> {
    for (const auto& consumer_table : consumer_tables) {
      LOG(INFO) << "Checking records for table " << consumer_table->name().ToString();
      auto consumer_results = ScanToStrings(consumer_table->name(), &consumer_cluster_);

      if (5 != PQntuples(consumer_results.get())) {
        return false;
      }
      int result;
      for (int i = 0; i < 5; ++i) {
        result = VERIFY_RESULT(GetInt32(consumer_results.get(), i, 0));
        if ((1000 + i) != result) {
          return false;
        }
      }
    }
    return true;
  };

  ASSERT_OK(WaitFor([&]() -> Result<bool> { return data_replicated_correctly(); },
                    MonoDelta::FromSeconds(20), "IsDataReplicatedCorrectly"));
  Destroy();
}

TEST_P(TwoDCYsqlTest, YB_DISABLE_TEST_IN_TSAN(ColocatedDatabaseReplication)) {
  constexpr int kNTabletsPerColocatedTable = 1;
  constexpr int kNTabletsPerTable = 3;
  std::vector<uint32_t> tables_vector = {kNTabletsPerColocatedTable, kNTabletsPerColocatedTable};
  // Create two colocated tables on each cluster.
  auto colocated_tables =
      ASSERT_RESULT(SetUpWithParams(tables_vector, tables_vector, 3, 1, true /* colocated */));
  const string kUniverseId = ASSERT_RESULT(GetUniverseId(&producer_cluster_));

  // Also create an additional non-colocated table in each database.
  auto non_colocated_table = ASSERT_RESULT(CreateTable(&producer_cluster_,
                                                       kNamespaceName,
                                                       "test_table_2",
                                                       kNTabletsPerTable,
                                                       false /* colocated */));
  std::shared_ptr<client::YBTable> non_colocated_producer_table;
  ASSERT_OK(producer_client()->OpenTable(non_colocated_table, &non_colocated_producer_table));
  non_colocated_table = ASSERT_RESULT(CreateTable(&consumer_cluster_,
                                                  kNamespaceName,
                                                  "test_table_2",
                                                  kNTabletsPerTable,
                                                  false /* colocated */));
  std::shared_ptr<client::YBTable> non_colocated_consumer_table;
  ASSERT_OK(consumer_client()->OpenTable(non_colocated_table, &non_colocated_consumer_table));

  // colocated_tables contains both producer and consumer universe tables (alternately).
  // Pick out just the producer tables from the list.
  std::vector<std::shared_ptr<client::YBTable>> producer_tables;
  std::vector<std::shared_ptr<client::YBTable>> consumer_tables;
  std::vector<std::shared_ptr<client::YBTable>> colocated_producer_tables;
  std::vector<std::shared_ptr<client::YBTable>> colocated_consumer_tables;
  producer_tables.reserve(colocated_tables.size() / 2 + 1);
  consumer_tables.reserve(colocated_tables.size() / 2 + 1);
  colocated_producer_tables.reserve(colocated_tables.size() / 2);
  colocated_consumer_tables.reserve(colocated_tables.size() / 2);
  for (int i = 0; i < colocated_tables.size(); ++i) {
    if (i % 2 == 0) {
      producer_tables.push_back(colocated_tables[i]);
      colocated_producer_tables.push_back(colocated_tables[i]);
    } else {
      consumer_tables.push_back(colocated_tables[i]);
      colocated_consumer_tables.push_back(colocated_tables[i]);
    }
  }
  producer_tables.push_back(non_colocated_producer_table);
  consumer_tables.push_back(non_colocated_consumer_table);

  // 1. Write some data to all tables.
  for (const auto& producer_table : producer_tables) {
    LOG(INFO) << "Writing records for table " << producer_table->name().ToString();
    WriteWorkload(0, 100, &producer_cluster_, producer_table->name());
  }

  // 2. Setup replication for only the colocated tables.
  // Get the producer namespace id, so we can construct the colocated parent table id.
  GetNamespaceInfoResponsePB ns_resp;
  ASSERT_OK(producer_client()->GetNamespaceInfo("", kNamespaceName, YQL_DATABASE_PGSQL, &ns_resp));

  rpc::RpcController rpc;
  master::SetupUniverseReplicationRequestPB setup_universe_req;
  master::SetupUniverseReplicationResponsePB setup_universe_resp;
  setup_universe_req.set_producer_id(kUniverseId);
  string master_addr = producer_cluster()->GetMasterAddresses();
  auto hp_vec = ASSERT_RESULT(HostPort::ParseStrings(master_addr, 0));
  HostPortsToPBs(hp_vec, setup_universe_req.mutable_producer_master_addresses());
  // Only need to add the colocated parent table id.
  setup_universe_req.mutable_producer_table_ids()->Reserve(1);
  setup_universe_req.add_producer_table_ids(
      ns_resp.namespace_().id() + master::kColocatedParentTableIdSuffix);
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
  ASSERT_OK(CorrectlyPollingAllTablets(consumer_cluster(), kNTabletsPerColocatedTable));

  // 4. Check that colocated tables are being replicated.
  auto data_replicated_correctly = [&](int num_results, bool onlyColocated) -> Result<bool> {
    auto &tables = onlyColocated ? colocated_consumer_tables : consumer_tables;
    for (const auto& consumer_table : tables) {
      LOG(INFO) << "Checking records for table " << consumer_table->name().ToString();
      auto consumer_results = ScanToStrings(consumer_table->name(), &consumer_cluster_);

      if (num_results != PQntuples(consumer_results.get())) {
        return false;
      }
      int result;
      for (int i = 0; i < num_results; ++i) {
        result = VERIFY_RESULT(GetInt32(consumer_results.get(), i, 0));
        if (i != result) {
          return false;
        }
      }
    }
    return true;
  };
  ASSERT_OK(WaitFor([&]() -> Result<bool> { return data_replicated_correctly(100, true); },
                    MonoDelta::FromSeconds(20), "IsDataReplicatedCorrectly"));
  // Ensure that the non colocated table is not replicated.
  auto non_coloc_results = ScanToStrings(non_colocated_consumer_table->name(), &consumer_cluster_);
  ASSERT_EQ(0, PQntuples(non_coloc_results.get()));

  // 5. Add the regular table to replication.
  // Prepare and send AlterUniverseReplication command.
  master::AlterUniverseReplicationRequestPB alter_req;
  master::AlterUniverseReplicationResponsePB alter_resp;
  alter_req.set_producer_id(kUniverseId);
  alter_req.add_producer_table_ids_to_add(non_colocated_producer_table->id());

  rpc.Reset();
  rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
  ASSERT_OK(master_proxy->AlterUniverseReplication(alter_req, &alter_resp, &rpc));
  ASSERT_FALSE(alter_resp.has_error());
  // Wait until we have 2 tables (colocated tablet + regular table) logged.
  ASSERT_OK(LoggedWaitFor([&]() -> Result<bool> {
    master::GetUniverseReplicationResponsePB tmp_resp;
    return VerifyUniverseReplication(consumer_cluster(), consumer_client(),
        kUniverseId, &tmp_resp).ok() &&
        tmp_resp.entry().tables_size() == 2;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Verify table created with alter."));

  ASSERT_OK(CorrectlyPollingAllTablets(
      consumer_cluster(), kNTabletsPerColocatedTable + kNTabletsPerTable));
  // Check that all data is replicated for the new table as well.
  ASSERT_OK(WaitFor([&]() -> Result<bool> { return data_replicated_correctly(100, false); },
                    MonoDelta::FromSeconds(20), "IsDataReplicatedCorrectly"));

  // 6. Add additional data to all tables
  for (const auto& producer_table : producer_tables) {
    LOG(INFO) << "Writing records for table " << producer_table->name().ToString();
    WriteWorkload(100, 150, &producer_cluster_, producer_table->name());
  }

  // 7. Verify all tables are properly replicated.
  ASSERT_OK(WaitFor([&]() -> Result<bool> { return data_replicated_correctly(150, false); },
                    MonoDelta::FromSeconds(20), "IsDataReplicatedCorrectly"));
  Destroy();
}

TEST_P(TwoDCYsqlTest, YB_DISABLE_TEST_IN_TSAN(ColocatedDatabaseDifferentTableOids)) {
  auto colocated_tables = ASSERT_RESULT(SetUpWithParams({}, {}, 3, 1, true /* colocated */));
  const string kUniverseId = ASSERT_RESULT(GetUniverseId(&producer_cluster_));

  // Create two tables with different table oids.
  auto conn = ASSERT_RESULT(producer_cluster_.ConnectToDB(kNamespaceName));
  auto table_info = ASSERT_RESULT(CreateTable(&producer_cluster_,
                                              kNamespaceName,
                                              "test_table_0",
                                              1 /* num_tablets */,
                                              true /* colocated */,
                                              123456 /* table_oid */));
  ASSERT_RESULT(CreateTable(&consumer_cluster_,
                            kNamespaceName,
                            "test_table_0",
                            1 /* num_tablets */,
                            true /* colocated */,
                            123457 /* table_oid */));
  std::shared_ptr<client::YBTable> producer_table;
  ASSERT_OK(producer_client()->OpenTable(table_info, &producer_table));

  // Try to setup replication, should fail on schema validation due to different table oids.
  ASSERT_OK(SetupUniverseReplication(producer_cluster(), consumer_cluster(), consumer_client(),
                                     kUniverseId, {producer_table}));
  master::GetUniverseReplicationResponsePB get_universe_replication_resp;
  ASSERT_NOK(VerifyUniverseReplication(consumer_cluster(), consumer_client(), kUniverseId,
      &get_universe_replication_resp));
  Destroy();
}

// TODO adapt rest of twodc-test.cc tests.

} // namespace enterprise
} // namespace yb
