// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
#include <functional>
#include <thread>
#include <set>
#include <vector>

#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include <glog/stl_logging.h>

#include "yb/client/callbacks.h"
#include "yb/client/async_initializer.h"
#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/client-test-util.h"
#include "yb/client/client_utils.h"
#include "yb/client/error.h"
#include "yb/client/meta_cache.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"
#include "yb/client/tablet_server.h"
#include "yb/client/value.h"
#include "yb/client/yb_op.h"

#include "yb/common/partial_row.h"
#include "yb/common/ql_value.h"
#include "yb/common/wire_protocol.h"

#include "yb/consensus/consensus.proxy.h"
#include "yb/gutil/atomicops.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master-test-util.h"
#include "yb/master/master.proxy.h"
#include "yb/master/mini_master.h"
#include "yb/master/ts_descriptor.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_test_util.h"
#include "yb/server/metadata.h"
#include "yb/server/hybrid_clock.h"
#include "yb/yql/cql/ql/util/statement_result.h"
#include "yb/yql/pggate/pg_tabledesc.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/capabilities.h"
#include "yb/util/metrics.h"
#include "yb/util/net/dns_resolver.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/random_util.h"
#include "yb/util/status.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"
#include "yb/util/thread.h"
#include "yb/util/tostring.h"

DECLARE_bool(enable_data_block_fsync);
DECLARE_bool(log_inject_latency);
DECLARE_double(leader_failure_max_missed_heartbeat_periods);
DECLARE_int32(heartbeat_interval_ms);
DECLARE_int32(log_inject_latency_ms_mean);
DECLARE_int32(log_inject_latency_ms_stddev);
DECLARE_int32(master_inject_latency_on_tablet_lookups_ms);
DECLARE_int32(max_create_tablets_per_ts);
DECLARE_int32(TEST_scanner_inject_latency_on_each_batch_ms);
DECLARE_int32(scanner_max_batch_size_bytes);
DECLARE_int32(scanner_ttl_ms);
DECLARE_int32(tablet_server_svc_queue_length);
DECLARE_int32(replication_factor);

DEFINE_int32(test_scan_num_rows, 1000, "Number of rows to insert and scan");
DECLARE_int32(min_backoff_ms_exponent);
DECLARE_int32(max_backoff_ms_exponent);
DECLARE_bool(TEST_force_master_lookup_all_tablets);
DECLARE_double(TEST_simulate_lookup_timeout_probability);

METRIC_DECLARE_counter(rpcs_queue_overflow);

DEFINE_CAPABILITY(ClientTest, 0x1523c5ae);
DECLARE_CAPABILITY(TabletReportLimit);

using namespace std::literals; // NOLINT
using namespace std::placeholders;

namespace yb {
namespace client {

using std::string;
using std::set;
using std::vector;

using base::subtle::Atomic32;
using base::subtle::NoBarrier_AtomicIncrement;
using base::subtle::NoBarrier_Load;
using base::subtle::NoBarrier_Store;
using master::CatalogManager;
using master::GetNamespaceInfoResponsePB;
using master::GetTableLocationsRequestPB;
using master::GetTableLocationsResponsePB;
using master::TabletLocationsPB;
using master::TSInfoPB;
using std::shared_ptr;
using tablet::TabletPeer;
using tserver::MiniTabletServer;

constexpr int32_t kNoBound = kint32max;
constexpr int kNumTablets = 2;

class ClientTest: public YBMiniClusterTestBase<MiniCluster> {
 public:
  ClientTest() {
    YBSchemaBuilder b;
    b.AddColumn("key")->Type(INT32)->NotNull()->HashPrimaryKey();
    b.AddColumn("int_val")->Type(INT32)->NotNull();
    b.AddColumn("string_val")->Type(STRING)->Nullable();
    b.AddColumn("non_null_with_default")->Type(INT32)->NotNull();
    CHECK_OK(b.Build(&schema_));

    FLAGS_enable_data_block_fsync = false; // Keep unit tests fast.
  }

  void SetUp() override {
    YBMiniClusterTestBase::SetUp();

    // Reduce the TS<->Master heartbeat interval
    FLAGS_heartbeat_interval_ms = 10;

    // Start minicluster and wait for tablet servers to connect to master.
    auto opts = MiniClusterOptions();
    opts.num_tablet_servers = 3;
    cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(cluster_->Start());

    // Connect to the cluster.
    client_ = ASSERT_RESULT(YBClientBuilder()
        .add_master_server_addr(yb::ToString(cluster_->mini_master()->bound_rpc_addr()))
        .Build());

    // Create a keyspace;
    ASSERT_OK(client_->CreateNamespace(kKeyspaceName));

    ASSERT_NO_FATALS(CreateTable(kTableName, kNumTablets, &client_table_));
    ASSERT_NO_FATALS(CreateTable(kTable2Name, 1, &client_table2_));
  }

  void DoTearDown() override {
    client_.reset();
    if (cluster_) {
      cluster_->Shutdown();
      cluster_.reset();
    }
    YBMiniClusterTestBase::DoTearDown();
  }

 protected:
  static const string kKeyspaceName;
  static const YBTableName kTableName;
  static const YBTableName kTable2Name;
  static const YBTableName kTable3Name;

  string GetFirstTabletId(YBTable* table) {
    GetTableLocationsRequestPB req;
    GetTableLocationsResponsePB resp;
    table->name().SetIntoTableIdentifierPB(req.mutable_table());
    CHECK_OK(cluster_->mini_master()->master()->catalog_manager()->GetTableLocations(
        &req, &resp));
    CHECK_GT(resp.tablet_locations_size(), 0);
    return resp.tablet_locations(0).tablet_id();
  }

  void CheckNoRpcOverflow() {
    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      MiniTabletServer* server = cluster_->mini_tablet_server(i);
      if (server->is_started()) {
        ASSERT_EQ(0, server->server()->rpc_server()->
            service_pool("yb.tserver.TabletServerService")->
            RpcsQueueOverflowMetric()->value());
      }
    }
  }

  YBSessionPtr CreateSession(YBClient* client = nullptr) {
    if (client == nullptr) {
      client = client_.get();
    }
    std::shared_ptr<YBSession> session = client->NewSession();
    session->SetTimeout(10s);
    return session;
  }

  // Inserts 'num_rows' test rows using 'client'
  void InsertTestRows(YBClient* client, const TableHandle& table, int num_rows, int first_row = 0) {
    auto session = CreateSession(client);
    for (int i = first_row; i < num_rows + first_row; i++) {
      ASSERT_OK(session->Apply(BuildTestRow(table, i)));
    }
    FlushSessionOrDie(session);
    ASSERT_NO_FATALS(CheckNoRpcOverflow());
  }

  // Inserts 'num_rows' using the default client.
  void InsertTestRows(const TableHandle& table, int num_rows, int first_row = 0) {
    InsertTestRows(client_.get(), table, num_rows, first_row);
  }

  void UpdateTestRows(const TableHandle& table, int lo, int hi) {
    auto session = CreateSession();
    for (int i = lo; i < hi; i++) {
      ASSERT_OK(session->Apply(UpdateTestRow(table, i)));
    }
    FlushSessionOrDie(session);
    ASSERT_NO_FATALS(CheckNoRpcOverflow());
  }

  void DeleteTestRows(const TableHandle& table, int lo, int hi) {
    auto session = CreateSession();
    for (int i = lo; i < hi; i++) {
      ASSERT_OK(session->Apply(DeleteTestRow(table, i)));
    }
    FlushSessionOrDie(session);
    ASSERT_NO_FATALS(CheckNoRpcOverflow());
  }

  shared_ptr<YBqlWriteOp> BuildTestRow(const TableHandle& table, int index) {
    auto insert = table.NewInsertOp();
    auto req = insert->mutable_request();
    QLAddInt32HashValue(req, index);
    const auto& columns = table.schema().columns();
    table.AddInt32ColumnValue(req, columns[1].name(), index * 2);
    table.AddStringColumnValue(req, columns[2].name(), StringPrintf("hello %d", index));
    table.AddInt32ColumnValue(req, columns[3].name(), index * 3);
    return insert;
  }

  shared_ptr<YBqlWriteOp> UpdateTestRow(const TableHandle& table, int index) {
    auto update = table.NewUpdateOp();
    auto req = update->mutable_request();
    QLAddInt32HashValue(req, index);
    const auto& columns = table.schema().columns();
    table.AddInt32ColumnValue(req, columns[1].name(), index * 2 + 1);
    table.AddStringColumnValue(req, columns[2].name(), StringPrintf("hello again %d", index));
    return update;
  }

  shared_ptr<YBqlWriteOp> DeleteTestRow(const TableHandle& table, int index) {
    auto del = table.NewDeleteOp();
    QLAddInt32HashValue(del->mutable_request(), index);
    return del;
  }

  void DoTestScanWithoutPredicates() {
    client::TableIteratorOptions options;
    options.columns = std::vector<std::string>{"key"};
    LOG_TIMING(INFO, "Scanning with no predicates") {
      uint64_t sum = 0;
      for (const auto& row : client::TableRange(client_table_, options)) {
        sum += row.column(0).int32_value();
      }
      // The sum should be the sum of the arithmetic series from
      // 0..FLAGS_test_scan_num_rows-1
      uint64_t expected = implicit_cast<uint64_t>(FLAGS_test_scan_num_rows) *
          (0 + (FLAGS_test_scan_num_rows - 1)) / 2;
      ASSERT_EQ(expected, sum);
    }
  }

  void DoTestScanWithStringPredicate() {
    TableIteratorOptions options;
    options.filter = FilterBetween("hello 2"s, Inclusive::kFalse,
                                   "hello 3"s, Inclusive::kFalse,
                                   "string_val");

    bool found = false;
    LOG_TIMING(INFO, "Scanning with string predicate") {
      for (const auto& row : TableRange(client_table_, options)) {
        found = true;
        Slice slice(row.column(2).string_value());
        if (!slice.starts_with("hello 2") && !slice.starts_with("hello 3")) {
          FAIL() << row.ToString();
        }
      }
    }
    ASSERT_TRUE(found);
  }

  void DoTestScanWithKeyPredicate() {
    auto op = client_table_.NewReadOp();
    auto req = op->mutable_request();

    auto* const condition = req->mutable_where_expr()->mutable_condition();
    condition->set_op(QL_OP_AND);
    client_table_.AddInt32Condition(condition, "key", QL_OP_GREATER_THAN_EQUAL, 5);
    client_table_.AddInt32Condition(condition, "key", QL_OP_LESS_THAN_EQUAL, 10);
    client_table_.AddColumns({"key"}, req);
    auto session = client_->NewSession();
    session->SetTimeout(60s);
    ASSERT_OK(session->ApplyAndFlush(op));
    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status());
    auto rowblock = ql::RowsResult(op.get()).GetRowBlock();
    for (const auto& row : rowblock->rows()) {
      int32_t key = row.column(0).int32_value();
      ASSERT_GE(key, 5);
      ASSERT_LE(key, 10);
    }
  }

  // Creates a table with RF=FLAGS_replication_factor, split into tablets based on 'split_rows'
  // (or single tablet if 'split_rows' is empty).
  void CreateTable(const YBTableName& table_name_orig,
                   int num_tablets,
                   TableHandle* table) {
    auto num_replicas = FLAGS_replication_factor;
    // The implementation allows table name without a keyspace.
    YBTableName table_name(table_name_orig.namespace_type(), table_name_orig.has_namespace() ?
        table_name_orig.namespace_name() : kKeyspaceName, table_name_orig.table_name());

    bool added_replicas = false;
    // Add more tablet servers to satisfy all replicas, if necessary.
    while (cluster_->num_tablet_servers() < num_replicas) {
      ASSERT_OK(cluster_->AddTabletServer());
      added_replicas = true;
    }

    if (added_replicas) {
      ASSERT_OK(cluster_->WaitForTabletServerCount(num_replicas));
    }

    ASSERT_OK(table->Create(table_name, num_tablets, schema_, client_.get()));
  }

