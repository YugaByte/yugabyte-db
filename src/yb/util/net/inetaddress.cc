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

#include "yb/util/net/inetaddress.h"

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "yb/gutil/strings/split.h"
#include "yb/util/net/net_util.h"

using boost::asio::ip::address;
using boost::asio::ip::address_v4;
using boost::asio::ip::address_v6;
using boost::asio::ip::tcp;

namespace yb {

InetAddress::InetAddress() {
}

InetAddress::InetAddress(const boost::asio::ip::address& address)
    : boost_addr_(address) {
}

InetAddress::InetAddress(const InetAddress& other) {
  boost_addr_ = other.boost_addr_;
}

std::string InetAddress::ToString() const {
  std::string strval;
  CHECK_OK(ToString(&strval));
  return strval;
}

CHECKED_STATUS InetAddress::ToString(std::string *strval) const {
  boost::system::error_code ec;
  *strval = boost_addr_.to_string(ec);
  if (ec.value()) {
    return STATUS(IllegalState, "InetAddress object cannot be converted to string: $0",
                  ec.message());
  }
  return Status::OK();
}

CHECKED_STATUS InetAddress::ToBytes(std::string* bytes) const {
  try {
    if (boost_addr_.is_v4()) {
      auto v4bytes = boost_addr_.to_v4().to_bytes();
      bytes->assign(reinterpret_cast<char *>(v4bytes.data()), v4bytes.size());
    } else if (boost_addr_.is_v6()) {
      auto v6bytes = boost_addr_.to_v6().to_bytes();
      bytes->assign(reinterpret_cast<char *>(v6bytes.data()), v6bytes.size());
    } else {
      return STATUS(Uninitialized, "InetAddress doesn't hold a valid IPv4 or IPv6 address");
    }
  } catch (std::exception& e) {
    return STATUS(Corruption, "Couldn't serialize InetAddress to raw bytes!");
  }
  return Status::OK();
}

CHECKED_STATUS InetAddress::FromSlice(const Slice& slice, size_t size_hint) {
  size_t expected_size = (size_hint == 0) ? slice.size() : size_hint;
  if (expected_size > slice.size()) {
    return STATUS_SUBSTITUTE(InvalidArgument, "Size of slice: $0 is smaller than provided "
        "size_hint: $1", slice.size(), expected_size);
  }
  if (expected_size == kInetAddressV4Size) {
    address_v4::bytes_type v4bytes;
    DCHECK_EQ(expected_size, v4bytes.size());
    memcpy(v4bytes.data(), slice.data(), v4bytes.size());
    address_v4 v4address(v4bytes);
    boost_addr_ = v4address;
  } else if (expected_size == kInetAddressV6Size) {
    address_v6::bytes_type v6bytes;
    DCHECK_EQ(expected_size, v6bytes.size());
    memcpy(v6bytes.data(), slice.data(), v6bytes.size());
    address_v6 v6address(v6bytes);
    boost_addr_ = v6address;
  } else {
    return STATUS_SUBSTITUTE(InvalidArgument, "Size of slice is invalid: $0", expected_size);
  }
  return Status::OK();
}

CHECKED_STATUS InetAddress::FromBytes(const std::string& bytes) {
  Slice slice (bytes.data(), bytes.size());
  return FromSlice(slice);
}

bool IsIPv6NonLinkLocal(const IpAddress& address) {
  if (!address.is_v6() || address.is_unspecified()) {
    return false;
  }
  boost::asio::ip::address_v6 v6_address = address.to_v6();
  return !v6_address.is_link_local();
}

bool IsIPv6External(const IpAddress& address) {
  return address.is_v6() && !address.is_unspecified() &&
         !address.is_loopback() && IsIPv6NonLinkLocal(address);
}

typedef std::function<bool(const IpAddress&)> FilterType;

const std::map<string, FilterType>* GetFilters() {
  static const auto ipv4_filter = [](const IpAddress& a) { return a.is_v4(); };
  static const auto ipv6_filter = [](const IpAddress& a) { return a.is_v6(); };
  static const auto ipv4_external_filter = [](const IpAddress& a) {
    return !a.is_unspecified() && a.is_v4() && !a.is_loopback();
  };
  static const auto ipv6_external_filter = [](const IpAddress& a) {
    return IsIPv6External(a);
  };
  static const auto ipv6_non_link_local_filter = [](const IpAddress& a) {
    return IsIPv6NonLinkLocal(a);
  };
  static const auto all_filter = [](const IpAddress& a) { return true; };

  static const std::map<string, FilterType> kFilters(
      { { "ipv4_all",
          ipv4_filter }, // any IPv4 address including 0.0.0.0 and loopback
        { "ipv6_all",
          ipv6_filter }, // any IPv6 address including ::, loopback etc
        { "ipv4_external", ipv4_external_filter }, // Non-loopback IPv4
        { "ipv6_external",
          ipv6_external_filter }, // Non-loopback non-link-local IPv6
        { "ipv6_non_link_local",
          ipv6_non_link_local_filter }, // Non-link-local, loopback is ok
        { "all", all_filter } });

  return &kFilters;
}

// Filter_spec has to be some subset of the following filters
// ipv4_all,ipv4_external,ipv6_all,ipv6_external,ipv6_non_link_local
// For ex: "ipv4_external,ipv4_all,ipv6_external,ipv6_non_link_local"
// This would result in a vector that has
// [ IPV4 external addresses, Remaining IPv4 addresses, IPv6 external addresses,
// Non-external IPv6 non_link_local addresses ]
// with [ link_local IPv6 addresses ] removed from the original list
void FilterAddresses(const string& filter_spec, vector<IpAddress>* addresses) {
  if (filter_spec.empty()) {
    return;
  }

  const std::map<string, FilterType>* kFilters = GetFilters();
  DCHECK(kFilters);
  vector<string> filter_names = strings::Split(filter_spec, ",");

  vector<const FilterType*> filters;
  filters.reserve(filter_names.size());
  for (const auto &filter_name : filter_names) {
    VLOG(4) << "filtering by " << filter_name;
    auto filter_it = kFilters->find(filter_name);
    if (filter_it != kFilters->end()) {
      filters.push_back(&filter_it->second);
    } else {
      LOG(ERROR) << "Unknown filter spec " << filter_name << " in filter spec "
                 << filter_spec;
    }
  }

  vector<vector<IpAddress> > matches(filters.size());
  for (const auto& address : *addresses) {
    for (size_t i = 0; i < filters.size(); ++i) {
      DCHECK(filters[i]);
      if ((*filters[i])(address)) {
        VLOG(3) << address.to_string() << " matches filter " << filter_names[i];
        matches[i].push_back(address);
        break;
      } else {
        VLOG(4) << address.to_string() << " does not match filter "
                << filter_names[i];
      }
    }
  }
  vector<IpAddress> results;
  for (const auto& match : matches) {
    results.insert(results.end(), match.begin(), match.end());
  }
  addresses->swap(results);
}

} // namespace yb
