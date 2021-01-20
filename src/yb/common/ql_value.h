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
// This file contains the QLValue class that represents QL values.

#ifndef YB_COMMON_QL_VALUE_H
#define YB_COMMON_QL_VALUE_H

#include <stdint.h>

#include "yb/common/pgsql_protocol.pb.h"
#include "yb/common/ql_datatype.h"
#include "yb/common/ql_protocol.pb.h"
#include "yb/common/ql_type.h"

#include "yb/util/decimal.h"
#include "yb/util/net/inetaddress.h"
#include "yb/util/timestamp.h"
#include "yb/util/uuid.h"
#include "yb/util/yb_partition.h"
#include "yb/util/varint.h"

namespace yb {

//--------------------------------------------------------------------------------------------------
void AppendToKey(const QLValuePB &value_pb, std::string *bytes);

//--------------------------------------------------------------------------------------------------
// An abstract class that defines a QL value interface to support different implementations
// for in-memory / serialization trade-offs.
class QLValue {
 public:
  // Shared_ptr.
  typedef std::shared_ptr<QLValue> SharedPtr;

  // Constructors & destructors.
  QLValue() { }
  explicit QLValue(const QLValuePB& pb) : pb_(pb) { }
  explicit QLValue(QLValuePB&& pb) : pb_(std::move(pb)) { }
  virtual ~QLValue();

  //-----------------------------------------------------------------------------------------
  // Access functions to value and type.
  InternalType type() const { return pb_.value_case(); }
  const QLValuePB& value() const { return pb_; }
  QLValuePB* mutable_value() { return &pb_; }

  //------------------------------------ Nullness methods -----------------------------------
  // Is the value null?
  static bool IsNull(const QLValuePB& pb) { return pb.value_case() == QLValuePB::VALUE_NOT_SET; }
  bool IsNull() const { return IsNull(pb_); }
  // Set the value to null by clearing all existing values.
  void SetNull() { pb_.Clear(); }

  //-------------------------------- virtual value methods ----------------------------------
  static bool IsVirtual(const QLValuePB& pb) {
    return pb.value_case() == QLValuePB::kVirtualValue;
  }
  bool IsVirtual() const {
    return type() == QLValuePB::kVirtualValue;
  }
  static bool IsMax(const QLValuePB& pb) {
    return IsVirtual(pb) && pb.virtual_value() == QLVirtualValuePB::LIMIT_MAX;
  }
  static bool IsMin(const QLValuePB& pb) {
    return IsVirtual(pb) && pb.virtual_value() == QLVirtualValuePB::LIMIT_MIN;
  }
  bool IsMax() const { return IsMax(pb_); }
  bool IsMin() const { return IsMin(pb_); }
  void SetMax() { pb_.set_virtual_value(QLVirtualValuePB::LIMIT_MAX); }
  void SetMin() { pb_.set_virtual_value(QLVirtualValuePB::LIMIT_MIN); }

  //----------------------------------- get value methods -----------------------------------
  // Get different datatype values. CHECK failure will result if the value stored is not of the
  // expected datatype or the value is null.

  #define QLVALUE_PRIMITIVE_GETTER(type, name) \
  static type BOOST_PP_CAT(name, _value)(const QLValuePB& pb) { \
    CHECK(pb.BOOST_PP_CAT(has_, BOOST_PP_CAT(name, _value))()) \
        << "Value: " << pb.ShortDebugString(); \
    return pb.BOOST_PP_CAT(name, _value)(); \
  } \
  \
  type BOOST_PP_CAT(name, _value)() const { \
    return BOOST_PP_CAT(name, _value)(pb_); \
  } \

