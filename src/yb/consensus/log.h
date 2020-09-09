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

#ifndef YB_CONSENSUS_LOG_H_
#define YB_CONSENSUS_LOG_H_

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <boost/atomic.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "yb/common/schema.h"
#include "yb/consensus/consensus_fwd.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/spinlock.h"
#include "yb/util/async_util.h"
#include "yb/util/blocking_queue.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/opid.h"
#include "yb/util/promise.h"
#include "yb/util/status.h"
#include "yb/util/threadpool.h"
#include "yb/util/shared_lock.h"

namespace yb {

class MetricEntity;
class ThreadPool;

namespace cdc {
class CDCServiceTest_TestLogRetentionByOpId_MaxRentionTime_Test;
class CDCServiceTest_TestLogRetentionByOpId_MinSpace_Test;
}

namespace log {

struct LogMetrics;
class LogEntryBatch;
class LogIndex;
class LogReader;

YB_STRONGLY_TYPED_BOOL(CreateNewSegment);

// Log interface, inspired by Raft's (logcabin) Log. Provides durability to YugaByte as a normal
// Write Ahead Log and also plays the role of persistent storage for the consensus state machine.
//
// Note: This class is not thread safe, the caller is expected to synchronize Log::Reserve() and
// Log::Append() calls.
//
// Log uses group commit to improve write throughput and latency without compromising ordering and
// durability guarantees.
//
// To add operations to the log, the caller must obtain the lock and call Reserve() with the
// collection of operations to be added. Then, the caller may release the lock and call
// AsyncAppend(). Reserve() reserves a slot on a queue for the log entry; AsyncAppend() indicates
// that the entry in the slot is safe to write to disk and adds a callback that will be invoked once
// the entry is written and synchronized to disk.
//
// For sample usage see mt-log-test.cc
//
// Methods on this class are _not_ thread-safe and must be externally synchronized unless otherwise
// noted.
//
// Note: The Log needs to be Close()d before any log-writing class is destroyed, otherwise the Log
// might hold references to these classes to execute the callbacks after each write.
class Log : public RefCountedThreadSafe<Log> {
 public:
  static const Status kLogShutdownStatus;

  // Opens or continues a log and sets 'log' to the newly built Log.
  // After a successful Open() the Log is ready to receive entries, if create_new_segment is true.
  static CHECKED_STATUS Open(const LogOptions &options,
                             const std::string& tablet_id,
                             const std::string& wal_dir,
                             const std::string& peer_uuid,
                             const Schema& schema,
                             uint32_t schema_version,
                             const scoped_refptr<MetricEntity>& metric_entity,
                             ThreadPool *append_thread_pool,
                             ThreadPool* allocation_thread_pool,
                             int64_t cdc_min_replicated_index,
                             scoped_refptr<Log> *log,
                             CreateNewSegment create_new_segment = CreateNewSegment::kTrue);

  ~Log();

  // Reserves a spot in the log's queue for 'entry_batch'.
  //
  // 'reserved_entry' is initialized by this method and any resources associated with it will be
  // released in AsyncAppend().  In order to ensure correct ordering of operations across multiple
  // threads, calls to this method must be externally synchronized.
  //
  // WARNING: the caller _must_ call AsyncAppend() or else the log will "stall" and will never be
  // able to make forward progress.
  CHECKED_STATUS Reserve(LogEntryTypePB type,
                         LogEntryBatchPB* entry_batch,
                         LogEntryBatch** reserved_entry);

  // Asynchronously appends 'entry' to the log. Once the append completes and is synced, 'callback'
  // will be invoked.
  CHECKED_STATUS AsyncAppend(LogEntryBatch* entry,
                             const StatusCallback& callback);

  CHECKED_STATUS TEST_AsyncAppendWithReplicates(
      LogEntryBatch* entry, const ReplicateMsgs& replicates, const StatusCallback& callback);

