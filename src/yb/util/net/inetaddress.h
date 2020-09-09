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

#ifndef YB_UTIL_NET_INETADDRESS_H
#define YB_UTIL_NET_INETADDRESS_H

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/system/error_code.hpp>

#include "yb/gutil/strings/substitute.h"
#include "yb/util/status.h"

namespace yb {

constexpr size_t kInetAddressV4Size = 4;
constexpr size_t kInetAddressV6Size = 16;

// Generic class that encapsulates IPv4 and IPv6 addresses and uses the boost implementation
// underneath.
class InetAddress {
 public:
  InetAddress();

  explicit InetAddress(const boost::asio::ip::address& address);

  InetAddress(const InetAddress& other);

  // Fills in strval with the string representation of an IPv4 or IPv6 address.
  CHECKED_STATUS ToString(std::string* strval) const;

  // Returns string representation of an IPv4 or IPv6 address. This method doesn't return a
  // Status for usecases in the code where we don't support returning a status.
  std::string ToString() const;

  // Fills in the given string with the raw bytes for the appropriate address in network byte order.
  CHECKED_STATUS ToBytes(std::string* bytes) const;

  // Given a string holding the raw bytes in network byte order, it builds the appropriate
  // InetAddress object.
  CHECKED_STATUS FromBytes(const std::string& bytes);

  // Give a slice holding raw bytes in network byte order, build the appropriate InetAddress
  // object. If size_hint is specified, it indicates the number of bytes to decode from the slice.
  CHECKED_STATUS FromSlice(const Slice& slice, size_t size_hint = 0);

  const boost::asio::ip::address& address() const {
    return boost_addr_;
  }

  bool isV4() const {
    DCHECK(!boost_addr_.is_unspecified());
    return boost_addr_.is_v4();
  }

  bool isV6() const {
    DCHECK(!boost_addr_.is_unspecified());
    return boost_addr_.is_v6();
  }

  bool operator==(const InetAddress& other) const {
    return (boost_addr_ == other.boost_addr_);
  }

  bool operator!=(const InetAddress& other) const {
    return !(*this == other);
  }

  bool operator<(const InetAddress& other) const {
    string this_bytes, other_bytes;
    Status s = ToBytes(&this_bytes);
    Status t = other.ToBytes(&other_bytes);
    DCHECK(s.ok() && t.ok());
    return this_bytes < other_bytes;
  }

  bool operator>(const InetAddress& other) const {
    return (other < *this);
  }

  bool operator<=(const InetAddress& other) const {
    return !(other < *this);
  }

  bool operator>=(const InetAddress& other) const {
    return !(*this < other);
  }

  InetAddress& operator=(const InetAddress& other) {
    boost_addr_ = other.boost_addr_;
    return *this;
  }

 private:
  boost::asio::ip::address boost_addr_;
};

void FilterAddresses(const string &transform_spec,
                     vector<boost::asio::ip::address> *addresses);

} // namespace yb

#endif // YB_UTIL_NET_INETADDRESS_H
