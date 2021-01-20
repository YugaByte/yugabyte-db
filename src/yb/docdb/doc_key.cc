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

#include "yb/docdb/doc_key.h"

#include <memory>
#include <sstream>

#include "yb/util/string_util.h"

#include "yb/common/partition.h"
#include "yb/docdb/doc_kv_util.h"
#include "yb/docdb/doc_path.h"
#include "yb/docdb/value_type.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/util/enums.h"
#include "yb/util/compare_util.h"

using std::ostringstream;

using strings::Substitute;

using yb::util::CompareVectors;
using yb::util::CompareUsingLessThan;

namespace yb {
namespace docdb {

namespace {

// Checks whether slice starts with primitive value.
// Valid cases are end of group or primitive value starting with value type.
Result<bool> HasPrimitiveValue(Slice* slice, AllowSpecial allow_special) {
  if (PREDICT_FALSE(slice->empty())) {
    return STATUS(Corruption, "Unexpected end of key when decoding document key");
  }
  ValueType current_value_type = static_cast<ValueType>(*slice->data());
  if (current_value_type == ValueType::kGroupEnd) {
    slice->consume_byte();
    return false;
  }

  if (IsPrimitiveValueType(current_value_type)) {
    return true;
  }

  if (allow_special && IsSpecialValueType(current_value_type)) {
    return true;
  }

  return STATUS_FORMAT(Corruption, "Expected a primitive value type, got $0", current_value_type);
}

constexpr auto kNumValuesNoLimit = std::numeric_limits<int>::max();

// Consumes up to n_values_limit primitive values from key until group end is found.
// Callback is called for each value and responsible for consuming this single value from slice.
template<class Callback>
Status ConsumePrimitiveValuesFromKey(
    Slice* slice, AllowSpecial allow_special, Callback callback,
    int n_values_limit = kNumValuesNoLimit) {
  const auto initial_slice(*slice);  // For error reporting.
  for (; n_values_limit > 0; --n_values_limit) {
    if (!VERIFY_RESULT(HasPrimitiveValue(slice, allow_special))) {
      return Status::OK();
    }

    RETURN_NOT_OK_PREPEND(callback(),
        Substitute("while consuming primitive values from $0",
                   initial_slice.ToDebugHexString()));
  }
  return Status::OK();
}

Status ConsumePrimitiveValuesFromKey(Slice* slice, AllowSpecial allow_special,
                                     boost::container::small_vector_base<Slice>* result,
                                     int n_values_limit = kNumValuesNoLimit) {
  return ConsumePrimitiveValuesFromKey(slice, allow_special, [slice, result]() -> Status {
    auto begin = slice->data();
    RETURN_NOT_OK(PrimitiveValue::DecodeKey(slice, /* out */ nullptr));
    if (result) {
      result->emplace_back(begin, slice->data());
    }
    return Status::OK();
  }, n_values_limit);
}

Status ConsumePrimitiveValuesFromKey(
    Slice* slice, AllowSpecial allow_special, std::vector<PrimitiveValue>* result,
    int n_values_limit = kNumValuesNoLimit) {
  return ConsumePrimitiveValuesFromKey(slice, allow_special, [slice, result] {
    result->emplace_back();
    return result->back().DecodeFromKey(slice);
  }, n_values_limit);
}

} // namespace

Result<bool> ConsumePrimitiveValueFromKey(Slice* slice) {
  if (!VERIFY_RESULT(HasPrimitiveValue(slice, AllowSpecial::kFalse))) {
    return false;
  }
  RETURN_NOT_OK(PrimitiveValue::DecodeKey(slice, nullptr /* out */));
  return true;
}

Status ConsumePrimitiveValuesFromKey(Slice* slice, std::vector<PrimitiveValue>* result) {
  return ConsumePrimitiveValuesFromKey(slice, AllowSpecial::kFalse, result);
}

// ------------------------------------------------------------------------------------------------
// DocKey
// ------------------------------------------------------------------------------------------------

DocKey::DocKey()
    : cotable_id_(boost::uuids::nil_uuid()),
      pgtable_id_(0),
      hash_present_(false),
      hash_(0) {
}

DocKey::DocKey(std::vector<PrimitiveValue> range_components)
    : cotable_id_(boost::uuids::nil_uuid()),
      pgtable_id_(0),
      hash_present_(false),
      hash_(0),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(DocKeyHash hash,
               std::vector<PrimitiveValue> hashed_components,
               std::vector<PrimitiveValue> range_components)
    : cotable_id_(boost::uuids::nil_uuid()),
      pgtable_id_(0),
      hash_present_(true),
      hash_(hash),
      hashed_group_(std::move(hashed_components)),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(const Uuid& cotable_id,
               DocKeyHash hash,
               std::vector<PrimitiveValue> hashed_components,
               std::vector<PrimitiveValue> range_components)
    : cotable_id_(cotable_id),
      pgtable_id_(0),
      hash_present_(true),
      hash_(hash),
      hashed_group_(std::move(hashed_components)),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(const PgTableOid pgtable_id,
               DocKeyHash hash,
               std::vector<PrimitiveValue> hashed_components,
               std::vector<PrimitiveValue> range_components)
    : cotable_id_(boost::uuids::nil_uuid()),
      pgtable_id_(pgtable_id),
      hash_present_(true),
      hash_(hash),
      hashed_group_(std::move(hashed_components)),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(const Uuid& cotable_id)
    : cotable_id_(cotable_id),
      pgtable_id_(0),
      hash_present_(false),
      hash_(0) {
}

DocKey::DocKey(const PgTableOid pgtable_id)
    : cotable_id_(boost::uuids::nil_uuid()),
      pgtable_id_(pgtable_id),
      hash_present_(false),
      hash_(0) {
}

DocKey::DocKey(const Schema& schema)
    : cotable_id_(schema.cotable_id()),
      pgtable_id_(schema.pgtable_id()),
      hash_present_(false),
      hash_(0) {
}

DocKey::DocKey(const Schema& schema, DocKeyHash hash)
    : cotable_id_(schema.cotable_id()),
      pgtable_id_(schema.pgtable_id()),
      hash_present_(true),
      hash_(hash) {
}

DocKey::DocKey(const Schema& schema, std::vector<PrimitiveValue> range_components)
    : cotable_id_(schema.cotable_id()),
      pgtable_id_(schema.pgtable_id()),
      hash_present_(false),
      hash_(0),
      range_group_(std::move(range_components)) {
}

DocKey::DocKey(const Schema& schema, DocKeyHash hash,
               std::vector<PrimitiveValue> hashed_components,
               std::vector<PrimitiveValue> range_components)
    : cotable_id_(schema.cotable_id()),
      pgtable_id_(schema.pgtable_id()),
      hash_present_(true),
      hash_(hash),
      hashed_group_(std::move(hashed_components)),
      range_group_(std::move(range_components)) {
}

KeyBytes DocKey::Encode() const {
  KeyBytes result;
  AppendTo(&result);
  return result;
}

namespace {

// Used as cache of allocated memory by EncodeAsRefCntPrefix.
thread_local boost::optional<KeyBytes> thread_local_encode_buffer;

}

RefCntPrefix DocKey::EncodeAsRefCntPrefix() const {
  KeyBytes* encode_buffer = thread_local_encode_buffer.get_ptr();
  if (!encode_buffer) {
    thread_local_encode_buffer.emplace();
    encode_buffer = thread_local_encode_buffer.get_ptr();
  }
  encode_buffer->Clear();
  AppendTo(encode_buffer);
  return RefCntPrefix(encode_buffer->AsSlice());
}

void DocKey::AppendTo(KeyBytes* out) const {
  auto encoder = DocKeyEncoder(out);
  if (!cotable_id_.IsNil()) {
    encoder.CotableId(cotable_id_).Hash(hash_present_, hash_, hashed_group_).Range(range_group_);
  } else {
    encoder.PgtableId(pgtable_id_).Hash(hash_present_, hash_, hashed_group_).Range(range_group_);
  }
}

void DocKey::Clear() {
  hash_present_ = false;
  hash_ = 0xdead;
  hashed_group_.clear();
  range_group_.clear();
}

void DocKey::ClearRangeComponents() {
  range_group_.clear();
}

void DocKey::ResizeRangeComponents(int new_size) {
  range_group_.resize(new_size);
}

namespace {

class DecodeDocKeyCallback {
 public:
  explicit DecodeDocKeyCallback(boost::container::small_vector_base<Slice>* out) : out_(out) {}