  // Synchronously append a new entry to the log.  Log does not take ownership of the passed
  // 'entry'. If skip_wal_write is true, only update consensus metadata and LogIndex, skip write
  // to wal.
  // TODO get rid of this method, transition to the asynchronous API.
  CHECKED_STATUS Append(LogEntryPB* entry,
                        LogEntryMetadata entry_metadata,
                        bool skip_wal_write = false);

  // Append the given set of replicate messages, asynchronously.  This requires that the replicates
  // have already been assigned OpIds.
  CHECKED_STATUS AsyncAppendReplicates(const ReplicateMsgs& replicates, const OpId& committed_op_id,
                                       RestartSafeCoarseTimePoint batch_mono_time,
                                       const StatusCallback& callback);

  // Blocks the current thread until all the entries in the log queue are flushed and fsynced (if
  // fsync of log entries is enabled).
  CHECKED_STATUS WaitUntilAllFlushed();

  // The closure submitted to allocation_pool_ to allocate a new segment.
  void SegmentAllocationTask();

  // Syncs all state and closes the log.
  CHECKED_STATUS Close();

  // Return true if there is any on-disk data for the given tablet.
  static bool HasOnDiskData(FsManager* fs_manager, const std::string& tablet_id);

  // Delete all WAL data from the log associated with this tablet.
  // REQUIRES: The Log must be closed.
  static CHECKED_STATUS DeleteOnDiskData(Env* env,
                                         const std::string& tablet_id,
                                         const std::string& wal_dir,
                                         const std::string& peer_uuid);

  // Returns a reader that is able to read through the previous segments. The reader pointer is
  // guaranteed to be live as long as the log itself is initialized and live.
  LogReader* GetLogReader() const;

  CHECKED_STATUS GetSegmentsSnapshot(SegmentSequence* segments) const;

  void SetMaxSegmentSizeForTests(uint64_t max_segment_size) {
    max_segment_size_ = max_segment_size;
  }

  void DisableSync() {
    sync_disabled_ = true;
  }

  // If we previous called DisableSync(), we should restore the default behavior and then call
  // Sync() which will perform the actual syncing if required.
  CHECKED_STATUS ReEnableSyncIfRequired() {
    sync_disabled_ = false;
    return Sync();
  }

  // Get ID of tablet.
  const std::string& tablet_id() const {
    return tablet_id_;
  }

  // Gets the last-used OpId written to the log.  If no entry has ever been written to the log,
  // returns (0, 0)
  yb::OpId GetLatestEntryOpId() const;

  int64_t GetMinReplicateIndex() const;

  // Runs the garbage collector on the set of previous segments. Segments that only refer to in-mem
  // state that has been flushed are candidates for garbage collection.
  //
  // 'min_op_idx' is the minimum operation index required to be retained.  If successful, num_gced
  // is set to the number of deleted log segments.
  //
  // This method is thread-safe.
  CHECKED_STATUS GC(int64_t min_op_idx, int* num_gced);

  // Computes the amount of bytes that would have been GC'd if Log::GC had been called.
  CHECKED_STATUS GetGCableDataSize(int64_t min_op_idx, int64_t* total_size) const;

  // Returns a map of log index -> segment size, of all the segments that currently cannot be GCed
  // because in-memory structures have anchors in them.
  //
  // 'min_op_idx' is the minimum operation index to start looking from, meaning that we skip the
  // segment that contains it and then start recording segments.
  void GetMaxIndexesToSegmentSizeMap(int64_t min_op_idx,
                                     std::map<int64_t, int64_t>* max_idx_to_segment_size) const;

  // Returns the file system location of the currently active WAL segment.
  const WritableLogSegment* ActiveSegmentForTests() const {
    return active_segment_.get();
  }

  // Forces the Log to allocate a new segment and roll over.  This can be used to make sure all
  // entries appended up to this point are available in closed, readable segments.
  CHECKED_STATUS AllocateSegmentAndRollOver();

