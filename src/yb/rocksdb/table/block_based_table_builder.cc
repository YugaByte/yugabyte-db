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

#include "yb/rocksdb/table/block_based_table_builder.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "yb/rocksdb/db/dbformat.h"

#include "yb/rocksdb/cache.h"
#include "yb/rocksdb/comparator.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/filter_policy.h"
#include "yb/rocksdb/flush_block_policy.h"
#include "yb/rocksdb/table.h"

#include "yb/rocksdb/table/block.h"
#include "yb/rocksdb/table/block_based_table_internal.h"
#include "yb/rocksdb/table/block_builder.h"
#include "yb/rocksdb/table/filter_block.h"
#include "yb/rocksdb/table/block_based_filter_block.h"
#include "yb/rocksdb/table/block_based_table_factory.h"
#include "yb/rocksdb/table/fixed_size_filter_block.h"
#include "yb/rocksdb/table/full_filter_block.h"
#include "yb/rocksdb/table/format.h"
#include "yb/rocksdb/table/index_builder.h"
#include "yb/rocksdb/table/meta_blocks.h"
#include "yb/rocksdb/table/table_builder.h"

#include "yb/rocksdb/util/coding.h"
#include "yb/rocksdb/util/compression.h"
#include "yb/rocksdb/util/crc32c.h"
#include "yb/rocksdb/util/stop_watch.h"
#include "yb/rocksdb/util/xxhash.h"

#include "yb/util/mem_tracker.h"
#include "yb/util/string_util.h"

#include "yb/gutil/macros.h"