  boost::container::small_vector_base<Slice>* hashed_group() const {
    return nullptr;
  }

  boost::container::small_vector_base<Slice>* range_group() const {
    return out_;
  }

  void SetHash(...) const {}

  void SetCoTableId(const Uuid cotable_id) const {}

  void SetPgTableId(const PgTableOid pgtable_id) const {}

 private:
  boost::container::small_vector_base<Slice>* out_;
};

class DummyCallback {
 public:
  boost::container::small_vector_base<Slice>* hashed_group() const {
    return nullptr;
  }

  boost::container::small_vector_base<Slice>* range_group() const {
    return nullptr;
  }

  void SetHash(...) const {}

  void SetCoTableId(const Uuid cotable_id) const {}

  void SetPgTableId(const PgTableOid pgtable_id) const {}

  PrimitiveValue* AddSubkey() const {
    return nullptr;
  }
};

class EncodedSizesCallback {
 public:
  explicit EncodedSizesCallback(DocKeyDecoder* decoder) : decoder_(decoder) {}

  boost::container::small_vector_base<Slice>* hashed_group() const {
    return nullptr;
  }

  boost::container::small_vector_base<Slice>* range_group() const {
    range_group_start_ = decoder_->left_input().data();
    return nullptr;
  }

  void SetHash(...) const {}

  void SetCoTableId(const Uuid cotable_id) const {}

  void SetPgTableId(const PgTableOid pgtable_id) const {}

  PrimitiveValue* AddSubkey() const {
    return nullptr;
  }

  const uint8_t* range_group_start() {
    return range_group_start_;
  }

 private:
  DocKeyDecoder* decoder_;
  mutable const uint8_t* range_group_start_ = nullptr;
};

} // namespace

yb::Status DocKey::PartiallyDecode(Slice *slice,
                                   boost::container::small_vector_base<Slice>* out) {
  CHECK_NOTNULL(out);
  DocKeyDecoder decoder(*slice);
  RETURN_NOT_OK(DoDecode(
      &decoder, DocKeyPart::kWholeDocKey, AllowSpecial::kFalse, DecodeDocKeyCallback(out)));
  *slice = decoder.left_input();
  return Status::OK();
}

Result<DocKeyHash> DocKey::DecodeHash(const Slice& slice) {
  DocKeyDecoder decoder(slice);
  RETURN_NOT_OK(decoder.DecodeCotableId());
  RETURN_NOT_OK(decoder.DecodePgtableId());
  uint16_t hash;
  RETURN_NOT_OK(decoder.DecodeHashCode(&hash));
  return hash;
}

Result<size_t> DocKey::EncodedSize(Slice slice, DocKeyPart part, AllowSpecial allow_special) {
  auto initial_begin = slice.cdata();
  DocKeyDecoder decoder(slice);
  RETURN_NOT_OK(DoDecode(&decoder, part, allow_special, DummyCallback()));
  return decoder.left_input().cdata() - initial_begin;
}

Result<std::pair<size_t, size_t>> DocKey::EncodedHashPartAndDocKeySizes(
    Slice slice,
    AllowSpecial allow_special) {
  auto initial_begin = slice.data();
  DocKeyDecoder decoder(slice);
  EncodedSizesCallback callback(&decoder);
  RETURN_NOT_OK(DoDecode(
      &decoder, DocKeyPart::kWholeDocKey, allow_special, callback));
  return std::make_pair(callback.range_group_start() - initial_begin,
                        decoder.left_input().data() - initial_begin);
}

class DocKey::DecodeFromCallback {
 public:
  explicit DecodeFromCallback(DocKey* key) : key_(key) {
  }

  std::vector<PrimitiveValue>* hashed_group() const {
    return &key_->hashed_group_;
  }

