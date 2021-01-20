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

#include <map>
#include <string>
#include <utility>
#include <boost/assign.hpp>
#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/client/client-test-util.h"
#include "yb/client/error.h"
#include "yb/client/schema.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_op.h"

#include "yb/common/ql_value.h"

#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/master/mini_master.h"
#include "yb/master/master.h"
#include "yb/master/master.pb.h"
#include "yb/master/master-test-util.h"
#include "yb/master/sys_catalog.h"
#include "yb/server/hybrid_clock.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/util/atomic.h"
#include "yb/util/faststring.h"
#include "yb/util/metrics.h"
#include "yb/util/random.h"
#include "yb/util/random_util.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"

using namespace std::literals;

DECLARE_bool(enable_data_block_fsync);
DECLARE_bool(enable_maintenance_manager);
DECLARE_bool(flush_rocksdb_on_shutdown);
DECLARE_int32(heartbeat_interval_ms);
DECLARE_bool(use_hybrid_clock);
DECLARE_int32(ht_lease_duration_ms);
DECLARE_int32(replication_factor);
DECLARE_int32(log_min_seconds_to_retain);
DECLARE_int32(catalog_manager_report_batch_size);

METRIC_DECLARE_counter(sys_catalog_peer_write_count);

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
using client::YBTableName;
using client::YBTableType;
using client::YBValue;
using std::shared_ptr;
using master::AlterTableRequestPB;
using master::AlterTableResponsePB;
using master::MiniMaster;
using std::map;
using std::pair;
using std::vector;
using tablet::TabletPeer;
using tserver::MiniTabletServer;

