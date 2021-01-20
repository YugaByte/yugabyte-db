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

#include "yb/docdb/docdb_rocksdb_util.h"

#include <thread>
#include <memory>

#include "yb/common/transaction.h"

#include "yb/rocksdb/memtablerep.h"
#include "yb/rocksdb/rate_limiter.h"
#include "yb/rocksdb/table.h"
#include "yb/rocksdb/db/db_impl.h"
#include "yb/rocksdb/db/version_edit.h"
#include "yb/rocksdb/db/version_set.h"
#include "yb/rocksdb/db/writebuffer.h"
#include "yb/rocksdb/table/filtering_iterator.h"
#include "yb/rocksdb/util/compression.h"

#include "yb/docdb/bounded_rocksdb_iterator.h"
#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/doc_ttl_util.h"
#include "yb/docdb/intent_aware_iterator.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/rocksutil/yb_rocksdb_logger.h"
#include "yb/server/hybrid_clock.h"
#include "yb/util/priority_thread_pool.h"
#include "yb/util/size_literals.h"
#include "yb/util/status.h"
#include "yb/util/trace.h"
#include "yb/gutil/sysinfo.h"

using namespace yb::size_literals;  // NOLINT.
using namespace std::literals;

DEFINE_int32(rocksdb_max_background_flushes, -1, "Number threads to do background flushes.");
DEFINE_bool(rocksdb_disable_compactions, false, "Disable rocksdb compactions.");
DEFINE_bool(rocksdb_compaction_measure_io_stats, false, "Measure stats for rocksdb compactions.");
DEFINE_int32(rocksdb_base_background_compactions, -1,
             "Number threads to do background compactions.");
DEFINE_int32(rocksdb_max_background_compactions, -1,
             "Increased number of threads to do background compactions (used when compactions need "
             "to catch up.)");
DEFINE_int32(rocksdb_level0_file_num_compaction_trigger, 5,
             "Number of files to trigger level-0 compaction. -1 if compaction should not be "
             "triggered by number of files at all.");

DEFINE_int32(rocksdb_level0_slowdown_writes_trigger, -1,
             "The number of files above which writes are slowed down.");
DEFINE_int32(rocksdb_level0_stop_writes_trigger, -1,
             "The number of files above which compactions are stopped.");
DEFINE_int32(rocksdb_universal_compaction_size_ratio, 20,
             "The percentage upto which files that are larger are include in a compaction.");
DEFINE_uint64(rocksdb_universal_compaction_always_include_size_threshold, 64_MB,
             "Always include files of smaller or equal size in a compaction.");
DEFINE_int32(rocksdb_universal_compaction_min_merge_width, 4,
             "The minimum number of files in a single compaction run.");
DEFINE_int64(rocksdb_compact_flush_rate_limit_bytes_per_sec, 256_MB,
             "Use to control write rate of flush and compaction.");
DEFINE_uint64(rocksdb_compaction_size_threshold_bytes, 2ULL * 1024 * 1024 * 1024,
             "Threshold beyond which compaction is considered large.");
DEFINE_uint64(rocksdb_max_file_size_for_compaction, 0,
             "Maximal allowed file size to participate in RocksDB compaction. 0 - unlimited.");
DEFINE_int32(rocksdb_max_write_buffer_number, 2,
             "Maximum number of write buffers that are built up in memory.");

DEFINE_int64(db_block_size_bytes, 32_KB,
             "Size of RocksDB data block (in bytes).");

DEFINE_int64(db_filter_block_size_bytes, 64_KB,
             "Size of RocksDB filter block (in bytes).");

DEFINE_int64(db_index_block_size_bytes, 32_KB,
             "Size of RocksDB index block (in bytes).");

DEFINE_int64(db_min_keys_per_index_block, 100,
             "Minimum number of keys per index block.");

DEFINE_int64(db_write_buffer_size, -1,
             "Size of RocksDB write buffer (in bytes). -1 to use default.");

DEFINE_int32(memstore_size_mb, 128,
             "Max size (in mb) of the memstore, before needing to flush.");

DEFINE_bool(use_docdb_aware_bloom_filter, true,
            "Whether to use the DocDbAwareFilterPolicy for both bloom storage and seeks.");
// Empirically 2 is a minimal value that provides best performance on sequential scan.
DEFINE_int32(max_nexts_to_avoid_seek, 2,
             "The number of next calls to try before doing resorting to do a rocksdb seek.");