namespace rocksdb {

extern const char kHashIndexPrefixesBlock[];
extern const char kHashIndexPrefixesMetadataBlock[];

typedef FilterPolicy::FilterType FilterType;

// Without anonymous namespace here, we fail the warning -Wmissing-prototypes
namespace {

FilterType GetFilterType(const BlockBasedTableOptions& table_opt) {
  std::shared_ptr<const FilterPolicy> policy(table_opt.filter_policy);
  return policy != nullptr ? policy->GetFilterType() : FilterType::kNoFilter;
}

// Create a filter builder based on its type.
FilterBlockBuilder* CreateFilterBlockBuilder(const ImmutableCFOptions& opt,
    const BlockBasedTableOptions& table_opt, FilterType filter_type) {
  switch (filter_type) {
    case FilterType::kBlockBasedFilter:
      return new BlockBasedFilterBlockBuilder(opt.prefix_extractor, table_opt);
    case FilterType::kFixedSizeFilter:
      return new FixedSizeFilterBlockBuilder(opt.prefix_extractor, table_opt);
    case FilterType::kFullFilter:
      return new FullFilterBlockBuilder(opt.prefix_extractor,
                                        table_opt.whole_key_filtering,
                                        table_opt.filter_policy->GetFilterBitsBuilder());
    case FilterType::kNoFilter:
      return nullptr;
  }
  RLOG(InfoLogLevel::FATAL_LEVEL, opt.info_log, "Corrupted filter_type: %d", filter_type);
  assert(false);
  return nullptr;
}

bool GoodCompressionRatio(size_t compressed_size, size_t raw_size) {
  // Check to see if compressed less than 12.5%
  return compressed_size < raw_size - (raw_size / 8u);
}

// format_version is the block format as defined in include/rocksdb/table.h
Slice CompressBlock(const Slice& raw,
                    const CompressionOptions& compression_options,
                    CompressionType* type, uint32_t format_version,
                    std::string* compressed_output) {
  if (*type == kNoCompression) {
    return raw;
  }

  // Will return compressed block contents if (1) the compression method is
  // supported in this platform and (2) the compression rate is "good enough".
  switch (*type) {
    case kSnappyCompression:
      if (Snappy_Compress(compression_options, raw.cdata(), raw.size(),
                          compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;  // fall back to no compression.
    case kZlibCompression:
      if (Zlib_Compress(
              compression_options,
              GetCompressFormatForVersion(kZlibCompression, format_version),
              raw.cdata(), raw.size(), compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;  // fall back to no compression.
    case kBZip2Compression:
      if (BZip2_Compress(
              compression_options,
              GetCompressFormatForVersion(kBZip2Compression, format_version),
              raw.cdata(), raw.size(), compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;  // fall back to no compression.
    case kLZ4Compression:
      if (LZ4_Compress(
              compression_options,
              GetCompressFormatForVersion(kLZ4Compression, format_version),
              raw.cdata(), raw.size(), compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;  // fall back to no compression.
    case kLZ4HCCompression:
      if (LZ4HC_Compress(
              compression_options,
              GetCompressFormatForVersion(kLZ4HCCompression, format_version),
              raw.cdata(), raw.size(), compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;     // fall back to no compression.
    case kZSTDNotFinalCompression:
      if (ZSTD_Compress(compression_options, raw.cdata(), raw.size(),
                        compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;     // fall back to no compression.
    default: {}  // Do not recognize this compression type
  }

  // Compression method is not supported, or not good compression ratio, so just
  // fall back to uncompressed form.
  *type = kNoCompression;
  return raw;
}

}  // namespace

// kBlockBasedTableMagicNumber was picked by running
//    echo rocksdb.table.block_based | sha1sum
// and taking the leading 64 bits.
// Please note that kBlockBasedTableMagicNumber may also be accessed by other
// .cc files
// for that reason we declare it extern in the header but to get the space
// allocated
// it must be not extern in one place.
const uint64_t kBlockBasedTableMagicNumber = 0x88e241b785f4cff7ull;
// We also support reading and writing legacy block based table format (for
// backwards compatibility)
const uint64_t kLegacyBlockBasedTableMagicNumber = 0xdb4775248b80fb57ull;

// A collector that collects properties of interest to block-based table.
// For now this class looks heavy-weight since we only write one additional
// property.
// But in the foreseeable future, we will add more and more properties that are
// specific to block-based table.
class BlockBasedTableBuilder::BlockBasedTablePropertiesCollector
    : public IntTblPropCollector {
 public:
  explicit BlockBasedTablePropertiesCollector(
      BlockBasedTableBuilder::Rep* rep, IndexType index_type,
      bool whole_key_filtering, bool prefix_filtering)
      : rep_(rep),
        index_type_(index_type),
        whole_key_filtering_(whole_key_filtering),
        prefix_filtering_(prefix_filtering) {}

  virtual Status InternalAdd(const Slice& key, const Slice& value,
                             uint64_t file_size) override {
    // Intentionally left blank. Have no interest in collecting stats for
    // individual key/value pairs.
    return Status::OK();
  }

  Status Finish(UserCollectedProperties* properties) override;

  // The name of the properties collector can be used for debugging purpose.
  const char* Name() const override {
    return "BlockBasedTablePropertiesCollector";
  }

  UserCollectedProperties GetReadableProperties() const override {
    // Intentionally left blank.
    return UserCollectedProperties();
  }

 private:
  BlockBasedTableBuilder::Rep* rep_;
  IndexType index_type_;
  bool whole_key_filtering_;
  bool prefix_filtering_;
};

// Originally following data was stored in BlockBasedTableBuilder::Rep and related to a single SST
// file. Since SST file is now split into two types of files - data file and metadata file,
// all file-related data was moved into dedicated structure for each file.
struct BlockBasedTableBuilder::FileWriterWithOffsetAndCachePrefix {
  // Pointer to file writer. BlockBasedTableBuilder constructor accepts raw pointers to
  // WritableFileWriter and it is responsibility of client code to delete writer instance after
  // usage.
  WritableFileWriter* writer = nullptr;

  // Current offset.
  uint64_t offset = 0;

  // BlockBasedTableBuilder uses compressed block cache passed to BlockBasedTableBuilder constructor
  // inside BlockBasedTableOptions instance. If that cache is set in options (off by default)
  // all data written to files is also put into compressed block cache to reduce number of file
  // read requests later during read operations.
  // File blocks are referred in cache by keys, which are composed from following data (see
  // BlockBasedTableBuilder::InsertBlockInCache):
  // - cache key prefix (unique for each file), generated by GenerateCachePrefix
  // - block offset within a file.
  block_based_table::CacheKeyPrefixBuffer compressed_cache_key_prefix;
};

struct BlockBasedTableBuilder::Rep {
  const ImmutableCFOptions ioptions;
  const BlockBasedTableOptions table_options;
  InternalKeyComparatorPtr internal_comparator;
  // When two file writers are passed to BlockBasedTableBuilder during creation - we
  // use separate writers for data and metadata. In case BlockBasedTableBuilder was created for
  // single file - both data_writer and metadata_writer will actually point to the same structure,
  // so data and metadata will both go into one file. As of 2017-03-10 we support both cases.
  // Actually it is only allowed to pass nullptr as data_writer at BlockBasedTableBuilder::Rep
  // constructor and higher levels in order to support one method signature (with default nullptr
  // value) for handling both cases. At the level of BlockBasedTableBuilder implementation both
  // writers (inside BlockBasedTableBuilder::Rep) are not null and refer to
  // the same file or separate files.
  std::shared_ptr<FileWriterWithOffsetAndCachePrefix> metadata_writer;
  std::shared_ptr<FileWriterWithOffsetAndCachePrefix> data_writer;
  Status status;

  FilterType filter_type;
  std::unique_ptr<FilterBlockBuilder> filter_block_builder;
  BlockBuilder data_block_builder;

  InternalKeySliceTransform internal_prefix_transform;
  const FilterPolicy::KeyTransformer* const filter_key_transformer;
  std::unique_ptr<IndexBuilder> data_index_builder;
  IndexBuilder::IndexBlocks data_index_blocks;
  BlockHandle last_index_block_handle;
  std::unique_ptr<IndexBuilder> filter_index_builder;

  std::string last_key;
  std::string last_filter_key;
  const CompressionType compression_type;
  const CompressionOptions compression_opts;
  TableProperties props;

  bool closed = false;  // Either Finish() or Abandon() has been called.

  BlockHandle data_pending_handle;    // Handle to add to data index block
  BlockHandle filter_pending_handle;  // Handle to add to filter index block

  std::string compressed_output;
  std::unique_ptr<FlushBlockPolicy> flush_block_policy;

  std::vector<std::unique_ptr<IntTblPropCollector>> table_properties_collectors;

  yb::MemTrackerPtr mem_tracker;

  Rep(const ImmutableCFOptions& _ioptions,
      const BlockBasedTableOptions& table_opt,
      const InternalKeyComparatorPtr& icomparator,
      const IntTblPropCollectorFactories& int_tbl_prop_collector_factories,
      uint32_t column_family_id,
      WritableFileWriter* metadata_file,
      WritableFileWriter* data_file,
      const CompressionType _compression_type,
      const CompressionOptions& _compression_opts,
      const bool skip_filters);

  bool is_split_sst() const { return data_writer != metadata_writer; }
};

Status BlockBasedTableBuilder::BlockBasedTablePropertiesCollector::Finish(
    UserCollectedProperties* properties) {
  std::string val;
  PutFixed32(&val, static_cast<uint32_t>(index_type_));
  properties->emplace(BlockBasedTablePropertyNames::kIndexType, val);
  properties->emplace(
      BlockBasedTablePropertyNames::kWholeKeyFiltering,
      ToBlockBasedTablePropertyValue(whole_key_filtering_));
  properties->emplace(
      BlockBasedTablePropertyNames::kPrefixFiltering,
      ToBlockBasedTablePropertyValue(prefix_filtering_));
  val.clear();
  PutFixed32(&val, rep_->data_index_builder->NumLevels());
  properties->emplace(BlockBasedTablePropertyNames::kNumIndexLevels, val);
  return Status::OK();
}

BlockBasedTableBuilder::Rep::Rep(
    const ImmutableCFOptions& _ioptions,
    const BlockBasedTableOptions& table_opt,
    const InternalKeyComparatorPtr& icomparator,
    const IntTblPropCollectorFactories& int_tbl_prop_collector_factories,
    uint32_t column_family_id,
    WritableFileWriter* metadata_file,
    WritableFileWriter* data_file,
    const CompressionType _compression_type,
    const CompressionOptions& _compression_opts,
    const bool skip_filters)
    : ioptions(_ioptions),
      table_options(table_opt),
      internal_comparator(icomparator),
      filter_type(GetFilterType(table_options)),
      filter_block_builder(skip_filters ? nullptr : CreateFilterBlockBuilder(
          _ioptions, table_options, filter_type)),
      data_block_builder(table_options.block_restart_interval,
                 table_options.use_delta_encoding),
      internal_prefix_transform(_ioptions.prefix_extractor),
      filter_key_transformer(table_opt.filter_policy ?
          table_opt.filter_policy->GetKeyTransformer() : nullptr),
      data_index_builder(
          IndexBuilder::CreateIndexBuilder(
              table_options.index_type, internal_comparator.get(), &internal_prefix_transform,
              table_options)),
      filter_index_builder(
          // Prefix_extractor is not used by binary search index which we use for bloom filter
          // blocks indexing.
          IndexBuilder::CreateIndexBuilder(
              IndexType::kBinarySearch, BytewiseComparator(),
              nullptr /* prefix_extractor */, table_options)),
      compression_type(_compression_type),
      compression_opts(_compression_opts),
      flush_block_policy(
          table_options.flush_block_policy_factory->NewFlushBlockPolicy(
              table_options, data_block_builder)) {
  if (_ioptions.mem_tracker) {
    mem_tracker = yb::MemTracker::FindOrCreateTracker(
        "BlockBasedTableBuilder", _ioptions.mem_tracker);
  }

  metadata_writer = std::make_shared<FileWriterWithOffsetAndCachePrefix>();
  metadata_writer->writer = metadata_file;
  if (data_file != nullptr) {
    data_writer = std::make_shared<FileWriterWithOffsetAndCachePrefix>();
    data_writer->writer = data_file;
  } else {
    data_writer = metadata_writer;
  }
  for (auto& collector_factories : int_tbl_prop_collector_factories) {
    table_properties_collectors.emplace_back(
        collector_factories->CreateIntTblPropCollector(column_family_id));
  }
  table_properties_collectors.emplace_back(
      new BlockBasedTablePropertiesCollector(
          this, table_options.index_type, table_options.whole_key_filtering,
          _ioptions.prefix_extractor != nullptr));
}

BlockBasedTableBuilder::BlockBasedTableBuilder(
    const ImmutableCFOptions& ioptions,
    const BlockBasedTableOptions& table_options,
    const InternalKeyComparatorPtr& internal_comparator,
    const IntTblPropCollectorFactories& int_tbl_prop_collector_factories,
    uint32_t column_family_id,
    WritableFileWriter* metadata_file,
    WritableFileWriter* data_file,
    const CompressionType compression_type,
    const CompressionOptions& compression_opts,
    const bool skip_filters) {
  BlockBasedTableOptions sanitized_table_options(table_options);
  if (sanitized_table_options.format_version == 0 &&
      sanitized_table_options.checksum != kCRC32c) {
    RLOG(InfoLogLevel::WARN_LEVEL, ioptions.info_log,
        "Silently converting format_version to 1 because checksum is "
        "non-default");
    // silently convert format_version to 1 to keep consistent with current
    // behavior
    sanitized_table_options.format_version = 1;
  }

  rep_ = new Rep(ioptions, sanitized_table_options, internal_comparator,
                 int_tbl_prop_collector_factories, column_family_id, metadata_file, data_file,
                 compression_type, compression_opts, skip_filters);

  if (rep_->filter_block_builder != nullptr) {
    rep_->filter_block_builder->StartBlock(0);
  }
  if (table_options.block_cache_compressed.get() != nullptr) {
    GenerateCachePrefix(
        table_options.block_cache_compressed.get(), metadata_file->writable_file(),
        &rep_->metadata_writer->compressed_cache_key_prefix);
    if (rep_->is_split_sst()) {
      GenerateCachePrefix(
          table_options.block_cache_compressed.get(), data_file->writable_file(),
          &rep_->metadata_writer->compressed_cache_key_prefix);
    }
  }
}

BlockBasedTableBuilder::~BlockBasedTableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_;
}

void BlockBasedTableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* const r = rep_;
  DCHECK(!r->closed);
  if (!ok()) return;
  if (r->props.num_entries > 0) {
    DCHECK_GT(r->internal_comparator->Compare(key, Slice(r->last_key)), 0);
  }

  const auto should_flush_data = r->flush_block_policy->Update(key, value);
  if (should_flush_data) {
    DCHECK(!r->data_block_builder.empty());
    FlushDataBlock(key);
  }

  if (r->filter_block_builder != nullptr) {
    const Slice user_key = ExtractUserKey(key);
    const Slice filter_key = r->filter_key_transformer ?
        r->filter_key_transformer->Transform(user_key) : user_key;
    if (!filter_key.empty() &&
        (r->props.num_entries == 0 ||
         BytewiseComparator()->Compare(r->last_filter_key, filter_key) != 0)) {
      // No need to insert duplicate keys into Bloom filter.
      if (r->filter_block_builder->ShouldFlush()) {
        FlushFilterBlock(&filter_key);
      }
      r->filter_block_builder->Add(filter_key);
      r->last_filter_key.assign(filter_key.cdata(), filter_key.size());
    }
  }

  r->last_key.assign(key.cdata(), key.size());
  r->data_block_builder.Add(key, value);
  r->props.num_entries++;
  r->props.raw_key_size += key.size();
  r->props.raw_value_size += value.size();

  r->data_index_builder->OnKeyAdded(key);

  NotifyCollectTableCollectorsOnAdd(key, value, r->data_writer->offset,
      r->table_properties_collectors,
      r->ioptions.info_log);
}

void BlockBasedTableBuilder::FlushDataBlock(const Slice& next_block_first_key) {
  Rep* const r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  size_t data_block_size = 0;

  if (!r->data_block_builder.empty()) {
    data_block_size = WriteBlock(&r->data_block_builder, &r->data_pending_handle,
        r->data_writer.get());
  }
  if (!ok()) return;

  if (!r->table_options.skip_table_builder_flush) {
    r->status = r->data_writer->writer->Flush();
  }
  if (!ok()) return;

  if (r->filter_block_builder != nullptr && r->filter_type == FilterType::kBlockBasedFilter) {
    // For FilterType::kBlockBasedFilter separate block of bloom filter is written per data block.
    r->filter_block_builder->StartBlock(r->data_writer->offset);
  }

  r->props.data_size += data_block_size;
  ++r->props.num_data_blocks;
  // Add item to index block.
  // We do not emit the index entry for a block until we have seen the
  // first key for the next data block.  This allows us to use shorter
  // keys in the index block.  For example, consider a block boundary
  // between the keys "the quick brown fox" and "the who".  We can use
  // "the r" as the key for the index block entry since it is >= all
  // entries in the first block and < all entries in subsequent
  // blocks.
  r->data_index_builder->AddIndexEntry(&r->last_key,
      next_block_first_key.empty() ? nullptr : &next_block_first_key,
      r->data_pending_handle);
  while (r->data_index_builder->ShouldFlush()) {
    auto result = r->data_index_builder->FlushNextBlock(
        &r->data_index_blocks, r->last_index_block_handle);
    if (!result.ok()) {
      r->status = result.status();
      return;
    }
    DCHECK(result.get());
    WriteBlock(
        r->data_index_blocks.index_block_contents, &r->last_index_block_handle,
        r->metadata_writer.get());
    if (!ok()) return;
    ++r->props.num_data_index_blocks;
  }
}

void BlockBasedTableBuilder::FlushFilterBlock(const Slice* const next_block_first_filter_key) {
  Rep* const r = rep_;
  assert(!r->closed);
  assert(r->filter_block_builder != nullptr);
  if (!ok()) return;

  const size_t filter_block_size = WriteRawBlock(r->filter_block_builder->Finish(), kNoCompression,
      &r->filter_pending_handle, r->metadata_writer.get());
  if (!ok()) return;

  if (!r->table_options.skip_table_builder_flush) {
    r->status = r->metadata_writer->writer->Flush();
  }
  if (!ok()) return;

  r->props.filter_size += filter_block_size;
  ++r->props.num_filter_blocks;

  const bool is_last_flush = next_block_first_filter_key == nullptr;
  if (!is_last_flush) {
    r->filter_block_builder->StartBlock(0);
  }

  // See explanation in BlockBasedTableBuilder::FlushDataBlock.
  r->filter_index_builder->AddIndexEntry(
      &r->last_filter_key, next_block_first_filter_key, r->filter_pending_handle);
}

size_t BlockBasedTableBuilder::WriteBlock(BlockBuilder* block,
                                          BlockHandle* handle,
                                          FileWriterWithOffsetAndCachePrefix* writer_info) {
  size_t block_size = WriteBlock(block->Finish(), handle, writer_info);
  block->Reset();
  return block_size;
}

size_t BlockBasedTableBuilder::WriteBlock(const Slice& raw_block_contents,
                                          BlockHandle* handle,
                                          FileWriterWithOffsetAndCachePrefix* writer_info) {
  // File format contains a sequence of blocks where each block has:
  //    block_data: uint8[n]
  //    type: uint8
  //    crc: uint32
  assert(ok());
  Rep* r = rep_;

  auto type = r->compression_type;
  Slice block_contents;
  if (raw_block_contents.size() < kCompressionSizeLimit) {
    block_contents =
        CompressBlock(raw_block_contents, r->compression_opts, &type,
                      r->table_options.format_version, &r->compressed_output);
  } else {
    RecordTick(r->ioptions.statistics, NUMBER_BLOCK_NOT_COMPRESSED);
    type = kNoCompression;
    block_contents = raw_block_contents;
  }
  size_t block_size = WriteRawBlock(block_contents, type, handle, writer_info);
  r->compressed_output.clear();
  return block_size;
}

size_t BlockBasedTableBuilder::WriteRawBlock(const Slice& block_contents,
                                             CompressionType type,
                                             BlockHandle* handle,
                                             FileWriterWithOffsetAndCachePrefix* writer_info) {
  Rep* r = rep_;
  StopWatch sw(r->ioptions.env, r->ioptions.statistics, WRITE_RAW_BLOCK_MICROS);
  const auto start_offset = writer_info->offset;
  handle->set_offset(writer_info->offset);
  handle->set_size(block_contents.size());
  r->status = writer_info->writer->Append(block_contents);
  if (r->status.ok()) {
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    char* trailer_without_type = trailer + 1;
    switch (r->table_options.checksum) {
      case kNoChecksum:
        // we don't support no checksum yet
        assert(false);
        // intentional fallthrough in release binary
        // We add a fallthrough annotation in release mode only -- otherwise we get a compile error
        // in debug mode ("fallthrough annotation in unreachable code").
#ifdef NDEBUG
        FALLTHROUGH_INTENDED;
#endif
      case kCRC32c: {
        auto crc = crc32c::Value(block_contents.data(), block_contents.size());
        crc = crc32c::Extend(crc, trailer, 1);  // Extend to cover block type
        EncodeFixed32(trailer_without_type, crc32c::Mask(crc));
        break;
      }
      case kxxHash: {
        void* xxh = XXH32_init(0);
        XXH32_update(xxh, block_contents.data(),
                     static_cast<uint32_t>(block_contents.size()));
        XXH32_update(xxh, trailer, 1);  // Extend  to cover block type
        EncodeFixed32(trailer_without_type, XXH32_digest(xxh));
        break;
      }
    }

    r->status = writer_info->writer->Append(Slice(trailer, kBlockTrailerSize));
    if (r->status.ok()) {
      r->status = InsertBlockInCache(block_contents, type, handle, writer_info);
    }
    if (r->status.ok()) {
      writer_info->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
  return writer_info->offset - start_offset;
}

Status BlockBasedTableBuilder::status() const {
  return rep_->status;
}

static void DeleteCachedBlock(const Slice& key, void* value) {
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}

//
// Make a copy of the block contents and insert into compressed block cache
//
Status BlockBasedTableBuilder::InsertBlockInCache(const Slice& block_contents,
                                                  const CompressionType type,
                                                  const BlockHandle* handle,
                                                  FileWriterWithOffsetAndCachePrefix* writer_info) {
  Rep* r = rep_;
  Cache* block_cache_compressed = r->table_options.block_cache_compressed.get();

  if (type != kNoCompression && block_cache_compressed != nullptr) {

    size_t size = block_contents.size();

    std::unique_ptr<char[]> ubuf(new char[size + 1]);
    memcpy(ubuf.get(), block_contents.data(), size);
    ubuf[size] = type;

    BlockContents results(std::move(ubuf), size, true, type, r->mem_tracker);

    Block* block = new Block(std::move(results));

    // make cache key by appending the file offset to the cache prefix id
    char* end = EncodeVarint64(
                  writer_info->compressed_cache_key_prefix.data +
                  writer_info->compressed_cache_key_prefix.size,
                  handle->offset());
    Slice key(writer_info->compressed_cache_key_prefix.data,
        static_cast<size_t> (end - writer_info->compressed_cache_key_prefix.data));

    // Insert into compressed block cache.
    RETURN_NOT_OK(block_cache_compressed->Insert(
        key, kDefaultQueryId, block, block->usable_size(), &DeleteCachedBlock));

    // Invalidate OS cache.
    auto status = writer_info->writer->InvalidateCache(
        static_cast<size_t>(writer_info->offset), size);
    if (!status.ok() && !status.IsNotSupported()) {
      return status;
    }
  }
  return Status::OK();
}

Status BlockBasedTableBuilder::Finish() {
  Rep* r = rep_;
  Slice end_slice;
  if (!r->data_block_builder.empty()) {
    FlushDataBlock(end_slice);  // no more data block
  }
  if (r->filter_block_builder != nullptr) {
    FlushFilterBlock(nullptr);  // no more filter block
  }
  assert(!r->closed);
  r->closed = true;

  auto index_finish_result = r->data_index_builder->FlushNextBlock(
      &r->data_index_blocks, r->last_index_block_handle);
  RETURN_NOT_OK(index_finish_result);
  if (index_finish_result.get()) {
    ++r->props.num_data_index_blocks;
  }

  // Write meta blocks and metaindex block with the following order.
  //    1. [meta block: filter]
  //    2. [other meta blocks]
  //    3. [meta block: properties]
  //    4. [metaindex block]
  // write meta blocks
  MetaIndexBuilder meta_index_builder;
  for (const auto& item : r->data_index_blocks.meta_blocks) {
    BlockHandle block_handle;
    WriteBlock(item.second, &block_handle, r->metadata_writer.get());
    meta_index_builder.Add(item.first, block_handle);
  }

  if (ok()) {
    if (r->filter_block_builder != nullptr) {
      // Add mapping from "<filter_block_prefix>.Name" to location of either filter block or
      // filter index (for fixed-size bloom filter). We only need filter index for fixed-size bloom
      // filter, which is stored as separate blocks in SST file. Filters of other types are stored
      // as single block in SST file, so we just need an offset to that block instead of index.
      std::string key;
      switch (r->filter_type) {
        case FilterType::kFullFilter:
          key = block_based_table::kFullFilterBlockPrefix;
          break;
        case FilterType::kBlockBasedFilter:
          key = block_based_table::kFilterBlockPrefix;
          break;
        case FilterType::kFixedSizeFilter:
          key = block_based_table::kFixedSizeFilterBlockPrefix;
          break;
        case FilterType::kNoFilter:
          RLOG(InfoLogLevel::FATAL_LEVEL, r->ioptions.info_log,
              "r->filter_block_builder should be null for FilterType::kNoFilter");
          assert(false);
      }
      key.append(r->table_options.filter_policy->Name());
      if (r->filter_type == FilterType::kFixedSizeFilter) {
        // Flush the fixed-size bloom filter index and add its offset under the corresponding
        // key to meta index.
        IndexBuilder::IndexBlocks filter_index_blocks;
        RETURN_NOT_OK(r->filter_index_builder->Finish(&filter_index_blocks));
        BlockHandle filter_index_block_handle;
        WriteBlock(filter_index_blocks.index_block_contents, &filter_index_block_handle,
            r->metadata_writer.get());
        meta_index_builder.Add(key, filter_index_block_handle);
        r->props.filter_index_size = r->filter_index_builder->EstimatedSize() + kBlockTrailerSize;
      } else {
        meta_index_builder.Add(key, r->filter_pending_handle);
      }
    }

    // Write properties block.
    {
      PropertyBlockBuilder property_block_builder;
      r->props.filter_policy_name = r->table_options.filter_policy != nullptr ?
          r->table_options.filter_policy->Name() : "";
      r->props.data_index_size =
          r->data_index_builder->EstimatedSize() + kBlockTrailerSize;

      // Add basic properties
      property_block_builder.AddTableProperty(r->props);

      // Add use collected properties
      NotifyCollectTableCollectorsOnFinish(r->table_properties_collectors,
          r->ioptions.info_log,
          &property_block_builder);

      BlockHandle properties_block_handle;
      WriteRawBlock(
          property_block_builder.Finish(),
          kNoCompression,
          &properties_block_handle,
          r->metadata_writer.get()
      );

      meta_index_builder.Add(kPropertiesBlock, properties_block_handle);
    }  // end of properties block writing
  }    // meta blocks

  BlockHandle meta_index_block_handle;
  // Write meta index and index block.
  if (ok()) {
    // Flush the meta index block.
    WriteRawBlock(
        meta_index_builder.Finish(), kNoCompression, &meta_index_block_handle,
        r->metadata_writer.get());
    // Flush index block if not already flushed.
    if (index_finish_result.get()) {
      WriteBlock(
          r->data_index_blocks.index_block_contents, &r->last_index_block_handle,
          r->metadata_writer.get());
    }
  }

  // Write footer
  if (ok()) {
    // No need to write out new footer if we're using default checksum.
    // We're writing legacy magic number because we want old versions of RocksDB
    // be able to read files generated with new release (just in case if
    // somebody wants to roll back after an upgrade)
    // TODO(icanadi) at some point in the future, when we're absolutely sure
    // nobody will roll back to RocksDB 2.x versions, retire the legacy magic
    // number and always write new table files with new magic number
    bool legacy = (r->table_options.format_version == 0);
    // this is guaranteed by BlockBasedTableBuilder's constructor
    assert(r->table_options.checksum == kCRC32c ||
        r->table_options.format_version != 0);
    Footer footer(legacy ? kLegacyBlockBasedTableMagicNumber
            : kBlockBasedTableMagicNumber,
        r->table_options.format_version);
    footer.set_metaindex_handle(meta_index_block_handle);
    footer.set_index_handle(r->last_index_block_handle);
    footer.set_checksum(r->table_options.checksum);
    std::string footer_encoding;
    footer.AppendEncodedTo(&footer_encoding);
    r->status = r->metadata_writer->writer->Append(footer_encoding);
    if (r->status.ok()) {
      r->metadata_writer->offset += footer_encoding.size();
    }
  }

  return r->status;
}

void BlockBasedTableBuilder::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t BlockBasedTableBuilder::NumEntries() const {
  return rep_->props.num_entries;
}

uint64_t BlockBasedTableBuilder::TotalFileSize() const {
  return rep_->is_split_sst() ? rep_->metadata_writer->offset + rep_->data_writer->offset :
      rep_->metadata_writer->offset;
}

uint64_t BlockBasedTableBuilder::BaseFileSize() const {
  return rep_->metadata_writer->offset;
}

bool BlockBasedTableBuilder::NeedCompact() const {
  for (const auto& collector : rep_->table_properties_collectors) {
    if (collector->NeedCompact()) {
      return true;
    }
  }
  return false;
}

TableProperties BlockBasedTableBuilder::GetTableProperties() const {
  TableProperties ret = rep_->props;
  for (const auto& collector : rep_->table_properties_collectors) {
    for (const auto& prop : collector->GetReadableProperties()) {
      ret.readable_properties.insert(prop);
    }
    CHECK_OK(collector->Finish(&ret.user_collected_properties));
  }
  return ret;
}

}  // namespace rocksdb
