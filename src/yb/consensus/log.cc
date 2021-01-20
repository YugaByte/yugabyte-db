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

#include "yb/consensus/log.h"

#include <algorithm>
#include <mutex>
#include <thread>

#include <boost/thread/shared_mutex.hpp>

#include "yb/common/wire_protocol.h"

#include "yb/consensus/consensus_util.h"
#include "yb/consensus/log_index.h"
#include "yb/consensus/log_metrics.h"
#include "yb/consensus/log_reader.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"

#include "yb/fs/fs_manager.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/walltime.h"
#include "yb/util/coding.h"
#include "yb/util/countdown_latch.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/env_util.h"
#include "yb/util/fault_injection.h"
#include "yb/util/file_util.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/opid.h"
#include "yb/util/path_util.h"
#include "yb/util/pb_util.h"
#include "yb/util/random.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"
#include "yb/util/stopwatch.h"
#include "yb/util/taskstream.h"
#include "yb/util/thread.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"
#include "yb/util/tsan_util.h"
#include "yb/util/shared_lock.h"

using namespace yb::size_literals;  // NOLINT.
using namespace std::literals;  // NOLINT.
using namespace std::placeholders;

// Log retention configuration.
// -----------------------------
DEFINE_int32(log_min_segments_to_retain, 2,
             "The minimum number of past log segments to keep at all times,"
             " regardless of what is required for durability. "
             "Must be at least 1.");
TAG_FLAG(log_min_segments_to_retain, runtime);
TAG_FLAG(log_min_segments_to_retain, advanced);

DEFINE_int32(log_min_seconds_to_retain, 900,
             "The minimum number of seconds for which to keep log segments to keep at all times, "
             "regardless of what is required for durability. Logs may be still retained for "
             "a longer amount of time if they are necessary for correct restart. This should be "
             "set long enough such that a tablet server which has temporarily failed can be "
             "restarted within the given time period. If a server is down for longer than this "
             "amount of time, it is possible that its tablets will be re-replicated on other "
             "machines.");
TAG_FLAG(log_min_seconds_to_retain, runtime);
TAG_FLAG(log_min_seconds_to_retain, advanced);

// Flags for controlling kernel watchdog limits.
DEFINE_int32(consensus_log_scoped_watch_delay_callback_threshold_ms, 1000,
             "If calling consensus log callback(s) take longer than this, the kernel watchdog "
             "will print out a stack trace.");
TAG_FLAG(consensus_log_scoped_watch_delay_callback_threshold_ms, runtime);
TAG_FLAG(consensus_log_scoped_watch_delay_callback_threshold_ms, advanced);
DEFINE_int32(consensus_log_scoped_watch_delay_append_threshold_ms, 1000,
             "If consensus log append takes longer than this, the kernel watchdog "
             "will print out a stack trace.");
TAG_FLAG(consensus_log_scoped_watch_delay_append_threshold_ms, runtime);
TAG_FLAG(consensus_log_scoped_watch_delay_append_threshold_ms, advanced);

// Fault/latency injection flags.
// -----------------------------
DEFINE_bool(log_inject_latency, false,
            "If true, injects artificial latency in log sync operations. "
            "Advanced option. Use at your own risk -- has a negative effect "
            "on performance for obvious reasons!");
DEFINE_int32(log_inject_latency_ms_mean, 100,
             "The number of milliseconds of latency to inject, on average. "
             "Only takes effect if --log_inject_latency is true");
DEFINE_int32(log_inject_latency_ms_stddev, 100,
             "The standard deviation of latency to inject in before log sync operations. "
             "Only takes effect if --log_inject_latency is true");
TAG_FLAG(log_inject_latency, unsafe);
TAG_FLAG(log_inject_latency_ms_mean, unsafe);
TAG_FLAG(log_inject_latency_ms_stddev, unsafe);

DEFINE_int32(log_inject_append_latency_ms_max, 0,
             "The maximum latency to inject before the log append operation.");

DEFINE_test_flag(bool, log_consider_all_ops_safe, false,
            "If true, we consider all operations to be safe and will not wait"
            "for the opId to apply to the local log. i.e. WaitForSafeOpIdToApply "
            "becomes a noop.");

// TaskStream flags.
// We have to make the queue length really long.
// TODO: Create new flags log_taskstream_queue_max_size and log_taskstream_queue_max_wait_ms
// and deprecate these flags.
DEFINE_int32(taskstream_queue_max_size, 100000,
             "Maximum number of operations waiting in the taskstream queue.");

DEFINE_int32(taskstream_queue_max_wait_ms, 1000,
             "Maximum time in ms to wait for items in the taskstream queue to arrive.");

DEFINE_int32(wait_for_safe_op_id_to_apply_default_timeout_ms, 15000 * yb::kTimeMultiplier,
             "Timeout used by WaitForSafeOpIdToApply when it was not specified by caller.");

// Validate that log_min_segments_to_retain >= 1
static bool ValidateLogsToRetain(const char* flagname, int value) {
  if (value >= 1) {
    return true;
  }
  LOG(ERROR) << strings::Substitute("$0 must be at least 1, value $1 is invalid",
                                    flagname, value);
  return false;
}
static bool dummy = google::RegisterFlagValidator(
    &FLAGS_log_min_segments_to_retain, &ValidateLogsToRetain);

static const char kSegmentPlaceholderFileTemplate[] = ".tmp.newsegmentXXXXXX";

namespace yb {
namespace log {

using env_util::OpenFileForRandom;
using std::shared_ptr;
using std::unique_ptr;
using strings::Substitute;

// This class represents a batch of operations to be written and synced to the log. It is opaque to
// the user and is managed by the Log class.
class LogEntryBatch {
 public:
  LogEntryBatch(LogEntryTypePB type, LogEntryBatchPB&& entry_batch_pb);
  ~LogEntryBatch();

  std::string ToString() const {
    return Format("{ type: $0 state: $1 max_op_id: $2 }", type_, state_, MaxReplicateOpId());
  }

  bool HasReplicateEntries() const {
    return type_ == LogEntryTypePB::REPLICATE && count() > 0;
  }

 private:
  friend class Log;
  friend class MultiThreadedLogTest;

  // Serializes contents of the entry to an internal buffer.
  CHECKED_STATUS Serialize();

  // Sets the callback that will be invoked after the entry is
  // appended and synced to disk
  void set_callback(const StatusCallback& cb) {
    callback_ = cb;
  }

  // Returns the callback that will be invoked after the entry is
  // appended and synced to disk.
  const StatusCallback& callback() {
    return callback_;
  }

  bool failed_to_append() const {
    return state_ == kEntryFailedToAppend;
  }

  void set_failed_to_append() {
    state_ = kEntryFailedToAppend;
  }

  // Mark the entry as reserved, but not yet ready to write to the log.
  void MarkReserved();

  // Mark the entry as ready to write to log.
  void MarkReady();

  // Returns a Slice representing the serialized contents of the entry.
  Slice data() const {
    DCHECK_EQ(state_, kEntrySerialized);
    return Slice(buffer_);
  }

  bool flush_marker() const;

  size_t count() const { return count_; }

  // Returns the total size in bytes of the object.
  size_t total_size_bytes() const {
    return total_size_bytes_;
  }

  // The highest OpId of a REPLICATE message in this batch.
  OpId MaxReplicateOpId() const {
    DCHECK_EQ(REPLICATE, type_);
    int idx = entry_batch_pb_.entry_size() - 1;
    if (idx < 0) {
      return OpId::Invalid();
    }
    DCHECK(entry_batch_pb_.entry(idx).replicate().IsInitialized());
    return OpId::FromPB(entry_batch_pb_.entry(idx).replicate().id());
  }

