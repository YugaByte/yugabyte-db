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

#include "yb/docdb/internal_doc_iterator.h"

#include <sstream>
#include <string>

#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_kv_util.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/yb_rocksdb.h"

using std::endl;
using std::string;
using std::stringstream;
using std::unique_ptr;

using strings::Substitute;

namespace yb {
namespace docdb {

InternalDocIterator::InternalDocIterator(rocksdb::DB* rocksdb,
                                         DocWriteBatchCache* doc_write_batch_cache,
                                         BloomFilterMode bloom_filter_mode,
                                         const KeyBytes& filter_key,
                                         const rocksdb::QueryId query_id,
                                         int* num_rocksdb_seeks)
    : db_(rocksdb),
      bloom_filter_mode_(bloom_filter_mode),
      filter_key_(filter_key),
      iter_(nullptr),
      doc_write_batch_cache_(doc_write_batch_cache),
      subdoc_user_timestamp_(Value::kInvalidUserTimestamp),
      found_exact_key_prefix_(false),
      subdoc_exists_(Trilean::kUnknown),
      query_id_(query_id),
      num_rocksdb_seeks_(num_rocksdb_seeks) {}

Status InternalDocIterator::SeekToDocument(const KeyBytes& encoded_doc_key) {
  SetDocumentKey(encoded_doc_key);
  return SeekToKeyPrefix();
}

Status InternalDocIterator::SeekToSubDocument(const PrimitiveValue& subkey) {
  DOCDB_DEBUG_LOG("Called with subkey=$0", subkey.ToString());
  RETURN_NOT_OK(AppendSubkeyInExistingSubDoc(subkey));
  return SeekToKeyPrefix();
}

Status InternalDocIterator::AppendSubkeyInExistingSubDoc(const PrimitiveValue &subkey) {
  if (!subdoc_exists()) {
    return STATUS_FORMAT(IllegalState, "Subdocument is supposed to exist. $0", ToDebugString());
  }
  if (!IsObjectType(subdoc_type_)) {
    return STATUS_FORMAT(IllegalState, "Expected object subdocument type. $0", ToDebugString());
  }
  AppendToPrefix(subkey);
  return Status::OK();
}

void InternalDocIterator::AppendToPrefix(const PrimitiveValue& subkey) {
  subkey.AppendToKey(&key_prefix_);
}

void InternalDocIterator::AppendHybridTimeToPrefix(
    const DocHybridTime& hybrid_time) {
  key_prefix_.AppendHybridTime(hybrid_time);
}

string InternalDocIterator::ToDebugString() {
  stringstream ss;
  ss << "InternalDocIterator:" << endl;
  ss << "  key_prefix: " << BestEffortDocDBKeyToStr(key_prefix_) << endl;
  if (subdoc_exists_ == Trilean::kTrue || subdoc_deleted()) {
    ss << "  subdoc_type: " << ToString(subdoc_type_) << endl;
    ss << "  subdoc_gen_ht: " << subdoc_ht_.ToString() << endl;
  }
  ss << "  subdoc_exists: " << subdoc_exists_ << endl;
  return ss.str();
}

Status InternalDocIterator::SeekToKeyPrefix() {
  const auto prev_subdoc_exists = subdoc_exists_;
  const auto prev_subdoc_ht = subdoc_ht_;
  const auto prev_key_prefix_only_lacks_ht = found_exact_key_prefix_;

  subdoc_exists_ = Trilean::kFalse;
  subdoc_type_ = ValueType::kInvalid;

  DOCDB_DEBUG_LOG("key_prefix=$0", BestEffortDocDBKeyToStr(key_prefix_));
  boost::optional<DocWriteBatchCache::Entry> cached_ht_and_type =
      doc_write_batch_cache_->Get(KeyBytes(key_prefix_.AsStringRef()));
  if (cached_ht_and_type) {
    subdoc_ht_ = cached_ht_and_type->doc_hybrid_time;
    subdoc_type_ = cached_ht_and_type->value_type;
    subdoc_user_timestamp_ = cached_ht_and_type->user_timestamp;
    found_exact_key_prefix_ = cached_ht_and_type->found_exact_key_prefix;
    subdoc_exists_ = ToTrilean(subdoc_type_ != ValueType::kTombstone);
  } else {
    if (!iter_) {
      // If iter hasn't been created yet, do so now.
      switch (bloom_filter_mode_) {
        case BloomFilterMode::USE_BLOOM_FILTER:
          {
            iter_ = CreateRocksDBIterator(db_, bloom_filter_mode_, filter_key_.AsSlice(),
                query_id_);
          }
          break;
        case BloomFilterMode::DONT_USE_BLOOM_FILTER:
          iter_ = CreateRocksDBIterator(db_, bloom_filter_mode_, boost::none, query_id_);
          break;
      }
    }
    ROCKSDB_SEEK(iter_.get(), key_prefix_.AsSlice());
    if (num_rocksdb_seeks_ != nullptr) {
      (*num_rocksdb_seeks_)++;
    }
    if (!HasMoreData()) {
      DOCDB_DEBUG_LOG("No more data found in RocksDB when trying to seek at prefix $0",
                      BestEffortDocDBKeyToStr(key_prefix_));
      subdoc_exists_ = Trilean::kFalse;
    } else {
      const rocksdb::Slice& key = iter_->key();
      // If the first key >= key_prefix_ in RocksDB starts with key_prefix_, then a
      // document/subdocument pointed to by key_prefix_ exists, or has been recently deleted.
      if (key_prefix_.IsPrefixOf(key)) {
        // TODO: make this return a Status and propagate it to the caller.
        RETURN_NOT_OK(Value::DecodePrimitiveValueType(iter_->value(), &subdoc_type_));
        RETURN_NOT_OK(Value::DecodeUserTimestamp(iter_->value(), &subdoc_user_timestamp_));
        RETURN_NOT_OK(key_prefix_.OnlyLacksHybridTimeFrom(key, &found_exact_key_prefix_));

        // TODO: with optional init markers we can find something that is more than one level
        //       deep relative to the current prefix.

        RETURN_NOT_OK(DecodeHybridTimeFromEndOfKey(key, &subdoc_ht_));

        // Cache the results of reading from RocksDB so that we don't have to read again in a later
        // operation in the same DocWriteBatch.
        DOCDB_DEBUG_LOG("Writing to DocWriteBatchCache: $0",
                        BestEffortDocDBKeyToStr(key_prefix_));
        if (prev_subdoc_exists != Trilean::kUnknown && prev_subdoc_ht > subdoc_ht_ &&
            prev_key_prefix_only_lacks_ht) {
          // We already saw an object init marker or a tombstone one level higher with a higher
          // hybrid_time, so just ignore this key/value pair. This had to be added when we switched
          // from a format with intermediate hybrid_times to our current format without them.
          //
          // Example (from a real test case):
          //
          // SubDocKey(DocKey([], ["a"]), [HT(38)]) -> {}
          // SubDocKey(DocKey([], ["a"]), [HT(37)]) -> DEL
          // SubDocKey(DocKey([], ["a"]), [HT(36)]) -> false
          // SubDocKey(DocKey([], ["a"]), [HT(1)]) -> {}
          // SubDocKey(DocKey([], ["a"]), ["y", HT(35)]) -> "lD\x97\xaf^m\x0a1\xa0\xfc\xc8YM"
          //
          // Caveat (04/17/2017): the HybridTime encoding in the above example is outdated.
          //
          // In the above layout, if we try to set "a.y.x" to a new value, we first seek to the
          // document key "a" and find that it exists, but then we seek to "a.y" and find that it
          // also exists as a primitive value (assuming we don't check the hybrid_time), and
          // therefore we can't create "a.y.x", which would be incorrect.
          subdoc_exists_ = Trilean::kFalse;
        } else {
          doc_write_batch_cache_->Put(key_prefix_, subdoc_ht_, subdoc_type_,
                                      subdoc_user_timestamp_, found_exact_key_prefix_);
          if (subdoc_type_ != ValueType::kTombstone) {
            subdoc_exists_ = ToTrilean(true);
          }
        }
      } else {
        DOCDB_DEBUG_LOG("Actual RocksDB key found ($0) does not start with $1",
                        BestEffortDocDBKeyToStr(KeyBytes(key.ToString())),
                        BestEffortDocDBKeyToStr(key_prefix_));
        subdoc_exists_ = Trilean::kFalse;
      }
    }

  }
  DOCDB_DEBUG_LOG("New InternalDocIterator state: $0", ToDebugString());
  return Status::OK();
}

}  // namespace docdb
}  // namespace yb
