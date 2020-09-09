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

#include "yb/util/net/net_util.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <algorithm>
#include <iostream>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/optional/optional.hpp>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/numbers.h"
#include "yb/gutil/strings/split.h"
#include "yb/gutil/strings/strip.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/strings/util.h"

#include "yb/util/debug/trace_event.h"
#include "yb/util/errno.h"
#include "yb/util/faststring.h"
#include "yb/util/env.h"
#include "yb/util/env_util.h"
#include "yb/util/memory/memory.h"
#include "yb/util/net/inetaddress.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/net/socket.h"
#include "yb/util/random.h"
#include "yb/util/scope_exit.h"
#include "yb/util/stopwatch.h"
#include "yb/util/subprocess.h"

// Mac OS 10.9 does not appear to define HOST_NAME_MAX in unistd.h
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64
#endif

using std::unordered_set;
using std::vector;
using strings::Substitute;

DEFINE_string(
    net_address_filter,
    "ipv4_external,ipv4_all",
    "Order in which to select ip addresses returned by the resolver"
    "Can be set to something like \"ipv4_all,ipv6_all\" to prefer IPv4 over "
    "IPv6 addresses."
    "Can be set to something like \"ipv4_external,ipv4_all,ipv6_all\" to "
    "prefer external IPv4 "
    "addresses first. Other options include ipv6_external,ipv6_non_link_local");

