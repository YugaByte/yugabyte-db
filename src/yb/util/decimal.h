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

#ifndef YB_UTIL_DECIMAL_H
#define YB_UTIL_DECIMAL_H

#include <vector>
#include <limits>

#include "yb/util/result.h"
#include "yb/util/slice.h"
#include "yb/util/varint.h"

namespace yb {
namespace util {

// The Decimal class can represent decimal numbers (including fractions) of arbitrary length.
//
// The API
// -------
// Typically Decimals should be used for parsing from String or Double, convert to String or Double,
// Serialize or Deserialize. The default constructor (With digit arrays, exponent, sign need not
// be used). It is not necessary to keep a decimal object in memory for long time. The encoded
// string should serve the same purpose, and it is easy to get the decimal from serialized string.
//
// The Serialization format specifications
// -------
// Both serialization formats are unique from a given decimal.
// 1) Comparable Serialization:
//  This is used by our storage layer. The lexicographical byte comparison of this encoding is the
//  same as numerical comparison of numbers. Also it is possible to find the end of the encoding
//  by looking at it. So Decode(slice) gives back the length of the encoding.
//
// 2) BigDecimal Serialization:
//  This gives a serialization to Cassandra's way to serializing Java BigDecimal. The scale
//  component is coded with 4 byte two's complement representation. Then it is followed by a byte
//  array for the corresponding BigInt's serialization.
//  See:
//    https://github.com/apache/cassandra/blob/trunk/doc/native_protocol_v4.spec
//    https://github.com/apache/cassandra/blob/81f6c784ce967fadb6ed7f58de1328e713eaf53c/
//    src/java/org/apache/cassandra/serializers/DecimalSerializer.java
//  Note that the byte buffer for Java BigInt doesn't have a size prefix or something similar. So
//  the length is not known. The Decode(slice) function expects the whole slice to encode the
//  decimal.
//
//  The internal representation in the Decimal class
//  -------
//  A decimal contains a sign bit (bool is_positive_), the exponent part (VarInt exponent_), and
//  a digit (0-9) array (vector<int8_t> digits_). The corresponding number is
//        +- 10^exp// 0.d1d2...dk .
//  A representation is called canonical if and only if d1 != 0 and dk != 0. (If number is zero,
//  then sign must be positive, and digit array empty). Note that every number can have only one
//  canonical representation.
//  Examples: 23.4 = ( is_positive_ = true, exponent_ = 2, digits_ = {2, 3, 4} )
//          -0.0004372 is ( is_positive = false, exponent = -3, digits_ = {4, 3, 7, 2} )
//          2378000 is ( is_positive = true, exponent = 7, digits_ = {2, 3, 7, 8} ).
//  We ensure the state is always canonical, enforced by the make_canonical() function, after
//  converting from String, double, constructor, or decode() the resulting representation must be
//  canonical.
//
//  Converting to string
//  -------
//  There are two ways to convert to string,
//    - PointString Format      (-0.0004372 or 2378000)
//    - Scientific Notation     (-4.372e-4 or 2.378e+6)
//  We have implemented both. Note that the pointstring format is infeasible if the exponent is too
//  large or too small, so scientific notation is used.
//    - The default ToString() function uses PointString format if the output has 10 bytes or less
//    and Scientific notation otherwise (this is a constant defined as kDefaultMaxLength).

class Decimal {
 public:
  static constexpr int kDefaultMaxLength = 20; // Enough for MIN_BIGINT=-9223372036854775808.
  static constexpr int kUnlimitedMaxLength = std::numeric_limits<int>::max();

  Decimal() {}
  Decimal(const std::vector<uint8_t>& digits,
          const VarInt& exponent = VarInt(0),
          bool is_positive = true)
      : digits_(digits), exponent_(exponent), is_positive_(is_positive) { make_canonical(); }
  Decimal(const Decimal& other) : Decimal(other.digits_, other.exponent_, other.is_positive_) {}

  // Ensure the type conversion is possible if you use these constructors. Use FromX() otherwise.
  explicit Decimal(const std::string& string_val) { CHECK_OK(FromString(string_val)); }
  explicit Decimal(double double_val) { CHECK_OK(FromDouble(double_val)); }
  explicit Decimal(const VarInt& varint_val) { CHECK_OK(FromVarInt(varint_val)); }

  void clear();

  std::string ToDebugString() const;
  CHECKED_STATUS ToPointString(std::string* string_val, int max_length = kDefaultMaxLength) const;
  std::string ToScientificString() const;
  std::string ToString() const;
  // Note: We are using decimal -> string -> double using std::stod() function.
  // In future, it may be better to write a direct conversion function.
  Result<long double> ToDouble() const;

  Result<VarInt> ToVarInt() const;

  // The FromX() functions always create a canonical Decimal,
  // but the (digits, varint, sign) constructor doesn't.

