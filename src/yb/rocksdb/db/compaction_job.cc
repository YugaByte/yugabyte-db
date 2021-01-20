//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
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
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "yb/rocksdb/db/compaction_job.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <functional>
#include <vector>
#include <memory>
#include <list>
#include <set>
#include <thread>
#include <utility>

#include "yb/rocksdb/db/builder.h"
#include "yb/rocksdb/db/db_iter.h"
#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/db/event_helpers.h"
#include "yb/rocksdb/db/filename.h"
#include "yb/rocksdb/db/file_numbers.h"
#include "yb/rocksdb/db/log_reader.h"
#include "yb/rocksdb/db/log_writer.h"
#include "yb/rocksdb/db/memtable.h"
#include "yb/rocksdb/db/memtable_list.h"
#include "yb/rocksdb/db/merge_context.h"
#include "yb/rocksdb/db/merge_helper.h"
#include "yb/rocksdb/db/version_set.h"
#include "yb/rocksdb/port/likely.h"
#include "yb/rocksdb/port/port.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/statistics.h"
#include "yb/rocksdb/status.h"
#include "yb/rocksdb/table.h"
#include "yb/rocksdb/table/block.h"
#include "yb/rocksdb/table/block_based_table_factory.h"
#include "yb/rocksdb/table/merger.h"
#include "yb/rocksdb/table/table_builder.h"
#include "yb/rocksdb/util/coding.h"
#include "yb/rocksdb/util/file_reader_writer.h"
#include "yb/rocksdb/util/log_buffer.h"
#include "yb/rocksdb/util/logging.h"
#include "yb/rocksdb/util/sst_file_manager_impl.h"
#include "yb/rocksdb/util/mutexlock.h"
#include "yb/rocksdb/util/perf_context_imp.h"
#include "yb/rocksdb/util/stop_watch.h"
#include "yb/rocksdb/util/sync_point.h"
#include "yb/rocksdb/util/thread_status_util.h"

#include "yb/util/stats/iostats_context_imp.h"
#include "yb/util/string_util.h"

namespace rocksdb {

// Maintains state for each sub-compaction
struct CompactionJob::SubcompactionState {
  Compaction* compaction;
  std::unique_ptr<CompactionIterator> c_iter;

  // The boundaries of the key-range this compaction is interested in. No two
  // subcompactions may have overlapping key-ranges.
  // 'start' is inclusive, 'end' is exclusive, and nullptr means unbounded
  Slice *start, *end;

  // The return status of this subcompaction
  Status status;

  // Files produced by this subcompaction
  struct Output {
    FileMetaData meta;
    bool finished;
    std::shared_ptr<const TableProperties> table_properties;
  };

  // State kept for output being generated
  std::vector<Output> outputs;
  std::unique_ptr<WritableFileWriter> base_outfile;
  std::unique_ptr<WritableFileWriter> data_outfile;
  std::unique_ptr<TableBuilder> builder;
  Output* current_output() {
    if (outputs.empty()) {
      // This subcompaction's outptut could be empty if compaction was aborted
      // before this subcompaction had a chance to generate any output files.
      // When subcompactions are executed sequentially this is more likely and
      // will be particulalry likely for the later subcompactions to be empty.
      // Once they are run in parallel however it should be much rarer.
      return nullptr;
    } else {
      return &outputs.back();
    }
  }

  // State during the subcompaction
  uint64_t total_bytes;
  uint64_t num_input_records;
  uint64_t num_output_records;
  CompactionJobStats compaction_job_stats;
  uint64_t approx_size;

  SubcompactionState(Compaction* c, Slice* _start, Slice* _end,
                     uint64_t size = 0)
      : compaction(c),
        start(_start),
        end(_end),
        base_outfile(nullptr),
        data_outfile(nullptr),
        builder(nullptr),
        total_bytes(0),
        num_input_records(0),
        num_output_records(0),
        approx_size(size) {
    assert(compaction != nullptr);
  }

  SubcompactionState(SubcompactionState&& o) { *this = std::move(o); }

  SubcompactionState& operator=(SubcompactionState&& o) {
    compaction = std::move(o.compaction);
    start = std::move(o.start);
    end = std::move(o.end);
    status = std::move(o.status);
    outputs = std::move(o.outputs);
    base_outfile = std::move(o.base_outfile);
    data_outfile = std::move(o.data_outfile);
    builder = std::move(o.builder);
    total_bytes = std::move(o.total_bytes);
    num_input_records = std::move(o.num_input_records);
    num_output_records = std::move(o.num_output_records);
    compaction_job_stats = std::move(o.compaction_job_stats);
    approx_size = std::move(o.approx_size);
    return *this;
  }

  // Because member unique_ptrs do not have these.
  SubcompactionState(const SubcompactionState&) = delete;

  SubcompactionState& operator=(const SubcompactionState&) = delete;
};

// Maintains state for the entire compaction
struct CompactionJob::CompactionState {
  Compaction* const compaction;

  // REQUIRED: subcompaction states are stored in order of increasing
  // key-range
  std::vector<CompactionJob::SubcompactionState> sub_compact_states;
  Status status;

  uint64_t total_bytes;
  uint64_t num_input_records;
  uint64_t num_output_records;

  explicit CompactionState(Compaction* c)
      : compaction(c),
        total_bytes(0),
        num_input_records(0),
        num_output_records(0) {}

  size_t NumOutputFiles() {
    size_t total = 0;
    for (auto& s : sub_compact_states) {
      total += s.outputs.size();
    }
    return total;
  }

  Slice SmallestUserKey() {
    for (const auto& sub_compact_state : sub_compact_states) {
      if (!sub_compact_state.outputs.empty() &&
          sub_compact_state.outputs[0].finished) {
        return sub_compact_state.outputs[0].meta.smallest.key.user_key();
      }
    }
    // If there is no finished output, return an empty slice.
    return Slice();
  }

