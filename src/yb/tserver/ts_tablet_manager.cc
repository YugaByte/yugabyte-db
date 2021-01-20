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

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/optional/optional.hpp>

#include <glog/logging.h>

#include "yb/client/client.h"

#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/metadata.pb.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/quorum_util.h"
#include "yb/consensus/retryable_requests.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/docdb/consensus_frontier.h"

#include "yb/fs/fs_manager.h"

#include "yb/gutil/strings/human_readable.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/strings/util.h"
#include "yb/gutil/sysinfo.h"

#include "yb/master/master.pb.h"
#include "yb/master/sys_catalog.h"

#include "yb/rocksdb/memory_monitor.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/poller.h"

#include "yb/tablet/metadata.pb.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet.pb.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_fwd.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/tablet_options.h"
#include "yb/tablet/operations/split_operation.h"

#include "yb/tserver/heartbeater.h"
#include "yb/tserver/remote_bootstrap_client.h"
#include "yb/tserver/remote_bootstrap_session.h"
#include "yb/tserver/remote_bootstrap_snapshots.h"
#include "yb/tserver/tablet_server.h"

#include "yb/util/background_task.h"
#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/env.h"
#include "yb/util/env_util.h"
#include "yb/util/fault_injection.h"
#include "yb/util/file_util.h"
#include "yb/util/flag_tags.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/pb_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/stopwatch.h"
#include "yb/util/trace.h"
#include "yb/util/tsan_util.h"
#include "yb/util/shared_lock.h"

using namespace std::literals;
using namespace std::placeholders;

DEFINE_int32(num_tablets_to_open_simultaneously, 0,
             "Number of threads available to open tablets during startup. If this "
             "is set to 0 (the default), then the number of bootstrap threads will "
             "be set based on the number of data directories. If the data directories "
             "are on some very fast storage device such as SSD or a RAID array, it "
             "may make sense to manually tune this.");
TAG_FLAG(num_tablets_to_open_simultaneously, advanced);

DEFINE_int32(tablet_start_warn_threshold_ms, 500,
             "If a tablet takes more than this number of millis to start, issue "
             "a warning with a trace.");
TAG_FLAG(tablet_start_warn_threshold_ms, hidden);

DEFINE_int32(db_block_cache_num_shard_bits, 4,
             "Number of bits to use for sharding the block cache (defaults to 4 bits)");
TAG_FLAG(db_block_cache_num_shard_bits, advanced);

DEFINE_bool(enable_log_cache_gc, true,
            "Set to true to enable log cache garbage collector.");

DEFINE_bool(log_cache_gc_evict_only_over_allocated, true,
            "If set to true, log cache garbage collection would evict only memory that was "
            "allocated over limit for log cache. Otherwise it will try to evict requested number "
            "of bytes.");

DEFINE_bool(enable_block_based_table_cache_gc, false,
            "Set to true to enable block based table garbage collector.");

DEFINE_int32(cleanup_split_tablets_interval_sec, 60,
             "Interval at which tablet manager tries to cleanup split tablets which are no longer "
             "needed. Setting this to 0 disables cleanup of split tablets.");

DEFINE_test_flag(double, fault_crash_after_blocks_deleted, 0.0,
                 "Fraction of the time when the tablet will crash immediately "
                 "after deleting the data blocks during tablet deletion.");

DEFINE_test_flag(double, fault_crash_after_wal_deleted, 0.0,
                 "Fraction of the time when the tablet will crash immediately "
                 "after deleting the WAL segments during tablet deletion.");

DEFINE_test_flag(double, fault_crash_after_cmeta_deleted, 0.0,
                 "Fraction of the time when the tablet will crash immediately "
                 "after deleting the consensus metadata during tablet deletion.");

DEFINE_test_flag(double, fault_crash_after_rb_files_fetched, 0.0,
                 "Fraction of the time when the tablet will crash immediately "
                 "after fetching the files during a remote bootstrap but before "
                 "marking the superblock as TABLET_DATA_READY.")

DEFINE_test_flag(bool, pretend_memory_exceeded_enforce_flush, false,
                 "Always pretend memory has been exceeded to enforce background flush.");

DEFINE_test_flag(int32, crash_if_remote_bootstrap_sessions_greater_than, 0,
                 "If greater than zero, this process will crash if we detect more than the "
                 "specified number of remote bootstrap sessions.");

DEFINE_test_flag(int32, crash_if_remote_bootstrap_sessions_per_table_greater_than, 0,
                 "If greater than zero, this process will crash if for any table we exceed the "
                 "specified number of remote bootstrap sessions");

DEFINE_test_flag(bool, force_single_tablet_failure, false,
                 "Force exactly one tablet to a failed state.");

DEFINE_test_flag(int32, apply_tablet_split_inject_delay_ms, 0,
                 "Inject delay into TSTabletManager::ApplyTabletSplit.");

DEFINE_test_flag(bool, skip_deleting_split_tablets, false,
                 "Skip deleting tablets which have been split.");

namespace {

constexpr int kDbCacheSizeUsePercentage = -1;
constexpr int kDbCacheSizeCacheDisabled = -2;

} // namespace

DEFINE_int32(flush_background_task_interval_msec, 0,
             "The tick interval time for the flush background task. "
             "This defaults to 0, which means disable the background task "
             "And only use callbacks on memstore allocations. ");

DEFINE_int64(global_memstore_size_percentage, 10,
             "Percentage of total available memory to use for the global memstore. "
             "Default is 10. See also memstore_size_mb and "
             "global_memstore_size_mb_max.");
DEFINE_int64(global_memstore_size_mb_max, 2048,
             "Global memstore size is determined as a percentage of the available "
             "memory. However, this flag limits it in absolute size. Value of 0 "
             "means no limit on the value obtained by the percentage. Default is 2048.");

DEFINE_int64(db_block_cache_size_bytes, kDbCacheSizeUsePercentage,
             "Size of cross-tablet shared RocksDB block cache (in bytes). "
             "This defaults to -1 for system auto-generated default, which would use "
             "FLAGS_db_block_cache_ram_percentage to select a percentage of the total memory as "
             "the default size for the shared block cache. Value of -2 disables block cache.");

DEFINE_int32(db_block_cache_size_percentage, 50,
             "Default percentage of total available memory to use as block cache size, if not "
             "asking for a raw number, through FLAGS_db_block_cache_size_bytes.");

DEFINE_int32(read_pool_max_threads, 128,
             "The maximum number of threads allowed for read_pool_. This pool is used "
             "to run multiple read operations, that are part of the same tablet rpc, "
             "in parallel.");
DEFINE_int32(read_pool_max_queue_size, 128,
             "The maximum number of tasks that can be held in the queue for read_pool_. This pool "
             "is used to run multiple read operations, that are part of the same tablet rpc, "
             "in parallel.");

DEFINE_int32(post_split_trigger_compaction_pool_max_threads, 1,
             "The maximum number of threads allowed for post_split_trigger_compaction_pool_. This "
             "pool is used to run compactions on tablets after they have been split and still "
             "contain irrelevant data from the tablet they were sourced from.");
DEFINE_int32(post_split_trigger_compaction_pool_max_queue_size, 16,
             "The maximum number of tasks that can be held in the pool for "
             "post_split_trigger_compaction_pool_. This pool is used to run compactions on tablets "
             "after they have been split and still contain irrelevant data from the tablet they "
             "were sourced from.");

DEFINE_test_flag(int32, sleep_after_tombstoning_tablet_secs, 0,
                 "Whether we sleep in LogAndTombstone after calling DeleteTabletData.");

constexpr int kTServerYbClientDefaultTimeoutMs = yb::RegularBuildVsSanitizers(5, 60) * 1000;

DEFINE_int32(tserver_yb_client_default_timeout_ms, kTServerYbClientDefaultTimeoutMs,
             "Default timeout for the YBClient embedded into the tablet server that is used "
             "for distributed transactions.");

DEFINE_bool(enable_restart_transaction_status_tablets_first, true,
            "Set to true to prioritize bootstrapping transaction status tablets first.");

namespace yb {
namespace tserver {

METRIC_DEFINE_histogram(server, op_apply_queue_length, "Operation Apply Queue Length",
                        MetricUnit::kTasks,
                        "Number of operations waiting to be applied to the tablet. "
                        "High queue lengths indicate that the server is unable to process "
                        "operations as fast as they are being written to the WAL.",
                        10000, 2);

METRIC_DEFINE_histogram(server, op_apply_queue_time, "Operation Apply Queue Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent waiting in the apply queue before being "
                        "processed. High queue times indicate that the server is unable to "
                        "process operations as fast as they are being written to the WAL.",
                        10000000, 2);

METRIC_DEFINE_histogram(server, op_apply_run_time, "Operation Apply Run Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent being applied to the tablet. "
                        "High values may indicate that the server is under-provisioned or "
                        "that operations consist of very large batches.",
                        10000000, 2);

METRIC_DEFINE_histogram(server, op_read_queue_length, "Operation Read op Queue Length",
                        MetricUnit::kTasks,
                        "Number of operations waiting to be applied to the tablet. "
                            "High queue lengths indicate that the server is unable to process "
                            "operations as fast as they are being written to the WAL.",
                        10000, 2);

METRIC_DEFINE_histogram(server, op_read_queue_time, "Operation Read op Queue Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent waiting in the read queue before being "
                            "processed. High queue times indicate that the server is unable to "
                            "process operations as fast as they are being written to the WAL.",
                        10000000, 2);

METRIC_DEFINE_histogram(server, op_read_run_time, "Operation Read op Run Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent being applied to the tablet. "
                            "High values may indicate that the server is under-provisioned or "
                            "that operations consist of very large batches.",
                        10000000, 2);

METRIC_DEFINE_histogram(server, ts_bootstrap_time, "TServer Bootstrap Time",
                        MetricUnit::kMicroseconds,
                        "Time that the tablet server takes to bootstrap all of its tablets.",
                        10000000, 2);

THREAD_POOL_METRICS_DEFINE(
    server, post_split_trigger_compaction_pool, "Thread pool for tablet compaction jobs.");

using consensus::ConsensusMetadata;
using consensus::ConsensusStatePB;
using consensus::RaftConfigPB;
using consensus::RaftPeerPB;
using consensus::StartRemoteBootstrapRequestPB;
using log::Log;
using master::ReportedTabletPB;
using master::TabletReportPB;
using master::TabletReportUpdatesPB;
using std::shared_ptr;
using std::string;
using std::unordered_set;
using std::vector;
using strings::Substitute;
using tablet::BOOTSTRAPPING;
using tablet::NOT_STARTED;
using tablet::RaftGroupMetadata;
using tablet::RaftGroupMetadataPtr;
using tablet::RaftGroupStatePB;
using tablet::RUNNING;
using tablet::TABLET_DATA_COPYING;
using tablet::TABLET_DATA_DELETED;
using tablet::TABLET_DATA_READY;
using tablet::TABLET_DATA_SPLIT_COMPLETED;
using tablet::TABLET_DATA_TOMBSTONED;
using tablet::TabletDataState;
using tablet::TabletPeer;
using tablet::TabletPeerPtr;
using tablet::TabletStatusListener;
using tablet::TabletStatusPB;

// Only called from the background task to ensure it's synchronized
void TSTabletManager::MaybeFlushTablet() {
  int iteration = 0;
  while (memory_monitor()->Exceeded() ||
         (iteration++ == 0 && FLAGS_TEST_pretend_memory_exceeded_enforce_flush)) {
    YB_LOG_EVERY_N_SECS(INFO, 5) << Format("Memstore global limit of $0 bytes reached, looking for "
                                           "tablet to flush", memory_monitor()->limit());
    auto flush_tick = rocksdb::FlushTick();
    TabletPeerPtr tablet_to_flush = TabletToFlush();
    // TODO(bojanserafimov): If tablet_to_flush flushes now because of other reasons,
    // we will schedule a second flush, which will unnecessarily stall writes for a short time. This
    // will not happen often, but should be fixed.
    if (tablet_to_flush) {
      LOG(INFO)
          << TabletLogPrefix(tablet_to_flush->tablet_id())
          << "Flushing tablet with oldest memstore write at "
          << tablet_to_flush->tablet()->OldestMutableMemtableWriteHybridTime();
      WARN_NOT_OK(
          tablet_to_flush->tablet()->Flush(
              tablet::FlushMode::kAsync, tablet::FlushFlags::kAll, flush_tick),
          Substitute("Flush failed on $0", tablet_to_flush->tablet_id()));
      for (auto listener : TEST_listeners) {
        listener->StartedFlush(tablet_to_flush->tablet_id());
      }
    }
  }
}

// Return the tablet with the oldest write in memstore, or nullptr if all tablet memstores are
// empty or about to flush.
TabletPeerPtr TSTabletManager::TabletToFlush() {
  SharedLock<RWMutex> lock(mutex_); // For using the tablet map
  HybridTime oldest_write_in_memstores = HybridTime::kMax;
  TabletPeerPtr tablet_to_flush;
  for (const TabletMap::value_type& entry : tablet_map_) {
    const auto tablet = entry.second->shared_tablet();
    if (tablet) {
      const auto ht = tablet->OldestMutableMemtableWriteHybridTime();
      if (ht.ok()) {
        if (*ht < oldest_write_in_memstores) {
          oldest_write_in_memstores = *ht;
          tablet_to_flush = entry.second;
        }
      } else {
        YB_LOG_EVERY_N_SECS(WARNING, 5) << Format(
            "Failed to get oldest mutable memtable write ht for tablet $0: $1",
            tablet->tablet_id(), ht.status());
      }
    }
  }
  return tablet_to_flush;
}

namespace {

class LRUCacheGC : public GarbageCollector {
 public:
  explicit LRUCacheGC(std::shared_ptr<rocksdb::Cache> cache) : cache_(std::move(cache)) {}