DEFINE_bool(trace_docdb_calls, false, "Whether we should trace calls into the docdb.");
DEFINE_bool(use_multi_level_index, true, "Whether to use multi-level data index.");

DEFINE_uint64(initial_seqno, 1ULL << 50, "Initial seqno for new RocksDB instances.");

DEFINE_int32(num_reserved_small_compaction_threads, -1, "Number of reserved small compaction "
             "threads. It allows splitting small vs. large compactions.");

DEFINE_bool(enable_ondisk_compression, true,
            "Determines whether SSTable compression is enabled or not.");

DEFINE_int32(priority_thread_pool_size, -1,
             "Max running workers in compaction thread pool. "
             "If -1 and max_background_compactions is specified - use max_background_compactions. "
             "If -1 and max_background_compactions is not specified - use sqrt(num_cpus).");

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using strings::Substitute;

namespace yb {
namespace docdb {

std::shared_ptr<rocksdb::BoundaryValuesExtractor> DocBoundaryValuesExtractorInstance();

void SeekForward(const rocksdb::Slice& slice, rocksdb::Iterator *iter) {
  if (!iter->Valid() || iter->key().compare(slice) >= 0) {
    return;
  }
  ROCKSDB_SEEK(iter, slice);
}

void SeekForward(const KeyBytes& key_bytes, rocksdb::Iterator *iter) {
  SeekForward(key_bytes.AsSlice(), iter);
}

KeyBytes AppendDocHt(const Slice& key, const DocHybridTime& doc_ht) {
  char buf[kMaxBytesPerEncodedHybridTime + 1];
  buf[0] = ValueTypeAsChar::kHybridTime;
  auto end = doc_ht.EncodedInDocDbFormat(buf + 1);
  return KeyBytes(key, Slice(buf, end));
}

void SeekPastSubKey(const Slice& key, rocksdb::Iterator* iter) {
  SeekForward(AppendDocHt(key, DocHybridTime::kMin), iter);
}

void SeekOutOfSubKey(KeyBytes* key_bytes, rocksdb::Iterator* iter) {
  key_bytes->AppendValueType(ValueType::kMaxByte);
  SeekForward(*key_bytes, iter);
  key_bytes->RemoveValueTypeSuffix(ValueType::kMaxByte);
}

void SeekPossiblyUsingNext(rocksdb::Iterator* iter, const Slice& seek_key,
                           int* next_count, int* seek_count) {
  for (int nexts = FLAGS_max_nexts_to_avoid_seek; nexts-- > 0;) {
    if (!iter->Valid() || iter->key().compare(seek_key) >= 0) {
      if (FLAGS_trace_docdb_calls) {
        TRACE("Did $0 Next(s) instead of a Seek", nexts);
      }
      return;
    }
    VLOG(4) << "Skipping: " << SubDocKey::DebugSliceToString(iter->key());

    iter->Next();
    ++*next_count;
  }

  if (FLAGS_trace_docdb_calls) {
    TRACE("Forced to do an actual Seek after $0 Next(s)", FLAGS_max_nexts_to_avoid_seek);
  }
  iter->Seek(seek_key);
  ++*seek_count;
}

void PerformRocksDBSeek(
    rocksdb::Iterator *iter,
    const rocksdb::Slice &seek_key,
    const char* file_name,
    int line) {
  int next_count = 0;
  int seek_count = 0;
  if (seek_key.size() == 0) {
    iter->SeekToFirst();
    ++seek_count;
  } else if (!iter->Valid() || iter->key().compare(seek_key) > 0) {
    iter->Seek(seek_key);
    ++seek_count;
  } else {
    SeekPossiblyUsingNext(iter, seek_key, &next_count, &seek_count);
  }
  VLOG(4) << Substitute(
      "PerformRocksDBSeek at $0:$1:\n"
      "    Seek key:         $2\n"
      "    Seek key (raw):   $3\n"
      "    Actual key:       $4\n"
      "    Actual key (raw): $5\n"
      "    Actual value:     $6\n"
      "    Next() calls:     $7\n"
      "    Seek() calls:     $8\n",
      file_name, line,
      BestEffortDocDBKeyToStr(seek_key),
      FormatSliceAsStr(seek_key),
      iter->Valid() ? BestEffortDocDBKeyToStr(KeyBytes(iter->key())) : "N/A",
      iter->Valid() ? FormatSliceAsStr(iter->key()) : "N/A",
      iter->Valid() ? FormatSliceAsStr(iter->value()) : "N/A",
      next_count,
      seek_count);
}

namespace {

rocksdb::ReadOptions PrepareReadOptions(
    rocksdb::DB* rocksdb,
    BloomFilterMode bloom_filter_mode,
    const boost::optional<const Slice>& user_key_for_filter,
    const rocksdb::QueryId query_id,
    std::shared_ptr<rocksdb::ReadFileFilter> file_filter,
    const Slice* iterate_upper_bound) {
  rocksdb::ReadOptions read_opts;
  read_opts.query_id = query_id;
  if (FLAGS_use_docdb_aware_bloom_filter &&
    bloom_filter_mode == BloomFilterMode::USE_BLOOM_FILTER) {
    DCHECK(user_key_for_filter);
    read_opts.table_aware_file_filter = rocksdb->GetOptions().table_factory->
        NewTableAwareReadFileFilter(read_opts, user_key_for_filter.get());
  }
  read_opts.file_filter = std::move(file_filter);
  read_opts.iterate_upper_bound = iterate_upper_bound;
  return read_opts;
}

} // namespace

BoundedRocksDbIterator CreateRocksDBIterator(
    rocksdb::DB* rocksdb,
    const KeyBounds* docdb_key_bounds,
    BloomFilterMode bloom_filter_mode,
    const boost::optional<const Slice>& user_key_for_filter,
    const rocksdb::QueryId query_id,
    std::shared_ptr<rocksdb::ReadFileFilter> file_filter,
    const Slice* iterate_upper_bound) {
  rocksdb::ReadOptions read_opts = PrepareReadOptions(rocksdb, bloom_filter_mode,
      user_key_for_filter, query_id, std::move(file_filter), iterate_upper_bound);
  return BoundedRocksDbIterator(rocksdb, read_opts, docdb_key_bounds);
}

unique_ptr<IntentAwareIterator> CreateIntentAwareIterator(
    const DocDB& doc_db,
    BloomFilterMode bloom_filter_mode,
    const boost::optional<const Slice>& user_key_for_filter,
    const rocksdb::QueryId query_id,
    const TransactionOperationContextOpt& txn_op_context,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time,
    std::shared_ptr<rocksdb::ReadFileFilter> file_filter,
    const Slice* iterate_upper_bound) {
  // TODO(dtxn) do we need separate options for intents db?
  rocksdb::ReadOptions read_opts = PrepareReadOptions(doc_db.regular, bloom_filter_mode,
      user_key_for_filter, query_id, std::move(file_filter), iterate_upper_bound);
  return std::make_unique<IntentAwareIterator>(
      doc_db, read_opts, deadline, read_time, txn_op_context);
}

namespace {

std::mutex rocksdb_flags_mutex;

// Auto initialize some of the RocksDB flags that are defaulted to -1.
void AutoInitRocksDBFlags(rocksdb::Options* options) {
  const int kNumCpus = base::NumCPUs();
  std::unique_lock<std::mutex> lock(rocksdb_flags_mutex);

  if (FLAGS_rocksdb_max_background_flushes == -1) {
    constexpr auto kCpusPerFlushThread = 8;
    constexpr auto kAutoMaxBackgroundFlushesHighLimit = 4;
    auto flushes = 1 + kNumCpus / kCpusPerFlushThread;
    FLAGS_rocksdb_max_background_flushes = std::min(flushes, kAutoMaxBackgroundFlushesHighLimit);
    LOG(INFO) << "Auto setting FLAGS_rocksdb_max_background_flushes to "
              << FLAGS_rocksdb_max_background_flushes;
  }
  options->max_background_flushes = FLAGS_rocksdb_max_background_flushes;

  if (FLAGS_rocksdb_disable_compactions) {
    return;
  }

  bool has_rocksdb_max_background_compactions = false;
  // This controls the maximum number of schedulable compactions, per each instance of rocksdb, of
  // which we will have many. We also do not want to waste resources by having too many queued
  // compactions.
  if (FLAGS_rocksdb_max_background_compactions == -1) {
    if (kNumCpus <= 4) {
      FLAGS_rocksdb_max_background_compactions = 1;
    } else if (kNumCpus <= 8) {
      FLAGS_rocksdb_max_background_compactions = 2;
    } else if (kNumCpus <= 32) {
      FLAGS_rocksdb_max_background_compactions = 3;
    } else {
      FLAGS_rocksdb_max_background_compactions = 4;
    }
    LOG(INFO) << "Auto setting FLAGS_rocksdb_max_background_compactions to "
              << FLAGS_rocksdb_max_background_compactions;
  } else {
    // If we have provided an override, note that, so we can use that in the actual thread pool
    // sizing as well.
    has_rocksdb_max_background_compactions = true;
  }
  options->max_background_compactions = FLAGS_rocksdb_max_background_compactions;

  if (FLAGS_rocksdb_base_background_compactions == -1) {
    FLAGS_rocksdb_base_background_compactions = FLAGS_rocksdb_max_background_compactions;
    LOG(INFO) << "Auto setting FLAGS_rocksdb_base_background_compactions to "
              << FLAGS_rocksdb_base_background_compactions;
  }
  options->base_background_compactions = FLAGS_rocksdb_base_background_compactions;

  // This controls the number of background threads to use in the compaction thread pool.
  if (FLAGS_priority_thread_pool_size == -1) {
    if (has_rocksdb_max_background_compactions) {
      // If we did override the per-rocksdb flag, but not this one, just port over that value.
      FLAGS_priority_thread_pool_size = FLAGS_rocksdb_max_background_compactions;
    } else {
      // If we did not override the per-rocksdb queue size, then just use a production friendly
      // formula.
      //
      // For less then 8cpus, just manually tune to 1-2 threads. Above that, we can use 3.5/8.
      if (kNumCpus < 4) {
        FLAGS_priority_thread_pool_size = 1;
      } else if (kNumCpus < 8) {
        FLAGS_priority_thread_pool_size = 2;
      } else {
        FLAGS_priority_thread_pool_size = std::floor(kNumCpus * 3.5 / 8.0);
      }
    }
    LOG(INFO) << "Auto setting FLAGS_priority_thread_pool_size to "
              << FLAGS_priority_thread_pool_size;
  }
}

class HybridTimeFilteringIterator : public rocksdb::FilteringIterator {
 public:
  HybridTimeFilteringIterator(
      rocksdb::InternalIterator* iterator, bool arena_mode, HybridTime hybrid_time_filter)
      : rocksdb::FilteringIterator(iterator, arena_mode), hybrid_time_filter_(hybrid_time_filter) {}

