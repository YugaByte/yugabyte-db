// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
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
// Currently we support two types of tables: plain table and block-based table.
//   1. Block-based table: this is the default table type that we inherited from
//      LevelDB, which was designed for storing data in hard disk or flash
//      device.
//   2. Plain table: it is one of RocksDB's SST file format optimized
//      for low query latency on pure-memory or really low-latency media.
//
// A tutorial of rocksdb table formats is available here:
//   https://github.com/facebook/rocksdb/wiki/A-Tutorial-of-RocksDB-SST-formats
//
// Example code is also available
//   https://github.com/facebook/rocksdb/wiki/A-Tutorial-of-RocksDB-SST-formats#wiki-examples

#ifndef YB_ROCKSDB_TABLE_H
#define YB_ROCKSDB_TABLE_H

#include <memory>
#include <string>
#include <unordered_map>

#include "yb/rocksdb/env.h"
#include "yb/rocksdb/iterator.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/immutable_options.h"
#include "yb/rocksdb/status.h"
#include "yb/util/size_literals.h"

namespace rocksdb {

// -- Block-based Table
class FlushBlockPolicyFactory;
struct TableReaderOptions;
struct TableBuilderOptions;
class TableBuilder;
class TableReader;
class WritableFileWriter;
struct EnvOptions;
struct Options;

using std::unique_ptr;
using namespace yb::size_literals;

enum ChecksumType : char {
  kNoChecksum = 0x0,  // not yet supported. Will fail
  kCRC32c = 0x1,
  kxxHash = 0x2,
};

YB_DEFINE_ENUM(IndexType,
  // A space efficient index block that is optimized for binary-search-based index.
  (kBinarySearch)

  // The hash index, if enabled, will do the hash lookup when `Options.prefix_extractor` is
  // provided.
  (kHashSearch)

  // Index is partitioned into blocks based on size according to TableOptions::index_block_size
  // and TableOptions::min_keys_per_index_block. To guarantee that index blocks fit required size,
  // index could have multiple levels. All levels are binary-search-based indexes.
  (kMultiLevelBinarySearch)
);

// For advanced user only
struct BlockBasedTableOptions {
  // @flush_block_policy_factory creates the instances of flush block policy.
  // which provides a configurable way to determine when to flush a block in
  // the block based tables.  If not set, table builder will use the default
  // block flush policy, which cut blocks by block size (please refer to
  // `FlushBlockBySizePolicy`).
  std::shared_ptr<FlushBlockPolicyFactory> flush_block_policy_factory;

  // TODO(kailiu) Temporarily disable this feature by making the default value
  // to be false.
  //
  // Indicating if we'd put index/filter blocks to the block cache.
  // If not specified, each "table reader" object will pre-load index/filter
  // block during table initialization.
  // Note: Fixed-size bloom filter data blocks are never pre-loaded.
  bool cache_index_and_filter_blocks = false;

  IndexType index_type = IndexType::kMultiLevelBinarySearch;

  // Influence the behavior when kHashSearch is used.
  // if false, stores a precise prefix to block range mapping
  // if true, does not store prefix and allows prefix hash collision
  // (less memory consumption)
  bool hash_index_allow_collision = true;

  // Use the specified checksum type. Newly created table files will be
  // protected with this checksum type. Old table files will still be readable,
  // even though they have different checksum type.
  ChecksumType checksum = kCRC32c;

  // Disable block cache. If this is set to true,
  // then no block cache should be used, and the block_cache should
  // point to a nullptr object.
  bool no_block_cache = false;

  // If non-NULL use the specified cache for blocks.
  // If NULL, rocksdb will automatically create and use an 8MB internal cache.
  std::shared_ptr<Cache> block_cache = nullptr;

  // If non-NULL use the specified cache for compressed blocks.
  // If NULL, rocksdb will not use a compressed block cache.
  std::shared_ptr<Cache> block_cache_compressed = nullptr;

  // Approximate size of user data packed per block, in bytes. Note that the
  // block size specified here corresponds to uncompressed data.  The
  // actual size of the unit read from disk may be smaller if
  // compression is enabled.  This parameter can be changed dynamically.
  // It is preferred to set large block size as it reduces the memory requirement
  // for indexing.
  size_t block_size = 4 * 1024;

  // Size of each filter block, in bytes. Only applicable for fixed size filter block.
  size_t filter_block_size = 64 * 1024;

