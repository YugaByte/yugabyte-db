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

#include "yb/master/mini_master.h"

#include <string>

#include <glog/logging.h>

#include "yb/fs/fs_manager.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/server/rpc_server.h"
#include "yb/server/webserver.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/status.h"

using strings::Substitute;

DECLARE_bool(rpc_server_allow_ephemeral_ports);
DECLARE_double(leader_failure_max_missed_heartbeat_periods);

namespace yb {
namespace master {

MiniMaster::MiniMaster(Env* env, string fs_root, uint16_t rpc_port, uint16_t web_port)
    : running_(false),
      env_(env),
      fs_root_(std::move(fs_root)),
      rpc_port_(rpc_port),
      web_port_(web_port) {}

MiniMaster::~MiniMaster() {
  if (running_) {
    LOG(WARNING) << "MiniMaster destructor called without clean shutdown for: "
                 << bound_rpc_addr_str();
  }
}

Status MiniMaster::Start() {
  CHECK(!running_);
  FLAGS_rpc_server_allow_ephemeral_ports = true;
  RETURN_NOT_OK(StartOnPorts(rpc_port_, web_port_));
  return master_->WaitForCatalogManagerInit();
}


Status MiniMaster::StartDistributedMaster(const vector<uint16_t>& peer_ports) {
  CHECK(!running_);
  return StartDistributedMasterOnPorts(rpc_port_, web_port_, peer_ports);
}

void MiniMaster::Shutdown() {
  if (running_) {
    master_->Shutdown();
  }
  running_ = false;
  master_.reset();
}

Status MiniMaster::StartOnPorts(uint16_t rpc_port, uint16_t web_port) {
  CHECK(!running_);
  CHECK(!master_);

  HostPort local_host_port("127.0.0.1", rpc_port);
  auto master_addresses = std::make_shared<std::vector<HostPort>>();
  master_addresses->push_back(local_host_port);
  MasterOptions opts(master_addresses);

  Status start_status = StartOnPorts(rpc_port, web_port, &opts);
  if (!start_status.ok()) {
    LOG(ERROR) << "MiniMaster failed to start on RPC port " << rpc_port
               << ", web port " << web_port << ": " << start_status;
    // Don't crash here. Handle the error in the caller (e.g. could retry there).
  }
  return start_status;
}

Status MiniMaster::StartOnPorts(uint16_t rpc_port, uint16_t web_port,
                                MasterOptions* opts) {
  opts->rpc_opts.rpc_bind_addresses = Substitute("127.0.0.1:$0", rpc_port);
  opts->webserver_opts.port = web_port;
  opts->fs_opts.wal_paths = { fs_root_ };
  opts->fs_opts.data_paths = { fs_root_ };

  gscoped_ptr<Master> server(new YB_EDITION_NS_PREFIX Master(*opts));
  RETURN_NOT_OK(server->Init());
  RETURN_NOT_OK(server->StartAsync());

  master_.swap(server);
  running_ = true;

  return Status::OK();
}

Status MiniMaster::StartDistributedMasterOnPorts(uint16_t rpc_port, uint16_t web_port,
                                                 const vector<uint16_t>& peer_ports) {
  CHECK(!running_);
  CHECK(!master_);

  auto peer_addresses = std::make_shared<std::vector<HostPort>>();

  for (uint16_t peer_port : peer_ports) {
    HostPort peer_address("127.0.0.1", peer_port);
    peer_addresses->push_back(peer_address);
  }
  MasterOptions opts(peer_addresses);

  return StartOnPorts(rpc_port, web_port, &opts);
}

Status MiniMaster::Restart() {
  CHECK(running_);

  Endpoint prev_rpc = bound_rpc_addr();
  Endpoint prev_http = bound_http_addr();
  Shutdown();

  MasterOptions opts(std::make_shared<std::vector<HostPort>>());
  RETURN_NOT_OK(StartOnPorts(prev_rpc.port(), prev_http.port(), &opts));
  CHECK(running_);
  return WaitForCatalogManagerInit();
}

Status MiniMaster::WaitForCatalogManagerInit() {
  RETURN_NOT_OK(master_->catalog_manager()->WaitForWorkerPoolTests());
  return master_->WaitForCatalogManagerInit();
}

Status MiniMaster::WaitUntilCatalogManagerIsLeaderAndReadyForTests() {
  return master_->WaitUntilCatalogManagerIsLeaderAndReadyForTests();
}

Endpoint MiniMaster::bound_rpc_addr() const {
  CHECK(running_);
  return master_->first_rpc_address();
}

Endpoint MiniMaster::bound_http_addr() const {
  CHECK(running_);
  return master_->first_http_address();
}

std::string MiniMaster::permanent_uuid() const {
  CHECK(master_);
  return DCHECK_NOTNULL(master_->fs_manager())->uuid();
}

std::string MiniMaster::bound_rpc_addr_str() const {
  return yb::ToString(bound_rpc_addr());
}

size_t MiniMaster::NumSystemTables() const {
  return master_->NumSystemTables();
}

} // namespace master
} // namespace yb