 private:
  bool Satisfied(Slice key) override {
    auto user_key = rocksdb::ExtractUserKey(key);
    auto doc_ht = DocHybridTime::DecodeFromEnd(&user_key);
    if (!doc_ht.ok()) {
      LOG(DFATAL) << "Unable to decode doc ht " << rocksdb::ExtractUserKey(key) << ": "
                  << doc_ht.status();
      return true;
    }
    return doc_ht->hybrid_time() <= hybrid_time_filter_;
  }

  HybridTime hybrid_time_filter_;
};

template <class T, class... Args>
T* CreateOnArena(rocksdb::Arena* arena, Args&&... args) {
  if (!arena) {
    return new T(std::forward<Args>(args)...);
  }
  auto mem = arena->AllocateAligned(sizeof(T));
  return new (mem) T(std::forward<Args>(args)...);
}

rocksdb::InternalIterator* WrapIterator(
    rocksdb::InternalIterator* iterator, rocksdb::Arena* arena, const Slice& filter) {
  if (!filter.empty()) {
    HybridTime hybrid_time_filter;
    memcpy(&hybrid_time_filter, filter.data(), sizeof(hybrid_time_filter));
    return CreateOnArena<HybridTimeFilteringIterator>(
        arena, iterator, arena != nullptr, hybrid_time_filter);
  }
  return iterator;
}

void AddSupportedFilterPolicy(
    const rocksdb::BlockBasedTableOptions::FilterPolicyPtr& filter_policy,
    rocksdb::BlockBasedTableOptions* table_options) {
  table_options->supported_filter_policies->emplace(filter_policy->Name(), filter_policy);
}

} // namespace

void InitRocksDBOptions(
    rocksdb::Options* options, const string& log_prefix,
    const shared_ptr<rocksdb::Statistics>& statistics,
    const tablet::TabletOptions& tablet_options) {
  AutoInitRocksDBFlags(options);
  SetLogPrefix(options, log_prefix);
  options->create_if_missing = true;
  options->disableDataSync = true;
  options->statistics = statistics;
  options->info_log_level = YBRocksDBLogger::ConvertToRocksDBLogLevel(FLAGS_minloglevel);
  options->initial_seqno = FLAGS_initial_seqno;
  options->boundary_extractor = DocBoundaryValuesExtractorInstance();
  options->compaction_measure_io_stats = FLAGS_rocksdb_compaction_measure_io_stats;
  options->memory_monitor = tablet_options.memory_monitor;
  if (FLAGS_db_write_buffer_size != -1) {
    options->write_buffer_size = FLAGS_db_write_buffer_size;
  } else {
    options->write_buffer_size = FLAGS_memstore_size_mb * 1_MB;
  }
  options->env = tablet_options.rocksdb_env;
  options->checkpoint_env = rocksdb::Env::Default();
  static PriorityThreadPool priority_thread_pool_for_compactions_and_flushes(
      FLAGS_priority_thread_pool_size);
  options->priority_thread_pool_for_compactions_and_flushes =
      &priority_thread_pool_for_compactions_and_flushes;

  if (FLAGS_num_reserved_small_compaction_threads != -1) {
    options->num_reserved_small_compaction_threads = FLAGS_num_reserved_small_compaction_threads;
  }

  options->compression = rocksdb::Snappy_Supported() && FLAGS_enable_ondisk_compression
      ? rocksdb::kSnappyCompression : rocksdb::kNoCompression;

  options->listeners.insert(
      options->listeners.end(), tablet_options.listeners.begin(),
      tablet_options.listeners.end()); // Append listeners

  // Set block cache options.
  rocksdb::BlockBasedTableOptions table_options;
  if (tablet_options.block_cache) {
    table_options.block_cache = tablet_options.block_cache;
    // Cache the bloom filters in the block cache.
    table_options.cache_index_and_filter_blocks = true;
  } else {
    table_options.no_block_cache = true;
    table_options.cache_index_and_filter_blocks = false;
  }
  table_options.block_size = FLAGS_db_block_size_bytes;
  table_options.filter_block_size = FLAGS_db_filter_block_size_bytes;
  table_options.index_block_size = FLAGS_db_index_block_size_bytes;
  table_options.min_keys_per_index_block = FLAGS_db_min_keys_per_index_block;

  // Set our custom bloom filter that is docdb aware.
  if (FLAGS_use_docdb_aware_bloom_filter) {
    const auto filter_block_size_bits = table_options.filter_block_size * 8;
    table_options.filter_policy = std::make_unique<const DocDbAwareV3FilterPolicy>(
        filter_block_size_bits, options->info_log.get());
    table_options.supported_filter_policies =
        std::make_shared<rocksdb::BlockBasedTableOptions::FilterPoliciesMap>();
    AddSupportedFilterPolicy(std::make_shared<const DocDbAwareHashedComponentsFilterPolicy>(
            filter_block_size_bits, options->info_log.get()), &table_options);
    AddSupportedFilterPolicy(std::make_shared<const DocDbAwareV2FilterPolicy>(
            filter_block_size_bits, options->info_log.get()), &table_options);
  }

  if (FLAGS_use_multi_level_index) {
    table_options.index_type = rocksdb::IndexType::kMultiLevelBinarySearch;
  } else {
    table_options.index_type = rocksdb::IndexType::kBinarySearch;
  }

  options->table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

  // Compaction related options.

  // Enable universal style compactions.
  bool compactions_enabled = !FLAGS_rocksdb_disable_compactions;
  options->compaction_style = compactions_enabled
    ? rocksdb::CompactionStyle::kCompactionStyleUniversal
    : rocksdb::CompactionStyle::kCompactionStyleNone;
  // Set the number of levels to 1.
  options->num_levels = 1;

  AutoInitRocksDBFlags(options);
  if (compactions_enabled) {
    options->level0_file_num_compaction_trigger = FLAGS_rocksdb_level0_file_num_compaction_trigger;
    options->level0_slowdown_writes_trigger = max_if_negative(
        FLAGS_rocksdb_level0_slowdown_writes_trigger);
    options->level0_stop_writes_trigger = max_if_negative(FLAGS_rocksdb_level0_stop_writes_trigger);
    // This determines the algo used to compute which files will be included. The "total size" based
    // computation compares the size of every new file with the sum of all files included so far.
    options->compaction_options_universal.stop_style =
        rocksdb::CompactionStopStyle::kCompactionStopStyleTotalSize;
    options->compaction_options_universal.size_ratio =
        FLAGS_rocksdb_universal_compaction_size_ratio;
    options->compaction_options_universal.always_include_size_threshold =
        FLAGS_rocksdb_universal_compaction_always_include_size_threshold;
    options->compaction_options_universal.min_merge_width =
        FLAGS_rocksdb_universal_compaction_min_merge_width;
    options->compaction_size_threshold_bytes = FLAGS_rocksdb_compaction_size_threshold_bytes;
    if (FLAGS_rocksdb_compact_flush_rate_limit_bytes_per_sec > 0) {
      options->rate_limiter.reset(
          rocksdb::NewGenericRateLimiter(FLAGS_rocksdb_compact_flush_rate_limit_bytes_per_sec));
    }
  } else {
    options->level0_slowdown_writes_trigger = std::numeric_limits<int>::max();
    options->level0_stop_writes_trigger = std::numeric_limits<int>::max();
  }

  uint64_t max_file_size_for_compaction = FLAGS_rocksdb_max_file_size_for_compaction;
  if (max_file_size_for_compaction != 0) {
    options->max_file_size_for_compaction = max_file_size_for_compaction;
  }

  options->max_write_buffer_number = FLAGS_rocksdb_max_write_buffer_number;

  options->memtable_factory = std::make_shared<rocksdb::SkipListFactory>(
      0 /* lookahead */, rocksdb::ConcurrentWrites::kFalse);

  options->iterator_replacer = std::make_shared<rocksdb::IteratorReplacer>(&WrapIterator);
}

void SetLogPrefix(rocksdb::Options* options, const std::string& log_prefix) {
  options->log_prefix = log_prefix;
  options->info_log = std::make_shared<YBRocksDBLogger>(options->log_prefix);
}

namespace {

// Helper class for RocksDBPatcher.
class RocksDBPatcherHelper {
 public:
  explicit RocksDBPatcherHelper(rocksdb::VersionSet* version_set)
      : version_set_(version_set), cfd_(version_set->GetColumnFamilySet()->GetDefault()),
        delete_edit_(cfd_), add_edit_(cfd_) {
  }