class AlterTableTest : public YBMiniClusterTestBase<MiniCluster>,
                       public ::testing::WithParamInterface<int> {
 public:
  AlterTableTest()
    : stop_threads_(false),
      inserted_idx_(0) {

    YBSchemaBuilder b;
    b.AddColumn("c0")->Type(INT32)->NotNull()->HashPrimaryKey();
    b.AddColumn("c1")->Type(INT32)->NotNull();
    CHECK_OK(b.Build(&schema_));

    FLAGS_enable_data_block_fsync = false; // Keep unit tests fast.
    FLAGS_use_hybrid_clock = false;
    FLAGS_ht_lease_duration_ms = 0;
    FLAGS_enable_ysql = false;
    ANNOTATE_BENIGN_RACE(&FLAGS_enable_maintenance_manager,
                         "safe to change at runtime");
  }

  void SetUp() override {
    // Make heartbeats faster to speed test runtime.
    FLAGS_heartbeat_interval_ms = 10;

    FLAGS_catalog_manager_report_batch_size = GetParam();

    YBMiniClusterTestBase::SetUp();

    MiniClusterOptions opts;
    opts.num_tablet_servers = num_replicas();
    FLAGS_replication_factor = num_replicas();
    cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(cluster_->Start());
    ASSERT_OK(cluster_->WaitForTabletServerCount(num_replicas()));

    client_ = CHECK_RESULT(YBClientBuilder()
        .add_master_server_addr(cluster_->mini_master()->bound_rpc_addr_str())
        .default_admin_operation_timeout(MonoDelta::FromSeconds(60))
        .Build());

    CHECK_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                                 kTableName.namespace_type()));

    // Add a table, make sure it reports itself.
    std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    CHECK_OK(table_creator->table_name(kTableName)
             .schema(&schema_)
             .table_type(YBTableType::YQL_TABLE_TYPE)
             .num_tablets(1)
             .Create());

    if (num_replicas() == 1) {
      tablet_peer_ = LookupTabletPeer();
    }
    LOG(INFO) << "Tablet successfully located";
  }

  void DoTearDown() override {
    client_.reset();
    tablet_peer_.reset();
    cluster_->Shutdown();
  }

  std::shared_ptr<TabletPeer> LookupTabletPeer() {
    vector<std::shared_ptr<TabletPeer> > peers;
    cluster_->mini_tablet_server(0)->server()->tablet_manager()->GetTabletPeers(&peers);
    return peers[0];
  }

  void ShutdownTS(int idx = 0) {
    // Drop the tablet_peer_ reference since the tablet peer becomes invalid once
    // we shut down the server. Additionally, if we hold onto the reference,
    // we'll end up calling the destructor from the test code instead of the
    // normal location, which can cause crashes, etc.
    tablet_peer_.reset();
    if (cluster_->mini_tablet_server(idx)->server() != nullptr) {
      cluster_->mini_tablet_server(idx)->Shutdown();
    }
  }

  void RestartTabletServer(int idx = 0) {
    tablet_peer_.reset();
    if (cluster_->mini_tablet_server(idx)->server()) {
      ASSERT_OK(cluster_->mini_tablet_server(idx)->Restart());
    } else {
      ASSERT_OK(cluster_->mini_tablet_server(idx)->Start());
    }

    ASSERT_OK(cluster_->mini_tablet_server(idx)->WaitStarted());
    if (idx == 0) {
      tablet_peer_ = LookupTabletPeer();
    }
  }

  Status WaitAlterTableCompletion(const YBTableName& table_name, int attempts) {
    int wait_time = 1000;
    for (int i = 0; i < attempts; ++i) {
      bool in_progress;
      string table_id;
      RETURN_NOT_OK(client_->IsAlterTableInProgress(table_name, table_id, &in_progress));
      if (!in_progress) {
        return Status::OK();
      }

      SleepFor(MonoDelta::FromMicroseconds(wait_time));
      wait_time = std::min(wait_time * 5 / 4, 1000000);
    }

    return STATUS(TimedOut, "AlterTable not completed within the timeout");
  }

  Status AddNewI32Column(const YBTableName& table_name,
                         const string& column_name) {
    return AddNewI32Column(table_name, column_name, MonoDelta::FromSeconds(60));
  }

  Status AddNewI32Column(const YBTableName& table_name,
                         const string& column_name,
                         const MonoDelta& timeout) {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(table_name));
    table_alterer->AddColumn(column_name)->Type(INT32)->NotNull();
    return table_alterer->timeout(timeout)->Alter();
  }

  enum VerifyPattern {
    C1_MATCHES_INDEX,
    C1_IS_DEADBEEF,
    C1_DOESNT_EXIST
  };

  void VerifyRows(int start_row, int num_rows, VerifyPattern pattern);

  void InsertRows(int start_row, int num_rows);

  void UpdateRow(int32_t row_key, const map<string, int32_t>& updates);

  std::vector<std::string> ScanToStrings();

  void WriteThread(QLWriteRequestPB::QLStmtType type);
  void ScannerThread();

  Status CreateTable(const YBTableName& table_name) {
    RETURN_NOT_OK(client_->CreateNamespaceIfNotExists(table_name.namespace_name(),
                                                      table_name.namespace_type()));

    std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    return table_creator->table_name(table_name)
        .schema(&schema_)
        .num_tablets(10)
        .Create();
  }

  int GetSysCatalogWrites() {
    auto GetSysCatalogMetric = [&](CounterPrototype& prototype) -> int64_t {
      auto metrics = cluster_->mini_master()->master()->catalog_manager()->sys_catalog()
          ->GetMetricEntity();
      return prototype.Instantiate(metrics)->value();
    };
    return GetSysCatalogMetric(METRIC_sys_catalog_peer_write_count);
  }

 protected:
  virtual int num_replicas() const { return 1; }

  static const YBTableName kTableName;

  std::unique_ptr<YBClient> client_;

  YBSchema schema_;

  std::shared_ptr<TabletPeer> tablet_peer_;

  AtomicBool stop_threads_;

  // The index of the last row inserted by InserterThread.
  // UpdaterThread uses this to figure out which rows can be
  // safely updated.
  AtomicInt<int32_t> inserted_idx_;
};

// Subclass which creates three servers and a replicated cluster.
class ReplicatedAlterTableTest : public AlterTableTest {
 protected:
  virtual int num_replicas() const override { return 3; }
};