  // Kills a tablet server.
  // Boolean flags control whether to restart the tserver, and if so, whether to wait for it to
  // finish bootstrapping.
  Status KillTServerImpl(const string& uuid, const bool restart, const bool wait_started) {
    bool ts_found = false;
    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      MiniTabletServer* ts = cluster_->mini_tablet_server(i);
      if (ts->server()->instance_pb().permanent_uuid() == uuid) {
        if (restart) {
          LOG(INFO) << "Restarting TS at " << ts->bound_rpc_addr();
              RETURN_NOT_OK(ts->Restart());
          if (wait_started) {
            LOG(INFO) << "Waiting for TS " << ts->bound_rpc_addr() << " to finish bootstrapping";
                RETURN_NOT_OK(ts->WaitStarted());
          }
        } else {
          LOG(INFO) << "Killing TS " << uuid << " at " << ts->bound_rpc_addr();
          ts->Shutdown();
        }
        ts_found = true;
        break;
      }
    }
    if (!ts_found) {
      return STATUS(InvalidArgument, strings::Substitute("Could not find tablet server $1", uuid));
    }

    return Status::OK();
  }

  Status RestartTServerAndWait(const string& uuid) {
    return KillTServerImpl(uuid, true, true);
  }

  Status RestartTServerAsync(const string& uuid) {
    return KillTServerImpl(uuid, true, false);
  }

  Status KillTServer(const string& uuid) {
    return KillTServerImpl(uuid, false, false);
  }

  void DoApplyWithoutFlushTest(int sleep_micros);

  Result<std::unique_ptr<rpc::Messenger>> CreateMessenger(const std::string& name) {
    return rpc::MessengerBuilder(name).Build();
  }

  void VerifyKeyRangeFiltering(const std::vector<string>& sorted_partitions,
                               const std::vector<internal::RemoteTabletPtr>& tablets,
                               const string& start_key, const string& end_key) {
    auto start_idx = FindPartitionStartIndex(sorted_partitions, start_key);
    auto end_idx = FindPartitionStartIndexExclusiveBound(sorted_partitions, end_key);
    auto filtered_tablets =
        ASSERT_RESULT(FilterTabletsByHashPartitionKeyRange(tablets, start_key, end_key));
    std::vector<string> filtered_partitions;
    std::transform(filtered_tablets.begin(), filtered_tablets.end(),
                   std::back_inserter(filtered_partitions),
                   [](const auto& tablet) { return tablet->partition().partition_key_start(); });
    std::sort(filtered_partitions.begin(), filtered_partitions.end());

    ASSERT_EQ(filtered_partitions,
              std::vector<string>(&sorted_partitions[start_idx], &sorted_partitions[end_idx + 1]));
  }

  size_t FindPartitionStartIndexExclusiveBound(
    const std::vector<std::string>& partitions,
    const std::string& partition_key) {
    if (partition_key.empty()) {
      return partitions.size() - 1;
    }

    auto it = std::lower_bound(partitions.begin(), partitions.end(), partition_key);
    if (it == partitions.end() || *it >= partition_key) {
      if (it == partitions.begin()) {
        return 0;
      }
      --it;
    }
    return it - partitions.begin();
  }

  enum WhichServerToKill {
    DEAD_MASTER,
    DEAD_TSERVER
  };
  void DoTestWriteWithDeadServer(WhichServerToKill which);

  YBSchema schema_;

  gscoped_ptr<MiniCluster> cluster_;
  std::unique_ptr<YBClient> client_;
  TableHandle client_table_;
  TableHandle client_table2_;
  TableHandle client_table3_;
};


const string ClientTest::kKeyspaceName("my_keyspace");
const YBTableName ClientTest::kTableName(YQL_DATABASE_CQL, kKeyspaceName, "client-testtb");
const YBTableName ClientTest::kTable2Name(YQL_DATABASE_CQL, kKeyspaceName, "client-testtb2");
const YBTableName ClientTest::kTable3Name(YQL_DATABASE_CQL, kKeyspaceName, "client-testtb3");

namespace {

TableFilter MakeFilter(int32_t lower_bound, int32_t upper_bound, std::string column = "key") {
  if (lower_bound != kNoBound) {
    if (upper_bound != kNoBound) {
      return FilterBetween(lower_bound, Inclusive::kTrue, upper_bound, Inclusive::kTrue,
                           std::move(column));
    } else {
      return FilterGreater(lower_bound, Inclusive::kTrue, std::move(column));
    }
  }
  if (upper_bound != kNoBound) {
    return FilterLess(upper_bound, Inclusive::kTrue, std::move(column));
  }
  return TableFilter();
}

int CountRowsFromClient(const TableHandle& table, YBConsistencyLevel consistency,
                        int32_t lower_bound, int32_t upper_bound) {
  TableIteratorOptions options;
  options.consistency = consistency;
  options.columns = std::vector<std::string>{"key"};
  options.filter = MakeFilter(lower_bound, upper_bound);
  return boost::size(TableRange(table, options));
}

int CountRowsFromClient(const TableHandle& table, int32_t lower_bound, int32_t upper_bound) {
  return CountRowsFromClient(table, YBConsistencyLevel::STRONG, lower_bound, upper_bound);
}

int CountRowsFromClient(const TableHandle& table) {
  return CountRowsFromClient(table, kNoBound, kNoBound);
}

// Count the rows of a table, checking that the operation succeeds.
//
// Must be public to use as a thread closure.
void CheckRowCount(const TableHandle& table) {
  CountRowsFromClient(table);
}

} // namespace

constexpr int kLookupWaitTimeSecs = 30;
constexpr int kNumTabletsPerTable = 8;
constexpr int kNumIterations = 1000;

class ClientTestForceMasterLookup :
    public ClientTest, public ::testing::WithParamInterface<bool /* force_master_lookup */> {
 public:
  void SetUp() override {
    ClientTest::SetUp();
    // Do we want to force going to the master instead of using cache.
    SetAtomicFlag(GetParam(), &FLAGS_TEST_force_master_lookup_all_tablets);
    SetAtomicFlag(0.5, &FLAGS_TEST_simulate_lookup_timeout_probability);
  }


  void PerformManyLookups(const std::shared_ptr<const YBTable>& table, bool point_lookup) {
    for (int i = 0; i < kNumIterations; i++) {
      if (point_lookup) {
          auto key_rt = ASSERT_RESULT(LookupFirstTabletFuture(table).get());
          ASSERT_NOTNULL(key_rt);
      } else {
        auto tablets = ASSERT_RESULT(client_->LookupAllTabletsFuture(
            table, CoarseMonoClock::Now() + MonoDelta::FromSeconds(kLookupWaitTimeSecs)).get());
        ASSERT_EQ(tablets.size(), kNumTabletsPerTable);
      }
    }
  }
};

INSTANTIATE_TEST_CASE_P(ForceMasterLookup, ClientTestForceMasterLookup, ::testing::Bool());

TEST_P(ClientTestForceMasterLookup, TestConcurrentLookups) {
  ASSERT_NO_FATALS(CreateTable(kTable3Name, kNumTabletsPerTable, &client_table3_));

  std::shared_ptr<YBTable> table;
  ASSERT_OK(client_->OpenTable(kTable3Name, &table));

  ASSERT_OK(cluster_->leader_mini_master()->master()->
            WaitUntilCatalogManagerIsLeaderAndReadyForTests());

  auto t1 = std::thread([&]() { ASSERT_NO_FATALS(
      PerformManyLookups(table, true /* point_lookup */)); });
  auto t2 = std::thread([&]() { ASSERT_NO_FATALS(
      PerformManyLookups(table, false /* point_lookup */)); });

  t1.join();
  t2.join();
}

TEST_F(ClientTest, TestLookupAllTablets) {
  ASSERT_NO_FATALS(CreateTable(kTable3Name, kNumTabletsPerTable, &client_table3_));

  std::shared_ptr<YBTable> table;
  ASSERT_OK(client_->OpenTable(kTable3Name, &table));

  ASSERT_OK(cluster_->leader_mini_master()->master()->
            WaitUntilCatalogManagerIsLeaderAndReadyForTests());

  auto future = client_->LookupAllTabletsFuture(
      table, CoarseMonoClock::Now() + MonoDelta::FromSeconds(kLookupWaitTimeSecs));

  auto tablets = ASSERT_RESULT(future.get());
  ASSERT_EQ(tablets.size(), 8);
}

TEST_F(ClientTest, TestPointThenRangeLookup) {
  ASSERT_NO_FATALS(CreateTable(kTable3Name, kNumTabletsPerTable, &client_table3_));

  std::shared_ptr<YBTable> table;
  ASSERT_OK(client_->OpenTable(kTable3Name, &table));

  ASSERT_OK(cluster_->leader_mini_master()->master()->
            WaitUntilCatalogManagerIsLeaderAndReadyForTests());

  auto key_rt = ASSERT_RESULT(LookupFirstTabletFuture(table).get());
  ASSERT_NOTNULL(key_rt);

  auto tablets = ASSERT_RESULT(client_->LookupAllTabletsFuture(
      table, CoarseMonoClock::Now() + MonoDelta::FromSeconds(kLookupWaitTimeSecs)).get());

  ASSERT_EQ(tablets.size(), kNumTabletsPerTable);
}

TEST_F(ClientTest, TestKeyRangeFiltering) {
  ASSERT_NO_FATALS(CreateTable(kTable3Name, 8, &client_table3_));

  std::shared_ptr<YBTable> table;
  ASSERT_OK(client_->OpenTable(kTable3Name, &table));

  ASSERT_OK(cluster_->leader_mini_master()->master()->
            WaitUntilCatalogManagerIsLeaderAndReadyForTests());

  auto tablets = ASSERT_RESULT(client_->LookupAllTabletsFuture(
      table, CoarseMonoClock::Now() + MonoDelta::FromSeconds(kLookupWaitTimeSecs)).get());
  // First, verify, that using empty bounds on both sides returns all tablets.
  auto filtered_tablets =
      ASSERT_RESULT(FilterTabletsByHashPartitionKeyRange(tablets, std::string(), std::string()));
  ASSERT_EQ(kNumTabletsPerTable, filtered_tablets.size());

  std::vector<std::string> partition_starts;
  for (const auto& tablet : tablets) {
    partition_starts.push_back(tablet->partition().partition_key_start());
  }
  std::sort(partition_starts.begin(), partition_starts.end());

  auto start_key = partition_starts[0];
  auto end_key = partition_starts[2];
  ASSERT_NO_FATALS(VerifyKeyRangeFiltering(partition_starts, tablets, start_key, end_key));

  start_key = partition_starts[5];
  end_key = partition_starts[7];
  ASSERT_NO_FATALS(VerifyKeyRangeFiltering(partition_starts, tablets, start_key, end_key));

  auto fixed_key = PartitionSchema::EncodeMultiColumnHashValue(10);
  ASSERT_NOK(FilterTabletsByHashPartitionKeyRange(tablets, fixed_key, fixed_key));

  for (int i = 0; i < kNumIterations; i++) {
    auto start_idx = RandomUniformInt(0, PartitionSchema::kMaxPartitionKey - 1);
    auto end_idx = RandomUniformInt(start_idx + 1, PartitionSchema::kMaxPartitionKey);
    ASSERT_NO_FATALS(VerifyKeyRangeFiltering(partition_starts, tablets,
                     PartitionSchema::EncodeMultiColumnHashValue(start_idx),
                     PartitionSchema::EncodeMultiColumnHashValue(end_idx)));
  }
}

TEST_F(ClientTest, TestListTables) {
  auto tables = ASSERT_RESULT(client_->ListTables());
  std::sort(tables.begin(), tables.end(), [](const YBTableName& n1, const YBTableName& n2) {
    return n1.ToString() < n2.ToString();
  });
  ASSERT_EQ(2 + master::kNumSystemTablesWithTxn, tables.size());
  ASSERT_EQ(kTableName, tables[0]) << "Tables:" << AsString(tables);
  ASSERT_EQ(kTable2Name, tables[1]) << "Tables:" << AsString(tables);
  tables.clear();
  tables = ASSERT_RESULT(client_->ListTables("testtb2"));
  ASSERT_EQ(1, tables.size());
  ASSERT_EQ(kTable2Name, tables[0]) << "Tables:" << AsString(tables);
}

