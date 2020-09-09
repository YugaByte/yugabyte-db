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

#include <string>
#include <vector>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/client/client-internal.h"
#include "yb/client/client-test-util.h"
#include "yb/client/client.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"

#include "yb/common/schema.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/strings/util.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/util/net/net_util.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"

using namespace std::literals;

DECLARE_int32(TEST_yb_num_total_tablets);

namespace yb {

// Note: this test needs to be in the client namespace in order for
// YBClient::Data class methods to be visible via FRIEND_TEST macro.
namespace client {

const int kNumTabletServerReplicas = 3;

using std::shared_ptr;
using std::string;
using std::vector;
using client::YBTableName;

class MasterFailoverTest : public YBTest {
 public:
  enum CreateTableMode {
    kWaitForCreate = 0,
    kNoWaitForCreate = 1
  };

  MasterFailoverTest() {
    opts_.master_rpc_ports = { 0, 0, 0 };
    opts_.num_masters = num_masters_ = opts_.master_rpc_ports.size();
    opts_.num_tablet_servers = kNumTabletServerReplicas;

    // Reduce various timeouts below as to make the detection of
    // leader master failures (specifically, failures as result of
    // long pauses) more rapid.

    // Set the TS->master heartbeat timeout to 1 second (down from 15 seconds).
    opts_.extra_tserver_flags.push_back("--heartbeat_rpc_timeout_ms=1000");
    // Allow one TS heartbeat failure before retrying with back-off (down from 3).
    opts_.extra_tserver_flags.push_back("--heartbeat_max_failures_before_backoff=1");
    // Wait for 500 ms after 'max_consecutive_failed_heartbeats'
    // before trying again (down from 1 second).
    opts_.extra_tserver_flags.push_back("--heartbeat_interval_ms=500");
  }

  void SetUp() override {
    YBTest::SetUp();
    ASSERT_NO_FATALS(RestartCluster());
  }

  void TearDown() override {
    client_.reset();
    if (cluster_) {
      cluster_->Shutdown();
    }
    YBTest::TearDown();
  }

  void RestartCluster() {
    if (cluster_) {
      cluster_->Shutdown();
      cluster_.reset();
    }
    opts_.timeout = MonoDelta::FromSeconds(NonTsanVsTsan(20, 60));
    cluster_.reset(new ExternalMiniCluster(opts_));
    ASSERT_OK(cluster_->Start());
    client_ = ASSERT_RESULT(cluster_->CreateClient());
  }

  Status CreateTable(const YBTableName& table_name, CreateTableMode mode) {
    RETURN_NOT_OK_PREPEND(
        client_->CreateNamespaceIfNotExists(table_name.namespace_name()),
        "Unable to create namespace " + table_name.namespace_name());
    client::YBSchema client_schema(client::YBSchemaFromSchema(yb::GetSimpleTestSchema()));
    std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    return table_creator->table_name(table_name)
        .table_type(YBTableType::YQL_TABLE_TYPE)
        .schema(&client_schema)
        .hash_schema(YBHashSchema::kMultiColumnHash)
        .timeout(MonoDelta::FromSeconds(90))
        .wait(mode == kWaitForCreate)
        .Create();
  }

  Status CreateIndex(
      const YBTableName& indexed_table_name, const YBTableName& index_name, CreateTableMode mode) {
    RETURN_NOT_OK_PREPEND(
      client_->CreateNamespaceIfNotExists(index_name.namespace_name()),
      "Unable to create namespace " + index_name.namespace_name());
    client::YBSchema client_schema(client::YBSchemaFromSchema(yb::GetSimpleTestSchema()));
    client::TableHandle table;
    RETURN_NOT_OK_PREPEND(
        table.Open(indexed_table_name, client_.get()),
        "Unable to open table " + indexed_table_name.ToString());

    std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    return table_creator->table_name(index_name)
        .table_type(YBTableType::YQL_TABLE_TYPE)
        .indexed_table_id(table->id())
        .schema(&client_schema)
        .hash_schema(YBHashSchema::kMultiColumnHash)
        .timeout(MonoDelta::FromSeconds(90))
        .wait(mode == kWaitForCreate)
        // In the new style create index request, the CQL proxy populates the
        // index info instead of the master. However, in these tests we bypass
        // the proxy and go directly to the master. We need to use the old
        // style create request to have the master generate the appropriate
        // index info.
        .TEST_use_old_style_create_request()
        .Create();
  }

