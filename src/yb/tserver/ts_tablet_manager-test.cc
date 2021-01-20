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

#include "yb/tserver/ts_tablet_manager.h"

#include <string>
#include <set>

#include <gtest/gtest.h>
#include <gflags/gflags.h>

#include "yb/common/partition.h"
#include "yb/common/schema.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/metadata.pb.h"
#include "yb/consensus/raft_consensus.h"
#include "yb/fs/fs_manager.h"
#include "yb/master/master.pb.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/tablet-test-util.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/util/test_util.h"
#include "yb/util/format.h"

#define ASSERT_REPORT_HAS_UPDATED_TABLET(report, tablet_id) \
  ASSERT_NO_FATALS(AssertReportHasUpdatedTablet(report, tablet_id))

#define ASSERT_MONOTONIC_REPORT_SEQNO(report_seqno, tablet_report) \
  ASSERT_NO_FATALS(AssertMonotonicReportSeqno(report_seqno, tablet_report))

DECLARE_bool(TEST_pretend_memory_exceeded_enforce_flush);

namespace yb {
namespace tserver {

using consensus::kInvalidOpIdIndex;
using consensus::RaftConfigPB;
using consensus::ConsensusRound;
using consensus::ConsensusRoundPtr;
using consensus::ReplicateMsg;
using master::ReportedTabletPB;
using master::TabletReportPB;
using master::TabletReportUpdatesPB;
using tablet::TabletPeer;
using gflags::FlagSaver;

static const char* const kTabletId = "my-tablet-id";
static const int kConsensusRunningWaitMs = 10000;

class TsTabletManagerTest : public YBTest {
 public:
  TsTabletManagerTest()
    : schema_({ ColumnSchema("key", UINT32) }, 1) {
  }

  void CreateMiniTabletServer() {
    auto mini_ts = MiniTabletServer::CreateMiniTabletServer(test_data_root_, 0);
    ASSERT_OK(mini_ts);
    mini_server_ = std::move(*mini_ts);
  }

  void SetUp() override {
    YBTest::SetUp();

    test_data_root_ = GetTestPath("TsTabletManagerTest-fsroot");
    CreateMiniTabletServer();
    ASSERT_OK(mini_server_->Start());
    mini_server_->FailHeartbeats();

    config_ = mini_server_->CreateLocalConfig();

    tablet_manager_ = mini_server_->server()->tablet_manager();
    fs_manager_ = mini_server_->server()->fs_manager();
  }

  void TearDown() override {
    if (mini_server_) {
      mini_server_->Shutdown();
    }
  }

  Status CreateNewTablet(const std::string& tablet_id,
                         const Schema& schema,
                         std::shared_ptr<tablet::TabletPeer>* out_tablet_peer) {
    return CreateNewTablet(tablet_id, tablet_id, schema, out_tablet_peer);
  }

  Status CreateNewTablet(const std::string& table_id,
                         const std::string& tablet_id,
                         const Schema& schema,
                         std::shared_ptr<tablet::TabletPeer>* out_tablet_peer) {
    Schema full_schema = SchemaBuilder(schema).Build();
    std::pair<PartitionSchema, Partition> partition = tablet::CreateDefaultPartition(full_schema);

    std::shared_ptr<tablet::TabletPeer> tablet_peer;
    RETURN_NOT_OK(tablet_manager_->CreateNewTablet(
        table_id, tablet_id, partition.second, tablet_id, tablet_id, TableType::DEFAULT_TABLE_TYPE,
        full_schema, partition.first, boost::none /* index_info */, config_, &tablet_peer));
    if (out_tablet_peer) {
      (*out_tablet_peer) = tablet_peer;
    }

    RETURN_NOT_OK(tablet_peer->WaitUntilConsensusRunning(
          MonoDelta::FromMilliseconds(kConsensusRunningWaitMs)));

    return tablet_peer->consensus()->EmulateElection();
  }

 protected:
  std::unique_ptr<MiniTabletServer> mini_server_;
  FsManager* fs_manager_;
  TSTabletManager* tablet_manager_;

  Schema schema_;
  RaftConfigPB config_;

