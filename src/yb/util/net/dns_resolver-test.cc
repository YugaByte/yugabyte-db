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

#include "yb/util/net/dns_resolver.h"

#include <vector>

#include <gtest/gtest.h>

#include "yb/gutil/strings/util.h"
#include "yb/util/countdown_latch.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/test_util.h"

using namespace std::literals;

namespace yb {

class DnsResolverTest : public YBTest {
 protected:
  void SetUp() override {
    io_thread_ = CHECK_RESULT(Thread::Make("io_thread", "io_thread", [this] {
      boost::system::error_code ec;
      io_service_.run(ec);
      LOG_IF(ERROR, ec) << "Failed to run io service: " << ec;
    }));
  }

  void TearDown() override {
    work_.reset();
    Join();
  }

 protected:
  void Join() {
    auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!io_service_.stopped()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        LOG(ERROR) << "Io service failed to stop";
        io_service_.stop();
        break;
      }
      std::this_thread::sleep_for(10ms);
    }
    io_thread_->Join();
  }

  ThreadPtr io_thread_;
  IoService io_service_;
  boost::optional<IoService::work> work_{io_service_};

  DnsResolver resolver_{&io_service_};
};

TEST_F(DnsResolverTest, TestResolution) {
  auto future = resolver_.ResolveFuture("localhost");
  ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
  auto addr = ASSERT_RESULT(future.get());
  LOG(INFO) << "Address: " << addr;
  if (addr.is_v4()) {
    EXPECT_TRUE(HasPrefixString(ToString(addr), "127."));
  } else if (addr.is_v6()) {
    EXPECT_TRUE(HasPrefixString(ToString(addr), "[::1]"));
  }
}

} // namespace yb