  QLVALUE_PRIMITIVE_GETTER(int8_t, int8);
  QLVALUE_PRIMITIVE_GETTER(int16_t, int16);
  QLVALUE_PRIMITIVE_GETTER(int32_t, int32);
  QLVALUE_PRIMITIVE_GETTER(int64_t, int64);
  QLVALUE_PRIMITIVE_GETTER(uint32_t, uint32);
  QLVALUE_PRIMITIVE_GETTER(uint64_t, uint64);
  QLVALUE_PRIMITIVE_GETTER(float, float);
  QLVALUE_PRIMITIVE_GETTER(double, double);
  QLVALUE_PRIMITIVE_GETTER(bool, bool);
  QLVALUE_PRIMITIVE_GETTER(const std::string&, decimal);
  QLVALUE_PRIMITIVE_GETTER(const std::string&, string);
  QLVALUE_PRIMITIVE_GETTER(const std::string&, binary);
  QLVALUE_PRIMITIVE_GETTER(const std::string&, jsonb);
  QLVALUE_PRIMITIVE_GETTER(uint32_t, date);
  QLVALUE_PRIMITIVE_GETTER(int64_t, time);
  QLVALUE_PRIMITIVE_GETTER(const QLMapValuePB&, map);
  QLVALUE_PRIMITIVE_GETTER(const QLSeqValuePB&, set);
  QLVALUE_PRIMITIVE_GETTER(const QLSeqValuePB&, list);
  QLVALUE_PRIMITIVE_GETTER(const QLSeqValuePB&, frozen);
  #undef QLVALUE_PRIMITIVE_GETTER

  static Timestamp timestamp_value(const QLValuePB& pb) {
    return Timestamp(timestamp_value_pb(pb));
  }

  static int64_t timestamp_value_pb(const QLValuePB& pb) {
    // Caller of this function should already read and know the PB value type before calling.
    CHECK(pb.has_timestamp_value()) << "Value: " << pb.ShortDebugString();
    return pb.timestamp_value();
  }

  Timestamp timestamp_value() const {
    return timestamp_value(pb_);
  }

  int64_t timestamp_value_pb() const {
    return timestamp_value_pb(pb_);
  }

  static const std::string& inetaddress_value_pb(const QLValuePB& pb) {
    CHECK(pb.has_inetaddress_value()) << "Value: " << pb.ShortDebugString();
    return pb.inetaddress_value();
  }

  static InetAddress inetaddress_value(const QLValuePB& pb) {
    InetAddress addr;
    CHECK_OK(addr.FromBytes(inetaddress_value_pb(pb)));
    return addr;
  }

  const std::string& inetaddress_value_pb() const {
    return inetaddress_value_pb(pb_);
  }

  InetAddress inetaddress_value() const {
    return inetaddress_value(pb_);
  }

  static const std::string& uuid_value_pb(const QLValuePB& pb) {
    CHECK(pb.has_uuid_value()) << "Value: " << pb.ShortDebugString();
    return pb.uuid_value();
  }
  static Uuid uuid_value(const QLValuePB& pb) {
    Uuid uuid;
    CHECK_OK(uuid.FromBytes(uuid_value_pb(pb)));
    return uuid;
  }
  const std::string& uuid_value_pb() const {
    return uuid_value_pb(pb_);
  }
  Uuid uuid_value() const {
    return uuid_value(pb_);
  }

  static const std::string& timeuuid_value_pb(const QLValuePB& pb) {
    // Caller of this function should already read and know the PB value type before calling.
    DCHECK(pb.has_timeuuid_value()) << "Value: " << pb.ShortDebugString();
    return pb.timeuuid_value();
  }
  static Uuid timeuuid_value(const QLValuePB& pb) {
    Uuid timeuuid;
    CHECK_OK(timeuuid.FromBytes(timeuuid_value_pb(pb)));
    CHECK_OK(timeuuid.IsTimeUuid());
    return timeuuid;
  }
  const std::string& timeuuid_value_pb() const {
    return timeuuid_value_pb(pb_);
  }
  Uuid timeuuid_value() const {
    return timeuuid_value(pb_);
  }

  static util::VarInt varint_value(const QLValuePB& pb) {
    CHECK(pb.has_varint_value()) << "Value: " << pb.ShortDebugString();
    util::VarInt varint;
    size_t num_decoded_bytes;
    CHECK_OK(varint.DecodeFromComparable(pb.varint_value(), &num_decoded_bytes));
    return varint;
  }
  util::VarInt varint_value() const {
    return varint_value(pb_);
  }