  // This is used to close a block before it reaches the configured
  // 'block_size'. If the percentage of free space in the current block is less
  // than this specified number and adding a new record to the block will
  // exceed the configured block size, then this block will be closed and the
  // new record will be written to the next block.
  int block_size_deviation = 10;

  // Number of keys between restart points for delta encoding of keys.
  // This parameter can be changed dynamically.  Most clients should
  // leave this parameter alone.  The minimum value allowed is 1.  Any smaller
  // value will be silently overwritten with 1.
  int block_restart_interval = 16;

  // Same as block_restart_interval but used for the index block.
  int index_block_restart_interval = 1;

  // Index block size for sharded index. Applied to data index when kMultiLevelBinarySearch is used.
  size_t index_block_size = 4_KB;

  // For kMultiLevelBinarySearch: minimum number of keys to put in index block. This constraint is
  // used to avoid too many index levels in case we have large keys.
  size_t min_keys_per_index_block = 64;

  // Use delta encoding to compress keys in blocks.
  // Iterator::PinData() requires this option to be disabled.
  //
  // Default: true
  bool use_delta_encoding = true;

  // If non-nullptr, use the specified filter policy for new SST files to reduce disk reads.
  // Many applications will benefit from passing the result of
  // NewBloomFilterPolicy() here.
  typedef std::shared_ptr<const FilterPolicy> FilterPolicyPtr;
  FilterPolicyPtr filter_policy = nullptr;

  // Other filter policies we support for backward compatibility to be able to use bloom filters
  // for existing SST files.
  // Note: wrapped into std::shared_ptr for options_helper to be compilable. Without wrapping there
  // is an error: offset of on non-standard-layout type 'struct BlockBasedTableOptions'.
  typedef std::unordered_map<std::string, FilterPolicyPtr> FilterPoliciesMap;
  std::shared_ptr<FilterPoliciesMap> supported_filter_policies;

  // If true, place whole keys in the filter (not just prefixes).
  // This must generally be true for gets to be efficient.
  bool whole_key_filtering = true;

  // If true, block will not be explicitly flushed to disk during building
  // a SstTable. Instead, buffer in WritableFileWriter will take
  // care of the flushing when it is full.
  //
  // On Windows, this option helps a lot when unbuffered I/O
  // (allow_os_buffer = false) is used, since it avoids small
  // unbuffered disk write.
  //
  // User may also adjust writable_file_max_buffer_size to optimize disk I/O
  // size.
  //
  // Default: false
  bool skip_table_builder_flush = false;

  // We currently have three versions:
  // 0 -- This version is currently written out by all RocksDB's versions by
  // default.  Can be read by really old RocksDB's. Doesn't support changing
  // checksum (default is CRC32).
  // 1 -- Can be read by RocksDB's versions since 3.0. Supports non-default
  // checksum, like xxHash. It is written by RocksDB when
  // BlockBasedTableOptions::checksum is something other than kCRC32c. (version
  // 0 is silently upconverted)
  // 2 -- Can be read by RocksDB's versions since 3.10. Changes the way we
  // encode compressed blocks with LZ4, BZip2 and Zlib compression. If you
  // don't plan to run RocksDB before version 3.10, you should probably use
  // this.
  // This option only affects newly written tables. When reading exising tables,
  // the information about version is read from the footer.
  uint32_t format_version = 2;
};

// Table Properties that are specific to block-based table properties.
struct BlockBasedTablePropertyNames {
  // value of this property is a fixed int32 number.
  static const char kIndexType[];
  // number of index levels for multi-level index, int32.
  static const char kNumIndexLevels[];
  // value is "1" for true and "0" for false.
  static const char kWholeKeyFiltering[];
  // value is "1" for true and "0" for false.
  static const char kPrefixFiltering[];
};

// Create default block based table factory.
extern TableFactory* NewBlockBasedTableFactory(
    const BlockBasedTableOptions& table_options = BlockBasedTableOptions());

#ifndef ROCKSDB_LITE

enum EncodingType : char {
  // Always write full keys without any special encoding.
  kPlain,
  // Find opportunity to write the same prefix once for multiple rows.
  // In some cases, when a key follows a previous key with the same prefix,
  // instead of writing out the full key, it just writes out the size of the
  // shared prefix, as well as other bytes, to save some bytes.
  //
  // When using this option, the user is required to use the same prefix
  // extractor to make sure the same prefix will be extracted from the same key.
  // The Name() value of the prefix extractor will be stored in the file. When
  // reopening the file, the name of the options.prefix_extractor given will be
  // bitwise compared to the prefix extractors stored in the file. An error
  // will be returned if the two don't match.
  kPrefix,
};

// Table Properties that are specific to plain table properties.
struct PlainTablePropertyNames {
  static const char kPrefixExtractorName[];
  static const char kEncodingType[];
  static const char kBloomVersion[];
  static const char kNumBloomBlocks[];
};

const uint32_t kPlainTableVariableLength = 0;

struct PlainTableOptions {
  // @user_key_len: plain table has optimization for fix-sized keys, which can
  //                be specified via user_key_len.  Alternatively, you can pass
  //                `kPlainTableVariableLength` if your keys have variable
  //                lengths.
  uint32_t user_key_len = kPlainTableVariableLength;