  Status RenameTable(const YBTableName& table_name_orig, const YBTableName& table_name_new) {
    std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(table_name_orig));
    return table_alterer
      ->RenameTo(table_name_new)
      ->timeout(MonoDelta::FromSeconds(90))
      ->wait(true)
      ->Alter();
  }

  // Test that we can get the table location information from the
  // master and then open scanners on the tablet server. This involves
  // sending RPCs to both the master and the tablet servers and
  // requires that the table and tablet exist both on the masters and
  // the tablet servers.
  Status OpenTableAndScanner(const YBTableName& table_name) {
    client::TableHandle table;
    RETURN_NOT_OK_PREPEND(table.Open(table_name, client_.get()),
                          "Unable to open table " + table_name.ToString());
    client::TableRange range(table);
    auto it = range.begin();
    if (it != range.end()) {
      ++it;
    }

    return Status::OK();
  }

 protected:
  int num_masters_;
  ExternalMiniClusterOptions opts_;
  gscoped_ptr<ExternalMiniCluster> cluster_;
  std::unique_ptr<YBClient> client_;
};

class MasterFailoverTestIndexCreation : public MasterFailoverTest,
                                        public ::testing::WithParamInterface<int> {
 public:
  MasterFailoverTestIndexCreation() {
    opts_.extra_tserver_flags.push_back("--allow_index_table_read_write=true");
    opts_.extra_tserver_flags.push_back(
        "--index_backfill_upperbound_for_user_enforced_txn_duration_ms=100");
    opts_.extra_master_flags.push_back("--TEST_slowdown_backfill_alter_table_rpcs_ms=50");
    opts_.extra_master_flags.push_back("--disable_index_backfill=false");
    // Sometimes during master failover we have the create index kick in before the tservers have
    // checked in. By default we wait for enough TSs -- else we fail the create table/idx request.
    // We don't have to wait for that in the tests here.
    opts_.extra_master_flags.push_back("--catalog_manager_check_ts_count_for_create_table=false");
  }

  // Master has to do 5 RPCs to TServers to create+backfill an index.
  // 4 corresponding to set each of the 4 IndexPermissions, and 1 for GetSafeTime.
  // We want to simulate a failure before and after each RPC, so total 10 stages.
  static constexpr int kNumMaxStages = 10;
};

INSTANTIATE_TEST_CASE_P(
    MasterFailoverTestIndexCreation, MasterFailoverTestIndexCreation,
    ::testing::Range(1, MasterFailoverTestIndexCreation::kNumMaxStages));
// Test that synchronous CreateTable (issue CreateTable call and then
// wait until the table has been created) works even when the original
// leader master has been paused.
//
// Temporarily disabled since multi-master isn't supported yet.
// This test fails as of KUDU-1138, since the tablet servers haven't
// registered with the follower master, and thus it's likely to deny
// the CreateTable request thinking there are no TS available.
TEST_F(MasterFailoverTest, DISABLED_TestCreateTableSync) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "This test can only be run in slow mode.";
    return;
  }

  int leader_idx = -1;
  ASSERT_OK(cluster_->GetLeaderMasterIndex(&leader_idx));

  LOG(INFO) << "Pausing leader master";
  ASSERT_OK(cluster_->master(leader_idx)->Pause());
  ScopedResumeExternalDaemon resume_daemon(cluster_->master(leader_idx));

  YBTableName table_name(YQL_DATABASE_CQL, "testCreateTableSync");
  ASSERT_OK(CreateTable(table_name, kWaitForCreate));
  ASSERT_OK(OpenTableAndScanner(table_name));
}

// Test that we can issue a CreateTable call, pause the leader master
// immediately after, then verify that the table has been created on
// the newly elected leader master.
//
// TODO enable this test once flakiness issues are worked out and
// eliminated on test machines.
TEST_F(MasterFailoverTest, DISABLED_TestPauseAfterCreateTableIssued) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "This test can only be run in slow mode.";
    return;
  }

  int leader_idx = -1;
  ASSERT_OK(cluster_->GetLeaderMasterIndex(&leader_idx));

  YBTableName table_name(YQL_DATABASE_CQL, "testPauseAfterCreateTableIssued");
  LOG(INFO) << "Issuing CreateTable for " << table_name.ToString();
  ASSERT_OK(CreateTable(table_name, kNoWaitForCreate));

  LOG(INFO) << "Pausing leader master";
  ASSERT_OK(cluster_->master(leader_idx)->Pause());
  ScopedResumeExternalDaemon resume_daemon(cluster_->master(leader_idx));

  auto deadline = CoarseMonoClock::Now() + 90s;
  ASSERT_OK(client_->data_->WaitForCreateTableToFinish(client_.get(), table_name, "" /* table_id */,
                                                       deadline));

  ASSERT_OK(OpenTableAndScanner(table_name));
}