TEST_F(ClientTest, TestListTabletServers) {
  std::vector<std::unique_ptr<YBTabletServer>> tss;
  ASSERT_OK(client_->ListTabletServers(&tss));
  ASSERT_EQ(3, tss.size());
  set<string> actual_ts_uuids;
  set<string> actual_ts_hostnames;
  set<string> expected_ts_uuids;
  set<string> expected_ts_hostnames;
  for (int i = 0; i < tss.size(); ++i) {
    auto server = cluster_->mini_tablet_server(i)->server();
    expected_ts_uuids.insert(server->instance_pb().permanent_uuid());
    actual_ts_uuids.insert(tss[i]->uuid());
    expected_ts_hostnames.insert(server->options().broadcast_addresses[0].host());
    actual_ts_hostnames.insert(tss[i]->hostname());
  }
  ASSERT_EQ(expected_ts_uuids, actual_ts_uuids);
  ASSERT_EQ(expected_ts_hostnames, actual_ts_hostnames);
}

TEST_F(ClientTest, TestBadTable) {
  shared_ptr<YBTable> t;
  Status s = client_->OpenTable(
      YBTableName(YQL_DATABASE_CQL, kKeyspaceName, "xxx-does-not-exist"), &t);
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(false), "Not found: The object does not exist");
}

// Test that, if the master is down, we experience a network error talking
// to it (no "find the new leader master" since there's only one master).
TEST_F(ClientTest, TestMasterDown) {
  DontVerifyClusterBeforeNextTearDown();
  cluster_->mini_master()->Shutdown();
  shared_ptr<YBTable> t;
  client_->data_->default_admin_operation_timeout_ = MonoDelta::FromSeconds(1);
  Status s = client_->OpenTable(YBTableName(YQL_DATABASE_CQL, kKeyspaceName, "other-tablet"), &t);
  ASSERT_TRUE(s.IsTimedOut());
}

// TODO scan with predicates is not supported.
TEST_F(ClientTest, TestScan) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  ASSERT_EQ(FLAGS_test_scan_num_rows, CountRowsFromClient(client_table_));

  // Scan after insert
  DoTestScanWithoutPredicates();
  DoTestScanWithStringPredicate();
  DoTestScanWithKeyPredicate();

  // Scan after update
  UpdateTestRows(client_table_, 0, FLAGS_test_scan_num_rows);
  DoTestScanWithKeyPredicate();

  // Scan after delete half
  DeleteTestRows(client_table_, 0, FLAGS_test_scan_num_rows / 2);
  DoTestScanWithKeyPredicate();

  // Scan after delete all
  DeleteTestRows(client_table_, FLAGS_test_scan_num_rows / 2 + 1, FLAGS_test_scan_num_rows);
  DoTestScanWithKeyPredicate();

  // Scan after re-insert
  InsertTestRows(client_table_, 1);
  DoTestScanWithKeyPredicate();
}

void CheckCounts(const TableHandle& table, const std::vector<int>& expected) {
  std::vector<std::pair<int, int>> bounds = {
    { kNoBound, kNoBound },
    { kNoBound, 15 },
    { 27, kNoBound },
    { 0, 15 },
    { 0, 10 },
    { 0, 20 },
    { 0, 30 },
    { 14, 30 },
    { 30, 30 },
    { 50, kNoBound },
  };
  ASSERT_EQ(bounds.size(), expected.size());
  for (size_t i = 0; i != bounds.size(); ++i) {
    ASSERT_EQ(expected[i], CountRowsFromClient(table, bounds[i].first, bounds[i].second));
  }
  // Run through various scans.
}

TEST_F(ClientTest, TestScanMultiTablet) {
  // 5 tablets, each with 10 rows worth of space.
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(YBTableName(YQL_DATABASE_CQL, "TestScanMultiTablet"), 5, &table));

  // Insert rows with keys 12, 13, 15, 17, 22, 23, 25, 27...47 into each
  // tablet, except the first which is empty.
    auto session = CreateSession();
  for (int i = 1; i < 5; i++) {
    ASSERT_OK(session->Apply(BuildTestRow(table, 2 + (i * 10))));
    ASSERT_OK(session->Apply(BuildTestRow(table, 3 + (i * 10))));
    ASSERT_OK(session->Apply(BuildTestRow(table, 5 + (i * 10))));
    ASSERT_OK(session->Apply(BuildTestRow(table, 7 + (i * 10))));
  }
  FlushSessionOrDie(session);

  // Run through various scans.
  CheckCounts(table, { 16, 3, 9, 3, 0, 4, 8, 6, 0, 0 });

  // Update every other row
  for (int i = 1; i < 5; ++i) {
    ASSERT_OK(session->Apply(UpdateTestRow(table, 2 + i * 10)));
    ASSERT_OK(session->Apply(UpdateTestRow(table, 5 + i * 10)));
  }
  FlushSessionOrDie(session);

  // Check all counts the same (make sure updates don't change # of rows)
  CheckCounts(table, { 16, 3, 9, 3, 0, 4, 8, 6, 0, 0 });

  // Delete half the rows
  for (int i = 1; i < 5; ++i) {
    ASSERT_OK(session->Apply(DeleteTestRow(table, 5 + i*10)));
    ASSERT_OK(session->Apply(DeleteTestRow(table, 7 + i*10)));
  }
  FlushSessionOrDie(session);

  // Check counts changed accordingly
  CheckCounts(table, { 8, 2, 4, 2, 0, 2, 4, 2, 0, 0 });

  // Delete rest of rows
  for (int i = 1; i < 5; ++i) {
    ASSERT_OK(session->Apply(DeleteTestRow(table, 2 + i*10)));
    ASSERT_OK(session->Apply(DeleteTestRow(table, 3 + i*10)));
  }
  FlushSessionOrDie(session);

  // Check counts changed accordingly
  CheckCounts(table, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
}

TEST_F(ClientTest, TestScanEmptyTable) {
  TableIteratorOptions options;
  options.columns = std::vector<std::string>();
  ASSERT_EQ(boost::size(TableRange(client_table_, options)), 0);
}

// Test scanning with an empty projection. This should yield an empty
// row block with the proper number of rows filled in. Impala issues
// scans like this in order to implement COUNT(*).
TEST_F(ClientTest, TestScanEmptyProjection) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));
  TableIteratorOptions options;
  options.columns = std::vector<std::string>();
  ASSERT_EQ(boost::size(TableRange(client_table_, options)), FLAGS_test_scan_num_rows);
}

// Test a scan where we have a predicate on a key column that is not
// in the projection.
TEST_F(ClientTest, TestScanPredicateKeyColNotProjected) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  size_t nrows = 0;
  TableIteratorOptions options;
  options.columns = std::vector<std::string>{"key", "int_val"};
  options.filter = MakeFilter(5, 10);
  for (const auto& row : TableRange(client_table_, options)) {
    int32_t key = row.column(0).int32_value();
    int32_t val = row.column(1).int32_value();
    ASSERT_EQ(key * 2, val);

    ++nrows;
  }

  ASSERT_EQ(6, nrows);
}

// Test a scan where we have a predicate on a non-key column that is
// not in the projection.
TEST_F(ClientTest, TestScanPredicateNonKeyColNotProjected) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  size_t nrows = 0;
  TableIteratorOptions options;
  options.columns = std::vector<std::string>{"key", "int_val"};
  options.filter = MakeFilter(10, 20, "int_val");
  TableRange range(client_table_, options);
  for (const auto& row : range) {
    int32_t key = row.column(0).int32_value();
    int32_t val = row.column(1).int32_value();
    ASSERT_EQ(key * 2, val);

    ++nrows;
  }

  ASSERT_EQ(nrows, 6);
}

TEST_F(ClientTest, TestGetTabletServerBlacklist) {
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(YBTableName(YQL_DATABASE_CQL, "blacklist"), kNumTablets, &table));
  InsertTestRows(table, 1, 0);

  // Look up the tablet and its replicas into the metadata cache.
  // We have to loop since some replicas may have been created slowly.
  scoped_refptr<internal::RemoteTablet> rt;
  while (true) {
    rt = ASSERT_RESULT(LookupFirstTabletFuture(table.table()).get());
    ASSERT_TRUE(rt.get() != nullptr);
    vector<internal::RemoteTabletServer*> tservers;
    rt->GetRemoteTabletServers(&tservers);
    if (tservers.size() == 3) {
      break;
    }
    rt->MarkStale();
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  // Get the Leader.
  internal::RemoteTabletServer *rts;
  set<string> blacklist;
  vector<internal::RemoteTabletServer*> candidates;
  vector<internal::RemoteTabletServer*> tservers;
  ASSERT_OK(client_->data_->GetTabletServer(client_.get(), rt,
                                            YBClient::LEADER_ONLY,
                                            blacklist, &candidates, &rts));
  tservers.push_back(rts);
  // Blacklist the leader, should not work.
  blacklist.insert(rts->permanent_uuid());
  {
    Status s = client_->data_->GetTabletServer(client_.get(), rt,
                                               YBClient::LEADER_ONLY,
                                               blacklist, &candidates, &rts);
    ASSERT_TRUE(s.IsServiceUnavailable());
  }
  // Keep blacklisting replicas until we run out.
  ASSERT_OK(client_->data_->GetTabletServer(client_.get(), rt,
                                            YBClient::CLOSEST_REPLICA,
                                            blacklist, &candidates, &rts));
  tservers.push_back(rts);
  blacklist.insert(rts->permanent_uuid());
  ASSERT_OK(client_->data_->GetTabletServer(client_.get(), rt,
                                            YBClient::FIRST_REPLICA,
                                            blacklist, &candidates, &rts));
  tservers.push_back(rts);
  blacklist.insert(rts->permanent_uuid());

  // Make sure none of the three modes work when all nodes are blacklisted.
  vector<YBClient::ReplicaSelection> selections;
  selections.push_back(YBClient::LEADER_ONLY);
  selections.push_back(YBClient::CLOSEST_REPLICA);
  selections.push_back(YBClient::FIRST_REPLICA);
  for (YBClient::ReplicaSelection selection : selections) {
    Status s = client_->data_->GetTabletServer(client_.get(), rt, selection,
                                               blacklist, &candidates, &rts);
    ASSERT_TRUE(s.IsServiceUnavailable());
  }

  // Make sure none of the modes work when all nodes are dead.
  for (internal::RemoteTabletServer* rt : tservers) {
    client_->data_->meta_cache_->MarkTSFailed(rt, STATUS(NetworkError, "test"));
  }
  blacklist.clear();
  for (YBClient::ReplicaSelection selection : selections) {
    Status s = client_->data_->GetTabletServer(client_.get(), rt,
                                               selection,
                                               blacklist, &candidates, &rts);
    ASSERT_TRUE(s.IsServiceUnavailable());
  }
}

