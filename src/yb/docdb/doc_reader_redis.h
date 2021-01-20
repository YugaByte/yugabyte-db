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

#ifndef YB_DOCDB_DOC_READER_REDIS_H_
#define YB_DOCDB_DOC_READER_REDIS_H_

#include <string>
#include <vector>

#include "yb/docdb/docdb_fwd.h"
#include "yb/rocksdb/cache.h"

#include "yb/common/doc_hybrid_time.h"
#include "yb/common/read_hybrid_time.h"
#include "yb/common/transaction.h"

#include "yb/docdb/docdb_types.h"
#include "yb/docdb/expiration.h"
#include "yb/docdb/intent.h"
#include "yb/docdb/primitive_value.h"
#include "yb/docdb/value.h"
#include "yb/docdb/subdocument.h"

#include "yb/util/status.h"
#include "yb/util/strongly_typed_bool.h"

namespace yb {
namespace docdb {

class SliceKeyBound {
 public:
  SliceKeyBound() {}
  SliceKeyBound(const Slice& key, BoundType type)
      : key_(key), type_(type) {}

  bool CanInclude(const Slice& other) const {
    if (!is_valid()) {
      return true;
    }
    int comp = key_.compare(Slice(other.data(), std::min(other.size(), key_.size())));
    if (is_lower()) {
      comp = -comp;
    }
    return is_exclusive() ? comp > 0 : comp >= 0;
  }

  static const SliceKeyBound& Invalid();

  bool is_valid() const { return type_ != BoundType::kInvalid; }
  const Slice& key() const { return key_; }

  bool is_exclusive() const {
    return type_ == BoundType::kExclusiveLower || type_ == BoundType::kExclusiveUpper;
  }

  bool is_lower() const {
    return type_ == BoundType::kExclusiveLower || type_ == BoundType::kInclusiveLower;
  }

  explicit operator bool() const {
    return is_valid();
  }

  std::string ToString() const {
    if (!is_valid()) {
      return "{ empty }";
    }
    return Format("{ $0$1 $2 }", is_lower() ? ">" : "<", is_exclusive() ? "" : "=",
                  SubDocKey::DebugSliceToString(key_));
  }

 private:
  Slice key_;
  BoundType type_ = BoundType::kInvalid;
};

class IndexBound {
 public:
  IndexBound() :
      index_(-1),
      is_lower_bound_(false) {}

  IndexBound(int64 index, bool is_lower_bound) :
      index_(index),
      is_lower_bound_(is_lower_bound) {}

  bool CanInclude(int64 curr_index) const {
    if (index_ == -1 ) {
      return true;
    }
    return is_lower_bound_ ? index_ <= curr_index : index_ >= curr_index;
  }

  static const IndexBound& Empty();

 private:
  int64 index_;
  bool is_lower_bound_;
};

// Pass data to GetRedisSubDocument function.
struct GetRedisSubDocumentData {
  GetRedisSubDocumentData(
    const Slice& subdoc_key,
    SubDocument* result_,
    bool* doc_found_ = nullptr,
    MonoDelta default_ttl = Value::kMaxTtl)
      : subdocument_key(subdoc_key),
        result(result_),
        doc_found(doc_found_),
        exp(default_ttl) {}

  Slice subdocument_key;
  SubDocument* result;
  bool* doc_found;

  DeadlineInfo* deadline_info = nullptr;

  // The TTL and hybrid time are return values external to the SubDocument
  // which occasionally need to be accessed for TTL calculation.
  mutable Expiration exp;
  bool return_type_only = false;

  // Represent bounds on the first and last subkey to be considered.
  const SliceKeyBound* low_subkey = &SliceKeyBound::Invalid();
  const SliceKeyBound* high_subkey = &SliceKeyBound::Invalid();

  // Represent bounds on the first and last ranks to be considered.
  const IndexBound* low_index = &IndexBound::Empty();
  const IndexBound* high_index = &IndexBound::Empty();
  // Maximum number of children to add for this subdocument (0 means no limit).
  int32_t limit = 0;
  // Only store a count of the number of records found, but don't store the records themselves.
  bool count_only = false;
  // Stores the count of records found, if count_only option is set.
  mutable size_t record_count = 0;

  GetRedisSubDocumentData Adjusted(
      const Slice& subdoc_key, SubDocument* result_, bool* doc_found_ = nullptr) const {
    GetRedisSubDocumentData result(subdoc_key, result_, doc_found_);
    result.deadline_info = deadline_info;
    result.exp = exp;
    result.return_type_only = return_type_only;
    result.low_subkey = low_subkey;
    result.high_subkey = high_subkey;
    result.low_index = low_index;
    result.high_index = high_index;
    result.limit = limit;
    return result;
  }

  std::string ToString() const {
    return Format("{ subdocument_key: $0 exp.ttl: $1 exp.write_time: $2 return_type_only: $3 "
                      "low_subkey: $4 high_subkey: $5 }",
                  SubDocKey::DebugSliceToString(subdocument_key), exp.ttl,
                  exp.write_ht, return_type_only, low_subkey, high_subkey);
  }
};

inline std::ostream& operator<<(std::ostream& out, const GetRedisSubDocumentData& data) {
  return out << data.ToString();
}

// Returns the whole SubDocument below some node identified by subdocument_key.
// subdocument_key should not have a timestamp.
// Before the function is called, if seek_fwd_suffices is true, the iterator is expected to be
// positioned on or before the first key when called.
// After this, the iter should be positioned just outside the considered data range. If low_subkey,
// and high_subkey are specified, the iterator will be positioned just past high_subkey. Otherwise,
// the iterator will be positioned just past the SubDocument.
// This function works with or without object init markers present.
// If tombstone and other values are inserted at the same timestamp, it results in undefined
// behavior.
// The projection, if set, restricts the scan to a subset of keys in the first level.
// The projection is used for QL selects to get only a subset of columns.
yb::Status GetRedisSubDocument(
    IntentAwareIterator *db_iter,
    const GetRedisSubDocumentData& data,
    const std::vector<PrimitiveValue>* projection = nullptr,
    SeekFwdSuffices seek_fwd_suffices = SeekFwdSuffices::kTrue);

// This version of GetRedisSubDocument creates a new iterator every time. This is not recommended
// for multiple calls to subdocs that are sequential or near each other, in e.g.
// doc_rowwise_iterator.
yb::Status GetRedisSubDocument(
    const DocDB& doc_db,
    const GetRedisSubDocumentData& data,
    const rocksdb::QueryId query_id,
    const TransactionOperationContextOpt& txn_op_context,
    CoarseTimePoint deadline,
    const ReadHybridTime& read_time = ReadHybridTime::Max());

}  // namespace docdb
}  // namespace yb

#endif  // YB_DOCDB_DOC_READER_REDIS_H_
