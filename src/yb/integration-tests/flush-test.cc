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
#include <regex>

#include "yb/integration-tests/test_workload.h"
#include "yb/integration-tests/ts_itest-base.h"
#include "yb/integration-tests/mini_cluster.h"

#include "yb/rocksdb/db/db_impl.h"
#include "yb/rocksdb/memory_monitor.h"
#include "yb/rocksdb/util/testutil.h"

#include "yb/tablet/tablet.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

using namespace std::literals;

DECLARE_bool(flush_rocksdb_on_shutdown);
DECLARE_int64(global_memstore_size_percentage);
DECLARE_int64(global_memstore_size_mb_max);
DECLARE_int32(memstore_size_mb);
DECLARE_int32(rocksdb_level0_file_num_compaction_trigger);
DECLARE_int32(rocksdb_max_background_flushes);

namespace yb {
namespace client {
class YBTableName;
}

namespace tserver {

namespace {

constexpr auto kWaitDelay = 10ms;

class RocksDbListener : public rocksdb::test::FlushedFileCollector {
 public:
  void OnCompactionStarted() override {
    ++num_compactions_started_;
  }

  int GetNumCompactionsStarted() { return num_compactions_started_; }

 private:
  std::atomic<int> num_compactions_started_;
};

Result<TabletId> GetTabletIdFromSstFilename(const std::string& filename) {
  static std::regex re(R"#(.*/tablet-([^/]+)/\d+\.sst)#");
  std::smatch match;
  if (std::regex_search(filename, match, re)) {
    return match.str(1);
  } else {
    return STATUS_FORMAT(InvalidArgument, "Cannot parse tablet id from SST filename: $0", filename);
  }
}

class TabletManagerListener : public tserver::TsTabletManagerListener {
 public:
  void StartedFlush(const TabletId& tablet_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_tablets_.push_back(tablet_id);
  }

  std::vector<TabletId> GetFlushedTablets() {
    std::lock_guard<std::mutex> lock(mutex_);
    return flushed_tablets_;
  }

 private:
  std::mutex mutex_;
  std::vector<TabletId> flushed_tablets_;
};

} // namespace

class FlushITest : public YBTest {
 public:
  FlushITest() {}

  void SetUp() override {
    HybridTime::TEST_SetPrettyToString(true);
    FLAGS_memstore_size_mb = kOverServerLimitMB;
    // Set the global memstore to kServerLimitMB.
    FLAGS_global_memstore_size_percentage = 100;
    FLAGS_global_memstore_size_mb_max = kServerLimitMB;
    YBTest::SetUp();

    rocksdb_listener_ = std::make_shared<RocksDbListener>();
    tablet_manager_listener_ = std::make_shared<TabletManagerListener>();

    // Start cluster
    MiniClusterOptions opts;
    opts.num_tablet_servers = 1;
    cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(cluster_->Start());
    // Patch tablet options inside tablet manager, will be applied to new created tablets.
    cluster_->GetTabletManager(0)->TEST_tablet_options()->listeners.push_back(
        rocksdb_listener_);
    // Patch tablet options inside mini tablet server, will use for new tablet server after
    // minicluster restart.
    cluster_->mini_tablet_server(0)->options()->listeners.push_back(rocksdb_listener_);
    SetupCluster();
    SetupWorkload();
  }

  void TearDown() override {
    workload_->StopAndJoin();
    cluster_->Shutdown();
    YBTest::TearDown();
  }

  void SetupCluster() {
    cluster_->GetTabletManager(0)->TEST_listeners.push_back(tablet_manager_listener_);
  }

  void SetupWorkload(
      const client::YBTableName& table_name = TestWorkloadOptions::kDefaultTableName) {
    workload_.reset(new TestWorkload(cluster_.get()));
    workload_->set_table_name(table_name);
    workload_->set_timeout_allowed(true);
    workload_->set_payload_bytes(kPayloadBytes);
    workload_->set_write_batch_size(1);
    workload_->set_num_write_threads(4);
    workload_->set_num_tablets(kNumTablets);
    workload_->Setup();
  }

 protected:
  size_t TotalBytesFlushed() {
    size_t bytes = 0;
    auto tablet_peers = cluster_->GetTabletPeers(0);
    for (auto& peer : tablet_peers) {
      bytes += peer->tablet()->regulardb_statistics()->getTickerCount(rocksdb::FLUSH_WRITE_BYTES);
    }
    return bytes;
  }

  size_t BytesWritten() {
    return workload_->rows_inserted() * kPayloadBytes;
  }

  void WriteAtLeast(size_t size_bytes) {
    workload_->Start();
    AssertLoggedWaitFor(
        [this, size_bytes] { return BytesWritten() >= size_bytes; }, 60s,
        Format("Waiting until we've written at least $0 bytes ...", size_bytes), kWaitDelay);
    workload_->StopAndJoin();
    LOG(INFO) << "Wrote " << BytesWritten() << " bytes.";
  }

