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

#include "yb/tserver/tablet_server.h"

#include <algorithm>
#include <limits>
#include <list>
#include <thread>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "yb/client/transaction_manager.h"
#include "yb/client/transaction_pool.h"

#include "yb/fs/fs_manager.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/service_if.h"
#include "yb/rpc/yb_rpc.h"
#include "yb/server/rpc_server.h"
#include "yb/server/webserver.h"
#include "yb/tablet/maintenance_manager.h"
#include "yb/tserver/heartbeater_factory.h"
#include "yb/tserver/metrics_snapshotter.h"
#include "yb/tserver/tablet_service.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver-path-handlers.h"
#include "yb/tserver/remote_bootstrap_service.h"
#include "yb/util/flag_tags.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/status.h"
#include "yb/util/env.h"
#include "yb/gutil/strings/split.h"
#include "yb/gutil/sysinfo.h"
#include "yb/rocksdb/env.h"

using std::make_shared;
using std::shared_ptr;
using std::vector;
using yb::rpc::ServiceIf;
using yb::tablet::TabletPeer;

using namespace yb::size_literals;
using namespace std::placeholders;

DEFINE_int32(tablet_server_svc_num_threads, -1,
             "Number of RPC worker threads for the TS service. If -1, it is auto configured.");
TAG_FLAG(tablet_server_svc_num_threads, advanced);

DEFINE_int32(ts_admin_svc_num_threads, 10,
             "Number of RPC worker threads for the TS admin service");
TAG_FLAG(ts_admin_svc_num_threads, advanced);

DEFINE_int32(ts_consensus_svc_num_threads, -1,
             "Number of RPC worker threads for the TS consensus service. If -1, it is auto "
             "configured.");
TAG_FLAG(ts_consensus_svc_num_threads, advanced);

DEFINE_int32(ts_remote_bootstrap_svc_num_threads, 10,
             "Number of RPC worker threads for the TS remote bootstrap service");
TAG_FLAG(ts_remote_bootstrap_svc_num_threads, advanced);

DEFINE_int32(tablet_server_svc_queue_length, yb::tserver::TabletServer::kDefaultSvcQueueLength,
             "RPC queue length for the TS service.");
TAG_FLAG(tablet_server_svc_queue_length, advanced);

DEFINE_int32(ts_admin_svc_queue_length, 50,
             "RPC queue length for the TS admin service");
TAG_FLAG(ts_admin_svc_queue_length, advanced);

DEFINE_int32(ts_consensus_svc_queue_length, yb::tserver::TabletServer::kDefaultSvcQueueLength,
             "RPC queue length for the TS consensus service.");
TAG_FLAG(ts_consensus_svc_queue_length, advanced);

DEFINE_int32(ts_remote_bootstrap_svc_queue_length, 50,
             "RPC queue length for the TS remote bootstrap service");
TAG_FLAG(ts_remote_bootstrap_svc_queue_length, advanced);

DEFINE_bool(enable_direct_local_tablet_server_call,
            true,
            "Enable direct call to local tablet server");
TAG_FLAG(enable_direct_local_tablet_server_call, advanced);

DEFINE_string(redis_proxy_bind_address, "", "Address to bind the redis proxy to");
DEFINE_int32(redis_proxy_webserver_port, 0, "Webserver port for redis proxy");

DEFINE_string(cql_proxy_bind_address, "", "Address to bind the CQL proxy to");
DEFINE_int32(cql_proxy_webserver_port, 0, "Webserver port for CQL proxy");

DEFINE_string(pgsql_proxy_bind_address, "", "Address to bind the PostgreSQL proxy to");
DECLARE_int32(pgsql_proxy_webserver_port);

DEFINE_int64(inbound_rpc_memory_limit, 0, "Inbound RPC memory limit");

DEFINE_bool(start_pgsql_proxy, false,
            "Whether to run a PostgreSQL server as a child process of the tablet server");

DEFINE_bool(tserver_enable_metrics_snapshotter, false, "Should metrics snapshotter be enabled");