TEST_F(ClientTest, TestScanWithEncodedRangePredicate) {
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(YBTableName(YQL_DATABASE_CQL, "split-table"),
                               kNumTablets,
                               &table));

  ASSERT_NO_FATALS(InsertTestRows(table, 100));

  TableRange all_range(table, {});
  auto all_rows = ScanToStrings(all_range);
  ASSERT_EQ(100, all_rows.size());

  // Test a double-sided range within first tablet
  {
    TableIteratorOptions options;
    options.filter = FilterBetween(5, Inclusive::kTrue, 8, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(8 - 5, rows.size());
    EXPECT_EQ(all_rows[5], rows.front());
    EXPECT_EQ(all_rows[7], rows.back());
  }

  // Test a double-sided range spanning tablets
  {
    TableIteratorOptions options;
    options.filter = FilterBetween(5, Inclusive::kTrue, 15, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(15 - 5, rows.size());
    EXPECT_EQ(all_rows[5], rows.front());
    EXPECT_EQ(all_rows[14], rows.back());
  }

  // Test a double-sided range within second tablet
  {
    TableIteratorOptions options;
    options.filter = FilterBetween(15, Inclusive::kTrue, 20, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(20 - 15, rows.size());
    EXPECT_EQ(all_rows[15], rows.front());
    EXPECT_EQ(all_rows[19], rows.back());
  }

  // Test a lower-bound only range.
  {
    TableIteratorOptions options;
    options.filter = FilterGreater(5, Inclusive::kTrue);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(95, rows.size());
    EXPECT_EQ(all_rows[5], rows.front());
    EXPECT_EQ(all_rows[99], rows.back());
  }

  // Test an upper-bound only range in first tablet.
  {
    TableIteratorOptions options;
    options.filter = FilterLess(5, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(5, rows.size());
    EXPECT_EQ(all_rows[0], rows.front());
    EXPECT_EQ(all_rows[4], rows.back());
  }

  // Test an upper-bound only range in second tablet.
  {
    TableIteratorOptions options;
    options.filter = FilterLess(15, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(15, rows.size());
    EXPECT_EQ(all_rows[0], rows.front());
    EXPECT_EQ(all_rows[14], rows.back());
  }
}

static std::unique_ptr<YBError> GetSingleErrorFromSession(YBSession* session) {
  CHECK_EQ(1, session->CountPendingErrors());
  CollectedErrors errors = session->GetAndClearPendingErrors();
  CHECK_EQ(1, errors.size());
  std::unique_ptr<YBError> result = std::move(errors.front());
  return result;
}

// Simplest case of inserting through the client API: a single row
// with manual batching.
// TODO Actually we need to check that hash columns present during insert. But it is not done yet.
TEST_F(ClientTest, DISABLED_TestInsertSingleRowManualBatch) {
  auto session = CreateSession();
  ASSERT_FALSE(session->HasPendingOperations());

  auto insert = client_table_.NewInsertOp();
  // Try inserting without specifying a key: should fail.
  client_table_.AddInt32ColumnValue(insert->mutable_request(), "int_val", 54321);
  client_table_.AddStringColumnValue(insert->mutable_request(), "string_val", "hello world");
  ASSERT_OK(session->ApplyAndFlush(insert));
  ASSERT_EQ(QLResponsePB::YQL_STATUS_RUNTIME_ERROR, insert->response().status());

  // Retry
  QLAddInt32HashValue(insert->mutable_request(), 12345);
  ASSERT_OK(session->Apply(insert));
  ASSERT_TRUE(session->HasPendingOperations()) << "Should be pending until we Flush";

  FlushSessionOrDie(session, { insert });
}

namespace {

CHECKED_STATUS ApplyInsertToSession(YBSession* session,
                                    const TableHandle& table,
                                    int row_key,
                                    int int_val,
                                    const char* string_val,
                                    std::shared_ptr<YBqlOp>* op = nullptr) {
  auto insert = table.NewInsertOp();
  QLAddInt32HashValue(insert->mutable_request(), row_key);
  table.AddInt32ColumnValue(insert->mutable_request(), "int_val", int_val);
  table.AddStringColumnValue(insert->mutable_request(), "string_val", string_val);
  if (op) {
    *op = insert;
  }
  return session->Apply(insert);
}

CHECKED_STATUS ApplyUpdateToSession(YBSession* session,
                                    const TableHandle& table,
                                    int row_key,
                                    int int_val) {
  auto update = table.NewUpdateOp();
  QLAddInt32HashValue(update->mutable_request(), row_key);
  table.AddInt32ColumnValue(update->mutable_request(), "int_val", int_val);
  return session->Apply(update);
}

CHECKED_STATUS ApplyDeleteToSession(YBSession* session,
                                    const TableHandle& table,
                                    int row_key) {
  auto del = table.NewDeleteOp();
  QLAddInt32HashValue(del->mutable_request(), row_key);
  return session->Apply(del);
}

} // namespace

TEST_F(ClientTest, TestWriteTimeout) {
  auto session = CreateSession();

  LOG(INFO) << "Time out the lookup on the master side";
  {
    google::FlagSaver saver;
    FLAGS_master_inject_latency_on_tablet_lookups_ms = 110;
    session->SetTimeout(100ms);
    ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
    Status s = session->Flush();
    ASSERT_TRUE(s.IsIOError()) << "unexpected status: " << s.ToString();
    auto error = GetSingleErrorFromSession(session.get());
    ASSERT_TRUE(error->status().IsTimedOut()) << error->status().ToString();
    ASSERT_STR_CONTAINS(error->status().ToString(),
        strings::Substitute("GetTableLocations($0, hash_code: NaN, 1) failed: "
            "timed out after deadline expired", client_table_->name().ToString()));
  }

  LOG(INFO) << "Time out the actual write on the tablet server";
  {
    google::FlagSaver saver;
    SetAtomicFlag(true, &FLAGS_log_inject_latency);
    SetAtomicFlag(110, &FLAGS_log_inject_latency_ms_mean);
    SetAtomicFlag(0, &FLAGS_log_inject_latency_ms_stddev);

    ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
    Status s = session->Flush();
    ASSERT_TRUE(s.IsIOError()) << s;
    auto error = GetSingleErrorFromSession(session.get());
    ASSERT_TRUE(error->status().IsTimedOut()) << error->status().ToString();
    ASSERT_STR_CONTAINS(error->status().ToString(), "timed out");
  }
}

// Test which does an async flush and then drops the reference
// to the Session. This should still call the callback.
TEST_F(ClientTest, TestAsyncFlushResponseAfterSessionDropped) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
  Synchronizer s;
  session->FlushAsync(s.AsStatusFunctor());
  session.reset();
  ASSERT_OK(s.Wait());

  // Try again, this time should not have an error response (to re-insert the same row).
  s.Reset();
  session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
  ASSERT_EQ(1, session->CountBufferedOperations());
  session->FlushAsync(s.AsStatusFunctor());
  ASSERT_EQ(0, session->CountBufferedOperations());
  session.reset();
  ASSERT_OK(s.Wait());
}

TEST_F(ClientTest, TestSessionClose) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
  // Closing the session now should return Status::IllegalState since we
  // have a pending operation.
  ASSERT_TRUE(session->Close().IsIllegalState());

  ASSERT_OK(session->Flush());

  ASSERT_OK(session->Close());
}

// Test which sends multiple batches through the same session, each of which
// contains multiple rows spread across multiple tablets.
TEST_F(ClientTest, TestMultipleMultiRowManualBatches) {
  auto session = CreateSession();

  const int kNumBatches = 5;
  const int kRowsPerBatch = 10;

  int row_key = 0;

  for (int batch_num = 0; batch_num < kNumBatches; batch_num++) {
    for (int i = 0; i < kRowsPerBatch; i++) {
      ASSERT_OK(ApplyInsertToSession(
                         session.get(),
                         (row_key % 2 == 0) ? client_table_ : client_table2_,
                         row_key, row_key * 10, "hello world"));
      row_key++;
    }
    ASSERT_TRUE(session->HasPendingOperations()) << "Should be pending until we Flush";
    FlushSessionOrDie(session);
    ASSERT_FALSE(session->HasPendingOperations()) << "Should have no more pending ops after flush";
  }

  const int kNumRowsPerTablet = kNumBatches * kRowsPerBatch / 2;
  ASSERT_EQ(kNumRowsPerTablet, CountRowsFromClient(client_table_));
  ASSERT_EQ(kNumRowsPerTablet, CountRowsFromClient(client_table2_));

  // Verify the data looks right.
  auto rows = ScanTableToStrings(client_table_);
  std::sort(rows.begin(), rows.end());
  ASSERT_EQ(kNumRowsPerTablet, rows.size());
  ASSERT_EQ("{ int32:0, int32:0, string:\"hello world\", null }", rows[0]);
}

// Test a batch where one of the inserted rows succeeds and duplicates succeed too.
TEST_F(ClientTest, TestBatchWithDuplicates) {
  auto session = CreateSession();

  // Insert a row with key "1"
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "original row"));
  FlushSessionOrDie(session);

  // Now make a batch that has key "1" along with
  // key "2" which will succeed. Flushing should not return an error.
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "Attempted dup"));
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 2, 1, "Should succeed"));
  Status s = session->Flush();
  ASSERT_TRUE(s.ok());

  // Verify that the other row was successfully inserted
  auto rows = ScanTableToStrings(client_table_);
  ASSERT_EQ(2, rows.size());
  std::sort(rows.begin(), rows.end());
  ASSERT_EQ("{ int32:1, int32:1, string:\"Attempted dup\", null }", rows[0]);
  ASSERT_EQ("{ int32:2, int32:1, string:\"Should succeed\", null }", rows[1]);
}

// Test flushing an empty batch (should be a no-op).
TEST_F(ClientTest, TestEmptyBatch) {
  auto session = CreateSession();
  FlushSessionOrDie(session);
}

void ClientTest::DoTestWriteWithDeadServer(WhichServerToKill which) {
  DontVerifyClusterBeforeNextTearDown();
  auto session = CreateSession();
  session->SetTimeout(1s);

  // Shut down the server.
  switch (which) {
    case DEAD_MASTER:
      cluster_->mini_master()->Shutdown();
      break;
    case DEAD_TSERVER:
      for (int i = 0; i < cluster_->num_tablet_servers(); ++i) {
        cluster_->mini_tablet_server(i)->Shutdown();
      }
      break;
  }

  // Try a write.
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "x"));
  Status s = session->Flush();
  ASSERT_TRUE(s.IsIOError()) << s.ToString();

  auto error = GetSingleErrorFromSession(session.get());
  switch (which) {
    case DEAD_MASTER:
      // Only one master, so no retry for finding the new leader master.
      ASSERT_TRUE(error->status().IsTimedOut());
      break;
    case DEAD_TSERVER:
      ASSERT_TRUE(error->status().IsTimedOut());
      auto pos = error->status().ToString().find("Connection refused");
      if (pos == std::string::npos) {
        pos = error->status().ToString().find("Broken pipe");
      }
      ASSERT_NE(std::string::npos, pos);
      break;
  }

  ASSERT_STR_CONTAINS(error->failed_op().ToString(), "QL_WRITE");
}

// Test error handling cases where the master is down (tablet resolution fails)
TEST_F(ClientTest, TestWriteWithDeadMaster) {
  client_->data_->default_admin_operation_timeout_ = MonoDelta::FromSeconds(1);
  DoTestWriteWithDeadServer(DEAD_MASTER);
}

// Test error handling when the TS is down (actual write fails its RPC)
TEST_F(ClientTest, TestWriteWithDeadTabletServer) {
  DoTestWriteWithDeadServer(DEAD_TSERVER);
}

void ClientTest::DoApplyWithoutFlushTest(int sleep_micros) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "x"));
  SleepFor(MonoDelta::FromMicroseconds(sleep_micros));
  session.reset(); // should not crash!

  // Should have no rows.
  auto rows = ScanTableToStrings(client_table_);
  ASSERT_EQ(0, rows.size());
}


// Applies some updates to the session, and then drops the reference to the
// Session before flushing. Makes sure that the tablet resolution callbacks
// properly deal with the session disappearing underneath.
//
// This test doesn't sleep between applying the operations and dropping the
// reference, in hopes that the reference will be dropped while DNS is still
// in-flight, etc.
TEST_F(ClientTest, TestApplyToSessionWithoutFlushing_OpsInFlight) {
  DoApplyWithoutFlushTest(0);
}

// Same as the above, but sleeps a little bit after applying the operations,
// so that the operations are already in the per-TS-buffer.
TEST_F(ClientTest, TestApplyToSessionWithoutFlushing_OpsBuffered) {
  DoApplyWithoutFlushTest(10000);
}

// Apply a large amount of data without calling Flush(), and ensure
// that we get an error on Apply() rather than sending a too-large
// RPC to the server.
TEST_F(ClientTest, DISABLED_TestApplyTooMuchWithoutFlushing) {
  // Applying a bunch of small rows without a flush should result
  // in an error.
  {
    bool got_expected_error = false;
    auto session = CreateSession();
    for (int i = 0; i < 1000000; i++) {
      Status s = ApplyInsertToSession(session.get(), client_table_, 1, 1, "x");
      if (s.IsIncomplete()) {
        ASSERT_STR_CONTAINS(s.ToString(), "not enough space remaining in buffer");
        got_expected_error = true;
        break;
      } else {
        ASSERT_OK(s);
      }
    }
    ASSERT_TRUE(got_expected_error);
  }

  // Writing a single very large row should also result in an error.
  {
    string huge_string(10 * 1024 * 1024, 'x');

    shared_ptr<YBSession> session = client_->NewSession();
    Status s = ApplyInsertToSession(session.get(), client_table_, 1, 1, huge_string.c_str());
    ASSERT_TRUE(s.IsIncomplete()) << "got unexpected status: " << s.ToString();
  }
}