const YBTableName AlterTableTest::kTableName(YQL_DATABASE_CQL, "my_keyspace", "fake-table");

INSTANTIATE_TEST_CASE_P(BatchSize, AlterTableTest, ::testing::Values(1, 10));

// Simple test to verify that the "alter table" command sent and executed
// on the TS handling the tablet of the altered table.
// TODO: create and verify multiple tablets when the client will support that.
TEST_P(AlterTableTest, TestTabletReports) {
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());
  ASSERT_OK(AddNewI32Column(kTableName, "new-i32"));
  ASSERT_EQ(1, tablet_peer_->tablet()->metadata()->schema_version());
}

// Verify that adding an existing column will return an "already present" error
TEST_P(AlterTableTest, TestAddExistingColumn) {
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  {
    Status s = AddNewI32Column(kTableName, "c1");
    ASSERT_TRUE(s.IsAlreadyPresent());
    ASSERT_STR_CONTAINS(s.ToString(), "The column already exists: c1");
  }

  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());
}

// Adding a nullable column with no default value should be equivalent
// to a NULL default.
TEST_P(AlterTableTest, TestAddNullableColumnWithoutDefault) {
  InsertRows(0, 1);
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));

  {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    table_alterer->AddColumn("new")->Type(INT32);
    ASSERT_OK(table_alterer->Alter());
  }

  InsertRows(1, 1);

  vector<string> rows = ScanToStrings();
  EXPECT_EQ(2, rows.size());
  EXPECT_EQ("{ int32:0, int32:0, null }", rows[0]);
  EXPECT_EQ("{ int32:16777216, int32:1, null }", rows[1]);
}

// Verify that, if a tablet server is down when an alter command is issued,
// it will eventually receive the command when it restarts.
TEST_P(AlterTableTest, TestAlterOnTSRestart) {
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  ShutdownTS();

  // Send the Alter request
  {
    Status s = AddNewI32Column(kTableName, "new-32", MonoDelta::FromMilliseconds(500));
    ASSERT_TRUE(s.IsTimedOut());
  }

  // Verify that the Schema is the old one
  YBSchema schema;
  PartitionSchema partition_schema;
  bool alter_in_progress = false;
  string table_id;
  ASSERT_OK(client_->GetTableSchema(kTableName, &schema, &partition_schema));
  ASSERT_TRUE(schema_.Equals(schema));
  ASSERT_OK(client_->IsAlterTableInProgress(kTableName, table_id, &alter_in_progress));
  ASSERT_TRUE(alter_in_progress);

  // Restart the TS and wait for the new schema
  RestartTabletServer();
  ASSERT_OK(WaitAlterTableCompletion(kTableName, 50));
  ASSERT_EQ(1, tablet_peer_->tablet()->metadata()->schema_version());
}

// Verify that nothing is left behind on cluster shutdown with pending async tasks
TEST_P(AlterTableTest, TestShutdownWithPendingTasks) {
  DontVerifyClusterBeforeNextTearDown();
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  ShutdownTS();

  // Send the Alter request
  {
    Status s = AddNewI32Column(kTableName, "new-i32", MonoDelta::FromMilliseconds(500));
    ASSERT_TRUE(s.IsTimedOut());
  }
}

// Verify that the new schema is applied/reported even when
// the TS is going down with the alter operation in progress.
// On TS restart the master should:
//  - get the new schema state, and mark the alter as complete
//  - get the old schema state, and ask the TS again to perform the alter.
TEST_P(AlterTableTest, TestRestartTSDuringAlter) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping slow test";
    return;
  }

  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  Status s = AddNewI32Column(kTableName, "new-i32", MonoDelta::FromMilliseconds(1));
  ASSERT_TRUE(s.IsTimedOut());

  // Restart the TS while alter is running
  for (int i = 0; i < 3; i++) {
    SleepFor(MonoDelta::FromMicroseconds(500));
    RestartTabletServer();
  }

  // Wait for the new schema
  ASSERT_OK(WaitAlterTableCompletion(kTableName, 50));
  ASSERT_EQ(1, tablet_peer_->tablet()->metadata()->schema_version());
}

