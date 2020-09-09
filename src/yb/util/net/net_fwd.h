//
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

#ifndef YB_UTIL_NET_NET_FWD_H
#define YB_UTIL_NET_NET_FWD_H

namespace boost {
namespace asio {

#if BOOST_VERSION >= 106600
class io_context;
typedef io_context io_service;
#else
class io_service;
#endif

namespace ip {

class address;

template <typename InternetProtocol>
class basic_endpoint;

template <typename InternetProtocol>
class basic_resolver;

template <typename InternetProtocol>
class basic_resolver_results;

class tcp;

} // namespace ip
} // namespace asio
} // namespace boost

namespace yb {

typedef boost::asio::ip::address IpAddress;
typedef boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> Endpoint;
class DnsResolver;
class HostPort;
class InetAddress;
class Tunnel;
typedef boost::asio::io_service IoService;
typedef boost::asio::ip::basic_resolver_results<boost::asio::ip::tcp> ResolverResults;

} // namespace yb

#endif // YB_UTIL_NET_NET_FWD_H