  int Levels() const {
    return cfd_->NumberLevels();
  }

  const std::vector<rocksdb::FileMetaData*>& LevelFiles(int level) {
    return cfd_->current()->storage_info()->LevelFiles(level);
  }

  template <class F>
  void IterateFiles(const F& f) {
    for (int level = 0; level < Levels(); ++level) {
      for (const auto* file : LevelFiles(level)) {
        f(level, *file);
      }
    }
  }

  void ModifyFile(int level, const rocksdb::FileMetaData& fmd) {
    delete_edit_->DeleteFile(level, fmd.fd.GetNumber());
    add_edit_->AddCleanedFile(level, fmd);
  }

  rocksdb::VersionEdit& Edit() {
    return *add_edit_;
  }

  CHECKED_STATUS Apply(
      const rocksdb::Options& options, const rocksdb::ImmutableCFOptions& imm_cf_options) {
    if (!delete_edit_.modified() && !add_edit_.modified()) {
      return Status::OK();
    }

    rocksdb::MutableCFOptions mutable_cf_options(options, imm_cf_options);
    {
      rocksdb::InstrumentedMutex mutex;
      rocksdb::InstrumentedMutexLock lock(&mutex);
      for (auto* edit : {&delete_edit_, &add_edit_}) {
        if (edit->modified()) {
          RETURN_NOT_OK(version_set_->LogAndApply(cfd_, mutable_cf_options, edit->get(), &mutex));
        }
      }
    }

    return Status::OK();
  }

