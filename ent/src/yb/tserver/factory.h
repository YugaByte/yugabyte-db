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

#ifndef ENT_SRC_YB_TSERVER_FACTORY_H
#define ENT_SRC_YB_TSERVER_FACTORY_H

#include <memory>

#include "yb/rpc/rpc_fwd.h"
#include "yb/rpc/secure_stream.h"

#include "yb/server/secure.h"

#include "yb/tserver/ts_tablet_manager.h"

namespace yb {

namespace cqlserver {

class CQLServer;
class CQLServerOptions;

}

namespace tserver {

class TabletServer;
class TabletServerOptions;

namespace enterprise {

class CQLServer;
class TabletServer;

class CQLServerEnt : public cqlserver::CQLServer {
 public:
  template <class... Args>
  explicit CQLServerEnt(Args&&... args) : CQLServer(std::forward<Args>(args)...) {
  }

 private:
  CHECKED_STATUS SetupMessengerBuilder(rpc::MessengerBuilder* builder) override {
    RETURN_NOT_OK(CQLServer::SetupMessengerBuilder(builder));
    secure_context_ = VERIFY_RESULT(server::SetupSecureContext(
        options_.rpc_opts.rpc_bind_addresses, *fs_manager_,
        server::SecureContextType::kClientToServer, builder));
    return Status::OK();
  }

  std::unique_ptr<rpc::SecureContext> secure_context_;
};

class Factory {
 public:
  std::unique_ptr<TabletServer> CreateTabletServer(const TabletServerOptions& options) {
    return std::make_unique<TabletServer>(options);
  }

  std::unique_ptr<cqlserver::CQLServer> CreateCQLServer(
      const cqlserver::CQLServerOptions& options, IoService* io,
      tserver::TabletServer* tserver) {
    return std::make_unique<CQLServerEnt>(options, io, tserver);
  }
};

} // namespace enterprise
} // namespace tserver
} // namespace yb

#endif // ENT_SRC_YB_TSERVER_FACTORY_H
