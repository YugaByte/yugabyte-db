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

#include "yb/consensus/log_reader.h"

#include <sys/statvfs.h>

#include <algorithm>
#include <mutex>

#include "yb/consensus/consensus_util.h"
#include "yb/consensus/log_index.h"
#include "yb/consensus/opid_util.h"

#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/util.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/util/coding.h"
#include "yb/util/env_util.h"
#include "yb/util/flag_tags.h"
#include "yb/util/hexdump.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/path_util.h"
#include "yb/util/pb_util.h"

DEFINE_bool(enable_log_retention_by_op_idx, false,
            "If true, logs will be retained based on an op id passed by the cdc service");

DEFINE_int32(log_max_seconds_to_retain, 24 * 3600, "Log files that are older will be "
             "deleted even if they contain cdc unreplicated entries. If 0, this flag will be "
             "ignored. This flag is ignored if a log segment contains entries that haven't been"
             "flushed to RocksDB.");

DEFINE_int64(log_stop_retaining_min_disk_mb, 100 * 1024, "Stop retaining logs if the space "
             "available for the logs falls below this limit. This flag is ignored if a log segment "
             "contains unflushed entries.");

METRIC_DEFINE_counter(tablet, log_reader_bytes_read, "Bytes Read From Log",
                      yb::MetricUnit::kBytes,
                      "Data read from the WAL since tablet start");

METRIC_DEFINE_counter(tablet, log_reader_entries_read, "Entries Read From Log",
                      yb::MetricUnit::kEntries,
                      "Number of entries read from the WAL since tablet start");

METRIC_DEFINE_histogram(tablet, log_reader_read_batch_latency, "Log Read Latency",
                        yb::MetricUnit::kBytes,
                        "Microseconds spent reading log entry batches",
                        60000000LU, 2);

DEFINE_test_flag(bool, record_segments_violate_max_time_policy, false,
    "If set, everytime GetSegmentPrefixNotIncluding runs, segments that violate the max time "
    "policy will be appended to LogReader::segments_violate_max_time_policy_.");

DEFINE_test_flag(bool, record_segments_violate_min_space_policy, false,
    "If set, everytime GetSegmentPrefixNotIncluding runs, segments that violate the max time "
    "policy will be appended to LogReader::segments_violate_min_space_policy_.");

namespace yb {
namespace log {

namespace {
struct LogSegmentSeqnoComparator {
  bool operator() (const scoped_refptr<ReadableLogSegment>& a,
                   const scoped_refptr<ReadableLogSegment>& b) {
    return a->header().sequence_number() < b->header().sequence_number();
  }
};
}

using consensus::ReplicateMsg;
using env_util::ReadFully;
using strings::Substitute;

const int64_t LogReader::kNoSizeLimit = -1;

Status LogReader::Open(Env *env,
                       const scoped_refptr<LogIndex>& index,
                       const std::string& tablet_id,
                       const std::string& tablet_wal_path,
                       const std::string& peer_uuid,
                       const scoped_refptr<MetricEntity>& metric_entity,
                       std::unique_ptr<LogReader> *reader) {
  std::unique_ptr<LogReader> log_reader(new LogReader(
      env, index, tablet_id, peer_uuid, metric_entity));

  RETURN_NOT_OK(log_reader->Init(tablet_wal_path));
  *reader = std::move(log_reader);
  return Status::OK();
}

LogReader::LogReader(Env* env,
                     const scoped_refptr<LogIndex>& index,
                     string tablet_id,
                     string peer_uuid,
                     const scoped_refptr<MetricEntity>& metric_entity)
    : env_(env),
      log_index_(index),
      tablet_id_(std::move(tablet_id)),
      log_prefix_(consensus::MakeTabletLogPrefix(tablet_id_, peer_uuid)),
      state_(kLogReaderInitialized) {
  if (metric_entity) {
    bytes_read_ = METRIC_log_reader_bytes_read.Instantiate(metric_entity);
    entries_read_ = METRIC_log_reader_entries_read.Instantiate(metric_entity);
    read_batch_latency_ = METRIC_log_reader_read_batch_latency.Instantiate(metric_entity);
  }
  if (PREDICT_FALSE(FLAGS_enable_log_retention_by_op_idx &&
                        (FLAGS_TEST_record_segments_violate_max_time_policy ||
                         FLAGS_TEST_record_segments_violate_min_space_policy))) {
    segments_violate_max_time_policy_ = std::make_unique<SegmentSequence>();
    segments_violate_min_space_policy_ = std::make_unique<SegmentSequence>();
  }
}

LogReader::~LogReader() {
}

Status LogReader::Init(const string& tablet_wal_path) {
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(state_, kLogReaderInitialized) << "bad state for Init(): " << state_;
  }
  VLOG_WITH_PREFIX(1) << "Reading wal from path:" << tablet_wal_path;