 private:
  class TrackedEdit {
   public:
    explicit TrackedEdit(rocksdb::ColumnFamilyData* cfd) {
      edit_.SetColumnFamily(cfd->GetID());
    }

    rocksdb::VersionEdit* get() {
      modified_ = true;
      return &edit_;
    }

    rocksdb::VersionEdit* operator->() {
      return get();
    }

    rocksdb::VersionEdit& operator*() {
      return *get();
    }

    bool modified() const {
      return modified_;
    }

   private:
    rocksdb::VersionEdit edit_;
    bool modified_ = false;
  };

  rocksdb::VersionSet* version_set_;
  rocksdb::ColumnFamilyData* cfd_;
  TrackedEdit delete_edit_;
  TrackedEdit add_edit_;
};

} // namespace

class RocksDBPatcher::Impl {
 public:
  Impl(const std::string& dbpath, const rocksdb::Options& options)
      : options_(SanitizeOptions(dbpath, &comparator_, options)),
        imm_cf_options_(options_),
        env_options_(options_),
        cf_options_(options_),
        version_set_(dbpath, &options_, env_options_, block_cache_.get(), &write_buffer_, nullptr) {
    cf_options_.comparator = comparator_.user_comparator();
  }

  CHECKED_STATUS Load() {
    std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
    column_families.emplace_back("default", cf_options_);
    return version_set_.Recover(column_families);
  }

