//
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
//

#include "yb/rocksdb/db/dbformat.h"

#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/doc_key.h"

namespace yb {
namespace docdb {

Status GetDocHybridTime(const rocksdb::UserBoundaryValues& values, DocHybridTime* out);

Status GetPrimitiveValue(const rocksdb::UserBoundaryValues& values,
                         size_t index,
                         PrimitiveValue* out);

namespace {

constexpr rocksdb::UserBoundaryTag kDocHybridTimeTag = 1;
// Here we reserve some tags for future use.
// Because Tag is persistent.
constexpr rocksdb::UserBoundaryTag kRangeComponentsStart = 10;

// Wrapper for UserBoundaryValue that stores DocHybridTime.
class DocHybridTimeValue : public rocksdb::UserBoundaryValue {
 public:
  explicit DocHybridTimeValue(Slice slice) {
    memcpy(buffer_, slice.data(), slice.size());
    encoded_ = Slice(buffer_, slice.size());
  }

  static CHECKED_STATUS Create(Slice data, rocksdb::UserBoundaryValuePtr* value) {
    CHECK_NOTNULL(value);
    if (data.size() > kMaxBytesPerEncodedHybridTime) {
      return STATUS_SUBSTITUTE(Corruption, "Too big encoded doc hybrid time: $0", data.size());
    }

    *value = std::make_shared<DocHybridTimeValue>(data);
    return Status::OK();
  }

  virtual ~DocHybridTimeValue() {}

  rocksdb::UserBoundaryTag Tag() override {
    return kDocHybridTimeTag;
  }

  Slice Encode() override {
    return encoded_;
  }

  int CompareTo(const UserBoundaryValue& pre_rhs) override {
    const auto* rhs = down_cast<const DocHybridTimeValue*>(&pre_rhs);
    return -encoded_.compare(rhs->encoded_);
  }

  CHECKED_STATUS value(DocHybridTime* out) const {
    CHECK_NOTNULL(out);
    DocHybridTime result;
    RETURN_NOT_OK(result.FullyDecodeFrom(encoded_));
    *out = std::move(result);
    return Status::OK();
  }

 private:
  char buffer_[kMaxBytesPerEncodedHybridTime];
  Slice encoded_;
};

// Wrapper for UserBoundaryValue that stores PrimitiveValue with index.
class PrimitiveBoundaryValue : public rocksdb::UserBoundaryValue {
 public:
  explicit PrimitiveBoundaryValue(size_t index, Slice slice) : index_(index) {
    buffer_.assign(slice.data(), slice.end());
  }

  static CHECKED_STATUS Create(size_t index, Slice data, rocksdb::UserBoundaryValuePtr* value) {
    CHECK_NOTNULL(value);

    *value = std::make_shared<PrimitiveBoundaryValue>(index, data);
    return Status::OK();
  }

  virtual ~PrimitiveBoundaryValue() {}

  static rocksdb::UserBoundaryTag TagForIndex(size_t index) {
    return static_cast<uint32_t>(kRangeComponentsStart + index);
  }

  rocksdb::UserBoundaryTag Tag() override {
    return TagForIndex(index_);
  }

  Slice Encode() override {
    return const_cast<const PrimitiveBoundaryValue*>(this)->Encode();
  }

  Slice Encode() const {
    return Slice(buffer_.data(), buffer_.size());
  }

  CHECKED_STATUS value(PrimitiveValue* out) const {
    CHECK_NOTNULL(out);
    PrimitiveValue result;
    Slice temp = Encode();
    RETURN_NOT_OK(result.DecodeFromKey(&temp));
    if (!temp.empty()) {
      return STATUS_SUBSTITUTE(Corruption,
                               "Extra data left after decoding: $0, remaining: $1",
                               Encode().ToDebugString(),
                               temp.size());
    }
    *out = std::move(result);
    return Status::OK();
  }

  int CompareTo(const rocksdb::UserBoundaryValue& pre_rhs) override {
    const auto* rhs = down_cast<const PrimitiveBoundaryValue*>(&pre_rhs);
    return Encode().compare(rhs->Encode());
  }
 private:
  size_t index_; // Index of corresponding range component.
  boost::container::small_vector<uint8_t, 128> buffer_;
};

class DocBoundaryValuesExtractor : public rocksdb::BoundaryValuesExtractor {
 public:
  virtual ~DocBoundaryValuesExtractor() {}

  Status Decode(rocksdb::UserBoundaryTag tag,
                Slice data,
                rocksdb::UserBoundaryValuePtr* value) override {
    CHECK_NOTNULL(value);
    if (tag == kDocHybridTimeTag) {
      return DocHybridTimeValue::Create(data, value);
    }
    if (tag >= kRangeComponentsStart) {
      return PrimitiveBoundaryValue::Create(tag - kRangeComponentsStart, data, value);
    }

    return STATUS_SUBSTITUTE(NotFound, "Unknown tag: $0", tag);
  }