  Slice LargestUserKey() {
    for (auto it = sub_compact_states.rbegin(); it < sub_compact_states.rend();
         ++it) {
      if (!it->outputs.empty() && it->current_output()->finished) {
        assert(it->current_output() != nullptr);
        return it->current_output()->meta.largest.key.user_key();
      }
    }
    // If there is no finished output, return an empty slice.
    return Slice();
  }
};

void CompactionJob::AggregateStatistics() {
  for (SubcompactionState& sc : compact_->sub_compact_states) {
    compact_->total_bytes += sc.total_bytes;
    compact_->num_input_records += sc.num_input_records;
    compact_->num_output_records += sc.num_output_records;
  }
  if (compaction_job_stats_) {
    for (SubcompactionState& sc : compact_->sub_compact_states) {
      compaction_job_stats_->Add(sc.compaction_job_stats);
    }
  }
}

CompactionJob::CompactionJob(
    int job_id, Compaction* compaction, const DBOptions& db_options,
    const EnvOptions& env_options, VersionSet* versions,
    std::atomic<bool>* shutting_down, LogBuffer* log_buffer,
    Directory* db_directory, Directory* output_directory, Statistics* stats,
    InstrumentedMutex* db_mutex, Status* db_bg_error,
    std::vector<SequenceNumber> existing_snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    FileNumbersProvider* file_numbers_provider,
    std::shared_ptr<Cache> table_cache, EventLogger* event_logger,
    bool paranoid_file_checks, bool measure_io_stats, const std::string& dbname,
    CompactionJobStats* compaction_job_stats)
    : job_id_(job_id),
      compact_(new CompactionState(compaction)),
      compaction_job_stats_(compaction_job_stats),
      compaction_stats_(1),
      dbname_(dbname),
      db_options_(db_options),
      env_options_(env_options),
      env_(db_options.env),
      versions_(versions),
      shutting_down_(shutting_down),
      log_buffer_(log_buffer),
      db_directory_(db_directory),
      output_directory_(output_directory),
      stats_(stats),
      db_mutex_(db_mutex),
      db_bg_error_(db_bg_error),
      existing_snapshots_(std::move(existing_snapshots)),
      earliest_write_conflict_snapshot_(earliest_write_conflict_snapshot),
      file_numbers_provider_(file_numbers_provider),
      table_cache_(std::move(table_cache)),
      event_logger_(event_logger),
      paranoid_file_checks_(paranoid_file_checks),
      measure_io_stats_(measure_io_stats) {
  assert(log_buffer_ != nullptr);
  const auto* cfd = compact_->compaction->column_family_data();
  ThreadStatusUtil::SetColumnFamily(cfd, cfd->ioptions()->env,
                                    cfd->options()->enable_thread_tracking);
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_COMPACTION);
  ReportStartedCompaction(compaction);
}

CompactionJob::~CompactionJob() {
  assert(compact_ == nullptr);
  ThreadStatusUtil::ResetThreadStatus();
}

void CompactionJob::ReportStartedCompaction(
    Compaction* compaction) {
  const auto* cfd = compact_->compaction->column_family_data();
  ThreadStatusUtil::SetColumnFamily(cfd, cfd->ioptions()->env,
                                    cfd->options()->enable_thread_tracking);

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_JOB_ID,
      job_id_);

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_INPUT_OUTPUT_LEVEL,
      (static_cast<uint64_t>(compact_->compaction->start_level()) << 32) +
          compact_->compaction->output_level());

  // In the current design, a CompactionJob is always created
  // for non-trivial compaction.
  assert(compaction->IsTrivialMove() == false ||
         compaction->is_manual_compaction() == true);

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_PROP_FLAGS,
      compaction->is_manual_compaction() +
          (compaction->deletion_compaction() << 1));

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_TOTAL_INPUT_BYTES,
      compaction->CalculateTotalInputSize());

  IOSTATS_RESET(bytes_written);
  IOSTATS_RESET(bytes_read);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_WRITTEN, 0);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_READ, 0);

  // Set the thread operation after operation properties
  // to ensure GetThreadList() can always show them all together.
  ThreadStatusUtil::SetThreadOperation(
      ThreadStatus::OP_COMPACTION);

  if (compaction_job_stats_) {
    compaction_job_stats_->is_manual_compaction =
        compaction->is_manual_compaction();
  }
}

void CompactionJob::Prepare() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_PREPARE);

  // Generate file_levels_ for compaction berfore making Iterator
  auto* c = compact_->compaction;
  assert(c->column_family_data() != nullptr);
  assert(c->column_family_data()->current()->storage_info()
      ->NumLevelFiles(compact_->compaction->level()) > 0);

  // Is this compaction producing files at the bottommost level?
  bottommost_level_ = c->bottommost_level();

  if (c->ShouldFormSubcompactions()) {
    const uint64_t start_micros = env_->NowMicros();
    GenSubcompactionBoundaries();
    MeasureTime(stats_, SUBCOMPACTION_SETUP_TIME,
                env_->NowMicros() - start_micros);

    assert(sizes_.size() == boundaries_.size() + 1);

    for (size_t i = 0; i <= boundaries_.size(); i++) {
      Slice* start = i == 0 ? nullptr : &boundaries_[i - 1];
      Slice* end = i == boundaries_.size() ? nullptr : &boundaries_[i];
      compact_->sub_compact_states.emplace_back(c, start, end, sizes_[i]);
    }
    MeasureTime(stats_, NUM_SUBCOMPACTIONS_SCHEDULED,
                compact_->sub_compact_states.size());
  } else {
    compact_->sub_compact_states.emplace_back(c, nullptr, nullptr);
  }
}

struct RangeWithSize {
  Range range;
  uint64_t size;

  RangeWithSize(const Slice& a, const Slice& b, uint64_t s = 0)
      : range(a, b), size(s) {}
};