  CHECKED_STATUS SetHybridTimeFilter(HybridTime value) {
    RocksDBPatcherHelper helper(&version_set_);

    helper.IterateFiles([&helper, value](int level, const rocksdb::FileMetaData& file) {
      if (!file.largest.user_frontier) {
        return;
      }
      if (down_cast<ConsensusFrontier&>(*file.largest.user_frontier).hybrid_time() <= value) {
        return;
      }
      rocksdb::FileMetaData fmd = file;
      down_cast<ConsensusFrontier&>(*fmd.largest.user_frontier).set_hybrid_time_filter(value);
      helper.ModifyFile(level, fmd);
    });

    return helper.Apply(options_, imm_cf_options_);
  }

  CHECKED_STATUS ModifyFlushedFrontier(const ConsensusFrontier& frontier) {
    RocksDBPatcherHelper helper(&version_set_);

    docdb::ConsensusFrontier final_frontier = frontier;

    auto* existing_frontier = down_cast<docdb::ConsensusFrontier*>(version_set_.FlushedFrontier());
    if (existing_frontier) {
      final_frontier.set_history_cutoff(existing_frontier->history_cutoff());
    }

    helper.Edit().ModifyFlushedFrontier(
        final_frontier.Clone(), rocksdb::FrontierModificationMode::kForce);

    helper.IterateFiles([&helper](int level, rocksdb::FileMetaData fmd) {
      bool modified = false;
      for (auto* user_frontier : {&fmd.smallest.user_frontier, &fmd.largest.user_frontier}) {
        if (!*user_frontier) {
          continue;
        }
        auto& consensus_frontier = down_cast<ConsensusFrontier&>(**user_frontier);
        if (!consensus_frontier.op_id().empty()) {
          consensus_frontier.set_op_id(OpId());
          modified = true;
        }
      }
      if (modified) {
        helper.ModifyFile(level, fmd);
      }
    });

    return helper.Apply(options_, imm_cf_options_);
  }