  string test_data_root_;
};

TEST_F(TsTabletManagerTest, TestCreateTablet) {
  // Create a new tablet.
  std::shared_ptr<TabletPeer> peer;
  ASSERT_OK(CreateNewTablet(kTabletId, schema_, &peer));
  ASSERT_EQ(kTabletId, peer->tablet()->tablet_id());
  peer.reset();

  // Re-load the tablet manager from the filesystem.
  LOG(INFO) << "Shutting down tablet manager";
  mini_server_->Shutdown();
  LOG(INFO) << "Restarting tablet manager";
  CreateMiniTabletServer();
  ASSERT_OK(mini_server_->Start());
  ASSERT_OK(mini_server_->WaitStarted());
  tablet_manager_ = mini_server_->server()->tablet_manager();

  // Ensure that the tablet got re-loaded and re-opened off disk.
  ASSERT_TRUE(tablet_manager_->LookupTablet(kTabletId, &peer));
  ASSERT_EQ(kTabletId, peer->tablet()->tablet_id());
}

TEST_F(TsTabletManagerTest, TestTombstonedTabletsAreUnregistered) {
  const std::string kTableId = "my-table-id";
  const std::string kTabletId1 = "my-tablet-id-1";
  const std::string kTabletId2 = "my-tablet-id-2";

  auto shutdown_tserver_and_reload_tablet_manager = [this]() {
    // Re-load the tablet manager from the filesystem.
    LOG(INFO) << "Shutting down tablet manager";
    mini_server_->Shutdown();
    LOG(INFO) << "Restarting tablet manager";
    CreateMiniTabletServer();
    ASSERT_OK(mini_server_->Start());
    ASSERT_OK(mini_server_->WaitStarted());
    tablet_manager_ = mini_server_->server()->tablet_manager();
  };

  auto count_tablet_in_assignment_map =
      [&kTableId](const TSTabletManager::TableDiskAssignmentMap* table_assignment_map,
                  const std::string& tablet_id) {
        auto table_assignment_iter = table_assignment_map->find(kTableId);
        EXPECT_NE(table_assignment_iter, table_assignment_map->end());
        // the number of data directories for this table should be non-empty.
        EXPECT_GT(table_assignment_iter->second.size(), 0);
        int tablet_count = 0;
        for (const auto& tablet_assignment_iter : table_assignment_iter->second) {
          // directory_map maps a directory name to a set of tablet ids.
          for (const TabletId& tablet : tablet_assignment_iter.second) {
            if (tablet_id == tablet) {
              tablet_count++;
            }
          }
        }
        return tablet_count;
      };

  auto assert_tablet_assignment_count =
      [this, &count_tablet_in_assignment_map](const std::string& tablet_id, int count) {
    ASSERT_EQ(
        count_tablet_in_assignment_map(&tablet_manager_->table_data_assignment_map_, tablet_id),
        count);
    ASSERT_EQ(
        count_tablet_in_assignment_map(&tablet_manager_->table_wal_assignment_map_, tablet_id),
        count);
  };

  // Create a new tablet.
  std::shared_ptr<TabletPeer> peer;
  ASSERT_OK(CreateNewTablet(kTableId, kTabletId1, schema_, &peer));
  ASSERT_EQ(kTabletId1, peer->tablet()->tablet_id());
  peer.reset();
  ASSERT_OK(CreateNewTablet(kTableId, kTabletId2, schema_, &peer));
  ASSERT_EQ(kTabletId2, peer->tablet()->tablet_id());

  assert_tablet_assignment_count(kTabletId1, 1);
  assert_tablet_assignment_count(kTabletId2, 1);

  shutdown_tserver_and_reload_tablet_manager();

  assert_tablet_assignment_count(kTabletId1, 1);
  assert_tablet_assignment_count(kTabletId2, 1);

  boost::optional<int64_t> cas_config_opid_index_less_or_equal;
  boost::optional<TabletServerErrorPB::Code> error_code;
  ASSERT_OK(tablet_manager_->DeleteTablet(kTabletId1,
      tablet::TABLET_DATA_TOMBSTONED,
      cas_config_opid_index_less_or_equal,
      &error_code));

  assert_tablet_assignment_count(kTabletId1, 0);
  assert_tablet_assignment_count(kTabletId2, 1);

  shutdown_tserver_and_reload_tablet_manager();

  assert_tablet_assignment_count(kTabletId1, 0);
  assert_tablet_assignment_count(kTabletId2, 1);

  ASSERT_OK(tablet_manager_->DeleteTablet(kTabletId1,
                                          tablet::TABLET_DATA_DELETED,
                                          cas_config_opid_index_less_or_equal,
                                          &error_code));

  assert_tablet_assignment_count(kTabletId1, 0);
  assert_tablet_assignment_count(kTabletId2, 1);

  shutdown_tserver_and_reload_tablet_manager();

  assert_tablet_assignment_count(kTabletId1, 0);
  assert_tablet_assignment_count(kTabletId2, 1);
}

TEST_F(TsTabletManagerTest, TestProperBackgroundFlushOnStartup) {
  FlagSaver flag_saver;
  FLAGS_TEST_pretend_memory_exceeded_enforce_flush = true;

  const int kNumTablets = 2;
  const int kNumRestarts = 3;

  std::vector<TabletId> tablet_ids;
  std::vector<ConsensusRoundPtr> consensus_rounds;

  for (int i = 0; i < kNumTablets; ++i) {
    std::shared_ptr<TabletPeer> peer;
    const auto tablet_id = Format("my-tablet-$0", i + 1);
    tablet_ids.emplace_back(tablet_id);
    ASSERT_OK(CreateNewTablet(tablet_id, schema_, &peer));
    ASSERT_EQ(tablet_id, peer->tablet()->tablet_id());

    auto replicate_ptr = std::make_shared<ReplicateMsg>();
    replicate_ptr->set_op_type(consensus::NO_OP);
    replicate_ptr->set_hybrid_time(peer->clock().Now().ToUint64());
    ConsensusRoundPtr round(new ConsensusRound(peer->consensus(), std::move(replicate_ptr)));
    consensus_rounds.emplace_back(round);
    round->BindToTerm(peer->raft_consensus()->TEST_LeaderTerm());
    ASSERT_OK(peer->consensus()->TEST_Replicate(round));
  }

  for (int i = 0; i < kNumRestarts; ++i) {
    LOG(INFO) << "Shutting down tablet manager";
    mini_server_->Shutdown();
    LOG(INFO) << "Restarting tablet manager";
    CreateMiniTabletServer();
    ASSERT_OK(mini_server_->Start());
    auto* tablet_manager = mini_server_->server()->tablet_manager();
    ASSERT_NE(nullptr, tablet_manager);
    tablet_manager->MaybeFlushTablet();
    ASSERT_OK(mini_server_->WaitStarted());
    for (auto& tablet_id : tablet_ids) {
      std::shared_ptr<TabletPeer> peer;
      ASSERT_TRUE(tablet_manager->LookupTablet(tablet_id, &peer));
      ASSERT_EQ(tablet_id, peer->tablet()->tablet_id());
    }
  }
}

static void AssertMonotonicReportSeqno(int64_t* report_seqno,
                                       const TabletReportPB &report) {
  ASSERT_LT(*report_seqno, report.sequence_number());
  *report_seqno = report.sequence_number();
}

static void AssertReportHasUpdatedTablet(const TabletReportPB& report,
                                         const string& tablet_id) {
  ASSERT_GE(report.updated_tablets_size(), 0);
  bool found_tablet = false;
  for (ReportedTabletPB reported_tablet : report.updated_tablets()) {
    if (reported_tablet.tablet_id() == tablet_id) {
      found_tablet = true;
      ASSERT_TRUE(reported_tablet.has_committed_consensus_state());
      ASSERT_TRUE(reported_tablet.committed_consensus_state().has_current_term())
          << reported_tablet.ShortDebugString();
      ASSERT_TRUE(reported_tablet.committed_consensus_state().has_leader_uuid())
          << reported_tablet.ShortDebugString();
      ASSERT_TRUE(reported_tablet.committed_consensus_state().has_config());
      const RaftConfigPB& committed_config = reported_tablet.committed_consensus_state().config();
      ASSERT_EQ(kInvalidOpIdIndex, committed_config.opid_index());
      ASSERT_EQ(1, committed_config.peers_size());
      ASSERT_TRUE(committed_config.peers(0).has_permanent_uuid())
          << reported_tablet.ShortDebugString();
      ASSERT_EQ(committed_config.peers(0).permanent_uuid(),
                reported_tablet.committed_consensus_state().leader_uuid())
          << reported_tablet.ShortDebugString();
    }
  }
  ASSERT_TRUE(found_tablet);
}

static void CopyReportToUpdates(const TabletReportPB& req, TabletReportUpdatesPB* resp) {
  resp->Clear();
  for (const auto & tablet : req.updated_tablets()) {
    auto new_tablet = resp->add_tablets();
    new_tablet->set_tablet_id(tablet.tablet_id());
  }
}

TEST_F(TsTabletManagerTest, TestTabletReports) {
  TabletReportPB report;
  TabletReportUpdatesPB updates;
  int64_t seqno = -1;

  // Generate a tablet report before any tablets are loaded. Should be empty.
  tablet_manager_->StartFullTabletReport(&report);
  ASSERT_EQ(0, report.updated_tablets().size());
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  CopyReportToUpdates(report, &updates);
  tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);