  // @bloom_bits_per_key: the number of bits used for bloom filer per prefix.
  //                      You may disable it by passing a zero.
  int bloom_bits_per_key = 10;

  // @hash_table_ratio: the desired utilization of the hash table used for
  //                    prefix hashing.
  //                    hash_table_ratio = number of prefixes / #buckets in the
  //                    hash table
  double hash_table_ratio = 0.75;

  // @index_sparseness: inside each prefix, need to build one index record for
  //                    how many keys for binary search inside each hash bucket.
  //                    For encoding type kPrefix, the value will be used when
  //                    writing to determine an interval to rewrite the full
  //                    key. It will also be used as a suggestion and satisfied
  //                    when possible.
  size_t index_sparseness = 16;

  // @huge_page_tlb_size: if <=0, allocate hash indexes and blooms from malloc.
  //                      Otherwise from huge page TLB. The user needs to
  //                      reserve huge pages for it to be allocated, like:
  //                          sysctl -w vm.nr_hugepages=20
  //                      See linux doc Documentation/vm/hugetlbpage.txt
  size_t huge_page_tlb_size = 0;

  // @encoding_type: how to encode the keys. See enum EncodingType above for
  //                 the choices. The value will determine how to encode keys
  //                 when writing to a new SST file. This value will be stored
  //                 inside the SST file which will be used when reading from
  //                 the file, which makes it possible for users to choose
  //                 different encoding type when reopening a DB. Files with
  //                 different encoding types can co-exist in the same DB and
  //                 can be read.
  EncodingType encoding_type = kPlain;

  // @full_scan_mode: mode for reading the whole file one record by one without
  //                  using the index.
  bool full_scan_mode = false;

  // @store_index_in_file: compute plain table index and bloom filter during
  //                       file building and store it in file. When reading
  //                       file, index will be mmaped instead of recomputation.
  bool store_index_in_file = false;
};

// -- Plain Table with prefix-only seek
// For this factory, you need to set Options.prefix_extrator properly to make it
// work. Look-up will starts with prefix hash lookup for key prefix. Inside the
// hash bucket found, a binary search is executed for hash conflicts. Finally,
// a linear search is used.

extern TableFactory* NewPlainTableFactory(const PlainTableOptions& options =
                                              PlainTableOptions());

#endif  // ROCKSDB_LITE

class RandomAccessFileReader;

// A base class for table factories.
class TableFactory {
 public:
  virtual ~TableFactory() {}

  // The type of the table.
  //
  // The client of this package should switch to a new name whenever
  // the table format implementation changes.
  //
  // Names starting with "rocksdb." are reserved and should not be used
  // by any clients of this package.
  virtual const char* Name() const = 0;

  // Returns a Table object table that can fetch data from file specified
  // in parameter file. It's the caller's responsibility to make sure
  // file is in the correct format.
  //
  // NewTableReader() is called in three places:
  // (1) TableCache::FindTable() calls the function when table cache miss
  //     and cache the table object returned.
  // (2) SstFileReader (for SST Dump) opens the table and dump the table
  //     contents using the interator of the table.
  // (3) DBImpl::AddFile() calls this function to read the contents of
  //     the sst file it's attempting to add
  //
  // table_reader_options is a TableReaderOptions which contain all the
  //    needed parameters and configuration to open the table.
  // base_file is a file handler to handle the base file for the table.
  // base_file_size is the physical file size of the base file.
  // table_reader is the output table reader.
  virtual Status NewTableReader(
      const TableReaderOptions& table_reader_options,
      unique_ptr<RandomAccessFileReader>&& base_file, uint64_t base_file_size,
      unique_ptr<TableReader>* table_reader) const = 0;