namespace yb {

namespace {
struct AddrinfoDeleter {
  void operator()(struct addrinfo* info) {
    freeaddrinfo(info);
  }
};
}

HostPort::HostPort()
    : port_(0) {
}

HostPort::HostPort(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

HostPort::HostPort(const Endpoint& endpoint)
    : host_(endpoint.address().to_string()), port_(endpoint.port()) {
}

Status HostPort::RemoveAndGetHostPortList(
    const Endpoint& remove,
    const std::vector<std::string>& multiple_server_addresses,
    uint16_t default_port,
    std::vector<HostPort> *res) {
  bool found = false;
  // Note that this outer loop is over a vector of comma-separated strings.
  for (const string& master_server_addr : multiple_server_addresses) {
    std::vector<std::string> addr_strings =
        strings::Split(master_server_addr, ",", strings::SkipEmpty());
    for (const auto& single_addr : addr_strings) {
      HostPort host_port;
      RETURN_NOT_OK(host_port.ParseString(single_addr, default_port));
      if (host_port.equals(remove)) {
        found = true;
        continue;
      } else {
        res->push_back(host_port);
      }
    }
  }

  if (!found) {
    std::ostringstream out;
    out.str("Current list of master addresses: ");
    for (const string& master_server_addr : multiple_server_addresses) {
      out.str(master_server_addr);
      out.str(" ");
    }
    LOG(ERROR) << out.str();

    return STATUS_SUBSTITUTE(NotFound,
                             "Cannot find $0 in addresses: $1",
                             yb::ToString(remove),
                             out.str());
  }

  return Status::OK();
}

// Accepts entries like: [::1], 127.0.0.1, [::1]:7100, 0.0.0.0:7100,
// f.q.d.n:7100
Status HostPort::ParseString(const string &str_in, uint16_t default_port) {
  uint32_t port;
  string host;

  string str(str_in);
  StripWhiteSpace(&str);
  size_t pos = str.rfind(':');
  if (str[0] == '[' && str[str.length() - 1] == ']' && str.length() > 2) {
    // The whole thing is an IPv6 address.
    host = str.substr(1, str.length() - 2);
    port = default_port;
  } else if (pos == string::npos) {
    // No port was specified, the whole thing must be a host.
    host = str;
    port = default_port;
  } else if (pos > 1 && pos + 1 < str.length() &&
             SimpleAtoi(str.substr(pos + 1), &port)) {

    if (port > numeric_limits<uint16_t>::max()) {
      return STATUS(InvalidArgument, "Invalid port", str);
    }

    // Got a host:port
    host = str.substr(0, pos);
    if (host[0] == '[' && host[host.length() - 1] == ']' && host.length() > 2) {
      // Remove brackets if we have an IPv6 address
      host = host.substr(1, host.length() - 2);
    }
  } else {
    return STATUS(InvalidArgument,
                  Format(
                      "Invalid port: expected port after ':' "
                      "at position $0 in $1",
                      pos, str));
  }

  host_ = host;
  port_ = port;
  return Status::OK();
}

namespace {
const string getaddrinfo_rc_to_string(int rc) {
  const char* s = nullptr;
  switch (rc) {
    case EAI_ADDRFAMILY: s = "EAI_ADDRFAMILY"; break;
    case EAI_AGAIN: s = "EAI_AGAIN"; break;
    case EAI_BADFLAGS: s = "EAI_BADFLAGS"; break;
    case EAI_FAIL: s = "EAI_FAIL"; break;
    case EAI_FAMILY: s = "EAI_FAMILY"; break;
    case EAI_MEMORY: s = "EAI_MEMORY"; break;
    case EAI_NODATA: s = "EAI_NODATA"; break;
    case EAI_NONAME: s = "EAI_NONAME"; break;
    case EAI_SERVICE: s = "EAI_SERVICE"; break;
    case EAI_SOCKTYPE: s = "EAI_SOCKTYPE"; break;
    case EAI_SYSTEM: s = "EAI_SYSTEM"; break;
    default: s = "UNKNOWN";
  }
  return Substitute("$0 ($1)", rc, s);
}

Result<std::unique_ptr<addrinfo, AddrinfoDeleter>> HostToInetAddrInfo(const std::string& host) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  int rc = 0;
  LOG_SLOW_EXECUTION(WARNING, 200,
                     Substitute("resolving address for $0", host)) {
    rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
  }
  if (rc != 0) {
    return STATUS_FORMAT(NetworkError, "Unable to resolve address $0, getaddrinfo returned $1: $2",
                         host.c_str(), getaddrinfo_rc_to_string(rc).c_str(), gai_strerror(rc));
  } else {
    return std::unique_ptr<addrinfo, AddrinfoDeleter>(res);
  }
}

template <typename F>
CHECKED_STATUS ResolveInetAddresses(const std::string& host, F func) {
  boost::optional<IpAddress> fast_resolve = TryFastResolve(host);
  if (fast_resolve) {
    func(*fast_resolve);
    VLOG(4) << "Fast resolved " << host << " to " << fast_resolve->to_string();
    return Status::OK();
  }

  auto addrinfo_holder = VERIFY_RESULT(HostToInetAddrInfo(host));
  struct addrinfo* addrinfo = addrinfo_holder.get();
  for (; addrinfo != nullptr; addrinfo = addrinfo->ai_next) {
    if (addrinfo->ai_family == AF_INET) {
      auto* addr = reinterpret_cast<struct sockaddr_in*>(addrinfo->ai_addr);
      Endpoint endpoint;
      memcpy(endpoint.data(), addr, sizeof(*addr));
      func(endpoint.address());
    } else if (addrinfo->ai_family == AF_INET6) {
      auto* addr = reinterpret_cast<struct sockaddr_in6*>(addrinfo->ai_addr);
      Endpoint endpoint;
      memcpy(endpoint.data(), addr, sizeof(*addr));
      func(endpoint.address());
    } else {
      return STATUS_FORMAT(NetworkError, "Unexpected address family: $0", addrinfo->ai_family);
    }
  }
  return Status::OK();
}

}  // namespace

Status HostPort::ResolveAddresses(std::vector<Endpoint>* addresses) const {
  TRACE_EVENT1("net", "HostPort::ResolveAddresses",
               "host", host_);
  if (!addresses) {
    return Status::OK();
  }
  vector<IpAddress> ip_addresses;
  RETURN_NOT_OK(ResolveInetAddresses(
      host_, [this, &ip_addresses](const IpAddress &ip_address) {
        ip_addresses.push_back(ip_address);
        VLOG(3) << "Resolved address " << ip_address.to_string() << " for host "
                << host_;
      }));

  FilterAddresses(FLAGS_net_address_filter, &ip_addresses);

  VLOG(2) << "Returned " << ip_addresses.size() << " addresses for host "
          << host_;
  for (const auto &ip_addr : ip_addresses) {
    VLOG(2) << "Returned address " << ip_addr.to_string() << " for host "
            << host_;
    addresses->push_back(Endpoint(ip_addr, port_));
  }
  return Status::OK();
}

Status HostPort::ParseStrings(const string& comma_sep_addrs,
                              uint16_t default_port,
                              std::vector<HostPort>* res,
                              const char* separator) {
  std::vector<string> addr_strings = strings::Split(
      comma_sep_addrs, separator, strings::SkipEmpty());
  std::vector<HostPort> host_ports;
  for (string& addr_string : addr_strings) {
    StripWhiteSpace(&addr_string);
    HostPort host_port;
    RETURN_NOT_OK(host_port.ParseString(addr_string, default_port));
    host_ports.push_back(host_port);
  }
  *res = host_ports;
  return Status::OK();
}

string HostPort::ToString() const { return HostPortToString(host_, port_); }

string HostPort::ToCommaSeparatedString(const std::vector<HostPort>& hostports) {
  vector<string> hostport_strs;
  for (const HostPort& hostport : hostports) {
    hostport_strs.push_back(hostport.ToString());
  }
  return JoinStrings(hostport_strs, ",");
}

bool IsPrivilegedPort(uint16_t port) { return port < 1024 && port != 0; }

Status ParseAddressList(const std::string& addr_list,
                        uint16_t default_port,
                        std::vector<Endpoint>* addresses) {
  std::vector<HostPort> host_ports;
  RETURN_NOT_OK(HostPort::ParseStrings(addr_list, default_port, &host_ports));
  std::unordered_set<Endpoint, EndpointHash> uniqued;

  for (const HostPort& host_port : host_ports) {
    std::vector<Endpoint> this_addresses;
    RETURN_NOT_OK(host_port.ResolveAddresses(&this_addresses));

    // Only add the unique ones -- the user may have specified
    // some IP addresses in multiple ways
    for (const auto& addr : this_addresses) {
      if (InsertIfNotPresent(&uniqued, addr)) {
        addresses->push_back(addr);
      } else {
        LOG(INFO) << "Address " << addr << " for " << host_port.ToString()
                  << " duplicates an earlier resolved entry.";
      }
    }
  }
  return Status::OK();
}

Status GetHostname(string* hostname) {
  TRACE_EVENT0("net", "GetHostname");
  char name[HOST_NAME_MAX];
  int ret = gethostname(name, HOST_NAME_MAX);
  if (ret != 0) {
    return STATUS(NetworkError, "Unable to determine local hostname", Errno(errno));
  }
  *hostname = name;
  return Status::OK();
}

Result<string> GetHostname() {
  std::string result;
  RETURN_NOT_OK(GetHostname(&result));
  return result;
}

Status GetLocalAddresses(const string &filter_spec,
                         std::vector<IpAddress> *result) {
  RETURN_NOT_OK(GetLocalAddresses(result, AddressFilter::ANY));
  FilterAddresses(filter_spec, result);
  return Status::OK();
}

Status GetLocalAddresses(std::vector<IpAddress>* result, AddressFilter filter) {
  ifaddrs* addresses;
  if (getifaddrs(&addresses)) {
    return STATUS(NetworkError, "Failed to list network interfaces", Errno(errno));
  }

  auto se = ScopeExit([addresses] {
    freeifaddrs(addresses);
  });

  for (auto address = addresses; address; address = address->ifa_next) {
    if (address->ifa_addr != nullptr) {
      Endpoint temp;
      auto family = address->ifa_addr->sa_family;
      size_t size;
      if (family == AF_INET) {
        size = sizeof(sockaddr_in);
      } else if (family == AF_INET6) {
        size = sizeof(sockaddr_in6);
      } else {
        continue;
      }
      memcpy(temp.data(), address->ifa_addr, size);
      switch (filter) {
        case AddressFilter::ANY:
          result->push_back(temp.address());
          break;
        case AddressFilter::EXTERNAL:
          if (!temp.address().is_unspecified() && !temp.address().is_loopback()) {
            result->push_back(temp.address());
          }
          break;
      }
    }
  }
  return Status::OK();
}

Status GetFQDN(string* hostname) {
  TRACE_EVENT0("net", "GetFQDN");
  // Start with the non-qualified hostname
  RETURN_NOT_OK(GetHostname(hostname));

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_CANONNAME;

  struct addrinfo* result;
  LOG_SLOW_EXECUTION(WARNING, 200,
                     Substitute("looking up canonical hostname for localhost $0", *hostname)) {
    TRACE_EVENT0("net", "getaddrinfo");
    const int rc = getaddrinfo(hostname->c_str(), nullptr, &hints, &result);
    if (rc != 0) {
      return STATUS(NetworkError,
                    Substitute("Unable to lookup FQDN ($0), getaddrinfo returned $1",
                               *hostname, getaddrinfo_rc_to_string(rc)),
                    Errno(errno));
    }
  }

  *hostname = result->ai_canonname;
  freeaddrinfo(result);
  return Status::OK();
}

Status EndpointFromHostPort(const HostPort& host_port, Endpoint* endpoint) {
  vector<Endpoint> addrs;
  RETURN_NOT_OK(host_port.ResolveAddresses(&addrs));
  if (addrs.empty()) {
    return STATUS(NetworkError, "Unable to resolve address", host_port.ToString());
  }
  *endpoint = addrs[0];
  if (addrs.size() > 1) {
    VLOG(1) << "Hostname " << host_port.host() << " resolved to more than one address. "
            << "Using address: " << *endpoint;
  }
  return Status::OK();
}

Status HostPortFromEndpointReplaceWildcard(const Endpoint& addr, HostPort* hp) {
  string host;
  if (addr.address().is_unspecified()) {
    auto status = GetFQDN(&host);
    if (!status.ok()) {
      std::vector<IpAddress> locals;
      if (GetLocalAddresses(&locals, AddressFilter::EXTERNAL).ok() && !locals.empty()) {
        hp->set_host(locals.front().to_string());
        hp->set_port(addr.port());
        return Status::OK();
      }
      return status;
    }
  } else {
    host = addr.address().to_string();
  }
  hp->set_host(host);
  hp->set_port(addr.port());
  return Status::OK();
}

void TryRunLsof(const Endpoint& addr, vector<string>* log) {
#if defined(__APPLE__)
  string cmd = strings::Substitute(
      "lsof -n -i 'TCP:$0' -sTCP:LISTEN ; "
      "for pid in $$(lsof -F p -n -i 'TCP:$0' -sTCP:LISTEN | cut -f 2 -dp) ; do"
      "  pstree $$pid || ps h -p $$pid;"
      "done",
      addr.port());
#else
  // Little inline bash script prints the full ancestry of any pid listening
  // on the same port as 'addr'. We could use 'pstree -s', but that option
  // doesn't exist on el6.
  //
  // Note the sed command to check for the process name wrapped in ().
  // Example prefix of /proc/$pid/stat output, with a process with spaces in the name:
  // 3917 (tmux: server) S 1
  string cmd = strings::Substitute(
      "export PATH=$$PATH:/usr/sbin ; "
      "lsof -n -i 'TCP:$0' -sTCP:LISTEN ; "
      "for pid in $$(lsof -F p -n -i 'TCP:$0' -sTCP:LISTEN | cut -f 2 -dp) ; do"
      "  while [ $$pid -gt 1 ] ; do"
      "    ps h -fp $$pid ;"
      "    pid=$$(sed 's/.* (.*) [^ ] \\([0-9]*\\).*/\\1/g' /proc/$$pid/stat);"
      "  done ; "
      "done",
      addr.port());
#endif // defined(__APPLE__)

  LOG_STRING(WARNING, log) << "Failed to bind to " << addr << ". "
                           << "Trying to use lsof to find any processes listening "
                           << "on the same port:";
  LOG_STRING(INFO, log) << "$ " << cmd;
  vector<string> argv = { "bash", "-c", cmd };
  string results;
  Status s = Subprocess::Call(argv, &results);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_STRING(WARNING, log) << s.ToString();
  }
  LOG_STRING(WARNING, log) << results;
}