TEST_P(AlterTableTest, TestGetSchemaAfterAlterTable) {
  ASSERT_OK(AddNewI32Column(kTableName, "new-i32"));

  YBSchema s;
  PartitionSchema partition_schema;
  ASSERT_OK(client_->GetTableSchema(kTableName, &s, &partition_schema));
}

void AlterTableTest::InsertRows(int start_row, int num_rows) {
  shared_ptr<YBSession> session = client_->NewSession();
  session->SetTimeout(15s);
  client::TableHandle table;
  ASSERT_OK(table.Open(kTableName, client_.get()));
  std::vector<std::shared_ptr<client::YBqlOp>> ops;

  // Insert a bunch of rows with the current schema
  for (int i = start_row; i < start_row + num_rows; i++) {
    auto op = table.NewInsertOp();
    // Endian-swap the key so that we spew inserts randomly
    // instead of just a sequential write pattern. This way
    // compactions may actually be triggered.
    int32_t key = bswap_32(i);
    auto req = op->mutable_request();
    QLAddInt32HashValue(req, key);

    if (table.schema().num_columns() > 1) {
      table.AddInt32ColumnValue(req, table.schema().columns()[1].name(), i);
    }

    ops.push_back(op);
    ASSERT_OK(session->Apply(op));

    if (ops.size() >= 50) {
      FlushSessionOrDie(session, ops);
      ops.clear();
    }
  }

  FlushSessionOrDie(session, ops);
}

void AlterTableTest::UpdateRow(int32_t row_key,
                               const map<string, int32_t>& updates) {
  shared_ptr<YBSession> session = client_->NewSession();
  session->SetTimeout(15s);

  client::TableHandle table;
  ASSERT_OK(table.Open(kTableName, client_.get()));

  auto update = table.NewUpdateOp();
  int32_t key = bswap_32(row_key); // endian swap to match 'InsertRows'
  QLAddInt32HashValue(update->mutable_request(), key);
  for (const auto& e : updates) {
    table.AddInt32ColumnValue(update->mutable_request(), e.first, e.second);
  }
  CHECK_OK(session->Apply(update));
  FlushSessionOrDie(session);
}

std::vector<string> AlterTableTest::ScanToStrings() {
  client::TableHandle table;
  EXPECT_OK(table.Open(kTableName, client_.get()));
  auto result = ScanTableToStrings(table);
  std::sort(result.begin(), result.end());
  return result;
}

// Verify that the 'num_rows' starting with 'start_row' fit the given pattern.
// Note that the 'start_row' here is not a row key, but the pre-transformation row
// key (InsertRows swaps endianness so that we random-write instead of sequential-write)
void AlterTableTest::VerifyRows(int start_row, int num_rows, VerifyPattern pattern) {
  client::TableHandle table;
  ASSERT_OK(table.Open(kTableName, client_.get()));

  int verified = 0;
  for (const auto& row : client::TableRange(table)) {
    int32_t key = row.column(0).int32_value();
    int32_t row_idx = bswap_32(key);
    if (row_idx < start_row || row_idx >= start_row + num_rows) {
      // Outside the range we're verifying
      continue;
    }
    verified++;

    switch (pattern) {
      case C1_MATCHES_INDEX:
        ASSERT_EQ(row_idx, row.column(1).int32_value());
        break;
      case C1_IS_DEADBEEF:
        ASSERT_TRUE(row.column(1).IsNull());
        break;
      case C1_DOESNT_EXIST:
        continue;
      default:
        ASSERT_TRUE(false) << "Invalid pattern: " << pattern;
        break;
    }
  }
  ASSERT_EQ(verified, num_rows);
}