  void SetReplicates(const ReplicateMsgs& replicates) {
    replicates_ = replicates;
  }

  // The type of entries in this batch.
  const LogEntryTypePB type_;

  // Contents of the log entries that will be written to disk.
  LogEntryBatchPB entry_batch_pb_;

  // Total size in bytes of all entries
  uint32_t total_size_bytes_ = 0;

  // Number of entries in 'entry_batch_pb_'
  const size_t count_;

  // The vector of refcounted replicates.  This makes sure there's at least a reference to each
  // replicate message until we're finished appending.
  ReplicateMsgs replicates_;

  // Callback to be invoked upon the entries being written and synced to disk.
  StatusCallback callback_;

  // Buffer to which 'phys_entries_' are serialized by call to 'Serialize()'
  faststring buffer_;

  // Offset into the log file for this entry batch.
  int64_t offset_;

  // Segment sequence number for this entry batch.
  uint64_t active_segment_sequence_number_;

  enum LogEntryState {
    kEntryInitialized,
    kEntryReserved,
    kEntryReady,
    kEntrySerialized,
    kEntryFailedToAppend
  };
  LogEntryState state_ = kEntryInitialized;

  DISALLOW_COPY_AND_ASSIGN(LogEntryBatch);
};

// This class is responsible for managing the task that appends to the log file.
// This task runs in a common thread pool with append tasks from other tablets.
// A token is used to ensure that only one append task per tablet is executed concurrently.
class Log::Appender {
 public:
  explicit Appender(Log* log, ThreadPool* append_thread_pool);

  // Initializes the objects and starts the task.
  Status Init();

  CHECKED_STATUS Submit(LogEntryBatch* item) {
    return task_stream_->Submit(item);
  }

  CHECKED_STATUS TEST_SubmitFunc(const std::function<void()>& func) {
    return task_stream_->TEST_SubmitFunc(func);
  }

  // Waits until the last enqueued elements are processed, sets the appender_ to closing
  // state. If any entries are added to the queue during the process, invoke their callbacks'
  // 'OnFailure()' method.
  void Shutdown();

  const std::string& LogPrefix() const {
    return log_->LogPrefix();
  }

  std::string GetRunThreadStack() const {
    return task_stream_->GetRunThreadStack();
  }

  std::string ToString() const {
    return task_stream_->ToString();
  }

 private:
  // Process the given log entry batch or does a sync if a null is passed.
  void ProcessBatch(LogEntryBatch* entry_batch);
  void GroupWork();

  Log* const log_;

  // Lock to protect access to thread_ during shutdown.
  mutable std::mutex lock_;
  unique_ptr<TaskStream<LogEntryBatch>> task_stream_;

  // vector of entry batches in group, to execute callbacks after call to Sync.
  std::vector<std::unique_ptr<LogEntryBatch>> sync_batch_;

  // Time at which current group was started
  MonoTime time_started_;
};

Log::Appender::Appender(Log *log, ThreadPool* append_thread_pool)
    : log_(log),
      task_stream_(new TaskStream<LogEntryBatch>(
          std::bind(&Log::Appender::ProcessBatch, this, _1), append_thread_pool,
          FLAGS_taskstream_queue_max_size,
          MonoDelta::FromMilliseconds(FLAGS_taskstream_queue_max_wait_ms))) {
  DCHECK(dummy);
}

Status Log::Appender::Init() {
  VLOG_WITH_PREFIX(1) << "Starting log task stream";
  return Status::OK();
}

void Log::Appender::ProcessBatch(LogEntryBatch* entry_batch) {
  // A callback function to TaskStream is expected to process the accumulated batch of entries.
  if (entry_batch == nullptr) {
    // Here, we do sync and call callbacks.
    GroupWork();
    return;
  }

  if (sync_batch_.empty()) { // Start of batch.
    // Used in tests to delay writing log entries.
    auto sleep_duration = log_->sleep_duration_.load(std::memory_order_acquire);
    if (sleep_duration.count() > 0) {
      std::this_thread::sleep_for(sleep_duration);
    }
    time_started_ = MonoTime::Now();
  }
  TRACE_EVENT_FLOW_END0("log", "Batch", entry_batch);
  Status s = log_->DoAppend(entry_batch);

  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(DFATAL) << "Error appending to the log: " << s;
    entry_batch->set_failed_to_append();
    // TODO If a single operation fails to append, should we abort all subsequent operations
    // in this batch or allow them to be appended? What about operations in future batches?
    if (!entry_batch->callback().is_null()) {
      entry_batch->callback().Run(s);
    }
    return;
  }
  if (!log_->sync_disabled_) {
    bool expected = false;
    if (log_->periodic_sync_needed_.compare_exchange_strong(expected, true,
                                                            std::memory_order_acq_rel)) {
      log_->periodic_sync_earliest_unsync_entry_time_ = MonoTime::Now();
    }
    log_->periodic_sync_unsynced_bytes_ += entry_batch->total_size_bytes();
  }
  sync_batch_.emplace_back(entry_batch);
}

void Log::Appender::GroupWork() {
  if (sync_batch_.empty()) {
    Status s = log_->Sync();
    return;
  }
  if (log_->metrics_) {
    log_->metrics_->entry_batches_per_group->Increment(sync_batch_.size());
  }
  TRACE_EVENT1("log", "batch", "batch_size", sync_batch_.size());

  auto se = ScopeExit([this] {
    if (log_->metrics_) {
      MonoTime time_now = MonoTime::Now();
      log_->metrics_->group_commit_latency->Increment(
          time_now.GetDeltaSince(time_started_).ToMicroseconds());
    }
    sync_batch_.clear();
  });

  Status s = log_->Sync();
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(DFATAL) << "Error syncing log: " << s;
    for (std::unique_ptr<LogEntryBatch>& entry_batch : sync_batch_) {
      if (!entry_batch->callback().is_null()) {
        entry_batch->callback().Run(s);
      }
    }
  } else {
    TRACE_EVENT0("log", "Callbacks");
    VLOG_WITH_PREFIX(2) << "Synchronized " << sync_batch_.size() << " entry batches";
    LongOperationTracker long_operation_tracker(
        "Log callback", FLAGS_consensus_log_scoped_watch_delay_callback_threshold_ms * 1ms);
    for (std::unique_ptr<LogEntryBatch>& entry_batch : sync_batch_) {
      if (PREDICT_TRUE(!entry_batch->failed_to_append() && !entry_batch->callback().is_null())) {
        entry_batch->callback().Run(Status::OK());
      }
      // It's important to delete each batch as we see it, because deleting it may free up memory
      // from memory trackers, and the callback of a later batch may want to use that memory.
      entry_batch.reset();
    }
    sync_batch_.clear();
  }
  VLOG_WITH_PREFIX(1) << "Exiting AppendTask for tablet " << log_->tablet_id();
}

void Log::Appender::Shutdown() {
  std::lock_guard<std::mutex> lock_guard(lock_);
  if (task_stream_) {
    VLOG_WITH_PREFIX(1) << "Shutting down log task stream";
    task_stream_->Stop();
    VLOG_WITH_PREFIX(1) << "Log append task stream is shut down";
    task_stream_.reset();
  }
}

