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

#ifndef YB_CLIENT_TABLET_SERVER_H
#define YB_CLIENT_TABLET_SERVER_H

namespace yb {
namespace client {

// In-memory representation of a remote tablet server.
class YBTabletServer {
 public:
  YBTabletServer(std::string uuid, std::string hostname, std::string placement_uuid = "")
      : uuid_(std::move(uuid)), hostname_(std::move(hostname)),
        placement_uuid_(std::move(placement_uuid)) {}

  YBTabletServer(const YBTabletServer&) = delete;
  void operator=(const YBTabletServer&) = delete;

  ~YBTabletServer() {}

  // Returns the UUID of this tablet server. Is globally unique and
  // guaranteed not to change for the lifetime of the tablet server.
  const std::string& uuid() const {
    return uuid_;
  }

  // Returns the hostname of the first RPC address that this tablet server
  // is listening on.
  const std::string& hostname() const {
    return hostname_;
  }

  const std::string& placement_uuid() const {
    return placement_uuid_;
  }

 private:
  const std::string uuid_;
  const std::string hostname_;
  const std::string placement_uuid_;
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_TABLET_SERVER_H