  if (!env_->FileExists(tablet_wal_path)) {
    return STATUS(IllegalState, "Cannot find wal location at", tablet_wal_path);
  }

  VLOG_WITH_PREFIX(1) << "Parsing segments from path: " << tablet_wal_path;

  std::vector<string> files_from_log_directory;
  RETURN_NOT_OK_PREPEND(env_->GetChildren(tablet_wal_path, &files_from_log_directory),
                        "Unable to read children from path");

  SegmentSequence read_segments;

  // Build a log segment from log files, ignoring non log files.
  for (const string &potential_log_file : files_from_log_directory) {
    if (!IsLogFileName(potential_log_file)) {
      continue;
    }

    string fqp = JoinPathSegments(tablet_wal_path, potential_log_file);
    scoped_refptr<ReadableLogSegment> segment;
    RETURN_NOT_OK_PREPEND(ReadableLogSegment::Open(env_, fqp, &segment),
                          Format("Unable to open readable log segment: $0", fqp));
    DCHECK(segment);
    CHECK(segment->IsInitialized()) << "Uninitialized segment at: " << segment->path();

    if (!segment->HasFooter()) {
      LOG_WITH_PREFIX(WARNING)
          << "Log segment " << fqp << " was likely left in-progress "
             "after a previous crash. Will try to rebuild footer by scanning data.";
      RETURN_NOT_OK(segment->RebuildFooterByScanning());
    }

    read_segments.push_back(segment);
  }

  // Sort the segments by sequence number.
  std::sort(read_segments.begin(), read_segments.end(), LogSegmentSeqnoComparator());

  {
    std::lock_guard<simple_spinlock> lock(lock_);

    string previous_seg_path;
    int64_t previous_seg_seqno = -1;
    for (const SegmentSequence::value_type& entry : read_segments) {
      VLOG_WITH_PREFIX(1) << " Log Reader Indexed: " << entry->footer().ShortDebugString();
      // Check that the log segments are in sequence.
      if (previous_seg_seqno != -1 && entry->header().sequence_number() != previous_seg_seqno + 1) {
        return STATUS(Corruption, Substitute("Segment sequence numbers are not consecutive. "
            "Previous segment: seqno $0, path $1; Current segment: seqno $2, path $3",
            previous_seg_seqno, previous_seg_path,
            entry->header().sequence_number(), entry->path()));
        previous_seg_seqno++;
      } else {
        previous_seg_seqno = entry->header().sequence_number();
      }
      previous_seg_path = entry->path();
      RETURN_NOT_OK(AppendSegmentUnlocked(entry));
    }

    state_ = kLogReaderReading;
  }
  return Status::OK();
}

Status LogReader::InitEmptyReaderForTests() {
  std::lock_guard<simple_spinlock> lock(lock_);
  state_ = kLogReaderReading;
  return Status::OK();
}

bool LogReader::ViolatesMaxTimePolicy(const scoped_refptr<ReadableLogSegment>& segment) const {
  if (FLAGS_log_max_seconds_to_retain <= 0) {
    return false;
  }

  if (!segment->HasFooter()) {
    return false;
  }

  int64_t now = GetCurrentTimeMicros();
  int64_t age_seconds = (now - segment->footer().close_timestamp_micros()) / 1000000;
  if (age_seconds > FLAGS_log_max_seconds_to_retain) {
    LOG(WARNING) << "Segment " << segment->path() << " violates max retention time policy. "
                 << "Segment age: " << age_seconds << " seconds. "
                 << "log_max_seconds_to_retain: " << FLAGS_log_max_seconds_to_retain;
    if (PREDICT_FALSE(FLAGS_TEST_record_segments_violate_max_time_policy)) {
      segments_violate_max_time_policy_->push_back(segment);
    }
    return true;
  }
  return false;
}

bool LogReader::ViolatesMinSpacePolicy(const scoped_refptr<ReadableLogSegment>& segment,
                                       int64_t *potential_reclaimed_space) const {
  if (FLAGS_log_stop_retaining_min_disk_mb <= 0) {
    return false;
  }
  auto free_space_result = env_->GetFreeSpaceBytes(segment->path());
  if (!free_space_result.ok()) {
    LOG(WARNING) << "Unable to get free space: " << free_space_result;
    return false;
  } else {
    uint64_t free_space = *free_space_result;
    if ((free_space + *potential_reclaimed_space) / 1024 < FLAGS_log_stop_retaining_min_disk_mb) {
      LOG(WARNING) << "Segment " << segment->path() << " violates minimum free space policy "
                   << "specified by log_stop_retaining_min_disk_mb: "
                   << FLAGS_log_stop_retaining_min_disk_mb;
      *potential_reclaimed_space += segment->file_size();
      if (PREDICT_FALSE(FLAGS_TEST_record_segments_violate_min_space_policy)) {
        segments_violate_min_space_policy_->push_back(segment);
      }
      return true;
    }
  }
  return false;
}

Status LogReader::GetSegmentPrefixNotIncluding(int64_t index, SegmentSequence* segments) const {
  return GetSegmentPrefixNotIncluding(index, index, segments);
}

Status LogReader::GetSegmentPrefixNotIncluding(int64_t index, int64_t cdc_max_replicated_index,
                                               SegmentSequence* segments) const {
  DCHECK_GE(index, 0);
  DCHECK(segments);
  segments->clear();

  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);