// Test that update updates and delete deletes with expected use
TEST_F(ClientTest, TestMutationsWork) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "original row"));
  FlushSessionOrDie(session);

  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 2));
  FlushSessionOrDie(session);
  auto rows = ScanTableToStrings(client_table_);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("{ int32:1, int32:2, string:\"original row\", null }", rows[0]);
  rows.clear();

  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_, &rows);
  ASSERT_EQ(0, rows.size());
}

TEST_F(ClientTest, TestMutateDeletedRow) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "original row"));
  FlushSessionOrDie(session);
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  FlushSessionOrDie(session);
  auto rows = ScanTableToStrings(client_table_);
  ASSERT_EQ(0, rows.size());

  // Attempt update deleted row
  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 2));
  Status s = session->Flush();
  ASSERT_TRUE(s.ok());
  ScanTableToStrings(client_table_, &rows);
  ASSERT_EQ(1, rows.size());

  // Attempt delete deleted row
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  s = session->Flush();
  ASSERT_TRUE(s.ok());
  ScanTableToStrings(client_table_, &rows);
  ASSERT_EQ(0, rows.size());
}

TEST_F(ClientTest, TestMutateNonexistentRow) {
  auto session = CreateSession();

  // Attempt update nonexistent row
  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 2));
  Status s = session->Flush();
  ASSERT_TRUE(s.ok());
  auto rows = ScanTableToStrings(client_table_);
  ASSERT_EQ(1, rows.size());

  // Attempt delete nonexistent row
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  s = session->Flush();
  ASSERT_TRUE(s.ok());
  ScanTableToStrings(client_table_, &rows);
  ASSERT_EQ(0, rows.size());
}

// Do a write with a bad schema on the client side. This should make the Prepare
// phase of the write fail, which will result in an error on the RPC response.
TEST_F(ClientTest, TestWriteWithBadSchema) {
  // Remove the 'int_val' column.
  // Now the schema on the client is "old"
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("int_val")->Alter());

  // Try to do a write with the bad schema.
  auto session = CreateSession();
  std::shared_ptr<YBqlOp> op;
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 12345, 12345, "x", &op));
  ASSERT_OK(session->Flush());
  ASSERT_EQ(QLResponsePB::YQL_STATUS_SCHEMA_VERSION_MISMATCH, op->response().status());
}

TEST_F(ClientTest, TestBasicAlterOperations) {
  // test that having no steps throws an error
  {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    Status s = table_alterer->Alter();
    ASSERT_TRUE(s.IsInvalidArgument());
    ASSERT_STR_CONTAINS(s.ToString(), "No alter steps provided");
  }

  // test that remove key should throws an error
  {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    Status s = table_alterer
      ->DropColumn("key")
      ->Alter();
    ASSERT_TRUE(s.IsInvalidArgument());
    ASSERT_STR_CONTAINS(s.ToString(), "cannot remove a key column");
  }

  // test that renaming to an already-existing name throws an error
  {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    table_alterer->AlterColumn("int_val")->RenameTo("string_val");
    Status s = table_alterer->Alter();
    ASSERT_TRUE(s.IsAlreadyPresent());
    ASSERT_STR_CONTAINS(s.ToString(), "The column already exists: string_val");
  }

  // Need a tablet peer for the next set of tests.
  string tablet_id = GetFirstTabletId(client_table_.get());
  std::shared_ptr<TabletPeer> tablet_peer;

  for (auto& ts : cluster_->mini_tablet_servers()) {
    ASSERT_TRUE(ts->server()->tablet_manager()->LookupTablet(tablet_id, &tablet_peer));
    if (tablet_peer->LeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY) {
      break;
    }
  }

  {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    table_alterer->DropColumn("int_val")
      ->AddColumn("new_col")->Type(INT32);
    ASSERT_OK(table_alterer->Alter());
    // TODO(nspiegelberg): The below assert is flakey because of KUDU-1539.
    ASSERT_EQ(1, tablet_peer->tablet()->metadata()->schema_version());
  }

  {
    const YBTableName kRenamedTableName(YQL_DATABASE_CQL, kKeyspaceName, "RenamedTable");
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    ASSERT_OK(table_alterer
              ->RenameTo(kRenamedTableName)
              ->Alter());
    // TODO(nspiegelberg): The below assert is flakey because of KUDU-1539.
    ASSERT_EQ(2, tablet_peer->tablet()->metadata()->schema_version());
    ASSERT_EQ(kRenamedTableName.table_name(), tablet_peer->tablet()->metadata()->table_name());

    const auto tables = ASSERT_RESULT(client_->ListTables());
    ASSERT_TRUE(::util::gtl::contains(tables.begin(), tables.end(), kRenamedTableName));
    ASSERT_FALSE(::util::gtl::contains(tables.begin(), tables.end(), kTableName));
  }
}

TEST_F(ClientTest, TestDeleteTable) {
  // Open the table before deleting it.
  ASSERT_OK(client_table_.Open(kTableName, client_.get()));

  // Insert a few rows, and scan them back. This is to populate the MetaCache.
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 10));
  auto rows = ScanTableToStrings(client_table_);
  ASSERT_EQ(10, rows.size());

  // Remove the table
  // NOTE that it returns when the operation is completed on the master side
  string tablet_id = GetFirstTabletId(client_table_.get());
  ASSERT_OK(client_->DeleteTable(kTableName));
  const auto tables = ASSERT_RESULT(client_->ListTables());
  ASSERT_FALSE(::util::gtl::contains(tables.begin(), tables.end(), kTableName));

  // Wait until the table is removed from the TS
  int wait_time = 1000;
  bool tablet_found = true;
  for (int i = 0; i < 80 && tablet_found; ++i) {
    std::shared_ptr<TabletPeer> tablet_peer;
    tablet_found = cluster_->mini_tablet_server(0)->server()->tablet_manager()->LookupTablet(
                      tablet_id, &tablet_peer);
    SleepFor(MonoDelta::FromMicroseconds(wait_time));
    wait_time = std::min(wait_time * 5 / 4, 1000000);
  }
  ASSERT_FALSE(tablet_found);

  // Try to open the deleted table
  Status s = client_table_.Open(kTableName, client_.get());
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), "The object does not exist");

  // Create a new table with the same name. This is to ensure that the client
  // doesn't cache anything inappropriately by table name (see KUDU-1055).
  ASSERT_NO_FATALS(CreateTable(kTableName, kNumTablets, &client_table_));

  // Should be able to insert successfully into the new table.
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 10));
}

TEST_F(ClientTest, TestGetTableSchema) {
  YBSchema schema;
  PartitionSchema partition_schema;

  // Verify the schema for the current table
  ASSERT_OK(client_->GetTableSchema(kTableName, &schema, &partition_schema));
  ASSERT_TRUE(schema_.Equals(schema));

  // Verify that a get schema request for a missing table throws not found
  Status s = client_->GetTableSchema(
      YBTableName(YQL_DATABASE_CQL, kKeyspaceName, "MissingTableName"), &schema, &partition_schema);
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), "The object does not exist");
}

TEST_F(ClientTest, TestGetTableSchemaByIdAsync) {
  Synchronizer sync;
  auto table_info = std::make_shared<YBTableInfo>();
  ASSERT_OK(client_->GetTableSchemaById(
      client_table_.table()->id(), table_info, sync.AsStatusCallback()));
  ASSERT_OK(sync.Wait());
  ASSERT_TRUE(schema_.Equals(table_info->schema));
}

TEST_F(ClientTest, TestGetTableSchemaByIdMissingTable) {
  // Verify that a get schema request for a missing table throws not found.
  Synchronizer sync;
  auto table_info = std::make_shared<YBTableInfo>();
  ASSERT_OK(client_->GetTableSchemaById("MissingTableId", table_info, sync.AsStatusCallback()));
  Status s = sync.Wait();
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), "The object does not exist");
}

void CreateCDCStreamCallbackSuccess(Synchronizer* sync, const Result<CDCStreamId>& stream) {
  ASSERT_TRUE(stream.ok());
  ASSERT_FALSE(stream->empty());
  sync->StatusCB(Status::OK());
}

void CreateCDCStreamCallbackFailure(Synchronizer* sync, const Result<CDCStreamId>& stream) {
  ASSERT_FALSE(stream.ok());
  sync->StatusCB(stream.status());
}

TEST_F(ClientTest, TestCreateCDCStreamAsync) {
  Synchronizer sync;
  std::unordered_map<std::string, std::string> options;
  client_->CreateCDCStream(
      client_table_.table()->id(), options,
      std::bind(&CreateCDCStreamCallbackSuccess, &sync, std::placeholders::_1));
  ASSERT_OK(sync.Wait());
}

TEST_F(ClientTest, TestCreateCDCStreamMissingTable) {
  Synchronizer sync;
  std::unordered_map<std::string, std::string> options;
  client_->CreateCDCStream(
      "MissingTableId", options,
      std::bind(&CreateCDCStreamCallbackFailure, &sync, std::placeholders::_1));
  Status s = sync.Wait();
  ASSERT_TRUE(s.IsNotFound());
}

TEST_F(ClientTest, TestDeleteCDCStreamAsync) {
  std::unordered_map<std::string, std::string> options;
  auto result = client_->CreateCDCStream(client_table_.table()->id(), options);
  ASSERT_TRUE(result.ok());

  // Delete the created CDC stream.
  Synchronizer sync;
  client_->DeleteCDCStream(*result, sync.AsStatusCallback());
  ASSERT_OK(sync.Wait());
}

TEST_F(ClientTest, TestDeleteCDCStreamMissingId) {
  // Try to delete a non-existent CDC stream.
  Synchronizer sync;
  client_->DeleteCDCStream("MissingStreamId", sync.AsStatusCallback());
  Status s = sync.Wait();
  ASSERT_TRUE(s.IsNotFound());
}

TEST_F(ClientTest, TestStaleLocations) {
  string tablet_id = GetFirstTabletId(client_table2_.get());

  // The Tablet is up and running the location should not be stale
  master::TabletLocationsPB locs_pb;
  ASSERT_OK(cluster_->mini_master()->master()->catalog_manager()->GetTabletLocations(
                  tablet_id, &locs_pb));
  ASSERT_FALSE(locs_pb.stale());

  // On Master restart and no tablet report we expect the locations to be stale
  for (int i = 0; i < cluster_->num_tablet_servers(); ++i) {
    cluster_->mini_tablet_server(i)->Shutdown();
  }
  ASSERT_OK(cluster_->mini_master()->Restart());
  ASSERT_OK(cluster_->mini_master()->master()->WaitUntilCatalogManagerIsLeaderAndReadyForTests());
  locs_pb.Clear();
  ASSERT_OK(cluster_->mini_master()->master()->catalog_manager()->GetTabletLocations(
                  tablet_id, &locs_pb));
  ASSERT_TRUE(locs_pb.stale());

  // Restart the TS and Wait for the tablets to be reported to the master.
  for (int i = 0; i < cluster_->num_tablet_servers(); ++i) {
    ASSERT_OK(cluster_->mini_tablet_server(i)->Start());
  }
  ASSERT_OK(cluster_->WaitForTabletServerCount(cluster_->num_tablet_servers()));
  locs_pb.Clear();
  ASSERT_OK(cluster_->mini_master()->master()->catalog_manager()->GetTabletLocations(
                  tablet_id, &locs_pb));

  // It may take a while to bootstrap the tablet and send the location report
  // so spin until we get a non-stale location.
  int wait_time = 1000;
  for (int i = 0; i < 80; ++i) {
    locs_pb.Clear();
    ASSERT_OK(cluster_->mini_master()->master()->catalog_manager()->GetTabletLocations(
                    tablet_id, &locs_pb));
    if (!locs_pb.stale()) {
      break;
    }
    SleepFor(MonoDelta::FromMicroseconds(wait_time));
    wait_time = std::min(wait_time * 5 / 4, 1000000);
  }
  ASSERT_FALSE(locs_pb.stale());
}