// This task is submitted to allocation_pool_ in order to asynchronously pre-allocate new log
// segments.
void Log::SegmentAllocationTask() {
  allocation_status_.Set(PreAllocateNewSegment());
}

const Status Log::kLogShutdownStatus(
    STATUS(ServiceUnavailable, "WAL is shutting down", "", Errno(ESHUTDOWN)));

Status Log::Open(const LogOptions &options,
                 const std::string& tablet_id,
                 const std::string& wal_dir,
                 const std::string& peer_uuid,
                 const Schema& schema,
                 uint32_t schema_version,
                 const scoped_refptr<MetricEntity>& metric_entity,
                 ThreadPool* append_thread_pool,
                 ThreadPool* allocation_thread_pool,
                 int64_t cdc_min_replicated_index,
                 scoped_refptr<Log>* log,
                 CreateNewSegment create_new_segment) {

  RETURN_NOT_OK_PREPEND(env_util::CreateDirIfMissing(options.env, DirName(wal_dir)),
                        Substitute("Failed to create table wal dir $0", DirName(wal_dir)));

  RETURN_NOT_OK_PREPEND(env_util::CreateDirIfMissing(options.env, wal_dir),
                        Substitute("Failed to create tablet wal dir $0", wal_dir));

  scoped_refptr<Log> new_log(new Log(options,
                                     wal_dir,
                                     tablet_id,
                                     peer_uuid,
                                     schema,
                                     schema_version,
                                     metric_entity,
                                     append_thread_pool,
                                     allocation_thread_pool,
                                     create_new_segment));
  RETURN_NOT_OK(new_log->Init());
  log->swap(new_log);
  return Status::OK();
}

Log::Log(
    LogOptions options,
    string wal_dir,
    string tablet_id,
    string peer_uuid,
    const Schema& schema,
    uint32_t schema_version,
    const scoped_refptr<MetricEntity>& metric_entity,
    ThreadPool* append_thread_pool,
    ThreadPool* allocation_thread_pool,
    CreateNewSegment create_new_segment)
    : options_(std::move(options)),
      wal_dir_(std::move(wal_dir)),
      tablet_id_(std::move(tablet_id)),
      peer_uuid_(std::move(peer_uuid)),
      schema_(schema),
      schema_version_(schema_version),
      active_segment_sequence_number_(options.initial_active_segment_sequence_number),
      log_state_(kLogInitialized),
      max_segment_size_(options_.segment_size_bytes),
      // We halve the initial log segment size here because we double it for every new segment,
      // including the very first segment.
      cur_max_segment_size_((options.initial_segment_size_bytes + 1) / 2),
      appender_(new Appender(this, append_thread_pool)),
      allocation_token_(allocation_thread_pool->NewToken(ThreadPool::ExecutionMode::SERIAL)),
      durable_wal_write_(options_.durable_wal_write),
      interval_durable_wal_write_(options_.interval_durable_wal_write),
      bytes_durable_wal_write_mb_(options_.bytes_durable_wal_write_mb),
      sync_disabled_(false),
      allocation_state_(kAllocationNotStarted),
      metric_entity_(metric_entity),
      on_disk_size_(0),
      log_prefix_(consensus::MakeTabletLogPrefix(tablet_id_, peer_uuid_)),
      create_new_segment_at_start_(create_new_segment) {
  set_wal_retention_secs(options.retention_secs);
  if (metric_entity_) {
    metrics_.reset(new LogMetrics(metric_entity_));
  }
}

Status Log::Init() {
  std::lock_guard<percpu_rwlock> write_lock(state_lock_);
  CHECK_EQ(kLogInitialized, log_state_);
  // Init the index
  log_index_.reset(new LogIndex(wal_dir_));
  // Reader for previous segments.
  RETURN_NOT_OK(LogReader::Open(get_env(),
                                log_index_,
                                tablet_id_,
                                wal_dir_,
                                peer_uuid_,
                                metric_entity_.get(),
                                &reader_));

  // The case where we are continuing an existing log.  We must pick up where the previous WAL left
  // off in terms of sequence numbers.
  if (reader_->num_segments() != 0) {
    VLOG_WITH_PREFIX(1) << "Using existing " << reader_->num_segments()
                        << " segments from path: " << wal_dir_;

    vector<scoped_refptr<ReadableLogSegment> > segments;
    RETURN_NOT_OK(reader_->GetSegmentsSnapshot(&segments));
    active_segment_sequence_number_ = segments.back()->header().sequence_number();
    LOG_WITH_PREFIX(INFO) << "Opened existing logs. Last segment is " << segments.back()->path();
  }

  if (durable_wal_write_) {
    YB_LOG_FIRST_N(INFO, 1) << "durable_wal_write is turned on.";
  } else if (interval_durable_wal_write_) {
    YB_LOG_FIRST_N(INFO, 1) << "interval_durable_wal_write_ms is turned on to sync every "
                            << interval_durable_wal_write_.ToMilliseconds() << " ms.";
  } else if (bytes_durable_wal_write_mb_ > 0) {
    YB_LOG_FIRST_N(INFO, 1) << "bytes_durable_wal_write_mb is turned on to sync every "
     << bytes_durable_wal_write_mb_ << " MB of data.";
  } else {
    YB_LOG_FIRST_N(INFO, 1) << "durable_wal_write is turned off. Buffered IO will be used for WAL.";
  }

  if (create_new_segment_at_start_) {
    RETURN_NOT_OK(EnsureInitialNewSegmentAllocated());
  }
  return Status::OK();
}

Status Log::AsyncAllocateSegment() {
  SCHECK_EQ(
      allocation_state_.load(std::memory_order_acquire), kAllocationNotStarted, AlreadyPresent,
      "Allocation already running");
  allocation_status_.Reset();
  allocation_state_.store(kAllocationInProgress, std::memory_order_release);
  return allocation_token_->SubmitClosure(Bind(&Log::SegmentAllocationTask, Unretained(this)));
}

Status Log::CloseCurrentSegment() {
  if (!footer_builder_.has_min_replicate_index()) {
    VLOG_WITH_PREFIX(1) << "Writing a segment without any REPLICATE message. Segment: "
                        << active_segment_->path();
  }
  VLOG_WITH_PREFIX(2) << "Segment footer for " << active_segment_->path()
                      << ": " << footer_builder_.ShortDebugString();

  footer_builder_.set_close_timestamp_micros(GetCurrentTimeMicros());
  return active_segment_->WriteFooterAndClose(footer_builder_);
}

Status Log::RollOver() {
  SCOPED_LATENCY_METRIC(metrics_, roll_latency);

  // Check if any errors have occurred during allocation
  RETURN_NOT_OK(allocation_status_.Get());

  DCHECK_EQ(allocation_state(), kAllocationFinished);

  LOG_WITH_PREFIX(INFO) << Format("Last appended OpId in segment $0: $1", active_segment_->path(),
                                  last_appended_entry_op_id_.ToString());

  RETURN_NOT_OK(Sync());
  RETURN_NOT_OK(CloseCurrentSegment());

  RETURN_NOT_OK(SwitchToAllocatedSegment());

  LOG_WITH_PREFIX(INFO) << "Rolled over to a new segment: " << active_segment_->path();
  return Status::OK();
}