  void AppendToKeyBytes(string *bytes) const {
    AppendToKey(pb_, bytes);
  }

  //----------------------------------- set value methods -----------------------------------
  // Set different datatype values.
  virtual void set_int8_value(int8_t val) {
    pb_.set_int8_value(val);
  }
  virtual void set_int16_value(int16_t val) {
    pb_.set_int16_value(val);
  }
  virtual void set_int32_value(int32_t val) {
    pb_.set_int32_value(val);
  }
  virtual void set_int64_value(int64_t val) {
    pb_.set_int64_value(val);
  }
  virtual void set_uint32_value(uint32_t val) {
    pb_.set_uint32_value(val);
  }
  virtual void set_uint64_value(uint64_t val) {
    pb_.set_uint64_value(val);
  }
  virtual void set_float_value(float val) {
    pb_.set_float_value(val);
  }
  virtual void set_double_value(double val) {
    pb_.set_double_value(val);
  }
  // val is a serialized binary representation of a decimal, not a plaintext
  virtual void set_decimal_value(const std::string& val) {
    pb_.set_decimal_value(val);
  }
  // val is a serialized binary representation of a decimal, not a plaintext
  virtual void set_decimal_value(std::string&& val) {
    pb_.set_decimal_value(std::move(val));
  }
  virtual void set_jsonb_value(std::string&& val) {
    pb_.set_jsonb_value(std::move(val));
  }
  void set_jsonb_value(const std::string& val) {
    pb_.set_jsonb_value(val);
  }
  virtual void set_bool_value(bool val) {
    pb_.set_bool_value(val);
  }
  virtual void set_string_value(const std::string& val) {
    pb_.set_string_value(val);
  }
  virtual void set_string_value(std::string&& val) {
    pb_.set_string_value(std::move(val));
  }
  virtual void set_string_value(const char* val) {
    pb_.set_string_value(val);
  }
  virtual void set_string_value(const char* val, size_t size) {
    pb_.set_string_value(val, size);
  }
  virtual void set_timestamp_value(const Timestamp& val) {
    pb_.set_timestamp_value(val.ToInt64());
  }
  virtual void set_timestamp_value(int64_t val) {
    pb_.set_timestamp_value(val);
  }
  virtual void set_date_value(uint32_t val) {
    pb_.set_date_value(val);
  }
  virtual void set_time_value(int64_t val) {
    pb_.set_time_value(val);
  }
  virtual void set_binary_value(const std::string& val) {
    pb_.set_binary_value(val);
  }
  virtual void set_binary_value(std::string&& val) {
    pb_.set_binary_value(std::move(val));
  }
  virtual void set_binary_value(const void* val, size_t size) {
    pb_.set_binary_value(val, size);
  }

  static void set_inetaddress_value(const InetAddress& val, QLValuePB* pb) {
    CHECK_OK(val.ToBytes(pb->mutable_inetaddress_value()));
  }

  void set_inetaddress_value(const InetAddress& val) {
    set_inetaddress_value(val, &pb_);
  }

  static void set_uuid_value(const Uuid& val, QLValuePB* out) {
    val.ToBytes(out->mutable_uuid_value());
  }

  void set_uuid_value(const Uuid& val) {
    set_uuid_value(val, &pb_);
  }

  virtual void set_timeuuid_value(const Uuid& val) {
    CHECK_OK(val.IsTimeUuid());
    val.ToBytes(pb_.mutable_timeuuid_value());
  }
  virtual void set_varint_value(const util::VarInt& val) {
    pb_.set_varint_value(val.EncodeToComparable());
  }

  //--------------------------------- mutable value methods ----------------------------------
  std::string* mutable_decimal_value() {
    return pb_.mutable_decimal_value();
  }
  std::string* mutable_jsonb_value() {
    return pb_.mutable_jsonb_value();
  }
  std::string* mutable_varint_value() {
    return pb_.mutable_varint_value();
  }
  std::string* mutable_string_value() {
    return pb_.mutable_string_value();
  }
  std::string* mutable_binary_value() {
    return pb_.mutable_binary_value();
  }
  QLMapValuePB* mutable_map_value() {
    return pb_.mutable_map_value();
  }
  QLSeqValuePB* mutable_set_value() {
    return pb_.mutable_set_value();
  }
  QLSeqValuePB* mutable_list_value() {
    return pb_.mutable_list_value();
  }
  QLSeqValuePB* mutable_frozen_value() {
    return pb_.mutable_frozen_value();
  }