  int64_t reclaimed_space = 0;
  for (const scoped_refptr<ReadableLogSegment>& segment : segments_) {
    // The last segment doesn't have a footer. Never include that one.
    if (!segment->HasFooter()) {
      break;
    }

    // Never garbage collect log segments with unflushed entries.
    if (segment->footer().max_replicate_index() >= index) {
      break;
    }

    // This log segment contains cdc unreplicated entries. Don't GC it unless the file is too old
    // (controlled by flag FLAGS_log_max_seconds_to_retain) or we don't have enough space for the
    // logs (controlled by flag FLAGS_log_stop_retaining_min_disk_mb).
    if (FLAGS_enable_log_retention_by_op_idx &&
        segment->footer().max_replicate_index() >= cdc_max_replicated_index) {

      // Since this log file contains cdc unreplicated entries, we don't want to GC it unless
      // it's too old, or we don't have enough space to store log files.

      if (!ViolatesMaxTimePolicy(segment) && !ViolatesMinSpacePolicy(segment, &reclaimed_space)) {
        // We exit the loop since this log segment already contains cdc unreplicated entries and so
        // do all subsequent files.
        break;
      }
    }

    // TODO: tests for edge cases here with backwards ordered replicates.
    segments->push_back(segment);
  }

  return Status::OK();
}

int64_t LogReader::GetMinReplicateIndex() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  int64_t min_remaining_op_idx = -1;

  for (const scoped_refptr<ReadableLogSegment>& segment : segments_) {
    if (!segment->HasFooter()) continue;
    if (!segment->footer().has_min_replicate_index()) continue;
    if (min_remaining_op_idx == -1 ||
        segment->footer().min_replicate_index() < min_remaining_op_idx) {
      min_remaining_op_idx = segment->footer().min_replicate_index();
    }
  }
  return min_remaining_op_idx;
}

void LogReader::GetMaxIndexesToSegmentSizeMap(int64_t min_op_idx, int32_t segments_count,
                                              int64_t max_close_time_us,
                                              std::map<int64_t, int64_t>*
                                              max_idx_to_segment_size) const {
  std::lock_guard<simple_spinlock> lock(lock_);
  DCHECK_GE(segments_count, 0);
  for (const scoped_refptr<ReadableLogSegment>& segment : segments_) {
    if (max_idx_to_segment_size->size() == segments_count) {
      break;
    }
    DCHECK(segment->HasFooter());
    if (segment->footer().max_replicate_index() < min_op_idx) {
      // This means we found a log we can GC. Adjust the expected number of logs.
      segments_count--;
      continue;
    }

    if (max_close_time_us < segment->footer().close_timestamp_micros()) {
      int64_t age_seconds = segment->footer().close_timestamp_micros() / 1000000;
      VLOG_WITH_PREFIX(2)
          << "Segment " << segment->path() << " is only " << age_seconds << "s old: "
          << "won't be counted towards log retention";
      break;
    }
    (*max_idx_to_segment_size)[segment->footer().max_replicate_index()] = segment->file_size();
  }
}

