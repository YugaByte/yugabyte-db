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

#ifndef YB_DOCDB_DOCDB_ROCKSDB_UTIL_H_
#define YB_DOCDB_DOCDB_ROCKSDB_UTIL_H_

#include <boost/optional.hpp>

#include "yb/common/read_hybrid_time.h"
#include "yb/common/transaction.h"

#include "yb/docdb/bounded_rocksdb_iterator.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/value.h"

#include "yb/rocksdb/cache.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/options.h"

#include "yb/tablet/tablet_options.h"

#include "yb/util/slice.h"

namespace yb {
namespace docdb {

class IntentAwareIterator;

// See to a rocksdb point that is at least sub_doc_key.
// If the iterator is already positioned far enough, does not perform a seek.
void SeekForward(const rocksdb::Slice& slice, rocksdb::Iterator *iter);

void SeekForward(const KeyBytes& key_bytes, rocksdb::Iterator *iter);

// When we replace HybridTime::kMin in the end of seek key, next seek will skip older versions of
// this key, but will not skip any subkeys in its subtree. If the iterator is already positioned far
// enough, does not perform a seek.
void SeekPastSubKey(const Slice& key, rocksdb::Iterator* iter);

// Seek out of the given SubDocKey. For efficiency, the method that takes a non-const KeyBytes
// pointer avoids memory allocation by using the KeyBytes buffer to prepare the key to seek to by
// appending an extra byte. The appended byte is removed when the method returns.
void SeekOutOfSubKey(KeyBytes* key_bytes, rocksdb::Iterator* iter);

KeyBytes AppendDocHt(const Slice& key, const DocHybridTime& doc_ht);

// A wrapper around the RocksDB seek operation that uses Next() up to the configured number of
// times to avoid invalidating iterator state. In debug mode it also allows printing detailed
// information about RocksDB seeks.
void PerformRocksDBSeek(
    rocksdb::Iterator *iter,
    const rocksdb::Slice &seek_key,
    const char* file_name,
    int line);

// TODO: is there too much overhead in passing file name and line here in release mode?
#define ROCKSDB_SEEK(iter, key) \
  do { \
    PerformRocksDBSeek((iter), (key), __FILE__, __LINE__); \
  } while (0)

enum class BloomFilterMode {
  USE_BLOOM_FILTER,
  DONT_USE_BLOOM_FILTER,
};

// It is only allowed to use bloom filters on scans within the same hashed components of the key,
// because BloomFilterAwareIterator relies on it and ignores SST file completely if there are no
// keys with the same hashed components as key specified for seek operation.
// Note: bloom_filter_mode should be specified explicitly to avoid using it incorrectly by default.
// user_key_for_filter is used with BloomFilterMode::USE_BLOOM_FILTER to exclude SST files which
// have the same hashed components as (Sub)DocKey encoded in user_key_for_filter.
BoundedRocksDbIterator CreateRocksDBIterator(
    rocksdb::DB* rocksdb,
    const KeyBounds* docdb_key_bounds,
    BloomFilterMode bloom_filter_mode,
    const boost::optional<const Slice>& user_key_for_filter,
    const rocksdb::QueryId query_id,
    std::shared_ptr<rocksdb::ReadFileFilter> file_filter = nullptr,
    const Slice* iterate_upper_bound = nullptr);

// Values and transactions committed later than high_ht can be skipped, so we won't spend time
// for re-requesting pending transaction status if we already know it wasn't committed at high_ht.
std::unique_ptr<IntentAwareIterator> CreateIntentAwareIterator(
    const DocDB& doc_db,
    BloomFilterMode bloom_filter_mode,
    const boost::optional<const Slice>& user_key_for_filter,
    const rocksdb::QueryId query_id,
    const TransactionOperationContextOpt& transaction_context,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time,
    std::shared_ptr<rocksdb::ReadFileFilter> file_filter = nullptr,
    const Slice* iterate_upper_bound = nullptr);

// Request RocksDB compaction and wait until it completes.
CHECKED_STATUS ForceRocksDBCompact(rocksdb::DB* db);

// Initialize the RocksDB 'options'.
// The 'statistics' object provided by the caller will be used by RocksDB to maintain the stats for
// the tablet.
void InitRocksDBOptions(
    rocksdb::Options* options, const std::string& log_prefix,
    const std::shared_ptr<rocksdb::Statistics>& statistics,
    const tablet::TabletOptions& tablet_options);

// Sets logs prefix for RocksDB options. This will also reinitialize options->info_log.
void SetLogPrefix(rocksdb::Options* options, const std::string& log_prefix);

// Class to edit RocksDB manifest w/o fully loading DB into memory.
class RocksDBPatcher {
 public:
  explicit RocksDBPatcher(const std::string& dbpath, const rocksdb::Options& options);
  ~RocksDBPatcher();

  // Loads DB into patcher.
  CHECKED_STATUS Load();

  // Set hybrid time filter for DB.
  CHECKED_STATUS SetHybridTimeFilter(HybridTime value);

  // Modify flushed frontier and clean up smallest/largest op id in per-SST file metadata.
  CHECKED_STATUS ModifyFlushedFrontier(const ConsensusFrontier& frontier);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace docdb
}  // namespace yb

#endif  // YB_DOCDB_DOCDB_ROCKSDB_UTIL_H_