// Test inserting/updating some data, dropping a column, and adding a new one
// with the same name. Data should not "reappear" from the old column.
//
// This is a regression test for KUDU-461.
TEST_P(AlterTableTest, TestDropAndAddNewColumn) {
  // Reduce flush threshold so that we get both on-disk data
  // for the alter as well as in-MRS data.
  // This also increases chances of a race.
  const int kNumRows = AllowSlowTests() ? 100000 : 1000;
  InsertRows(0, kNumRows);

  LOG(INFO) << "Verifying initial pattern";
  VerifyRows(0, kNumRows, C1_MATCHES_INDEX);

  LOG(INFO) << "Dropping and adding back c1";
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  ASSERT_OK(AddNewI32Column(kTableName, "c1"));

  LOG(INFO) << "Verifying that the new default shows up";
  VerifyRows(0, kNumRows, C1_IS_DEADBEEF);
}

TEST_P(AlterTableTest, DISABLED_TestCompactionAfterDrop) {
  LOG(INFO) << "Inserting rows";
  InsertRows(0, 3);

  std::string docdb_dump = tablet_peer_->tablet()->TEST_DocDBDumpStr();
  // DocDB should not be empty right now.
  ASSERT_NE(0, docdb_dump.length());

  LOG(INFO) << "Dropping c1";
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  LOG(INFO) << "Forcing compaction";
  tablet_peer_->tablet()->ForceRocksDBCompactInTest();

  docdb_dump = tablet_peer_->tablet()->TEST_DocDBDumpStr();

  LOG(INFO) << "Checking that docdb is empty";
  ASSERT_EQ("", docdb_dump);

  ASSERT_OK(cluster_->RestartSync());
  tablet_peer_ = LookupTabletPeer();
}

// This tests the scenario where the log entries immediately after last RocksDB flush are for a
// different schema than the one that was last flushed to the superblock.
TEST_P(AlterTableTest, TestLogSchemaReplay) {
  ASSERT_OK(AddNewI32Column(kTableName, "c2"));
  InsertRows(0, 2);
  UpdateRow(1, { {"c1", 0} });

  LOG(INFO) << "Flushing RocksDB";
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));

  UpdateRow(0, { {"c1", 1}, {"c2", 10001} });

  LOG(INFO) << "Dropping c1";
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  UpdateRow(1, { {"c2", 10002} });

  auto rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, int32:10001 }", rows[0]);
  ASSERT_EQ("{ int32:16777216, int32:10002 }", rows[1]);

  google::FlagSaver flag_saver;
  // Restart without flushing RocksDB
  FLAGS_flush_rocksdb_on_shutdown = false;
  LOG(INFO) << "Restarting tablet";
  ASSERT_NO_FATALS(RestartTabletServer());

  rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, int32:10001 }", rows[0]);
  ASSERT_EQ("{ int32:16777216, int32:10002 }", rows[1]);
}

// Tests that a renamed table can still be altered. This is a regression test, we used to not carry
// over column ids after a table rename.
TEST_P(AlterTableTest, TestRenameTableAndAdd) {
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  YBTableName new_name(kTableName.namespace_type(), kTableName.namespace_name(), "someothername");
  ASSERT_OK(table_alterer->RenameTo(new_name)
            ->Alter());

  ASSERT_OK(AddNewI32Column(new_name, "new"));
}

// Test restarting a tablet server several times after various
// schema changes.
// This is a regression test for KUDU-462.
TEST_P(AlterTableTest, TestBootstrapAfterAlters) {
  ASSERT_OK(AddNewI32Column(kTableName, "c2"));
  InsertRows(0, 1);
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));
  InsertRows(1, 1);

  UpdateRow(0, { {"c1", 10001} });
  UpdateRow(1, { {"c1", 10002} });

  auto rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, int32:10001, null }", rows[0]);
  ASSERT_EQ("{ int32:16777216, int32:10002, null }", rows[1]);

  LOG(INFO) << "Dropping c1";
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, null }", rows[0]);
  ASSERT_EQ("{ int32:16777216, null }", rows[1]);

  // Test that restart doesn't fail when trying to replay updates or inserts
  // with the dropped column.
  ASSERT_NO_FATALS(RestartTabletServer());

  rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, null }", rows[0]);
  ASSERT_EQ("{ int32:16777216, null }", rows[1]);

  // Add back a column called 'c2', but should not materialize old data.
  ASSERT_OK(AddNewI32Column(kTableName, "c1"));
  rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, null, null }", rows[0]);
  ASSERT_EQ("{ int32:16777216, null, null }", rows[1]);

  ASSERT_NO_FATALS(RestartTabletServer());
  rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, null, null }", rows[0]);
  ASSERT_EQ("{ int32:16777216, null, null }", rows[1]);
}