uint16_t GetFreePort(std::unique_ptr<FileLock>* file_lock) {
  // To avoid a race condition where the free port returned to the caller gets used by another
  // process before this caller can use it, we will lock the port using a file level lock.
  // First create the directory, if it doesn't already exist, where these lock files will live.
  Env* env = Env::Default();
  bool created = false;
  const string lock_file_dir = "/tmp/yb-port-locks";
  Status status = env_util::CreateDirIfMissing(env, lock_file_dir, &created);
  if (!status.ok()) {
    LOG(FATAL) << "Could not create " << lock_file_dir << " directory: "
               << status.ToString();
  }

  // Now, find a unused port in the [kMinPort..kMaxPort] range.
  constexpr uint16_t kMinPort = 15000;
  constexpr uint16_t kMaxPort = 30000;
  static yb::Random rand(GetCurrentTimeMicros());
  Status s;
  for (int i = 0; i < 1000; ++i) {
    const uint16_t random_port = kMinPort + rand.Next() % (kMaxPort - kMinPort + 1);
    VLOG(1) << "Trying to bind to port " << random_port;

    Endpoint sock_addr(boost::asio::ip::address_v4::loopback(), random_port);
    Socket sock;
    s = sock.Init(0);
    if (!s.ok()) {
      VLOG(1) << "Failed to call Init() on socket ith address " << sock_addr;
      continue;
    }

    s = sock.Bind(sock_addr, /* explain_addr_in_use */ false);
    if (s.ok()) {
      // We found an unused port.

      // Now, lock this "port" for use by the current process before 'sock' goes out of scope.
      // This will ensure that no other process can get this port while this process is still
      // running. LockFile() returns immediately if we can't get the lock. That's the behavior
      // we want. In that case, we'll just try another port.
      const string lock_file = lock_file_dir + "/" + std::to_string(random_port) + ".lck";
      FileLock *lock = nullptr;
      s = env->LockFile(lock_file, &lock, false /* recursive_lock_ok */);
      if (s.ok()) {
        CHECK(lock) << "Lock should not be NULL";
        file_lock->reset(lock);
        LOG(INFO) << "Selected random free RPC port " << random_port;
        return random_port;
      } else {
        VLOG(1) << "Could not lock file " << lock_file << ": " << s.ToString();
      }
    } else {
      VLOG(1) << "Failed to bind to port " << random_port << ": " << s.ToString();
    }
  }

  LOG(FATAL) << "Could not find a free random port between " <<  kMinPort << " and "
             << kMaxPort << " inclusively" << ": " << s.ToString();
  return 0;  // never reached
}