// Test creating and accessing a table which has multiple tablets,
// each of which is replicated.
//
// TODO: this should probably be the default for _all_ of the tests
// in this file. However, some things like alter table are not yet
// working on replicated tables - see KUDU-304
TEST_F(ClientTest, TestReplicatedMultiTabletTable) {
  const YBTableName kReplicatedTable(YQL_DATABASE_CQL, "replicated");
  const int kNumRowsToWrite = 100;

  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kReplicatedTable,
                               kNumTablets,
                               &table));

  // Should have no rows to begin with.
  ASSERT_EQ(0, CountRowsFromClient(table));

  // Insert some data.
  ASSERT_NO_FATALS(InsertTestRows(table, kNumRowsToWrite));

  // Should now see the data.
  ASSERT_EQ(kNumRowsToWrite, CountRowsFromClient(table));

  // TODO: once leader re-election is in, should somehow force a re-election
  // and ensure that the client handles refreshing the leader.
}

TEST_F(ClientTest, TestReplicatedMultiTabletTableFailover) {
  const YBTableName kReplicatedTable(YQL_DATABASE_CQL, "replicated_failover_on_reads");
  const int kNumRowsToWrite = 100;
  const int kNumTries = 100;

  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kReplicatedTable,
                               kNumTablets,
                               &table));

  // Insert some data.
  ASSERT_NO_FATALS(InsertTestRows(table, kNumRowsToWrite));

  // Find the leader of the first tablet.
  auto remote_tablet = ASSERT_RESULT(LookupFirstTabletFuture(table.table()).get());
  internal::RemoteTabletServer *remote_tablet_server = remote_tablet->LeaderTServer();

  // Kill the leader of the first tablet.
  ASSERT_OK(KillTServer(remote_tablet_server->permanent_uuid()));

  // We wait until we fail over to the new leader(s).
  int tries = 0;
  for (;;) {
    tries++;
    int num_rows = CountRowsFromClient(table);
    if (num_rows == kNumRowsToWrite) {
      LOG(INFO) << "Found expected number of rows: " << num_rows;
      break;
    } else {
      LOG(INFO) << "Only found " << num_rows << " rows on try "
                << tries << ", retrying";
      ASSERT_LE(tries, kNumTries);
      SleepFor(MonoDelta::FromMilliseconds(10 * tries)); // sleep a bit more with each attempt.
    }
  }
}

// This test that we can keep writing to a tablet when the leader
// tablet dies.
// This currently forces leader promotion through RPC and creates
// a new client afterwards.
// TODO Remove the leader promotion part when we have automated
// leader election.
TEST_F(ClientTest, TestReplicatedTabletWritesAndAltersWithLeaderElection) {
  const YBTableName kReplicatedTable(YQL_DATABASE_CQL, kKeyspaceName,
     "replicated_failover_on_writes");
  const int kNumRowsToWrite = 100;

  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kReplicatedTable,
                               1,
                               &table));

  // Insert some data.
  ASSERT_NO_FATALS(InsertTestRows(table, kNumRowsToWrite));

  // TODO: we have to sleep here to make sure that the leader has time to
  // propagate the writes to the followers. We can remove this once the
  // followers run a leader election on their own and handle advancing
  // the commit index.
  SleepFor(MonoDelta::FromMilliseconds(1500));

  // Find the leader replica
  auto remote_tablet = ASSERT_RESULT(LookupFirstTabletFuture(table.table()).get());
  internal::RemoteTabletServer *remote_tablet_server;
  set<string> blacklist;
  vector<internal::RemoteTabletServer*> candidates;
  ASSERT_OK(client_->data_->GetTabletServer(client_.get(),
                                            remote_tablet,
                                            YBClient::LEADER_ONLY,
                                            blacklist,
                                            &candidates,
                                            &remote_tablet_server));

  string killed_uuid = remote_tablet_server->permanent_uuid();
  // Kill the tserver that is serving the leader tablet.
  ASSERT_OK(KillTServer(killed_uuid));

  // Since we waited before, hopefully all replicas will be up to date
  // and we can just promote another replica.
  auto client_messenger = rpc::CreateAutoShutdownMessengerHolder(
      ASSERT_RESULT(CreateMessenger("client")));
  int new_leader_idx = -1;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    MiniTabletServer* ts = cluster_->mini_tablet_server(i);
    LOG(INFO) << "GOT TS " << i << " WITH UUID ???";
    if (ts->is_started()) {
      const string& uuid = ts->server()->instance_pb().permanent_uuid();
      LOG(INFO) << uuid;
      if (uuid != killed_uuid) {
        new_leader_idx = i;
        break;
      }
    }
  }
  ASSERT_NE(-1, new_leader_idx);

  MiniTabletServer* new_leader = cluster_->mini_tablet_server(new_leader_idx);
  ASSERT_TRUE(new_leader != nullptr);
  rpc::ProxyCache proxy_cache(client_messenger.get());
  consensus::ConsensusServiceProxy new_leader_proxy(
      &proxy_cache, HostPort::FromBoundEndpoint(new_leader->bound_rpc_addr()));

  consensus::RunLeaderElectionRequestPB req;
  consensus::RunLeaderElectionResponsePB resp;
  rpc::RpcController controller;

  LOG(INFO) << "Promoting server at index " << new_leader_idx << " listening at "
            << new_leader->bound_rpc_addr() << " ...";
  req.set_dest_uuid(new_leader->server()->fs_manager()->uuid());
  req.set_tablet_id(remote_tablet->tablet_id());
  ASSERT_OK(new_leader_proxy.RunLeaderElection(req, &resp, &controller));
  ASSERT_FALSE(resp.has_error()) << "Got error. Response: " << resp.ShortDebugString();

  LOG(INFO) << "Inserting additional rows...";
  ASSERT_NO_FATALS(InsertTestRows(table,
                                  kNumRowsToWrite,
                                  kNumRowsToWrite));

  // TODO: we have to sleep here to make sure that the leader has time to
  // propagate the writes to the followers. We can remove this once the
  // followers run a leader election on their own and handle advancing
  // the commit index.
  SleepFor(MonoDelta::FromMilliseconds(1500));

  LOG(INFO) << "Counting rows...";
  ASSERT_EQ(2 * kNumRowsToWrite, CountRowsFromClient(table,
                                                     YBConsistencyLevel::CONSISTENT_PREFIX,
                                                     kNoBound, kNoBound));

  // Test altering the table metadata and ensure that meta operations are resilient as well.
  {
    std::shared_ptr<TabletPeer> tablet_peer;
    ASSERT_TRUE(new_leader->server()->tablet_manager()->LookupTablet(remote_tablet->tablet_id(),
        &tablet_peer));
    auto old_version = tablet_peer->tablet()->metadata()->schema_version();
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kReplicatedTable));
    table_alterer->AddColumn("new_col")->Type(INT32);
    ASSERT_OK(table_alterer->Alter());
    ASSERT_EQ(old_version + 1, tablet_peer->tablet()->metadata()->schema_version());
  }
}

namespace {

void CheckCorrectness(const TableHandle& table, int expected[], int nrows) {
  int readrows = 0;

  for (const auto& row : TableRange(table)) {
    ASSERT_LE(readrows, nrows);
    int32_t key = row.column(0).int32_value();
    ASSERT_NE(key, -1) << "Deleted key found in table in table " << key;
    ASSERT_EQ(expected[key], row.column(1).int32_value())
        << "Incorrect int value for key " <<  key;
    ASSERT_EQ(row.column(2).string_value(), "")
        << "Incorrect string value for key " << key;
    ++readrows;
  }
  ASSERT_EQ(readrows, nrows);
}

} // anonymous namespace

// Randomized mutations accuracy testing
TEST_F(ClientTest, TestRandomWriteOperation) {
  auto session = CreateSession();
  int row[FLAGS_test_scan_num_rows]; // -1 indicates empty
  int nrows;

  // First half-fill
  for (int i = 0; i < FLAGS_test_scan_num_rows/2; ++i) {
    ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, i, i, ""));
    row[i] = i;
  }
  for (int i = FLAGS_test_scan_num_rows/2; i < FLAGS_test_scan_num_rows; ++i) {
    row[i] = -1;
  }
  nrows = FLAGS_test_scan_num_rows/2;

  // Randomized testing
  LOG(INFO) << "Randomized mutations testing.";
  unsigned int seed = SeedRandom();
  for (int i = 0; i <= 1000; ++i) {
    // Test correctness every so often
    if (i % 50 == 0) {
      LOG(INFO) << "Correctness test " << i;
      FlushSessionOrDie(session);
      ASSERT_NO_FATALS(CheckCorrectness(client_table_, row, nrows));
      LOG(INFO) << "...complete";
    }

    int change = rand_r(&seed) % FLAGS_test_scan_num_rows;
    // Insert if empty
    if (row[change] == -1) {
      ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, change, change, ""));
      row[change] = change;
      ++nrows;
      VLOG(1) << "Insert " << change;
    } else {
      // Update or delete otherwise
      int update = rand_r(&seed) & 1;
      if (update) {
        ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, change, ++row[change]));
        VLOG(1) << "Update " << change;
      } else {
        ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, change));
        row[change] = -1;
        --nrows;
        VLOG(1) << "Delete " << change;
      }
    }
  }

  // And one more time for the last batch.
  FlushSessionOrDie(session);
  ASSERT_NO_FATALS(CheckCorrectness(client_table_, row, nrows));
}

// Test whether a batch can handle several mutations in a batch
TEST_F(ClientTest, TestSeveralRowMutatesPerBatch) {
  auto session = CreateSession();

  // Test insert/update
  LOG(INFO) << "Testing insert/update in same batch, key " << 1 << ".";
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, ""));
  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 2));
  FlushSessionOrDie(session);
  auto rows = ScanTableToStrings(client_table_);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("{ int32:1, int32:2, string:\"\", null }", rows[0]);
  rows.clear();


  LOG(INFO) << "Testing insert/delete in same batch, key " << 2 << ".";
  // Test insert/delete
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 2, 1, ""));
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 2));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_, &rows);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("{ int32:1, int32:2, string:\"\", null }", rows[0]);
  rows.clear();

  // Test update/delete
  LOG(INFO) << "Testing update/delete in same batch, key " << 1 << ".";
  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 1));
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_, &rows);
  ASSERT_EQ(0, rows.size());

  // Test delete/insert (insert a row first)
  LOG(INFO) << "Inserting row for delete/insert test, key " << 1 << ".";
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, ""));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_, &rows);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("{ int32:1, int32:1, string:\"\", null }", rows[0]);
  rows.clear();
  LOG(INFO) << "Testing delete/insert in same batch, key " << 1 << ".";
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 2, ""));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_, &rows);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("{ int32:1, int32:2, string:\"\", null }", rows[0]);
  rows.clear();
}

// Tests that master permits are properly released after a whole bunch of
// rows are inserted.
TEST_F(ClientTest, TestMasterLookupPermits) {
  int initial_value = client_->data_->meta_cache_->master_lookup_sem_.GetValue();
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));
  ASSERT_EQ(initial_value,
            client_->data_->meta_cache_->master_lookup_sem_.GetValue());
}

// Define callback for deadlock simulation, as well as various helper methods.
namespace {

class DeadlockSimulationCallback {
 public:
  explicit DeadlockSimulationCallback(Atomic32* i) : i_(i) {}

  void operator()(const Status& s) const {
    CHECK_OK(s);
    NoBarrier_AtomicIncrement(i_, 1);
  }
 private:
  Atomic32* const i_;
};

// Returns col1 value of first row.
int32_t ReadFirstRowKeyFirstCol(const TableHandle& tbl) {
  TableRange range(tbl);

  auto it = range.begin();
  EXPECT_NE(it, range.end());
  return it->column(1).int32_value();
}

// Checks that all rows have value equal to expected, return number of rows.
int CheckRowsEqual(const TableHandle& tbl, int32_t expected) {
  int cnt = 0;
  for (const auto& row : TableRange(tbl)) {
    EXPECT_EQ(row.column(1).int32_value(), expected);
    EXPECT_EQ(row.column(2).string_value(), "");
    EXPECT_EQ(row.column(3).int32_value(), 12345);
    ++cnt;
  }
  return cnt;
}

// Return a session "loaded" with updates. Sets the session timeout
// to the parameter value. Larger timeouts decrease false positives.
shared_ptr<YBSession> LoadedSession(YBClient* client,
                                    const TableHandle& tbl,
                                    bool fwd, int max, MonoDelta timeout) {
  shared_ptr<YBSession> session = client->NewSession();
  session->SetTimeout(timeout);
  for (int i = 0; i < max; ++i) {
    int key = fwd ? i : max - i;
    CHECK_OK(ApplyUpdateToSession(session.get(), tbl, key, fwd));
  }
  return session;
}

} // anonymous namespace

