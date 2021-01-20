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

#ifndef YB_DOCDB_KEY_BYTES_H_
#define YB_DOCDB_KEY_BYTES_H_

#include <string>

#include "yb/util/slice.h"

#include "yb/common/doc_hybrid_time.h"
#include "yb/common/schema.h"
#include "yb/docdb/doc_kv_util.h"
#include "yb/docdb/value_type.h"
#include "yb/util/bytes_formatter.h"
#include "yb/util/byte_buffer.h"
#include "yb/util/decimal.h"
#include "yb/util/enums.h"

namespace yb {
namespace docdb {

// Represents part (usually a prefix) of a RocksDB key. Has convenience methods for composing keys
// used in our DocDB layer -> RocksDB mapping.
class KeyBytes {
 public:
  KeyBytes() = default;
  explicit KeyBytes(const std::string& data) : data_(data) {}
  explicit KeyBytes(const Slice& slice) : data_(slice) {}

  KeyBytes(const KeyBytes& rhs) = default;
  KeyBytes& operator=(const KeyBytes& rhs) = default;
  KeyBytes(KeyBytes&& rhs) = default;
  KeyBytes& operator=(KeyBytes&& rhs) = default;

  KeyBytes(const Slice& slice, char suffix) {
    data_.reserve(slice.size() + 1);
    data_.append(slice.cdata(), slice.size());
    data_.push_back(suffix);
  }

  KeyBytes(const Slice& slice1, const Slice& slice2) {
    data_.reserve(slice1.size() + slice2.size());
    data_.append(slice1.cdata(), slice1.size());
    data_.append(slice2.cdata(), slice2.size());
  }

  void Reserve(size_t len) {
    data_.reserve(len);
  }

  std::string ToString() const {
    return FormatSliceAsStr(data_.AsSlice());
  }

  bool empty() const {
    return data_.empty();
  }

  std::string ToStringBuffer() const {
    return data().ToString();
  }

  const KeyBuffer& data() const {
    return data_;
  }

  void Append(const KeyBytes& other) {
    data_.append(other.data_);
  }

  void AppendRawBytes(const char* raw_bytes, size_t n) {
    data_.append(raw_bytes, n);
  }

  void AppendRawBytes(const Slice& slice) {
    data_.append(slice);
  }

  void AppendRawBytes(const std::string& data) {
    data_.append(data);
  }

  void AppendValueType(ValueType value_type) {
    data_.push_back(static_cast<char>(value_type));
  }

  void AppendValueTypeBeforeGroupEnd(ValueType value_type) {
    if (data_.empty() || data_.back() != ValueTypeAsChar::kGroupEnd) {
      AppendValueType(value_type);
      AppendValueType(ValueType::kGroupEnd);
    } else {
      data_.back() = static_cast<char>(value_type);
      data_.push_back(ValueTypeAsChar::kGroupEnd);
    }
  }

  void AppendString(const std::string& raw_string) {
    ZeroEncodeAndAppendStrToKey(raw_string, &data_);
  }

  void AppendDescendingString(const std::string &raw_string) {
    ComplementZeroEncodeAndAppendStrToKey(raw_string, &data_);
  }

  void AppendDecimal(const std::string& encoded_decimal_str) {
    data_.append(encoded_decimal_str);
  }

  void AppendDecimalDescending(const std::string& encoded_decimal_str) {
    // Flipping all the bits negates the decimal number this string represents.

    // 0 is a special case because the first two bits are always 10.
    if ((unsigned char)encoded_decimal_str[0] == 128) {
      data_.push_back(encoded_decimal_str[0]);
      return;
    }

    data_.reserve(data_.size() + encoded_decimal_str.size());
    for (auto c : encoded_decimal_str) {
      data_.push_back(~c);
    }
  }

  void AppendVarInt(const std::string& encoded_varint_str) {
    data_.append(encoded_varint_str);
  }

  void AppendUInt64AsVarInt(uint64_t value);

  void AppendVarIntDescending(const std::string& encoded_varint_str) {
    // Flipping all the bits negates the varint number this string represents.

    // 0 is a special case because the first two bits are always 10.
    if (static_cast<uint8_t>(encoded_varint_str[0]) == 128) {
      data_.push_back(encoded_varint_str[0]);
      return;
    }

    for (auto c : encoded_varint_str) {
      data_.push_back(~c);
    }
  }

  void AppendInt64(int64_t x) {
    util::AppendInt64ToKey(x, &data_);
  }

  void AppendUInt64(uint64_t x) {
    AppendUInt64ToKey(x, &data_);
  }

  void AppendDescendingUInt64(int64_t x) {
    AppendUInt64ToKey(~x, &data_);
  }

  void AppendInt32(int32_t x) {
    util::AppendInt32ToKey(x, &data_);
  }

  void AppendUInt32(uint32_t x) {
    AppendUInt32ToKey(x, &data_);
  }

