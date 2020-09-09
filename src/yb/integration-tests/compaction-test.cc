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

#include "yb/client/transaction_pool.h"

#include "yb/integration-tests/test_workload.h"
#include "yb/integration-tests/mini_cluster.h"

#include "yb/master/mini_master.h"
#include "yb/master/master.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/size_literals.h"
#include "yb/util/test_util.h"

using namespace std::literals; // NOLINT

DECLARE_int64(db_write_buffer_size);
DECLARE_int32(rocksdb_level0_file_num_compaction_trigger);
DECLARE_int32(timestamp_history_retention_interval_sec);

namespace yb {

namespace tserver {

namespace {

constexpr auto kWaitDelay = 10ms;
constexpr auto kPayloadBytes = 8_KB;
constexpr auto kMemStoreSize = 100_KB;
constexpr auto kNumTablets = 3;



class RocksDbListener : public rocksdb::EventListener {
 public:
  void OnCompactionCompleted(rocksdb::DB* db, const rocksdb::CompactionJobInfo&) override {
    IncrementValueByDb(db, &num_compactions_completed_);
  }

  int GetNumCompactionsCompleted(rocksdb::DB* db) {
    return GetValueByDb(db, num_compactions_completed_);
  }

  void OnFlushCompleted(rocksdb::DB* db, const rocksdb::FlushJobInfo&) override {
    IncrementValueByDb(db, &num_flushes_completed_);
  }

  int GetNumFlushesCompleted(rocksdb::DB* db) {
    return GetValueByDb(db, num_flushes_completed_);
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    num_compactions_completed_.clear();
    num_flushes_completed_.clear();
  }

 private:
  typedef std::unordered_map<const rocksdb::DB*, int> CountByDbMap;

  void IncrementValueByDb(const rocksdb::DB* db, CountByDbMap* map);
  int GetValueByDb(const rocksdb::DB* db, const CountByDbMap& map);

  std::mutex mutex_;
  CountByDbMap num_compactions_completed_;
  CountByDbMap num_flushes_completed_;
};

void RocksDbListener::IncrementValueByDb(const rocksdb::DB* db, CountByDbMap* map) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = map->find(db);
  if (it == map->end()) {
    (*map)[db] = 1;
  } else {
    ++(it->second);
  }
}

int RocksDbListener::GetValueByDb(const rocksdb::DB* db, const CountByDbMap& map) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = map.find(db);
  if (it == map.end()) {
    return 0;
  } else {
    return it->second;
  }
}

} // namespace

class CompactionTest : public YBTest {
 public:
  CompactionTest() {}

  void SetUp() override {
    YBTest::SetUp();

    ASSERT_OK(clock_->Init());
    rocksdb_listener_ = std::make_shared<RocksDbListener>();

    // Start cluster.
    MiniClusterOptions opts;
    opts.num_tablet_servers = 1;
    cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(cluster_->Start());
    // These flags should be set after minicluster start, so it wouldn't override them.
    FLAGS_db_write_buffer_size = kMemStoreSize;
    FLAGS_rocksdb_level0_file_num_compaction_trigger = 3;
    // Patch tablet options inside tablet manager, will be applied to newly created tablets.
    cluster_->GetTabletManager(0)->TEST_tablet_options()->listeners.push_back(rocksdb_listener_);

    client_ = ASSERT_RESULT(cluster_->CreateClient());
    transaction_manager_ = std::make_unique<client::TransactionManager>(
        client_.get(), clock_, client::LocalTabletFilter());
    transaction_pool_ = std::make_unique<client::TransactionPool>(
        transaction_manager_.get(), nullptr /* metric_entity */);
  }

  void TearDown() override {
    workload_->StopAndJoin();
    // Shutdown client before destroying transaction manager, so we don't have transaction RPCs
    // in progress after transaction manager is destroyed.
    client_->Shutdown();
    cluster_->Shutdown();
    YBTest::TearDown();
  }

  void SetupWorkload(IsolationLevel isolation_level) {
    workload_.reset(new TestWorkload(cluster_.get()));
    workload_->set_timeout_allowed(true);
    workload_->set_payload_bytes(kPayloadBytes);
    workload_->set_write_batch_size(1);
    workload_->set_num_write_threads(4);
    workload_->set_num_tablets(kNumTablets);
    workload_->set_transactional(isolation_level, transaction_pool_.get());
    workload_->set_ttl(ttl_to_use());
    workload_->Setup();
  }

 protected:

  // -1 implies no ttl.
  virtual int ttl_to_use() {
    return -1;
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

  void WriteAtLeastFilesPerDb(int num_files) {
    auto dbs = GetAllRocksDbs(cluster_.get());
    workload_->Start();
    AssertLoggedWaitFor(
        [this, &dbs, num_files] {
            for (auto* db : dbs) {
              if (rocksdb_listener_->GetNumFlushesCompleted(db
              ) < num_files) {
                return false;
              }
            }
            return true;
          }, 60s,
        Format("Waiting until we've written at least $0 files per rocksdb ...", num_files),
        kWaitDelay);
    workload_->StopAndJoin();
    LOG(INFO) << "Wrote " << BytesWritten() << " bytes.";
  }

  void TestCompactionAfterTruncate();

  std::unique_ptr<MiniCluster> cluster_;
  std::unique_ptr<client::YBClient> client_;
  server::ClockPtr clock_{new server::HybridClock()};
  std::unique_ptr<client::TransactionManager> transaction_manager_;
  std::unique_ptr<client::TransactionPool> transaction_pool_;
  std::unique_ptr<TestWorkload> workload_;
  std::shared_ptr<RocksDbListener> rocksdb_listener_;
};

void CompactionTest::TestCompactionAfterTruncate() {
  // Write some data before truncate to make sure truncate wouldn't be noop.
  WriteAtLeast(kMemStoreSize * kNumTablets * 1.2);

  const auto table_info = ASSERT_RESULT(FindTable(cluster_.get(), workload_->table_name()));
  ASSERT_OK(workload_->client().TruncateTable(table_info->id(), true /* wait */));

  rocksdb_listener_->Reset();
  // Write enough to trigger compactions.
  WriteAtLeastFilesPerDb(FLAGS_rocksdb_level0_file_num_compaction_trigger + 1);

  auto dbs = GetAllRocksDbs(cluster_.get());
  AssertLoggedWaitFor(
      [&dbs] {
        for (auto* db : dbs) {
          if (db->GetLiveFilesMetaData().size() >
              FLAGS_rocksdb_level0_file_num_compaction_trigger) {
            return false;
          }
        }
        return true;
      },
      60s, "Waiting until we have number of SST files not higher than threshold ...", kWaitDelay);
}

TEST_F(CompactionTest, CompactionAfterTruncate) {
  SetupWorkload(IsolationLevel::NON_TRANSACTIONAL);
  TestCompactionAfterTruncate();
}

TEST_F(CompactionTest, CompactionAfterTruncateTransactional) {
  SetupWorkload(IsolationLevel::SNAPSHOT_ISOLATION);
  TestCompactionAfterTruncate();
}

class CompactionTestWithTTL : public CompactionTest {
 protected:
  int ttl_to_use() override {
    return kTTLSec;
  }
  const int kTTLSec = 1;
};

TEST_F(CompactionTestWithTTL, CompactionAfterExpiry) {
  FLAGS_timestamp_history_retention_interval_sec = 0;
  FLAGS_rocksdb_level0_file_num_compaction_trigger = 10;
  SetupWorkload(IsolationLevel::NON_TRANSACTIONAL);

  rocksdb_listener_->Reset();
  auto dbs = GetAllRocksDbs(cluster_.get(), false);

  // Write enough to be short of triggering compactions.
  WriteAtLeastFilesPerDb(0.8 * FLAGS_rocksdb_level0_file_num_compaction_trigger);
  size_t size_before_compaction = 0;
  for (auto* db : dbs) {
    size_before_compaction += db->GetCurrentVersionSstFilesUncompressedSize();
  }
  LOG(INFO) << "size_before_compaction is " << size_before_compaction;

  LOG(INFO) << "Sleeping";
  SleepFor(MonoDelta::FromSeconds(2 * kTTLSec));

  // Write enough to trigger compactions.
  WriteAtLeastFilesPerDb(FLAGS_rocksdb_level0_file_num_compaction_trigger);

  AssertLoggedWaitFor(
      [&dbs] {
        for (auto* db : dbs) {
          if (db->GetLiveFilesMetaData().size() >
              FLAGS_rocksdb_level0_file_num_compaction_trigger) {
            return false;
          }
        }
        return true;
      },
      60s, "Waiting until we have number of SST files not higher than threshold ...", kWaitDelay);

  // Assert that the data size is smaller now.
  size_t size_after_compaction = 0;
  for (auto* db : dbs) {
    size_after_compaction += db->GetCurrentVersionSstFilesUncompressedSize();
  }
  LOG(INFO) << "size_after_compaction is " << size_after_compaction;
  EXPECT_LT(size_after_compaction, size_before_compaction);

  SleepFor(MonoDelta::FromSeconds(2 * kTTLSec));

  constexpr int kCompactionTimeoutSec = 60;
  const auto table_info = ASSERT_RESULT(FindTable(cluster_.get(), workload_->table_name()));
  ASSERT_OK(workload_->client().FlushTables(
    {table_info->id()}, false, kCompactionTimeoutSec, /* compaction */ true));
  // Assert that the data size is all wiped up now.
  size_t size_after_manual_compaction = 0;
  for (auto* db : dbs) {
    size_after_manual_compaction += db->GetCurrentVersionSstFilesUncompressedSize();
  }
  LOG(INFO) << "size_after_manual_compaction is " << size_after_manual_compaction;
  EXPECT_EQ(size_after_manual_compaction, 0);
}

} // namespace tserver
} // namespace yb