  client::YBTableName GetTableName(int i) {
    auto table_name = TestWorkloadOptions::kDefaultTableName;
    table_name.set_table_name(Format("$0_$1", table_name.table_name(), i));
    return table_name;
  }

  int NumRunningFlushes() {
    int compactions = 0;
    for (auto& peer : cluster_->GetTabletPeers(0)) {
      auto* db = pointer_cast<rocksdb::DBImpl*>(peer->tablet()->TEST_db());
      if (db) {
        compactions += db->TEST_NumRunningFlushes();
      }
    }
    return compactions;
  }

  void WriteUntilCompaction() {
    int num_compactions_started = rocksdb_listener_->GetNumCompactionsStarted();
    workload_->Start();
    AssertLoggedWaitFor(
        [this, num_compactions_started] {
          return rocksdb_listener_->GetNumCompactionsStarted() > num_compactions_started;
        }, 60s, "Waiting until we've written to start compaction ...", kWaitDelay);
    workload_->StopAndJoin();
    LOG(INFO) << "Wrote " << BytesWritten() << " bytes.";
    AssertLoggedWaitFor(
        [this] {
          return NumTotalRunningCompactions(cluster_.get()) == 0 && NumRunningFlushes() == 0;
        }, 60s, "Waiting until compactions and flushes are done ...", kWaitDelay);
  }

  void AddTabletsWithNonEmptyMemTable(std::unordered_map<TabletId, int>* tablets, int order) {
    for (auto& peer : cluster_->GetTabletPeers(0)) {
      const auto tablet_id = peer->tablet_id();
      if (tablets->count(tablet_id) == 0) {
        auto* db = pointer_cast<rocksdb::DBImpl*>(peer->tablet()->TEST_db());
        if (db) {
          auto* cf = pointer_cast<rocksdb::ColumnFamilyHandleImpl*>(db->DefaultColumnFamily());
          if (cf->cfd()->mem()->num_entries() > 0) {
            tablets->insert(std::make_pair(tablet_id, order));
          }
        }
      }
    }
  }

  MemoryMonitor* memory_monitor() {
    return cluster_->GetTabletManager(0)->memory_monitor();
  }

  size_t GetRocksDbMemoryUsage() {
    return memory_monitor()->memory_usage();
  }

  void DumpMemoryUsage() {
    auto* server = cluster_->mini_tablet_server(0)->server();
    LOG(INFO) << server->mem_tracker()->FindChild("Tablets")->LogUsage("MEM ");
    LOG(INFO) << "rocksdb memory usage: " << GetRocksDbMemoryUsage();
  }

  void TestFlushPicksOldestInactiveTabletAfterCompaction(bool with_restart);