  std::vector<PrimitiveValue>* range_group() const {
    return &key_->range_group_;
  }

  void SetHash(bool present, DocKeyHash hash = 0) const {
    key_->hash_present_ = present;
    if (present) {
      key_->hash_ = hash;
    }
  }
  void SetCoTableId(const Uuid cotable_id) const {
    key_->cotable_id_ = cotable_id;
  }

  void SetPgTableId(const PgTableOid pgtable_id) const {
    key_->pgtable_id_ = pgtable_id;
  }

 private:
  DocKey* key_;
};

Status DocKey::DecodeFrom(Slice *slice, DocKeyPart part_to_decode, AllowSpecial allow_special) {
  Clear();
  DocKeyDecoder decoder(*slice);
  RETURN_NOT_OK(DoDecode(&decoder, part_to_decode, allow_special, DecodeFromCallback(this)));
  *slice = decoder.left_input();
  return Status::OK();
}

Result<size_t> DocKey::DecodeFrom(
    const Slice& slice, DocKeyPart part_to_decode, AllowSpecial allow_special) {
  Slice copy = slice;
  RETURN_NOT_OK(DecodeFrom(&copy, part_to_decode, allow_special));
  return slice.size() - copy.size();
}

namespace {

// Return limit on number of range components to decode based on part_to_decode and whether hash
// component are present in key (hash_present).
int MaxRangeComponentsToDecode(const DocKeyPart part_to_decode, const bool hash_present) {
  switch (part_to_decode) {
    case DocKeyPart::kUpToId:
      LOG(FATAL) << "Internal error: unexpected to have DocKeyPart::kUpToId here";
    case DocKeyPart::kWholeDocKey:
      return kNumValuesNoLimit;
    case DocKeyPart::kUpToHashCode: FALLTHROUGH_INTENDED;
    case DocKeyPart::kUpToHash:
      return 0;
    case DocKeyPart::kUpToHashOrFirstRange:
      return hash_present ? 0 : 1;
  }
  FATAL_INVALID_ENUM_VALUE(DocKeyPart, part_to_decode);
}

} // namespace

template<class Callback>
yb::Status DocKey::DoDecode(DocKeyDecoder* decoder,
                            DocKeyPart part_to_decode,
                            AllowSpecial allow_special,
                            const Callback& callback) {
  Uuid cotable_id;
  PgTableOid pgtable_id;
  if (VERIFY_RESULT(decoder->DecodeCotableId(&cotable_id))) {
    callback.SetCoTableId(cotable_id);
  } else if (VERIFY_RESULT(decoder->DecodePgtableId(&pgtable_id))) {
    callback.SetPgTableId(pgtable_id);
  }

  switch (part_to_decode) {
    case DocKeyPart::kUpToId:
      return Status::OK();
    case DocKeyPart::kUpToHashCode: FALLTHROUGH_INTENDED;
    case DocKeyPart::kUpToHash: FALLTHROUGH_INTENDED;
    case DocKeyPart::kUpToHashOrFirstRange: FALLTHROUGH_INTENDED;
    case DocKeyPart::kWholeDocKey:
      uint16_t hash_code;
      const auto hash_present = VERIFY_RESULT(decoder->DecodeHashCode(&hash_code, allow_special));
      if (hash_present) {
        callback.SetHash(/* present */ true, hash_code);
        if (part_to_decode == DocKeyPart::kUpToHashCode) {
          return Status::OK();
        }
        RETURN_NOT_OK_PREPEND(
            ConsumePrimitiveValuesFromKey(
                decoder->mutable_input(), allow_special, callback.hashed_group()),
            "Error when decoding hashed components of a document key");
      } else {
        callback.SetHash(/* present */ false);
      }
      if (decoder->left_input().empty()) {
        return Status::OK();
      }
      // The rest are range components.
      const auto max_components_to_decode =
          MaxRangeComponentsToDecode(part_to_decode, hash_present);
      if (max_components_to_decode > 0) {
        RETURN_NOT_OK_PREPEND(
            ConsumePrimitiveValuesFromKey(
                decoder->mutable_input(), allow_special, callback.range_group(),
                max_components_to_decode),
            "Error when decoding range components of a document key");
      }
      return Status::OK();
  }
  FATAL_INVALID_ENUM_VALUE(DocKeyPart, part_to_decode);
}

yb::Status DocKey::FullyDecodeFrom(const rocksdb::Slice& slice) {
  rocksdb::Slice mutable_slice = slice;
  Status status = DecodeFrom(&mutable_slice);
  if (!mutable_slice.empty()) {
    return STATUS_SUBSTITUTE(InvalidArgument,
        "Expected all bytes of the slice to be decoded into DocKey, found $0 extra bytes",
        mutable_slice.size());
  }
  return status;
}

string DocKey::ToString() const {
  string result = "DocKey(";
  if (!cotable_id_.IsNil()) {
    result += "CoTableId=";
    result += cotable_id_.ToString();
    result += ", ";
  } else if (pgtable_id_ > 0) {
    result += "PgTableId=";
    result += std::to_string(pgtable_id_);
    result += ", ";
  }

  if (hash_present_) {
    result += StringPrintf("0x%04x", hash_);
    result += ", ";
  }

  result += rocksdb::VectorToString(hashed_group_);
  result += ", ";
  result += rocksdb::VectorToString(range_group_);
  result.push_back(')');
  return result;
}

bool DocKey::operator ==(const DocKey& other) const {
  return cotable_id_ == other.cotable_id_ &&
         pgtable_id_ == other.pgtable_id_ &&
         HashedComponentsEqual(other) &&
         range_group_ == other.range_group_;
}

bool DocKey::HashedComponentsEqual(const DocKey& other) const {
  return hash_present_ == other.hash_present_ &&
      // Only compare hashes and hashed groups if the hash presence flag is set.
      (!hash_present_ || (hash_ == other.hash_ && hashed_group_ == other.hashed_group_));
}

void DocKey::AddRangeComponent(const PrimitiveValue& val) {
  range_group_.push_back(val);
}

void DocKey::SetRangeComponent(const PrimitiveValue& val, int idx) {
  DCHECK_LT(idx, range_group_.size());
  range_group_[idx] = val;
}

int DocKey::CompareTo(const DocKey& other) const {
  int result = CompareUsingLessThan(cotable_id_, other.cotable_id_);
  if (result != 0) return result;

  result = CompareUsingLessThan(pgtable_id_, other.pgtable_id_);
  if (result != 0) return result;

  result = CompareUsingLessThan(hash_present_, other.hash_present_);
  if (result != 0) return result;

  if (hash_present_) {
    result = CompareUsingLessThan(hash_, other.hash_);
    if (result != 0) return result;
  }

  result = CompareVectors(hashed_group_, other.hashed_group_);
  if (result != 0) return result;

  return CompareVectors(range_group_, other.range_group_);
}

DocKey DocKey::FromRedisKey(uint16_t hash, const string &key) {
  DocKey new_doc_key;
  new_doc_key.hash_present_ = true;
  new_doc_key.hash_ = hash;
  new_doc_key.hashed_group_.emplace_back(key);
  return new_doc_key;
}

KeyBytes DocKey::EncodedFromRedisKey(uint16_t hash, const std::string &key) {
  KeyBytes result;
  result.AppendValueType(ValueType::kUInt16Hash);
  result.AppendUInt16(hash);
  result.AppendValueType(ValueType::kString);
  result.AppendString(key);
  result.AppendValueType(ValueType::kGroupEnd);
  result.AppendValueType(ValueType::kGroupEnd);
  DCHECK_EQ(result, FromRedisKey(hash, key).Encode());
  return result;
}

std::string DocKey::DebugSliceToString(Slice slice) {
  DocKey key;
  auto decoded_size = key.DecodeFrom(slice, DocKeyPart::kWholeDocKey, AllowSpecial::kTrue);
  if (!decoded_size.ok()) {
    return decoded_size.status().ToString() + ": " + slice.ToDebugHexString();
  }
  slice.remove_prefix(*decoded_size);
  auto result = key.ToString();
  if (!slice.empty()) {
    result += " + ";
    result += slice.ToDebugHexString();
  }
  return result;
}

// ------------------------------------------------------------------------------------------------
// SubDocKey
// ------------------------------------------------------------------------------------------------

KeyBytes SubDocKey::DoEncode(bool include_hybrid_time) const {
  KeyBytes key_bytes = doc_key_.Encode();
  for (const auto& subkey : subkeys_) {
    subkey.AppendToKey(&key_bytes);
  }
  if (has_hybrid_time() && include_hybrid_time) {
    AppendDocHybridTime(doc_ht_, &key_bytes);
  }
  return key_bytes;
}

namespace {

class DecodeSubDocKeyCallback {
 public:
  explicit DecodeSubDocKeyCallback(boost::container::small_vector_base<Slice>* out) : out_(out) {}