  // Another report should now be incremental, but with no changes.
  tablet_manager_->GenerateTabletReport(&report);
  ASSERT_EQ(0, report.updated_tablets().size());
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  CopyReportToUpdates(report, &updates);
  tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);

  // Create a tablet and do another incremental report - should include the tablet.
  ASSERT_OK(CreateNewTablet("tablet-1", schema_, nullptr));
  int updated_tablets = 0;
  while (updated_tablets != 1) {
    tablet_manager_->GenerateTabletReport(&report);
    updated_tablets = report.updated_tablets().size();
    ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  }

  ASSERT_REPORT_HAS_UPDATED_TABLET(report, "tablet-1");

  // If we don't acknowledge the report, and ask for another incremental report,
  // it should include the tablet again.
  tablet_manager_->GenerateTabletReport(&report);
  ASSERT_EQ(1, report.updated_tablets().size());
  ASSERT_REPORT_HAS_UPDATED_TABLET(report, "tablet-1");
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);

  // Now acknowledge the last report, and further incrementals should be empty.
  CopyReportToUpdates(report, &updates);
  tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);
  tablet_manager_->GenerateTabletReport(&report);
  ASSERT_EQ(0, report.updated_tablets().size());
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  CopyReportToUpdates(report, &updates);
  tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);

  // Create a second tablet, and ensure the incremental report shows it.
  ASSERT_OK(CreateNewTablet("tablet-2", schema_, nullptr));

  // Wait up to 10 seconds to get a tablet report from tablet-2.
  // TabletPeer does not mark tablets dirty until after it commits the
  // initial configuration change, so there is also a window for tablet-1 to
  // have been marked dirty since the last report.
  MonoDelta timeout(MonoDelta::FromSeconds(10));
  MonoTime start(MonoTime::Now());
  report.Clear();
  while (true) {
    bool found_tablet_2 = false;
    tablet_manager_->GenerateTabletReport(&report);
    ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report) << report.ShortDebugString();
    for (const ReportedTabletPB& reported_tablet : report.updated_tablets()) {
      if (reported_tablet.tablet_id() == "tablet-2") {
        found_tablet_2  = true;
        break;
      }
    }
    if (found_tablet_2) break;
    MonoDelta elapsed(MonoTime::Now().GetDeltaSince(start));
    ASSERT_TRUE(elapsed.LessThan(timeout)) << "Waited too long for tablet-2 to be marked dirty: "
                                           << elapsed.ToString() << ". "
                                           << "Latest report: " << report.ShortDebugString();
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  CopyReportToUpdates(report, &updates);
  tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);

  // Asking for a full tablet report should re-report both tablets
  tablet_manager_->StartFullTabletReport(&report);
  ASSERT_EQ(2, report.updated_tablets().size());
  ASSERT_REPORT_HAS_UPDATED_TABLET(report, "tablet-1");
  ASSERT_REPORT_HAS_UPDATED_TABLET(report, "tablet-2");
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
}