Status Log::Reserve(LogEntryTypePB type,
                    LogEntryBatchPB* entry_batch,
                    LogEntryBatch** reserved_entry) {
  TRACE_EVENT0("log", "Log::Reserve");
  DCHECK(reserved_entry != nullptr);
  {
    SharedLock<rw_spinlock> read_lock(state_lock_.get_lock());
    CHECK_EQ(kLogWriting, log_state_);
  }

  // In DEBUG builds, verify that all of the entries in the batch match the specified type.  In
  // non-debug builds the foreach loop gets optimized out.
#ifndef NDEBUG
  for (const LogEntryPB& entry : entry_batch->entry()) {
    DCHECK_EQ(entry.type(), type) << "Bad batch: " << entry_batch->DebugString();
  }
#endif

  auto new_entry_batch = std::make_unique<LogEntryBatch>(type, std::move(*entry_batch));
  new_entry_batch->MarkReserved();

  // Release the memory back to the caller: this will be freed when
  // the entry is removed from the queue.
  //
  // TODO (perf) Use a ring buffer instead of a blocking queue and set
  // 'reserved_entry' to a pre-allocated slot in the buffer.
  *reserved_entry = new_entry_batch.release();
  return Status::OK();
}

Status Log::TEST_AsyncAppendWithReplicates(
    LogEntryBatch* entry, const ReplicateMsgs& replicates, const StatusCallback& callback) {
  entry->SetReplicates(replicates);
  return AsyncAppend(entry, callback);
}

Status Log::AsyncAppend(LogEntryBatch* entry_batch, const StatusCallback& callback) {
  {
    SharedLock<rw_spinlock> read_lock(state_lock_.get_lock());
    CHECK_EQ(kLogWriting, log_state_);
  }

  entry_batch->set_callback(callback);
  entry_batch->MarkReady();

  if (entry_batch->HasReplicateEntries()) {
    last_submitted_op_id_ = entry_batch->MaxReplicateOpId();
  }

  auto submit_status = appender_->Submit(entry_batch);
  if (PREDICT_FALSE(!submit_status.ok())) {
    LOG_WITH_PREFIX(WARNING)
        << "Failed to submit batch " << entry_batch->MaxReplicateOpId() << ": " << submit_status;
    delete entry_batch;
    return kLogShutdownStatus;
  }

  return Status::OK();
}

Status Log::AsyncAppendReplicates(const ReplicateMsgs& msgs, const yb::OpId& committed_op_id,
                                  RestartSafeCoarseTimePoint batch_mono_time,
                                  const StatusCallback& callback) {
  auto batch = CreateBatchFromAllocatedOperations(msgs);
  if (!committed_op_id.empty()) {
    committed_op_id.ToPB(batch.mutable_committed_op_id());
  }
  // Set batch mono time if it was specified.
  if (batch_mono_time != RestartSafeCoarseTimePoint()) {
    batch.set_mono_time(batch_mono_time.ToUInt64());
  }

  LogEntryBatch* reserved_entry_batch;
  RETURN_NOT_OK(Reserve(REPLICATE, &batch, &reserved_entry_batch));

  // If we're able to reserve, set the vector of replicate shared pointers in the LogEntryBatch.
  // This will make sure there's a reference for each replicate while we're appending.
  reserved_entry_batch->SetReplicates(msgs);

  RETURN_NOT_OK(AsyncAppend(reserved_entry_batch, callback));
  return Status::OK();
}

bool Log::NeedNewSegment(uint32_t entry_batch_bytes) {
  return (active_segment_->Size() + entry_batch_bytes + 4) > cur_max_segment_size_;
}

Status Log::RollOverIfNecessary(uint32_t entry_batch_bytes) {
  // If the size of this entry overflows the current segment, get a new one.
  auto allocation_state = this->allocation_state();
  if (allocation_state == kAllocationNotStarted) {
    if (!NeedNewSegment(entry_batch_bytes)) {
      return Status::OK();
    }
  }
  enum class Outcome {
    kNotDefined,
    kRunRollOver,
    kWaitRollOver,
    kDoNothing,
  };
  Outcome outcome = Outcome::kNotDefined;
  {
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    switch (allocation_state) {
      case kAllocationNotStarted: {
          if (!NeedNewSegment(entry_batch_bytes)) {
            return Status::OK();
          }
          LOG_WITH_PREFIX(INFO) << "Max segment size " << cur_max_segment_size_ << " reached. "
                                << "Starting new segment allocation. ";
          auto status = AsyncAllocateSegment();
          if (!status.ok()) {
            if (!status.IsAlreadyPresent()) {
              return status;
            }
            outcome = Outcome::kWaitRollOver;
          } else if (options_.async_preallocate_segments) {
            allocation_requested_ = true;
            outcome = Outcome::kDoNothing;
          } else {
            outcome = Outcome::kRunRollOver;
          }
        } break;
      case kAllocationFinished: {
          if (!allocation_requested_) {
            outcome = Outcome::kWaitRollOver;
          } else {
            outcome = Outcome::kRunRollOver;
            allocation_requested_ = false;
          }
        } break;
      case kAllocationInProgress: {
        VLOG_WITH_PREFIX(1) << "Segment allocation already in progress...";
        outcome = allocation_requested_ ? Outcome::kDoNothing : Outcome::kWaitRollOver;
      } break;
    }
  }
  switch (outcome) {
    case Outcome::kNotDefined:
      FATAL_INVALID_ENUM_VALUE(SegmentAllocationState, allocation_state);
      return Status::OK();
    case Outcome::kRunRollOver:
      LOG_SLOW_EXECUTION(WARNING, 50, "Log roll took a long time") {
        return RollOver();
      }
      return Status::OK();
    case Outcome::kWaitRollOver: {
        std::unique_lock<std::mutex> lock(allocation_mutex_);
        allocation_cond_.wait(lock, [this] {
          return allocation_state_.load(std::memory_order_acquire) == kAllocationNotStarted;
        });
      }
      return Status::OK();
    case Outcome::kDoNothing:
      return Status::OK();
  }
  FATAL_INVALID_ENUM_VALUE(Outcome, outcome);
}

Status Log::DoAppend(LogEntryBatch* entry_batch,
                     bool caller_owns_operation,
                     bool skip_wal_write) {
  if (!skip_wal_write) {
    RETURN_NOT_OK(entry_batch->Serialize());
    Slice entry_batch_data = entry_batch->data();
    LOG_IF(DFATAL, entry_batch_data.size() <= 0 && !entry_batch->flush_marker())
        << "Cannot call DoAppend() with no data";

    uint32_t entry_batch_bytes = entry_batch->total_size_bytes();
    // If there is no data to write return OK.
    if (PREDICT_FALSE(entry_batch_bytes == 0)) {
      return Status::OK();
    }

    RETURN_NOT_OK(RollOverIfNecessary(entry_batch_bytes));

    int64_t start_offset = active_segment_->written_offset();

    LOG_SLOW_EXECUTION(WARNING, 50, "Append to log took a long time") {
      SCOPED_LATENCY_METRIC(metrics_, append_latency);
      LongOperationTracker long_operation_tracker(
          "Log append", FLAGS_consensus_log_scoped_watch_delay_append_threshold_ms * 1ms);

      RETURN_NOT_OK(active_segment_->WriteEntryBatch(entry_batch_data));
    }

    if (metrics_) {
      metrics_->bytes_logged->IncrementBy(entry_batch_bytes);
    }

    // Populate the offset and sequence number for the entry batch if we did a WAL write.
    entry_batch->offset_ = start_offset;
    entry_batch->active_segment_sequence_number_ = active_segment_sequence_number_;
  }

  // We keep track of the last-written OpId here. This is needed to initialize Consensus on
  // startup.
  if (entry_batch->HasReplicateEntries()) {
    last_appended_entry_op_id_ = entry_batch->MaxReplicateOpId();
  }

  CHECK_OK(UpdateIndexForBatch(*entry_batch));
  UpdateFooterForBatch(entry_batch);

  // We expect the caller to free the actual entries if caller_owns_operation is set.
  if (caller_owns_operation) {
    for (int i = 0; i < entry_batch->entry_batch_pb_.entry_size(); i++) {
      LogEntryPB* entry_pb = entry_batch->entry_batch_pb_.mutable_entry(i);
      entry_pb->release_replicate();
    }
  }

  return Status::OK();
}