namespace yb {
namespace tserver {

TabletServer::TabletServer(const TabletServerOptions& opts)
    : RpcAndWebServerBase(
          "TabletServer", opts, "yb.tabletserver", server::CreateMemTrackerForServer()),
      fail_heartbeats_for_tests_(false),
      opts_(opts),
      tablet_manager_(new TSTabletManager(fs_manager_.get(), this, metric_registry())),
      path_handlers_(new TabletServerPathHandlers(this)),
      maintenance_manager_(new MaintenanceManager(MaintenanceManager::DEFAULT_OPTIONS)),
      master_config_index_(0),
      tablet_server_service_(nullptr),
      shared_object_(CHECK_RESULT(TServerSharedObject::Create())) {
  SetConnectionContextFactory(rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>(
      FLAGS_inbound_rpc_memory_limit, mem_tracker()));

  LOG(INFO) << "yb::tserver::TabletServer created at " << this;
  LOG(INFO) << "yb::tserver::TSTabletManager created at " << tablet_manager_.get();
}

TabletServer::~TabletServer() {
  Shutdown();
}

std::string TabletServer::ToString() const {
  return strings::Substitute("TabletServer : rpc=$0, uuid=$1",
                             yb::ToString(first_rpc_address()),
                             fs_manager_->uuid());
}

Status TabletServer::ValidateMasterAddressResolution() const {
  return server::ResolveMasterAddresses(opts_.GetMasterAddresses(), nullptr);
}

Status TabletServer::UpdateMasterAddresses(const consensus::RaftConfigPB& new_config,
                                           bool is_master_leader) {
  shared_ptr<server::MasterAddresses> new_master_addresses;
  if (is_master_leader) {
    SetCurrentMasterIndex(new_config.opid_index());
    new_master_addresses = make_shared<server::MasterAddresses>();

    SetCurrentMasterIndex(new_config.opid_index());

    for (const auto& peer : new_config.peers()) {
      std::vector<HostPort> list;
      for (const auto& hp : peer.last_known_private_addr()) {
        list.push_back(HostPortFromPB(hp));
      }
      for (const auto& hp : peer.last_known_broadcast_addr()) {
        list.push_back(HostPortFromPB(hp));
      }
      new_master_addresses->push_back(std::move(list));
    }
  } else {
    new_master_addresses = make_shared<server::MasterAddresses>(*opts_.GetMasterAddresses());

    for (auto& list : *new_master_addresses) {
      std::sort(list.begin(), list.end());
    }

    for (const auto& peer : new_config.peers()) {
      std::vector<HostPort> list;
      for (const auto& hp : peer.last_known_private_addr()) {
        list.push_back(HostPortFromPB(hp));
      }
      for (const auto& hp : peer.last_known_broadcast_addr()) {
        list.push_back(HostPortFromPB(hp));
      }
      std::sort(list.begin(), list.end());
      bool found = false;
      for (const auto& existing : *new_master_addresses) {
        if (existing == list) {
          found = true;
          break;
        }
      }
      if (!found) {
        new_master_addresses->push_back(std::move(list));
      }
    }
  }

  LOG(INFO) << "Got new list of " << new_config.peers_size() << " masters at index "
            << new_config.opid_index() << " old masters = "
            << yb::ToString(opts_.GetMasterAddresses())
            << " new masters = " << yb::ToString(new_master_addresses) << " from "
            << (is_master_leader ? "leader." : "follower.");

  opts_.SetMasterAddresses(new_master_addresses);

  heartbeater_->set_master_addresses(new_master_addresses);

  return Status::OK();
}

Status TabletServer::Init() {
  CHECK(!initted_.load(std::memory_order_acquire));

  // Validate that the passed master address actually resolves.
  // We don't validate that we can connect at this point -- it should
  // be allowed to start the TS and the master in whichever order --
  // our heartbeat thread will loop until successfully connecting.
  RETURN_NOT_OK(ValidateMasterAddressResolution());

  RETURN_NOT_OK(RpcAndWebServerBase::Init());
  RETURN_NOT_OK(path_handlers_->Register(web_server_.get()));

  log_prefix_ = Format("P $0: ", permanent_uuid());

  heartbeater_ = CreateHeartbeater(opts_, this);

  if (FLAGS_tserver_enable_metrics_snapshotter) {
    metrics_snapshotter_.reset(new MetricsSnapshotter(opts_, this));
  }

  RETURN_NOT_OK_PREPEND(tablet_manager_->Init(),
                        "Could not init Tablet Manager");

  initted_.store(true, std::memory_order_release);

  auto bound_addresses = rpc_server()->GetBoundAddresses();
  if (!bound_addresses.empty()) {
    shared_object_->SetEndpoint(bound_addresses.front());
  }

  // 5433 is kDefaultPort in src/yb/yql/pgwrapper/pg_wrapper.h.
  RETURN_NOT_OK(pgsql_proxy_bind_address_.ParseString(FLAGS_pgsql_proxy_bind_address, 5433));
  shared_object_->SetPostgresAuthKey(RandomUniformInt<uint64_t>());

  return Status::OK();
}

Status TabletServer::WaitInited() {
  return tablet_manager_->WaitForAllBootstrapsToFinish();
}

void TabletServer::AutoInitServiceFlags() {
  const int32 num_cores = base::NumCPUs();

  if (FLAGS_tablet_server_svc_num_threads == -1) {
    // Auto select number of threads for the TS service based on number of cores.
    // But bound it between 64 & 512.
    const int32 num_threads = std::min(512, num_cores * 32);
    FLAGS_tablet_server_svc_num_threads = std::max(64, num_threads);
    LOG(INFO) << "Auto setting FLAGS_tablet_server_svc_num_threads to "
              << FLAGS_tablet_server_svc_num_threads;
  }

  if (FLAGS_ts_consensus_svc_num_threads == -1) {
    // Auto select number of threads for the TS service based on number of cores.
    // But bound it between 64 & 512.
    const int32 num_threads = std::min(512, num_cores * 32);
    FLAGS_ts_consensus_svc_num_threads = std::max(64, num_threads);
    LOG(INFO) << "Auto setting FLAGS_ts_consensus_svc_num_threads to "
              << FLAGS_ts_consensus_svc_num_threads;
  }
}

Status TabletServer::RegisterServices() {
  tablet_server_service_ = new TabletServiceImpl(this);
  LOG(INFO) << "yb::tserver::TabletServiceImpl created at " << tablet_server_service_;
  std::unique_ptr<ServiceIf> ts_service(tablet_server_service_);
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_tablet_server_svc_queue_length,
                                                     std::move(ts_service)));