  CHECKED_STATUS DecodeDocKey(Slice* slice) const {
    return DocKey::PartiallyDecode(slice, out_);
  }

  // We don't need subkeys in partial decoding.
  PrimitiveValue* AddSubkey() const {
    return nullptr;
  }

  DocHybridTime& doc_hybrid_time() const {
    return doc_hybrid_time_;
  }

  void DocHybridTimeSlice(Slice slice) const {
    out_->push_back(slice);
  }
 private:
  boost::container::small_vector_base<Slice>* out_;
  mutable DocHybridTime doc_hybrid_time_;
};

} // namespace

Status SubDocKey::PartiallyDecode(Slice* slice, boost::container::small_vector_base<Slice>* out) {
  CHECK_NOTNULL(out);
  return DoDecode(slice, HybridTimeRequired::kTrue, AllowSpecial::kFalse,
                  DecodeSubDocKeyCallback(out));
}

class SubDocKey::DecodeCallback {
 public:
  explicit DecodeCallback(SubDocKey* key) : key_(key) {}

  CHECKED_STATUS DecodeDocKey(Slice* slice) const {
    return key_->doc_key_.DecodeFrom(slice);
  }

  PrimitiveValue* AddSubkey() const {
    key_->subkeys_.emplace_back();
    return &key_->subkeys_.back();
  }

  DocHybridTime& doc_hybrid_time() const {
    return key_->doc_ht_;
  }

