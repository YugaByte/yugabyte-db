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

#include "yb/master/master.h"

#include <algorithm>
#include <list>
#include <memory>
#include <vector>

#include <boost/bind.hpp>
#include <glog/logging.h>

#include "yb/common/wire_protocol.h"
#include "yb/consensus/raft_consensus.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/flush_manager.h"
#include "yb/master/master_rpc.h"
#include "yb/master/master_util.h"
#include "yb/master/master.pb.h"
#include "yb/master/master.service.h"
#include "yb/master/master_service.h"
#include "yb/master/master_tablet_service.h"
#include "yb/master/master-path-handlers.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/ts_manager.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/service_if.h"
#include "yb/rpc/service_pool.h"
#include "yb/rpc/yb_rpc.h"
#include "yb/server/rpc_server.h"
#include "yb/tablet/maintenance_manager.h"
#include "yb/server/default-path-handlers.h"
#include "yb/tserver/tablet_service.h"
#include "yb/tserver/remote_bootstrap_service.h"
#include "yb/util/flag_tags.h"
#include "yb/util/metrics.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/status.h"
#include "yb/util/threadpool.h"
#include "yb/util/shared_lock.h"
#include "yb/client/async_initializer.h"

DEFINE_int32(master_rpc_timeout_ms, 1500,
             "Timeout for retrieving master registration over RPC.");
TAG_FLAG(master_rpc_timeout_ms, experimental);

METRIC_DEFINE_entity(cluster);

using std::min;
using std::shared_ptr;
using std::vector;

using yb::consensus::RaftPeerPB;
using yb::rpc::ServiceIf;
using yb::tserver::ConsensusServiceImpl;
using strings::Substitute;

DEFINE_int32(master_tserver_svc_num_threads, 10,
             "Number of RPC worker threads to run for the master tserver service");
TAG_FLAG(master_tserver_svc_num_threads, advanced);

DEFINE_int32(master_svc_num_threads, 10,
             "Number of RPC worker threads to run for the master service");
TAG_FLAG(master_svc_num_threads, advanced);

DEFINE_int32(master_consensus_svc_num_threads, 10,
             "Number of RPC threads for the master consensus service");
TAG_FLAG(master_consensus_svc_num_threads, advanced);

DEFINE_int32(master_remote_bootstrap_svc_num_threads, 10,
             "Number of RPC threads for the master remote bootstrap service");
TAG_FLAG(master_remote_bootstrap_svc_num_threads, advanced);

DEFINE_int32(master_tserver_svc_queue_length, 1000,
             "RPC queue length for master tserver service");
TAG_FLAG(master_tserver_svc_queue_length, advanced);

DEFINE_int32(master_svc_queue_length, 1000,
             "RPC queue length for master service");
TAG_FLAG(master_svc_queue_length, advanced);

DEFINE_int32(master_consensus_svc_queue_length, 1000,
             "RPC queue length for master consensus service");
TAG_FLAG(master_consensus_svc_queue_length, advanced);

DEFINE_int32(master_remote_bootstrap_svc_queue_length, 50,
             "RPC queue length for master remote bootstrap service");
TAG_FLAG(master_remote_bootstrap_svc_queue_length, advanced);

DECLARE_int64(inbound_rpc_memory_limit);