  void CollectGarbage(size_t required) {
    if (!FLAGS_enable_block_based_table_cache_gc) {
      return;
    }

    auto evicted = cache_->Evict(required);
    LOG(INFO) << "Evicted from table cache: " << HumanReadableNumBytes::ToString(evicted)
              << ", new usage: " << HumanReadableNumBytes::ToString(cache_->GetUsage())
              << ", required: " << HumanReadableNumBytes::ToString(required);
  }

  virtual ~LRUCacheGC() = default;

 private:
  std::shared_ptr<rocksdb::Cache> cache_;
};

class FunctorGC : public GarbageCollector {
 public:
  explicit FunctorGC(std::function<void(size_t)> impl) : impl_(std::move(impl)) {}

  void CollectGarbage(size_t required) {
    impl_(required);
  }

  virtual ~FunctorGC() = default;

 private:
  std::function<void(size_t)> impl_;
};

} // namespace

TSTabletManager::TSTabletManager(FsManager* fs_manager,
                                 TabletServer* server,
                                 MetricRegistry* metric_registry)
  : fs_manager_(fs_manager),
    server_(server),
    metric_registry_(metric_registry),
    state_(MANAGER_INITIALIZING) {
  ThreadPoolMetrics metrics = {
      METRIC_op_apply_queue_length.Instantiate(server_->metric_entity()),
      METRIC_op_apply_queue_time.Instantiate(server_->metric_entity()),
      METRIC_op_apply_run_time.Instantiate(server_->metric_entity())
  };
  CHECK_OK(ThreadPoolBuilder("apply")
               .set_metrics(std::move(metrics))
               .Build(&apply_pool_));

  // This pool is shared by all replicas hosted by this server.
  //
  // Some submitted tasks use blocking IO, so we configure no upper bound on
  // the maximum number of threads in each pool (otherwise the default value of
  // "number of CPUs" may cause blocking tasks to starve other "fast" tasks).
  // However, the effective upper bound is the number of replicas as each will
  // submit its own tasks via a dedicated token.
  CHECK_OK(ThreadPoolBuilder("consensus")
               .set_min_threads(1)
               .unlimited_threads()
               .Build(&raft_pool_));
  CHECK_OK(ThreadPoolBuilder("prepare")
               .set_min_threads(1)
               .unlimited_threads()
               .Build(&tablet_prepare_pool_));
  CHECK_OK(ThreadPoolBuilder("append")
               .set_min_threads(1)
               .unlimited_threads()
               .set_idle_timeout(MonoDelta::FromMilliseconds(10000))
               .Build(&append_pool_));
  CHECK_OK(ThreadPoolBuilder("log-alloc")
               .set_min_threads(1)
               .unlimited_threads()
               .Build(&allocation_pool_));
  ThreadPoolMetrics read_metrics = {
      METRIC_op_read_queue_length.Instantiate(server_->metric_entity()),
      METRIC_op_read_queue_time.Instantiate(server_->metric_entity()),
      METRIC_op_read_run_time.Instantiate(server_->metric_entity())
  };
  CHECK_OK(ThreadPoolBuilder("read-parallel")
               .set_max_threads(FLAGS_read_pool_max_threads)
               .set_max_queue_size(FLAGS_read_pool_max_queue_size)
               .set_metrics(std::move(read_metrics))
               .Build(&read_pool_));
  CHECK_OK(ThreadPoolBuilder("compaction")
              .set_max_threads(FLAGS_post_split_trigger_compaction_pool_max_threads)
              .set_max_queue_size(FLAGS_post_split_trigger_compaction_pool_max_queue_size)
              .set_metrics(THREAD_POOL_METRICS_INSTANCE(
                  server_->metric_entity(), post_split_trigger_compaction_pool))
              .Build(&post_split_trigger_compaction_pool_));

  int64_t block_cache_size_bytes = FLAGS_db_block_cache_size_bytes;
  int64_t total_ram_avail = MemTracker::GetRootTracker()->limit();
  // Auto-compute size of block cache if asked to.
  if (FLAGS_db_block_cache_size_bytes == kDbCacheSizeUsePercentage) {
    // Check some bounds.
    CHECK(FLAGS_db_block_cache_size_percentage > 0 && FLAGS_db_block_cache_size_percentage <= 100)
        << Substitute(
               "Flag tablet_block_cache_size_percentage must be between 0 and 100. Current value: "
               "$0",
               FLAGS_db_block_cache_size_percentage);

    block_cache_size_bytes = total_ram_avail * FLAGS_db_block_cache_size_percentage / 100;
  }

  block_based_table_mem_tracker_ = MemTracker::FindOrCreateTracker(
      block_cache_size_bytes, "BlockBasedTable", server_->mem_tracker());

  if (FLAGS_db_block_cache_size_bytes != kDbCacheSizeCacheDisabled) {
    tablet_options_.block_cache = rocksdb::NewLRUCache(block_cache_size_bytes,
                                                       FLAGS_db_block_cache_num_shard_bits);
    tablet_options_.block_cache->SetMetrics(server_->metric_entity());
    block_based_table_gc_ = std::make_shared<LRUCacheGC>(tablet_options_.block_cache);
    block_based_table_mem_tracker_->AddGarbageCollector(block_based_table_gc_);
  }

  auto log_cache_mem_tracker = consensus::LogCache::GetServerMemTracker(server_->mem_tracker());
  log_cache_gc_ = std::make_shared<FunctorGC>(
      std::bind(&TSTabletManager::LogCacheGC, this, log_cache_mem_tracker.get(), _1));
  log_cache_mem_tracker->AddGarbageCollector(log_cache_gc_);

  // Calculate memstore_size_bytes
  bool should_count_memory = FLAGS_global_memstore_size_percentage > 0;
  CHECK(FLAGS_global_memstore_size_percentage > 0 && FLAGS_global_memstore_size_percentage <= 100)
    << Substitute(
        "Flag tablet_block_cache_size_percentage must be between 0 and 100. Current value: "
        "$0",
        FLAGS_global_memstore_size_percentage);
  size_t memstore_size_bytes = total_ram_avail * FLAGS_global_memstore_size_percentage / 100;

  if (FLAGS_global_memstore_size_mb_max != 0) {
    memstore_size_bytes = std::min(memstore_size_bytes,
                                   static_cast<size_t>(FLAGS_global_memstore_size_mb_max << 20));
  }

  // Add memory monitor and background thread for flushing
  if (should_count_memory) {
    background_task_.reset(new BackgroundTask(
      std::function<void()>([this](){ MaybeFlushTablet(); }),
      "tablet manager",
      "flush scheduler bgtask",
      std::chrono::milliseconds(FLAGS_flush_background_task_interval_msec)));
    tablet_options_.memory_monitor = std::make_shared<rocksdb::MemoryMonitor>(
        memstore_size_bytes,
        std::function<void()>([this](){
                                YB_WARN_NOT_OK(background_task_->Wake(), "Wakeup error"); }));
  }
}

TSTabletManager::~TSTabletManager() {
}

Status TSTabletManager::Init() {
  CHECK_EQ(state(), MANAGER_INITIALIZING);

  async_client_init_.emplace(
      "tserver_client", 0 /* num_reactors */,
      FLAGS_tserver_yb_client_default_timeout_ms / 1000, server_->permanent_uuid(),
      &server_->options(), server_->metric_entity(), server_->mem_tracker(),
      server_->messenger());

  async_client_init_->AddPostCreateHook([this](client::YBClient* client) {
    auto* tserver = server();
    if (tserver != nullptr && tserver->proxy() != nullptr) {
      client->SetLocalTabletServer(tserver->permanent_uuid(), tserver->proxy(), tserver);
    }
  });

  tablet_options_.env = server_->GetEnv();
  tablet_options_.rocksdb_env = server_->GetRocksDBEnv();
  tablet_options_.listeners = server_->options().listeners;

  // Start the threadpool we'll use to open tablets.
  // This has to be done in Init() instead of the constructor, since the
  // FsManager isn't initialized until this point.
  int max_bootstrap_threads = FLAGS_num_tablets_to_open_simultaneously;
  if (max_bootstrap_threads == 0) {
    size_t num_cpus = base::NumCPUs();
    if (num_cpus <= 2) {
      max_bootstrap_threads = 2;
    } else {
      max_bootstrap_threads = min(num_cpus - 1, fs_manager_->GetDataRootDirs().size() * 8);
    }
    LOG_WITH_PREFIX(INFO) <<  "max_bootstrap_threads=" << max_bootstrap_threads;
  }
  ThreadPoolMetrics bootstrap_metrics = {
          NULL,
          NULL,
          METRIC_ts_bootstrap_time.Instantiate(server_->metric_entity())
  };
  RETURN_NOT_OK(ThreadPoolBuilder("tablet-bootstrap")
                .set_max_threads(max_bootstrap_threads)
                .set_metrics(std::move(bootstrap_metrics))
                .Build(&open_tablet_pool_));

  CleanupCheckpoints();

  // Search for tablets in the metadata dir.
  vector<string> tablet_ids;
  RETURN_NOT_OK(fs_manager_->ListTabletIds(&tablet_ids));

  InitLocalRaftPeerPB();

  deque<RaftGroupMetadataPtr> metas;

  // First, load all of the tablet metadata. We do this before we start
  // submitting the actual OpenTablet() tasks so that we don't have to compete
  // for disk resources, etc, with bootstrap processes and running tablets.
  MonoTime start(MonoTime::Now());
  for (const string& tablet_id : tablet_ids) {
    RaftGroupMetadataPtr meta;
    RETURN_NOT_OK_PREPEND(OpenTabletMeta(tablet_id, &meta),
                          "Failed to open tablet metadata for tablet: " + tablet_id);
    if (PREDICT_FALSE(!CanServeTabletData(meta->tablet_data_state()))) {
      RETURN_NOT_OK(HandleNonReadyTabletOnStartup(meta));
      continue;
    }
    RegisterDataAndWalDir(
        fs_manager_, meta->table_id(), meta->raft_group_id(), meta->data_root_dir(),
        meta->wal_root_dir());
    if (FLAGS_enable_restart_transaction_status_tablets_first) {
      // Prioritize bootstrapping transaction status tablets first.
      if (meta->table_type() == TRANSACTION_STATUS_TABLE_TYPE) {
        metas.push_front(meta);
      } else {
        metas.push_back(meta);
      }
    } else {
      metas.push_back(meta);
    }
  }

  MonoDelta elapsed = MonoTime::Now().GetDeltaSince(start);
  LOG(INFO) << "Loaded metadata for " << tablet_ids.size() << " tablet in "
            << elapsed.ToMilliseconds() << " ms";

  // Now submit the "Open" task for each.
  for (const RaftGroupMetadataPtr& meta : metas) {
    scoped_refptr<TransitionInProgressDeleter> deleter;
    RETURN_NOT_OK(StartTabletStateTransition(
        meta->raft_group_id(), "opening tablet", &deleter));

    TabletPeerPtr tablet_peer = VERIFY_RESULT(CreateAndRegisterTabletPeer(meta, NEW_PEER));
    RETURN_NOT_OK(open_tablet_pool_->SubmitFunc(
        std::bind(&TSTabletManager::OpenTablet, this, meta, deleter)));
  }

  {
    std::lock_guard<RWMutex> lock(mutex_);
    state_ = MANAGER_RUNNING;
  }

  if (background_task_) {
    RETURN_NOT_OK(background_task_->Init());
  }

  tablets_cleaner_ = std::make_unique<rpc::Poller>(
      LogPrefix(), std::bind(&TSTabletManager::CleanupSplitTablets, this));

  return Status::OK();
}

void TSTabletManager::CleanupCheckpoints() {
  for (const auto& data_root : fs_manager_->GetDataRootDirs()) {
    auto tables_dir = JoinPathSegments(data_root, FsManager::kRocksDBDirName);
    auto tables = fs_manager_->env()->GetChildren(tables_dir, ExcludeDots::kTrue);
    if (!tables.ok()) {
      LOG_WITH_PREFIX(WARNING)
          << "Failed to get tables in " << tables_dir << ": " << tables.status();
      continue;
    }
    for (const auto& table : *tables) {
      auto table_dir = JoinPathSegments(tables_dir, table);
      auto tablets = fs_manager_->env()->GetChildren(table_dir, ExcludeDots::kTrue);
      if (!tablets.ok()) {
        LOG_WITH_PREFIX(WARNING)
            << "Failed to get tablets in " << table_dir << ": " << tables.status();
        continue;
      }
      for (const auto& tablet : *tablets) {
        auto checkpoints_dir = JoinPathSegments(
            table_dir, tablet, RemoteBootstrapSession::kCheckpointsDir);
        if (fs_manager_->env()->FileExists(checkpoints_dir)) {
          LOG_WITH_PREFIX(INFO) << "Cleaning up checkpoints dir: " << yb::ToString(checkpoints_dir);
          auto status = fs_manager_->env()->DeleteRecursively(checkpoints_dir);
          WARN_NOT_OK(status, Format("Cleanup of checkpoints dir $0 failed", checkpoints_dir));
        }
      }
    }
  }
}

Status TSTabletManager::Start() {
  async_client_init_->Start();
  if (FLAGS_cleanup_split_tablets_interval_sec > 0) {
    tablets_cleaner_->Start(
        &server_->messenger()->scheduler(), FLAGS_cleanup_split_tablets_interval_sec * 1s);
    LOG(INFO) << "Split tablets cleanup monitor started...";
  } else {
    LOG(INFO)
        << "Split tablets cleanup is disabled by cleanup_split_tablets_interval_sec flag set to 0";
  }

  return Status::OK();
}

void TSTabletManager::CleanupSplitTablets() {
  VLOG_WITH_PREFIX_AND_FUNC(3) << "looking for tablets to cleanup...";
  auto tablet_peers = GetTabletPeers();
  for (const auto& tablet_peer : tablet_peers) {
    if (tablet_peer->CanBeDeleted()) {
      const auto& tablet_id = tablet_peer->tablet_id();
      if (PREDICT_FALSE(FLAGS_TEST_skip_deleting_split_tablets)) {
        LOG_WITH_PREFIX(INFO) << Format("Skipped triggering delete of tablet $0", tablet_id);
      } else {
        LOG_WITH_PREFIX(INFO) << Format("Triggering delete of tablet $0", tablet_id);
        client().DeleteTablet(tablet_peer->tablet_id(), [tablet_id] (const Status& status) {
          LOG(INFO) << Format("Tablet $0 deletion result: $1", tablet_id, status);
        });
      }
    }
  }
}

Status TSTabletManager::WaitForAllBootstrapsToFinish() {
  CHECK_EQ(state(), MANAGER_RUNNING);

  open_tablet_pool_->Wait();

  Status s = Status::OK();

  SharedLock<RWMutex> shared_lock(mutex_);
  for (const TabletMap::value_type& entry : tablet_map_) {
    if (entry.second->state() == tablet::FAILED) {
      if (s.ok()) {
        s = entry.second->error();
      }
    }
  }

  return s;
}

Result<scoped_refptr<TransitionInProgressDeleter>>
TSTabletManager::StartTabletStateTransitionForCreation(const TabletId& tablet_id) {
  scoped_refptr<TransitionInProgressDeleter> deleter;
  SharedLock<RWMutex> lock(mutex_);
  TRACE("Acquired tablet manager lock");

  // Sanity check that the tablet isn't already registered.
  TabletPeerPtr junk;
  if (LookupTabletUnlocked(tablet_id, &junk)) {
    return STATUS(AlreadyPresent, "Tablet already registered", tablet_id);
  }

  RETURN_NOT_OK(StartTabletStateTransition(tablet_id, "creating tablet", &deleter));

  return deleter;
}

Status TSTabletManager::CreateNewTablet(
    const string& table_id,
    const string& tablet_id,
    const Partition& partition,
    const string& namespace_name,
    const string& table_name,
    TableType table_type,
    const Schema& schema,
    const PartitionSchema& partition_schema,
    const boost::optional<IndexInfo>& index_info,
    RaftConfigPB config,
    TabletPeerPtr* tablet_peer,
    const bool colocated) {
  if (state() != MANAGER_RUNNING) {
    return STATUS_FORMAT(IllegalState, "Manager is not running: $0", state());
  }
  CHECK(IsRaftConfigMember(server_->instance_pb().permanent_uuid(), config));

  for (int i = 0; i < config.peers_size(); ++i) {
    const auto& config_peer = config.peers(i);
    CHECK(config_peer.has_member_type());
  }

  // Set the initial opid_index for a RaftConfigPB to -1.
  config.set_opid_index(consensus::kInvalidOpIdIndex);

  scoped_refptr<TransitionInProgressDeleter> deleter =
      VERIFY_RESULT(StartTabletStateTransitionForCreation(tablet_id));

  // Create the metadata.
  TRACE("Creating new metadata...");
  RaftGroupMetadataPtr meta;
  string data_root_dir;
  string wal_root_dir;
  GetAndRegisterDataAndWalDir(fs_manager_, table_id, tablet_id, &data_root_dir, &wal_root_dir);
  Status create_status = RaftGroupMetadata::CreateNew(fs_manager_,
                                                   table_id,
                                                   tablet_id,
                                                   namespace_name,
                                                   table_name,
                                                   table_type,
                                                   schema,
                                                   IndexMap(),
                                                   partition_schema,
                                                   partition,
                                                   index_info,
                                                   0 /* schema_version */,
                                                   TABLET_DATA_READY,
                                                   &meta,
                                                   data_root_dir,
                                                   wal_root_dir,
                                                   colocated);
  if (!create_status.ok()) {
    UnregisterDataWalDir(table_id, tablet_id, data_root_dir, wal_root_dir);
  }
  RETURN_NOT_OK_PREPEND(create_status, "Couldn't create tablet metadata")
  LOG(INFO) << TabletLogPrefix(tablet_id)
            << "Created tablet metadata for table: " << table_id;

  // We must persist the consensus metadata to disk before starting a new
  // tablet's TabletPeer and Consensus implementation.
  std::unique_ptr<ConsensusMetadata> cmeta;
  RETURN_NOT_OK_PREPEND(ConsensusMetadata::Create(fs_manager_, tablet_id, fs_manager_->uuid(),
                                                  config, consensus::kMinimumTerm, &cmeta),
                        "Unable to create new ConsensusMeta for tablet " + tablet_id);
  TabletPeerPtr new_peer = VERIFY_RESULT(CreateAndRegisterTabletPeer(meta, NEW_PEER));

  // We can run this synchronously since there is nothing to bootstrap.
  RETURN_NOT_OK(
      open_tablet_pool_->SubmitFunc(std::bind(&TSTabletManager::OpenTablet, this, meta, deleter)));

  if (tablet_peer) {
    *tablet_peer = new_peer;
  }
  return Status::OK();
}

struct TabletCreationMetaData {
  TabletId tablet_id;
  scoped_refptr<TransitionInProgressDeleter> transition_deleter;
  Partition partition;
  docdb::KeyBounds key_bounds;
  RaftGroupMetadataPtr raft_group_metadata;
};

namespace {

// Creates SplitTabletsCreationMetaData for two new tablets for `tablet` splitting based on request.
SplitTabletsCreationMetaData PrepareTabletCreationMetaDataForSplit(
    const SplitTabletRequestPB& request, const tablet::Tablet& tablet) {
  SplitTabletsCreationMetaData metas;

  const auto& split_partition_key = request.split_partition_key();
  const auto& split_encoded_key = request.split_encoded_key();

  std::shared_ptr<Partition> source_partition = tablet.metadata()->partition();
  const auto source_key_bounds = *tablet.doc_db().key_bounds;

  {
    TabletCreationMetaData meta;
    meta.tablet_id = request.new_tablet1_id();
    meta.partition = *source_partition;
    meta.key_bounds = source_key_bounds;
    meta.partition.set_partition_key_end(split_partition_key);
    meta.key_bounds.upper.Reset(split_encoded_key);
    metas.push_back(meta);
  }

  {
    TabletCreationMetaData meta;
    meta.tablet_id = request.new_tablet2_id();
    meta.partition = *source_partition;
    meta.key_bounds = source_key_bounds;
    meta.partition.set_partition_key_start(split_partition_key);
    meta.key_bounds.lower.Reset(split_encoded_key);
    metas.push_back(meta);
  }

  return metas;
}

}  // namespace

Status TSTabletManager::StartSubtabletsSplit(
    const RaftGroupMetadata& source_tablet_meta, SplitTabletsCreationMetaData* tcmetas) {
  auto* const env = fs_manager_->env();

  auto iter = tcmetas->begin();
  while (iter != tcmetas->end()) {
    const auto& subtablet_id = iter->tablet_id;

    auto transition_deleter_result = StartTabletStateTransitionForCreation(subtablet_id);
    if (transition_deleter_result.ok()) {
      iter->transition_deleter = *transition_deleter_result;
    } else if (transition_deleter_result.status().IsAlreadyPresent()) {
      // State transition for sub tablet with subtablet_id could be already registered because its
      // remote bootstrap (from already split parent tablet leader) is in progress.
      iter = tcmetas->erase(iter);
      continue;
    } else {
      return transition_deleter_result.status();
    }

    // Try to load metadata from previous not completed split.
    if (RaftGroupMetadata::Load(fs_manager_, subtablet_id, &iter->raft_group_metadata).ok() &&
        CanServeTabletData(iter->raft_group_metadata->tablet_data_state())) {
      // Sub tablet has been already created and ready during previous split attempt at this node or
      // as a result of remote bootstrap from another node, no need to re-create.
      iter = tcmetas->erase(iter);
      continue;
    }

    // Delete on-disk data for new tablet IDs in case it is present as a leftover from previously
    // failed tablet split attempt.
    // TODO(tsplit): add test for that.
    const auto data_dir = source_tablet_meta.GetSubRaftGroupDataDir(subtablet_id);
    if (env->FileExists(data_dir)) {
      RETURN_NOT_OK_PREPEND(
          env->DeleteRecursively(data_dir),
          Format("Unable to recursively delete data dir for tablet $0", subtablet_id));
    }
    RETURN_NOT_OK(Log::DeleteOnDiskData(
        env, subtablet_id, source_tablet_meta.GetSubRaftGroupWalDir(subtablet_id),
        fs_manager_->uuid()));
    RETURN_NOT_OK(ConsensusMetadata::DeleteOnDiskData(fs_manager_, subtablet_id));

    ++iter;
  }
  return Status::OK();
}

void TSTabletManager::CreatePeerAndOpenTablet(
    const tablet::RaftGroupMetadataPtr& meta,
    const scoped_refptr<TransitionInProgressDeleter>& deleter) {
  Status s = ResultToStatus(CreateAndRegisterTabletPeer(meta, NEW_PEER));
  if (!s.ok()) {
    LOG(DFATAL) << "Failed to create and register tablet peer: " << s;
    return;
  }
  s = open_tablet_pool_->SubmitFunc(std::bind(&TSTabletManager::OpenTablet, this, meta, deleter));
  if (!s.ok()) {
    LOG(DFATAL) << Format("Failed to schedule opening tablet $0: $1", meta->table_id(), s);
    return;
  }
}

Status TSTabletManager::ApplyTabletSplit(tablet::SplitOperationState* op_state) {
  if (state() != MANAGER_RUNNING) {
    return STATUS_FORMAT(IllegalState, "Manager is not running: $0", state());
  }

  auto* tablet = CHECK_NOTNULL(op_state->tablet());
  const auto tablet_id = tablet->tablet_id();
  const auto* request = op_state->request();
  SCHECK_EQ(
      request->tablet_id(), tablet_id, IllegalState,
      Format(
          "Unexpected SPLIT_OP $0 designated for tablet $1 to be applied to tablet $2",
          op_state->op_id(), request->tablet_id(), tablet_id));
  SCHECK(
      tablet_id != request->new_tablet1_id() && tablet_id != request->new_tablet2_id(),
      IllegalState,
      Format(
          "One of SPLIT_OP $0 destination tablet IDs ($1, $2) is the same as source tablet ID $3",
          op_state->op_id(), request->new_tablet1_id(), request->new_tablet2_id(), tablet_id));

  LOG_WITH_PREFIX(INFO) << "Tablet " << tablet_id << " split operation apply started";

  auto tablet_peer = VERIFY_RESULT(LookupTablet(tablet_id));
  RETURN_NOT_OK(tablet_peer->raft_consensus()->FlushLogIndex());

  auto& meta = *CHECK_NOTNULL(tablet->metadata());

  // TODO(tsplit): We can later implement better per-disk distribution during compaction of split
  // tablets.
  const auto table_id = meta.table_id();
  const auto data_root_dir =
      VERIFY_RESULT(GetAssignedRootDirForTablet(TabletDirType::kData, table_id, tablet_id));
  const auto wal_root_dir =
      VERIFY_RESULT(GetAssignedRootDirForTablet(TabletDirType::kWal, table_id, tablet_id));

  if (FLAGS_TEST_apply_tablet_split_inject_delay_ms > 0) {
    LOG(INFO) << "TEST: ApplyTabletSplit: injecting delay of "
              << FLAGS_TEST_apply_tablet_split_inject_delay_ms << " ms for " << AsString(*op_state);
    std::this_thread::sleep_for(FLAGS_TEST_apply_tablet_split_inject_delay_ms * 1ms);
    LOG(INFO) << "TEST: ApplyTabletSplit: delay finished";
  }

  auto tcmetas = PrepareTabletCreationMetaDataForSplit(*request, *tablet);

  RETURN_NOT_OK(StartSubtabletsSplit(meta, &tcmetas));

  for (const auto& tcmeta : tcmetas) {
    RegisterDataAndWalDir(fs_manager_, table_id, tcmeta.tablet_id, data_root_dir, wal_root_dir);
  }

  bool successfully_completed = false;
  auto se = ScopeExit([&] {
    if (!successfully_completed) {
      for (const auto& tcmeta : tcmetas) {
        UnregisterDataWalDir(table_id, tcmeta.tablet_id, data_root_dir, wal_root_dir);
      }
    }
  });

  std::unique_ptr<ConsensusMetadata> cmeta;
  RETURN_NOT_OK(ConsensusMetadata::Load(fs_manager_, tablet_id, fs_manager_->uuid(), &cmeta));

  for (auto& tcmeta : tcmetas) {
    const auto& new_tablet_id = tcmeta.tablet_id;

    // Copy raft group metadata.
    tcmeta.raft_group_metadata = VERIFY_RESULT(tablet->CreateSubtablet(
        new_tablet_id, tcmeta.partition, tcmeta.key_bounds, yb::OpId::FromPB(op_state->op_id()),
        op_state->hybrid_time()));
    LOG_WITH_PREFIX(INFO) << "Created raft group metadata for table: " << table_id
                          << " tablet: " << new_tablet_id;

    // Copy consensus metadata.
    // Here we reuse the same cmeta instance for both new tablets. This is safe, because:
    // 1) Their consensus metadata only differ by tablet id.
    // 2) Flush() will save it into a new path corresponding to tablet id we set before flushing.
    cmeta->set_tablet_id(new_tablet_id);
    RETURN_NOT_OK(cmeta->Flush());

    const auto& dest_wal_dir = tcmeta.raft_group_metadata->wal_dir();
    RETURN_NOT_OK(tablet_peer->raft_consensus()->CopyLogTo(dest_wal_dir));

    tcmeta.raft_group_metadata->set_tablet_data_state(TABLET_DATA_READY);
    RETURN_NOT_OK(tcmeta.raft_group_metadata->Flush());
  }

  meta.set_tablet_data_state(tablet::TABLET_DATA_SPLIT_COMPLETED);
  RETURN_NOT_OK(meta.Flush());

  for (auto& tcmeta : tcmetas) {
    // Call CreatePeerAndOpenTablet asynchronously to avoid write-locking TSTabletManager::mutex_
    // here since apply of SPLIT_OP is done under ReplicaState lock and this could lead to deadlock
    // in case of reverse lock order in some other thread.
    // See https://github.com/yugabyte/yugabyte-db/issues/4312 for more details.
    RETURN_NOT_OK(apply_pool_->SubmitFunc(std::bind(
        &TSTabletManager::CreatePeerAndOpenTablet, this, tcmeta.raft_group_metadata,
        tcmeta.transition_deleter)));
  }

  successfully_completed = true;
  LOG_WITH_PREFIX(INFO) << "Tablet " << tablet_id << " split operation has been applied";
  return Status::OK();
}

string LogPrefix(const string& tablet_id, const string& uuid) {
  return "T " + tablet_id + " P " + uuid + ": ";
}

Status CheckLeaderTermNotLower(
    const string& tablet_id,
    const string& uuid,
    int64_t leader_term,
    int64_t last_logged_term) {
  if (PREDICT_FALSE(leader_term < last_logged_term)) {
    Status s = STATUS(InvalidArgument,
        Substitute("Leader has replica of tablet $0 with term $1 lower than last "
                   "logged term $2 on local replica. Rejecting remote bootstrap request",
                   tablet_id, leader_term, last_logged_term));
    LOG(WARNING) << LogPrefix(tablet_id, uuid) << "Remote bootstrap: " << s;
    return s;
  }
  return Status::OK();
}

Status HandleReplacingStaleTablet(
    RaftGroupMetadataPtr meta,
    TabletPeerPtr old_tablet_peer,
    const string& tablet_id,
    const string& uuid,
    const int64_t& leader_term) {
  TabletDataState data_state = meta->tablet_data_state();
  switch (data_state) {
    case TABLET_DATA_COPYING: {
      // This should not be possible due to the transition_in_progress_ "lock".
      LOG(FATAL) << LogPrefix(tablet_id, uuid) << " Remote bootstrap: "
                 << "Found tablet in TABLET_DATA_COPYING state during StartRemoteBootstrap()";
    }
    case TABLET_DATA_TOMBSTONED: {
      RETURN_NOT_OK(old_tablet_peer->CheckShutdownOrNotStarted());
      int64_t last_logged_term = meta->tombstone_last_logged_opid().term;
      RETURN_NOT_OK(CheckLeaderTermNotLower(tablet_id,
                                            uuid,
                                            leader_term,
                                            last_logged_term));
      break;
    }
    case TABLET_DATA_SPLIT_COMPLETED:
    case TABLET_DATA_READY: {
      if (tablet_id == master::kSysCatalogTabletId) {
        LOG(FATAL) << LogPrefix(tablet_id, uuid) << " Remote bootstrap: "
                   << "Found tablet in " << TabletDataState_Name(data_state)
                   << " state during StartRemoteBootstrap()";
      }
      // There's a valid race here that can lead us to come here:
      // 1. Leader sends a second remote bootstrap request as a result of receiving a
      // TABLET_NOT_FOUND from this tserver while it was in the middle of a remote bootstrap.
      // 2. The remote bootstrap request arrives after the first one is finished, and it is able to
      // grab the mutex.
      // 3. This tserver finds that it already has the metadata for the tablet, and determines that
      // it needs to replace the tablet setting replacing_tablet to true.
      // In this case, the master can simply ignore this error.
      return STATUS_FORMAT(
          IllegalState, "Tablet $0 in $1 state", tablet_id, TabletDataState_Name(data_state));
    }
    default: {
      return STATUS(IllegalState,
          Substitute("Found tablet $0 in unexpected state $1 for remote bootstrap.",
                     tablet_id, TabletDataState_Name(data_state)));
    }
  }

  return Status::OK();
}

Status TSTabletManager::StartRemoteBootstrap(const StartRemoteBootstrapRequestPB& req) {
  // To prevent racing against Shutdown, we increment this as soon as we start. This should be done
  // before checking for ClosingUnlocked, as on shutdown, we proceed in reverse:
  // - first mark as closing
  // - then wait for num_tablets_being_remote_bootstrapped_ == 0
  ++num_tablets_being_remote_bootstrapped_;
  auto decrement_num_rbs_se = ScopeExit([this](){
    --num_tablets_being_remote_bootstrapped_;
  });

  LongOperationTracker tracker("StartRemoteBootstrap", 5s);

  const string& tablet_id = req.tablet_id();
  const string& bootstrap_peer_uuid = req.bootstrap_peer_uuid();
  HostPort bootstrap_peer_addr = HostPortFromPB(DesiredHostPort(
      req.source_broadcast_addr(), req.source_private_addr(), req.source_cloud_info(),
      server_->MakeCloudInfoPB()));
  int64_t leader_term = req.caller_term();

  const string kLogPrefix = TabletLogPrefix(tablet_id);

  TabletPeerPtr old_tablet_peer;
  RaftGroupMetadataPtr meta;
  bool replacing_tablet = false;
  scoped_refptr<TransitionInProgressDeleter> deleter;
  {
    std::lock_guard<RWMutex> lock(mutex_);
    if (ClosingUnlocked()) {
      auto result = STATUS_FORMAT(
          IllegalState, "StartRemoteBootstrap in wrong state: $0",
          TSTabletManagerStatePB_Name(state_));
      LOG(WARNING) << kLogPrefix << result;
      return result;
    }

    if (LookupTabletUnlocked(tablet_id, &old_tablet_peer)) {
      meta = old_tablet_peer->tablet_metadata();
      replacing_tablet = true;
    }
    RETURN_NOT_OK(StartTabletStateTransition(
        tablet_id, Substitute("remote bootstrapping tablet from peer $0", bootstrap_peer_uuid),
        &deleter));
  }

  if (replacing_tablet) {
    // Make sure the existing tablet peer is shut down and tombstoned.
    RETURN_NOT_OK(HandleReplacingStaleTablet(meta,
                                             old_tablet_peer,
                                             tablet_id,
                                             fs_manager_->uuid(),
                                             leader_term));
  }

  string init_msg = kLogPrefix + Substitute("Initiating remote bootstrap from Peer $0 ($1)",
                                            bootstrap_peer_uuid, bootstrap_peer_addr.ToString());
  LOG(INFO) << init_msg;
  TRACE(init_msg);

  auto rb_client = std::make_unique<RemoteBootstrapClient>(tablet_id, fs_manager_);

  // Download and persist the remote superblock in TABLET_DATA_COPYING state.
  if (replacing_tablet) {
    RETURN_NOT_OK(rb_client->SetTabletToReplace(meta, leader_term));
  }
  RETURN_NOT_OK(rb_client->Start(bootstrap_peer_uuid,
                                 &server_->proxy_cache(),
                                 bootstrap_peer_addr,
                                 &meta,
                                 this));

  // From this point onward, the superblock is persisted in TABLET_DATA_COPYING
  // state, and we need to tombstone the tablet if additional steps prior to
  // getting to a TABLET_DATA_READY state fail.

  // Registering a non-initialized TabletPeer offers visibility through the Web UI.
  RegisterTabletPeerMode mode = replacing_tablet ? REPLACEMENT_PEER : NEW_PEER;
  TabletPeerPtr tablet_peer = VERIFY_RESULT(CreateAndRegisterTabletPeer(meta, mode));
  MarkTabletBeingRemoteBootstrapped(tablet_peer->tablet_id(),
      tablet_peer->tablet_metadata()->table_id());

  // TODO: If we ever make this method asynchronous, we need to move this code somewhere else.
  auto se = ScopeExit([this, tablet_peer] {
    UnmarkTabletBeingRemoteBootstrapped(tablet_peer->tablet_id(),
        tablet_peer->tablet_metadata()->table_id());
  });

  // Download all of the remote files.
  TOMBSTONE_NOT_OK(rb_client->FetchAll(tablet_peer->status_listener()),
                   meta,
                   fs_manager_->uuid(),
                   "Remote bootstrap: Unable to fetch data from remote peer " +
                       bootstrap_peer_uuid + " (" + bootstrap_peer_addr.ToString() + ")",
                   this);

  MAYBE_FAULT(FLAGS_TEST_fault_crash_after_rb_files_fetched);

  // Write out the last files to make the new replica visible and update the
  // TabletDataState in the superblock to TABLET_DATA_READY.
  // Finish() will call EndRemoteSession() and wait for the leader to successfully submit a
  // ChangeConfig request (to change this server's role from PRE_VOTER or PRE_OBSERVER to VOTER or
  // OBSERVER respectively). If the RPC times out, we will ignore the error (since the leader could
  // have successfully submitted the ChangeConfig request and failed to respond in time)
  // and check the committed config until we find that this server's role has changed, or until we
  // time out which will cause us to tombstone the tablet.
  TOMBSTONE_NOT_OK(rb_client->Finish(),
                   meta,
                   fs_manager_->uuid(),
                   "Remote bootstrap: Failed calling Finish()",
                   this);

  LOG(INFO) << kLogPrefix << "Remote bootstrap: Opening tablet";

  // TODO(hector):  ENG-3173: We need to simulate a failure in OpenTablet during remote bootstrap
  // and verify that this tablet server gets remote bootstrapped again by the leader. We also need
  // to check what happens when this server receives raft consensus requests since at this point,
  // this tablet server could be a voter (if the ChangeRole request in Finish succeeded and its
  // initial role was PRE_VOTER).
  OpenTablet(meta, nullptr);
  // If OpenTablet fails, tablet_peer->error() will be set.
  RETURN_NOT_OK(ShutdownAndTombstoneTabletPeerNotOk(
      tablet_peer->error(), tablet_peer, meta, fs_manager_->uuid(),
      "Remote bootstrap: OpenTablet() failed", this));

  auto status = rb_client->VerifyChangeRoleSucceeded(tablet_peer->shared_consensus());
  if (!status.ok()) {
    // If for some reason this tserver wasn't promoted (e.g. from PRE-VOTER to VOTER), the leader
    // will find out and do the CHANGE_CONFIG.
    LOG(WARNING) << kLogPrefix << "Remote bootstrap finished. "
                               << "Failure calling VerifyChangeRoleSucceeded: "
                               << status.ToString();
  } else {
    LOG(INFO) << kLogPrefix << "Remote bootstrap for tablet ended successfully";
  }

  WARN_NOT_OK(rb_client->Remove(), "Remove remote bootstrap sessions failed");

  return Status::OK();
}

// Create and register a new TabletPeer, given tablet metadata.
Result<TabletPeerPtr> TSTabletManager::CreateAndRegisterTabletPeer(
    const RaftGroupMetadataPtr& meta, RegisterTabletPeerMode mode) {
  TabletPeerPtr tablet_peer(new tablet::TabletPeer(
      meta,
      local_peer_pb_,
      scoped_refptr<server::Clock>(server_->clock()),
      fs_manager_->uuid(),
      Bind(&TSTabletManager::ApplyChange, Unretained(this), meta->raft_group_id()),
      metric_registry_,
      this,
      async_client_init_->get_client_future()));
  RETURN_NOT_OK(RegisterTablet(meta->raft_group_id(), tablet_peer, mode));
  return tablet_peer;
}

Status TSTabletManager::DeleteTablet(
    const string& tablet_id,
    TabletDataState delete_type,
    const boost::optional<int64_t>& cas_config_opid_index_less_or_equal,
    boost::optional<TabletServerErrorPB::Code>* error_code) {

  if (delete_type != TABLET_DATA_DELETED && delete_type != TABLET_DATA_TOMBSTONED) {
    return STATUS(InvalidArgument, "DeleteTablet() requires an argument that is one of "
                                   "TABLET_DATA_DELETED or TABLET_DATA_TOMBSTONED",
                                   Substitute("Given: $0 ($1)",
                                              TabletDataState_Name(delete_type), delete_type));
  }

  TRACE("Deleting tablet $0", tablet_id);

  TabletPeerPtr tablet_peer;
  scoped_refptr<TransitionInProgressDeleter> deleter;
  {
    // Acquire the lock in exclusive mode as we'll add a entry to the
    // transition_in_progress_ map.
    std::lock_guard<RWMutex> lock(mutex_);
    TRACE("Acquired tablet manager lock");
    RETURN_NOT_OK(CheckRunningUnlocked(error_code));

    if (!LookupTabletUnlocked(tablet_id, &tablet_peer)) {
      *error_code = TabletServerErrorPB::TABLET_NOT_FOUND;
      return STATUS(NotFound, "Tablet not found", tablet_id);
    }
    // Sanity check that the tablet's deletion isn't already in progress
    Status s = StartTabletStateTransition(tablet_id, "deleting tablet", &deleter);
    if (PREDICT_FALSE(!s.ok())) {
      *error_code = TabletServerErrorPB::TABLET_NOT_RUNNING;
      return s;
    }
  }

  // If the tablet is already deleted, the CAS check isn't possible because
  // consensus and therefore the log is not available.
  TabletDataState data_state = tablet_peer->tablet_metadata()->tablet_data_state();
  bool tablet_deleted = (data_state == TABLET_DATA_DELETED || data_state == TABLET_DATA_TOMBSTONED);

  // If a tablet peer is in the FAILED state, then we need to be able to tombstone or delete this
  // tablet. If the tablet is tombstoned, then this TS can be remote bootstrapped with the same
  // tablet.
  bool tablet_failed = tablet_peer->state() == RaftGroupStatePB::FAILED;

  // They specified an "atomic" delete. Check the committed config's opid_index.
  // TODO: There's actually a race here between the check and shutdown, but
  // it's tricky to fix. We could try checking again after the shutdown and
  // restarting the tablet if the local replica committed a higher config
  // change op during that time, or potentially something else more invasive.
  if (cas_config_opid_index_less_or_equal && !tablet_deleted && !tablet_failed) {
    shared_ptr<consensus::Consensus> consensus = tablet_peer->shared_consensus();
    if (!consensus) {
      *error_code = TabletServerErrorPB::TABLET_NOT_RUNNING;
      return STATUS(IllegalState, "Consensus not available. Tablet shutting down");
    }
    RaftConfigPB committed_config = consensus->CommittedConfig();
    if (committed_config.opid_index() > *cas_config_opid_index_less_or_equal) {
      *error_code = TabletServerErrorPB::CAS_FAILED;
      return STATUS(IllegalState, Substitute("Request specified cas_config_opid_index_less_or_equal"
                                             " of $0 but the committed config has opid_index of $1",
                                             *cas_config_opid_index_less_or_equal,
                                             committed_config.opid_index()));
    }
  }

  RaftGroupMetadataPtr meta = tablet_peer->tablet_metadata();
  // No matter if the tablet was deleted (drop table), or tombstoned (potentially moved to a
  // different TS), we do not need to flush rocksdb anymore, as this data is irrelevant.
  //
  // Note: This might change for PITR.
  bool delete_data = delete_type == TABLET_DATA_DELETED || delete_type == TABLET_DATA_TOMBSTONED;
  tablet_peer->Shutdown(tablet::IsDropTable(delete_data));

  yb::OpId last_logged_opid = tablet_peer->GetLatestLogEntryOpId();

  Status s = DeleteTabletData(meta,
                              delete_type,
                              fs_manager_->uuid(),
                              last_logged_opid,
                              this);
  if (PREDICT_FALSE(!s.ok())) {
    s = s.CloneAndPrepend(Substitute("Unable to delete on-disk data from tablet $0",
                                     tablet_id));
    LOG(WARNING) << s.ToString();
    tablet_peer->SetFailed(s);
    return s;
  }

  tablet_peer->status_listener()->StatusMessage("Deleted tablet blocks from disk");

  // We only remove DELETED tablets from the tablet map.
  if (delete_type == TABLET_DATA_DELETED) {
    std::lock_guard<RWMutex> lock(mutex_);
    RETURN_NOT_OK(CheckRunningUnlocked(error_code));
    CHECK_EQ(1, tablet_map_.erase(tablet_id)) << tablet_id;
    dirty_tablets_.erase(tablet_id);
  }

  // We unregister TOMBSTONED tablets in addition to DELETED tablets because they do not have
  // any more data on disk, so we shouldn't count these tablets when load balancing the disks.
  UnregisterDataWalDir(meta->table_id(),
                       tablet_id,
                       meta->data_root_dir(),
                       meta->wal_root_dir());

  return Status::OK();
}

Status TSTabletManager::CheckRunningUnlocked(
    boost::optional<TabletServerErrorPB::Code>* error_code) const {
  if (state_ == MANAGER_RUNNING) {
    return Status::OK();
  }
  *error_code = TabletServerErrorPB::TABLET_NOT_RUNNING;
  return STATUS(ServiceUnavailable, Substitute("Tablet Manager is not running: $0",
                                               TSTabletManagerStatePB_Name(state_)));
}

// NO_THREAD_SAFETY_ANALYSIS because this analysis does not work with unique_lock.
Status TSTabletManager::StartTabletStateTransition(
    const string& tablet_id,
    const string& reason,
    scoped_refptr<TransitionInProgressDeleter>* deleter) NO_THREAD_SAFETY_ANALYSIS {
  std::unique_lock<std::mutex> lock(transition_in_progress_mutex_);
  const auto emplace_result = transition_in_progress_.emplace(tablet_id, reason);
  if (!emplace_result.second) {
    return STATUS_FORMAT(
        AlreadyPresent, "State transition of tablet $0 already in progress: $1", tablet_id,
        *emplace_result.first);
  }
  deleter->reset(new TransitionInProgressDeleter(
      &transition_in_progress_, &transition_in_progress_mutex_, tablet_id));
  return Status::OK();
}

bool TSTabletManager::IsTabletInTransition(const TabletId& tablet_id) const {
  std::unique_lock<std::mutex> lock(transition_in_progress_mutex_);
  return ContainsKey(transition_in_progress_, tablet_id);
}

Status TSTabletManager::OpenTabletMeta(const string& tablet_id,
                                       RaftGroupMetadataPtr* metadata) {
  LOG(INFO) << "Loading metadata for tablet " << tablet_id;
  TRACE("Loading metadata...");
  RaftGroupMetadataPtr meta;
  RETURN_NOT_OK_PREPEND(RaftGroupMetadata::Load(fs_manager_, tablet_id, &meta),
                        strings::Substitute("Failed to load tablet metadata for tablet id $0",
                                            tablet_id));
  TRACE("Metadata loaded");
  metadata->swap(meta);
  return Status::OK();
}

void TSTabletManager::OpenTablet(const RaftGroupMetadataPtr& meta,
                                 const scoped_refptr<TransitionInProgressDeleter>& deleter) {
  string tablet_id = meta->raft_group_id();
  TRACE_EVENT1("tserver", "TSTabletManager::OpenTablet",
               "tablet_id", tablet_id);

  TabletPeerPtr tablet_peer;
  CHECK(LookupTablet(tablet_id, &tablet_peer))
      << "Tablet not registered prior to OpenTabletAsync call: " << tablet_id;

  tablet::TabletPtr tablet;
  scoped_refptr<Log> log;
  const string kLogPrefix = TabletLogPrefix(tablet_id);

  LOG(INFO) << kLogPrefix << "Bootstrapping tablet";
  TRACE("Bootstrapping tablet");

  consensus::ConsensusBootstrapInfo bootstrap_info;
  consensus::RetryableRequests retryable_requests(kLogPrefix);
  yb::OpId split_op_id;

  LOG_TIMING_PREFIX(INFO, kLogPrefix, "bootstrapping tablet") {
    if (CompareAndSetFlag(&FLAGS_TEST_force_single_tablet_failure,
                          true /* expected */, false /* val */)) {
      LOG(ERROR) << "Setting the state of a tablet to FAILED";
      tablet_peer->SetFailed(STATUS(InternalError, "Setting tablet to failed state for test",
                                    tablet_id));
      return;
    }

    // TODO: handle crash mid-creation of tablet? do we ever end up with a
    // partially created tablet here?
    auto s = tablet_peer->SetBootstrapping();
    if (!s.ok()) {
      LOG(ERROR) << kLogPrefix << "Tablet failed to set bootstrapping: " << s;
      tablet_peer->SetFailed(s);
      return;
    }

    tablet::TabletInitData tablet_init_data = {
      .metadata = meta,
      .client_future = async_client_init_->get_client_future(),
      .clock = scoped_refptr<server::Clock>(server_->clock()),
      .parent_mem_tracker = MemTracker::FindOrCreateTracker("Tablets", server_->mem_tracker()),
      .block_based_table_mem_tracker = block_based_table_mem_tracker_,
      .metric_registry = metric_registry_,
      .log_anchor_registry = tablet_peer->log_anchor_registry(),
      .tablet_options = tablet_options_,
      .log_prefix_suffix = " P " + tablet_peer->permanent_uuid(),
      .transaction_participant_context = tablet_peer.get(),
      .local_tablet_filter = std::bind(&TSTabletManager::PreserveLocalLeadersOnly, this, _1),
      .transaction_coordinator_context = tablet_peer.get(),
      .txns_enabled = tablet::TransactionsEnabled::kTrue,
      // We are assuming we're never dealing with the system catalog tablet in TSTabletManager.
      .is_sys_catalog = tablet::IsSysCatalogTablet::kFalse,
      .snapshot_coordinator = nullptr,
      .tablet_splitter = this,
    };
    tablet::BootstrapTabletData data = {
      .tablet_init_data = tablet_init_data,
      .listener = tablet_peer->status_listener(),
      .append_pool = append_pool(),
      .allocation_pool = allocation_pool_.get(),
      .retryable_requests = &retryable_requests,
    };
    s = BootstrapTablet(data, &tablet, &log, &bootstrap_info);
    if (!s.ok()) {
      LOG(ERROR) << kLogPrefix << "Tablet failed to bootstrap: " << s;
      tablet_peer->SetFailed(s);
      return;
    }
  }

  MonoTime start(MonoTime::Now());
  LOG_TIMING_PREFIX(INFO, kLogPrefix, "starting tablet") {
    TRACE("Initializing tablet peer");
    auto s = tablet_peer->InitTabletPeer(
        tablet,
        server_->mem_tracker(),
        server_->messenger(),
        &server_->proxy_cache(),
        log,
        tablet->GetMetricEntity(),
        raft_pool(),
        tablet_prepare_pool(),
        &retryable_requests,
        bootstrap_info.split_op_info);

    if (!s.ok()) {
      LOG(ERROR) << kLogPrefix << "Tablet failed to init: "
                 << s.ToString();
      tablet_peer->SetFailed(s);
      return;
    }

    TRACE("Starting tablet peer");
    s = tablet_peer->Start(bootstrap_info);
    if (!s.ok()) {
      LOG(ERROR) << kLogPrefix << "Tablet failed to start: "
                 << s.ToString();
      tablet_peer->SetFailed(s);
      return;
    }

    tablet_peer->RegisterMaintenanceOps(server_->maintenance_manager());
  }

  int elapsed_ms = MonoTime::Now().GetDeltaSince(start).ToMilliseconds();
  if (elapsed_ms > FLAGS_tablet_start_warn_threshold_ms) {
    LOG(WARNING) << kLogPrefix << "Tablet startup took " << elapsed_ms << "ms";
    if (Trace::CurrentTrace()) {
      LOG(WARNING) << kLogPrefix << "Trace:" << std::endl
                   << Trace::CurrentTrace()->DumpToString(true);
    }
  }

  WARN_NOT_OK(
      tablet->TriggerPostSplitCompactionIfNeeded([&]() {
        return post_split_trigger_compaction_pool_->NewToken(ThreadPool::ExecutionMode::SERIAL);
      }),
      "Failed to submit compaction for post-split tablet.");
  if (tablet->StillHasParentDataAfterSplit()) {
    std::lock_guard<RWMutex> lock(mutex_);
    tablets_being_compacted_after_split_.insert(tablet->tablet_id());
    VLOG(2) << TabletLogPrefix(tablet->tablet_id())
            << " marking as being compacted after split";
  }
}

void TSTabletManager::StartShutdown() {
  {
    std::lock_guard<RWMutex> lock(mutex_);
    switch (state_) {
      case MANAGER_QUIESCING: {
        VLOG(1) << "Tablet manager shut down already in progress..";
        return;
      }
      case MANAGER_SHUTDOWN: {
        VLOG(1) << "Tablet manager has already been shut down.";
        return;
      }
      case MANAGER_INITIALIZING:
      case MANAGER_RUNNING: {
        LOG_WITH_PREFIX(INFO) << "Shutting down tablet manager...";
        state_ = MANAGER_QUIESCING;
        break;
      }
      default: {
        LOG(FATAL) << "Invalid state: " << TSTabletManagerStatePB_Name(state_);
      }
    }
  }

  tablets_cleaner_->Shutdown();

  async_client_init_->Shutdown();

  if (background_task_) {
    background_task_->Shutdown();
  }

  // Wait for all RBS operations to finish.
  const MonoDelta kSingleWait = 10ms;
  const MonoDelta kReportInterval = 5s;
  const MonoDelta kMaxWait = 30s;
  MonoDelta waited = MonoDelta::kZero;
  MonoDelta next_report_time = kReportInterval;
  while (int remaining_rbs = num_tablets_being_remote_bootstrapped_ > 0) {
    if (waited >= next_report_time) {
      if (waited >= kMaxWait) {
        LOG_WITH_PREFIX(DFATAL)
            << "Waited for " << waited << "ms. Still had "
            << remaining_rbs << " pending remote bootstraps";
      } else {
        LOG_WITH_PREFIX(WARNING)
            << "Still waiting for " << remaining_rbs
            << " ongoing RemoteBootstraps to finish after " << waited;
      }
      next_report_time = std::min(kMaxWait, waited + kReportInterval);
    }
    SleepFor(kSingleWait);
    waited += kSingleWait;
  }

  // Shut down the bootstrap pool, so new tablets are registered after this point.
  open_tablet_pool_->Shutdown();

  // Take a snapshot of the peers list -- that way we don't have to hold
  // on to the lock while shutting them down, which might cause a lock
  // inversion. (see KUDU-308 for example).
  for (const TabletPeerPtr& peer : GetTabletPeers()) {
    if (peer->StartShutdown()) {
      shutting_down_peers_.push_back(peer);
    }
  }
}

void TSTabletManager::CompleteShutdown() {
  for (const TabletPeerPtr& peer : shutting_down_peers_) {
    peer->CompleteShutdown();
  }

  // Shut down the apply pool.
  apply_pool_->Shutdown();

  if (raft_pool_) {
    raft_pool_->Shutdown();
  }
  if (tablet_prepare_pool_) {
    tablet_prepare_pool_->Shutdown();
  }
  if (append_pool_) {
    append_pool_->Shutdown();
  }
  if (post_split_trigger_compaction_pool_) {
    post_split_trigger_compaction_pool_->Shutdown();
  }

  {
    std::lock_guard<RWMutex> l(mutex_);
    tablet_map_.clear();
    dirty_tablets_.clear();

    std::lock_guard<std::mutex> dir_assignment_lock(dir_assignment_mutex_);
    table_data_assignment_map_.clear();
    table_wal_assignment_map_.clear();

    state_ = MANAGER_SHUTDOWN;
  }
}

std::string TSTabletManager::LogPrefix() const {
  return "P " + fs_manager_->uuid() + ": ";
}

std::string TSTabletManager::TabletLogPrefix(const TabletId& tablet_id) const {
  return tserver::LogPrefix(tablet_id, fs_manager_->uuid());
}

bool TSTabletManager::ClosingUnlocked() const {
  return state_ == MANAGER_QUIESCING || state_ == MANAGER_SHUTDOWN;
}

Status TSTabletManager::RegisterTablet(const TabletId& tablet_id,
                                       const TabletPeerPtr& tablet_peer,
                                       RegisterTabletPeerMode mode) {
  std::lock_guard<RWMutex> lock(mutex_);
  if (ClosingUnlocked()) {
    auto result = STATUS_FORMAT(
        IllegalState, "Unable to register tablet peer: $0: closing", tablet_id);
    LOG(WARNING) << result;
    return result;
  }

  // If we are replacing a tablet peer, we delete the existing one first.
  if (mode == REPLACEMENT_PEER && tablet_map_.erase(tablet_id) != 1) {
    auto result = STATUS_FORMAT(
        NotFound, "Unable to remove previous tablet peer $0: not registered", tablet_id);
    LOG(WARNING) << result;
    return result;
  }
  if (!InsertIfNotPresent(&tablet_map_, tablet_id, tablet_peer)) {
    auto result = STATUS_FORMAT(
        AlreadyPresent, "Unable to register tablet peer $0: already registered", tablet_id);
    LOG(WARNING) << result;
    return result;
  }

  LOG_WITH_PREFIX(INFO) << "Registered tablet " << tablet_id;

  return Status::OK();
}

bool TSTabletManager::LookupTablet(const string& tablet_id,
                                   TabletPeerPtr* tablet_peer) const {
  SharedLock<RWMutex> shared_lock(mutex_);
  return LookupTabletUnlocked(tablet_id, tablet_peer);
}

Result<std::shared_ptr<tablet::TabletPeer>> TSTabletManager::LookupTablet(
    const TabletId& tablet_id) const {
  TabletPeerPtr tablet_peer;
  SCHECK(LookupTablet(tablet_id, &tablet_peer), NotFound, Format("Tablet $0 not found", tablet_id));
  return tablet_peer;
}

bool TSTabletManager::LookupTabletUnlocked(const string& tablet_id,
                                           TabletPeerPtr* tablet_peer) const {
  const TabletPeerPtr* found = FindOrNull(tablet_map_, tablet_id);
  if (!found) {
    return false;
  }
  *tablet_peer = *found;
  return true;
}

Status TSTabletManager::GetTabletPeer(const string& tablet_id,
                                      TabletPeerPtr* tablet_peer) const {
  if (!LookupTablet(tablet_id, tablet_peer)) {
    return STATUS(NotFound, "Tablet not found", tablet_id);
  }
  TabletDataState data_state = (*tablet_peer)->tablet_metadata()->tablet_data_state();
  if (!CanServeTabletData(data_state)) {
    return STATUS(
        IllegalState, "Tablet data state not ready: " + TabletDataState_Name(data_state),
        tablet_id);
  }
  return Status::OK();
}

const NodeInstancePB& TSTabletManager::NodeInstance() const {
  return server_->instance_pb();
}

Status TSTabletManager::GetRegistration(ServerRegistrationPB* reg) const {
  return server_->GetRegistration(reg, server::RpcOnly::kTrue);
}

void TSTabletManager::GetTabletPeers(TabletPeers* tablet_peers, TabletPtrs* tablet_ptrs) const {
  SharedLock<RWMutex> shared_lock(mutex_);
  GetTabletPeersUnlocked(tablet_peers);
  if (tablet_ptrs) {
    for (const auto& peer : *tablet_peers) {
      auto tablet_ptr = peer->shared_tablet();
      if (tablet_ptr) {
        tablet_ptrs->push_back(tablet_ptr);
      }
    }
  }
}

void TSTabletManager::GetTabletPeersUnlocked(TabletPeers* tablet_peers) const {
  AppendValuesFromMap(tablet_map_, tablet_peers);
}

void TSTabletManager::PreserveLocalLeadersOnly(std::vector<const TabletId*>* tablet_ids) const {
  SharedLock<decltype(mutex_)> shared_lock(mutex_);
  auto filter = [this](const TabletId* id) REQUIRES_SHARED(mutex_) {
    auto it = tablet_map_.find(*id);
    if (it == tablet_map_.end()) {
      return true;
    }
    auto leader_status = it->second->LeaderStatus();
    return leader_status != consensus::LeaderStatus::LEADER_AND_READY;
  };
  tablet_ids->erase(std::remove_if(tablet_ids->begin(), tablet_ids->end(), filter),
                    tablet_ids->end());
}

TSTabletManager::TabletPeers TSTabletManager::GetTabletPeers() const {
  TabletPeers peers;
  GetTabletPeers(&peers);
  return peers;
}

void TSTabletManager::ApplyChange(const string& tablet_id,
                                  shared_ptr<consensus::StateChangeContext> context) {
  WARN_NOT_OK(
      apply_pool_->SubmitFunc(
          std::bind(&TSTabletManager::MarkTabletDirty, this, tablet_id, context)),
      "Unable to run MarkDirty callback")
}

void TSTabletManager::MarkTabletDirty(const TabletId& tablet_id,
                                      std::shared_ptr<consensus::StateChangeContext> context) {
  std::lock_guard<RWMutex> lock(mutex_);
  MarkDirtyUnlocked(tablet_id, context);
}

void TSTabletManager::MarkTabletBeingRemoteBootstrapped(
    const TabletId& tablet_id, const TableId& table_id) {
  std::lock_guard<RWMutex> lock(mutex_);
  tablets_being_remote_bootstrapped_.insert(tablet_id);
  tablets_being_remote_bootstrapped_per_table_[table_id].insert(tablet_id);
  MaybeDoChecksForTests(table_id);
  LOG(INFO) << "Concurrent remote bootstrap sessions: "
            << tablets_being_remote_bootstrapped_.size()
            << "Concurrent remote bootstrap sessions for table " << table_id
            << ": " << tablets_being_remote_bootstrapped_per_table_[table_id].size();
}

void TSTabletManager::UnmarkTabletBeingRemoteBootstrapped(
    const TabletId& tablet_id, const TableId& table_id) {
  std::lock_guard<RWMutex> lock(mutex_);
  tablets_being_remote_bootstrapped_.erase(tablet_id);
  tablets_being_remote_bootstrapped_per_table_[table_id].erase(tablet_id);
}

int TSTabletManager::GetNumDirtyTabletsForTests() const {
  SharedLock<RWMutex> lock(mutex_);
  return dirty_tablets_.size();
}

Status TSTabletManager::GetNumTabletsPendingBootstrap(
    IsTabletServerReadyResponsePB* resp) const {
  if (state() != MANAGER_RUNNING) {
    resp->set_num_tablets_not_running(INT_MAX);
    resp->set_total_tablets(INT_MAX);
    return Status::OK();
  }

  SharedLock<RWMutex> shared_lock(mutex_);
  int num_pending = 0;
  int total_tablets = 0;
  for (const auto& entry : tablet_map_) {
    RaftGroupStatePB state = entry.second->state();
    TabletDataState data_state = entry.second->data_state();
    // Do not count tablets that will never get to RUNNING state.
    if (!CanServeTabletData(data_state)) {
      continue;
    }
    bool not_started_or_bootstrap = state == NOT_STARTED || state == BOOTSTRAPPING;
    if (not_started_or_bootstrap || state == RUNNING) {
      total_tablets++;
    }
    if (not_started_or_bootstrap) {
      num_pending++;
    }
  }

  LOG(INFO) << num_pending << " tablets pending bootstrap out of " << total_tablets;
  resp->set_num_tablets_not_running(num_pending);
  resp->set_total_tablets(total_tablets);

  return Status::OK();
}

int TSTabletManager::GetNumLiveTablets() const {
  int count = 0;
  SharedLock<RWMutex> lock(mutex_);
  for (const auto& entry : tablet_map_) {
    RaftGroupStatePB state = entry.second->state();
    if (state == BOOTSTRAPPING ||
        state == RUNNING) {
      count++;
    }
  }
  return count;
}

int TSTabletManager::GetLeaderCount() const {
  int count = 0;
  SharedLock<RWMutex> lock(mutex_);
  for (const auto& entry : tablet_map_) {
    consensus::LeaderStatus leader_status = entry.second->LeaderStatus(/* allow_stale =*/ true);
    if (leader_status != consensus::LeaderStatus::NOT_LEADER) {
      count++;
    }
  }
  return count;
}

void TSTabletManager::MarkDirtyUnlocked(const TabletId& tablet_id,
                                        std::shared_ptr<consensus::StateChangeContext> context) {
  TabletReportState* state = FindOrNull(dirty_tablets_, tablet_id);
  if (state != nullptr) {
    CHECK_GE(next_report_seq_, state->change_seq);
    state->change_seq = next_report_seq_;
  } else {
    TabletReportState state;
    state.change_seq = next_report_seq_;
    InsertOrDie(&dirty_tablets_, tablet_id, state);
  }
  VLOG(2) << TabletLogPrefix(tablet_id)
          << "Marking dirty. Reason: " << AsString(context)
          << ". Will report this tablet to the Master in the next heartbeat "
          << "as part of report #" << next_report_seq_;
  server_->heartbeater()->TriggerASAP();
}

void TSTabletManager::InitLocalRaftPeerPB() {
  DCHECK_EQ(state(), MANAGER_INITIALIZING);
  local_peer_pb_.set_permanent_uuid(fs_manager_->uuid());
  ServerRegistrationPB reg;
  CHECK_OK(server_->GetRegistration(&reg, server::RpcOnly::kTrue));
  TakeRegistration(&reg, &local_peer_pb_);
}

void TSTabletManager::CreateReportedTabletPB(const TabletPeerPtr& tablet_peer,
                                             ReportedTabletPB* reported_tablet) {
  reported_tablet->set_tablet_id(tablet_peer->tablet_id());
  reported_tablet->set_state(tablet_peer->state());
  reported_tablet->set_tablet_data_state(tablet_peer->tablet_metadata()->tablet_data_state());
  if (tablet_peer->state() == tablet::FAILED) {
    AppStatusPB* error_status = reported_tablet->mutable_error();
    StatusToPB(tablet_peer->error(), error_status);
  }
  reported_tablet->set_schema_version(tablet_peer->tablet_metadata()->schema_version());

  if (tablet_peer->tablet() != nullptr) {
      reported_tablet->set_processing_parent_data(
                  tablet_peer->tablet()->StillHasParentDataAfterSplit());
  }

  // We cannot get consensus state information unless the TabletPeer is running.
  shared_ptr<consensus::Consensus> consensus = tablet_peer->shared_consensus();
  if (consensus) {
    *reported_tablet->mutable_committed_consensus_state() =
        consensus->ConsensusState(consensus::CONSENSUS_CONFIG_COMMITTED);
  }
}

void TSTabletManager::GenerateTabletReport(TabletReportPB* report, bool include_bootstrap) {
  report->Clear();
  // Creating the tablet report can be slow in the case that it is in the
  // middle of flushing its consensus metadata. We don't want to hold
  // lock_ for too long, even in read mode, since it can cause other readers
  // to block if there is a waiting writer (see KUDU-2193). So, we just make
  // a local copy of the set of replicas.
  vector<std::shared_ptr<TabletPeer>> to_report;
  TabletIdSet tablet_ids;
  int32_t dirty_count, report_limit;
  {
    std::lock_guard<RWMutex> write_lock(mutex_);
    uint32_t cur_report_seq = next_report_seq_++;
    report->set_sequence_number(cur_report_seq);

    TabletIdSet::iterator i = tablets_being_compacted_after_split_.begin();
    while (i != tablets_being_compacted_after_split_.end()) {
      TabletPeerPtr* tablet_peer = FindOrNull(tablet_map_, *i);
      if (tablet_peer) {
          const auto& tablet = (*tablet_peer)->tablet();
          const std::string& tablet_id = tablet->tablet_id();
          if (!tablet->StillHasParentDataAfterSplit()) {
            i = tablets_being_compacted_after_split_.erase(i);
            VLOG(1) << "Tablet " << tablet_id << " compacted after split and marked for report";
            InsertOrUpdate(&dirty_tablets_, tablet_id, TabletReportState{cur_report_seq});
          } else {
            ++i;
          }
      } else {
          VLOG(1) << "Tablet " << *i << " should being compacted after split "
                                        "but was not found";
          i = tablets_being_compacted_after_split_.erase(i);
      }
    }

    if (include_bootstrap) {
      for (auto const& tablet_id : tablets_being_remote_bootstrapped_) {
        VLOG(1) << "Tablet " << tablet_id << " being remote bootstrapped and marked for report";
        InsertOrUpdate(&dirty_tablets_, tablet_id, TabletReportState{cur_report_seq});
      }
    }
    for (const DirtyMap::value_type& dirty_entry : dirty_tablets_) {
      const TabletId& tablet_id = dirty_entry.first;
      tablet_ids.insert(tablet_id);
    }

    for (auto const& tablet_id : tablet_ids) {
      TabletPeerPtr* tablet_peer = FindOrNull(tablet_map_, tablet_id);
      if (tablet_peer) {
        // Dirty entry, report on it.
        to_report.push_back(*tablet_peer);
      } else {
        // Tell the Master that this tablet was removed from the TServer side.
        report->add_removed_tablet_ids(tablet_id);
        // Don't count this as a 'dirty_tablet_' because the Master may not have it either.
        dirty_tablets_.erase(tablet_id);
      }
    }
    dirty_count = dirty_tablets_.size();
    report_limit = report_limit_;
  }
  for (const auto& replica : to_report) {
    CreateReportedTabletPB(replica, report->add_updated_tablets());
    // Enforce a max tablet limit on reported tablets.
    if (report->updated_tablets_size() >= report_limit) break;
  }
  report->set_remaining_tablet_count(dirty_count - report->updated_tablets_size());
}

void TSTabletManager::StartFullTabletReport(TabletReportPB* report) {
  report->Clear();
  // Creating the tablet report can be slow in the case that it is in the
  // middle of flushing its consensus metadata. We don't want to hold
  // lock_ for too long, even in read mode, since it can cause other readers
  // to block if there is a waiting writer (see KUDU-2193). So, we just make
  // a local copy of the set of replicas.
  vector<std::shared_ptr<TabletPeer>> to_report;
  int32_t dirty_count, report_limit;
  {
    std::lock_guard<RWMutex> write_lock(mutex_);
    uint32_t cur_report_seq = next_report_seq_++;
    report->set_sequence_number(cur_report_seq);
    GetTabletPeersUnlocked(&to_report);
    // Mark all tablets as dirty, to be cleaned when reading the heartbeat response.
    for (const auto& peer : to_report) {
      InsertOrUpdate(&dirty_tablets_, peer->tablet_id(), TabletReportState{cur_report_seq});
    }
    dirty_count = dirty_tablets_.size();
    report_limit = report_limit_;
  }
  for (const auto& replica : to_report) {
    CreateReportedTabletPB(replica, report->add_updated_tablets());
    // Enforce a max tablet limit on reported tablets.
    if (report->updated_tablets_size() >= report_limit) break;
  }
  report->set_remaining_tablet_count(dirty_count - report->updated_tablets_size());
}

void TSTabletManager::MarkTabletReportAcknowledged(int32_t acked_seq,
                                                   const TabletReportUpdatesPB& updates,
                                                   bool dirty_check) {
  std::lock_guard<RWMutex> l(mutex_);

  CHECK_LT(acked_seq, next_report_seq_);

  // Clear the "dirty" state for any tablets processed in this report.
  for (auto const & tablet : updates.tablets()) {
    auto it = dirty_tablets_.find(tablet.tablet_id());
    if (it != dirty_tablets_.end()) {
      const TabletReportState& state = it->second;
      if (state.change_seq <= acked_seq) {
        // This entry has not changed since this tablet report, we no longer need to track it
        // as dirty. Next modification will be re-added with a higher sequence number.
        dirty_tablets_.erase(it);
      }
    }
  }
#ifndef NDEBUG
  // Verify dirty_tablets_ always processes all tablet changes.
  if (dirty_check) {
    for (auto const & d : dirty_tablets_) {
      if (d.second.change_seq <= acked_seq) {
        LOG(DFATAL) << "Dirty Tablet should have been reported but wasn't: "
                    << d.first << "@" << d.second.change_seq << " <= " << acked_seq;
      }
    }
  }
#endif
}

Status TSTabletManager::HandleNonReadyTabletOnStartup(
    const RaftGroupMetadataPtr& meta) {
  const string& tablet_id = meta->raft_group_id();
  TabletDataState data_state = meta->tablet_data_state();
  CHECK(data_state == TABLET_DATA_DELETED ||
        data_state == TABLET_DATA_TOMBSTONED ||
        data_state == TABLET_DATA_COPYING)
      << "Unexpected TabletDataState in tablet " << tablet_id << ": "
      << TabletDataState_Name(data_state) << " (" << data_state << ")";

  if (data_state == TABLET_DATA_COPYING) {
    // We tombstone tablets that failed to remotely bootstrap.
    data_state = TABLET_DATA_TOMBSTONED;
  }

  const string kLogPrefix = TabletLogPrefix(tablet_id);

  // If the tablet is already fully tombstoned with no remaining data or WAL,
  // then no need to roll anything forward.
  bool skip_deletion = meta->IsTombstonedWithNoRocksDBData() &&
                       !Log::HasOnDiskData(meta->fs_manager(), meta->wal_dir());

  LOG_IF(WARNING, !skip_deletion)
      << kLogPrefix << "Tablet Manager startup: Rolling forward tablet deletion "
      << "of type " << TabletDataState_Name(data_state);

  if (!skip_deletion) {
    // Passing no OpId will retain the last_logged_opid that was previously in the metadata.
    RETURN_NOT_OK(DeleteTabletData(meta, data_state, fs_manager_->uuid(), yb::OpId()));
  }

  // We only delete the actual superblock of a TABLET_DATA_DELETED tablet on startup.
  // TODO: Consider doing this after a fixed delay, instead of waiting for a restart.
  // See KUDU-941.
  if (data_state == TABLET_DATA_DELETED) {
    LOG(INFO) << kLogPrefix << "Deleting tablet superblock";
    return meta->DeleteSuperBlock();
  }

  // Register TOMBSTONED tablets so that they get reported to the Master, which
  // allows us to permanently delete replica tombstones when a table gets deleted.
  if (data_state == TABLET_DATA_TOMBSTONED) {
    RETURN_NOT_OK(CreateAndRegisterTabletPeer(meta, NEW_PEER));
  }

  return Status::OK();
}

void TSTabletManager::GetAndRegisterDataAndWalDir(FsManager* fs_manager,
                                                  const string& table_id,
                                                  const string& tablet_id,
                                                  string* data_root_dir,
                                                  string* wal_root_dir) {
  // Skip sys catalog table and kudu table from modifying the map.
  if (table_id == master::kSysCatalogTableId) {
    return;
  }
  LOG(INFO) << "Get and update data/wal directory assignment map for table: " \
            << table_id << " and tablet " << tablet_id;
  std::lock_guard<std::mutex> dir_assignment_lock(dir_assignment_mutex_);
  // Initialize the map if the directory mapping does not exist.
  auto data_root_dirs = fs_manager->GetDataRootDirs();
  CHECK(!data_root_dirs.empty()) << "No data root directories found";
  auto table_data_assignment_iter = table_data_assignment_map_.find(table_id);
  if (table_data_assignment_iter == table_data_assignment_map_.end()) {
    for (string data_root_iter : data_root_dirs) {
      unordered_set<string> tablet_id_set;
      table_data_assignment_map_[table_id][data_root_iter] = tablet_id_set;
    }
  }
  // Find the data directory with the least count of tablets for this table.
  table_data_assignment_iter = table_data_assignment_map_.find(table_id);
  auto data_assignment_value_map = table_data_assignment_iter->second;
  string min_dir;
  uint64_t min_dir_count = kuint64max;
  for (auto it = data_assignment_value_map.begin(); it != data_assignment_value_map.end(); ++it) {
    if (min_dir_count > it->second.size()) {
      min_dir = it->first;
      min_dir_count = it->second.size();
    }
  }
  *data_root_dir = min_dir;
  // Increment the count for min_dir.
  auto data_assignment_value_iter = table_data_assignment_map_[table_id].find(min_dir);
  data_assignment_value_iter->second.insert(tablet_id);

  // Find the wal directory with the least count of tablets for this table.
  min_dir = "";
  min_dir_count = kuint64max;
  auto wal_root_dirs = fs_manager->GetWalRootDirs();
  CHECK(!wal_root_dirs.empty()) << "No wal root directories found";
  auto table_wal_assignment_iter = table_wal_assignment_map_.find(table_id);
  if (table_wal_assignment_iter == table_wal_assignment_map_.end()) {
    for (string wal_root_iter : wal_root_dirs) {
      unordered_set<string> tablet_id_set;
      table_wal_assignment_map_[table_id][wal_root_iter] = tablet_id_set;
    }
  }
  table_wal_assignment_iter = table_wal_assignment_map_.find(table_id);
  auto wal_assignment_value_map = table_wal_assignment_iter->second;
  for (auto it = wal_assignment_value_map.begin(); it != wal_assignment_value_map.end(); ++it) {
    if (min_dir_count > it->second.size()) {
      min_dir = it->first;
      min_dir_count = it->second.size();
    }
  }
  *wal_root_dir = min_dir;
  auto wal_assignment_value_iter = table_wal_assignment_map_[table_id].find(min_dir);
  wal_assignment_value_iter->second.insert(tablet_id);
}

void TSTabletManager::RegisterDataAndWalDir(FsManager* fs_manager,
                                            const string& table_id,
                                            const string& tablet_id,
                                            const string& data_root_dir,
                                            const string& wal_root_dir) {
  // Skip sys catalog table from modifying the map.
  if (table_id == master::kSysCatalogTableId) {
    return;
  }
  LOG(INFO) << "Update data/wal directory assignment map for table: "
            << table_id << " and tablet " << tablet_id;
  std::lock_guard<std::mutex> dir_assignment_lock(dir_assignment_mutex_);
  // Initialize the map if the directory mapping does not exist.
  auto data_root_dirs = fs_manager->GetDataRootDirs();
  CHECK(!data_root_dirs.empty()) << "No data root directories found";
  auto table_data_assignment_iter = table_data_assignment_map_.find(table_id);
  if (table_data_assignment_iter == table_data_assignment_map_.end()) {
    for (string data_root_iter : data_root_dirs) {
      unordered_set<string> tablet_id_set;
      table_data_assignment_map_[table_id][data_root_iter] = tablet_id_set;
    }
  }
  // Increment the count for data_root_dir.
  table_data_assignment_iter = table_data_assignment_map_.find(table_id);
  auto data_assignment_value_map = table_data_assignment_iter->second;
  auto data_assignment_value_iter = table_data_assignment_map_[table_id].find(data_root_dir);
  if (data_assignment_value_iter == table_data_assignment_map_[table_id].end()) {
    unordered_set<string> tablet_id_set;
    tablet_id_set.insert(tablet_id);
    table_data_assignment_map_[table_id][data_root_dir] = tablet_id_set;
  } else {
    data_assignment_value_iter->second.insert(tablet_id);
  }

  auto wal_root_dirs = fs_manager->GetWalRootDirs();
  CHECK(!wal_root_dirs.empty()) << "No wal root directories found";
  auto table_wal_assignment_iter = table_wal_assignment_map_.find(table_id);
  if (table_wal_assignment_iter == table_wal_assignment_map_.end()) {
    for (string wal_root_iter : wal_root_dirs) {
      unordered_set<string> tablet_id_set;
      table_wal_assignment_map_[table_id][wal_root_iter] = tablet_id_set;
    }
  }
  // Increment the count for wal_root_dir.
  table_wal_assignment_iter = table_wal_assignment_map_.find(table_id);
  auto wal_assignment_value_map = table_wal_assignment_iter->second;
  auto wal_assignment_value_iter = table_wal_assignment_map_[table_id].find(wal_root_dir);
  if (wal_assignment_value_iter == table_wal_assignment_map_[table_id].end()) {
    unordered_set<string> tablet_id_set;
    tablet_id_set.insert(tablet_id);
    table_wal_assignment_map_[table_id][wal_root_dir] = tablet_id_set;
  } else {
    wal_assignment_value_iter->second.insert(tablet_id);
  }
}

TSTabletManager::TableDiskAssignmentMap* TSTabletManager::GetTableDiskAssignmentMapUnlocked(
    TabletDirType dir_type) {
  switch (dir_type) {
    case TabletDirType::kData:
      return &table_data_assignment_map_;
    case TabletDirType::kWal:
      return &table_wal_assignment_map_;
  }
  FATAL_INVALID_ENUM_VALUE(TabletDirType, dir_type);
}

Result<const std::string&> TSTabletManager::GetAssignedRootDirForTablet(
    TabletDirType dir_type, const TableId& table_id, const TabletId& tablet_id) {
  std::lock_guard<std::mutex> dir_assignment_lock(dir_assignment_mutex_);

  TableDiskAssignmentMap* table_assignment_map = GetTableDiskAssignmentMapUnlocked(dir_type);
  auto tablets_by_root_dir = table_assignment_map->find(table_id);
  if (tablets_by_root_dir == table_assignment_map->end()) {
    return STATUS_FORMAT(
        IllegalState, "Table ID $0 is not in $1 table assignment map", table_id, dir_type);
  }
  for (auto& data_dir_and_tablets : tablets_by_root_dir->second) {
    if (data_dir_and_tablets.second.count(tablet_id) > 0) {
      return data_dir_and_tablets.first;
    }
  }
  return STATUS_FORMAT(
      IllegalState, "Tablet ID $0 is not found in $1 assignment map for table $2", tablet_id,
      dir_type, table_id);
}

void TSTabletManager::UnregisterDataWalDir(const string& table_id,
                                           const string& tablet_id,
                                           const string& data_root_dir,
                                           const string& wal_root_dir) {
  // Skip sys catalog table from modifying the map.
  if (table_id == master::kSysCatalogTableId) {
    return;
  }
  LOG(INFO) << "Unregister data/wal directory assignment map for table: "
            << table_id << " and tablet " << tablet_id;
  std::lock_guard<std::mutex> lock(dir_assignment_mutex_);
  auto table_data_assignment_iter = table_data_assignment_map_.find(table_id);
  if (table_data_assignment_iter == table_data_assignment_map_.end()) {
    // It is possible that we can't find an assignment for the table if the operations followed in
    // this order:
    // 1. The only tablet for a table gets tombstoned, and UnregisterDataWalDir removes it from
    //    the maps.
    // 2. TSTabletManager gets restarted (so the maps are cleared).
    // 3. During TsTabletManager initialization, the tombstoned TABLET won't get registered,
    //    so if a DeleteTablet request with type DELETED gets sent, UnregisterDataWalDir won't
    //    find the table.

    // Check that both maps should be consistent.
    DCHECK(table_wal_assignment_map_.find(table_id) == table_wal_assignment_map_.end());
  }
  if (table_data_assignment_iter != table_data_assignment_map_.end()) {
    auto data_assignment_value_iter = table_data_assignment_map_[table_id].find(data_root_dir);
    DCHECK(data_assignment_value_iter != table_data_assignment_map_[table_id].end())
      << "No data directory index found for table: " << table_id;
    if (data_assignment_value_iter != table_data_assignment_map_[table_id].end()) {
      data_assignment_value_iter->second.erase(tablet_id);
    } else {
      LOG(WARNING) << "Tablet " << tablet_id << " not in the set for data directory "
                   << data_root_dir << "for table " << table_id;
    }
  }
  auto table_wal_assignment_iter = table_wal_assignment_map_.find(table_id);
  if (table_wal_assignment_iter != table_wal_assignment_map_.end()) {
    auto wal_assignment_value_iter = table_wal_assignment_map_[table_id].find(wal_root_dir);
    DCHECK(wal_assignment_value_iter != table_wal_assignment_map_[table_id].end())
      << "No wal directory index found for table: " << table_id;
    if (wal_assignment_value_iter != table_wal_assignment_map_[table_id].end()) {
      wal_assignment_value_iter->second.erase(tablet_id);
    } else {
      LOG(WARNING) << "Tablet " << tablet_id << " not in the set for wal directory "
                   << wal_root_dir << "for table " << table_id;
    }
  }
}

client::YBClient& TSTabletManager::client() {
  return *async_client_init_->client();
}

void TSTabletManager::MaybeDoChecksForTests(const TableId& table_id) {
  // First check that the global RBS limits are respected if the flag is non-zero.
  if (PREDICT_FALSE(FLAGS_TEST_crash_if_remote_bootstrap_sessions_greater_than > 0) &&
      tablets_being_remote_bootstrapped_.size() >
      FLAGS_TEST_crash_if_remote_bootstrap_sessions_greater_than) {
    string tablets;
    // The purpose of limiting the number of remote bootstraps is to cap how much
    // network bandwidth all the RBS sessions use.
    // When we finish transferring the files, we wait until the role of the new peer
    // has been changed from PRE_VOTER to VOTER before we remove the tablet_id
    // from tablets_being_remote_bootstrapped_. Since it's possible to be here
    // because a few tablets are already open, and in the RUNNING state, but still
    // in the tablets_being_remote_bootstrapped_ list, we check the state of each
    // tablet before deciding if the load balancer has violated the concurrent RBS limit.
    int count = 0;
    for (const auto& tablet_id : tablets_being_remote_bootstrapped_) {
      TabletPeerPtr* tablet_peer = FindOrNull(tablet_map_, tablet_id);
      if (tablet_peer && (*tablet_peer)->state() == RaftGroupStatePB::RUNNING) {
        continue;
      }
      if (!tablets.empty()) {
        tablets += ", ";
      }
      tablets += tablet_id;
      count++;
    }
    if (count > FLAGS_TEST_crash_if_remote_bootstrap_sessions_greater_than) {
      LOG(FATAL) << "Exceeded the specified maximum number of concurrent remote bootstrap sessions."
                 << " Specified: " << FLAGS_TEST_crash_if_remote_bootstrap_sessions_greater_than
                 << ", number concurrent remote bootstrap sessions: "
                 << tablets_being_remote_bootstrapped_.size() << ", for tablets: " << tablets;
    }
  }

  // Check that the per-table RBS limits are respected if the flag is non-zero.
  if (PREDICT_FALSE(FLAGS_TEST_crash_if_remote_bootstrap_sessions_per_table_greater_than > 0) &&
      tablets_being_remote_bootstrapped_per_table_[table_id].size() >
          FLAGS_TEST_crash_if_remote_bootstrap_sessions_per_table_greater_than) {
    string tablets;
    int count = 0;
    for (const auto& tablet_id : tablets_being_remote_bootstrapped_per_table_[table_id]) {
      TabletPeerPtr* tablet_peer = FindOrNull(tablet_map_, tablet_id);
      if (tablet_peer && (*tablet_peer)->state() == RaftGroupStatePB::RUNNING) {
        continue;
      }
      if (!tablets.empty()) {
        tablets += ", ";
      }
      tablets += tablet_id;
      count++;
    }
    if (count > FLAGS_TEST_crash_if_remote_bootstrap_sessions_per_table_greater_than) {
      LOG(FATAL) << "Exceeded the specified maximum number of concurrent remote bootstrap "
                 << "sessions per table. Specified: "
                 << FLAGS_TEST_crash_if_remote_bootstrap_sessions_per_table_greater_than
                 << ", number of concurrent remote bootstrap sessions for table " << table_id
                 << ": " << tablets_being_remote_bootstrapped_per_table_[table_id].size()
                 << ", for tablets: " << tablets;
    }
  }
}

size_t GetLogCacheSize(TabletPeer* peer) {
  return down_cast<consensus::RaftConsensus*>(peer->consensus())->LogCacheSize();
}

void TSTabletManager::LogCacheGC(MemTracker* log_cache_mem_tracker, size_t bytes_to_evict) {
  if (!FLAGS_enable_log_cache_gc) {
    return;
  }

  if (FLAGS_log_cache_gc_evict_only_over_allocated) {
    if (!log_cache_mem_tracker->has_limit()) {
      return;
    }
    auto limit = log_cache_mem_tracker->limit();
    auto consumption = log_cache_mem_tracker->consumption();
    if (consumption <= limit) {
      return;
    }
    bytes_to_evict = std::min<size_t>(bytes_to_evict, consumption - limit);
  }

  std::vector<TabletPeerPtr> peers;
  {
    SharedLock<RWMutex> shared_lock(mutex_);
    peers.reserve(tablet_map_.size());
    for (const auto& pair : tablet_map_) {
      if (GetLogCacheSize(pair.second.get()) > 0) {
        peers.push_back(pair.second);
      }
    }
  }
  std::sort(peers.begin(), peers.end(), [](const auto& lhs, const auto& rhs) {
    // Note inverse order.
    return GetLogCacheSize(lhs.get()) > GetLogCacheSize(rhs.get());
  });

  size_t total_evicted = 0;
  for (const auto& peer : peers) {
    size_t evicted = down_cast<consensus::RaftConsensus*>(
        peer->consensus())->EvictLogCache(bytes_to_evict - total_evicted);
    total_evicted += evicted;
    if (total_evicted >= bytes_to_evict) {
      break;
    }
  }

  LOG(INFO) << "Evicted from log cache: " << HumanReadableNumBytes::ToString(total_evicted)
            << ", required: " << HumanReadableNumBytes::ToString(bytes_to_evict);
}

Status DeleteTabletData(const RaftGroupMetadataPtr& meta,
                        TabletDataState data_state,
                        const string& uuid,
                        const yb::OpId& last_logged_opid,
                        TSTabletManager* ts_manager) {
  const string& tablet_id = meta->raft_group_id();
  const string kLogPrefix = LogPrefix(tablet_id, uuid);
  LOG(INFO) << kLogPrefix << "Deleting tablet data with delete state "
            << TabletDataState_Name(data_state);
  CHECK(data_state == TABLET_DATA_DELETED ||
        data_state == TABLET_DATA_TOMBSTONED)
      << "Unexpected data_state to delete tablet " << meta->raft_group_id() << ": "
      << TabletDataState_Name(data_state) << " (" << data_state << ")";

  // Note: Passing an unset 'last_logged_opid' will retain the last_logged_opid
  // that was previously in the metadata.
  RETURN_NOT_OK(meta->DeleteTabletData(data_state, last_logged_opid));
  LOG(INFO) << kLogPrefix << "Tablet deleted. Last logged OpId: "
            << meta->tombstone_last_logged_opid();
  MAYBE_FAULT(FLAGS_TEST_fault_crash_after_blocks_deleted);

  RETURN_NOT_OK(Log::DeleteOnDiskData(
      meta->fs_manager()->env(), meta->raft_group_id(), meta->wal_dir(),
      meta->fs_manager()->uuid()));
  MAYBE_FAULT(FLAGS_TEST_fault_crash_after_wal_deleted);

  // We do not delete the superblock or the consensus metadata when tombstoning
  // a tablet.
  if (data_state == TABLET_DATA_TOMBSTONED) {
    return Status::OK();
  }

  // Only TABLET_DATA_DELETED tablets get this far.
  RETURN_NOT_OK(ConsensusMetadata::DeleteOnDiskData(meta->fs_manager(), meta->raft_group_id()));
  MAYBE_FAULT(FLAGS_TEST_fault_crash_after_cmeta_deleted);

  return Status::OK();
}

void LogAndTombstone(const RaftGroupMetadataPtr& meta,
                     const std::string& msg,
                     const std::string& uuid,
                     const Status& s,
                     TSTabletManager* ts_manager) {
  const string& tablet_id = meta->raft_group_id();
  const string kLogPrefix = LogPrefix(tablet_id, uuid);
  LOG(WARNING) << kLogPrefix << msg << ": " << s.ToString();

  // Tombstone the tablet when remote bootstrap fails.
  LOG(INFO) << kLogPrefix << "Tombstoning tablet after failed remote bootstrap";
  Status delete_status = DeleteTabletData(meta,
                                          TABLET_DATA_TOMBSTONED,
                                          uuid,
                                          yb::OpId(),
                                          ts_manager);

  if (PREDICT_FALSE(FLAGS_TEST_sleep_after_tombstoning_tablet_secs > 0)) {
    // We sleep here so that the test can verify that the state of the tablet is
    // TABLET_DATA_TOMBSTONED.
    LOG(INFO) << "Sleeping after remote bootstrap failed";
    SleepFor(MonoDelta::FromSeconds(FLAGS_TEST_sleep_after_tombstoning_tablet_secs));
  }

  if (PREDICT_FALSE(!delete_status.ok())) {
    // This failure should only either indicate a bug or an IO error.
    LOG(FATAL) << kLogPrefix << "Failed to tombstone tablet after remote bootstrap: "
               << delete_status.ToString();
  }

  // Remove the child tracker if present.
  if (ts_manager != nullptr) {
    auto tracker = MemTracker::FindTracker(
        Format("tablet-$0", meta->raft_group_id()), ts_manager->server()->mem_tracker());
    if (tracker) {
      tracker->UnregisterFromParent();
    }
  }
}

TransitionInProgressDeleter::TransitionInProgressDeleter(
    TransitionInProgressMap* map, std::mutex* mutex, const TabletId& tablet_id)
    : in_progress_(map), mutex_(mutex), tablet_id_(tablet_id) {}

TransitionInProgressDeleter::~TransitionInProgressDeleter() {
  std::string transition;
  {
    std::unique_lock<std::mutex> lock(*mutex_);
    const auto iter = in_progress_->find(tablet_id_);
    CHECK(iter != in_progress_->end());
    transition = iter->second;
    in_progress_->erase(iter);
  }
  LOG(INFO) << "Deleted transition in progress " << transition
            << " for tablet " << tablet_id_;
}

Status ShutdownAndTombstoneTabletPeerNotOk(
    const Status& status, const tablet::TabletPeerPtr& tablet_peer,
    const tablet::RaftGroupMetadataPtr& meta, const std::string& uuid, const char* msg,
    TSTabletManager* ts_tablet_manager) {
  if (status.ok()) {
    return status;
  }
  // If shutdown was initiated by someone else we should not wait for shutdown to complete.
  if (tablet_peer && tablet_peer->StartShutdown()) {
    tablet_peer->CompleteShutdown();
  }
  tserver::LogAndTombstone(meta, msg, uuid, status, ts_tablet_manager);
  return status;
}

} // namespace tserver
} // namespace yb