scoped_refptr<ReadableLogSegment> LogReader::GetSegmentBySequenceNumber(int64_t seq) const {
  std::lock_guard<simple_spinlock> lock(lock_);
  if (segments_.empty()) {
    return nullptr;
  }

  // We always have a contiguous set of log segments, so we can find the requested
  // segment in our vector by calculating its offset vs the first element.
  int64_t first_seqno = segments_[0]->header().sequence_number();
  int64_t relative = seq - first_seqno;
  if (relative < 0 || relative >= segments_.size()) {
    return nullptr;
  }

  DCHECK_EQ(segments_[relative]->header().sequence_number(), seq);
  return segments_[relative];
}

Status LogReader::ReadBatchUsingIndexEntry(const LogIndexEntry& index_entry,
                                           faststring* tmp_buf,
                                           LogEntryBatchPB* batch) const {
  const int64_t index = index_entry.op_id.index;

  scoped_refptr<ReadableLogSegment> segment = GetSegmentBySequenceNumber(
    index_entry.segment_sequence_number);
  if (PREDICT_FALSE(!segment)) {
    return STATUS(NotFound, Substitute("Segment $0 which contained index $1 has been GCed",
                                       index_entry.segment_sequence_number,
                                       index));
  }

  CHECK_GT(index_entry.offset_in_segment, 0);
  int64_t offset = index_entry.offset_in_segment;
  ScopedLatencyMetric scoped(read_batch_latency_.get());
  RETURN_NOT_OK_PREPEND(segment->ReadEntryHeaderAndBatch(&offset, tmp_buf, batch),
                        Substitute("Failed to read LogEntry for index $0 from log segment "
                                   "$1 offset $2",
                                   index,
                                   index_entry.segment_sequence_number,
                                   index_entry.offset_in_segment));

  if (bytes_read_) {
    bytes_read_->IncrementBy(kEntryHeaderSize + tmp_buf->length());
    entries_read_->IncrementBy(batch->entry_size());
  }

  return Status::OK();
}

Status LogReader::ReadReplicatesInRange(
    const int64_t starting_at,
    const int64_t up_to,
    int64_t max_bytes_to_read,
    ReplicateMsgs* replicates) const {
  DCHECK_GT(starting_at, 0);
  DCHECK_GE(up_to, starting_at);
  DCHECK(log_index_) << "Require an index to random-read logs";

  ReplicateMsgs replicates_tmp;
  LogIndexEntry prev_index_entry;
  prev_index_entry.segment_sequence_number = -1;
  prev_index_entry.offset_in_segment = -1;

  int64_t total_size = 0;
  bool limit_exceeded = false;
  faststring tmp_buf;
  LogEntryBatchPB batch;
  for (int64_t index = starting_at; index <= up_to && !limit_exceeded; index++) {
    LogIndexEntry index_entry;
    RETURN_NOT_OK_PREPEND(log_index_->GetEntry(index, &index_entry),
                          Substitute("Failed to read log index for op $0", index));

    // Since a given LogEntryBatch may contain multiple REPLICATE messages,
    // it's likely that this index entry points to the same batch as the previous
    // one. If that's the case, we've already read this REPLICATE and we can
    // skip reading the batch again.
    if (index == starting_at ||
        index_entry.segment_sequence_number != prev_index_entry.segment_sequence_number ||
        index_entry.offset_in_segment != prev_index_entry.offset_in_segment) {
      RETURN_NOT_OK(ReadBatchUsingIndexEntry(index_entry, &tmp_buf, &batch));

      // Sanity-check the property that a batch should only have increasing indexes.
      int64_t prev_index = 0;
      for (int i = 0; i < batch.entry_size(); ++i) {
        LogEntryPB* entry = batch.mutable_entry(i);
        if (!entry->has_replicate()) continue;
        int64_t this_index = entry->replicate().id().index();
        CHECK_GT(this_index, prev_index)
          << "Expected that an entry batch should only include increasing log indexes: "
          << index_entry.ToString()
          << "\nBatch: " << batch.DebugString();
        prev_index = this_index;
      }
    }

    bool found = false;
    for (int i = 0; i < batch.entry_size(); ++i) {
      LogEntryPB* entry = batch.mutable_entry(i);
      if (!entry->has_replicate()) {
        continue;
      }

      if (entry->replicate().id().index() != index) {
        continue;
      }

      int64_t space_required = entry->replicate().SpaceUsed();
      if (replicates_tmp.empty() ||
          max_bytes_to_read <= 0 ||
          total_size + space_required < max_bytes_to_read) {
        total_size += space_required;
        replicates_tmp.emplace_back(entry->release_replicate());
      } else {
        limit_exceeded = true;
      }
      found = true;
      break;
    }
    CHECK(found) << "Incorrect index entry didn't yield expected log entry: "
                 << index_entry.ToString();

    prev_index_entry = index_entry;
  }

  replicates->swap(replicates_tmp);
  return Status::OK();
}

