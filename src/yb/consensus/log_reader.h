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
#ifndef YB_CONSENSUS_LOG_READER_H
#define YB_CONSENSUS_LOG_READER_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "yb/consensus/log_metrics.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/spinlock.h"
#include "yb/util/locks.h"

namespace yb {

namespace cdc {
class CDCServiceTestMaxRentionTime_TestLogRetentionByOpId_MaxRentionTime_Test;
class CDCServiceTestMinSpace_TestLogRetentionByOpId_MinSpace_Test;
}

namespace log {
class Log;
class LogIndex;
struct LogIndexEntry;

// Reads a set of segments from a given path. Segment headers and footers
// are read and parsed, but entries are not.
// This class is thread safe.
class LogReader {
 public:
  ~LogReader();

  // Opens a LogReader on a specific log directory, and sets 'reader' to the newly created
  // LogReader.
  //
  // 'index' may be NULL, but if it is, ReadReplicatesInRange() may not be used.
  static CHECKED_STATUS Open(Env *env,
                             const scoped_refptr<LogIndex>& index,
                             const std::string& tablet_id,
                             const std::string& tablet_wal_path,
                             const std::string& peer_uuid,
                             const scoped_refptr<MetricEntity>& metric_entity,
                             std::unique_ptr<LogReader> *reader);

  // Returns the biggest prefix of segments, from the current sequence, guaranteed
  // not to include any replicate messages with indexes >= 'index'.
  CHECKED_STATUS GetSegmentPrefixNotIncluding(int64_t index, SegmentSequence* segments) const;

  CHECKED_STATUS GetSegmentPrefixNotIncluding(int64_t index, int64_t cdc_replicated_index,
                                              SegmentSequence* segments) const;

  // Return the minimum replicate index that is retained in the currently available
  // logs. May return -1 if no replicates have been logged.
  int64_t GetMinReplicateIndex() const;

  // Returns a map of maximum log index in segment -> segment size representing all the segments
  // that start after 'min_op_idx', up to 'segments_count'.
  //
  // 'min_op_idx' is the minimum operation index to start looking from, we don't record
  // the segments before the one that contain that id.
  //
  // 'segments_count' is the number of segments we'll add to the map. It _must_ be sized so that
  // we don't add the last segment. If we find logs that can be GCed, we'll decrease the number of
  // elements we'll add to the map by 1 since they.
  //
  // 'max_close_time_us' is the timestamp in microseconds from which we don't want to evict,
  // meaning that log segments that we closed after that time must not be added to the map.
  void GetMaxIndexesToSegmentSizeMap(int64_t min_op_idx, int32_t segments_count,
                                     int64_t max_close_time_us,
                                     std::map<int64_t, int64_t>* max_idx_to_segment_size) const;

  // Return a readable segment with the given sequence number, or NULL if it
  // cannot be found (e.g. if it has already been GCed).
  scoped_refptr<ReadableLogSegment> GetSegmentBySequenceNumber(int64_t seq) const;

  // Copies a snapshot of the current sequence of segments into 'segments'.
  // 'segments' will be cleared first.
  CHECKED_STATUS GetSegmentsSnapshot(SegmentSequence* segments) const;

  // Reads all ReplicateMsgs from 'starting_at' to 'up_to' both inclusive.
  // The caller takes ownership of the returned ReplicateMsg objects.
  //
  // Will attempt to read no more than 'max_bytes_to_read', unless it is set to
  // LogReader::kNoSizeLimit. If the size limit would prevent reading any operations at
  // all, then will read exactly one operation.
  //
  // Requires that a LogIndex was passed into LogReader::Open().
  CHECKED_STATUS ReadReplicatesInRange(
      const int64_t starting_at,
      const int64_t up_to,
      int64_t max_bytes_to_read,
      ReplicateMsgs* replicates) const;
  static const int64_t kNoSizeLimit;

  // Look up the OpId for the given operation index.
  // Returns a bad Status if the log index fails to load (eg. due to an IO error).
  Result<yb::OpId> LookupOpId(int64_t op_index) const;

  // Returns the number of segments.
  const int num_segments() const;

  std::string ToString() const;

  const std::string& LogPrefix() const {
    return log_prefix_;
  }