  // For a log created with CreateNewSegment::kFalse, this is used to finish log initialization by
  // allocating a new segment.
  CHECKED_STATUS EnsureInitialNewSegmentAllocated();

  // Returns the total size of the current segments, in bytes.
  // Returns 0 if the log is shut down.
  uint64_t OnDiskSize();

  // Set the schema for the _next_ log segment.
  //
  // This method is thread-safe.
  void SetSchemaForNextLogSegment(const Schema& schema, uint32_t version);

  void set_wal_retention_secs(uint32_t wal_retention_secs);

  uint32_t wal_retention_secs() const;

  // Waits until specified op id is added to log.
  // Returns current op id after waiting, which could be greater than or equal to specified op id.
  //
  // On timeout returns default constructed OpId.
  yb::OpId WaitForSafeOpIdToApply(const yb::OpId& op_id, MonoDelta duration = MonoDelta());

  // Return a readable segment with the given sequence number, or NULL if it
  // cannot be found (e.g. if it has already been GCed).
  scoped_refptr<ReadableLogSegment> GetSegmentBySequenceNumber(int64_t seq) const;

  void TEST_SetSleepDuration(const std::chrono::nanoseconds& duration) {
    sleep_duration_.store(duration, std::memory_order_release);
  }

  void TEST_SetAllOpIdsSafe(bool value) {
    all_op_ids_safe_ = value;
  }

  uint64_t active_segment_sequence_number() const;

  CHECKED_STATUS TEST_SubmitFuncToAppendToken(const std::function<void()>& func);

  // Returns the number of segments.
  const int num_segments() const;

  const std::string& LogPrefix() const {
    return log_prefix_;
  }

  void set_cdc_min_replicated_index(int64_t cdc_min_replicated_index) {
    // TODO: check that the passed index is greater than the current index.
    cdc_min_replicated_index_.store(cdc_min_replicated_index, std::memory_order_release);
  }

  int64_t cdc_min_replicated_index() {
    return cdc_min_replicated_index_.load(std::memory_order_acquire);
  }

  CHECKED_STATUS FlushIndex();

  // Copies log to a new dir.
  // Flushes necessary files and uses hard links where it is safe.
  CHECKED_STATUS CopyTo(const std::string& dest_wal_dir);

 private:
  friend class LogTest;
  friend class LogTestBase;

  FRIEND_TEST(LogTest, TestMultipleEntriesInABatch);
  FRIEND_TEST(LogTest, TestReadLogWithReplacedReplicates);
  FRIEND_TEST(LogTest, TestWriteAndReadToAndFromInProgressSegment);
  FRIEND_TEST(cdc::CDCServiceTest, TestLogRetentionByOpId_MaxRentionTime);
  FRIEND_TEST(cdc::CDCServiceTest, TestLogRetentionByOpId_MinSpace);

  class Appender;

  // Log state.
  enum LogState {
    kLogInitialized,
    kLogWriting,
    kLogClosed
  };

  // State of segment (pre-) allocation.
  enum SegmentAllocationState {
    kAllocationNotStarted, // No segment allocation requested
    kAllocationInProgress, // Next segment allocation started
    kAllocationFinished // Next segment ready
  };

  Log(LogOptions options,
      std::string wal_dir,
      std::string tablet_id,
      std::string peer_uuid,
      const Schema& schema,
      uint32_t schema_version,
      const scoped_refptr<MetricEntity>& metric_entity,
      ThreadPool* append_thread_pool,
      ThreadPool* allocation_thread_pool,
      CreateNewSegment create_new_segment = CreateNewSegment::kTrue);

  Env* get_env() {
    return options_.env;
  }

  // Initializes a new one or continues an existing log.
  CHECKED_STATUS Init();

  // Make segments roll over.
  CHECKED_STATUS RollOver();

  // Writes the footer and closes the current segment.
  CHECKED_STATUS CloseCurrentSegment();