  void DocHybridTimeSlice(Slice slice) const {
  }
 private:
  SubDocKey* key_;
};

Status SubDocKey::DecodeFrom(
    Slice* slice, HybridTimeRequired require_hybrid_time, AllowSpecial allow_special) {
  Clear();
  return DoDecode(slice, require_hybrid_time, allow_special, DecodeCallback(this));
}

Result<bool> SubDocKey::DecodeSubkey(Slice* slice) {
  return DecodeSubkey(slice, DummyCallback());
}

template<class Callback>
Result<bool> SubDocKey::DecodeSubkey(Slice* slice, const Callback& callback) {
  if (!slice->empty() && *slice->data() != ValueTypeAsChar::kHybridTime) {
    RETURN_NOT_OK(PrimitiveValue::DecodeKey(slice, callback.AddSubkey()));
    return true;
  }
  return false;
}

template<class Callback>
Status SubDocKey::DoDecode(rocksdb::Slice* slice,
                           const HybridTimeRequired require_hybrid_time,
                           AllowSpecial allow_special,
                           const Callback& callback) {
  if (allow_special && require_hybrid_time) {
    return STATUS(NotSupported,
                  "Not supported to have both require_hybrid_time and allow_special");
  }
  const rocksdb::Slice original_bytes(*slice);

  RETURN_NOT_OK(callback.DecodeDocKey(slice));
  for (;;) {
    if (allow_special && !slice->empty() &&
        IsSpecialValueType(static_cast<ValueType>(slice->cdata()[0]))) {
      callback.doc_hybrid_time() = DocHybridTime::kInvalid;
      return Status::OK();
    }
    auto decode_result = DecodeSubkey(slice, callback);
    RETURN_NOT_OK_PREPEND(
        decode_result,
        Substitute("While decoding SubDocKey $0", ToShortDebugStr(original_bytes)));
    if (!decode_result.get()) {
      break;
    }
  }
  if (slice->empty()) {
    if (!require_hybrid_time) {
      callback.doc_hybrid_time() = DocHybridTime::kInvalid;
      return Status::OK();
    }
    return STATUS_SUBSTITUTE(
        Corruption,
        "Found too few bytes in the end of a SubDocKey for a type-prefixed hybrid_time: $0",
        ToShortDebugStr(*slice));
  }

  // The reason the following is not handled as a Status is that the logic above (loop + emptiness
  // check) should guarantee this is the only possible case left.
  DCHECK_EQ(ValueType::kHybridTime, DecodeValueType(*slice));
  slice->consume_byte();

  auto begin = slice->data();
  RETURN_NOT_OK(ConsumeHybridTimeFromKey(slice, &callback.doc_hybrid_time()));
  callback.DocHybridTimeSlice(Slice(begin, slice->data()));

  return Status::OK();
}

Status SubDocKey::FullyDecodeFrom(const rocksdb::Slice& slice,
                                  HybridTimeRequired require_hybrid_time) {
  rocksdb::Slice mutable_slice = slice;
  RETURN_NOT_OK(DecodeFrom(&mutable_slice, require_hybrid_time));
  if (!mutable_slice.empty()) {
    return STATUS_SUBSTITUTE(InvalidArgument,
        "Expected all bytes of the slice to be decoded into SubDocKey, found $0 extra bytes: $1",
        mutable_slice.size(), mutable_slice.ToDebugHexString());
  }
  return Status::OK();
}

Status SubDocKey::DecodePrefixLengths(
    Slice slice, boost::container::small_vector_base<size_t>* out) {
  auto begin = slice.data();
  auto hashed_part_size = VERIFY_RESULT(DocKey::EncodedSize(slice, DocKeyPart::kUpToHash));
  if (hashed_part_size != 0) {
    slice.remove_prefix(hashed_part_size);
    out->push_back(hashed_part_size);
  }
  while (VERIFY_RESULT(ConsumePrimitiveValueFromKey(&slice))) {
    out->push_back(slice.data() - begin);
  }
  if (!out->empty()) {
    if (begin[out->back()] != ValueTypeAsChar::kGroupEnd) {
      return STATUS_FORMAT(Corruption, "Range keys group end expected at $0 in $1",
                           out->back(), Slice(begin, slice.end()).ToDebugHexString());
    }
    ++out->back(); // Add range key group end to last prefix
  }
  while (VERIFY_RESULT(SubDocKey::DecodeSubkey(&slice))) {
    out->push_back(slice.data() - begin);
  }

  return Status::OK();
}

Status SubDocKey::DecodeDocKeyAndSubKeyEnds(
    Slice slice, boost::container::small_vector_base<size_t>* out) {
  auto begin = slice.data();
  if (out->empty()) {
    auto id_size = VERIFY_RESULT(DocKey::EncodedSize(slice, DocKeyPart::kUpToId));
    out->push_back(id_size);
  }
  if (out->size() == 1) {
    auto id_size = out->front();
    SCHECK_GE(slice.size(), id_size + 1, Corruption,
              Format("Cannot have exclusively ID in key $0", slice.ToDebugHexString()));
    // Identify table tombstone.
    if (slice[0] == ValueTypeAsChar::kPgTableOid && slice[id_size] == ValueTypeAsChar::kGroupEnd) {
      SCHECK_GE(slice.size(), id_size + 2, Corruption,
                Format("Space for kHybridTime expected in key $0", slice.ToDebugHexString()));
      SCHECK_EQ(slice[id_size + 1], ValueTypeAsChar::kHybridTime, Corruption,
                Format("Hybrid time expected in key $0", slice.ToDebugHexString()));
      // Consume kGroupEnd without pushing to out because the empty key of a table tombstone
      // shouldn't count as an end.
      slice.remove_prefix(id_size + 1);
    } else {
      auto doc_key_size = VERIFY_RESULT(DocKey::EncodedSize(slice, DocKeyPart::kWholeDocKey));
      slice.remove_prefix(doc_key_size);
      out->push_back(doc_key_size);
    }
  } else {
    slice.remove_prefix(out->back());
  }
  while (VERIFY_RESULT(SubDocKey::DecodeSubkey(&slice))) {
    out->push_back(slice.data() - begin);
  }

  return Status::OK();
}

std::string SubDocKey::DebugSliceToString(Slice slice) {
  auto r = DebugSliceToStringAsResult(slice);
  if (r.ok()) {
    return r.get();
  }
  return r.status().ToString();
}

Result<std::string> SubDocKey::DebugSliceToStringAsResult(Slice slice) {
  SubDocKey key;
  auto status = key.DecodeFrom(&slice, HybridTimeRequired::kFalse, AllowSpecial::kTrue);
  if (status.ok()) {
    if (slice.empty()) {
      return key.ToString();
    }
    return key.ToString() + "+" + slice.ToDebugHexString();
  }
  return status;
}

string SubDocKey::ToString() const {
  std::stringstream result;
  result << "SubDocKey(" << doc_key_.ToString() << ", [";

  bool need_comma = false;
  for (const auto& subkey : subkeys_) {
    if (need_comma) {
      result << ", ";
    }
    need_comma = true;
    result << subkey.ToString();
  }

  if (has_hybrid_time()) {
    if (need_comma) {
      result << "; ";
    }
    result << doc_ht_.ToString();
  }
  result << "])";
  return result.str();
}

Status SubDocKey::FromDocPath(const DocPath& doc_path) {
  RETURN_NOT_OK(doc_key_.FullyDecodeFrom(doc_path.encoded_doc_key().AsSlice()));
  subkeys_ = doc_path.subkeys();
  return Status::OK();
}

void SubDocKey::Clear() {
  doc_key_.Clear();
  subkeys_.clear();
  doc_ht_ = DocHybridTime::kInvalid;
}

bool SubDocKey::StartsWith(const SubDocKey& prefix) const {
  return doc_key_ == prefix.doc_key_ &&
         // Subkeys precede the hybrid_time field in the encoded representation, so the hybrid_time
         // either has to be undefined in the prefix, or the entire key must match, including
         // subkeys and the hybrid_time (in this case the prefix is the same as this key).
         (!prefix.has_hybrid_time() ||
          (doc_ht_ == prefix.doc_ht_ && prefix.num_subkeys() == num_subkeys())) &&
         prefix.num_subkeys() <= num_subkeys() &&
         // std::mismatch finds the first difference between two sequences. Prior to C++14, the
         // behavior is undefined if the second range is shorter than the first range, so we make
         // sure the potentially shorter range is first.
         std::mismatch(
             prefix.subkeys_.begin(), prefix.subkeys_.end(), subkeys_.begin()
         ).first == prefix.subkeys_.end();
}

bool SubDocKey::operator==(const SubDocKey& other) const {
  if (doc_key_ != other.doc_key_ ||
      subkeys_ != other.subkeys_)
    return false;

  const bool ht_is_valid = doc_ht_.is_valid();
  const bool other_ht_is_valid = other.doc_ht_.is_valid();
  if (ht_is_valid != other_ht_is_valid)
    return false;
  if (ht_is_valid) {
    return doc_ht_ == other.doc_ht_;
  } else {
    // Both keys don't have a hybrid time.
    return true;
  }
}

int SubDocKey::CompareTo(const SubDocKey& other) const {
  int result = CompareToIgnoreHt(other);
  if (result != 0) return result;

  const bool ht_is_valid = doc_ht_.is_valid();
  const bool other_ht_is_valid = other.doc_ht_.is_valid();
  if (ht_is_valid) {
    if (other_ht_is_valid) {
      // HybridTimes are sorted in reverse order.
      return -doc_ht_.CompareTo(other.doc_ht_);
    } else {
      // This key has a hybrid time and the other one is identical but lacks the hybrid time, so
      // this one is greater.
      return 1;
    }
  } else {
    if (other_ht_is_valid) {
      // This key is a "prefix" of the other key, which has a hybrid time, so this one is less.
      return -1;
    } else {
      // Neither key has a hybrid time.
      return 0;
    }
  }

}

int SubDocKey::CompareToIgnoreHt(const SubDocKey& other) const {
  int result = doc_key_.CompareTo(other.doc_key_);
  if (result != 0) return result;

  result = CompareVectors(subkeys_, other.subkeys_);
  return result;
}

string BestEffortDocDBKeyToStr(const KeyBytes &key_bytes) {
  rocksdb::Slice mutable_slice(key_bytes.AsSlice());
  SubDocKey subdoc_key;
  Status decode_status = subdoc_key.DecodeFrom(
      &mutable_slice, HybridTimeRequired::kFalse, AllowSpecial::kTrue);
  if (decode_status.ok()) {
    ostringstream ss;
    if (!subdoc_key.has_hybrid_time() && subdoc_key.num_subkeys() == 0) {
      // This is really just a DocKey.
      ss << subdoc_key.doc_key().ToString();
    } else {
      ss << subdoc_key.ToString();
    }
    if (mutable_slice.size() > 0) {
      ss << "+" << mutable_slice.ToDebugString();
      // Can append the above status of why we could not decode a SubDocKey, if needed.
    }
    return ss.str();
  }

  // We could not decode a SubDocKey at all, even without a hybrid_time.
  return key_bytes.ToString();
}

std::string BestEffortDocDBKeyToStr(const rocksdb::Slice& slice) {
  return BestEffortDocDBKeyToStr(KeyBytes(slice));
}

KeyBytes SubDocKey::AdvanceOutOfSubDoc() const {
  KeyBytes subdoc_key_no_ts = EncodeWithoutHt();
  subdoc_key_no_ts.AppendValueType(ValueType::kMaxByte);
  return subdoc_key_no_ts;
}

KeyBytes SubDocKey::AdvanceOutOfDocKeyPrefix() const {
  // To construct key bytes that will seek past this DocKey and DocKeys that have the same hash
  // components but add more range components to it, we will strip the group-end of the range
  // components and append 0xff, which will be lexicographically higher than any key bytes
  // with the same hash and range component prefix. For example,
  //
  // DocKey(0x1234, ["aa", "bb"], ["cc", "dd"])
  // Encoded: H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00!
  // Result:  H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00\xff
  // This key will also skip all DocKeys that have additional range components, e.g.
  // DocKey(0x1234, ["aa", "bb"], ["cc", "dd", "ee"])
  // (encoded as H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00$ee\x00\00!). That should
  // make no difference to DocRowwiseIterator in a valid database, because all keys actually stored
  // in DocDB will have exactly the same number of range components.
  //
  // Now, suppose there are no range components in the key passed to us (note: that does not
  // necessarily mean there are no range components in the schema, just the doc key being passed to
  // us is a custom-constructed DocKey with no range components because the caller wants a key
  // that will skip pass all doc keys with the same hash components prefix). Example:
  //
  // DocKey(0x1234, ["aa", "bb"], [])
  // Encoded: H\0x12\0x34$aa\x00\x00$bb\x00\x00!!
  // Result: H\0x12\0x34$aa\x00\x00$bb\x00\x00!\xff
  KeyBytes doc_key_encoded = doc_key_.Encode();
  doc_key_encoded.RemoveValueTypeSuffix(ValueType::kGroupEnd);
  doc_key_encoded.AppendValueType(ValueType::kMaxByte);
  return doc_key_encoded;
}

// ------------------------------------------------------------------------------------------------
// DocDbAwareFilterPolicy
// ------------------------------------------------------------------------------------------------

namespace {

template<DocKeyPart doc_key_part>
class DocKeyComponentsExtractor : public rocksdb::FilterPolicy::KeyTransformer {
 public:
  DocKeyComponentsExtractor(const DocKeyComponentsExtractor&) = delete;
  DocKeyComponentsExtractor& operator=(const DocKeyComponentsExtractor&) = delete;