  void AppendDescendingUInt32(int32_t x) {
    AppendUInt32ToKey(~x, &data_);
  }

  void AppendIntentTypeSet(IntentTypeSet intent_type_set) {
    data_.push_back(static_cast<char>(intent_type_set.ToUIntPtr()));
  }

  void AppendDescendingInt64(int64_t x) {
    // AppendInt64ToKey flips the highest bit. We flip all the x's bits before calling
    // AppendInt64ToKey, but the order of the operations (FLIP_HIGHEST_BIT and FLIP_ALL_BITS)
    // doesn't matter because they are commutative operations.
    // Example for an 8-bit integer (works the same for 64-bit integers):
    //    normal encoding (flip first bit)
    //    -128 = 0x80 -> 0x00
    //    -1   = 0xFF -> 0x7F
    //    0    = 0x00 -> 0x80
    //    127  = 0x7F -> 0xFF
    //    (everything compares correctly)
    //
    //    descending encoding (flip all bits, then first, or flip first bit, then all, doesn't
    //    matter which)
    //    -128 = 0x80 -> 0x7F -> 0xFF
    //    -1   = 0xFF -> 0x00 -> 0x80
    //    0    = 0x00 -> 0xFF -> 0x7F
    //    127  = 0x7F -> 0x80 -> 0x00
    util::AppendInt64ToKey(~x, &data_);
  }

  void AppendDescendingInt32(int32_t x) {
    util::AppendInt32ToKey(~x, &data_);
  }

  void AppendUInt16(int16_t x) {
    AppendUInt16ToKey(x, &data_);
  }

  void AppendHybridTime(const DocHybridTime& hybrid_time) {
    hybrid_time.AppendEncodedInDocDbFormat(&data_);
  }

  void AppendColumnId(ColumnId column_id) {
    AppendColumnIdToKey(column_id, &data_);
  }

  void AppendDescendingFloat(float x) {
    util::AppendFloatToKey(x, &data_, /* descending */ true);
  }

  void AppendFloat(float x) {
    util::AppendFloatToKey(x, &data_);
  }

  void AppendDescendingDouble(double x) {
    util::AppendDoubleToKey(x, &data_, /* descending */ true);
  }

  void AppendDouble(double x) {
    util::AppendDoubleToKey(x, &data_);
  }

  void RemoveValueTypeSuffix(ValueType value_type) {
    CHECK_GE(data_.size(), sizeof(char));
    CHECK_EQ(data_.back(), static_cast<char>(value_type));
    data_.pop_back();
  }

  size_t size() const { return data_.size(); }

  bool IsPrefixOf(const rocksdb::Slice& slice) const {
    return slice.starts_with(data_.AsSlice());
  }

  rocksdb::Slice AsSlice() const { return data_.AsSlice(); }

  operator Slice() const { return data_.AsSlice(); }

  void Reset(const Slice& slice) {
    data_.assign(slice.cdata(), slice.size());
  }

  void ResetRawBytes(const char* raw_bytes, size_t n) {
    data_.assign(raw_bytes, n);
  }

  void Clear() {
    data_.clear();
  }

  int CompareTo(const KeyBytes& other) const {
    return data_.AsSlice().compare(other.data_.AsSlice());
  }

  int CompareTo(const Slice& other) const {
    return data_.AsSlice().compare(other);
  }

  // This can be used to e.g. move the internal state of KeyBytes somewhere else, including a
  // string field in a protobuf, without copying the bytes.
  KeyBuffer* mutable_data() {
    return &data_;
  }

  void Truncate(size_t new_size) {
    DCHECK_LE(new_size, data_.size());
    data_.Truncate(new_size);
  }

  void RemoveLastByte() {
    DCHECK(!data_.empty());
    data_.pop_back();
  }

 private:
  KeyBuffer data_;
};

inline bool operator<(const KeyBytes& lhs, const KeyBytes& rhs) {
  return lhs.data() < rhs.data();
}

inline bool operator>=(const KeyBytes& lhs, const KeyBytes& rhs) {
  return !(lhs < rhs);
}

inline bool operator>(const KeyBytes& lhs, const KeyBytes& rhs) {
  return rhs < lhs;
}

inline bool operator<=(const KeyBytes& lhs, const KeyBytes& rhs) {
  return !(rhs < lhs);
}

inline bool operator==(const KeyBytes& lhs, const KeyBytes& rhs) {
  return lhs.AsSlice() == rhs.AsSlice();
}

inline bool operator!=(const KeyBytes& lhs, const KeyBytes& rhs) {
  return lhs.AsSlice() != rhs.AsSlice();
}

void AppendDocHybridTime(const DocHybridTime& time, KeyBytes* key);

}  // namespace docdb
}  // namespace yb

#endif  // YB_DOCDB_KEY_BYTES_H_