bool HostPort::equals(const Endpoint& endpoint) const {
  return endpoint.address().to_string() == host() && endpoint.port() == port();
}

HostPort HostPort::FromBoundEndpoint(const Endpoint& endpoint) {
  if (endpoint.address().is_unspecified()) {
    return HostPort(endpoint.address().is_v4() ? "127.0.0.1" : "::1", endpoint.port());
  } else {
    return HostPort(endpoint);
  }
}

std::string HostPortToString(const std::string& host, int port) {
  DCHECK_GE(port, 0);
  DCHECK_LE(port, 65535);
  if (host.find(':') != string::npos) {
    return Format("[$0]:$1", host, port);
  } else {
    return Format("$0:$1", host, port);
  }
}

Status HostToAddresses(
    const std::string& host,
    boost::container::small_vector_base<IpAddress>* addresses) {
  return ResolveInetAddresses(host, [&addresses](const IpAddress& address) {
    addresses->push_back(address);
  });
}

Result<IpAddress> HostToAddress(const std::string& host) {
  boost::container::small_vector<IpAddress, 1> addrs;
  RETURN_NOT_OK(HostToAddresses(host, &addrs));
  if (addrs.empty()) {
    return STATUS(NetworkError, "Unable to resolve address", host);
  }
  auto addr = addrs.front();
  if (addrs.size() > 1) {
    VLOG(1) << "Hostname " << host << " resolved to more than one address. "
            << "Using address: " << addr;
  }
  return addr;
}

bool IsWildcardAddress(const std::string& host_str) {
  boost::system::error_code ec;
  auto addr = IpAddress::from_string(host_str, ec);
  return !ec && addr.is_unspecified();
}

Result<IpAddress> ParseIpAddress(const std::string& host) {
  boost::system::error_code ec;
  auto addr = IpAddress::from_string(host, ec);
  if (ec) {
    return STATUS_FORMAT(InvalidArgument, "Failed to parse $0: $1", host, ec.message());
  }

  VLOG(4) << "Resolving ip address to itself for input: " << host;
  return addr;
}

boost::optional<IpAddress> TryFastResolve(const std::string& host) {
  auto result = ParseIpAddress(host);
  if (result.ok()) {
    return *result;
  }

  // For testing purpose we resolve A.B.C.D.ip.yugabyte to A.B.C.D.
  static const std::string kYbIpSuffix = ".ip.yugabyte";
  if (boost::ends_with(host, kYbIpSuffix)) {
    boost::system::error_code ec;
    auto address = IpAddress::from_string(
        host.substr(0, host.length() - kYbIpSuffix.length()), ec);
    if (!ec) {
      return address;
    }
  }

  return boost::none;
}

} // namespace yb