Status Log::UpdateIndexForBatch(const LogEntryBatch& batch) {
  if (batch.type_ != REPLICATE) {
    return Status::OK();
  }

  for (const LogEntryPB& entry_pb : batch.entry_batch_pb_.entry()) {
    LogIndexEntry index_entry;

    index_entry.op_id = yb::OpId::FromPB(entry_pb.replicate().id());
    index_entry.segment_sequence_number = batch.active_segment_sequence_number_;
    index_entry.offset_in_segment = batch.offset_;
    RETURN_NOT_OK(log_index_->AddEntry(index_entry));
  }
  return Status::OK();
}

void Log::UpdateFooterForBatch(LogEntryBatch* batch) {
  footer_builder_.set_num_entries(footer_builder_.num_entries() + batch->count());

  // We keep track of the last-written OpId here.  This is needed to initialize Consensus on
  // startup.  We also retrieve the OpId of the first operation in the batch so that, if we roll
  // over to a new segment, we set the first operation in the footer immediately.
  // Update the index bounds for the current segment.
  for (const LogEntryPB& entry_pb : batch->entry_batch_pb_.entry()) {
    int64_t index = entry_pb.replicate().id().index();
    if (!footer_builder_.has_min_replicate_index() ||
        index < footer_builder_.min_replicate_index()) {
      footer_builder_.set_min_replicate_index(index);
      min_replicate_index_.store(index, std::memory_order_release);
    }
    if (!footer_builder_.has_max_replicate_index() ||
        index > footer_builder_.max_replicate_index()) {
      footer_builder_.set_max_replicate_index(index);
    }
  }
}

Status Log::AllocateSegmentAndRollOver() {
  {
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    RETURN_NOT_OK(AsyncAllocateSegment());
  }
  return RollOver();
}

Status Log::EnsureInitialNewSegmentAllocated() {
  if (log_state_ == LogState::kLogWriting) {
    // New segment already created.
    return Status::OK();
  }
  if (log_state_ != LogState::kLogInitialized) {
    return STATUS_FORMAT(
        IllegalState, "Unexpected log state in EnsureInitialNewSegmentAllocated: $0", log_state_);
  }
  {
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    RETURN_NOT_OK(AsyncAllocateSegment());
  }
  RETURN_NOT_OK(allocation_status_.Get());
  RETURN_NOT_OK(SwitchToAllocatedSegment());

  RETURN_NOT_OK(appender_->Init());
  log_state_ = LogState::kLogWriting;
  return Status::OK();
}

Status Log::Sync() {
  TRACE_EVENT0("log", "Sync");
  SCOPED_LATENCY_METRIC(metrics_, sync_latency);

  if (!sync_disabled_) {
    if (PREDICT_FALSE(GetAtomicFlag(&FLAGS_log_inject_latency))) {
      Random r(GetCurrentTimeMicros());
      int sleep_ms = r.Normal(GetAtomicFlag(&FLAGS_log_inject_latency_ms_mean),
                              GetAtomicFlag(&FLAGS_log_inject_latency_ms_stddev));
      if (sleep_ms > 0) {
        LOG_WITH_PREFIX(INFO) << "Injecting " << sleep_ms << "ms of latency in Log::Sync()";
        SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
      }
    }

    bool timed_or_data_limit_sync = false;
    if (!durable_wal_write_ && periodic_sync_needed_.load()) {
      if (interval_durable_wal_write_) {
        if (MonoTime::Now() > periodic_sync_earliest_unsync_entry_time_
            + interval_durable_wal_write_) {
          timed_or_data_limit_sync = true;
        }
      }
      if (bytes_durable_wal_write_mb_ > 0) {
        if (periodic_sync_unsynced_bytes_ >= bytes_durable_wal_write_mb_ * 1_MB) {
          timed_or_data_limit_sync = true;
        }
      }
    }

    if (durable_wal_write_ || timed_or_data_limit_sync) {
      periodic_sync_needed_.store(false);
      periodic_sync_unsynced_bytes_ = 0;
      LOG_SLOW_EXECUTION(WARNING, 50, "Fsync log took a long time") {
        RETURN_NOT_OK(active_segment_->Sync());
      }
    }
  }

  // Update the reader on how far it can read the active segment.
  reader_->UpdateLastSegmentOffset(active_segment_->written_offset());

  {
    std::lock_guard<std::mutex> write_lock(last_synced_entry_op_id_mutex_);
    last_synced_entry_op_id_.store(last_appended_entry_op_id_, boost::memory_order_release);
    last_synced_entry_op_id_cond_.notify_all();
  }

  return Status::OK();
}

Status Log::GetSegmentsToGCUnlocked(int64_t min_op_idx, SegmentSequence* segments_to_gc) const {
  // Find the prefix of segments in the segment sequence that is guaranteed not to include
  // 'min_op_idx'.
  RETURN_NOT_OK(reader_->GetSegmentPrefixNotIncluding(
      min_op_idx, cdc_min_replicated_index_.load(std::memory_order_acquire), segments_to_gc));

  int max_to_delete = std::max(reader_->num_segments() - FLAGS_log_min_segments_to_retain, 0);
  if (segments_to_gc->size() > max_to_delete) {
    VLOG_WITH_PREFIX(2)
        << "GCing " << segments_to_gc->size() << " in " << wal_dir_
        << " would not leave enough remaining segments to satisfy minimum "
        << "retention requirement. Only considering "
        << max_to_delete << "/" << reader_->num_segments();
    segments_to_gc->resize(max_to_delete);
  } else if (segments_to_gc->size() < max_to_delete) {
    int extra_segments = max_to_delete - segments_to_gc->size();
    VLOG_WITH_PREFIX(2) << "Too many log segments, need to GC " << extra_segments << " more.";
  }

  // Don't GC segments that are newer than the configured time-based retention.
  int64_t now = GetCurrentTimeMicros();
  for (int i = 0; i < segments_to_gc->size(); i++) {
    const scoped_refptr<ReadableLogSegment>& segment = (*segments_to_gc)[i];

    // Segments here will always have a footer, since we don't return the in-progress segment up
    // above. However, segments written by older YB builds may not have the timestamp info (TODO:
    // make sure we indeed care about these old builds). In that case, we're allowed to GC them.
    if (!segment->footer().has_close_timestamp_micros()) continue;

    int64_t age_seconds = (now - segment->footer().close_timestamp_micros()) / 1000000;
    if (age_seconds < wal_retention_secs()) {
      VLOG_WITH_PREFIX(2)
          << "Segment " << segment->path() << " is only " << age_seconds << "s old: "
          << "cannot GC it yet due to configured time-based retention policy.";
      // Truncate the list of segments to GC here -- if this one is too new, then all later ones are
      // also too new.
      segments_to_gc->resize(i);
      break;
    }
  }

  return Status::OK();
}