// Generates a histogram representing potential divisions of key ranges from
// the input. It adds the starting and/or ending keys of certain input files
// to the working set and then finds the approximate size of data in between
// each consecutive pair of slices. Then it divides these ranges into
// consecutive groups such that each group has a similar size.
void CompactionJob::GenSubcompactionBoundaries() {
  auto* c = compact_->compaction;
  auto* cfd = c->column_family_data();
  const Comparator* cfd_comparator = cfd->user_comparator();
  std::vector<Slice> bounds;
  int start_lvl = c->start_level();
  int out_lvl = c->output_level();

  // Add the starting and/or ending key of certain input files as a potential
  // boundary
  for (size_t lvl_idx = 0; lvl_idx < c->num_input_levels(); lvl_idx++) {
    int lvl = c->level(lvl_idx);
    if (lvl >= start_lvl && lvl <= out_lvl) {
      const LevelFilesBrief* flevel = c->input_levels(lvl_idx);
      size_t num_files = flevel->num_files;

      if (num_files == 0) {
        continue;
      }

      if (lvl == 0) {
        // For level 0 add the starting and ending key of each file since the
        // files may have greatly differing key ranges (not range-partitioned)
        for (size_t i = 0; i < num_files; i++) {
          bounds.emplace_back(flevel->files[i].smallest.key);
          bounds.emplace_back(flevel->files[i].largest.key);
        }
      } else {
        // For all other levels add the smallest/largest key in the level to
        // encompass the range covered by that level
        bounds.emplace_back(flevel->files[0].smallest.key);
        bounds.emplace_back(flevel->files[num_files - 1].largest.key);
        if (lvl == out_lvl) {
          // For the last level include the starting keys of all files since
          // the last level is the largest and probably has the widest key
          // range. Since it's range partitioned, the ending key of one file
          // and the starting key of the next are very close (or identical).
          for (size_t i = 1; i < num_files; i++) {
            bounds.emplace_back(flevel->files[i].smallest.key);
          }
        }
      }
    }
  }

  std::sort(bounds.begin(), bounds.end(),
    [cfd_comparator] (const Slice& a, const Slice& b) -> bool {
      return cfd_comparator->Compare(ExtractUserKey(a), ExtractUserKey(b)) < 0;
    });
  // Remove duplicated entries from bounds
  bounds.erase(std::unique(bounds.begin(), bounds.end(),
    [cfd_comparator] (const Slice& a, const Slice& b) -> bool {
      return cfd_comparator->Compare(ExtractUserKey(a), ExtractUserKey(b)) == 0;
    }), bounds.end());

  // Combine consecutive pairs of boundaries into ranges with an approximate
  // size of data covered by keys in that range
  uint64_t sum = 0;
  std::vector<RangeWithSize> ranges;
  auto* v = cfd->current();
  for (auto it = bounds.begin();;) {
    const Slice a = *it;
    it++;

    if (it == bounds.end()) {
      break;
    }

    const Slice b = *it;
    uint64_t size = versions_->ApproximateSize(v, a, b, start_lvl, out_lvl + 1);
    ranges.emplace_back(a, b, size);
    sum += size;
  }

  // Group the ranges into subcompactions
  const double min_file_fill_percent = 4.0 / 5;
  uint64_t max_output_files = static_cast<uint64_t>(std::ceil(
      sum / min_file_fill_percent /
      cfd->GetCurrentMutableCFOptions()->MaxFileSizeForLevel(out_lvl)));
  uint64_t subcompactions =
      std::min({static_cast<uint64_t>(ranges.size()),
                static_cast<uint64_t>(db_options_.max_subcompactions),
                max_output_files});

  double mean = subcompactions != 0 ? sum * 1.0 / subcompactions
                                    : std::numeric_limits<double>::max();

  if (subcompactions > 1) {
    // Greedily add ranges to the subcompaction until the sum of the ranges'
    // sizes becomes >= the expected mean size of a subcompaction
    sum = 0;
    for (size_t i = 0; i < ranges.size() - 1; i++) {
      sum += ranges[i].size;
      if (subcompactions == 1) {
        // If there's only one left to schedule then it goes to the end so no
        // need to put an end boundary
        continue;
      }
      if (sum >= mean) {
        boundaries_.emplace_back(ExtractUserKey(ranges[i].range.limit));
        sizes_.emplace_back(sum);
        subcompactions--;
        sum = 0;
      }
    }
    sizes_.emplace_back(sum + ranges.back().size);
  } else {
    // Only one range so its size is the total sum of sizes computed above
    sizes_.emplace_back(sum);
  }
}

Result<FileNumbersHolder> CompactionJob::Run() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_RUN);
  TEST_SYNC_POINT("CompactionJob::Run():Start");
  log_buffer_->FlushBufferToLog();
  LogCompaction();
  for (auto listener : db_options_.listeners) {
    listener->OnCompactionStarted();
  }

  const size_t num_threads = compact_->sub_compact_states.size();
  assert(num_threads > 0);
  const uint64_t start_micros = env_->NowMicros();

  // Launch a thread for each of subcompactions 1...num_threads-1
  std::vector<std::thread> thread_pool;
  thread_pool.reserve(num_threads - 1);
  FileNumbersHolder file_numbers_holder(file_numbers_provider_->CreateHolder());
  file_numbers_holder.Reserve(num_threads);
  for (size_t i = 1; i < compact_->sub_compact_states.size(); i++) {
    thread_pool.emplace_back(&CompactionJob::ProcessKeyValueCompaction, this, &file_numbers_holder,
                             &compact_->sub_compact_states[i]);
  }

  // Always schedule the first subcompaction (whether or not there are also
  // others) in the current thread to be efficient with resources
  ProcessKeyValueCompaction(&file_numbers_holder, &compact_->sub_compact_states[0]);

  // Wait for all other threads (if there are any) to finish execution
  for (auto& thread : thread_pool) {
    thread.join();
  }

  if (output_directory_ && !db_options_.disableDataSync) {
    RETURN_NOT_OK(output_directory_->Fsync());
  }

  compaction_stats_.micros = env_->NowMicros() - start_micros;
  MeasureTime(stats_, COMPACTION_TIME, compaction_stats_.micros);

  // Check if any thread encountered an error during execution
  Status status;
  for (const auto& state : compact_->sub_compact_states) {
    if (!state.status.ok()) {
      status = state.status;
      break;
    }
  }

  TablePropertiesCollection tp;
  for (const auto& state : compact_->sub_compact_states) {
    for (const auto& output : state.outputs) {
      auto fn = TableFileName(db_options_.db_paths, output.meta.fd.GetNumber(),
                              output.meta.fd.GetPathId());
      tp[fn] = output.table_properties;
    }
  }
  compact_->compaction->SetOutputTableProperties(std::move(tp));

  // Finish up all book-keeping to unify the subcompaction results
  AggregateStatistics();
  UpdateCompactionStats();
  RecordCompactionIOStats();
  LogFlush(db_options_.info_log);
  TEST_SYNC_POINT("CompactionJob::Run():End");

  compact_->status = status;
  return file_numbers_holder;
}

