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

#ifndef YB_ROCKSDB_TABLE_BLOCK_BASED_TABLE_BUILDER_H
#define YB_ROCKSDB_TABLE_BLOCK_BASED_TABLE_BUILDER_H

#include <stdint.h>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "yb/rocksdb/flush_block_policy.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/status.h"
#include "yb/rocksdb/table/table_builder.h"

namespace rocksdb {

class BlockBuilder;
class BlockHandle;
class WritableFile;
struct BlockBasedTableOptions;

extern const uint64_t kBlockBasedTableMagicNumber;
extern const uint64_t kLegacyBlockBasedTableMagicNumber;

class BlockBasedTableBuilder : public TableBuilder {
 public:
  // Create a builder that will store the contents of the table it is
  // building in *file.  Does not close the file.  It is up to the
  // caller to close the file after calling Finish().
  BlockBasedTableBuilder(
      const ImmutableCFOptions& ioptions,
      const BlockBasedTableOptions& table_options,
      const std::shared_ptr<const InternalKeyComparator>& internal_comparator,
      const IntTblPropCollectorFactories& int_tbl_prop_collector_factories,
      uint32_t column_family_id, WritableFileWriter* metadata_file,
      WritableFileWriter* data_file,
      const CompressionType compression_type,
      const CompressionOptions& compression_opts, const bool skip_filters);

  // REQUIRES: Either Finish() or Abandon() has been called.
  ~BlockBasedTableBuilder();

  // Add key,value to the table being constructed.
  // REQUIRES: key is after any previously added key according to comparator.
  // REQUIRES: Finish(), Abandon() have not been called
  void Add(const Slice& key, const Slice& value) override;

  // Return non-ok iff some error has been detected.
  Status status() const override;

  // Finish building the table.  Stops using the file passed to the
  // constructor after this function returns.
  // REQUIRES: Finish(), Abandon() have not been called
  Status Finish() override;

  // Indicate that the contents of this builder should be abandoned.  Stops
  // using the file passed to the constructor after this function returns.
  // If the caller is not going to call Finish(), it must call Abandon()
  // before destroying this builder.
  // REQUIRES: Finish(), Abandon() have not been called
  void Abandon() override;

  // Number of calls to Add() so far.
  uint64_t NumEntries() const override;

  // Total size of the file(s) generated so far.  If invoked after a successful
  // Finish() call, returns the total size of the final generated file(s).
  uint64_t TotalFileSize() const override;

  // Size of the base file generated so far.  If invoked after a successful Finish() call, returns
  // the size of the final generated base file. SST is either stored in single base file, or
  // metadata is stored in base file while data is split among data files (S-Blocks).
  // Block-based SST are always stored in separate files, other SST types are stored in one file
  // each.
  uint64_t BaseFileSize() const override;

  bool NeedCompact() const override;

  // Get table properties
  TableProperties GetTableProperties() const override;

 private:
  struct FileWriterWithOffsetAndCachePrefix;

  bool ok() const { return status().ok(); }
  // Call block's Finish() method and then write the finalize block contents to
  // file. Returns number of bytes written to file.
  size_t WriteBlock(BlockBuilder* block, BlockHandle* handle,
      FileWriterWithOffsetAndCachePrefix* writer_info);
  // Directly write block content to the file. Returns number of bytes written to file.
  size_t WriteBlock(const Slice& block_contents, BlockHandle* handle,
      FileWriterWithOffsetAndCachePrefix* writer_info);
  size_t WriteRawBlock(const Slice& data, CompressionType, BlockHandle* handle,
      FileWriterWithOffsetAndCachePrefix* writer_info);
  Status InsertBlockInCache(const Slice& block_contents,
                            const CompressionType type,
                            const BlockHandle* handle,
                            FileWriterWithOffsetAndCachePrefix* writer_info);

  struct Rep;
  class BlockBasedTablePropertiesCollectorFactory;
  class BlockBasedTablePropertiesCollector;
  Rep* rep_;

  // Flush the current data block into disk. next_block_first_key should be nullptr if this is the
  // last block written to disk.
  // REQUIRES: Finish(), Abandon() have not been called.
  void FlushDataBlock(const Slice& next_block_first_key);

  // Flush the current filter block into disk. next_block_first_filter_key should be nullptr if this
  // is the last block written to disk.
  // REQUIRES: Finish(), Abandon() have not been called.
  void FlushFilterBlock(const Slice* const next_block_first_filter_key);

  // Some compression libraries fail when the raw size is bigger than int. If
  // uncompressed size is bigger than kCompressionSizeLimit, don't compress it
  const uint64_t kCompressionSizeLimit = std::numeric_limits<int>::max();

  // No copying allowed
  BlockBasedTableBuilder(const BlockBasedTableBuilder&) = delete;
  void operator=(const BlockBasedTableBuilder&) = delete;
};

}  // namespace rocksdb

#endif  // YB_ROCKSDB_TABLE_BLOCK_BASED_TABLE_BUILDER_H