Status Log::Append(LogEntryPB* phys_entry,
                   LogEntryMetadata entry_metadata,
                   bool skip_wal_write) {
  LogEntryBatchPB entry_batch_pb;
  if (entry_metadata.entry_time != RestartSafeCoarseTimePoint()) {
    entry_batch_pb.set_mono_time(entry_metadata.entry_time.ToUInt64());
  }

  entry_batch_pb.mutable_entry()->AddAllocated(phys_entry);
  LogEntryBatch entry_batch(phys_entry->type(), std::move(entry_batch_pb));
  // Mark this as reserved, as we're building it from preallocated data.
  entry_batch.state_ = LogEntryBatch::kEntryReserved;
  // Ready assumes the data is reserved before it is ready.
  entry_batch.MarkReady();
  if (skip_wal_write) {
    // Get the LogIndex entry from read path metadata.
    entry_batch.offset_ = entry_metadata.offset;
    entry_batch.active_segment_sequence_number_ = entry_metadata.active_segment_sequence_number;
  }
  Status s = DoAppend(&entry_batch, false, skip_wal_write);
  if (s.ok() && !skip_wal_write) {
    // Only sync if we actually performed a wal write.
    s = Sync();
  }
  entry_batch.entry_batch_pb_.mutable_entry()->ExtractSubrange(0, 1, nullptr);
  return s;
}

Status Log::WaitUntilAllFlushed() {
  // In order to make sure we empty the queue we need to use the async API.
  LogEntryBatchPB entry_batch;
  entry_batch.add_entry()->set_type(log::FLUSH_MARKER);
  LogEntryBatch* reserved_entry_batch;
  RETURN_NOT_OK(Reserve(FLUSH_MARKER, &entry_batch, &reserved_entry_batch));
  Synchronizer s;
  RETURN_NOT_OK(AsyncAppend(reserved_entry_batch, s.AsStatusCallback()));
  return s.Wait();
}

void Log::set_wal_retention_secs(uint32_t wal_retention_secs) {
  LOG_WITH_PREFIX(INFO) << "Setting log wal retention time to " << wal_retention_secs << " seconds";
  wal_retention_secs_.store(wal_retention_secs, std::memory_order_release);
}

uint32_t Log::wal_retention_secs() const {
  uint32_t wal_retention_secs = wal_retention_secs_.load(std::memory_order_acquire);
  auto flag_wal_retention = ANNOTATE_UNPROTECTED_READ(FLAGS_log_min_seconds_to_retain);
  return flag_wal_retention > 0 ?
      std::max(wal_retention_secs, static_cast<uint32_t>(flag_wal_retention)) :
      wal_retention_secs;
}

yb::OpId Log::GetLatestEntryOpId() const {
  return last_synced_entry_op_id_.load(boost::memory_order_acquire);
}

int64_t Log::GetMinReplicateIndex() const {
  return min_replicate_index_.load(std::memory_order_acquire);
}

yb::OpId Log::WaitForSafeOpIdToApply(const yb::OpId& min_allowed, MonoDelta duration) {
  if (FLAGS_TEST_log_consider_all_ops_safe || all_op_ids_safe_) {
    return min_allowed;
  }

  auto result = last_synced_entry_op_id_.load(boost::memory_order_acquire);

  if (result < min_allowed) {
    auto start = CoarseMonoClock::Now();
    std::unique_lock<std::mutex> lock(last_synced_entry_op_id_mutex_);
    auto wait_time = duration ? duration.ToSteadyDuration()
                              : FLAGS_wait_for_safe_op_id_to_apply_default_timeout_ms * 1ms;
    for (;;) {
      if (last_synced_entry_op_id_cond_.wait_for(
              lock, wait_time, [this, min_allowed, &result] {
            result = last_synced_entry_op_id_.load(boost::memory_order_acquire);
            return result >= min_allowed;
      })) {
        break;
      }
      if (duration) {
        return yb::OpId();
      }
      // TODO(bogdan): If the log is closed at this point, consider refactoring to return status
      // and fail cleanly.
      LOG_WITH_PREFIX(ERROR) << "Appender stack: " << appender_->GetRunThreadStack();
      LOG_WITH_PREFIX(DFATAL)
          << "Long wait for safe op id: " << min_allowed
          << ", current: " << GetLatestEntryOpId()
          << ", last appended: " << last_appended_entry_op_id_
          << ", last submitted: " << last_submitted_op_id_
          << ", appender: " << appender_->ToString()
          << ", passed: " << (CoarseMonoClock::Now() - start);
    }
  }

  DCHECK_GE(result.term, min_allowed.term)
      << "result: " << result << ", min_allowed: " << min_allowed;
  return result;
}

Status Log::GC(int64_t min_op_idx, int32_t* num_gced) {
  CHECK_GE(min_op_idx, 0);

  LOG_WITH_PREFIX(INFO) << "Running Log GC on " << wal_dir_ << ": retaining ops >= " << min_op_idx
                        << ", log segment size = " << options_.segment_size_bytes;
  VLOG_TIMING(1, "Log GC") {
    SegmentSequence segments_to_delete;

    {
      std::lock_guard<percpu_rwlock> l(state_lock_);
      CHECK_EQ(kLogWriting, log_state_);

      RETURN_NOT_OK(GetSegmentsToGCUnlocked(min_op_idx, &segments_to_delete));

      if (segments_to_delete.size() == 0) {
        VLOG_WITH_PREFIX(1) << "No segments to delete.";
        *num_gced = 0;
        return Status::OK();
      }
      // Trim the prefix of segments from the reader so that they are no longer referenced by the
      // log.
      RETURN_NOT_OK(reader_->TrimSegmentsUpToAndIncluding(
          segments_to_delete[segments_to_delete.size() - 1]->header().sequence_number()));
    }

    // Now that they are no longer referenced by the Log, delete the files.
    *num_gced = 0;
    for (const scoped_refptr<ReadableLogSegment>& segment : segments_to_delete) {
      LOG_WITH_PREFIX(INFO) << "Deleting log segment in path: " << segment->path()
                            << " (GCed ops < " << min_op_idx << ")";
      RETURN_NOT_OK(get_env()->DeleteFile(segment->path()));
      (*num_gced)++;
    }

    // Determine the minimum remaining replicate index in order to properly GC the index chunks.
    int64_t min_remaining_op_idx = reader_->GetMinReplicateIndex();
    if (min_remaining_op_idx > 0) {
      log_index_->GC(min_remaining_op_idx);
    }
  }
  return Status::OK();
}

Status Log::GetGCableDataSize(int64_t min_op_idx, int64_t* total_size) const {
  if (min_op_idx < 0) {
    return STATUS_FORMAT(InvalidArgument, "Invalid min op index $0", min_op_idx);
  }

  SegmentSequence segments_to_delete;
  *total_size = 0;
  {
    SharedLock<rw_spinlock> read_lock(state_lock_.get_lock());
    if (log_state_ != kLogWriting) {
      return STATUS_FORMAT(IllegalState, "Invalid log state $0, expected $1",
          log_state_, kLogWriting);
    }
    Status s = GetSegmentsToGCUnlocked(min_op_idx, &segments_to_delete);

    if (!s.ok() || segments_to_delete.size() == 0) {
      return Status::OK();
    }
  }
  for (const scoped_refptr<ReadableLogSegment>& segment : segments_to_delete) {
    *total_size += segment->file_size();
  }
  return Status::OK();
}