Status CompactionJob::Install(const MutableCFOptions& mutable_cf_options) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_INSTALL);
  db_mutex_->AssertHeld();
  Status status = compact_->status;
  ColumnFamilyData* cfd = compact_->compaction->column_family_data();
  cfd->internal_stats()->AddCompactionStats(
      compact_->compaction->output_level(), compaction_stats_);

  if (status.ok()) {
    status = InstallCompactionResults(mutable_cf_options);
  }
  VersionStorageInfo::LevelSummaryStorage tmp;
  auto vstorage = cfd->current()->storage_info();
  const auto& stats = compaction_stats_;
  const auto micros = static_cast<double>(std::max<uint64_t>(stats.micros, 1));
  const auto bytes_read_non_output_levels = static_cast<double>(
      std::max<uint64_t>(stats.bytes_read_non_output_levels, 1));
  LOG_TO_BUFFER(
      log_buffer_,
      "[%s] compacted to: %s, MB/sec: %.1f rd, %.1f wr, level %d, "
      "files in(%d, %d) out(%d) "
      "MB in(%.1f, %.1f) out(%.1f), read-write-amplify(%.1f) "
      "write-amplify(%.1f) %s, records in: %d, records dropped: %d\n",
      cfd->GetName().c_str(), vstorage->LevelSummary(&tmp),
      (stats.bytes_read_non_output_levels + stats.bytes_read_output_level) /
          static_cast<double>(stats.micros),
      stats.bytes_written / static_cast<double>(stats.micros),
      compact_->compaction->output_level(),
      stats.num_input_files_in_non_output_levels,
      stats.num_input_files_in_output_level,
      stats.num_output_files,
      stats.bytes_read_non_output_levels / 1048576.0,
      stats.bytes_read_output_level / 1048576.0,
      stats.bytes_written / 1048576.0,
      (stats.bytes_written + stats.bytes_read_output_level + stats.bytes_read_non_output_levels) /
          bytes_read_non_output_levels,
      stats.bytes_written / bytes_read_non_output_levels,
      status.ToString().c_str(), stats.num_input_records,
      stats.num_dropped_records);

  UpdateCompactionJobStats(stats);

  auto stream = event_logger_->LogToBuffer(log_buffer_);
  stream << "job" << job_id_
         << "event" << "compaction_finished"
         << "compaction_time_micros" << compaction_stats_.micros
         << "output_level" << compact_->compaction->output_level()
         << "num_output_files" << compact_->NumOutputFiles()
         << "total_output_size" << compact_->total_bytes
         << "num_input_records" << compact_->num_input_records
         << "num_output_records" << compact_->num_output_records
         << "num_subcompactions" << compact_->sub_compact_states.size();

  if (measure_io_stats_ && compaction_job_stats_ != nullptr) {
    stream << "file_write_nanos" << compaction_job_stats_->file_write_nanos;
    stream << "file_range_sync_nanos"
           << compaction_job_stats_->file_range_sync_nanos;
    stream << "file_fsync_nanos" << compaction_job_stats_->file_fsync_nanos;
    stream << "file_prepare_write_nanos"
           << compaction_job_stats_->file_prepare_write_nanos;
  }

  stream << "lsm_state";
  stream.StartArray();
  for (int level = 0; level < vstorage->num_levels(); ++level) {
    stream << vstorage->NumLevelFiles(level);
  }
  stream.EndArray();

  CleanupCompaction();
  return status;
}