// Orchestrate a master failover at various points of a backfill,
// ensure that the backfill eventually completes.
TEST_P(MasterFailoverTestIndexCreation, TestPauseAfterCreateIndexIssued) {
  const int kPauseAfterStage = GetParam();
  YBTableName table_name(YQL_DATABASE_CQL, "test", "testPauseAfterCreateTableIssued");
  LOG(INFO) << "Issuing CreateTable for " << table_name.ToString();
  FLAGS_TEST_yb_num_total_tablets = 5;
  ASSERT_OK(CreateTable(table_name, kWaitForCreate));
  LOG(INFO) << "CreateTable done for " << table_name.ToString();

  MonoDelta total_time_taken_for_one_iteration;
  // In the first run, we estimate the total time taken for one create index to complete.
  // The second run will pause the master at the desired point during create index.
  for (int i = 0; i < 2; i++) {
    auto start = ToSteady(CoarseMonoClock::Now());
    int leader_idx = -1;
    ASSERT_OK(cluster_->GetLeaderMasterIndex(&leader_idx));
    ScopedResumeExternalDaemon resume_daemon(cluster_->master(leader_idx));

    OpIdPB op_id;
    ASSERT_OK(cluster_->GetLastOpIdForLeader(&op_id));
    ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(static_cast<int>(op_id.index())));

    YBTableName index_table_name(
        YQL_DATABASE_CQL, "test", "testPauseAfterCreateTableIssuedIdx" + yb::ToString(i));
    LOG(INFO) << "Issuing CreateIndex for " << index_table_name.ToString();
    ASSERT_OK(CreateIndex(table_name, index_table_name, kNoWaitForCreate));

    if (i != 0) {
      // In the first run, we estimate how long it takes for an uninterrupted
      // backfill process to complete, then the remaining iterations kill the
      // master leader at various points to cause the  master failover during
      // the various stages of index backfill.
      MonoDelta sleep_time = total_time_taken_for_one_iteration * kPauseAfterStage / kNumMaxStages;
      LOG(INFO) << "Sleeping for " << sleep_time << ", before master pause";
      SleepFor(sleep_time);

      LOG(INFO) << "Pausing leader master 0-based: " << leader_idx << " i.e. m-"
                << 1 + leader_idx;
      ASSERT_OK(cluster_->master(leader_idx)->Pause());
    }

    IndexInfoPB index_info_pb;
    TableId index_table_id;
    const auto deadline = CoarseMonoClock::Now() + 900s;
    do {
      ASSERT_OK(client_->data_->WaitForCreateTableToFinish(
          client_.get(), index_table_name, "" /* table_id */, deadline));
      ASSERT_OK(client_->data_->WaitForCreateTableToFinish(
          client_.get(), table_name, "" /* table_id */, deadline));

      Result<YBTableInfo> table_info = client_->GetYBTableInfo(table_name);
      ASSERT_TRUE(table_info);
      Result<YBTableInfo> index_table_info = client_->GetYBTableInfo(index_table_name);
      ASSERT_TRUE(index_table_info);

      index_table_id = index_table_info->table_id;
      index_info_pb.Clear();
      table_info->index_map[index_table_id].ToPB(&index_info_pb);
      YB_LOG_EVERY_N_SECS(INFO, 1) << "The index info for "
                                   << index_table_name.ToString() << " is "
                                   << yb::ToString(index_info_pb);

      ASSERT_TRUE(index_info_pb.has_index_permissions());
    } while (index_info_pb.index_permissions() <
                 IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE &&
             CoarseMonoClock::Now() < deadline);

    EXPECT_EQ(index_info_pb.index_permissions(),
              IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);

    LOG(INFO) << "All good for iteration " << i;
    ASSERT_OK(client_->DeleteIndexTable(index_table_name, nullptr, /* wait */ true));
    ASSERT_OK(client_->data_->WaitForDeleteTableToFinish(
        client_.get(), index_table_id, deadline));

    // For the first round we just simply calculate the time it takes
    if (i == 0) {
      total_time_taken_for_one_iteration = ToSteady(CoarseMonoClock::Now()) - start;
    }
  }
}