void Log::GetMaxIndexesToSegmentSizeMap(int64_t min_op_idx,
                                        std::map<int64_t, int64_t>* max_idx_to_segment_size)
                                        const {
  SharedLock<rw_spinlock> read_lock(state_lock_.get_lock());
  CHECK_EQ(kLogWriting, log_state_);
  // We want to retain segments so we're only asking the extra ones.
  int segments_count = std::max(reader_->num_segments() - FLAGS_log_min_segments_to_retain, 0);
  if (segments_count == 0) {
    return;
  }

  int64_t now = GetCurrentTimeMicros();
  int64_t max_close_time_us = now - (wal_retention_secs() * 1000000);
  reader_->GetMaxIndexesToSegmentSizeMap(min_op_idx, segments_count, max_close_time_us,
                                         max_idx_to_segment_size);
}

LogReader* Log::GetLogReader() const {
  return reader_.get();
}

Status Log::GetSegmentsSnapshot(SegmentSequence* segments) const {
  SharedLock<rw_spinlock> read_lock(state_lock_.get_lock());
  if (!reader_) {
    return STATUS(IllegalState, "Log already closed");
  }

  return reader_->GetSegmentsSnapshot(segments);
}

uint64_t Log::OnDiskSize() {
  SegmentSequence segments;
  {
    shared_lock<rw_spinlock> l(state_lock_.get_lock());
    // If the log is closed, the tablet is either being deleted or tombstoned,
    // so we don't count the size of its log anymore as it should be deleted.
    if (log_state_ == kLogClosed || !reader_->GetSegmentsSnapshot(&segments).ok()) {
      return on_disk_size_.load();
    }
  }
  uint64_t ret = 0;
  for (const auto& segment : segments) {
    ret += segment->file_size();
  }

  on_disk_size_.store(ret, std::memory_order_release);
  return ret;
}

void Log::SetSchemaForNextLogSegment(const Schema& schema,
                                     uint32_t version) {
  std::lock_guard<rw_spinlock> l(schema_lock_);
  schema_ = schema;
  schema_version_ = version;
}

Status Log::Close() {
  // Allocation pool is used from appender pool, so we should shutdown appender first.
  appender_->Shutdown();
  allocation_token_.reset();

  std::lock_guard<percpu_rwlock> l(state_lock_);
  switch (log_state_) {
    case kLogWriting:
      RETURN_NOT_OK(Sync());
      RETURN_NOT_OK(CloseCurrentSegment());
      RETURN_NOT_OK(ReplaceSegmentInReaderUnlocked());
      log_state_ = kLogClosed;
      VLOG_WITH_PREFIX(1) << "Log closed";

      // Release FDs held by these objects.
      log_index_.reset();
      reader_.reset();

      return Status::OK();

    case kLogClosed:
      VLOG_WITH_PREFIX(1) << "Log already closed";
      return Status::OK();

    default:
      return STATUS(IllegalState, Substitute("Bad state for Close() $0", log_state_));
  }
}

const int Log::num_segments() const {
  boost::shared_lock<rw_spinlock> read_lock(state_lock_.get_lock());
  return (reader_) ? reader_->num_segments() : 0;
}

scoped_refptr<ReadableLogSegment> Log::GetSegmentBySequenceNumber(int64_t seq) const {
  SharedLock<rw_spinlock> read_lock(state_lock_.get_lock());
  if (!reader_) {
    return nullptr;
  }

  return reader_->GetSegmentBySequenceNumber(seq);
}

bool Log::HasOnDiskData(FsManager* fs_manager, const string& wal_dir) {
  return fs_manager->env()->FileExists(wal_dir);
}

Status Log::DeleteOnDiskData(Env* env,
                             const string& tablet_id,
                             const string& wal_dir,
                             const string& peer_uuid) {
  if (!env->FileExists(wal_dir)) {
    return Status::OK();
  }
  LOG(INFO) << "T " << tablet_id << " P " << peer_uuid
            << ": Deleting WAL dir " << wal_dir;
  RETURN_NOT_OK_PREPEND(env->DeleteRecursively(wal_dir),
                        "Unable to recursively delete WAL dir for tablet " + tablet_id);
  return Status::OK();
}

Status Log::FlushIndex() {
  if (!log_index_) {
    return Status::OK();
  }
  return log_index_->Flush();
}

Status Log::CopyTo(const std::string& dest_wal_dir) {
  RETURN_NOT_OK_PREPEND(env_util::CreateDirIfMissing(options_.env, dest_wal_dir),
                        Format("Failed to create tablet WAL dir $0", dest_wal_dir));
  // Make sure log segments we have so far are immutable, so we can hardlink them instead of
  // copying.
  if (footer_builder_.IsInitialized() && footer_builder_.num_entries() > 0) {
    // If active log segment has entries - close it and rollover to next one, so this one become
    // immutable. If active log segment empty - we will just skip it.
    RETURN_NOT_OK(AllocateSegmentAndRollOver());
  }
  RETURN_NOT_OK(log_index_->Flush());

  auto* const env = options_.env;
  const auto files = VERIFY_RESULT(env->GetChildren(wal_dir_, ExcludeDots::kTrue));

  const auto active_segment_filename =
      FsManager::GetWalSegmentFileName(active_segment_sequence_number_);

  for (const auto& file : files) {
    const auto src_path = JoinPathSegments(wal_dir_, file);
    const auto dest_path = JoinPathSegments(dest_wal_dir, file);

    // Segment files except the active one are immutable, so we can use hardlinks.
    if (file == active_segment_filename) {
      // Skip active segment file, because we've just rolled over to it and it is empty and not
      // closed.
      continue;
    } else if (FsManager::IsWalSegmentFileName(file)) {
      RETURN_NOT_OK(env->LinkFile(src_path, dest_path));
      VLOG_WITH_PREFIX(1) << Format("Hard linked $0 to $1", src_path, dest_path);
    } else {
      RETURN_NOT_OK_PREPEND(
          CopyFile(env, src_path, dest_path),
          Format("Failed to copy file $0 to $1", src_path, dest_path));
      VLOG_WITH_PREFIX(1) << Format("Copied $0 to $1", src_path, dest_path);
    }
  }
  return Status::OK();
}

uint64_t Log::NextSegmentDesiredSize() {
  return std::min(cur_max_segment_size_ * 2, max_segment_size_);
}

Status Log::PreAllocateNewSegment() {
  TRACE_EVENT1("log", "PreAllocateNewSegment", "file", next_segment_path_);
  CHECK_EQ(allocation_state(), kAllocationInProgress);

  WritableFileOptions opts;
  // We always want to sync on close: https://github.com/yugabyte/yugabyte-db/issues/3490
  opts.sync_on_close = true;
  opts.o_direct = durable_wal_write_;
  RETURN_NOT_OK(CreatePlaceholderSegment(opts, &next_segment_path_, &next_segment_file_));

  if (options_.preallocate_segments) {
    uint64_t next_segment_size = NextSegmentDesiredSize();
    TRACE("Preallocating $0 byte segment in $1", next_segment_size, next_segment_path_);
    // TODO (perf) zero the new segments -- this could result in additional performance
    // improvements.
    RETURN_NOT_OK(next_segment_file_->PreAllocate(next_segment_size));
  }

  {
    std::lock_guard<std::mutex> lock(allocation_mutex_);
    // We implement something like shared lock for allocation_state_, so modifications should be
    // done while holding the mutex.
    allocation_state_.store(kAllocationFinished, std::memory_order_release);
  }
  return Status::OK();
}