  Status Extract(Slice user_key, Slice value, rocksdb::UserBoundaryValues* values) override {
    if (user_key.TryConsumeByte(ValueTypeAsChar::kTransactionId) ||
        user_key.TryConsumeByte(ValueTypeAsChar::kTransactionApplyState) ||
        user_key.TryConsumeByte(ValueTypeAsChar::kExternalTransactionId)) {
      // Skipping:
      // For intents db:
      // - reverse index from transaction id to keys of write intents belonging to that transaction.
      // - external transaction records (transactions that originated on a CDC producer)
      // For regular db:
      // - transaction apply state records.
      return Status::OK();
    }

    CHECK_NOTNULL(values);
    boost::container::small_vector<Slice, 20> slices;
    auto user_key_copy = user_key;
    RETURN_NOT_OK(SubDocKey::PartiallyDecode(&user_key_copy, &slices));
    size_t size = slices.size();
    if (size == 0) {
      return STATUS(Corruption, "Key does not contain hybrid time", user_key.ToDebugString());
    }
    // Last one contains Doc Hybrid Time, so number of range is less by 1.
    --size;

    rocksdb::UserBoundaryValuePtr temp;
    RETURN_NOT_OK(DocHybridTimeValue::Create(slices.back(), &temp));
    values->push_back(std::move(temp));

    for (size_t i = 0; i != size; ++i) {
      RETURN_NOT_OK(PrimitiveBoundaryValue::Create(i, slices[i], &temp));
      values->push_back(std::move(temp));
    }

    DCHECK(PerformSanityCheck(user_key, slices, *values));

    return Status::OK();
  }

  rocksdb::UserFrontierPtr CreateFrontier() override {
    return new docdb::ConsensusFrontier();
  }

  bool PerformSanityCheck(Slice user_key,
                          const boost::container::small_vector_base<Slice>& slices,
                          const rocksdb::UserBoundaryValues& values) {
#ifndef NDEBUG
    SubDocKey sub_doc_key;
    CHECK_OK(sub_doc_key.FullyDecodeFrom(user_key));

    DocHybridTime doc_ht, doc_ht2;
    Slice temp_slice = slices.back();
    CHECK_OK(doc_ht.DecodeFrom(&temp_slice));
    CHECK(temp_slice.empty());
    CHECK_EQ(sub_doc_key.doc_hybrid_time(), doc_ht);
    CHECK_OK(GetDocHybridTime(values, &doc_ht2));
    CHECK_EQ(doc_ht, doc_ht2);

    const auto& range_group = sub_doc_key.doc_key().range_group();
    CHECK_EQ(range_group.size(), slices.size() - 1);

    for (size_t i = 0; i != range_group.size(); ++i) {
      PrimitiveValue primitive_value, primitive_value2;
      temp_slice = slices[i];
      CHECK_OK(primitive_value.DecodeFromKey(&temp_slice));
      CHECK(temp_slice.empty());
      CHECK_EQ(range_group[i], primitive_value);
      CHECK_OK(GetPrimitiveValue(values, i, &primitive_value2));
      CHECK_EQ(range_group[i], primitive_value2);
    }
#endif
    return true;
  }
};

} // namespace

std::shared_ptr<rocksdb::BoundaryValuesExtractor> DocBoundaryValuesExtractorInstance() {
  static std::shared_ptr<rocksdb::BoundaryValuesExtractor> instance =
      std::make_shared<DocBoundaryValuesExtractor>();
  return instance;
}

// Used in tests
Status GetPrimitiveValue(const rocksdb::UserBoundaryValues& values,
                         size_t index,
                         PrimitiveValue* out) {
  auto value = rocksdb::UserValueWithTag(values, PrimitiveBoundaryValue::TagForIndex(index));
  if (!value) {
    return STATUS_SUBSTITUTE(NotFound, "Not found value for index $0", index);
  }
  const auto* primitive_value = down_cast<PrimitiveBoundaryValue*>(value.get());
  return primitive_value->value(out);
}

Status GetDocHybridTime(const rocksdb::UserBoundaryValues& values, DocHybridTime* out) {
  auto value = rocksdb::UserValueWithTag(values, kDocHybridTimeTag);
  if (!value) {
    return STATUS(NotFound, "Not found value for doc hybrid time");
  }
  const auto* time_value = down_cast<DocHybridTimeValue*>(value.get());
  return time_value->value(out);
}

rocksdb::UserBoundaryTag TagForRangeComponent(size_t index) {
  return PrimitiveBoundaryValue::TagForIndex(index);
}

} // namespace docdb
} // namespace yb