  std::unique_ptr<ServiceIf> admin_service(new TabletServiceAdminImpl(this));
  LOG(INFO) << "yb::tserver::TabletServiceAdminImpl created at " << admin_service.get();
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_ts_admin_svc_queue_length,
                                                     std::move(admin_service)));

  std::unique_ptr<ServiceIf> consensus_service(new ConsensusServiceImpl(metric_entity(),
                                                                        tablet_manager_.get()));
  LOG(INFO) << "yb::tserver::ConsensusServiceImpl created at " << consensus_service.get();
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_ts_consensus_svc_queue_length,
                                                     std::move(consensus_service),
                                                     rpc::ServicePriority::kHigh));

  std::unique_ptr<ServiceIf> remote_bootstrap_service =
      std::make_unique<RemoteBootstrapServiceImpl>(
          fs_manager_.get(), tablet_manager_.get(), metric_entity());
  LOG(INFO) << "yb::tserver::RemoteBootstrapServiceImpl created at " <<
    remote_bootstrap_service.get();
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_ts_remote_bootstrap_svc_queue_length,
                                                     std::move(remote_bootstrap_service)));
  return Status::OK();
}

Status TabletServer::Start() {
  CHECK(initted_.load(std::memory_order_acquire));

  AutoInitServiceFlags();

  RETURN_NOT_OK(RegisterServices());
  RETURN_NOT_OK(RpcAndWebServerBase::Start());

  // If enabled, creates a proxy to call this tablet server locally.
  if (FLAGS_enable_direct_local_tablet_server_call) {
    proxy_ = std::make_shared<TabletServerServiceProxy>(proxy_cache_.get(), HostPort());
  }

  RETURN_NOT_OK(tablet_manager_->Start());

  RETURN_NOT_OK(heartbeater_->Start());

  if (FLAGS_tserver_enable_metrics_snapshotter) {
    RETURN_NOT_OK(metrics_snapshotter_->Start());
  }

  RETURN_NOT_OK(maintenance_manager_->Init());

  google::FlushLogFiles(google::INFO); // Flush the startup messages.

  return Status::OK();
}