 private:
  FRIEND_TEST(cdc::CDCServiceTestMaxRentionTime, TestLogRetentionByOpId_MaxRentionTime);
  FRIEND_TEST(cdc::CDCServiceTestMinSpace, TestLogRetentionByOpId_MinSpace);
  FRIEND_TEST(LogTest, TestLogReader);
  FRIEND_TEST(LogTest, TestReadLogWithReplacedReplicates);
  friend class Log;
  friend class LogTest;

  enum State {
    kLogReaderInitialized,
    kLogReaderReading,
    kLogReaderClosed
  };

  // Appends 'segment' to the segments available for read by this reader.
  // Index entries in 'segment's footer will be added to the index.
  // If the segment has no footer it will be scanned so this should not be used
  // for new segments.
  CHECKED_STATUS AppendSegment(const scoped_refptr<ReadableLogSegment>& segment);

  // Same as above but for segments without any entries.
  // Used by the Log to add "empty" segments.
  CHECKED_STATUS AppendEmptySegment(const scoped_refptr<ReadableLogSegment>& segment);

  // Removes segments with sequence numbers less than or equal to 'seg_seqno' from this reader.
  CHECKED_STATUS TrimSegmentsUpToAndIncluding(int64_t seg_seqno);

  // Replaces the last segment in the reader with 'segment'.
  // Used to replace a segment that was still in the process of being written
  // with its complete version which has a footer and index entries.
  // Requires that the last segment in 'segments_' has the same sequence
  // number as 'segment'.
  // Expects 'segment' to be properly closed and to have footer.
  CHECKED_STATUS ReplaceLastSegment(const scoped_refptr<ReadableLogSegment>& segment);

  // Appends 'segment' to the segment sequence.
  // Assumes that the segment was scanned, if no footer was found.
  // To be used only internally, clients of this class with private access (i.e. friends)
  // should use the thread safe version, AppendSegment(), which will also scan the segment
  // if no footer is present.
  CHECKED_STATUS AppendSegmentUnlocked(const scoped_refptr<ReadableLogSegment>& segment);

  // Used by Log to update its LogReader on how far it is possible to read
  // the current segment. Requires that the reader has at least one segment
  // and that the last segment has no footer, meaning it is currently being
  // written to.
  void UpdateLastSegmentOffset(int64_t readable_to_offset);

  // Read the LogEntryBatch pointed to by the provided index entry.
  // 'tmp_buf' is used as scratch space to avoid extra allocation.
  CHECKED_STATUS ReadBatchUsingIndexEntry(const LogIndexEntry& index_entry,
                                          faststring* tmp_buf,
                                          LogEntryBatchPB* batch) const;

  LogReader(Env* env, const scoped_refptr<LogIndex>& index,
            std::string tablet_name, std::string peer_uuid,
            const scoped_refptr<MetricEntity>& metric_entity);

  // Reads the headers of all segments in 'path_'.
  CHECKED_STATUS Init(const std::string& path_);

  // Initializes an 'empty' reader for tests, i.e. does not scan a path looking for segments.
  CHECKED_STATUS InitEmptyReaderForTests();

  // Determines if a file is older than the time specified by FLAGS_log_max_seconds_to_retain.
  bool ViolatesMaxTimePolicy(const scoped_refptr<ReadableLogSegment>& segment) const;

  // Return true if by keeping this log segment, we would violate the required minimum free space.
  // potential_reclaimed_space is used for the calculation of free space. If NotEnoughSpace returns
  // true, it will add the size of segment to potential_reclaimed_space.
  bool ViolatesMinSpacePolicy(const scoped_refptr<ReadableLogSegment>& segment,
                              int64_t *potential_reclaimed_space) const;

  Env *env_;

  const scoped_refptr<LogIndex> log_index_;
  const std::string tablet_id_;
  const std::string log_prefix_;

  // Metrics
  scoped_refptr<Counter> bytes_read_;
  scoped_refptr<Counter> entries_read_;
  scoped_refptr<Histogram> read_batch_latency_;

  // The sequence of all current log segments in increasing sequence number
  // order.
  SegmentSequence segments_;

  mutable simple_spinlock lock_;

  State state_;

  // Used for test only.
  mutable std::unique_ptr<SegmentSequence> segments_violate_max_time_policy_;
  mutable std::unique_ptr<SegmentSequence> segments_violate_min_space_policy_;

  DISALLOW_COPY_AND_ASSIGN(LogReader);
};

}  // namespace log
}  // namespace yb

#endif // YB_CONSENSUS_LOG_READER_H