// Starts many clients which update a table in parallel.
// Half of the clients update rows in ascending order while the other
// half update rows in descending order.
// This ensures that we don't hit a deadlock in such a situation.
TEST_F(ClientTest, TestDeadlockSimulation) {
  if (!AllowSlowTests()) {
    LOG(WARNING) << "TestDeadlockSimulation disabled since slow.";
    return;
  }

  // Make reverse client who will make batches that update rows
  // in reverse order. Separate client used so rpc calls come in at same time.
  auto rev_client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(ToString(cluster_->mini_master()->bound_rpc_addr()))
      .Build());
  TableHandle rev_table;
  ASSERT_OK(rev_table.Open(kTableName, client_.get()));

  // Load up some rows
  const int kNumRows = 300;
  const auto kTimeout = 60s;
  auto session = CreateSession();
  for (int i = 0; i < kNumRows; ++i)
    ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, i, i,  ""));
  FlushSessionOrDie(session);

  // Check both clients see rows
  int fwd = CountRowsFromClient(client_table_);
  ASSERT_EQ(kNumRows, fwd);
  int rev = CountRowsFromClient(rev_table);
  ASSERT_EQ(kNumRows, rev);

  // Generate sessions
  const int kNumSessions = 100;
  shared_ptr<YBSession> fwd_sessions[kNumSessions];
  shared_ptr<YBSession> rev_sessions[kNumSessions];
  for (int i = 0; i < kNumSessions; ++i) {
    fwd_sessions[i] = LoadedSession(client_.get(), client_table_, true, kNumRows, kTimeout);
    rev_sessions[i] = LoadedSession(rev_client.get(), rev_table, true, kNumRows, kTimeout);
  }

  // Run async calls - one thread updates sequentially, another in reverse.
  Atomic32 ctr1, ctr2;
  NoBarrier_Store(&ctr1, 0);
  NoBarrier_Store(&ctr2, 0);
  for (int i = 0; i < kNumSessions; ++i) {
    // The callbacks are freed after they are invoked.
    fwd_sessions[i]->FlushAsync(DeadlockSimulationCallback(&ctr1));
    rev_sessions[i]->FlushAsync(DeadlockSimulationCallback(&ctr2));
  }

  // Spin while waiting for ops to complete.
  int lctr1, lctr2, prev1 = 0, prev2 = 0;
  do {
    lctr1 = NoBarrier_Load(&ctr1);
    lctr2 = NoBarrier_Load(&ctr2);
    // Display progress in 10% increments.
    if (prev1 == 0 || lctr1 + lctr2 - prev1 - prev2 > kNumSessions / 10) {
      LOG(INFO) << "# updates: " << lctr1 << " fwd, " << lctr2 << " rev";
      prev1 = lctr1;
      prev2 = lctr2;
    }
    SleepFor(MonoDelta::FromMilliseconds(100));
  } while (lctr1 != kNumSessions|| lctr2 != kNumSessions);
  int32_t expected = ReadFirstRowKeyFirstCol(client_table_);

  // Check transaction from forward client.
  fwd = CheckRowsEqual(client_table_, expected);
  ASSERT_EQ(fwd, kNumRows);

  // Check from reverse client side.
  rev = CheckRowsEqual(rev_table, expected);
  ASSERT_EQ(rev, kNumRows);
}

TEST_F(ClientTest, TestCreateDuplicateTable) {
  std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_TRUE(table_creator->table_name(kTableName)
              .schema(&schema_)
              .Create().IsAlreadyPresent());
}

TEST_F(ClientTest, CreateTableWithoutTservers) {
  DoTearDown();

  YBMiniClusterTestBase::SetUp();

  MiniClusterOptions options;
  options.num_tablet_servers = 0;
  // Start minicluster with only master (to simulate tserver not yet heartbeating).
  cluster_.reset(new MiniCluster(env_.get(), options));
  ASSERT_OK(cluster_->Start());

  // Connect to the cluster.
  client_ = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(yb::ToString(cluster_->mini_master()->bound_rpc_addr()))
      .Build());

  std::unique_ptr<client::YBTableCreator> table_creator(client_->NewTableCreator());
  Status s = table_creator->table_name(YBTableName(YQL_DATABASE_CQL, kKeyspaceName, "foobar"))
      .schema(&schema_)
      .Create();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(), "num_tablets should be greater than 0.");
}

TEST_F(ClientTest, TestCreateTableWithTooManyTablets) {
  FLAGS_max_create_tablets_per_ts = 1;
  auto many_tablets = FLAGS_replication_factor + 1;

  std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  Status s = table_creator->table_name(YBTableName(YQL_DATABASE_CQL, kKeyspaceName, "foobar"))
      .schema(&schema_)
      .num_tablets(many_tablets)
      .Create();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(
      s.ToString(),
      strings::Substitute(
          "The requested number of tablets ($0) is over the permitted maximum ($1)", many_tablets,
          FLAGS_replication_factor));
}

// TODO(bogdan): Disabled until ENG-2687
TEST_F(ClientTest, DISABLED_TestCreateTableWithTooManyReplicas) {
  std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  Status s = table_creator->table_name(YBTableName(YQL_DATABASE_CQL, kKeyspaceName, "foobar"))
      .schema(&schema_)
      .num_tablets(2)
      .Create();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(),
                      "Not enough live tablet servers to create table with the requested "
                      "replication factor 3. 1 tablet servers are alive");
}

// Test that scanners will retry after receiving ERROR_SERVER_TOO_BUSY from an
// overloaded tablet server. Regression test for KUDU-1079.
TEST_F(ClientTest, TestServerTooBusyRetry) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  // Introduce latency in each scan to increase the likelihood of
  // ERROR_SERVER_TOO_BUSY.
  FLAGS_TEST_scanner_inject_latency_on_each_batch_ms = 10;

  // Reduce the service queue length of each tablet server in order to increase
  // the likelihood of ERROR_SERVER_TOO_BUSY.
  FLAGS_tablet_server_svc_queue_length = 1;
  // Set the backoff limits to be small for this test, so that we finish in a reasonable
  // amount of time.
  FLAGS_min_backoff_ms_exponent = 0;
  FLAGS_max_backoff_ms_exponent = 3;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    MiniTabletServer* ts = cluster_->mini_tablet_server(i);
    ASSERT_OK(ts->Restart());
    ASSERT_OK(ts->WaitStarted());
  }

  bool stop = false;
  vector<scoped_refptr<yb::Thread> > threads;
  int t = 0;
  while (!stop) {
    scoped_refptr<yb::Thread> thread;
    ASSERT_OK(yb::Thread::Create("test", strings::Substitute("t$0", t++),
                                 &CheckRowCount, std::cref(client_table_), &thread));
    threads.push_back(thread);

    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      scoped_refptr<Counter> counter = METRIC_rpcs_queue_overflow.Instantiate(
          cluster_->mini_tablet_server(i)->server()->metric_entity());
      stop = counter->value() > 0;
    }
  }

  for (const scoped_refptr<yb::Thread>& thread : threads) {
    thread->Join();
  }
}

TEST_F(ClientTest, TestReadFromFollower) {
  // Create table and write some rows.
  const YBTableName kReadFromFollowerTable(YQL_DATABASE_CQL, "TestReadFromFollower");
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kReadFromFollowerTable, 1, &table));
  ASSERT_NO_FATALS(InsertTestRows(table, FLAGS_test_scan_num_rows));

  // Find the followers.
  GetTableLocationsRequestPB req;
  GetTableLocationsResponsePB resp;
  table->name().SetIntoTableIdentifierPB(req.mutable_table());
  CHECK_OK(cluster_->mini_master()->master()->catalog_manager()->GetTableLocations(&req, &resp));
  ASSERT_EQ(1, resp.tablet_locations_size());
  ASSERT_EQ(3, resp.tablet_locations(0).replicas_size());
  const string& tablet_id = resp.tablet_locations(0).tablet_id();

  vector<master::TSInfoPB> followers;
  for (const auto& replica : resp.tablet_locations(0).replicas()) {
    if (replica.role() == consensus::RaftPeerPB_Role_FOLLOWER) {
      followers.push_back(replica.ts_info());
    }
  }
  ASSERT_EQ(cluster_->num_tablet_servers() - 1, followers.size());

  auto client_messenger =
      CreateAutoShutdownMessengerHolder(ASSERT_RESULT(CreateMessenger("client")));
  rpc::ProxyCache proxy_cache(client_messenger.get());
  for (const master::TSInfoPB& ts_info : followers) {
    // Try to read from followers.
    auto tserver_proxy = std::make_unique<tserver::TabletServerServiceProxy>(
        &proxy_cache, HostPortFromPB(ts_info.private_rpc_addresses(0)));

    std::unique_ptr<QLRowBlock> row_block;
    ASSERT_OK(WaitFor([&]() -> bool {
      // Setup read request.
      tserver::ReadRequestPB req;
      tserver::ReadResponsePB resp;
      rpc::RpcController controller;
      req.set_tablet_id(tablet_id);
      req.set_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
      QLReadRequestPB *ql_read = req.mutable_ql_batch()->Add();
      std::shared_ptr<std::vector<ColumnSchema>> selected_cols =
          std::make_shared<std::vector<ColumnSchema>>(schema_.columns());
      QLRSRowDescPB *rsrow_desc = ql_read->mutable_rsrow_desc();
      for (int i = 0; i < schema_.num_columns(); i++) {
        ql_read->add_selected_exprs()->set_column_id(yb::kFirstColumnId + i);
        ql_read->mutable_column_refs()->add_ids(yb::kFirstColumnId + i);

        QLRSColDescPB *rscol_desc = rsrow_desc->add_rscol_descs();
        rscol_desc->set_name((*selected_cols)[i].name());
        (*selected_cols)[i].type()->ToQLTypePB(rscol_desc->mutable_ql_type());
      }

      EXPECT_OK(tserver_proxy->Read(req, &resp, &controller));

      // Verify response.
      EXPECT_FALSE(resp.has_error());
      EXPECT_EQ(1, resp.ql_batch_size());
      const QLResponsePB &ql_resp = resp.ql_batch(0);
      EXPECT_EQ(QLResponsePB_QLStatus_YQL_STATUS_OK, ql_resp.status());
      EXPECT_TRUE(ql_resp.has_rows_data_sidecar());

      EXPECT_TRUE(controller.finished());
      Slice rows_data = EXPECT_RESULT(controller.GetSidecar(ql_resp.rows_data_sidecar()));
      yb::ql::RowsResult rowsResult(kReadFromFollowerTable, selected_cols, rows_data.ToBuffer());
      row_block = rowsResult.GetRowBlock();
      return FLAGS_test_scan_num_rows == row_block->row_count();
    }, MonoDelta::FromSeconds(30), "Waiting for replication to followers"));

    std::vector<bool> seen_key(row_block->row_count());
    for (int i = 0; i < row_block->row_count(); i++) {
      const QLRow& row = row_block->row(i);
      auto key = row.column(0).int32_value();
      ASSERT_LT(key, seen_key.size());
      ASSERT_FALSE(seen_key[key]);
      seen_key[key] = true;
      ASSERT_EQ(key * 2, row.column(1).int32_value());
      ASSERT_EQ(StringPrintf("hello %d", key), row.column(2).string_value());
      ASSERT_EQ(key * 3, row.column(3).int32_value());
    }
  }
}

TEST_F(ClientTest, Capability) {
  constexpr CapabilityId kFakeCapability = 0x9c40e9a7;

  auto rt = ASSERT_RESULT(LookupFirstTabletFuture(client_table_.table()).get());
  ASSERT_TRUE(rt.get() != nullptr);
  auto tservers = rt->GetRemoteTabletServers();
  ASSERT_EQ(tservers.size(), 3);
  for (const auto& replica : tservers) {
    // Capability is related to executable, so it should be present since we run mini cluster for
    // this test.
    ASSERT_TRUE(replica->HasCapability(CAPABILITY_ClientTest));

    // Check that fake capability is not reported.
    ASSERT_FALSE(replica->HasCapability(kFakeCapability));

    // This capability is defined on the TServer, passed to the Master on registration,
    // then propagated to the YBClient.  Ensure that this runtime pipeline holds.
    ASSERT_TRUE(replica->HasCapability(CAPABILITY_TabletReportLimit));
  }
}