void TabletServer::Shutdown() {
  LOG(INFO) << "TabletServer shutting down...";

  bool expected = true;
  if (initted_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
    maintenance_manager_->Shutdown();
    WARN_NOT_OK(heartbeater_->Stop(), "Failed to stop TS Heartbeat thread");

    if (FLAGS_tserver_enable_metrics_snapshotter) {
      WARN_NOT_OK(metrics_snapshotter_->Stop(), "Failed to stop TS Metrics Snapshotter thread");
    }

    {
      std::lock_guard<simple_spinlock> l(lock_);
      tablet_server_service_ = nullptr;
    }
    tablet_manager_->StartShutdown();
    RpcAndWebServerBase::Shutdown();
    tablet_manager_->CompleteShutdown();
  }

  LOG(INFO) << "TabletServer shut down complete. Bye!";
}

Status TabletServer::PopulateLiveTServers(const master::TSHeartbeatResponsePB& heartbeat_resp) {
  std::lock_guard<simple_spinlock> l(lock_);
  // We reset the list each time, since we want to keep the tservers that are live from the
  // master's perspective.
  // TODO: In the future, we should enhance the logic here to keep track information retrieved
  // from the master and compare it with information stored here. Based on this information, we
  // can only send diff updates CQL clients about whether a node came up or went down.
  live_tservers_.assign(heartbeat_resp.tservers().begin(), heartbeat_resp.tservers().end());
  return Status::OK();
}

Status TabletServer::GetTabletStatus(const GetTabletStatusRequestPB* req,
                                     GetTabletStatusResponsePB* resp) const {
  VLOG(3) << "GetTabletStatus called for tablet " << req->tablet_id();
  tablet::TabletPeerPtr peer;
  if (!tablet_manager_->LookupTablet(req->tablet_id(), &peer)) {
    return STATUS(NotFound, "Tablet not found", req->tablet_id());
  }
  peer->GetTabletStatusPB(resp->mutable_tablet_status());
  return Status::OK();
}

bool TabletServer::LeaderAndReady(const TabletId& tablet_id, bool allow_stale) const {
  tablet::TabletPeerPtr peer;
  if (!tablet_manager_->LookupTablet(tablet_id, &peer)) {
    return false;
  }
  return peer->LeaderStatus(allow_stale) == consensus::LeaderStatus::LEADER_AND_READY;
}

Status TabletServer::SetUniverseKeyRegistry(
    const yb::UniverseKeyRegistryPB& universe_key_registry) {
  return Status::OK();
}

void TabletServer::set_cluster_uuid(const std::string& cluster_uuid) {
  std::lock_guard<simple_spinlock> l(lock_);
  cluster_uuid_ = cluster_uuid;
}

std::string TabletServer::cluster_uuid() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return cluster_uuid_;
}

TabletServiceImpl* TabletServer::tablet_server_service() {
  std::lock_guard<simple_spinlock> l(lock_);
  return tablet_server_service_;
}

Status GetDynamicUrlTile(
  const string& path, const string& hostport, const int port,
  const string& http_addr_host, string* url) {
  // We get an incoming hostport string like '127.0.0.1:5433' or '[::1]:5433' or [::1]
  // and a port 13000 which has to be converted to '127.0.0.1:13000'. If the hostport is
  // a wildcard - 0.0.0.0 - the URLs are formed based on the http address for web instead
  HostPort hp;
  RETURN_NOT_OK(hp.ParseString(hostport, port));
  if (IsWildcardAddress(hp.host())) {
    hp.set_host(http_addr_host);
  }
  hp.set_port(port);

  *url = strings::Substitute("http://$0$1", hp.ToString(), path);
  return Status::OK();
}