  // To extend/construct collections we return freshly allocated elements for the caller to set.
  virtual QLValuePB* add_map_key() {
    return pb_.mutable_map_value()->add_keys();
  }
  virtual QLValuePB* add_map_value() {
    return pb_.mutable_map_value()->add_values();
  }
  virtual QLValuePB* add_set_elem() {
    return pb_.mutable_set_value()->add_elems();
  }
  virtual QLValuePB* add_list_elem() {
    return pb_.mutable_list_value()->add_elems();
  }
  virtual QLValuePB* add_frozen_elem() {
    return pb_.mutable_frozen_value()->add_elems();
  }

  // For collections, the call to `mutable_foo` takes care of setting the correct type to `foo`
  // internally and allocating the message if needed
  // TODO(neil) Change these set to "mutable_xxx_value()".
  virtual void set_map_value() {
    pb_.mutable_map_value();
  }
  virtual void set_set_value() {
    pb_.mutable_set_value();
  }
  virtual void set_list_value() {
    pb_.mutable_list_value();
  }
  virtual void set_frozen_value() {
    pb_.mutable_frozen_value();
  }

  //----------------------------------- assignment methods ----------------------------------
  QLValue& operator=(const QLValuePB& other) {
    pb_ = other;
    return *this;
  }
  QLValue& operator=(QLValuePB&& other) {
    pb_ = std::move(other);
    return *this;
  }

  //----------------------------------- comparison methods -----------------------------------
  virtual bool Comparable(const QLValue& other) const {
    return type() == other.type() || EitherIsNull(other) || EitherIsVirtual(other);
  }
  virtual bool BothNotNull(const QLValue& other) const {
    return !IsNull() && !other.IsNull();
  }
  virtual bool BothNull(const QLValue& other) const {
    return IsNull() && other.IsNull();
  }
  virtual bool EitherIsNull(const QLValue& other) const {
    return IsNull() || other.IsNull();
  }
  virtual bool EitherIsVirtual(const QLValue& other) const {
    return IsVirtual() || other.IsVirtual();
  }

  virtual int CompareTo(const QLValue& other) const;

  // In YCQL null is not comparable with regular values (w.r.t. ordering).
  virtual bool operator <(const QLValue& v) const {
    return BothNotNull(v) && CompareTo(v) < 0;
  }
  virtual bool operator >(const QLValue& v) const {
    return BothNotNull(v) && CompareTo(v) > 0;
  }

  // In YCQL equality holds for null values.
  virtual bool operator <=(const QLValue& v) const {
    return (BothNotNull(v) && CompareTo(v) <= 0) || BothNull(v);
  }
  virtual bool operator >=(const QLValue& v) const {
    return (BothNotNull(v) && CompareTo(v) >= 0) || BothNull(v);
  }
  virtual bool operator ==(const QLValue& v) const {
    return (BothNotNull(v) && CompareTo(v) == 0) || BothNull(v);
  }
  virtual bool operator !=(const QLValue& v) const {
    return !(*this == v);
  }

  //----------------------------- serializer / deserializer ---------------------------------
  static void Serialize(const std::shared_ptr<QLType>& ql_type,
                        const QLClient& client,
                        const QLValuePB& pb,
                        faststring* buffer);

  void Serialize(const std::shared_ptr<QLType>& ql_type,
                 const QLClient& client,
                 faststring* buffer) const;
  CHECKED_STATUS Deserialize(const std::shared_ptr<QLType>& ql_type,
                             const QLClient& client,
                             Slice* data);

  //------------------------------------ debug string ---------------------------------------
  // Return a string for debugging.
  std::string ToString() const;