Result<yb::OpId> LogReader::LookupOpId(int64_t op_index) const {
  LogIndexEntry index_entry;
  RETURN_NOT_OK_PREPEND(log_index_->GetEntry(op_index, &index_entry),
                        strings::Substitute("Failed to read log index for op $0", op_index));
  return index_entry.op_id;
}

Status LogReader::GetSegmentsSnapshot(SegmentSequence* segments) const {
  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  segments->assign(segments_.begin(), segments_.end());
  return Status::OK();
}

Status LogReader::TrimSegmentsUpToAndIncluding(int64_t segment_sequence_number) {
  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  auto iter = segments_.begin();
  std::vector<int64_t> deleted_segments;

  while (iter != segments_.end()) {
    auto current_seq_no = (*iter)->header().sequence_number();
    if (current_seq_no > segment_sequence_number) {
      break;
    }
    deleted_segments.push_back(current_seq_no);
    iter = segments_.erase(iter);
  }
  LOG_WITH_PREFIX(INFO) << "Removed log segment sequence numbers from log reader: "
                        << yb::ToString(deleted_segments);
  return Status::OK();
}

void LogReader::UpdateLastSegmentOffset(int64_t readable_to_offset) {
  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  DCHECK(!segments_.empty());
  // Get the last segment
  ReadableLogSegment* segment = segments_.back().get();
  DCHECK(!segment->HasFooter());
  segment->UpdateReadableToOffset(readable_to_offset);
}

Status LogReader::ReplaceLastSegment(const scoped_refptr<ReadableLogSegment>& segment) {
  // This is used to replace the last segment once we close it properly so it must
  // have a footer.
  DCHECK(segment->HasFooter());

  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  // Make sure the segment we're replacing has the same sequence number
  CHECK(!segments_.empty());
  CHECK_EQ(segment->header().sequence_number(), segments_.back()->header().sequence_number());
  segments_[segments_.size() - 1] = segment;

  return Status::OK();
}

Status LogReader::AppendSegment(const scoped_refptr<ReadableLogSegment>& segment) {
  DCHECK(segment->IsInitialized());
  if (PREDICT_FALSE(!segment->HasFooter())) {
    RETURN_NOT_OK(segment->RebuildFooterByScanning());
  }
  std::lock_guard<simple_spinlock> lock(lock_);
  return AppendSegmentUnlocked(segment);
}

Status LogReader::AppendSegmentUnlocked(const scoped_refptr<ReadableLogSegment>& segment) {
  DCHECK(segment->IsInitialized());
  DCHECK(segment->HasFooter());

  if (!segments_.empty()) {
    CHECK_EQ(segments_.back()->header().sequence_number() + 1,
             segment->header().sequence_number());
  }
  segments_.push_back(segment);
  return Status::OK();
}

Status LogReader::AppendEmptySegment(const scoped_refptr<ReadableLogSegment>& segment) {
  DCHECK(segment->IsInitialized());
  std::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, kLogReaderReading);
  if (!segments_.empty()) {
    CHECK_EQ(segments_.back()->header().sequence_number() + 1,
             segment->header().sequence_number());
  }
  segments_.push_back(segment);
  return Status::OK();
}

const int LogReader::num_segments() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return segments_.size();
}

string LogReader::ToString() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  string ret = "Reader's SegmentSequence: \n";
  for (const SegmentSequence::value_type& entry : segments_) {
    ret.append(Substitute("Segment: $0 Footer: $1\n",
                          entry->header().sequence_number(),
                          !entry->HasFooter() ? "NONE" : entry->footer().ShortDebugString()));
  }
  return ret;
}

}  // namespace log
}  // namespace yb
