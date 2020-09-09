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

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/util.h"
#include "yb/util/format.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/socket.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/status.h"
#include "yb/util/test_util.h"
#include "yb/util/tostring.h"

namespace yb {

class NetUtilTest : public YBTest {
 protected:
  Status DoParseBindAddresses(const string& input, string* result) {
    std::vector<Endpoint> addrs;
    RETURN_NOT_OK(ParseAddressList(input, kDefaultPort, &addrs));
    std::sort(addrs.begin(), addrs.end());

    std::vector<string> addr_strs;
    for (const auto& addr : addrs) {
      addr_strs.push_back(ToString(addr));
    }
    *result = JoinStrings(addr_strs, ",");
    return Status::OK();
  }

  Status GetHostPorts(const string &endpoint_str, vector<HostPort> *hps) {
    return HostPort::ParseStrings(endpoint_str, NetUtilTest::kDefaultPort, hps);
  }

  Status GetHostPort(const string &endpoint_str, HostPort *hp) {
    return hp->ParseString(endpoint_str, NetUtilTest::kDefaultPort);
  }

  uint16_t GetDefaultPort() { return NetUtilTest::kDefaultPort; }

  static const uint16_t kDefaultPort = 7150;
};

TEST(SockaddrTest, Test) {
  boost::system::error_code ec;
  const auto& kRegularAddr = "1.1.1.1";
  auto address = IpAddress::from_string(kRegularAddr, ec);
  ASSERT_FALSE(ec);
  Endpoint endpoint(address, 12345);
  ASSERT_EQ("1.1.1.1:12345", ToString(endpoint));
  ASSERT_EQ(12345, endpoint.port());

  ASSERT_FALSE(IsWildcardAddress(kRegularAddr));
  const auto kWildcardAddr1 = "0.0.0.0";
  ASSERT_TRUE(IsWildcardAddress(kWildcardAddr1));
  const auto kWildcardAddr2 = "::";
  ASSERT_TRUE(IsWildcardAddress(kWildcardAddr2));
  ASSERT_FALSE(IsWildcardAddress("::1"));
}

TEST_F(NetUtilTest, TestParseAddresses) {
  string ret;
  ASSERT_OK(DoParseBindAddresses("0.0.0.0:12345", &ret));
  ASSERT_EQ("0.0.0.0:12345", ret);

  ASSERT_OK(DoParseBindAddresses("0.0.0.0", &ret));
  ASSERT_EQ("0.0.0.0:7150", ret);

  ASSERT_OK(DoParseBindAddresses("0.0.0.0:12345, 0.0.0.0:12346", &ret));
  ASSERT_EQ("0.0.0.0:12345,0.0.0.0:12346", ret);

  // Test some invalid addresses.
  Status s = DoParseBindAddresses("0.0.0.0:xyz", &ret);
  ASSERT_STR_CONTAINS(s.ToString(), "Invalid port");

  s = DoParseBindAddresses("0.0.0.0:100000", &ret);
  ASSERT_STR_CONTAINS(s.ToString(), "Invalid port");

  s = DoParseBindAddresses("0.0.0.0:", &ret);
  ASSERT_STR_CONTAINS(s.ToString(), "Invalid port");
}

TEST_F(NetUtilTest, TestHostPortParsing) {
  const vector<string> hosts = { "samplehost.example.org", "192.168.1.45" };
  const vector<string> ipv6_hosts = { "2600:1f18:1094:c832:36e6:43b9:e6c8:02",
                                      "0:0:0:0:0:0:0:1", "fe80%lo" };
  for (const auto &host : hosts) {
    std::unique_ptr<HostPort> hp(new HostPort());
    ASSERT_OK(GetHostPort(Format("$0:7100", host), hp.get()));
    ASSERT_EQ(hp->port(), 7100);
    ASSERT_EQ(hp->host(), host);

    hp.reset(new HostPort());
    ASSERT_OK(GetHostPort(Format("$0", host), hp.get()));
    ASSERT_EQ(hp->port(), GetDefaultPort());
    ASSERT_EQ(hp->host(), host);
  }

  for (const auto &host : ipv6_hosts) {
    std::unique_ptr<HostPort> hp(new HostPort());
    ASSERT_OK(GetHostPort(Format("[$0]:7100", host), hp.get()));
    ASSERT_EQ(hp->port(), 7100);
    ASSERT_EQ(hp->host(), host);

    hp.reset(new HostPort());
    ASSERT_OK(GetHostPort(Format("[$0]", host), hp.get()));
    ASSERT_EQ(hp->port(), GetDefaultPort());
    ASSERT_EQ(hp->host(), host);
  }

  vector<HostPort> hps;
  ASSERT_OK(GetHostPorts("[::1]:7100,127.0.0.1:7200", &hps));
  ASSERT_EQ(hps.size(), 2);
  ASSERT_EQ(hps[0].port(), 7100);
  ASSERT_EQ(hps[1].port(), 7200);

  hps.clear();
  ASSERT_OK(GetHostPorts(
      "[2600:1f18:1094:c832:36e6:43b9:e6c8:f02]:7100,0.0.0.0:7200", &hps));
  ASSERT_EQ(hps.size(), 2);
  ASSERT_EQ(hps[0].port(), 7100);
  ASSERT_EQ(hps[1].port(), 7200);
}

TEST_F(NetUtilTest, TestResolveAddresses) {
  HostPort hp("localhost", 12345);
  std::vector<Endpoint> addrs;
  ASSERT_OK(hp.ResolveAddresses(&addrs));
  ASSERT_TRUE(!addrs.empty());
  for (const auto& addr : addrs) {
    LOG(INFO) << "Address: " << ToString(addr);
    if (addr.address().is_v4()) {
      EXPECT_TRUE(HasPrefixString(ToString(addr), "127."));
      EXPECT_TRUE(HasSuffixString(ToString(addr), ":12345"));
    } else if (addr.address().is_v6()) {
      EXPECT_TRUE(HasPrefixString(ToString(addr), "[::1]"));
      EXPECT_TRUE(HasSuffixString(ToString(addr), ":12345"));
    }
    EXPECT_TRUE(addr.address().is_loopback());
  }

  ASSERT_OK(hp.ResolveAddresses(nullptr));
}

// Ensure that we are able to do a reverse DNS lookup on various IP addresses.
// The reverse lookups should never fail, but may return numeric strings.
TEST_F(NetUtilTest, TestReverseLookup) {
  string host;
  Endpoint addr(boost::asio::ip::address_v4(), 12345);
  HostPort hp;
  EXPECT_EQ(12345, addr.port());
  ASSERT_OK(HostPortFromEndpointReplaceWildcard(addr, &hp));
  EXPECT_NE("0.0.0.0", hp.host());
  EXPECT_NE("", hp.host());
  EXPECT_EQ(12345, hp.port());

  addr = Endpoint(boost::asio::ip::address_v4::loopback(), 12345);
  ASSERT_OK(HostPortFromEndpointReplaceWildcard(addr, &hp));
  EXPECT_EQ("127.0.0.1", hp.host());
  EXPECT_EQ(12345, hp.port());
}

TEST_F(NetUtilTest, TestLsof) {
  Socket s;
  ASSERT_OK(s.Init(0));

  Endpoint addr; // wildcard
  ASSERT_OK(s.BindAndListen(addr, 1));

  ASSERT_OK(s.GetSocketAddress(&addr));
  ASSERT_NE(addr.port(), 0);
  vector<string> lsof_lines;
  TryRunLsof(addr, &lsof_lines);
  SCOPED_TRACE(JoinStrings(lsof_lines, "\n"));

  ASSERT_GE(lsof_lines.size(), 3);
  ASSERT_STR_CONTAINS(lsof_lines[2], "net_util-test");
}

TEST_F(NetUtilTest, TestGetFQDN) {
  string fqdn;
  ASSERT_OK(GetFQDN(&fqdn));
  LOG(INFO) << "fqdn is " << fqdn;
}

TEST_F(NetUtilTest, LocalAddresses) {
  std::vector<IpAddress> addresses, external_addresses;
  ASSERT_OK(GetLocalAddresses(&addresses, AddressFilter::ANY));
  LOG(INFO) << "Any addresses: " << ToString(addresses);
  ASSERT_OK(GetLocalAddresses(&external_addresses, AddressFilter::EXTERNAL));
  LOG(INFO) << "External addresses: " << ToString(external_addresses);
  ASSERT_GT(addresses.size(), external_addresses.size());
}

} // namespace yb
