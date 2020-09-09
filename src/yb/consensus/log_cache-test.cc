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

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "yb/common/wire_protocol-test-util.h"
#include "yb/consensus/consensus-test-util.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_cache.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/bind_helpers.h"
#include "yb/gutil/stl_util.h"
#include "yb/server/hybrid_clock.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"
#include "yb/util/test_util.h"

using std::atomic;
using std::shared_ptr;
using std::thread;

DECLARE_int32(log_cache_size_limit_mb);
DECLARE_int32(global_log_cache_size_limit_mb);

METRIC_DECLARE_entity(tablet);

using std::atomic;
using std::vector;
using std::thread;
using namespace std::chrono_literals;

namespace yb {
namespace consensus {

static const char* kPeerUuid = "leader";
static const char* kTestTable = "test-table";
static const char* kTestTablet = "test-tablet";

constexpr int kNumMessages = 100;
constexpr int kMessageIndex1 = 60;
constexpr int kMessageIndex2 = 80;

std::string OpIdToString(const yb::OpId& opid) {
  return Format("$0.$1", opid.term, opid.index);
}

class LogCacheTest : public YBTest {
 public:
  LogCacheTest()
    : schema_(GetSimpleTestSchema()),
      metric_entity_(METRIC_ENTITY_tablet.Instantiate(&metric_registry_, "LogCacheTest")) {
  }

  void SetUp() override {
    YBTest::SetUp();
    fs_manager_.reset(new FsManager(env_.get(), GetTestPath("fs_root"), "tserver_test"));
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
    ASSERT_OK(ThreadPoolBuilder("log").Build(&log_thread_pool_));
    ASSERT_OK(log::Log::Open(log::LogOptions(),
                            kTestTablet,
                            fs_manager_->GetFirstTabletWalDirOrDie(kTestTable, kTestTablet),
                            fs_manager_->uuid(),
                            schema_,
                            0, // schema_version
                            NULL,
                            log_thread_pool_.get(),
                            log_thread_pool_.get(),
                            std::numeric_limits<int64_t>::max(), // cdc_min_replicated_index
                            &log_));

    CloseAndReopenCache(MinimumOpId());
    clock_.reset(new server::HybridClock());
    ASSERT_OK(clock_->Init());
  }

  void TearDown() override {
    ASSERT_OK(log_->WaitUntilAllFlushed());
  }

  void CloseAndReopenCache(const OpIdPB& preceding_id) {
    // Blow away the memtrackers before creating the new cache.
    cache_.reset();

    cache_.reset(new LogCache(
        metric_entity_, log_.get(), nullptr /* mem_tracker */, kPeerUuid, kTestTablet));
    cache_->Init(preceding_id);
  }

 protected:
  static void FatalOnError(const Status& s) {
    ASSERT_OK(s);
  }

  Status AppendReplicateMessagesToCache(int64_t first, int64_t count, size_t payload_size = 0) {
    for (int64_t cur_index = first; cur_index < first + count; cur_index++) {
      int64_t term = cur_index / kTermDivisor;
      int64_t index = cur_index;
      ReplicateMsgs msgs = { CreateDummyReplicate(term, index, clock_->Now(), payload_size) };
      RETURN_NOT_OK(cache_->AppendOperations(
          msgs, yb::OpId() /* committed_op_id */, RestartSafeCoarseMonoClock().Now(),
          Bind(&FatalOnError)));
      cache_->TrackOperationsMemory({yb::OpId::FromPB(msgs[0]->id())});
      std::this_thread::sleep_for(100ms);
    }
    return Status::OK();
  }