  // The input is expected to be of the form [+-]?[0-9]*('.'[0-9]*)?([eE][+-]?[0-9]+)?,
  // whitespace is not allowed. Use this after removing whitespace.
  CHECKED_STATUS FromString(const Slice &slice);

  // Note: We are using double -> string -> decimal using std::to_string() function.
  // In future, it may be better to write a direct conversion function.
  CHECKED_STATUS FromDouble(double double_val);
  CHECKED_STATUS FromVarInt(const VarInt& varint_val);

  // Checks if this is a whole number. Assumes canonical.
  bool is_integer() const;

  // <0, =0, >0 if this <,=,> other numerically. Assumes canonical.
  int CompareTo(const Decimal& other) const;

  bool operator==(const Decimal& other) const { return CompareTo(other) == 0; }
  bool operator!=(const Decimal& other) const { return CompareTo(other) != 0; }
  bool operator<(const Decimal& other) const { return CompareTo(other) < 0; }
  bool operator<=(const Decimal& other) const { return CompareTo(other) <= 0; }
  bool operator>(const Decimal& other) const { return CompareTo(other) > 0; }
  bool operator>=(const Decimal& other) const { return CompareTo(other) >= 0; }
  Decimal operator-() const { return Decimal(digits_, exponent_, !is_positive_); }
  Decimal operator+() const { return Decimal(digits_, exponent_, is_positive_); }
  Decimal operator+(const Decimal& other) const;

  // Encodes the decimal by using comparable encoding, as described above.
  std::string EncodeToComparable() const;

  // Decodes a Decimal from a given Slice. Sets num_decoded_bytes = number of bytes decoded.
  CHECKED_STATUS DecodeFromComparable(const Slice& slice, size_t *num_decoded_bytes);
  CHECKED_STATUS DecodeFromComparable(const std::string& string, size_t* num_decoded_bytes) {
    return DecodeFromComparable(Slice(string), num_decoded_bytes);
  }

  CHECKED_STATUS DecodeFromComparable(const Slice& string);
  CHECKED_STATUS DecodeFromComparable(const std::string& string);

  // Encode the decimal by using to Cassandra serialization format, as described above.
  std::string EncodeToSerializedBigDecimal(bool* is_out_of_range) const;

  CHECKED_STATUS DecodeFromSerializedBigDecimal(Slice slice);

  const Decimal& Negate() { is_positive_ = !is_positive_; return *this; }

 private:
  friend class DecimalTest;

  // Checks the representation by components, For testing purposes. For Decimal, == is the same as
  // IsIdenticalTo, because we guarantee canonical-ness at all times, but the checking method is
  // different.
  bool IsIdenticalTo(const Decimal &other) const;

  bool is_canonical() const;
  void make_canonical();

  std::vector<uint8_t> digits_;
  VarInt exponent_;
  bool is_positive_ = false;
};

Decimal DecimalFromComparable(const Slice& slice);
Decimal DecimalFromComparable(const std::string& string);

std::ostream& operator<<(ostream& os, const Decimal& d);

template <typename T>
inline T BitMask(int32_t a, int32_t b) {
  T r = 0l;
  for (int i = a; i < b; i++) {
    r |= (1l << i);
  }
  return r;
}

inline int GetFloatFraction(float f) {
  return BitMask<int32_t>(0, 23) & *(reinterpret_cast<int32_t *>(&f));
}

inline int GetFloatExp(float f) {
  return (BitMask<int32_t>(23, 31) & (*(reinterpret_cast<int32_t *>(&f)))) >> 23;
}

inline int64_t GetDoubleFraction(double d) {
  return BitMask<int64_t>(0, 52) & *(reinterpret_cast<int64_t *>(&d));
}

inline int64_t GetDoubleExp(double d) {
  return (BitMask<int64_t>(52, 63) & (*(reinterpret_cast<int64_t *>(&d)))) >> 52;
}

inline float CreateFloat(int32_t sign, int32_t exponent, int32_t fraction) {
  int32_t f = (sign << 31) | (exponent << 23) | fraction;
  return *reinterpret_cast<float *>(&f);
}

inline double CreateDouble(int64_t sign, int64_t exp, int64_t fraction) {
  int64_t d = (sign << 63) | (exp << 52) | fraction;
  return *reinterpret_cast<double *>(&d);
}

inline bool IsNanFloat(float f) {
  return (GetFloatExp(f) == 0b11111111) && GetFloatFraction(f);
}

inline bool IsNanDouble(double d) {
  return (GetDoubleExp(d) == 0b11111111111) && GetDoubleFraction(d);
}

inline float CanonicalizeFloat(float f) {
  if (IsNanFloat(f)) {
    return CreateFloat(0, 0b11111111, (1 << 22));
  }
  return f;
}

inline double CanonicalizeDouble(double d) {
  if (IsNanDouble(d)) {
    return CreateDouble(0, 0b11111111111, (1l << 51));
  }
  return d;
}

} // namespace util
} // namespace yb

#endif // YB_UTIL_DECIMAL_H