namespace yb {
namespace master {

Master::Master(const MasterOptions& opts)
  : RpcAndWebServerBase(
        "Master", opts, "yb.master", server::CreateMemTrackerForServer()),
    state_(kStopped),
    ts_manager_(new TSManager()),
    catalog_manager_(new enterprise::CatalogManager(this)),
    path_handlers_(new MasterPathHandlers(this)),
    flush_manager_(new FlushManager(this, catalog_manager())),
    opts_(opts),
    registration_initialized_(false),
    maintenance_manager_(new MaintenanceManager(MaintenanceManager::DEFAULT_OPTIONS)),
    metric_entity_cluster_(METRIC_ENTITY_cluster.Instantiate(metric_registry_.get(),
                                                             "yb.cluster")),
    master_tablet_server_(new MasterTabletServer(this, metric_entity())) {
  SetConnectionContextFactory(rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>(
      GetAtomicFlag(&FLAGS_inbound_rpc_memory_limit),
      mem_tracker()));

  LOG(INFO) << "yb::master::Master created at " << this;
  LOG(INFO) << "yb::master::TSManager created at " << ts_manager_.get();
  LOG(INFO) << "yb::master::CatalogManager created at " << catalog_manager_.get();
}

Master::~Master() {
  Shutdown();
}

string Master::ToString() const {
  if (state_ != kRunning) {
    return "Master (stopped)";
  }
  return strings::Substitute("Master@$0", yb::ToString(first_rpc_address()));
}

Status Master::Init() {
  CHECK_EQ(kStopped, state_);

  RETURN_NOT_OK(ThreadPoolBuilder("init").set_max_threads(1).Build(&init_pool_));

  RETURN_NOT_OK(RpcAndWebServerBase::Init());

  RETURN_NOT_OK(path_handlers_->Register(web_server_.get()));

  async_client_init_ = std::make_unique<client::AsyncClientInitialiser>(
      "master_client", 0 /* num_reactors */,
      // TODO: use the correct flag
      60, // FLAGS_tserver_yb_client_default_timeout_ms / 1000,
      "" /* tserver_uuid */,
      &options(),
      metric_entity(),
      mem_tracker(),
      messenger());
  async_client_init_->builder().set_master_address_flag_name("master_addresses");
  async_client_init_->builder().AddMasterAddressSource([this] {
    std::vector<std::string> result;
    consensus::ConsensusStatePB state;
    auto status = catalog_manager_->GetCurrentConfig(&state);
    if (!status.ok()) {
      LOG(WARNING) << "Failed to get current config: " << status;
      return result;
    }
    for (const auto& peer : state.config().peers()) {
      std::vector<std::string> peer_addresses;
      for (const auto& list : {peer.last_known_private_addr(), peer.last_known_broadcast_addr()}) {
        for (const auto& entry : list) {
          peer_addresses.push_back(HostPort::FromPB(entry).ToString());
        }
      }
      if (!peer_addresses.empty()) {
        result.push_back(JoinStrings(peer_addresses, ","));
      }
    }
    return result;
  });
  async_client_init_->Start();

  state_ = kInitialized;
  return Status::OK();
}

Status Master::Start() {
  RETURN_NOT_OK(StartAsync());
  RETURN_NOT_OK(WaitForCatalogManagerInit());
  google::FlushLogFiles(google::INFO); // Flush the startup messages.
  return Status::OK();
}

Status Master::RegisterServices() {
  std::unique_ptr<ServiceIf> master_service(new MasterServiceImpl(this));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_master_svc_queue_length,
                                                     std::move(master_service)));

  std::unique_ptr<ServiceIf> master_tablet_service(
      new MasterTabletServiceImpl(master_tablet_server_.get(), this));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_master_tserver_svc_queue_length,
                                                     std::move(master_tablet_service)));

  std::unique_ptr<ServiceIf> consensus_service(
      new ConsensusServiceImpl(metric_entity(), catalog_manager_.get()));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_master_consensus_svc_queue_length,
                                                     std::move(consensus_service),
                                                     rpc::ServicePriority::kHigh));

  std::unique_ptr<ServiceIf> remote_bootstrap_service(
      new tserver::RemoteBootstrapServiceImpl(
          fs_manager_.get(), catalog_manager_.get(), metric_entity()));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_master_remote_bootstrap_svc_queue_length,
                                                     std::move(remote_bootstrap_service)));
  return Status::OK();
}

void Master::DisplayGeneralInfoIcons(std::stringstream* output) {
  server::RpcAndWebServerBase::DisplayGeneralInfoIcons(output);
  // Tasks.
  DisplayIconTile(output, "fa-list-ul", "Tasks", "/tasks");
  DisplayIconTile(output, "fa-list-ul", "Replica Info", "/tablet-replication");
}