  // Sets 'out' to a newly created temporary file (see Env::NewTempWritableFile()) for a placeholder
  // segment. Sets 'result_path' to the fully qualified path to the unique filename created for the
  // segment.
  CHECKED_STATUS CreatePlaceholderSegment(const WritableFileOptions& opts,
                                          std::string* result_path,
                                          std::shared_ptr<WritableFile>* out);

  // Creates a new WAL segment on disk, writes the next_segment_header_ to disk as the header, and
  // sets active_segment_ to point to this new segment.
  CHECKED_STATUS SwitchToAllocatedSegment() EXCLUDES(allocation_mutex_);

  // Preallocates the space for a new segment.
  CHECKED_STATUS PreAllocateNewSegment();

  // Returns the desired size for the next log segment to be created.
  uint64_t NextSegmentDesiredSize();

  // Writes serialized contents of 'entry' to the log. Called inside AppenderThread. If
  // 'caller_owns_operation' is true, then the 'operation' field of the entry will be released after
  // the entry is appended. If skip_wal_write is true, only update consensus metadata and LogIndex,
  // skip WAL write.
  //
  // TODO once Append() is removed, 'caller_owns_operation' and associated logic will no longer be
  // needed.
  CHECKED_STATUS DoAppend(
      LogEntryBatch* entry, bool caller_owns_operation = true, bool skip_wal_write = false);

  // Update footer_builder_ to reflect the log indexes seen in 'batch'.
  void UpdateFooterForBatch(LogEntryBatch* batch);

  // Update the LogIndex to include entries for the replicate messages found in 'batch'. The index
  // entry points to the offset 'start_offset' in the current log segment.
  CHECKED_STATUS UpdateIndexForBatch(const LogEntryBatch& batch);

  // Replaces the last "empty" segment in 'log_reader_', i.e. the one currently being written to, by
  // the same segment once properly closed.
  CHECKED_STATUS ReplaceSegmentInReaderUnlocked();

  CHECKED_STATUS Sync();

  // Helper method to get the segment sequence to GC based on the provided min_op_idx.
  CHECKED_STATUS GetSegmentsToGCUnlocked(int64_t min_op_idx, SegmentSequence* segments_to_gc) const;

  // Kick off an asynchronous task that pre-allocates a new log-segment, setting
  // 'allocation_status_'. To wait for the result of the task, use allocation_status_.Get().
  CHECKED_STATUS AsyncAllocateSegment() REQUIRES(allocation_mutex_);

  SegmentAllocationState allocation_state() EXCLUDES(allocation_mutex_) {
    return allocation_state_.load(std::memory_order_acquire);
  }

  bool NeedNewSegment(uint32_t entry_batch_bytes);
  CHECKED_STATUS RollOverIfNecessary(uint32_t entry_batch_bytes) EXCLUDES(allocation_mutex_);

  LogOptions options_;

  // The dir path where the write-ahead log for this tablet is stored.
  std::string wal_dir_;

  // The ID of the tablet this log is dedicated to.
  std::string tablet_id_;

  // Peer this log is dedicated to.
  std::string peer_uuid_;

  // Lock to protect modifications to schema_ and schema_version_.
  mutable rw_spinlock schema_lock_;

  // The current schema of the tablet this log is dedicated to.
  Schema schema_;

  // The schema version
  uint32_t schema_version_;

  // The currently active segment being written.
  gscoped_ptr<WritableLogSegment> active_segment_;

  // The current (active) segment sequence number. Initialized in the Log constructor based on
  // LogOptions.
  std::atomic<uint64_t> active_segment_sequence_number_;

  // The writable file for the next allocated segment
  std::shared_ptr<WritableFile> next_segment_file_;

  // The path for the next allocated segment.
  std::string next_segment_path_;

  // Lock to protect mutations to log_state_ and other shared state variables.
  mutable percpu_rwlock state_lock_;

  LogState log_state_;

  // A reader for the previous segments that were not yet GC'd.
  std::unique_ptr<LogReader> reader_;