  const size_t kServerLimitMB = 2;
  // Used to set memstore limit to value higher than server limit, so flushes are only being
  // triggered by memory monitor which we want to test.
  const size_t kOverServerLimitMB = kServerLimitMB * 10;
  const size_t kNumTablets = 3;
  const size_t kPayloadBytes = 8_KB;
  std::unique_ptr<MiniCluster> cluster_;
  std::unique_ptr<TestWorkload> workload_;
  std::shared_ptr<RocksDbListener> rocksdb_listener_;
  std::shared_ptr<TabletManagerListener> tablet_manager_listener_;
};

TEST_F(FlushITest, TestFlushHappens) {
  const size_t flushed_before_writes = TotalBytesFlushed();
  WriteAtLeast((kServerLimitMB * 1_MB) + 1);
  ASSERT_OK(WaitFor(
      [this, flushed_before_writes] {
        return TotalBytesFlushed() > flushed_before_writes; },
      60s, "Flush", 10ms));
  const size_t flushed_after_writes = TotalBytesFlushed() - flushed_before_writes;
  LOG(INFO) << "Flushed " << flushed_after_writes << " bytes.";
  ASSERT_GT(flushed_after_writes, 0);
}

void FlushITest::TestFlushPicksOldestInactiveTabletAfterCompaction(bool with_restart) {
  // Trigger compaction early.
  FLAGS_rocksdb_level0_file_num_compaction_trigger = 2;

  // Set memstore limit to 1/nth of server limit so we can cause memory usage with enough
  // granularity.
  FLAGS_memstore_size_mb = kServerLimitMB / 5;
  std::unordered_map<TabletId, int> inactive_tablets_to_flush;

  // Write to tables until compaction started and until we occupy 50% of kServerLimitMB by
  // memtables.
  int tables = 1; // First empty table is created by test setup.
  while (GetRocksDbMemoryUsage() < kServerLimitMB * 1_MB / 2) {
    SetupWorkload(GetTableName(tables));
    WriteUntilCompaction();
    AddTabletsWithNonEmptyMemTable(&inactive_tablets_to_flush, tables);
    tables++;
  }

  LOG(INFO) << "Tablets to flush: " << inactive_tablets_to_flush.size();

  std::unordered_set<TabletId> inactive_tablets;
  for (auto& peer : cluster_->GetTabletPeers(0)) {
    inactive_tablets.insert(peer->tablet_id());
  }

  DumpMemoryUsage();

  if (with_restart) {
    // We want to restore memtables from log on restart to test how memory monitor based flush
    // works after restart.
    FLAGS_flush_rocksdb_on_shutdown = false;
    LOG(INFO) << "Restarting cluster ...";
    ASSERT_OK(cluster_->RestartSync());
    LOG(INFO) << "Cluster has been restarted";
    SetupCluster();
    DumpMemoryUsage();
  }

  rocksdb_listener_->Clear();

  FLAGS_memstore_size_mb = kOverServerLimitMB;
  SetupWorkload(GetTableName(tables));
  tables++;
  WriteAtLeast((kServerLimitMB * 1_MB) + 1);

  DumpMemoryUsage();

  AssertLoggedWaitFor(
      [this] { return !memory_monitor()->Exceeded(); }, 30s,
      "Waiting until memory is freed by flushes...", kWaitDelay);

  LOG(INFO) << "Tables: " << tables;

  ERROR_NOT_OK(
      LoggedWaitFor(
          [this, &inactive_tablets_to_flush] {
            const auto flushed_tablets = tablet_manager_listener_->GetFlushedTablets().size();
            YB_LOG_EVERY_N_SECS(INFO, 5) << "Flushed tablets: " << flushed_tablets;
            return flushed_tablets >= inactive_tablets_to_flush.size();
          }, 30s,
          "Waiting until flush started for all inactive tablets with non-empty memtable ...",
          kWaitDelay),
      "Flush wasn't started for some inactive tablets with non-empty memtable");

  auto flushed_tablets = tablet_manager_listener_->GetFlushedTablets();
  LOG(INFO) << "Flushed tablets: " << flushed_tablets.size();

  {
    std::unordered_set<TabletId> flushed_tablets_not_completed(
        flushed_tablets.begin(), flushed_tablets.end());
    ERROR_NOT_OK(
        LoggedWaitFor(
            [&] {
              for (const auto& filename : rocksdb_listener_->GetAndClearFlushedFiles()) {
                const auto tablet_id = CHECK_RESULT(GetTabletIdFromSstFilename(filename));
                flushed_tablets_not_completed.erase(tablet_id);
              }
              return flushed_tablets_not_completed.size() == 0;
            }, 30s, "Waiting for flushes to complete ...", kWaitDelay),
        "Some flushes weren't completed");
    ASSERT_EQ(flushed_tablets_not_completed.size(), 0) << "Flush wasn't completed for tablets: "
        << yb::ToString(flushed_tablets_not_completed);
  }

  int current_order = 0;
  for (const auto& tablet_id : flushed_tablets) {
    auto iter = inactive_tablets_to_flush.find(tablet_id);
    if (iter != inactive_tablets_to_flush.end()) {
      const auto expected_flush_order = iter->second;
      inactive_tablets_to_flush.erase(iter);
      LOG(INFO) << "Checking tablet " << tablet_id << " expected order: " << expected_flush_order;
      ASSERT_GE(expected_flush_order, current_order) << "Tablet was flushed not in order: "
                                                      << tablet_id;
      current_order = std::max(current_order, expected_flush_order);
    } else {
      ASSERT_EQ(inactive_tablets.count(tablet_id), 0)
          << "Flushed inactive tablet with empty memstore: " << tablet_id;
      ASSERT_EQ(inactive_tablets_to_flush.size(), 0)
          << "Tablet " << tablet_id << " from active table was flushed before inactive tablets "
                                       "with non-empty memtable: "
          << yb::ToString(inactive_tablets_to_flush);
    }
  }
  ASSERT_EQ(inactive_tablets_to_flush.size(), 0) << "Some inactive tables with non-empty memtable "
                                                    "were not flushed: "
                                                 << yb::ToString(inactive_tablets_to_flush);
}

TEST_F(FlushITest, TestFlushPicksOldestInactiveTabletAfterCompaction) {
  TestFlushPicksOldestInactiveTabletAfterCompaction(false /* with_restart */);
}

TEST_F(FlushITest, TestFlushPicksOldestInactiveTabletAfterCompactionWithRestart) {
  TestFlushPicksOldestInactiveTabletAfterCompaction(true /* with_restart */);
}

} // namespace tserver
} // namespace yb
