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

#ifndef YB_UTIL_UUID_H
#define YB_UTIL_UUID_H

#include <uuid/uuid.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/system/error_code.hpp>

#include "yb/gutil/strings/substitute.h"
#include "yb/util/status.h"
#include "yb/util/hexdump.h"
#include "yb/util/slice.h"

namespace yb {

constexpr size_t kUuidSize = 16;
constexpr int kShaDigestSize = 5;
// Generic class that UUID type and uses the boost implementation underneath.
// Implements a custom comparator that follows the cassandra implementation.
class Uuid {
 public:
  static constexpr size_t kUuidMsbSize = 8;
  static constexpr size_t kUuidLsbSize = kUuidSize - kUuidMsbSize;
  static constexpr size_t kTimeUUIDMacOffset = 10;
  static constexpr size_t kTimeUUIDTotalMacBytes = 6;

  // The timestamp is a 60-bit value.  For UUID version 1, this is
  // represented by Coordinated Universal Time (UTC) as a count of 100-
  // nanosecond intervals since 00:00:00.00, 15 October 1582 (the date of
  // Gregorian reform to the Christian calendar).
  static constexpr int64_t kGregorianOffsetMillis = -12219292800000;

  static constexpr int64_t kMillisPerHundredNanos = 10000;

  Uuid();

  explicit Uuid(const boost::uuids::uuid& boost_uuid) : boost_uuid_(boost_uuid) {}

  explicit Uuid(const uuid_t copy);

  Uuid(const Uuid& other);

  // Generate a new boost uuid
  static boost::uuids::uuid Generate();

  // Builds an Uuid object given a string representation of the UUID.
  CHECKED_STATUS FromString(const std::string& strval);

  // Fills in strval with the string representation of the UUID.
  CHECKED_STATUS ToString(std::string* strval) const;

  // Returns string representation the UUID. This method doesn't return a
  // Status for usecases in the code where we don't support returning a status.
  std::string ToString() const;

  // Fills in the given string with the raw bytes for the appropriate address in network byte order.
  void ToBytes(std::string* bytes) const;
  void ToBytes(std::array<uint8_t, kUuidSize>* out) const;
  Slice AsSlice() const;

  // Encodes the UUID into the time comparable uuid to be stored in RocksDB.
  void EncodeToComparable(uint8_t* output) const;
  void EncodeToComparable(std::string* bytes) const;

  // Given a string holding the raw bytes in network byte order, it builds the appropriate
  // UUID object.
  CHECKED_STATUS FromBytes(const std::string& bytes);

  // Given a string representation of uuid in hex where the bytes are in host byte order, build
  // an appropriate UUID object.
  CHECKED_STATUS FromHexString(const std::string& hex_string);

  std::string ToHexString() const;

  // Decodes the Comparable UUID bytes into a lexical UUID.
  CHECKED_STATUS DecodeFromComparable(const std::string& bytes);

  // Give a slice holding raw bytes in network byte order, build the appropriate UUID
  // object. If size_hint is specified, it indicates the number of bytes to decode from the slice.
  CHECKED_STATUS FromSlice(const Slice& slice, size_t size_hint = 0);

  CHECKED_STATUS DecodeFromComparableSlice(const Slice& slice, size_t size_hint = 0);

  // For time UUIDs only.
  // This function takes a time UUID and generates a SHA hash for the MAC address bits.
  // This is done because it is not secure to generate UUIDs directly from the MAC address.
  CHECKED_STATUS HashMACAddress();

  // Builds the smallest TimeUUID that willl compare as larger than any TimeUUID with the given
  // timestamp.
  CHECKED_STATUS MaxFromUnixTimestamp(int64_t timestamp_ms);

  // Builds the largest TimeUUID that willl compare as smaller than any TimeUUID with the given
  // timestamp.
  CHECKED_STATUS MinFromUnixTimestamp(int64_t timestamp_ms);

  // This function takes a 64 bit integer that represents the timestamp, that is basically the
  // number of milliseconds since epoch.
  CHECKED_STATUS ToUnixTimestamp(int64_t *timestamp_ms) const;