TEST_F(ClientTest, TestCreateTableWithRangePartition) {
  std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  const std::string kPgsqlKeyspaceID = "1234";
  const std::string kPgsqlKeyspaceName = "psql" + kKeyspaceName;
  const std::string kPgsqlTableName = "pgsqlrangepartitionedtable";
  const std::string kPgsqlTableId = "pgsqlrangepartitionedtableid";
  const size_t kColIdx = 1;
  const int64_t kKeyValue = 48238;
  auto pgsql_table_name = YBTableName(
      YQL_DATABASE_PGSQL, kPgsqlKeyspaceID, kPgsqlKeyspaceName, kPgsqlTableName);

  auto yql_table_name = YBTableName(YQL_DATABASE_CQL, kKeyspaceName, "yqlrangepartitionedtable");

  YBSchemaBuilder schemaBuilder;
  schemaBuilder.AddColumn("key")->PrimaryKey()->Type(yb::STRING)->NotNull();
  schemaBuilder.AddColumn("value")->Type(yb::INT64)->NotNull();
  YBSchema schema;
  EXPECT_OK(client_->CreateNamespaceIfNotExists(kPgsqlKeyspaceName,
                                                YQLDatabase::YQL_DATABASE_PGSQL,
                                                "" /* creator_role_name */,
                                                kPgsqlKeyspaceID));
  // Create a PGSQL table using range partition.
  EXPECT_OK(schemaBuilder.Build(&schema));
  Status s = table_creator->table_name(pgsql_table_name)
      .table_id(kPgsqlTableId)
      .schema(&schema)
      .set_range_partition_columns({"key"})
      .table_type(PGSQL_TABLE_TYPE)
      .num_tablets(1)
      .Create();
  EXPECT_OK(s);

  // Write to the PGSQL table.
  shared_ptr<YBTable> pgsq_table;
  EXPECT_OK(client_->OpenTable(kPgsqlTableId , &pgsq_table));
  std::shared_ptr<YBPgsqlWriteOp> pgsql_write_op(pgsq_table->NewPgsqlInsert());
  PgsqlWriteRequestPB* psql_write_request = pgsql_write_op->mutable_request();

  psql_write_request->add_range_column_values()->mutable_value()->set_string_value("pgsql_key1");
  PgsqlColumnValuePB* pgsql_column = psql_write_request->add_column_values();
  // 1 is the index for column value.

  pgsql_column->set_column_id(pgsq_table->schema().ColumnId(kColIdx));
  pgsql_column->mutable_expr()->mutable_value()->set_int64_value(kKeyValue);
  std::shared_ptr<YBSession> session = CreateSession(client_.get());
  EXPECT_OK(session->Apply(pgsql_write_op));

  // Create a YQL table using range partition.
  s = table_creator->table_name(yql_table_name)
      .schema(&schema)
      .set_range_partition_columns({"key"})
      .table_type(YQL_TABLE_TYPE)
      .num_tablets(1)
      .Create();
  EXPECT_OK(s);

  // Write to the YQL table.
  client::TableHandle table;
  EXPECT_OK(table.Open(yql_table_name, client_.get()));
  std::shared_ptr<YBqlWriteOp> write_op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
  QLWriteRequestPB* const req = write_op->mutable_request();
  req->add_range_column_values()->mutable_value()->set_string_value("key1");
  QLColumnValuePB* column = req->add_column_values();
  // 1 is the index for column value.
  column->set_column_id(pgsq_table->schema().ColumnId(kColIdx));
  column->mutable_expr()->mutable_value()->set_int64_value(kKeyValue);
  EXPECT_OK(session->Apply(write_op));
}

// TODO(jason): enable the test in clang when we use clang version at least 9 (otherwise, there is a
// compilation error: P0428R2).
#if !defined(__clang__)
TEST_F(ClientTest, FlushTable) {
  const tablet::Tablet* tablet;
  constexpr int kTimeoutSecs = 30;
  int current_row = 0;

  {
    std::shared_ptr<TabletPeer> tablet_peer;
    string tablet_id = GetFirstTabletId(client_table2_.get());
    for (auto& ts : cluster_->mini_tablet_servers()) {
      ASSERT_TRUE(ts->server()->tablet_manager()->LookupTablet(tablet_id, &tablet_peer));
      if (tablet_peer->LeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY) {
        break;
      }
    }
    tablet = tablet_peer->tablet();
  }

  auto test_good_flush_and_compact = ([&]<class T>(T table_id_or_name) {
    int initial_num_sst_files = tablet->GetCurrentVersionNumSSTFiles();

    // Test flush table.
    InsertTestRows(client_table2_, 1, current_row++);
    ASSERT_EQ(tablet->GetCurrentVersionNumSSTFiles(), initial_num_sst_files);
    ASSERT_OK(client_->FlushTables(
        {table_id_or_name}, /* add_indexes */ false, kTimeoutSecs, false /* is_compaction */));
    ASSERT_EQ(tablet->GetCurrentVersionNumSSTFiles(), initial_num_sst_files + 1);

    // Insert and flush more rows.
    InsertTestRows(client_table2_, 1, current_row++);
    ASSERT_OK(client_->FlushTables(
        {table_id_or_name}, /* add_indexes */ false, kTimeoutSecs, false /* is_compaction */));
    InsertTestRows(client_table2_, 1, current_row++);
    ASSERT_OK(client_->FlushTables(
        {table_id_or_name}, /* add_indexes */ false, kTimeoutSecs, false /* is_compaction */));

    // Test compact table.
    ASSERT_EQ(tablet->GetCurrentVersionNumSSTFiles(), initial_num_sst_files + 3);
    ASSERT_OK(client_->FlushTables(
        {table_id_or_name}, /* add_indexes */ false, kTimeoutSecs, true /* is_compaction */));
    ASSERT_EQ(tablet->GetCurrentVersionNumSSTFiles(), 1);
  });

  test_good_flush_and_compact(client_table2_.table()->id());
  test_good_flush_and_compact(client_table2_.table()->name());

  auto test_bad_flush_and_compact = ([&]<class T>(T table_id_or_name) {
    // Test flush table.
    ASSERT_NOK(client_->FlushTables(
        {table_id_or_name}, /* add_indexes */ false, kTimeoutSecs, false /* is_compaction */));
    // Test compact table.
    ASSERT_NOK(client_->FlushTables(
        {table_id_or_name}, /* add_indexes */ false, kTimeoutSecs, true /* is_compaction */));
  });

  test_bad_flush_and_compact("bad table id");
  test_bad_flush_and_compact(YBTableName(
      YQLDatabase::YQL_DATABASE_CQL,
      "bad namespace name",
      "bad table name"));
}
#endif  // !defined(__clang__)

TEST_F(ClientTest, GetNamespaceInfo) {
  const std::string kPgsqlKeyspaceID = "1234";
  const std::string kPgsqlKeyspaceName = "psql" + kKeyspaceName;
  GetNamespaceInfoResponsePB resp;

  // Setup.
  ASSERT_OK(client_->CreateNamespace(kPgsqlKeyspaceName,
                                     YQLDatabase::YQL_DATABASE_PGSQL,
                                     "" /* creator_role_name */,
                                     kPgsqlKeyspaceID,
                                     "" /* source_namespace_id */,
                                     boost::none /* next_pg_oid */,
                                     boost::none /* txn */,
                                     true /* colocated */));

  // CQL non-colocated.
  ASSERT_OK(client_->GetNamespaceInfo(
        "" /* namespace_id */, kKeyspaceName, YQL_DATABASE_CQL, &resp));
  ASSERT_EQ(resp.namespace_().name(), kKeyspaceName);
  ASSERT_EQ(resp.namespace_().database_type(), YQL_DATABASE_CQL);
  ASSERT_FALSE(resp.colocated());

  // SQL colocated.
  ASSERT_OK(client_->GetNamespaceInfo(
        kPgsqlKeyspaceID, "" /* namespace_name */, YQL_DATABASE_PGSQL, &resp));
  ASSERT_EQ(resp.namespace_().id(), kPgsqlKeyspaceID);
  ASSERT_EQ(resp.namespace_().name(), kPgsqlKeyspaceName);
  ASSERT_EQ(resp.namespace_().database_type(), YQL_DATABASE_PGSQL);
  ASSERT_TRUE(resp.colocated());
}

TEST_F(ClientTest, BadMasterAddress) {
  auto messenger = ASSERT_RESULT(CreateMessenger("test-messenger"));
  auto host = "should.not.resolve";

  // Put host entry in cache.
  ASSERT_NOK(messenger->resolver().Resolve(host));

  {
    struct TestServerOptions : public server::ServerBaseOptions {
      TestServerOptions() : server::ServerBaseOptions(1) {}
    };
    TestServerOptions opts;
    auto master_addr = std::make_shared<server::MasterAddresses>();
    // Put several hosts, so resolve would take place.
    master_addr->push_back({HostPort(host, 1)});
    master_addr->push_back({HostPort(host, 2)});
    opts.SetMasterAddresses(master_addr);

    AsyncClientInitialiser async_init(
        "test-client", /* num_reactors= */ 1, /* timeout_seconds= */ 1, "UUID", &opts,
        /* metric_entity= */ nullptr, /* parent_mem_tracker= */ nullptr, messenger.get());
    async_init.Start();
    async_init.get_client_future().wait_for(1s);
  }

  messenger->Shutdown();
}

TEST_F(ClientTest, RefreshPartitions) {
  const auto kLookupTimeout = 10s;
  const auto kNumLookupThreads = 2;
  const auto kNumLookups = 100;

  std::atomic<bool> stop_requested{false};
  std::atomic<size_t> num_lookups_called{0};
  std::atomic<size_t> num_lookups_done{0};

  const auto callback = [&num_lookups_done](
                            size_t lookup_idx, const Result<internal::RemoteTabletPtr>& tablet) {
    const auto prefix = Format("Lookup $0 got ", lookup_idx);
    if (tablet.ok()) {
      LOG(INFO) << prefix << "tablet: " << (*tablet)->tablet_id();
    } else {
      LOG(INFO) << prefix << "error: " << AsString(tablet.status());
    }
    num_lookups_done.fetch_add(1);
  };

  const auto lookup_func = [&]() {
    while(!stop_requested) {
      const auto hash_code = RandomUniformInt<uint16_t>(0, PartitionSchema::kMaxPartitionKey);
      const auto partition_key = PartitionSchema::EncodeMultiColumnHashValue(hash_code);
      client_->LookupTabletByKey(
          client_table_.table(), partition_key, CoarseMonoClock::now() + kLookupTimeout,
          std::bind(callback, num_lookups_called.fetch_add(1), std::placeholders::_1));
    }
  };

  const auto marker_func = [&]() {
    while(!stop_requested) {
      const auto table = client_table_.table();
      table->MarkPartitionsAsStale();
      const auto result = table->MaybeRefreshPartitions();
      if (!result.ok()) {
        LOG(INFO) << AsString(result.status());
      }
    }
  };

  LOG(INFO) << "Starting threads";

  std::vector<std::thread> threads;
  for (int i = 0; i < kNumLookupThreads; ++i) {
    threads.push_back(std::thread(lookup_func));
  }
  threads.push_back(std::thread(marker_func));

  ASSERT_OK(LoggedWaitFor([&num_lookups_called]{
    return num_lookups_called >= kNumLookups;
  }, 120s, "Tablet lookup calls"));

  LOG(INFO) << "Stopping threads";
  stop_requested = true;
  for (auto& thread : threads) {
    thread.join();
  }
  LOG(INFO) << "Stopped threads";

  ASSERT_OK(LoggedWaitFor([&num_lookups_done, num_lookups = num_lookups_called.load()] {
    return num_lookups_done >= num_lookups;
  }, kLookupTimeout, "Tablet lookup responses"));

  LOG(INFO) << "num_lookups_done: " << num_lookups_done;
}

}  // namespace client
}  // namespace yb