Status Log::SwitchToAllocatedSegment() {
  CHECK_EQ(allocation_state(), kAllocationFinished);

  // Increment "next" log segment seqno.
  active_segment_sequence_number_++;
  const string new_segment_path =
      FsManager::GetWalSegmentFilePath(wal_dir_, active_segment_sequence_number_);

  RETURN_NOT_OK(get_env()->RenameFile(next_segment_path_, new_segment_path));
  RETURN_NOT_OK(get_env()->SyncDir(wal_dir_));

  // Create a new segment.
  std::unique_ptr<WritableLogSegment> new_segment(
      new WritableLogSegment(new_segment_path, next_segment_file_));

  // Set up the new header and footer.
  LogSegmentHeaderPB header;
  header.set_major_version(kLogMajorVersion);
  header.set_minor_version(kLogMinorVersion);
  header.set_sequence_number(active_segment_sequence_number_);
  header.set_tablet_id(tablet_id_);

  // Set up the new footer. This will be maintained as the segment is written.
  footer_builder_.Clear();
  footer_builder_.set_num_entries(0);

  // Set the new segment's schema.
  {
    SharedLock<decltype(schema_lock_)> l(schema_lock_);
    SchemaToPB(schema_, header.mutable_schema());
    header.set_schema_version(schema_version_);
  }

  RETURN_NOT_OK(new_segment->WriteHeaderAndOpen(header));
  // Transform the currently-active segment into a readable one, since we need to be able to replay
  // the segments for other peers.
  {
    if (active_segment_.get() != nullptr) {
      std::lock_guard<decltype(state_lock_)> l(state_lock_);
      CHECK_OK(ReplaceSegmentInReaderUnlocked());
    }
  }

  // Open the segment we just created in readable form and add it to the reader.
  std::unique_ptr<RandomAccessFile> readable_file;
  RETURN_NOT_OK(get_env()->NewRandomAccessFile(new_segment_path, &readable_file));

  scoped_refptr<ReadableLogSegment> readable_segment(
    new ReadableLogSegment(new_segment_path,
                           shared_ptr<RandomAccessFile>(readable_file.release())));
  RETURN_NOT_OK(readable_segment->Init(header, new_segment->first_entry_offset()));
  RETURN_NOT_OK(reader_->AppendEmptySegment(readable_segment));

  // Now set 'active_segment_' to the new segment.
  active_segment_.reset(new_segment.release());
  cur_max_segment_size_ = NextSegmentDesiredSize();

  {
    std::lock_guard<decltype(allocation_mutex_)> lock_guard(allocation_mutex_);
    allocation_state_.store(kAllocationNotStarted, std::memory_order_release);
  }
  // Notify roll over waiters.
  allocation_cond_.notify_all();

  return Status::OK();
}

Status Log::ReplaceSegmentInReaderUnlocked() {
  // We should never switch to a new segment if we wrote nothing to the old one.
  CHECK(active_segment_->IsClosed());
  shared_ptr<RandomAccessFile> readable_file;
  RETURN_NOT_OK(OpenFileForRandom(
      get_env(), active_segment_->path(), &readable_file));

  scoped_refptr<ReadableLogSegment> readable_segment(
      new ReadableLogSegment(active_segment_->path(), readable_file));
  // Note: active_segment_->header() will only contain an initialized PB if we wrote the header out.
  RETURN_NOT_OK(readable_segment->Init(active_segment_->header(),
                                       active_segment_->footer(),
                                       active_segment_->first_entry_offset()));

  return reader_->ReplaceLastSegment(readable_segment);
}

Status Log::CreatePlaceholderSegment(const WritableFileOptions& opts,
                                     string* result_path,
                                     shared_ptr<WritableFile>* out) {
  string path_tmpl = JoinPathSegments(wal_dir_, kSegmentPlaceholderFileTemplate);
  VLOG_WITH_PREFIX(2) << "Creating temp. file for place holder segment, template: " << path_tmpl;
  std::unique_ptr<WritableFile> segment_file;
  RETURN_NOT_OK(get_env()->NewTempWritableFile(opts,
                                               path_tmpl,
                                               result_path,
                                               &segment_file));
  VLOG_WITH_PREFIX(1) << "Created next WAL segment, placeholder path: " << *result_path;
  out->reset(segment_file.release());
  return Status::OK();
}

uint64_t Log::active_segment_sequence_number() const {
  return active_segment_sequence_number_;
}

Status Log::TEST_SubmitFuncToAppendToken(const std::function<void()>& func) {
  return appender_->TEST_SubmitFunc(func);
}

Status Log::ResetLastSyncedEntryOpId(const OpId& op_id) {
  RETURN_NOT_OK(WaitUntilAllFlushed());

  OpId old_value;
  {
    std::lock_guard<std::mutex> write_lock(last_synced_entry_op_id_mutex_);
    old_value = last_synced_entry_op_id_.load(boost::memory_order_acquire);
    last_synced_entry_op_id_.store(op_id, boost::memory_order_release);
    last_synced_entry_op_id_cond_.notify_all();
  }
  LOG_WITH_PREFIX(INFO) << "Reset last synced entry op id from " << old_value << " to " << op_id;

  return Status::OK();
}

Log::~Log() {
  WARN_NOT_OK(Close(), "Error closing log");
}

// ------------------------------------------------------------------------------------------------
// LogEntryBatch

LogEntryBatch::LogEntryBatch(LogEntryTypePB type, LogEntryBatchPB&& entry_batch_pb)
    : type_(type),
      entry_batch_pb_(std::move(entry_batch_pb)),
      count_(entry_batch_pb_.entry().size()) {
  if (type_ != LogEntryTypePB::FLUSH_MARKER) {
    DCHECK_NE(entry_batch_pb_.mono_time(), 0);
  }
}

LogEntryBatch::~LogEntryBatch() {
  // ReplicateMsg objects are pointed to by LogEntryBatchPB but are really owned by shared pointers
  // in replicates_. To avoid double freeing, release them from the protobuf.
  for (auto& entry : *entry_batch_pb_.mutable_entry()) {
    if (entry.has_replicate()) {
      entry.release_replicate();
    }
  }
}

void LogEntryBatch::MarkReserved() {
  DCHECK_EQ(state_, kEntryInitialized);
  state_ = kEntryReserved;
}

bool LogEntryBatch::flush_marker() const {
  return count() == 1 && entry_batch_pb_.entry(0).type() == FLUSH_MARKER;
}

Status LogEntryBatch::Serialize() {
  DCHECK_EQ(state_, kEntryReady);
  buffer_.clear();
  // FLUSH_MARKER LogEntries are markers and are not serialized.
  if (PREDICT_FALSE(flush_marker())) {
    total_size_bytes_ = 0;
    state_ = kEntrySerialized;
    return Status::OK();
  }
  DCHECK_NE(entry_batch_pb_.mono_time(), 0);
  total_size_bytes_ = entry_batch_pb_.ByteSize();
  buffer_.reserve(total_size_bytes_);

  if (!pb_util::AppendToString(entry_batch_pb_, &buffer_)) {
    return STATUS(IOError, Substitute("unable to serialize the entry batch, contents: $1",
                                      entry_batch_pb_.DebugString()));
  }

  state_ = kEntrySerialized;
  return Status::OK();
}

void LogEntryBatch::MarkReady() {
  DCHECK_EQ(state_, kEntryReserved);
  state_ = kEntryReady;
}

}  // namespace log
}  // namespace yb