TEST_P(AlterTableTest, TestAlterWalRetentionSecs) {
  InsertRows(1, 1000);
  int kWalRetentionSecs = RandomUniformBool()
      ? FLAGS_log_min_seconds_to_retain / 2
      : FLAGS_log_min_seconds_to_retain * 2;

  LOG(INFO) << "Modifying wal retention time to " << kWalRetentionSecs;
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));

  ASSERT_OK(table_alterer->SetWalRetentionSecs(kWalRetentionSecs)->Alter());

  int expected_wal_retention_secs = max(FLAGS_log_min_seconds_to_retain, kWalRetentionSecs);

  ASSERT_EQ(kWalRetentionSecs, tablet_peer_->tablet()->metadata()->wal_retention_secs());
  ASSERT_EQ(expected_wal_retention_secs, tablet_peer_->log()->wal_retention_secs());

  // Test that the wal retention time gets set correctly in the metadata and in the log objects.
  ASSERT_NO_FATALS(RestartTabletServer());

  ASSERT_EQ(kWalRetentionSecs, tablet_peer_->tablet()->metadata()->wal_retention_secs());
  ASSERT_EQ(expected_wal_retention_secs, tablet_peer_->log()->wal_retention_secs());
}

TEST_P(AlterTableTest, TestCompactAfterUpdatingRemovedColumn) {
  // Disable maintenance manager, since we manually flush/compact
  // in this test.
  FLAGS_enable_maintenance_manager = false;

  vector<string> rows;

  ASSERT_OK(AddNewI32Column(kTableName, "c2"));
  InsertRows(0, 1);
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));
  InsertRows(1, 1);
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));


  rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, int32:0, null }", rows[0]);
  ASSERT_EQ("{ int32:16777216, int32:1, null }", rows[1]);

  // Add a delta for c1.
  UpdateRow(0, { {"c1", 54321} });

  // Drop c1.
  LOG(INFO) << "Dropping c1";
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  rows = ScanToStrings();
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("{ int32:0, null }", rows[0]);
}

typedef std::vector<std::shared_ptr<client::YBqlOp>> Ops;

std::pair<bool, int> AnalyzeResponse(const Ops& ops) {
  std::pair<bool, int> result = { false, 0 };
  for (const auto& op : ops) {
    if (op->response().status() == QLResponsePB::YQL_STATUS_OK) {
      ++result.second;
    } else {
      if (QLResponsePB::YQL_STATUS_SCHEMA_VERSION_MISMATCH == op->response().status()) {
        result.first = true;
      }
    }
  }
  return result;
}