void CompactionJob::ProcessKeyValueCompaction(
    FileNumbersHolder* holder, SubcompactionState* sub_compact) {
  assert(sub_compact != nullptr);
  std::unique_ptr<InternalIterator> input(
      versions_->MakeInputIterator(sub_compact->compaction));

  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_PROCESS_KV);

  // I/O measurement variables
  PerfLevel prev_perf_level = PerfLevel::kEnableTime;
  const uint64_t kRecordStatsEvery = 1000;
  uint64_t prev_write_nanos = 0;
  uint64_t prev_fsync_nanos = 0;
  uint64_t prev_range_sync_nanos = 0;
  uint64_t prev_prepare_write_nanos = 0;
  if (measure_io_stats_) {
    prev_perf_level = GetPerfLevel();
    SetPerfLevel(PerfLevel::kEnableTime);
    prev_write_nanos = IOSTATS(write_nanos);
    prev_fsync_nanos = IOSTATS(fsync_nanos);
    prev_range_sync_nanos = IOSTATS(range_sync_nanos);
    prev_prepare_write_nanos = IOSTATS(prepare_write_nanos);
  }

  ColumnFamilyData* cfd = sub_compact->compaction->column_family_data();
  auto compaction_filter = cfd->ioptions()->compaction_filter;
  std::unique_ptr<CompactionFilter> compaction_filter_from_factory = nullptr;
  if (compaction_filter == nullptr) {
    compaction_filter_from_factory =
        sub_compact->compaction->CreateCompactionFilter();
    compaction_filter = compaction_filter_from_factory.get();
  }

  if (compaction_filter) {
    // This is used to persist the history cutoff hybrid time chosen for the DocDB compaction
    // filter.
    largest_user_frontier_ = compaction_filter->GetLargestUserFrontier();
  }

  MergeHelper merge(
      env_, cfd->user_comparator(), cfd->ioptions()->merge_operator,
      compaction_filter, db_options_.info_log.get(),
      cfd->ioptions()->min_partial_merge_operands,
      false /* internal key corruption is expected */,
      existing_snapshots_.empty() ? 0 : existing_snapshots_.back(),
      compact_->compaction->level(), db_options_.statistics.get());

  TEST_SYNC_POINT("CompactionJob::Run():Inprogress");

  Slice* start = sub_compact->start;
  Slice* end = sub_compact->end;
  if (start != nullptr) {
    IterKey start_iter;
    start_iter.SetInternalKey(*start, kMaxSequenceNumber, kValueTypeForSeek);
    input->Seek(start_iter.GetKey());
  } else {
    input->SeekToFirst();
  }

  Status status;
  sub_compact->c_iter.reset(new CompactionIterator(
      input.get(), cfd->user_comparator(), &merge, versions_->LastSequence(),
      &existing_snapshots_, earliest_write_conflict_snapshot_, false,
      sub_compact->compaction, compaction_filter));
  auto c_iter = sub_compact->c_iter.get();
  c_iter->SeekToFirst();
  const auto& c_iter_stats = c_iter->iter_stats();
  // TODO(noetzli): check whether we could check !shutting_down_->... only
  // only occasionally (see diff D42687)
  while (status.ok() && !shutting_down_->load(std::memory_order_acquire) &&
         !cfd->IsDropped() && c_iter->Valid()) {
    // Invariant: c_iter.status() is guaranteed to be OK if c_iter->Valid()
    // returns true.
    const Slice& key = c_iter->key();
    const Slice& value = c_iter->value();

    // If an end key (exclusive) is specified, check if the current key is
    // >= than it and exit if it is because the iterator is out of its range
    if (end != nullptr &&
        cfd->user_comparator()->Compare(c_iter->user_key(), *end) >= 0) {
      break;
    } else if (sub_compact->compaction->ShouldStopBefore(key) &&
               sub_compact->builder != nullptr) {
      status = FinishCompactionOutputFile(input->status(), sub_compact);
      if (!status.ok()) {
        break;
      }
    }

    if (c_iter_stats.num_input_records % kRecordStatsEvery ==
        kRecordStatsEvery - 1) {
      RecordDroppedKeys(c_iter_stats, &sub_compact->compaction_job_stats);
      c_iter->ResetRecordCounts();
      RecordCompactionIOStats();
    }

    // Open output file if necessary
    if (sub_compact->builder == nullptr) {
      status = OpenCompactionOutputFile(holder, sub_compact);
      if (!status.ok()) {
        break;
      }
    }
    assert(sub_compact->builder != nullptr);
    assert(sub_compact->current_output() != nullptr);
    sub_compact->builder->Add(key, value);
    auto boundaries = MakeFileBoundaryValues(db_options_.boundary_extractor.get(),
                                             key,
                                             value);
    if (!boundaries) {
      status = std::move(boundaries.status());
      break;
    }
    auto& boundary_values = *boundaries;
    sub_compact->current_output()->meta.UpdateBoundaries(std::move(boundary_values.key),
                                                         boundary_values);
    sub_compact->num_output_records++;

    // Close output file if it is big enough
    // TODO(aekmekji): determine if file should be closed earlier than this
    // during subcompactions (i.e. if output size, estimated by input size, is
    // going to be 1.2MB and max_output_file_size = 1MB, prefer to have 0.6MB
    // and 0.6MB instead of 1MB and 0.2MB)
    if (sub_compact->builder->TotalFileSize() >=
        sub_compact->compaction->max_output_file_size()) {
      status = FinishCompactionOutputFile(input->status(), sub_compact);
    }

    c_iter->Next();
  }

  sub_compact->num_input_records = c_iter_stats.num_input_records;
  sub_compact->compaction_job_stats.num_input_deletion_records =
      c_iter_stats.num_input_deletion_records;
  sub_compact->compaction_job_stats.num_corrupt_keys =
      c_iter_stats.num_input_corrupt_records;
  sub_compact->compaction_job_stats.total_input_raw_key_bytes +=
      c_iter_stats.total_input_raw_key_bytes;
  sub_compact->compaction_job_stats.total_input_raw_value_bytes +=
      c_iter_stats.total_input_raw_value_bytes;

  RecordDroppedKeys(c_iter_stats, &sub_compact->compaction_job_stats);
  RecordCompactionIOStats();

  if (status.ok() &&
      (shutting_down_->load(std::memory_order_acquire) || cfd->IsDropped())) {
    status = STATUS(ShutdownInProgress,
        "Database shutdown or Column family drop during compaction");
  }
  if (status.ok() && sub_compact->builder != nullptr) {
    status = FinishCompactionOutputFile(input->status(), sub_compact);
  }
  if (status.ok()) {
    status = input->status();
  }

  if (measure_io_stats_) {
    sub_compact->compaction_job_stats.file_write_nanos +=
        IOSTATS(write_nanos) - prev_write_nanos;
    sub_compact->compaction_job_stats.file_fsync_nanos +=
        IOSTATS(fsync_nanos) - prev_fsync_nanos;
    sub_compact->compaction_job_stats.file_range_sync_nanos +=
        IOSTATS(range_sync_nanos) - prev_range_sync_nanos;
    sub_compact->compaction_job_stats.file_prepare_write_nanos +=
        IOSTATS(prepare_write_nanos) - prev_prepare_write_nanos;
    if (prev_perf_level != PerfLevel::kEnableTime) {
      SetPerfLevel(prev_perf_level);
    }
  }

  sub_compact->c_iter.reset();
  input.reset();
  sub_compact->status = status;
  if (compaction_filter) {
    compaction_filter->CompactionFinished();
  }
}