  static DocKeyComponentsExtractor& GetInstance() {
    static DocKeyComponentsExtractor<doc_key_part> instance;
    return instance;
  }

  // For encoded DocKey extracts specified part, for non-DocKey returns empty key, so they will
  // always match the filter (this is correct, but might be optimized for performance if/when
  // needed).
  // As of 2020-05-12 intents DB could contain keys in non-DocKey format.
  Slice Transform(Slice key) const override {
    auto size_result = DocKey::EncodedSize(key, doc_key_part);
    return size_result.ok() ? Slice(key.data(), *size_result) : Slice();
  }

 private:
  DocKeyComponentsExtractor() = default;
};

} // namespace

void DocDbAwareFilterPolicyBase::CreateFilter(
    const rocksdb::Slice* keys, int n, std::string* dst) const {
  CHECK_GT(n, 0);
  return builtin_policy_->CreateFilter(keys, n, dst);
}

bool DocDbAwareFilterPolicyBase::KeyMayMatch(
    const rocksdb::Slice& key, const rocksdb::Slice& filter) const {
  return builtin_policy_->KeyMayMatch(key, filter);
}

rocksdb::FilterBitsBuilder* DocDbAwareFilterPolicyBase::GetFilterBitsBuilder() const {
  return builtin_policy_->GetFilterBitsBuilder();
}

rocksdb::FilterBitsReader* DocDbAwareFilterPolicyBase::GetFilterBitsReader(
    const rocksdb::Slice& contents) const {
  return builtin_policy_->GetFilterBitsReader(contents);
}

rocksdb::FilterPolicy::FilterType DocDbAwareFilterPolicyBase::GetFilterType() const {
  return builtin_policy_->GetFilterType();
}

const rocksdb::FilterPolicy::KeyTransformer*
DocDbAwareHashedComponentsFilterPolicy::GetKeyTransformer() const {
  return &DocKeyComponentsExtractor<DocKeyPart::kUpToHash>::GetInstance();
}

const rocksdb::FilterPolicy::KeyTransformer*
DocDbAwareV2FilterPolicy::GetKeyTransformer() const {
  // We want for DocDbAwareV2FilterPolicy to disable bloom filtering during read path for
  // range-partitioned tablets (see https://github.com/yugabyte/yugabyte-db/issues/6435).
  return &DocKeyComponentsExtractor<DocKeyPart::kUpToHash>::GetInstance();
}

const rocksdb::FilterPolicy::KeyTransformer*
DocDbAwareV3FilterPolicy::GetKeyTransformer() const {
  return &DocKeyComponentsExtractor<DocKeyPart::kUpToHashOrFirstRange>::GetInstance();
}

DocKeyEncoderAfterTableIdStep DocKeyEncoder::CotableId(const Uuid& cotable_id) {
  if (!cotable_id.IsNil()) {
    std::string bytes;
    cotable_id.EncodeToComparable(&bytes);
    out_->AppendValueType(ValueType::kTableId);
    out_->AppendRawBytes(bytes);
  }
  return DocKeyEncoderAfterTableIdStep(out_);
}

DocKeyEncoderAfterTableIdStep DocKeyEncoder::PgtableId(const PgTableOid pgtable_id) {
  if (pgtable_id > 0) {
    out_->AppendValueType(ValueType::kPgTableOid);
    out_->AppendUInt32(pgtable_id);
  }
  return DocKeyEncoderAfterTableIdStep(out_);
}

DocKeyEncoderAfterTableIdStep DocKeyEncoder::Schema(const class Schema& schema) {
  if (schema.pgtable_id() > 0) {
    return PgtableId(schema.pgtable_id());
  } else {
    return CotableId(schema.cotable_id());
  }
}

Result<bool> DocKeyDecoder::DecodeCotableId(Uuid* uuid) {
  if (!input_.TryConsumeByte(ValueTypeAsChar::kTableId)) {
    return false;
  }

  if (input_.size() < kUuidSize) {
    return STATUS_FORMAT(
        Corruption, "Not enough bytes for cotable id: $0", input_.ToDebugHexString());
  }

  if (uuid) {
    RETURN_NOT_OK(uuid->DecodeFromComparableSlice(Slice(input_.data(), kUuidSize)));
  }
  input_.remove_prefix(kUuidSize);

  return true;
}

Result<bool> DocKeyDecoder::DecodePgtableId(PgTableOid* pgtable_id) {
  if (input_.empty() || input_[0] != ValueTypeAsChar::kPgTableOid) {
    return false;
  }

  input_.consume_byte();

  if (input_.size() < sizeof(PgTableOid)) {
    return STATUS_FORMAT(
        Corruption, "Not enough bytes for pgtable id: $0", input_.ToDebugHexString());
  }

  static_assert(
      sizeof(PgTableOid) == sizeof(uint32_t),
      "It looks like the pgtable ID's size has changed -- need to update encoder/decoder.");
  if (pgtable_id) {
    *pgtable_id = BigEndian::Load32(input_.data());
  }
  input_.remove_prefix(sizeof(PgTableOid));

  return true;
}

Result<bool> DocKeyDecoder::DecodeHashCode(uint16_t* out, AllowSpecial allow_special) {
  if (input_.empty()) {
    return false;
  }

  auto first_value_type = static_cast<ValueType>(input_[0]);

  auto good_value_type = allow_special ? IsPrimitiveOrSpecialValueType(first_value_type)
                                       : IsPrimitiveValueType(first_value_type);
  if (first_value_type == ValueType::kGroupEnd) {
    return false;
  }

  if (!good_value_type) {
    return STATUS_FORMAT(Corruption,
        "Expected first value type to be primitive or GroupEnd, got $0 in $1",
        first_value_type, input_.ToDebugHexString());
  }

  if (input_.empty() || input_[0] != ValueTypeAsChar::kUInt16Hash) {
    return false;
  }

  if (input_.size() < sizeof(DocKeyHash) + 1) {
    return STATUS_FORMAT(
        Corruption,
        "Could not decode a 16-bit hash component of a document key: only $0 bytes left",
        input_.size());
  }

  // We'll need to update this code if we ever change the size of the hash field.
  static_assert(sizeof(DocKeyHash) == sizeof(uint16_t),
      "It looks like the DocKeyHash's size has changed -- need to update encoder/decoder.");
  if (out) {
    *out = BigEndian::Load16(input_.data() + 1);
  }
  input_.remove_prefix(sizeof(DocKeyHash) + 1);
  return true;
}

Status DocKeyDecoder::DecodePrimitiveValue(PrimitiveValue* out, AllowSpecial allow_special) {
  if (allow_special &&
      !input_.empty() &&
      (input_[0] == ValueTypeAsChar::kLowest || input_[0] == ValueTypeAsChar::kHighest)) {
    input_.consume_byte();
    return Status::OK();
  }
  return PrimitiveValue::DecodeKey(&input_, out);
}

Status DocKeyDecoder::ConsumeGroupEnd() {
  if (input_.empty() || input_[0] != ValueTypeAsChar::kGroupEnd) {
    return STATUS_FORMAT(Corruption, "Group end expected but $0 found", input_.ToDebugHexString());
  }
  input_.consume_byte();
  return Status::OK();
}

bool DocKeyDecoder::GroupEnded() const {
  return input_.empty() || input_[0] == ValueTypeAsChar::kGroupEnd;
}

Result<bool> DocKeyDecoder::HasPrimitiveValue() {
  return docdb::HasPrimitiveValue(&input_, AllowSpecial::kFalse);
}

Status DocKeyDecoder::DecodeToRangeGroup() {
  RETURN_NOT_OK(DecodeCotableId());
  RETURN_NOT_OK(DecodePgtableId());
  if (VERIFY_RESULT(DecodeHashCode())) {
    while (VERIFY_RESULT(HasPrimitiveValue())) {
      RETURN_NOT_OK(DecodePrimitiveValue());
    }
  }

  return Status::OK();
}

Result<bool> ClearRangeComponents(KeyBytes* out, AllowSpecial allow_special) {
  auto prefix_size = VERIFY_RESULT(
      DocKey::EncodedSize(out->AsSlice(), DocKeyPart::kUpToHash, allow_special));
  auto& str = *out->mutable_data();
  if (str.size() == prefix_size + 1 && str[prefix_size] == ValueTypeAsChar::kGroupEnd) {
    return false;
  }
  if (str.size() > prefix_size) {
    str[prefix_size] = ValueTypeAsChar::kGroupEnd;
    str.Truncate(prefix_size + 1);
  } else {
    str.PushBack(ValueTypeAsChar::kGroupEnd);
  }
  return true;
}

Result<bool> HashedOrFirstRangeComponentsEqual(const Slice& lhs, const Slice& rhs) {
  DocKeyDecoder lhs_decoder(lhs);
  DocKeyDecoder rhs_decoder(rhs);
  RETURN_NOT_OK(lhs_decoder.DecodeCotableId());
  RETURN_NOT_OK(rhs_decoder.DecodeCotableId());
  RETURN_NOT_OK(lhs_decoder.DecodePgtableId());
  RETURN_NOT_OK(rhs_decoder.DecodePgtableId());

  const bool hash_present = VERIFY_RESULT(lhs_decoder.DecodeHashCode(AllowSpecial::kTrue));
  if (hash_present != VERIFY_RESULT(rhs_decoder.DecodeHashCode(AllowSpecial::kTrue))) {
    return false;
  }

  size_t consumed = lhs_decoder.ConsumedSizeFrom(lhs.data());
  if (consumed != rhs_decoder.ConsumedSizeFrom(rhs.data()) ||
      !strings::memeq(lhs.data(), rhs.data(), consumed)) {
    return false;
  }

  // Check all hashed components if present or first range component otherwise.
  int num_components_to_check = hash_present ? kNumValuesNoLimit : 1;

  while (!lhs_decoder.GroupEnded() && num_components_to_check > 0) {
    auto lhs_start = lhs_decoder.left_input().data();
    auto rhs_start = rhs_decoder.left_input().data();
    auto value_type = lhs_start[0];
    if (rhs_decoder.GroupEnded() || rhs_start[0] != value_type) {
      return false;
    }

    if (PREDICT_FALSE(!IsPrimitiveOrSpecialValueType(static_cast<ValueType>(value_type)))) {
      return false;
    }

    RETURN_NOT_OK(lhs_decoder.DecodePrimitiveValue(AllowSpecial::kTrue));
    RETURN_NOT_OK(rhs_decoder.DecodePrimitiveValue(AllowSpecial::kTrue));
    consumed = lhs_decoder.ConsumedSizeFrom(lhs_start);
    if (consumed != rhs_decoder.ConsumedSizeFrom(rhs_start) ||
        !strings::memeq(lhs_start, rhs_start, consumed)) {
      return false;
    }
    --num_components_to_check;
  }
  if (num_components_to_check == 0) {
    // We don't care about difference in rest of range components.
    return true;
  }

  return rhs_decoder.GroupEnded();
}

bool DocKeyBelongsTo(Slice doc_key, const Schema& schema) {
  bool has_table_id = !doc_key.empty() &&
      (doc_key[0] == ValueTypeAsChar::kTableId || doc_key[0] == ValueTypeAsChar::kPgTableOid);

  if (schema.cotable_id().IsNil() && schema.pgtable_id() == 0) {
    return !has_table_id;
  }

  if (!has_table_id) {
    return false;
  }

  if (doc_key[0] == ValueTypeAsChar::kTableId) {
    doc_key.consume_byte();

    uint8_t bytes[kUuidSize];
    schema.cotable_id().EncodeToComparable(bytes);
    return doc_key.starts_with(Slice(bytes, kUuidSize));
  } else {
    DCHECK(doc_key[0] == ValueTypeAsChar::kPgTableOid);
    doc_key.consume_byte();
    char buf[sizeof(PgTableOid)];
    BigEndian::Store32(buf, schema.pgtable_id());
    return doc_key.starts_with(Slice(buf, sizeof(PgTableOid)));
  }
}

Result<boost::optional<DocKeyHash>> DecodeDocKeyHash(const Slice& encoded_key) {
  DocKey key;
  RETURN_NOT_OK(key.DecodeFrom(encoded_key, DocKeyPart::kUpToHashCode));
  return key.has_hash() ? key.hash() : boost::optional<DocKeyHash>();
}

const KeyBounds KeyBounds::kNoBounds;

}  // namespace docdb
}  // namespace yb