  // Whether SST split into metadata and data file(s) is supported for writing.
  // There is a AdaptiveTableFactory inheriting common TableFactory interface. AdaptiveTableFactory
  // can delegate reads to one type of table factory and writes to another type of table factory
  // and in that case it can only support split SST either for read or write.
  virtual bool IsSplitSstForWriteSupported() const = 0;

  // Return a table builder to write to file(s) for this table type.
  // Particular table factory implementations can support either writing whole SST to a single file
  // (passed in base_file parameter, data_file should be nullptr in that case) or support separate
  // files for data (data_file) and metadata (base_file).
  //
  // It is called in several places:
  // (1) When flushing memtable to a level-0 output file, it creates a table
  //     builder (In DBImpl::WriteLevel0Table(), by calling BuildTable())
  // (2) During compaction, it gets the builder for writing compaction output
  //     files in DBImpl::OpenCompactionOutputFile().
  // (3) When recovering from transaction logs, it creates a table builder to
  //     write to a level-0 output file (In DBImpl::WriteLevel0TableForRecovery,
  //     by calling BuildTable())
  // (4) When running Repairer, it creates a table builder to convert logs to
  //     SST files (In Repairer::ConvertLogToTable() by calling BuildTable())
  //
  // ImmutableCFOptions is a subset of Options that can not be altered.
  // Multiple configured can be acceseed from there, including and not limited
  // to compression options. file is a handle of a writable file.
  // It is the caller's responsibility to keep the file open and close the file
  // after closing the table builder. compression_type is the compression type
  // to use in this table.
  virtual TableBuilder* NewTableBuilder(
      const TableBuilderOptions& table_builder_options,
      uint32_t column_family_id, WritableFileWriter* base_file,
      WritableFileWriter* data_file = nullptr) const = 0;

  // Sanitizes the specified DB Options and ColumnFamilyOptions.
  //
  // If the function cannot find a way to sanitize the input DB Options,
  // a non-ok Status will be returned.
  virtual Status SanitizeOptions(
      const DBOptions& db_opts,
      const ColumnFamilyOptions& cf_opts) const = 0;

  // Return a string that contains printable format of table configurations.
  // RocksDB prints configurations at DB Open().
  virtual std::string GetPrintableTableOptions() const = 0;

  // Returns the raw pointer of the table options that is used by this
  // TableFactory, or nullptr if this function is not supported.
  // Since the return value is a raw pointer, the TableFactory owns the
  // pointer and the caller should not delete the pointer.
  //
  // In certan case, it is desirable to alter the underlying options when the
  // TableFactory is not used by any open DB by casting the returned pointer
  // to the right class.   For instance, if BlockBasedTableFactory is used,
  // then the pointer can be casted to BlockBasedTableOptions.
  //
  // Note that changing the underlying TableFactory options while the
  // TableFactory is currently used by any open DB is undefined behavior.
  // Developers should use DB::SetOption() instead to dynamically change
  // options while the DB is open.
  virtual void* GetOptions() { return nullptr; }

  // Returns SST file filter for pruning out files which doesn't contain some part of user_key.
  // It should be in sync with FilterPolicy used for bloom filter construction. For example,
  // file filter should only consider hashed components of the key when using with
  // DocDbAwareFilterPolicy and HashedComponentsExtractor.
  virtual std::shared_ptr<TableAwareReadFileFilter> NewTableAwareReadFileFilter(
      const ReadOptions &read_options, const Slice &user_key) const { return nullptr; }
};

#ifndef ROCKSDB_LITE
// Create a special table factory that can open either of the supported
// table formats, based on setting inside the SST files. It should be used to
// convert a DB from one table format to another.
// @table_factory_to_write: the table factory used when writing to new files.
// @block_based_table_factory:  block based table factory to use. If NULL, use
//                              a default one.
// @plain_table_factory: plain table factory to use. If NULL, use a default one.
extern TableFactory* NewAdaptiveTableFactory(
    std::shared_ptr<TableFactory> table_factory_to_write = nullptr,
    std::shared_ptr<TableFactory> block_based_table_factory = nullptr,
    std::shared_ptr<TableFactory> plain_table_factory = nullptr);

#endif  // ROCKSDB_LITE

}  // namespace rocksdb

#endif  // YB_ROCKSDB_TABLE_H