void CompactionJob::RecordDroppedKeys(
    const CompactionIteratorStats& c_iter_stats,
    CompactionJobStats* compaction_job_stats) {
  if (c_iter_stats.num_record_drop_user > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_USER,
               c_iter_stats.num_record_drop_user);
  }
  if (c_iter_stats.num_record_drop_hidden > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_NEWER_ENTRY,
               c_iter_stats.num_record_drop_hidden);
    if (compaction_job_stats) {
      compaction_job_stats->num_records_replaced +=
          c_iter_stats.num_record_drop_hidden;
    }
  }
  if (c_iter_stats.num_record_drop_obsolete > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_OBSOLETE,
               c_iter_stats.num_record_drop_obsolete);
    if (compaction_job_stats) {
      compaction_job_stats->num_expired_deletion_records +=
          c_iter_stats.num_record_drop_obsolete;
    }
  }
}

void CompactionJob::CloseFile(Status* status, std::unique_ptr<WritableFileWriter>* writer) {
  if (status->ok() && !db_options_.disableDataSync) {
    StopWatch sw(env_, stats_, COMPACTION_OUTFILE_SYNC_MICROS);
    *status = (*writer)->Sync(db_options_.use_fsync);
  }
  if (status->ok()) {
    *status = (*writer)->Close();
  }
  writer->reset();

}

Status CompactionJob::FinishCompactionOutputFile(
    const Status& input_status, SubcompactionState* sub_compact) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_SYNC_FILE);
  assert(sub_compact != nullptr);
  assert(sub_compact->base_outfile);
  const bool is_split_sst = sub_compact->compaction->column_family_data()->ioptions()
      ->table_factory->IsSplitSstForWriteSupported();
  assert((sub_compact->data_outfile != nullptr) == is_split_sst);
  assert(sub_compact->builder != nullptr);
  assert(sub_compact->current_output() != nullptr);

  uint64_t output_number = sub_compact->current_output()->meta.fd.GetNumber();
  assert(output_number != 0);

  TableProperties table_properties;
  // Check for iterator errors
  Status s = input_status;
  auto meta = &sub_compact->current_output()->meta;
  const uint64_t current_entries = sub_compact->builder->NumEntries();
  meta->marked_for_compaction = sub_compact->builder->NeedCompact();
  if (s.ok()) {
    s = sub_compact->builder->Finish();
  } else {
    sub_compact->builder->Abandon();
  }

  const uint64_t current_total_bytes = sub_compact->builder->TotalFileSize();
  meta->fd.total_file_size = current_total_bytes;
  meta->fd.base_file_size = sub_compact->builder->BaseFileSize();
  sub_compact->current_output()->finished = true;
  sub_compact->total_bytes += current_total_bytes;

  // Finish and check for file errors
  if (sub_compact->data_outfile) {
    CloseFile(&s, &sub_compact->data_outfile);
  }
  CloseFile(&s, &sub_compact->base_outfile);

  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    ColumnFamilyData* cfd = sub_compact->compaction->column_family_data();
    InternalIterator* iter = cfd->table_cache()->NewIterator(
        ReadOptions(), env_options_, cfd->internal_comparator(), meta->fd, meta->UserFilter(),
        nullptr, cfd->internal_stats()->GetFileReadHist(
                     compact_->compaction->output_level()),
        false);
    s = iter->status();

    if (s.ok() && paranoid_file_checks_) {
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {}
      s = iter->status();
    }

    delete iter;
    if (s.ok()) {
      auto tp = sub_compact->builder->GetTableProperties();
      sub_compact->current_output()->table_properties =
          std::make_shared<TableProperties>(tp);
      TableFileCreationInfo info(std::move(tp));
      info.db_name = dbname_;
      info.cf_name = cfd->GetName();
      info.file_path =
          TableFileName(cfd->ioptions()->db_paths, meta->fd.GetNumber(),
                        meta->fd.GetPathId());
      info.file_size = meta->fd.GetTotalFileSize();
      info.job_id = job_id_;
      RLOG(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
          "[%s] [JOB %d] Generated table #%" PRIu64 ": %" PRIu64
          " keys, %" PRIu64 " bytes%s",
          cfd->GetName().c_str(), job_id_, output_number, current_entries,
          current_total_bytes,
          meta->marked_for_compaction ? " (need compaction)" : "");
      EventHelpers::LogAndNotifyTableFileCreation(
          event_logger_, cfd->ioptions()->listeners, meta->fd, info);
    }
  }

  // Report new file to SstFileManagerImpl
  auto sfm =
      static_cast<SstFileManagerImpl*>(db_options_.sst_file_manager.get());
  if (sfm && meta->fd.GetPathId() == 0) {
    ColumnFamilyData* cfd = sub_compact->compaction->column_family_data();
    auto fn = TableFileName(cfd->ioptions()->db_paths, meta->fd.GetNumber(),
                            meta->fd.GetPathId());
    RETURN_NOT_OK(sfm->OnAddFile(fn));
    if (is_split_sst) {
      RETURN_NOT_OK(sfm->OnAddFile(TableBaseToDataFileName(fn)));
    }
    if (sfm->IsMaxAllowedSpaceReached()) {
      InstrumentedMutexLock l(db_mutex_);
      if (db_bg_error_->ok()) {
        s = STATUS(IOError, "Max allowed space was reached");
        *db_bg_error_ = s;
        TEST_SYNC_POINT(
            "CompactionJob::FinishCompactionOutputFile:MaxAllowedSpaceReached");
      }
    }
  }

  sub_compact->builder.reset();
  return s;
}

Status CompactionJob::InstallCompactionResults(
    const MutableCFOptions& mutable_cf_options) {
  db_mutex_->AssertHeld();

  auto* compaction = compact_->compaction;
  // paranoia: verify that the files that we started with
  // still exist in the current version and in the same original level.
  // This ensures that a concurrent compaction did not erroneously
  // pick the same files to compact_.
  if (!versions_->VerifyCompactionFileConsistency(compaction)) {
    Compaction::InputLevelSummaryBuffer inputs_summary;

    RLOG(InfoLogLevel::ERROR_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] Compaction %s aborted",
        compaction->column_family_data()->GetName().c_str(), job_id_,
        compaction->InputLevelSummary(&inputs_summary));
    return STATUS(Corruption, "Compaction input files inconsistent");
  }

  {
    Compaction::InputLevelSummaryBuffer inputs_summary;
    RLOG(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] Compacted %s => %" PRIu64 " bytes",
        compaction->column_family_data()->GetName().c_str(), job_id_,
        compaction->InputLevelSummary(&inputs_summary), compact_->total_bytes);
  }

  // Add compaction outputs
  compaction->AddInputDeletions(compaction->edit());

  for (const auto& sub_compact : compact_->sub_compact_states) {
    for (const auto& out : sub_compact.outputs) {
      compaction->edit()->AddFile(compaction->output_level(), out.meta);
    }
  }
  if (largest_user_frontier_) {
    compaction->edit()->UpdateFlushedFrontier(largest_user_frontier_);
  }
  return versions_->LogAndApply(compaction->column_family_data(),
                                mutable_cf_options, compaction->edit(),
                                db_mutex_, db_directory_);
}