 private:
  const rocksdb::InternalKeyComparator comparator_{rocksdb::BytewiseComparator()};
  rocksdb::WriteBuffer write_buffer_{1_KB};
  std::shared_ptr<rocksdb::Cache> block_cache_{rocksdb::NewLRUCache(1_MB)};

  rocksdb::Options options_;
  rocksdb::ImmutableCFOptions imm_cf_options_;
  rocksdb::EnvOptions env_options_;
  rocksdb::ColumnFamilyOptions cf_options_;
  rocksdb::VersionSet version_set_;
};

RocksDBPatcher::RocksDBPatcher(const std::string& dbpath, const rocksdb::Options& options)
    : impl_(new Impl(dbpath, options)) {
}

RocksDBPatcher::~RocksDBPatcher() {
}

Status RocksDBPatcher::Load() {
  return impl_->Load();
}

Status RocksDBPatcher::SetHybridTimeFilter(HybridTime value) {
  return impl_->SetHybridTimeFilter(value);
}

Status RocksDBPatcher::ModifyFlushedFrontier(const ConsensusFrontier& frontier) {
  return impl_->ModifyFlushedFrontier(frontier);
}

Status ForceRocksDBCompact(rocksdb::DB* db) {
  RETURN_NOT_OK_PREPEND(
      db->CompactRange(rocksdb::CompactRangeOptions(), /* begin = */ nullptr, /* end = */ nullptr),
      "Compact range failed:");
  return Status::OK();
}

}  // namespace docdb
}  // namespace yb