  CHECKED_STATUS IsTimeUuid() const {
    if (boost_uuid_.version() == boost::uuids::uuid::version_time_based) {
      return Status::OK();
    }
    return STATUS_SUBSTITUTE(InvalidArgument,
                             "Not a type 1 UUID. Current type: $0", boost_uuid_.version());
  }

  bool IsNil() const {
    return boost_uuid_.is_nil();
  }

  bool operator==(const Uuid& other) const {
    return (boost_uuid_ == other.boost_uuid_);
  }

  bool operator!=(const Uuid& other) const {
    return !(*this == other);
  }

  // A custom comparator that compares UUID v1 according to their timestamp.
  // If not, it will compare the version first and then lexicographically.
  bool operator<(const Uuid& other) const {
    // First compare the version, variant and then the timestamp bytes.
    if (boost_uuid_.version() < other.boost_uuid_.version()) {
      return true;
    } else if (boost_uuid_.version() > other.boost_uuid_.version()) {
      return false;
    }
    if (boost_uuid_.version() == boost::uuids::uuid::version_time_based) {
      // Compare the hi timestamp bits.
      for (int i = 6; i < kUuidMsbSize; i++) {
        if (boost_uuid_.data[i] < other.boost_uuid_.data[i]) {
          return true;
        } else if (boost_uuid_.data[i] > other.boost_uuid_.data[i]) {
          return false;
        }
      }
      // Compare the mid timestamp bits.
      for (int i = 4; i < 6; i++) {
        if (boost_uuid_.data[i] < other.boost_uuid_.data[i]) {
          return true;
        } else if (boost_uuid_.data[i] > other.boost_uuid_.data[i]) {
          return false;
        }
      }
      // Compare the low timestamp bits.
      for (int i = 0; i < 4; i++) {
        if (boost_uuid_.data[i] < other.boost_uuid_.data[i]) {
          return true;
        } else if (boost_uuid_.data[i] > other.boost_uuid_.data[i]) {
          return false;
        }
      }
    } else {
      // Compare all the other bits
      for (int i = 0; i < kUuidMsbSize; i++) {
        if (boost_uuid_.data[i] < other.boost_uuid_.data[i]) {
          return true;
        } else if (boost_uuid_.data[i] > other.boost_uuid_.data[i]) {
          return false;
        }
      }
    }

    // Then compare the remaining bytes.
    for (int i = kUuidMsbSize; i < kUuidSize; i++) {
      if (boost_uuid_.data[i] < other.boost_uuid_.data[i]) {
        return true;
      } else if (boost_uuid_.data[i] > other.boost_uuid_.data[i]) {
        return false;
      }
    }
    return false;
  }

  bool operator>(const Uuid& other) const {
    return (other < *this);
  }

  bool operator<=(const Uuid& other) const {
    return !(other < *this);
  }

  bool operator>=(const Uuid& other) const {
    return !(*this < other);
  }

  Uuid& operator=(const Uuid& other) {
    boost_uuid_ = other.boost_uuid_;
    return *this;
  }

 private:
  boost::uuids::uuid boost_uuid_;

  // Encodes the MSB of the uuid into a timestamp based byte stream as follows.
  // [Timestamp Low (32 bits)][Timestamp Mid (16 bits)][Version (4 bits)][Timestamp High (12 bits)]
  // into
  // [Version (4 bits)][Timestamp High (12 bits)][Timestamp Mid (16 bits)][Timestamp Low (32 bits)]
  // So that their lexical comparison of the bytes will result in time based comparison.
  void ToTimestampBytes(uint8_t* output) const {
    output[0] = boost_uuid_.data[6];
    output[1] = boost_uuid_.data[7];
    output[2] = boost_uuid_.data[4];
    output[3] = boost_uuid_.data[5];
    output[4] = boost_uuid_.data[0];
    output[5] = boost_uuid_.data[1];
    output[6] = boost_uuid_.data[2];
    output[7] = boost_uuid_.data[3];
  }

