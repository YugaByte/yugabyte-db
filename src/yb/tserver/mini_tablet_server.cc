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

#include "yb/tserver/mini_tablet_server.h"

#include <utility>
#include <functional>

#include <glog/logging.h>

#include "yb/gutil/macros.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/common/schema.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log.pb.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.pb.h"

#include "yb/rpc/messenger.h"

#include "yb/server/metadata.h"
#include "yb/server/rpc_server.h"
#include "yb/server/webserver.h"

#include "yb/tablet/maintenance_manager.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/tablet-test-util.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/net/sockaddr.h"
#include "yb/util/net/tunnel.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"

using std::pair;

using yb::consensus::Consensus;
using yb::consensus::ConsensusOptions;
using yb::consensus::RaftPeerPB;
using yb::consensus::RaftConfigPB;
using yb::log::Log;
using yb::log::LogOptions;
using strings::Substitute;
using yb::tablet::TabletPeer;

DECLARE_bool(rpc_server_allow_ephemeral_ports);
DECLARE_double(leader_failure_max_missed_heartbeat_periods);

namespace yb {
namespace tserver {

MiniTabletServer::MiniTabletServer(const string& fs_root,
                                   uint16_t rpc_port,
                                   const TabletServerOptions& extra_opts,
                                   int index)
  : started_(false),
    opts_(extra_opts),
    index_(index + 1) {

  // Start RPC server on loopback.
  FLAGS_rpc_server_allow_ephemeral_ports = true;
  opts_.rpc_opts.rpc_bind_addresses = server::TEST_RpcBindEndpoint(index_, rpc_port);
  // A.B.C.D.xip.io resolves to A.B.C.D so it is very useful for testing.
  opts_.broadcast_addresses = {
      HostPort(server::TEST_RpcAddress(index_, server::Private::kFalse), rpc_port) };
  opts_.webserver_opts.port = 0;
  opts_.webserver_opts.bind_interface = opts_.broadcast_addresses.front().host();
  if (!opts_.has_placement_cloud()) {
    opts_.SetPlacement(Format("cloud$0", (index_ + 1) / 2), Format("rack$0", index_), "zone");
  }
  opts_.fs_opts.wal_paths = { fs_root };
  opts_.fs_opts.data_paths = { fs_root };
}

MiniTabletServer::~MiniTabletServer() {
}

Result<std::unique_ptr<MiniTabletServer>> MiniTabletServer::CreateMiniTabletServer(
    const string& fs_root, uint16_t rpc_port, int index) {
  auto options_result = TabletServerOptions::CreateTabletServerOptions();
  RETURN_NOT_OK(options_result);
  return std::make_unique<MiniTabletServer>(fs_root, rpc_port, *options_result, index);
}

Status MiniTabletServer::Start() {
  CHECK(!started_);

  gscoped_ptr<TabletServer> server(new enterprise::TabletServer(opts_));
  RETURN_NOT_OK(server->Init());

  server::TEST_SetupConnectivity(server->messenger(), index_);

  RETURN_NOT_OK(server->Start());

  server_.swap(server);

  tunnel_ = std::make_unique<Tunnel>(&server_->messenger()->io_service());
  auto se = ScopeExit([this] {
    if (!started_) {
      tunnel_->Shutdown();
    }
  });

  std::vector<Endpoint> local;
  RETURN_NOT_OK(opts_.broadcast_addresses[0].ResolveAddresses(&local));
  Endpoint remote = VERIFY_RESULT(ParseEndpoint(opts_.rpc_opts.rpc_bind_addresses, 0));
  RETURN_NOT_OK(tunnel_->Start(
      local.front(), remote, [messenger = server_->messenger()](const IpAddress& address) {
    return !messenger->TEST_ShouldArtificiallyRejectIncomingCallsFrom(address);
  }));

  started_ = true;
  return Status::OK();
}

void MiniTabletServer::SetIsolated(bool isolated) {
  if (isolated) {
    server::TEST_Isolate(server_->messenger());
  } else {
    server::TEST_SetupConnectivity(server_->messenger(), index_);
  }
}

Status MiniTabletServer::WaitStarted() {
  return server_->WaitInited();
}

void MiniTabletServer::Shutdown() {
  if (tunnel_) {
    tunnel_->Shutdown();
  }
  if (started_) {
    // Save bind address and port so we can later restart the server.
    opts_.rpc_opts.rpc_bind_addresses = server::TEST_RpcBindEndpoint(
        index_, bound_rpc_addr().port());
    opts_.webserver_opts.port = bound_http_addr().port();
    server_->Shutdown();
    tunnel_.reset();
    server_.reset();
  }
  started_ = false;
}

namespace {

CHECKED_STATUS ForAllTablets(
    MiniTabletServer* mts,
    std::function<Status(TabletPeer* tablet_peer)> action) {
  if (!mts->server()) {
    return STATUS(IllegalState, "Server is not running");
  }
  auto tablets = mts->server()->tablet_manager()->GetTabletPeers();
  for (const auto& tablet : tablets) {
    RETURN_NOT_OK(action(tablet.get()));
  }
  return Status::OK();
}

}  // namespace

Status MiniTabletServer::FlushTablets(tablet::FlushMode mode, tablet::FlushFlags flags) {
  if (!server_) {
    return Status::OK();
  }
  return ForAllTablets(this, [mode, flags](TabletPeer* tablet_peer) -> Status {
    if (!tablet_peer->tablet()) {
      return Status::OK();
    }
    return tablet_peer->tablet()->Flush(mode, flags);
  });
}

Status MiniTabletServer::CompactTablets() {
  if (!server_) {
    return Status::OK();
  }
  return ForAllTablets(this, [](TabletPeer* tablet_peer) {
    if (tablet_peer->tablet()) {
      tablet_peer->tablet()->ForceRocksDBCompactInTest();
    }
    return Status::OK();
  });
}

Status MiniTabletServer::SwitchMemtables() {
  return ForAllTablets(this, [](TabletPeer* tablet_peer) {
    return tablet_peer->tablet()->TEST_SwitchMemtable();
  });
}

Status MiniTabletServer::CleanTabletLogs() {
  if (!server_) {
    // Nothing to clean.
    return Status::OK();
  }
  return ForAllTablets(this, [](TabletPeer* tablet_peer) {
    return tablet_peer->RunLogGC();
  });
}

Status MiniTabletServer::Restart() {
  CHECK(started_);
  Shutdown();
  return Start();
}

Status MiniTabletServer::RestartStoppedServer() {
  Shutdown();
  return Start();
}

RaftConfigPB MiniTabletServer::CreateLocalConfig() const {
  CHECK(started_) << "Must Start()";
  RaftConfigPB config;
  RaftPeerPB* peer = config.add_peers();
  peer->set_permanent_uuid(server_->instance_pb().permanent_uuid());
  peer->set_member_type(RaftPeerPB::VOTER);
  auto host_port = peer->mutable_last_known_private_addr()->Add();
  host_port->set_host(bound_rpc_addr().address().to_string());
  host_port->set_port(bound_rpc_addr().port());
  return config;
}

Status MiniTabletServer::AddTestTablet(const std::string& ns_id,
                                       const std::string& table_id,
                                       const std::string& tablet_id,
                                       const Schema& schema,
                                       TableType table_type) {
  return AddTestTablet(ns_id, table_id, tablet_id, schema, CreateLocalConfig(), table_type);
}

Status MiniTabletServer::AddTestTablet(const std::string& ns_id,
                                       const std::string& table_id,
                                       const std::string& tablet_id,
                                       const Schema& schema,
                                       const RaftConfigPB& config,
                                       TableType table_type) {
  CHECK(started_) << "Must Start()";
  Schema schema_with_ids = SchemaBuilder(schema).Build();
  pair<PartitionSchema, Partition> partition = tablet::CreateDefaultPartition(schema_with_ids);

  return server_->tablet_manager()->CreateNewTablet(
      table_id, tablet_id, partition.second, ns_id, table_id, table_type, schema_with_ids,
      partition.first, boost::none /* index_info */, config, nullptr);
}

void MiniTabletServer::FailHeartbeats() {
  server_->set_fail_heartbeats_for_tests(true);
}

Endpoint MiniTabletServer::bound_rpc_addr() const {
  CHECK(started_);
  return server_->first_rpc_address();
}

Endpoint MiniTabletServer::bound_http_addr() const {
  CHECK(started_);
  return server_->first_http_address();
}

} // namespace tserver
} // namespace yb