void CompactionJob::RecordCompactionIOStats() {
  RecordTick(stats_, COMPACT_READ_BYTES, IOSTATS(bytes_read));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_READ, IOSTATS(bytes_read));
  IOSTATS_RESET(bytes_read);
  RecordTick(stats_, COMPACT_WRITE_BYTES, IOSTATS(bytes_written));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_WRITTEN, IOSTATS(bytes_written));
  IOSTATS_RESET(bytes_written);
}

Status CompactionJob::OpenFile(const std::string table_name, uint64_t file_number,
    const std::string file_type_label, const std::string fname,
    std::unique_ptr<WritableFile>* writable_file) {
  Status s = NewWritableFile(env_, fname, writable_file, env_options_);
  if (!s.ok()) {
    RLOG(InfoLogLevel::ERROR_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] OpenCompactionOutputFiles for table #%" PRIu64
        " fails at NewWritableFile for %s file with status %s", table_name.c_str(),
        job_id_, file_number, file_type_label.c_str(), s.ToString().c_str());
    LogFlush(db_options_.info_log);
  }
  return s;
}

Status CompactionJob::OpenCompactionOutputFile(
    FileNumbersHolder* holder, SubcompactionState* sub_compact) {
  assert(sub_compact != nullptr);
  assert(sub_compact->builder == nullptr);
  FileNumber file_number = file_numbers_provider_->NewFileNumber(holder);

  // Make the output file
  unique_ptr<WritableFile> base_writable_file;
  unique_ptr<WritableFile> data_writable_file;
  const std::string base_fname = TableFileName(db_options_.db_paths, file_number,
                                    sub_compact->compaction->output_path_id());
  const std::string data_fname = TableBaseToDataFileName(base_fname);
  const std::string table_name = sub_compact->compaction->column_family_data()->GetName();
  RETURN_NOT_OK(OpenFile(table_name, file_number, "base", base_fname, &base_writable_file));
  RETURN_NOT_OK(OpenFile(table_name, file_number, "data", data_fname, &data_writable_file));

  SubcompactionState::Output out;
  out.meta.fd =
      FileDescriptor(file_number, sub_compact->compaction->output_path_id(), 0, 0);
  // Update sequence number boundaries for out.
  for (size_t level_idx = 0; level_idx < compact_->compaction->num_input_levels(); level_idx++) {
    for (FileMetaData *fmd : *compact_->compaction->inputs(level_idx) ) {
      out.meta.UpdateBoundariesExceptKey(fmd->smallest, UpdateBoundariesType::kSmallest);
      out.meta.UpdateBoundariesExceptKey(fmd->largest, UpdateBoundariesType::kLargest);
    }
  }
  out.finished = false;

  sub_compact->outputs.push_back(out);

  ColumnFamilyData* cfd = sub_compact->compaction->column_family_data();

  {
    auto setup_outfile = [this, sub_compact] (
        size_t preallocation_block_size, std::unique_ptr<WritableFile>* writable_file,
        std::unique_ptr<WritableFileWriter>* writer) {
      (*writable_file)->SetIOPriority(Env::IO_LOW);
      if (preallocation_block_size > 0) {
        (*writable_file)->SetPreallocationBlockSize(preallocation_block_size);
      }
      writer->reset(new WritableFileWriter(
          std::move(*writable_file), env_options_, sub_compact->compaction->suspender()));
    };

    const bool is_split_sst = cfd->ioptions()->table_factory->IsSplitSstForWriteSupported();
    const size_t preallocation_data_block_size = static_cast<size_t>(
        sub_compact->compaction->OutputFilePreallocationSize());
    // if we don't have separate data file - preallocate size for base file
    setup_outfile(
        is_split_sst ? 0 : preallocation_data_block_size, &base_writable_file,
        &sub_compact->base_outfile);
    if (is_split_sst) {
      setup_outfile(preallocation_data_block_size, &data_writable_file, &sub_compact->data_outfile);
    }
  }

  // If the Column family flag is to only optimize filters for hits,
  // we can skip creating filters if this is the bottommost_level where
  // data is going to be found
  bool skip_filters =
      cfd->ioptions()->optimize_filters_for_hits && bottommost_level_;
  sub_compact->builder.reset(NewTableBuilder(
      *cfd->ioptions(), cfd->internal_comparator(),
      cfd->int_tbl_prop_collector_factories(), cfd->GetID(),
      sub_compact->base_outfile.get(), sub_compact->data_outfile.get(),
      sub_compact->compaction->output_compression(), cfd->ioptions()->compression_opts,
      skip_filters));
  LogFlush(db_options_.info_log);
  return Status::OK();
}