 private:
  // Deserialize a CQL number (8, 16, 32 and 64-bit integer). <num_type> is the parsed integer type.
  // <converter> converts the number from network byte-order to machine order and <data_type>
  // is the coverter's return type. The converter's return type <data_type> is unsigned while
  // <num_type> may be signed or unsigned. <setter> sets the value in QLValue.
  template<typename num_type, typename data_type>
  CHECKED_STATUS CQLDeserializeNum(
      size_t len, data_type (*converter)(const void*), void (QLValue::*setter)(num_type),
      Slice* data) {
    num_type value = 0;
    RETURN_NOT_OK(CQLDecodeNum(len, converter, data, &value));
    (this->*setter)(value);
    return Status::OK();
  }

  // Deserialize a CQL floating point number (float or double). <float_type> is the parsed floating
  // point type. <converter> converts the number from network byte-order to machine order and
  // <data_type> is the coverter's return type. The converter's return type <data_type> is an
  // integer type. <setter> sets the value in QLValue.
  template<typename float_type, typename data_type>
  CHECKED_STATUS CQLDeserializeFloat(
      size_t len, data_type (*converter)(const void*), void (QLValue::*setter)(float_type),
      Slice* data) {
    float_type value = 0.0;
    RETURN_NOT_OK(CQLDecodeFloat(len, converter, data, &value));
    (this->*setter)(value);
    return Status::OK();
  }

  // TODO(neil) This should be changed to shared_ptr<QLValuePB>. That way, we assign the pointers
  // instead of copying the same value many times during expression evaluation.
  // Protobuf value.
  QLValuePB pb_;
};

//--------------------------------------------------------------------------------------------------
// QLValuePB operators
bool operator <(const QLValuePB& lhs, const QLValuePB& rhs);
bool operator >(const QLValuePB& lhs, const QLValuePB& rhs);
bool operator <=(const QLValuePB& lhs, const QLValuePB& rhs);
bool operator >=(const QLValuePB& lhs, const QLValuePB& rhs);
bool operator ==(const QLValuePB& lhs, const QLValuePB& rhs);
bool operator !=(const QLValuePB& lhs, const QLValuePB& rhs);

bool operator <(const QLValuePB& lhs, const QLValue& rhs);
bool operator >(const QLValuePB& lhs, const QLValue& rhs);
bool operator <=(const QLValuePB& lhs, const QLValue& rhs);
bool operator >=(const QLValuePB& lhs, const QLValue& rhs);
bool operator ==(const QLValuePB& lhs, const QLValue& rhs);
bool operator !=(const QLValuePB& lhs, const QLValue& rhs);

InternalType type(const QLValuePB& v);
bool IsNull(const QLValuePB& v);
void SetNull(QLValuePB* v);
bool EitherIsNull(const QLValuePB& lhs, const QLValuePB& rhs);
bool BothNotNull(const QLValuePB& lhs, const QLValuePB& rhs);
bool BothNull(const QLValuePB& lhs, const QLValuePB& rhs);
bool Comparable(const QLValuePB& lhs, const QLValuePB& rhs);
int Compare(const QLValuePB& lhs, const QLValuePB& rhs);
bool EitherIsNull(const QLValuePB& lhs, const QLValue& rhs);
bool Comparable(const QLValuePB& lhs, const QLValue& rhs);
bool BothNotNull(const QLValuePB& lhs, const QLValue& rhs);
bool BothNull(const QLValuePB& lhs, const QLValue& rhs);
int Compare(const QLValuePB& lhs, const QLValue& rhs);
int Compare(const QLSeqValuePB& lhs, const QLSeqValuePB& rhs);
int Compare(const bool lhs, const bool rhs);

#define YB_SET_INT_VALUE(ql_valuepb, input, bits) \
  case DataType::BOOST_PP_CAT(INT, bits): { \
    auto value = CheckedStoInt<BOOST_PP_CAT(BOOST_PP_CAT(int, bits), _t)>(input); \
    RETURN_NOT_OK(value); \
    ql_valuepb->BOOST_PP_CAT(BOOST_PP_CAT(set_int, bits), _value)(*value); \
  } break;

} // namespace yb

#endif // YB_COMMON_QL_VALUE_H