// Thread which inserts rows into the table.
// After each batch of rows is inserted, inserted_idx_ is updated
// to communicate how much data has been written (and should now be
// updateable)
void AlterTableTest::WriteThread(QLWriteRequestPB::QLStmtType type) {
  shared_ptr<YBSession> session = client_->NewSession();
  session->SetTimeout(15s);

  client::TableHandle table;
  ASSERT_OK(table.Open(kTableName, client_.get()));
  Ops ops;
  int32_t processed = 0;
  int32_t i = 0;
  Random rng(1);
  for (;;) {
    bool should_stop = stop_threads_.Load();
    if (!should_stop) {
      auto op = table.NewWriteOp(type);
      auto req = op->mutable_request();
      // Endian-swap the key so that we spew inserts randomly
      // instead of just a sequential write pattern. This way
      // compactions may actually be triggered.

      if (type == QLWriteRequestPB::QL_STMT_INSERT) {
        int32_t key = bswap_32(i++);
        QLAddInt32HashValue(req, key);
        table.AddInt32ColumnValue(req, table.schema().columns()[1].name(), i);
      } else {
        int32_t max = inserted_idx_.Load();
        if (max == 0) {
          // Inserter hasn't inserted anything yet, so we have nothing to update.
          SleepFor(MonoDelta::FromMicroseconds(100));
          continue;
        }
        // Endian-swap the key to match the way the insert generates keys.
        int32_t key = bswap_32(rng.Uniform(max-1));
        QLAddInt32HashValue(req, key);
        table.AddInt32ColumnValue(req, table.schema().columns()[1].name(), i);
      }

      ops.push_back(op);
      ASSERT_OK(session->Apply(op));
    }

    if (should_stop || ops.size() >= 10) {
      Status s = session->Flush();
      ASSERT_TRUE(s.ok() || s.IsBusy() || s.IsIOError());
      auto result = AnalyzeResponse(ops);
      ops.clear();
      processed += result.second;
      if (type == QLWriteRequestPB::QL_STMT_INSERT) {
        inserted_idx_.Store(processed);
        i = processed;
      }
      if (result.first) {
        ASSERT_OK(table.Open(kTableName, client_.get()));
      }
    }

    if (should_stop) {
      break;
    }
  }

  LOG(INFO) << "Processed: " << processed << " of type " << QLWriteRequestPB::QLStmtType_Name(type);
  ASSERT_GT(processed, 0);
}

// Thread which loops reading data from the table.
// No verification is performed.
void AlterTableTest::ScannerThread() {
  client::TableHandle table;
  ASSERT_OK(table.Open(kTableName, client_.get()));
  while (!stop_threads_.Load()) {
    int inserted_at_scanner_start = inserted_idx_.Load();
    client::TableIteratorOptions options;
    bool failed = false;
    options.error_handler = [&failed](const Status& status) {
      LOG(WARNING) << "Scan failed: " << status;
      failed = true;
    };
    size_t count = boost::size(client::TableRange(table, options));
    if (failed) {
      continue;
    }

    LOG(INFO) << "Scanner saw " << count << " rows";
    // We may have gotten more rows than we expected, because inserts
    // kept going while we set up the scan. But, we should never get
    // fewer.
    ASSERT_GE(count, inserted_at_scanner_start)
      << "We didn't get as many rows as expected";
  }
}

// Test altering a table while also sending a lot of writes,
// checking for races between the two.
TEST_P(AlterTableTest, TestAlterUnderWriteLoad) {
  scoped_refptr<Thread> writer;
  CHECK_OK(Thread::Create(
      "test", "inserter",
      std::bind(&AlterTableTest::WriteThread, this, QLWriteRequestPB::QL_STMT_INSERT), &writer));

  scoped_refptr<Thread> updater;
  CHECK_OK(Thread::Create(
      "test", "updater",
      std::bind(&AlterTableTest::WriteThread, this, QLWriteRequestPB::QL_STMT_UPDATE), &updater));

  scoped_refptr<Thread> scanner;
  CHECK_OK(
      Thread::Create("test", "scanner", std::bind(&AlterTableTest::ScannerThread, this), &scanner));

  // Add columns until we reach 10.
  for (int i = 2; i < 10; i++) {
    MonoDelta delay;
    if (AllowSlowTests()) {
      // In slow test mode, let more writes accumulate in between
      // alters, so that we get enough writes to cause flushes,
      // compactions, etc.
      delay = MonoDelta::FromSeconds(3);
    } else {
      delay = MonoDelta::FromMilliseconds(100);
    }
    SleepFor(delay);

    ASSERT_OK(AddNewI32Column(kTableName, strings::Substitute("c$0", i)));
  }

  stop_threads_.Store(true);
  writer->Join();
  updater->Join();
  scanner->Join();
}