Status Master::StartAsync() {
  CHECK_EQ(kInitialized, state_);

  RETURN_NOT_OK(maintenance_manager_->Init());
  RETURN_NOT_OK(RegisterServices());
  RETURN_NOT_OK(RpcAndWebServerBase::Start());

  // Now that we've bound, construct our ServerRegistrationPB.
  RETURN_NOT_OK(InitMasterRegistration());

  // Start initializing the catalog manager.
  RETURN_NOT_OK(init_pool_->SubmitClosure(Bind(&Master::InitCatalogManagerTask,
                                               Unretained(this))));

  state_ = kRunning;
  return Status::OK();
}

void Master::InitCatalogManagerTask() {
  Status s = InitCatalogManager();
  if (!s.ok()) {
    LOG(ERROR) << ToString() << ": Unable to init master catalog manager: " << s.ToString();
  }
  init_status_.Set(s);
}

Status Master::InitCatalogManager() {
  if (catalog_manager_->IsInitialized()) {
    return STATUS(IllegalState, "Catalog manager is already initialized");
  }
  RETURN_NOT_OK_PREPEND(catalog_manager_->Init(),
                        "Unable to initialize catalog manager");
  return Status::OK();
}

Status Master::WaitForCatalogManagerInit() {
  CHECK_EQ(state_, kRunning);

  return init_status_.Get();
}

Status Master::WaitUntilCatalogManagerIsLeaderAndReadyForTests(const MonoDelta& timeout) {
  RETURN_NOT_OK(catalog_manager_->WaitForWorkerPoolTests(timeout));
  Status s;
  MonoTime start = MonoTime::Now();
  int backoff_ms = 1;
  const int kMaxBackoffMs = 256;
  do {
    CatalogManager::ScopedLeaderSharedLock l(catalog_manager_.get());
    if (l.catalog_status().ok() && l.leader_status().ok()) {
      return Status::OK();
    }
    l.Unlock();

    SleepFor(MonoDelta::FromMilliseconds(backoff_ms));
    backoff_ms = min(backoff_ms << 1, kMaxBackoffMs);
  } while (MonoTime::Now().GetDeltaSince(start).LessThan(timeout));
  return STATUS(TimedOut, "Maximum time exceeded waiting for master leadership",
                          s.ToString());
}

void Master::Shutdown() {
  if (state_ == kRunning) {
    string name = ToString();
    LOG(INFO) << name << " shutting down...";
    maintenance_manager_->Shutdown();
    // We shutdown RpcAndWebServerBase here in order to shutdown messenger and reactor threads
    // before shutting down catalog manager. This is needed to prevent async calls callbacks
    // (running on reactor threads) from trying to use catalog manager thread pool which would be
    // already shutdown.
    RpcAndWebServerBase::Shutdown();
    catalog_manager_->Shutdown();
    LOG(INFO) << name << " shutdown complete.";
  } else {
    LOG(INFO) << ToString() << " did not start, shutting down all that started...";
    RpcAndWebServerBase::Shutdown();
  }
  state_ = kStopped;
}

Status Master::GetMasterRegistration(ServerRegistrationPB* reg) const {
  if (!registration_initialized_.load(std::memory_order_acquire)) {
    return STATUS(ServiceUnavailable, "Master startup not complete");
  }
  reg->CopyFrom(registration_);
  return Status::OK();
}

Status Master::InitMasterRegistration() {
  CHECK(!registration_initialized_.load());

  ServerRegistrationPB reg;
  RETURN_NOT_OK(GetRegistration(&reg));
  registration_.Swap(&reg);
  registration_initialized_.store(true);

  return Status::OK();
}

Status Master::ResetMemoryState(const RaftConfigPB& config) {
  LOG(INFO) << "Memory state set to config: " << config.ShortDebugString();

  auto master_addr = std::make_shared<server::MasterAddresses>();
  for (const RaftPeerPB& peer : config.peers()) {
    master_addr->push_back({HostPortFromPB(DesiredHostPort(peer, opts_.MakeCloudInfoPB()))});
  }

  SetMasterAddresses(std::move(master_addr));

  return Status::OK();
}

