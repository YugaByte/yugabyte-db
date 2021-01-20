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

#ifndef YB_ROCKSDB_TABLE_BLOCK_BASED_TABLE_INTERNAL_H
#define YB_ROCKSDB_TABLE_BLOCK_BASED_TABLE_INTERNAL_H

#include "yb/rocksdb/table/block.h"
#include "yb/rocksdb/table/format.h"
#include "yb/util/logging.h"

namespace rocksdb {

namespace block_based_table {

constexpr char kFilterBlockPrefix[] = "filter.";
constexpr char kFullFilterBlockPrefix[] = "fullfilter.";
constexpr char kFixedSizeFilterBlockPrefix[] = "fixedsizefilter.";

// Read the block identified by "handle" from "file".
// The only relevant option is options.verify_checksums for now.
// On failure return non-OK.
// On success fill *result and return OK - caller owns *result
inline CHECKED_STATUS ReadBlockFromFile(
    RandomAccessFileReader* file, const Footer& footer, const ReadOptions& options,
    const BlockHandle& handle, std::unique_ptr<Block>* result, Env* env,
    const std::shared_ptr<yb::MemTracker>& mem_tracker,
    bool do_uncompress = true) {
  BlockContents contents;
  Status s = ReadBlockContents(file, footer, options, handle, &contents, env,
                               mem_tracker, do_uncompress);
  if (s.ok()) {
    result->reset(new Block(std::move(contents)));
  }

  return s;
}

// The longest prefix of the cache key used to identify blocks.
// We are using the fact that we know the size of the unique ID for Posix files.
static constexpr size_t kMaxCacheKeyPrefixSize =
    yb::FileWithUniqueId::kPosixFileUniqueIdMaxSize + 1;
static constexpr size_t kCacheKeyBufferSize =
    block_based_table::kMaxCacheKeyPrefixSize + yb::kMaxVarint64Length;

struct CacheKeyPrefixBuffer {
  char data[kMaxCacheKeyPrefixSize];
  size_t size = 0;
};

inline Slice GetCacheKey(const CacheKeyPrefixBuffer& cache_key_prefix, const BlockHandle& handle,
    char* cache_key) {
  DCHECK_ONLY_NOTNULL(cache_key);
  DCHECK_NE(cache_key_prefix.size, 0);
  DCHECK_LE(cache_key_prefix.size, kMaxCacheKeyPrefixSize);
  memcpy(cache_key, cache_key_prefix.data, cache_key_prefix.size);
  char* end = EncodeVarint64(cache_key + cache_key_prefix.size, handle.offset());
  return Slice(cache_key, static_cast<size_t>(end - cache_key));
}

// Generate a cache key prefix from the file. Used for both data and metadata files.
inline void GenerateCachePrefix(
    Cache* cc, yb::FileWithUniqueId* file, CacheKeyPrefixBuffer* prefix) {
  // generate an id from the file
  prefix->size = file->GetUniqueId(prefix->data);

  // If the prefix wasn't generated or was too long,
  // create one from the cache.
  if (prefix->size == 0) {
    char* end = EncodeVarint64(prefix->data, cc->NewId());
    prefix->size = static_cast<size_t>(end - prefix->data);
  }
}

} // namespace block_based_table

} // namespace rocksdb

#endif // YB_ROCKSDB_TABLE_BLOCK_BASED_TABLE_INTERNAL_H