TEST_F(TsTabletManagerTest, TestTabletReportLimit) {
  TabletReportPB report;
  TabletReportUpdatesPB updates;
  int64_t seqno = -1;

  // Generate a tablet report before any tablets are loaded. Should be empty.
  tablet_manager_->StartFullTabletReport(&report);
  ASSERT_EQ(0, report.updated_tablets().size());
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  CopyReportToUpdates(report, &updates);
  tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);

  // Another report should now be incremental, but with no changes.
  tablet_manager_->GenerateTabletReport(&report);
  ASSERT_EQ(0, report.updated_tablets().size());
  ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
  CopyReportToUpdates(report, &updates);
  tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);

  // Set a report limit and create a set of tablets clearly over that limit.
  const int32_t limit = 10, total_tablets = 25;
  tablet_manager_->SetReportLimit(limit);
  std::set<std::string> tablet_ids, tablet_ids_full;
  for (int i = 0; i < total_tablets; ++i) {
    auto id = "tablet-" + std::to_string(i);
    ASSERT_OK(CreateNewTablet(id, schema_, nullptr));
    tablet_ids.insert(id);
    tablet_ids_full.insert(id);
    LOG(INFO) << "Adding " << id;
  }

  // Ensure that incremental report requests returns all in batches.
  for (int n = limit, left = total_tablets; left > 0; left -= n, n = std::min(limit, left)) {
    tablet_manager_->GenerateTabletReport(&report);
    ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
    ASSERT_EQ(n, report.updated_tablets().size());
    for (auto& t : report.updated_tablets()) {
      LOG(INFO) << "Erasing " << t.tablet_id();
      ASSERT_EQ(1, tablet_ids.erase(t.tablet_id()));
    }
    CopyReportToUpdates(report, &updates);
    tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);
}

  // Generate a Full Report and ensure that the same batching occurs.
  tablet_manager_->StartFullTabletReport(&report);
  for (int n = limit, left = total_tablets; left > 0; left -= n, n = std::min(limit, left)) {
    ASSERT_MONOTONIC_REPORT_SEQNO(&seqno, report);
    ASSERT_EQ(n, report.updated_tablets().size());
    for (auto& t : report.updated_tablets()) {
      ASSERT_EQ(1, tablet_ids_full.erase(t.tablet_id()));
    }
    CopyReportToUpdates(report, &updates);
    tablet_manager_->MarkTabletReportAcknowledged(seqno, updates);
    tablet_manager_->GenerateTabletReport(&report);
  }
  ASSERT_EQ(0, report.updated_tablets().size()); // Last incremental report is empty.
}

} // namespace tserver
} // namespace yb