void CompactionJob::CleanupCompaction() {
  for (SubcompactionState& sub_compact : compact_->sub_compact_states) {
    const auto& sub_status = sub_compact.status;

    if (sub_compact.builder != nullptr) {
      // May happen if we get a shutdown call in the middle of compaction
      sub_compact.builder->Abandon();
      sub_compact.builder.reset();
    } else if (sub_status.ok() &&
        (sub_compact.base_outfile != nullptr || sub_compact.data_outfile != nullptr)) {
      std::string log_message;
      log_message.append("sub_status.ok(), but: sub_compact.base_outfile ");
      log_message.append(sub_compact.base_outfile == nullptr ? "==" : "!=");
      log_message.append(" nullptr, sub_compact.data_outfile ");
      log_message.append(sub_compact.data_outfile == nullptr ? "==" : "!=");
      log_message.append(" nullptr");
      RLOG(InfoLogLevel::FATAL_LEVEL, db_options_.info_log, log_message.c_str());
      assert(!"If sub_status is OK, sub_compact.*_outfile should be nullptr");
    }
    for (const auto& out : sub_compact.outputs) {
      // If this file was inserted into the table cache then remove
      // them here because this compaction was not committed.
      if (!sub_status.ok()) {
        TableCache::Evict(table_cache_.get(), out.meta.fd.GetNumber());
      }
    }
  }
  delete compact_;
  compact_ = nullptr;
}

#ifndef ROCKSDB_LITE
namespace {
void CopyPrefix(
    const Slice& src, size_t prefix_length, std::string* dst) {
  assert(prefix_length > 0);
  size_t length = src.size() > prefix_length ? prefix_length : src.size();
  dst->assign(src.cdata(), length);
}
}  // namespace

#endif  // !ROCKSDB_LITE

void CompactionJob::UpdateCompactionStats() {
  Compaction* compaction = compact_->compaction;
  compaction_stats_.num_input_files_in_non_output_levels = 0;
  compaction_stats_.num_input_files_in_output_level = 0;
  for (int input_level = 0;
       input_level < static_cast<int>(compaction->num_input_levels());
       ++input_level) {
    if (compaction->start_level() + input_level
        != compaction->output_level()) {
      UpdateCompactionInputStatsHelper(
          &compaction_stats_.num_input_files_in_non_output_levels,
          &compaction_stats_.bytes_read_non_output_levels,
          input_level);
    } else {
      UpdateCompactionInputStatsHelper(
          &compaction_stats_.num_input_files_in_output_level,
          &compaction_stats_.bytes_read_output_level,
          input_level);
    }
  }

  for (const auto& sub_compact : compact_->sub_compact_states) {
    size_t num_output_files = sub_compact.outputs.size();
    if (sub_compact.builder != nullptr) {
      // An error occurred so ignore the last output.
      assert(num_output_files > 0);
      --num_output_files;
    }
    compaction_stats_.num_output_files += static_cast<int>(num_output_files);

    for (const auto& out : sub_compact.outputs) {
      compaction_stats_.bytes_written += out.meta.fd.total_file_size;
    }
    if (sub_compact.num_input_records > sub_compact.num_output_records) {
      compaction_stats_.num_dropped_records +=
          sub_compact.num_input_records - sub_compact.num_output_records;
    }
  }
}

void CompactionJob::UpdateCompactionInputStatsHelper(
    int* num_files, uint64_t* bytes_read, int input_level) {
  const Compaction* compaction = compact_->compaction;
  auto num_input_files = compaction->num_input_files(input_level);
  *num_files += static_cast<int>(num_input_files);

  for (size_t i = 0; i < num_input_files; ++i) {
    const auto* file_meta = compaction->input(input_level, i);
    *bytes_read += file_meta->fd.GetTotalFileSize();
    compaction_stats_.num_input_records +=
        static_cast<uint64_t>(file_meta->num_entries);
  }
}

void CompactionJob::UpdateCompactionJobStats(
    const InternalStats::CompactionStats& stats) const {
#ifndef ROCKSDB_LITE
  if (compaction_job_stats_) {
    compaction_job_stats_->elapsed_micros = stats.micros;

    // input information
    compaction_job_stats_->total_input_bytes =
        stats.bytes_read_non_output_levels +
        stats.bytes_read_output_level;
    compaction_job_stats_->num_input_records =
        compact_->num_input_records;
    compaction_job_stats_->num_input_files =
        stats.num_input_files_in_non_output_levels +
        stats.num_input_files_in_output_level;
    compaction_job_stats_->num_input_files_at_output_level =
        stats.num_input_files_in_output_level;

    // output information
    compaction_job_stats_->total_output_bytes = stats.bytes_written;
    compaction_job_stats_->num_output_records =
        compact_->num_output_records;
    compaction_job_stats_->num_output_files = stats.num_output_files;

    if (compact_->NumOutputFiles() > 0U) {
      CopyPrefix(
          compact_->SmallestUserKey(),
          CompactionJobStats::kMaxPrefixLength,
          &compaction_job_stats_->smallest_output_key_prefix);
      CopyPrefix(
          compact_->LargestUserKey(),
          CompactionJobStats::kMaxPrefixLength,
          &compaction_job_stats_->largest_output_key_prefix);
    }
  }
#endif  // !ROCKSDB_LITE
}

void CompactionJob::LogCompaction() {
  Compaction* compaction = compact_->compaction;
  ColumnFamilyData* cfd = compaction->column_family_data();

  // Let's check if anything will get logged. Don't prepare all the info if
  // we're not logging
  if (db_options_.info_log_level <= InfoLogLevel::INFO_LEVEL) {
    Compaction::InputLevelSummaryBuffer inputs_summary;
    RLOG(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] Compacting %s, score %.2f", cfd->GetName().c_str(),
        job_id_, compaction->InputLevelSummary(&inputs_summary),
        compaction->score());
    char scratch[2345];
    compaction->Summary(scratch, sizeof(scratch));
    RLOG(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
        "[%s] Compaction start summary: %s\n", cfd->GetName().c_str(), scratch);
    // build event logger report
    auto stream = event_logger_->Log();
    stream << "job" << job_id_ << "event"
           << "compaction_started";
    for (size_t i = 0; i < compaction->num_input_levels(); ++i) {
      stream << ("files_L" + ToString(compaction->level(i)));
      stream.StartArray();
      for (auto f : *compaction->inputs(i)) {
        stream << f->fd.GetNumber();
      }
      stream.EndArray();
    }
    stream << "score" << compaction->score() << "input_data_size"
           << compaction->CalculateTotalInputSize();
  }
}

}  // namespace rocksdb