void Master::DumpMasterOptionsInfo(std::ostream* out) {
  *out << "Master options : ";
  auto master_addresses_shared_ptr = opts_.GetMasterAddresses();  // ENG-285
  bool first = true;
  for (const auto& list : *master_addresses_shared_ptr) {
    if (first) {
      first = false;
    } else {
      *out << ", ";
    }
    bool need_comma = false;
    for (const HostPort& hp : list) {
      if (need_comma) {
        *out << "/ ";
      }
      need_comma = true;
      *out << hp.ToString();
    }
  }
  *out << "\n";
}

Status Master::ListRaftConfigMasters(std::vector<RaftPeerPB>* masters) const {
  consensus::ConsensusStatePB cpb;
  RETURN_NOT_OK(catalog_manager_->GetCurrentConfig(&cpb));
  if (cpb.has_config()) {
    for (RaftPeerPB peer : cpb.config().peers()) {
      masters->push_back(peer);
    }
    return Status::OK();
  } else {
    return STATUS(NotFound, "No raft config found.");
  }
}

Status Master::ListMasters(std::vector<ServerEntryPB>* masters) const {
  if (IsShellMode()) {
    ServerEntryPB local_entry;
    local_entry.mutable_instance_id()->CopyFrom(catalog_manager_->NodeInstance());
    RETURN_NOT_OK(GetMasterRegistration(local_entry.mutable_registration()));
    local_entry.set_role(IsShellMode() ? RaftPeerPB::NON_PARTICIPANT : RaftPeerPB::LEADER);
    masters->push_back(local_entry);
    return Status::OK();
  }

  consensus::ConsensusStatePB cpb;
  RETURN_NOT_OK(catalog_manager_->GetCurrentConfig(&cpb));
  if (!cpb.has_config()) {
      return STATUS(NotFound, "No raft config found.");
  }

  for (const RaftPeerPB& peer : cpb.config().peers()) {
    // Get all network addresses associated with this peer master
    std::vector<HostPort> addrs;
    for (const auto& hp : peer.last_known_private_addr()) {
      addrs.push_back(HostPortFromPB(hp));
    }
    for (const auto& hp : peer.last_known_broadcast_addr()) {
      addrs.push_back(HostPortFromPB(hp));
    }

    // Make GetMasterRegistration calls for peer master info.
    ServerEntryPB peer_entry;
    Status s = GetMasterEntryForHosts(
        proxy_cache_.get(), addrs, MonoDelta::FromMilliseconds(FLAGS_master_rpc_timeout_ms),
        &peer_entry);
    if (!s.ok()) {
      // In case of errors talking to the peer master,
      // fill in fields from our catalog best as we can.
      s = s.CloneAndPrepend(
        Format("Unable to get registration information for peer ($0) id ($1)",
              addrs, peer.permanent_uuid()));
      LOG(WARNING) << "ListMasters: " << s;
      StatusToPB(s, peer_entry.mutable_error());
      peer_entry.mutable_instance_id()->set_permanent_uuid(peer.permanent_uuid());
      peer_entry.mutable_instance_id()->set_instance_seqno(0);
      auto reg = peer_entry.mutable_registration();
      reg->mutable_private_rpc_addresses()->CopyFrom(peer.last_known_private_addr());
      reg->mutable_broadcast_addresses()->CopyFrom(peer.last_known_broadcast_addr());
    }
    masters->push_back(peer_entry);
  }

  return Status::OK();
}

Status Master::InformRemovedMaster(const HostPortPB& hp_pb) {
  HostPort hp(hp_pb.host(), hp_pb.port());
  MasterServiceProxy proxy(proxy_cache_.get(), hp);
  RemovedMasterUpdateRequestPB req;
  RemovedMasterUpdateResponsePB resp;
  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(FLAGS_master_rpc_timeout_ms));
  RETURN_NOT_OK(proxy.RemovedMasterUpdate(req, &resp, &controller));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  return Status::OK();
}

Status Master::GoIntoShellMode() {
  maintenance_manager_->Shutdown();
  RETURN_NOT_OK(catalog_manager()->GoIntoShellMode());
  return Status::OK();
}

} // namespace master
} // namespace yb
