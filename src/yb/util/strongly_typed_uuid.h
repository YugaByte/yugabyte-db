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

#ifndef YB_UTIL_STRONGLY_TYPED_UUID_H
#define YB_UTIL_STRONGLY_TYPED_UUID_H

#include <random>

#include <boost/optional.hpp>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/nil_generator.hpp>
#include <boost/uuid/random_generator.hpp>

#include "yb/gutil/endian.h"

#include "yb/util/result.h"

// A "strongly-typed UUID" tool. This is needed to prevent passing the wrong UUID as a
// function parameter, and to make callsites more readable by enforcing that MyUuidType is
// specified instead of just UUID. Conversion from strongly-typed UUIDs
// to regular UUIDs is automatic, but the reverse conversion is always explicit.
#define YB_STRONGLY_TYPED_UUID(TypeName) \
  struct BOOST_PP_CAT(TypeName, _Tag); \
  typedef ::yb::StronglyTypedUuid<BOOST_PP_CAT(TypeName, _Tag)> TypeName; \
  typedef boost::hash<TypeName> BOOST_PP_CAT(TypeName, Hash); \
  inline Result<TypeName> BOOST_PP_CAT(FullyDecode, TypeName)(const Slice& slice) { \
    return TypeName(VERIFY_RESULT(yb::FullyDecodeUuid(slice))); \
  } \
  inline TypeName BOOST_PP_CAT(TryFullyDecode, TypeName)(const Slice& slice) { \
    return TypeName(yb::TryFullyDecodeUuid(slice)); \
  } \
  inline Result<TypeName> BOOST_PP_CAT(Decode, TypeName)(Slice* slice) { \
    return TypeName(VERIFY_RESULT(yb::DecodeUuid(slice))); \
  }

namespace yb {

template <class Tag>
class StronglyTypedUuid {
 public:
  // This is public so that we can construct a strongly-typed UUID value out of a regular one.
  // In that case we'll have to spell out the class name, which will enforce readability.
  explicit StronglyTypedUuid(const boost::uuids::uuid& uuid) : uuid_(uuid) {}

  StronglyTypedUuid<Tag>(uint64_t pb1, uint64_t pb2) {
    pb1 = LittleEndian::FromHost64(pb1);
    pb2 = LittleEndian::FromHost64(pb2);
    memcpy(uuid_.data, &pb1, sizeof(pb1));
    memcpy(uuid_.data + sizeof(pb1), &pb2, sizeof(pb2));
  }

  // Gets the underlying UUID, only if not undefined.
  const boost::uuids::uuid& operator *() const {
    return uuid_;
  }

  // Converts a UUID to a string, returns "<Undefined{ClassName}>" if UUID is undefined, where
  // {ClassName} is the name associated with the Tag class.
  std::string ToString() const {
    return to_string(uuid_);
  }

  // Returns true iff the UUID is nil.
  bool IsNil() const {
    return uuid_.is_nil();
  }

  explicit operator bool() const {
    return !IsNil();
  }

  bool operator!() const {
    return IsNil();
  }

  // Represent UUID as pair of uint64 for protobuf serialization.
  // This serialization is independent of the byte order on the machine.
  // For instance we could convert UUID to pair of uint64 on little endian machine, transfer them
  // to big endian machine and UUID created from them will be the same.
  std::pair<uint64_t, uint64_t> ToUInt64Pair() const {
    std::pair<uint64_t, uint64_t> result;
    memcpy(&result.first, uuid_.data, sizeof(result.first));
    memcpy(&result.second, uuid_.data + sizeof(result.first), sizeof(result.second));
    return std::pair<uint64_t, uint64_t>(
        LittleEndian::ToHost64(result.first), LittleEndian::ToHost64(result.second));
  }

  // Represents an invalid UUID.
  static StronglyTypedUuid<Tag> Nil() {
    return StronglyTypedUuid(boost::uuids::nil_uuid());
  }

  // Converts a string to a StronglyTypedUuid, if such a conversion exists.
  // The empty string maps to undefined.
  static Result<StronglyTypedUuid<Tag>> FromString(const std::string& strval) {
    if (strval.empty()) {
      return StronglyTypedUuid<Tag>::Nil();
    }
    try {
      return StronglyTypedUuid(boost::lexical_cast<boost::uuids::uuid>(strval));
    } catch (std::exception& e) {
      return STATUS_FORMAT(
          InvalidArgument, "String '$0' cannot be converted to a uuid: $1", strval, e.what());
    }
  }

  // Generate a random StronglyTypedUuid.
  static StronglyTypedUuid<Tag> GenerateRandom() {
    return StronglyTypedUuid(boost::uuids::random_generator()());
  }

  static StronglyTypedUuid<Tag> GenerateRandom(std::mt19937_64* rng) {
    return StronglyTypedUuid(boost::uuids::basic_random_generator<std::mt19937_64>(rng)());
  }

  uint8_t* data() {
    return uuid_.data;
  }

  const uint8_t* data() const {
    return uuid_.data;
  }

  size_t size() const {
    return uuid_.size();
  }

  Slice AsSlice() const {
    return Slice(uuid_.data, uuid_.size());
  }

  static size_t StaticSize() {
    return boost::uuids::uuid::static_size();
  }

  static size_t StaticStringSize() {
    return 36;
  }

 private:
  // Represented as an optional UUID.
  boost::uuids::uuid uuid_;
};

template <class Tag>
std::ostream& operator << (std::ostream& out, const StronglyTypedUuid<Tag>& uuid) {
  out << *uuid;
  return out;
}

template <class Tag>
bool operator == (const StronglyTypedUuid<Tag>& lhs, const StronglyTypedUuid<Tag>& rhs) noexcept {
  return *lhs == *rhs;
}

template <class Tag>
bool operator != (const StronglyTypedUuid<Tag>& lhs, const StronglyTypedUuid<Tag>& rhs) noexcept {
  return !(lhs == rhs);
}

template <class Tag>
bool operator < (const StronglyTypedUuid<Tag>& lhs, const StronglyTypedUuid<Tag>& rhs) noexcept {
  return *lhs < *rhs;
}

template <class Tag>
bool operator > (const StronglyTypedUuid<Tag>& lhs, const StronglyTypedUuid<Tag>& rhs) noexcept {
  return rhs < lhs;
}

template <class Tag>
bool operator <= (const StronglyTypedUuid<Tag>& lhs, const StronglyTypedUuid<Tag>& rhs) noexcept {
  return !(rhs < lhs);
}

template <class Tag>
bool operator >= (const StronglyTypedUuid<Tag>& lhs, const StronglyTypedUuid<Tag>& rhs) noexcept {
  return !(lhs < rhs);
}

template <class Tag>
std::size_t hash_value(const StronglyTypedUuid<Tag>& u) noexcept {
  return hash_value(*u);
}

Result<boost::uuids::uuid> FullyDecodeUuid(const Slice& slice);
boost::uuids::uuid TryFullyDecodeUuid(const Slice& slice);
Result<boost::uuids::uuid> DecodeUuid(Slice* slice);

} // namespace yb

#endif // YB_UTIL_STRONGLY_TYPED_UUID_H