TEST_P(AlterTableTest, TestInsertAfterAlterTable) {
  YBTableName kSplitTableName(YQL_DATABASE_CQL, "my_keyspace", "split-table");

  // Create a new table with 10 tablets.
  //
  // With more tablets, there's a greater chance that the TS will heartbeat
  // after some but not all tablets have finished altering.
  ASSERT_OK(CreateTable(kSplitTableName));

  // Add a column, and immediately try to insert a row including that
  // new column.
  ASSERT_OK(AddNewI32Column(kSplitTableName, "new-i32"));
  client::TableHandle table;
  ASSERT_OK(table.Open(kSplitTableName, client_.get()));
  auto insert = table.NewInsertOp();
  auto req = insert->mutable_request();
  QLAddInt32HashValue(req, 1);
  table.AddInt32ColumnValue(req, "c1", 1);
  table.AddInt32ColumnValue(req, "new-i32", 1);
  shared_ptr<YBSession> session = client_->NewSession();
  session->SetTimeout(15s);
  ASSERT_OK(session->Apply(insert));
  Status s = session->Flush();
  if (!s.ok()) {
    ASSERT_EQ(1, session->CountPendingErrors());
    client::CollectedErrors errors = session->GetAndClearPendingErrors();
    ASSERT_EQ(1, errors.size());
    ASSERT_OK(errors[0]->status()); // will fail
  }
}

// Issue a bunch of alter tables in quick succession. Regression for a bug
// seen in an earlier implementation of "alter table" where these could
// conflict with each other.
TEST_P(AlterTableTest, TestMultipleAlters) {
  YBTableName kSplitTableName(YQL_DATABASE_CQL, "my_keyspace", "split-table");
  const size_t kNumNewCols = 10;

  // With more tablets, there's a greater chance that the TS will heartbeat
  // after some but not all tablets have finished altering.
  ASSERT_OK(CreateTable(kSplitTableName));

  // Issue a bunch of new alters without waiting for them to finish.
  for (int i = 0; i < kNumNewCols; i++) {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kSplitTableName));
    table_alterer->AddColumn(strings::Substitute("new_col$0", i))
                 ->Type(INT32)->NotNull();
    ASSERT_OK(table_alterer->wait(false)->Alter());
  }

  // Now wait. This should block on all of them.
  WaitAlterTableCompletion(kSplitTableName, 50);

  // All new columns should be present.
  YBSchema new_schema;
  PartitionSchema partition_schema;
  ASSERT_OK(client_->GetTableSchema(kSplitTableName, &new_schema, &partition_schema));
  ASSERT_EQ(kNumNewCols + schema_.num_columns(), new_schema.num_columns());
}

TEST_P(ReplicatedAlterTableTest, TestReplicatedAlter) {
  const int kNumRows = 100;
  InsertRows(0, kNumRows);

  LOG(INFO) << "Verifying initial pattern";
  VerifyRows(0, kNumRows, C1_MATCHES_INDEX);

  LOG(INFO) << "Dropping and adding back c1";
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  ASSERT_OK(AddNewI32Column(kTableName, "c1"));

  bool alter_in_progress;
  string table_id;
  ASSERT_OK(client_->IsAlterTableInProgress(kTableName, table_id, &alter_in_progress));
  ASSERT_FALSE(alter_in_progress);

  LOG(INFO) << "Verifying that the new default shows up";
  VerifyRows(0, kNumRows, C1_IS_DEADBEEF);
}

TEST_P(ReplicatedAlterTableTest, TestAlterOneTSDown) {
  const int kNumRows = 100;
  InsertRows(0, kNumRows);

  LOG(INFO) << "Verifying initial pattern";
  VerifyRows(0, kNumRows, C1_MATCHES_INDEX);

  ShutdownTS(0);
  // Now operating with 2 out of 3 servers.  Alter should still work because it's quorum-based.

  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());
  ASSERT_OK(AddNewI32Column(kTableName, "c1"));
  ASSERT_OK(AddNewI32Column(kTableName, "new_col"));

  bool alter_in_progress;
  string table_id;
  ASSERT_OK(client_->IsAlterTableInProgress(kTableName, table_id, &alter_in_progress));
  ASSERT_FALSE(alter_in_progress);

  LOG(INFO) << "Verifying that the new default shows up";
  VerifyRows(0, kNumRows, C1_IS_DEADBEEF);

  RestartTabletServer(0);
}

}  // namespace yb