// Test the scenario where we create a table, pause the leader master,
// and then issue the DeleteTable call: DeleteTable should go to the newly
// elected leader master and succeed.
TEST_F(MasterFailoverTest, TestDeleteTableSync) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "This test can only be run in slow mode.";
    return;
  }

  int leader_idx = -1;

  ASSERT_OK(cluster_->GetLeaderMasterIndex(&leader_idx));

  YBTableName table_name(YQL_DATABASE_CQL, "test", "testDeleteTableSync");
  ASSERT_OK(CreateTable(table_name, kWaitForCreate));

  LOG(INFO) << "Pausing leader master";
  ASSERT_OK(cluster_->master(leader_idx)->Pause());
  ScopedResumeExternalDaemon resume_daemon(cluster_->master(leader_idx));

  ASSERT_OK(client_->DeleteTable(table_name));
  shared_ptr<YBTable> table;
  Status s = client_->OpenTable(table_name, &table);
  ASSERT_TRUE(s.IsNotFound());
}

// Test the scenario where we create a table, pause the leader master,
// and then issue the AlterTable call renaming a table: AlterTable
// should go to the newly elected leader master and succeed, renaming
// the table.
//
// TODO: Add an equivalent async test. Add a test for adding and/or
// renaming a column in a table.
TEST_F(MasterFailoverTest, TestRenameTableSync) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "This test can only be run in slow mode.";
    return;
  }

  int leader_idx = -1;

  ASSERT_OK(cluster_->GetLeaderMasterIndex(&leader_idx));

  YBTableName table_name_orig(YQL_DATABASE_CQL, "test", "testAlterTableSync");
  ASSERT_OK(CreateTable(table_name_orig, kWaitForCreate));

  LOG(INFO) << "Pausing leader master";
  ASSERT_OK(cluster_->master(leader_idx)->Pause());
  ScopedResumeExternalDaemon resume_daemon(cluster_->master(leader_idx));

  YBTableName table_name_new(YQL_DATABASE_CQL, "test", "testAlterTableSyncRenamed");
  ASSERT_OK(RenameTable(table_name_orig, table_name_new));
  shared_ptr<YBTable> table;
  ASSERT_OK(client_->OpenTable(table_name_new, &table));

  Status s = client_->OpenTable(table_name_orig, &table);
  ASSERT_TRUE(s.IsNotFound());
}

TEST_F(MasterFailoverTest, TestFailoverAfterTsFailure) {
  for (auto master : cluster_->master_daemons()) {
    ASSERT_OK(cluster_->SetFlag(master, "enable_register_ts_from_raft", "true"));
  }
  YBTableName table_name(YQL_DATABASE_CQL, "test", "testFailoverAfterTsFailure");
  ASSERT_OK(CreateTable(table_name, kWaitForCreate));

  cluster_->tablet_server(0)->Shutdown();

  // Roll over to a new master.
  ASSERT_OK(cluster_->ChangeConfig(cluster_->GetLeaderMaster(), consensus::REMOVE_SERVER));

  // Count all servers equal to 3.
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    int tserver_count;
    RETURN_NOT_OK(client_->TabletServerCount(&tserver_count, false /* primary_only */));
    return tserver_count == 3;
  }, MonoDelta::FromSeconds(30), "Wait for tablet server count"));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    int tserver_count;
    RETURN_NOT_OK(client_->TabletServerCount(&tserver_count, true /* primary_only */));
    return tserver_count == 2;
  }, MonoDelta::FromSeconds(30), "Wait for tablet server count"));

  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(client_->GetTablets(table_name, 0, &tablets));

  // Assert master sees that all tablets have 3 replicas.
  for (const auto& loc : tablets) {
    ASSERT_EQ(loc.replicas_size(), 3);
  }

  // Make sure we can issue a delete table that doesn't crash with the fake ts. Then, make sure
  // when we restart the server, we properly re-register and have no crashes.
  ASSERT_OK(client_->DeleteTable(table_name, false /* wait */));
  ASSERT_OK(cluster_->tablet_server(0)->Start());

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    int tserver_count;
    RETURN_NOT_OK(client_->TabletServerCount(&tserver_count, true /* primary_only */));
    bool is_idle = VERIFY_RESULT(client_->IsLoadBalancerIdle());
    // We have registered the new tserver and the LB is idle.
    return tserver_count == 3 && is_idle;
  }, MonoDelta::FromSeconds(30), "Wait for LB idle"));

  cluster_->AssertNoCrashes();
}

}  // namespace client
}  // namespace yb