  // Reverse the timestamp based byte stream into regular UUID style MSB.
  // See comments for toTimestampBytes function for more detail.
  void FromTimestampBytes(const uint8_t* input) {
    uint8_t tmp[kUuidMsbSize];
    tmp[0] = input[4];
    tmp[1] = input[5];
    tmp[2] = input[6];
    tmp[3] = input[7];
    tmp[4] = input[2];
    tmp[5] = input[3];
    tmp[6] = input[0];
    tmp[7] = input[1];
    memcpy(boost_uuid_.data, tmp, kUuidMsbSize);
  }

  // Utility method used by minTimeuuid and maxTimeuuid.
  // Set the timestamp bytes of a (time)uuid to the specified value (in hundred nanos).
  // Also ensure that the uuid is a version 1 uuid (i.e. timeuuid) by setting the version.
  void FromTimestamp(int64_t ts_hnanos);

  // Encodes the MSB of the uuid into a version based byte stream as follows.
  // Used for non-time-based UUIDs, i.e. not version 1.
  // [Timestamp Low (32 bits)][Timestamp Mid (16 bits)][Version (4 bits)][Timestamp High (12 bits)]
  // into
  // [Version (4 bits)][Timestamp Low (32 bits)][Timestamp Mid (16 bits)][Timestamp High (12 bits)]
  // So that their lexical comparison of the bytes will result in version based comparison.
  void ToVersionFirstBytes(uint8_t* output) const {
    output[0] = (uint8_t) ( ((boost_uuid_.data[6] & 0xF0))
                          | ((boost_uuid_.data[0] & 0xF0) >> 4));
    output[1] = (uint8_t) ( ((boost_uuid_.data[0] & 0x0F) << 4)
                          | ((boost_uuid_.data[1] & 0xF0) >> 4));
    output[2] = (uint8_t) ( ((boost_uuid_.data[1] & 0x0F) << 4)
                          | ((boost_uuid_.data[2] & 0xF0) >> 4));
    output[3] = (uint8_t) ( ((boost_uuid_.data[2] & 0x0F) << 4)
                          | ((boost_uuid_.data[3] & 0xF0) >> 4));
    output[4] = (uint8_t) ( ((boost_uuid_.data[3] & 0x0F) << 4)
                          | ((boost_uuid_.data[4] & 0xF0) >> 4));
    output[5] = (uint8_t) ( ((boost_uuid_.data[4] & 0x0F) << 4)
                          | ((boost_uuid_.data[5] & 0xF0) >> 4));
    output[6] = (uint8_t) ( ((boost_uuid_.data[5] & 0x0F) << 4)
                          | ((boost_uuid_.data[6] & 0x0F)));
    output[7] = boost_uuid_.data[7];
  }

  // Reverse the version based byte stream into regular UUID style MSB.
  // See comments for toVersionFirstBytes function for more detail.
  void FromVersionFirstBytes(const uint8_t* input) {
    uint8_t tmp[kUuidMsbSize];
    tmp[0] = (uint8_t)  ( ((input[0] & 0x0F) << 4)
                        | ((input[1] & 0xF0) >> 4));
    tmp[1] = (uint8_t)  ( ((input[1] & 0x0F) << 4)
                        | ((input[2] & 0xF0) >> 4));
    tmp[2] = (uint8_t)  ( ((input[2] & 0x0F) << 4)
                        | ((input[3] & 0xF0) >> 4));
    tmp[3] = (uint8_t)  ( ((input[3] & 0x0F) << 4)
                        | ((input[4] & 0xF0) >> 4));
    tmp[4] = (uint8_t)  ( ((input[4] & 0x0F) << 4)
                        | ((input[5] & 0xF0) >> 4));
    tmp[5] = (uint8_t)  ( ((input[5] & 0x0F) << 4)
                        | ((input[6] & 0xF0) >> 4));
    tmp[6] = (uint8_t)  ( ((input[0] & 0xF0))
                        | ((input[6] & 0x0F)));
    tmp[7] = input[7];
    memcpy(boost_uuid_.data, tmp, kUuidMsbSize);
  }

};

} // namespace yb

#endif // YB_UTIL_UUID_H
