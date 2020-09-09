// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
#ifndef YB_UTIL_NET_NET_UTIL_H
#define YB_UTIL_NET_NET_UTIL_H

#include <string>
#include <vector>
#include <memory>

#include <boost/container/small_vector.hpp>
#include <boost/optional/optional_fwd.hpp>

#include "yb/util/env.h"
#include "yb/util/status.h"
#include "yb/util/net/net_fwd.h"

DECLARE_string(net_address_filter);
namespace yb {

// A container for a host:port pair.
class HostPort {
 public:
  HostPort();
  HostPort(std::string host, uint16_t port);
  explicit HostPort(const Endpoint& endpoint);

  static HostPort FromBoundEndpoint(const Endpoint& endpoint);

  // Parse a "host:port" pair into this object.
  // If there is no port specified in the string, then 'default_port' is used.
  CHECKED_STATUS ParseString(const std::string& str, uint16_t default_port);

  static Result<HostPort> FromString(const std::string& str, uint16_t default_port) {
    HostPort result;
    RETURN_NOT_OK(result.ParseString(str, default_port));
    return std::move(result);
  }

  // Resolve any addresses corresponding to this host:port pair.
  // Note that a host may resolve to more than one IP address.
  //
  // 'addresses' may be NULL, in which case this function simply checks that
  // the host/port pair can be resolved, without returning anything.
  CHECKED_STATUS ResolveAddresses(std::vector<Endpoint>* addresses) const;

  std::string ToString() const;

  const std::string& host() const { return host_; }
  void set_host(const std::string& host) { host_ = host; }

  uint16_t port() const { return port_; }
  void set_port(uint16_t port) { port_ = port; }

  // Parse a comma separated list of "host:port" pairs into a vector
  // HostPort objects. If no port is specified for an entry in the
  // comma separated list, 'default_port' is used for that entry's
  // pair.
  static CHECKED_STATUS ParseStrings(
      const std::string& comma_sep_addrs,
      uint16_t default_port,
      std::vector<HostPort>* res,
      const char* separator = ",");

  static Result<std::vector<HostPort>> ParseStrings(
      const std::string& comma_sep_addrs, uint16_t default_port,
      const char* separator = ",") {
    std::vector<HostPort> result;
    RETURN_NOT_OK(ParseStrings(comma_sep_addrs, default_port, &result, separator));
    return std::move(result);
  }

  template <class PB>
  static HostPort FromPB(const PB& pb) {
    return HostPort(pb.host(), pb.port());
  }

  // Takes a vector of HostPort objects and returns a comma separated
  // string containing of "host:port" pairs. This method is the
  // "inverse" of ParseStrings().
  static std::string ToCommaSeparatedString(const std::vector<HostPort>& host_ports);

  // Checks if the host/port are same as the sockaddr
  bool equals(const Endpoint& endpoint) const;

  // Remove a given host/port from a vector of comma separated server multiple addresses, each in
  // [host:port,]+ format and returns a final list as a remaining vector of hostports.
  static CHECKED_STATUS RemoveAndGetHostPortList(
      const Endpoint& remove,
      const std::vector<std::string>& multiple_server_addresses,
      uint16_t default_port,
      std::vector<HostPort> *res);

  bool operator==(const HostPort& other) const {
    return host() == other.host() && port() == other.port();
  }

  friend bool operator<(const HostPort& lhs, const HostPort& rhs) {
    auto cmp_hosts = lhs.host_.compare(rhs.host_);
    return cmp_hosts < 0 || (cmp_hosts == 0 && lhs.port_ < rhs.port_);
  }

 private:
  std::string host_;
  uint16_t port_;
};

inline std::ostream& operator<<(std::ostream& out, const HostPort& value) {
  return out << value.ToString();
}

struct HostPortHash {
  size_t operator()(const HostPort& hostPort) const {
    return hash<std::string>()(std::to_string(hostPort.port()) + hostPort.host());
  }
};

// Parse and resolve the given comma-separated list of addresses.
//
// The resulting addresses will be resolved, made unique, and added to
// the 'addresses' vector.
//
// Any elements which do not include a port will be assigned 'default_port'.
CHECKED_STATUS ParseAddressList(const std::string& addr_list,
                                uint16_t default_port,
                                std::vector<Endpoint>* addresses);

// Return true if the given port is likely to need root privileges to bind to.
bool IsPrivilegedPort(uint16_t port);

// Return the local machine's hostname.
CHECKED_STATUS GetHostname(std::string* hostname);

// Return the local machine's hostname as a Result.
Result<std::string> GetHostname();

// Return the local machine's FQDN.
CHECKED_STATUS GetFQDN(std::string* fqdn);

// Returns a single socket address from a HostPort.
// If the hostname resolves to multiple addresses, returns the first in the
// list and logs a message in verbose mode.
CHECKED_STATUS EndpointFromHostPort(const HostPort& host_port, Endpoint* endpoint);

// Converts the given Endpoint into a HostPort, substituting the FQDN
// in the case that the provided address is the wildcard.
//
// In the case of other addresses, the returned HostPort will contain just the
// stringified form of the IP.
CHECKED_STATUS HostPortFromEndpointReplaceWildcard(const Endpoint& addr, HostPort* hp);

// Try to run 'lsof' to determine which process is preventing binding to
// the given 'addr'. If pids can be determined, outputs full 'ps' and 'pstree'
// output for that process.
//
// Output is issued to the log at WARNING level, or appended to 'log' if it
// is non-NULL (mostly useful for testing).
void TryRunLsof(const Endpoint& addr, std::vector<std::string>* log = NULL);

// Get a free port that a local server could listen to. For use in tests. Tries up to a 1000 times
// and fatals after that.
// @param file_lock This will be set to a file lock ensuring the port does not get taken by
//                  another call to this function, possibly in another process.
uint16_t GetFreePort(std::unique_ptr<FileLock>* file_lock);

enum class AddressFilter {
  ANY, // all interfaces would be listed.
  EXTERNAL, // don't list local loopbacks
};
Status GetLocalAddresses(std::vector<IpAddress>* result, AddressFilter filter);

// Get local addresses, filtered and ordered by the filter_spec specified
// For details of the filter_spec, see inetaddress.h
Status GetLocalAddresses(const string &filter_spec,
                         std::vector<IpAddress> *result);

// Convert the given host/port pair to a string of the host:port format.
std::string HostPortToString(const std::string& host, int port);

template <class PB>
static std::string HostPortPBToString(const PB& pb) {
  return HostPortToString(pb.host(), pb.port());
}

CHECKED_STATUS HostToAddresses(
    const std::string& host,
    boost::container::small_vector_base<IpAddress>* addresses);

Result<IpAddress> HostToAddress(const std::string& host);
boost::optional<IpAddress> TryFastResolve(const std::string& host);
Result<IpAddress> ParseIpAddress(const std::string& host);

// Returns true if host_str is 0.0.0.0 or [::]
bool IsWildcardAddress(const std::string& host_str);

} // namespace yb

#endif  // YB_UTIL_NET_NET_UTIL_H