  const Schema schema_;
  MetricRegistry metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  gscoped_ptr<FsManager> fs_manager_;
  std::unique_ptr<ThreadPool> log_thread_pool_;
  gscoped_ptr<LogCache> cache_;
  scoped_refptr<log::Log> log_;
  scoped_refptr<server::Clock> clock_;
};

TEST_F(LogCacheTest, TestAppendAndGetMessages) {
  ASSERT_EQ(0, cache_->metrics_.num_ops->value());
  ASSERT_EQ(0, cache_->metrics_.size->value());
  ASSERT_OK(AppendReplicateMessagesToCache(1, kNumMessages));
  ASSERT_EQ(kNumMessages, cache_->metrics_.num_ops->value());
  ASSERT_GE(cache_->metrics_.size->value(), 5 * kNumMessages);
  ASSERT_OK(log_->WaitUntilAllFlushed());

  auto read_result = ASSERT_RESULT(cache_->ReadOps(0, 8_MB));
  EXPECT_EQ(kNumMessages, read_result.messages.size());
  EXPECT_EQ(OpIdStrForIndex(0), OpIdToString(read_result.preceding_op));

  // Get starting in the middle of the cache.
  read_result = ASSERT_RESULT(cache_->ReadOps(kMessageIndex1, 8_MB));
  EXPECT_EQ(kNumMessages - kMessageIndex1, read_result.messages.size());
  EXPECT_EQ(OpIdStrForIndex(kMessageIndex1), OpIdToString(read_result.preceding_op));
  EXPECT_EQ(OpIdStrForIndex(kMessageIndex1 + 1), OpIdToString(read_result.messages[0]->id()));

  // Get at the end of the cache.
  read_result = ASSERT_RESULT(cache_->ReadOps(kNumMessages, 8_MB));
  EXPECT_EQ(0, read_result.messages.size());
  EXPECT_EQ(OpIdStrForIndex(kNumMessages), OpIdToString(read_result.preceding_op));

  // Get messages from the beginning until some point in the middle of the cache.
  read_result = ASSERT_RESULT(cache_->ReadOps(0, kMessageIndex1, 8_MB));
  EXPECT_EQ(kMessageIndex1, read_result.messages.size());
  EXPECT_EQ(OpIdStrForIndex(0), OpIdToString(read_result.preceding_op));
  EXPECT_EQ(OpIdStrForIndex(1), OpIdToString(read_result.messages[0]->id()));

  // Get messages from some point in the middle of the cache until another point.
  read_result = ASSERT_RESULT(cache_->ReadOps(kMessageIndex1, kMessageIndex2, 8_MB));
  EXPECT_EQ(kMessageIndex2 - kMessageIndex1, read_result.messages.size());
  EXPECT_EQ(OpIdStrForIndex(kMessageIndex1), OpIdToString(read_result.preceding_op));
  EXPECT_EQ(OpIdStrForIndex(kMessageIndex1 + 1), OpIdToString(read_result.messages[0]->id()));

  // Evict some and verify that the eviction took effect.
  cache_->EvictThroughOp(kNumMessages / 2);
  ASSERT_EQ(kNumMessages / 2, cache_->metrics_.num_ops->value());

  // Can still read data that was evicted, since it got written through.
  int start = (kNumMessages / 2) - 10;
  read_result = ASSERT_RESULT(cache_->ReadOps(start, 8_MB));
  EXPECT_EQ(kNumMessages - start, read_result.messages.size());
  EXPECT_EQ(OpIdStrForIndex(start), OpIdToString(read_result.preceding_op));
  EXPECT_EQ(OpIdStrForIndex(start + 1), OpIdToString(read_result.messages[0]->id()));
}


// Ensure that the cache always yields at least one message,
// even if that message is larger than the batch size. This ensures
// that we don't get "stuck" in the case that a large message enters
// the cache.
TEST_F(LogCacheTest, TestAlwaysYieldsAtLeastOneMessage) {
  // generate a 2MB dummy payload
  const int kPayloadSize = 2_MB;

  // Append several large ops to the cache
  ASSERT_OK(AppendReplicateMessagesToCache(1, 4, kPayloadSize));
  ASSERT_OK(log_->WaitUntilAllFlushed());

  // We should get one of them, even though we only ask for 100 bytes
  auto read_result = ASSERT_RESULT(cache_->ReadOps(0, 100));
  ASSERT_EQ(1, read_result.messages.size());

  // Should yield one op also in the 'cache miss' case.
  cache_->EvictThroughOp(50);
  read_result = ASSERT_RESULT(cache_->ReadOps(0, 100));
  ASSERT_EQ(1, read_result.messages.size());
}

// Tests that the cache returns STATUS(NotFound, "") if queried for messages after an
// index that is higher than it's latest, returns an empty set of messages when queried for
// the last index and returns all messages when queried for MinimumOpId().
TEST_F(LogCacheTest, TestCacheEdgeCases) {
  // Append 1 message to the cache
  ASSERT_OK(AppendReplicateMessagesToCache(1, 1));
  ASSERT_OK(log_->WaitUntilAllFlushed());

  // Test when the searched index is MinimumOpId().index().
  auto read_result = ASSERT_RESULT(cache_->ReadOps(0, 100));
  ASSERT_EQ(1, read_result.messages.size());
  ASSERT_EQ(yb::OpId(0, 0), read_result.preceding_op);

  // Test when 'after_op_index' is the last index in the cache.
  read_result = ASSERT_RESULT(cache_->ReadOps(1, 100));
  ASSERT_EQ(0, read_result.messages.size());
  ASSERT_EQ(yb::OpId(0, 1), read_result.preceding_op);

  // Now test the case when 'after_op_index' is after the last index
  // in the cache.
  auto failed_result = cache_->ReadOps(2, 100);
  ASSERT_FALSE(failed_result.ok());
  ASSERT_TRUE(failed_result.status().IsIncomplete())
      << "unexpected status: " << failed_result.status();

  // Evict entries from the cache, and ensure that we can still read
  // entries at the beginning of the log.
  cache_->EvictThroughOp(50);
  read_result = ASSERT_RESULT(cache_->ReadOps(0, 100));
  ASSERT_EQ(1, read_result.messages.size());
  ASSERT_EQ(yb::OpId(0, 0), read_result.preceding_op);
}


TEST_F(LogCacheTest, TestMemoryLimit) {
  FLAGS_log_cache_size_limit_mb = 1;
  CloseAndReopenCache(MinimumOpId());

  const int kPayloadSize = 400_KB;
  // Limit should not be violated.
  ASSERT_OK(AppendReplicateMessagesToCache(1, 1, kPayloadSize));
  ASSERT_OK(log_->WaitUntilAllFlushed());
  ASSERT_EQ(1, cache_->num_cached_ops());

  // Verify the size is right. It's not exactly kPayloadSize because of in-memory
  // overhead, etc.
  int size_with_one_msg = cache_->BytesUsed();
  ASSERT_GT(size_with_one_msg, 300_KB);
  ASSERT_LT(size_with_one_msg, 500_KB);

  // Add another operation which fits under the 1MB limit.
  ASSERT_OK(AppendReplicateMessagesToCache(2, 1, kPayloadSize));
  ASSERT_OK(log_->WaitUntilAllFlushed());
  ASSERT_EQ(2, cache_->num_cached_ops());

  int size_with_two_msgs = cache_->BytesUsed();
  ASSERT_GT(size_with_two_msgs, 2 * 300_KB);
  ASSERT_LT(size_with_two_msgs, 2 * 500_KB);

  // Append a third operation, which will push the cache size above the 1MB limit
  // and cause eviction of the first operation.
  LOG(INFO) << "appending op 3";
  // Verify that we have trimmed by appending a message that would
  // otherwise be rejected, since the cache max size limit is 2MB.
  ASSERT_OK(AppendReplicateMessagesToCache(3, 1, kPayloadSize));
  ASSERT_OK(log_->WaitUntilAllFlushed());
  ASSERT_EQ(2, cache_->num_cached_ops());
  ASSERT_EQ(size_with_two_msgs, cache_->BytesUsed());

  // Test explicitly evicting one of the ops.
  cache_->EvictThroughOp(2);
  ASSERT_EQ(1, cache_->num_cached_ops());
  ASSERT_EQ(size_with_one_msg, cache_->BytesUsed());

  // Explicitly evict the last op.
  cache_->EvictThroughOp(3);
  ASSERT_EQ(0, cache_->num_cached_ops());
  ASSERT_EQ(cache_->BytesUsed(), 0);
}

TEST_F(LogCacheTest, TestGlobalMemoryLimit) {
  FLAGS_global_log_cache_size_limit_mb = 4;
  CloseAndReopenCache(MinimumOpId());

  // Exceed the global hard limit.
  ScopedTrackedConsumption consumption(cache_->parent_tracker_, 3_MB);

  const int kPayloadSize = 768_KB;

  // Should succeed, but only end up caching one of the two ops because of the global limit.
  ASSERT_OK(AppendReplicateMessagesToCache(1, 2, kPayloadSize));
  ASSERT_OK(log_->WaitUntilAllFlushed());

  ASSERT_EQ(1, cache_->num_cached_ops());
  ASSERT_LE(cache_->BytesUsed(), 1_MB);
}

// Test that the log cache properly replaces messages when an index
// is reused. This is a regression test for a bug where the memtracker's
// consumption wasn't properly managed when messages were replaced.
TEST_F(LogCacheTest, TestReplaceMessages) {
  const int kPayloadSize = 128_KB;
  shared_ptr<MemTracker> tracker = cache_->tracker_;;
  ASSERT_EQ(0, tracker->consumption());

  ASSERT_OK(AppendReplicateMessagesToCache(1, 1, kPayloadSize));
  int size_with_one_msg = tracker->consumption();

  for (int i = 0; i < 10; i++) {
    ASSERT_OK(AppendReplicateMessagesToCache(1, 1, kPayloadSize));
  }

  ASSERT_OK(log_->WaitUntilAllFlushed());

  EXPECT_EQ(size_with_one_msg, tracker->consumption());
  EXPECT_EQ(Substitute("Pinned index: 2, LogCacheStats(num_ops=1, bytes=$0, disk_reads=0)",
                       size_with_one_msg),
            cache_->ToString());
}

TEST_F(LogCacheTest, TestMTReadAndWrite) {
  atomic<bool> stop { false };
  bool stopped = false;
  atomic<int64_t> num_appended{0};
  atomic<int64_t> next_index{0};
  vector<thread> threads;

  auto stop_workload = [&]() {
    if (!stopped) {
      LOG(INFO) << "Stopping workload";
      stop = true;
      for (auto& t : threads) {
        t.join();
      }
      stopped = true;
      LOG(INFO) << "Workload stopped";
    }
  };

  auto se = ScopeExit(stop_workload);

  // Add a writer thread.
  threads.emplace_back([&] {
    const int kBatch = 10;
    int64_t index = 1;
    while (!stop) {
      auto append_status = AppendReplicateMessagesToCache(index, kBatch);
      if (append_status.IsServiceUnavailable()) {
        std::this_thread::sleep_for(10ms);
        continue;
      }
      index += kBatch;
      next_index = index;
      num_appended++;
    }
  });

  // Add a reader thread.
  threads.emplace_back([&] {
    int64_t index = 0;
    while (!stop) {
      if (index >= next_index) {
        // We've gone ahead of the writer.
        std::this_thread::sleep_for(5ms);
        continue;
      }
      auto read_result = ASSERT_RESULT(cache_->ReadOps(index, 1_MB));
      index += read_result.messages.size();
    }
  });

  LOG(INFO) << "Starting the workload";
  std::this_thread::sleep_for(5s);
  stop_workload();
  ASSERT_GT(num_appended, 0);
}

} // namespace consensus
} // namespace yb