  // Index which translates between operation indexes and the position of the operation in the log.
  scoped_refptr<LogIndex> log_index_;

  // Lock for notification of last_synced_entry_op_id_ changes.
  mutable std::mutex last_synced_entry_op_id_mutex_;
  mutable std::condition_variable last_synced_entry_op_id_cond_;

  // The last known OpId for a REPLICATE message appended and synced to this log (any segment).
  // NOTE: this op is not necessarily durable unless gflag durable_wal_write is true.
  boost::atomic<yb::OpId> last_synced_entry_op_id_{yb::OpId()};

  // The last know OpId for a REPLICATE message appended to this log (any segment).
  // This variable is not accessed concurrently.
  yb::OpId last_appended_entry_op_id_;

  yb::OpId last_submitted_op_id_;

  // A footer being prepared for the current segment.  When the segment is closed, it will be
  // written.
  LogSegmentFooterPB footer_builder_;

  // The maximum segment size, in bytes.
  uint64_t max_segment_size_;

  // The maximum segment size we want for the current WAL segment, in bytes.  This value keeps
  // doubling (for each subsequent WAL segment) till it gets to max_segment_size_.
  uint64_t cur_max_segment_size_;

  // Appender manages a TaskStream writing to the log. We will use one taskstream per tablet.
  std::unique_ptr<Appender> appender_;

  // A thread pool for asynchronously pre-allocating new log segments.
  std::unique_ptr<ThreadPoolToken> allocation_token_;

  // If true, sync on all appends.
  bool durable_wal_write_;

  // If non-zero, sync every interval of time.
  MonoDelta interval_durable_wal_write_;

  // If non-zero, sync if more than given amount of data to sync.
  int32_t bytes_durable_wal_write_mb_;

  // Keeps track of oldest entry which needs to be synced.
  MonoTime periodic_sync_earliest_unsync_entry_time_ = MonoTime::kMin;

  // For periodic sync, indicates if there are entries to be sync'ed.
  std::atomic<bool> periodic_sync_needed_ = {false};

  // For periodic sync, indicates number of bytes which need to be sync'ed.
  size_t periodic_sync_unsynced_bytes_ = 0;

  // If true, ignore the 'durable_wal_write_' flags above.  This is used to disable fsync during
  // bootstrap.
  bool sync_disabled_;

  // The status of the most recent log-allocation action.
  Promise<Status> allocation_status_;

  // Read-write lock to protect 'allocation_state_'.
  mutable std::mutex allocation_mutex_;
  std::condition_variable allocation_cond_;
  std::atomic<SegmentAllocationState> allocation_state_;
  bool allocation_requested_ GUARDED_BY(allocation_mutex_) = false;

  scoped_refptr<MetricEntity> metric_entity_;
  gscoped_ptr<LogMetrics> metrics_;

  // The cached on-disk size of the log, used to track its size even if it has been closed.
  std::atomic<uint64_t> on_disk_size_;

  // Listener that will be invoked after new entry was appended to the log.
  std::function<void()> post_append_listener_;

  // Used in tests delay writing log entries.
  std::atomic<std::chrono::nanoseconds> sleep_duration_{std::chrono::nanoseconds(0)};

  // Used in tests to declare all operations as safe.
  bool all_op_ids_safe_ = false;

  const std::string log_prefix_;

  std::atomic<uint32_t> wal_retention_secs_{0};

  // Minimum replicate index for the current log being written. Used for CDC read initialization.
  std::atomic<int64_t> min_replicate_index_{-1};

  // The current replicated index that CDC has read.  Used for CDC read cache optimization.
  std::atomic<int64_t> cdc_min_replicated_index_{std::numeric_limits<int64_t>::max()};

  CreateNewSegment create_new_segment_at_start_;

  DISALLOW_COPY_AND_ASSIGN(Log);
};

}  // namespace log
}  // namespace yb
#endif /* YB_CONSENSUS_LOG_H_ */