Status TabletServer::DisplayRpcIcons(std::stringstream* output) {
  ServerRegistrationPB reg;
  RETURN_NOT_OK(GetRegistration(&reg));
  string http_addr_host = reg.http_addresses(0).host();

  // RPCs in Progress.
  DisplayIconTile(output, "fa-tasks", "TServer Live Ops", "/rpcz");
  // YCQL RPCs in Progress.
  string cass_url;
  RETURN_NOT_OK(GetDynamicUrlTile(
      "/rpcz", FLAGS_cql_proxy_bind_address, FLAGS_cql_proxy_webserver_port,
      http_addr_host, &cass_url));
  DisplayIconTile(output, "fa-tasks", "YCQL Live Ops", cass_url);

  // YEDIS RPCs in Progress.
  string redis_url;
  RETURN_NOT_OK(GetDynamicUrlTile(
      "/rpcz", FLAGS_redis_proxy_bind_address, FLAGS_redis_proxy_webserver_port,
      http_addr_host,  &redis_url));
  DisplayIconTile(output, "fa-tasks", "YEDIS Live Ops", redis_url);

  // YSQL RPCs in Progress.
  string sql_url;
  RETURN_NOT_OK(GetDynamicUrlTile(
      "/rpcz", FLAGS_pgsql_proxy_bind_address, FLAGS_pgsql_proxy_webserver_port,
      http_addr_host, &sql_url));
  DisplayIconTile(output, "fa-tasks", "YSQL Live Ops", sql_url);

  // YSQL All Ops
  string sql_all_url;
  RETURN_NOT_OK(GetDynamicUrlTile(
      "/statements", FLAGS_pgsql_proxy_bind_address, FLAGS_pgsql_proxy_webserver_port,
      http_addr_host, &sql_all_url));
  DisplayIconTile(output, "fa-tasks", "YSQL All Ops", sql_all_url);
  return Status::OK();
}

Env* TabletServer::GetEnv() {
  return Env::Default();
}

rocksdb::Env* TabletServer::GetRocksDBEnv() {
  return rocksdb::Env::Default();
}

int TabletServer::GetSharedMemoryFd() {
  return shared_object_.GetFd();
}

uint64_t TabletServer::GetSharedMemoryPostgresAuthKey() {
  return shared_object_->postgres_auth_key();
}

void TabletServer::SetYSQLCatalogVersion(uint64_t new_version, uint64_t new_breaking_version) {
  std::lock_guard<simple_spinlock> l(lock_);

  if (new_version > ysql_catalog_version_) {
    ysql_catalog_version_ = new_version;
    shared_object_->SetYSQLCatalogVersion(new_version);
    ysql_last_breaking_catalog_version_ = new_breaking_version;
  } else if (new_version < ysql_catalog_version_) {
    LOG(DFATAL) << "Ignoring ysql catalog version update: new version too old. "
                 << "New: " << new_version << ", Old: " << ysql_catalog_version_;
  }
}

TabletPeerLookupIf* TabletServer::tablet_peer_lookup() {
  return tablet_manager_.get();
}

client::YBClient* TabletServer::client() {
  return &tablet_manager_->client();
}

client::TransactionPool* TabletServer::TransactionPool() {
  auto result = transaction_pool_.load(std::memory_order_acquire);
  if (result) {
    return result;
  }
  std::lock_guard<decltype(transaction_pool_mutex_)> lock(transaction_pool_mutex_);
  if (transaction_pool_holder_) {
    return transaction_pool_holder_.get();
  }
  transaction_manager_holder_ = std::make_unique<client::TransactionManager>(
      &tablet_manager()->client(), clock(),
      std::bind(&TSTabletManager::PreserveLocalLeadersOnly, tablet_manager(), _1));
  transaction_pool_holder_ = std::make_unique<client::TransactionPool>(
      transaction_manager_holder_.get(), metric_entity().get());
  transaction_pool_.store(transaction_pool_holder_.get(), std::memory_order_release);
  return transaction_pool_holder_.get();
}

}  // namespace tserver
}  // namespace yb
