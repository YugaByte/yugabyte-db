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

#include "yb/integration-tests/mini_cluster.h"

#include <algorithm>

#include "yb/client/client.h"

#include "yb/consensus/consensus.h"

#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/mini_master.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"

#include "yb/rocksdb/db/db_impl.h"

#include "yb/rpc/messenger.h"
#include "yb/server/hybrid_clock.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/path_util.h"
#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"

using namespace std::literals;
using strings::Substitute;

DEFINE_string(mini_cluster_base_dir, "", "Directory for master/ts data");
DEFINE_bool(mini_cluster_reuse_data, false, "Reuse data of mini cluster");
DECLARE_int32(master_svc_num_threads);
DECLARE_int32(memstore_size_mb);
DECLARE_int32(master_consensus_svc_num_threads);
DECLARE_int32(master_remote_bootstrap_svc_num_threads);
DECLARE_int32(generic_svc_num_threads);
DECLARE_int32(tablet_server_svc_num_threads);
DECLARE_int32(ts_admin_svc_num_threads);
DECLARE_int32(ts_consensus_svc_num_threads);
DECLARE_int32(ts_remote_bootstrap_svc_num_threads);
DECLARE_int32(replication_factor);
DECLARE_string(use_private_ip);
DECLARE_int32(load_balancer_initial_delay_secs);

namespace yb {

using client::YBClient;
using client::YBClientBuilder;
using master::CatalogManager;
using master::MiniMaster;
using master::TabletLocationsPB;
using master::TSDescriptor;
using std::shared_ptr;
using std::string;
using std::vector;
using tserver::MiniTabletServer;
using tserver::TabletServer;

namespace {

const std::vector<uint16_t> EMPTY_MASTER_RPC_PORTS = {};
const int kMasterLeaderElectionWaitTimeSeconds = NonTsanVsTsan(20, 60);

std::string GetClusterDataDirName(const MiniClusterOptions& options) {
  std::string cluster_name = "minicluster-data";
  if (options.cluster_id == "") {
    return cluster_name;
  }
  return Format("$0-$1", cluster_name, options.cluster_id);
}

std::string GetFsRoot(const MiniClusterOptions& options) {
  if (!options.data_root.empty()) {
    return options.data_root;
  }
  if (!FLAGS_mini_cluster_base_dir.empty()) {
    return FLAGS_mini_cluster_base_dir;
  }
  return JoinPathSegments(GetTestDataDirectory(), GetClusterDataDirName(options));
}

} // namespace

MiniClusterOptions::MiniClusterOptions()
    : num_masters(1),
      num_tablet_servers(1) {
}

MiniCluster::MiniCluster(Env* env, const MiniClusterOptions& options)
    : env_(env),
      fs_root_(GetFsRoot(options)),
      num_masters_initial_(options.num_masters),
      num_ts_initial_(options.num_tablet_servers) {
  mini_masters_.resize(num_masters_initial_);
}

MiniCluster::~MiniCluster() {
  Shutdown();
}

Status MiniCluster::Start(const std::vector<tserver::TabletServerOptions>& extra_tserver_options) {
  CHECK(!fs_root_.empty()) << "No Fs root was provided";
  CHECK(!running_);

  EnsurePortsAllocated();

  if (!env_->FileExists(fs_root_)) {
    RETURN_NOT_OK(env_->CreateDir(fs_root_));
  }

  // TODO: properly handle setting these variables in case of multiple MiniClusters in the same
  // process.

  // Use conservative number of threads for the mini cluster for unit test env
  // where several unit tests tend to run in parallel.
  // To get default number of threads - try to find SERVICE_POOL_OPTIONS macro usage.
  FLAGS_master_svc_num_threads = 2;
  FLAGS_master_consensus_svc_num_threads = 2;
  FLAGS_master_remote_bootstrap_svc_num_threads = 2;
  FLAGS_generic_svc_num_threads = 2;

  FLAGS_tablet_server_svc_num_threads = 8;
  FLAGS_ts_admin_svc_num_threads = 2;
  FLAGS_ts_consensus_svc_num_threads = 8;
  FLAGS_ts_remote_bootstrap_svc_num_threads = 2;

  // We are testing public/private IPs using mini cluster. So set mode to 'cloud'.
  FLAGS_use_private_ip = "cloud";

  // This dictates the RF of newly created tables.
  SetAtomicFlag(num_ts_initial_ >= 3 ? 3 : 1, &FLAGS_replication_factor);
  FLAGS_memstore_size_mb = 16;
  // Default master args to make sure we don't wait to trigger new LB tasks upon master leader
  // failover.
  FLAGS_load_balancer_initial_delay_secs = 0;

  // start the masters
  RETURN_NOT_OK_PREPEND(StartMasters(),
                        "Couldn't start distributed masters");

  if (!extra_tserver_options.empty() && extra_tserver_options.size() != num_ts_initial_) {
    return STATUS_SUBSTITUTE(InvalidArgument, "num tserver options: $0 doesn't match with num "
        "tservers: $1", extra_tserver_options.size(), num_ts_initial_);
  }

  for (int i = 0; i < num_ts_initial_; i++) {
    if (!extra_tserver_options.empty()) {
      RETURN_NOT_OK_PREPEND(AddTabletServer(extra_tserver_options[i]),
                            Substitute("Error adding TS $0", i));
    } else {
      RETURN_NOT_OK_PREPEND(AddTabletServer(),
                            Substitute("Error adding TS $0", i));
    }

  }

  RETURN_NOT_OK_PREPEND(WaitForTabletServerCount(num_ts_initial_),
                        "Waiting for tablet servers to start");

  running_ = true;
  return Status::OK();
}

Status MiniCluster::StartMasters() {
  CHECK_GE(master_rpc_ports_.size(), num_masters_initial_);
  EnsurePortsAllocated();

  LOG(INFO) << "Creating distributed mini masters. RPC ports: "
            << JoinInts(master_rpc_ports_, ", ");

  if (mini_masters_.size() < num_masters_initial_) {
    mini_masters_.resize(num_masters_initial_);
  }

  bool started = false;
  auto se = ScopeExit([this, &started] {
    if (!started) {
      for (const auto& master : mini_masters_) {
        if (master) {
          master->Shutdown();
        }
      }
    }
  });

  for (int i = 0; i < num_masters_initial_; i++) {
    mini_masters_[i] = std::make_shared<MiniMaster>(
        env_, GetMasterFsRoot(i), master_rpc_ports_[i], master_web_ports_[i], i);
    auto status = mini_masters_[i]->StartDistributedMaster(master_rpc_ports_);
    LOG_IF(INFO, !status.ok()) << "Failed to start master: " << status;
    RETURN_NOT_OK_PREPEND(status, Substitute("Couldn't start follower $0", i));
    VLOG(1) << "Started MiniMaster with UUID " << mini_masters_[i]->permanent_uuid()
            << " at index " << i;
  }
  int i = 0;
  for (const shared_ptr<MiniMaster>& master : mini_masters_) {
    LOG(INFO) << "Waiting to initialize catalog manager on master " << i++;
    RETURN_NOT_OK_PREPEND(master->WaitForCatalogManagerInit(),
                          Substitute("Could not initialize catalog manager on master $0", i));
  }
  started = true;
  return Status::OK();
}

Status MiniCluster::StartSync() {
  RETURN_NOT_OK(Start());
  int count = 0;
  for (const shared_ptr<MiniTabletServer>& tablet_server : mini_tablet_servers_) {
    RETURN_NOT_OK_PREPEND(tablet_server->WaitStarted(),
                          Substitute("TabletServer $0 failed to start.", count));
    count++;
  }
  return Status::OK();
}

Status MiniCluster::RestartSync() {
  LOG(INFO) << string(80, '-');
  LOG(INFO) << __FUNCTION__;
  LOG(INFO) << string(80, '-');

  LOG(INFO) << "Restart tablet server(s)...";
  for (auto& tablet_server : mini_tablet_servers_) {
    CHECK_OK(tablet_server->Restart());
    CHECK_OK(tablet_server->WaitStarted());
  }
  LOG(INFO) << "Restart master server(s)...";
  for (auto& master_server : mini_masters_) {
    LOG(INFO) << "Restarting master " << master_server->permanent_uuid();
    LongOperationTracker long_operation_tracker("Master restart", 5s);
    CHECK_OK(master_server->Restart());
    LOG(INFO) << "Waiting for catalog manager at " << master_server->permanent_uuid();
    CHECK_OK(master_server->WaitForCatalogManagerInit());
  }
  LOG(INFO) << string(80, '-');
  LOG(INFO) << __FUNCTION__ << " done";
  LOG(INFO) << string(80, '-');

  RETURN_NOT_OK_PREPEND(WaitForAllTabletServers(),
                        "Waiting for tablet servers to start");
  running_ = true;
  return Status::OK();
}

Status MiniCluster::AddTabletServer(const tserver::TabletServerOptions& extra_opts) {
  if (mini_masters_.empty()) {
    return STATUS(IllegalState, "Master not yet initialized");
  }
  int new_idx = mini_tablet_servers_.size();

  EnsurePortsAllocated(0 /* num_masters (will pick default) */, new_idx + 1);
  const uint16_t ts_rpc_port = tserver_rpc_ports_[new_idx];
  gscoped_ptr<MiniTabletServer> tablet_server(
    new MiniTabletServer(GetTabletServerFsRoot(new_idx), ts_rpc_port, extra_opts, new_idx));

  // set the master addresses
  auto master_addr = std::make_shared<server::MasterAddresses>();
  for (const shared_ptr<MiniMaster>& master : mini_masters_) {
    master_addr->push_back({HostPort(master->bound_rpc_addr())});
    for (const auto& hp : master->master()->opts().broadcast_addresses) {
      master_addr->back().push_back(hp);
    }
  }

  tablet_server->options()->master_addresses_flag = server::MasterAddressesToString(*master_addr);
  tablet_server->options()->SetMasterAddresses(master_addr);
  tablet_server->options()->webserver_opts.port = tserver_web_ports_[new_idx];
  RETURN_NOT_OK(tablet_server->Start());
  mini_tablet_servers_.push_back(shared_ptr<MiniTabletServer>(tablet_server.release()));
  return Status::OK();
}

Status MiniCluster::AddTabletServer() {
  auto options = tserver::TabletServerOptions::CreateTabletServerOptions();
  RETURN_NOT_OK(options);
  return AddTabletServer(*options);
}

string MiniCluster::GetMasterAddresses() const {
  string peer_addrs = "";
  for (const auto& master : mini_masters_) {
    if (!peer_addrs.empty()) {
      peer_addrs += ",";
    }
    peer_addrs += master->bound_rpc_addr_str();
  }
  return peer_addrs;
}

int MiniCluster::LeaderMasterIdx() {
  Stopwatch sw;
  sw.start();
  while (sw.elapsed().wall_seconds() < kMasterLeaderElectionWaitTimeSeconds) {
    for (int i = 0; i < mini_masters_.size(); i++) {
      MiniMaster* master = mini_master(i);
      if (master->master() == nullptr || master->master()->IsShutdown()) {
        continue;
      }
      CatalogManager::ScopedLeaderSharedLock l(master->master()->catalog_manager());
      if (l.catalog_status().ok() && l.leader_status().ok()) {
        return i;
      }
    }
    SleepFor(MonoDelta::FromMilliseconds(1));
  }
  LOG(ERROR) << "No leader master elected after " << kMasterLeaderElectionWaitTimeSeconds
             << " seconds.";
  return -1;
}

MiniMaster* MiniCluster::leader_mini_master() {
  auto idx = LeaderMasterIdx();
  return idx != -1 ? mini_master(idx) : nullptr;
}

void MiniCluster::Shutdown() {
  if (!running_)
    return;

  for (const shared_ptr<MiniTabletServer>& tablet_server : mini_tablet_servers_) {
    tablet_server->Shutdown();
  }
  mini_tablet_servers_.clear();

  for (shared_ptr<MiniMaster>& master_server : mini_masters_) {
    master_server->Shutdown();
    master_server.reset();
  }
  mini_masters_.clear();

  running_ = false;
}

Status MiniCluster::FlushTablets(tablet::FlushMode mode, tablet::FlushFlags flags) {
  for (const auto& tablet_server : mini_tablet_servers_) {
    RETURN_NOT_OK(tablet_server->FlushTablets(mode, flags));
  }
  return Status::OK();
}

Status MiniCluster::CompactTablets() {
  for (const auto& tablet_server : mini_tablet_servers_) {
    RETURN_NOT_OK(tablet_server->CompactTablets());
  }
  return Status::OK();
}

Status MiniCluster::SwitchMemtables() {
  for (const auto& tablet_server : mini_tablet_servers_) {
    RETURN_NOT_OK(tablet_server->SwitchMemtables());
  }
  return Status::OK();
}

Status MiniCluster::CleanTabletLogs() {
  for (const auto& tablet_server : mini_tablet_servers_) {
    RETURN_NOT_OK(tablet_server->CleanTabletLogs());
  }
  return Status::OK();
}

void MiniCluster::ShutdownMasters() {
  for (shared_ptr<MiniMaster>& master_server : mini_masters_) {
    master_server->Shutdown();
    master_server.reset();
  }
}

MiniMaster* MiniCluster::mini_master(int idx) {
  CHECK_GE(idx, 0) << "Master idx must be >= 0";
  CHECK_LT(idx, mini_masters_.size()) << "Master idx must be < num masters started";
  return mini_masters_[idx].get();
}

MiniTabletServer* MiniCluster::mini_tablet_server(int idx) {
  CHECK_GE(idx, 0) << "TabletServer idx must be >= 0";
  CHECK_LT(idx, mini_tablet_servers_.size()) << "TabletServer idx must be < 'num_ts_started_'";
  return mini_tablet_servers_[idx].get();
}

MiniTabletServer* MiniCluster::find_tablet_server(const std::string& uuid) {
  for (const auto& server : mini_tablet_servers_) {
    if (!server->server()) {
      continue;
    }
    if (server->server()->instance_pb().permanent_uuid() == uuid) {
      return server.get();
    }
  }
  return nullptr;
}

string MiniCluster::GetMasterFsRoot(int idx) {
  return JoinPathSegments(fs_root_, Substitute("master-$0-root", idx + 1));
}

string MiniCluster::GetTabletServerFsRoot(int idx) {
  return JoinPathSegments(fs_root_, Substitute("ts-$0-root", idx + 1));
}

tserver::TSTabletManager* MiniCluster::GetTabletManager(int idx) {
  return mini_tablet_server(idx)->server()->tablet_manager();
}

std::vector<std::shared_ptr<tablet::TabletPeer>> MiniCluster::GetTabletPeers(int idx) {
  return GetTabletManager(idx)->GetTabletPeers();
}

Status MiniCluster::WaitForReplicaCount(const string& tablet_id,
                                        int expected_count,
                                        TabletLocationsPB* locations) {
  Stopwatch sw;
  sw.start();
  while (sw.elapsed().wall_seconds() < kTabletReportWaitTimeSeconds) {
    locations->Clear();
    Status s =
        leader_mini_master()->master()->catalog_manager()->GetTabletLocations(tablet_id, locations);
    if (s.ok() && ((locations->stale() && expected_count == 0) ||
        (!locations->stale() && locations->replicas_size() == expected_count))) {
      return Status::OK();
    }

    SleepFor(MonoDelta::FromMilliseconds(1));
  }
  return STATUS(TimedOut, Substitute("Tablet $0 never reached expected replica count $1",
                                     tablet_id, expected_count));
}

Status MiniCluster::WaitForAllTabletServers() {
  return WaitForTabletServerCount(num_tablet_servers());
}

Status MiniCluster::WaitForTabletServerCount(int count) {
  vector<shared_ptr<master::TSDescriptor> > descs;
  return WaitForTabletServerCount(count, &descs);
}

Status MiniCluster::WaitForTabletServerCount(int count,
                                             vector<shared_ptr<TSDescriptor> >* descs) {
  Stopwatch sw;
  sw.start();
  while (sw.elapsed().wall_seconds() < kRegistrationWaitTimeSeconds) {
    auto leader = leader_mini_master();
    if (leader) {
      leader->master()->ts_manager()->GetAllDescriptors(descs);
      if (descs->size() == count) {
        // GetAllDescriptors() may return servers that are no longer online.
        // Do a second step of verification to verify that the descs that we got
        // are aligned (same uuid/seqno) with the TSs that we have in the cluster.
        int match_count = 0;
        for (const shared_ptr<TSDescriptor>& desc : *descs) {
          for (auto mini_tablet_server : mini_tablet_servers_) {
            auto ts = mini_tablet_server->server();
            if (ts->instance_pb().permanent_uuid() == desc->permanent_uuid() &&
                ts->instance_pb().instance_seqno() == desc->latest_seqno()) {
              match_count++;
              break;
            }
          }
        }

        if (match_count == count) {
          LOG(INFO) << count << " TS(s) registered with Master after "
                    << sw.elapsed().wall_seconds() << "s";
          return Status::OK();
        }
      }

      YB_LOG_EVERY_N_SECS(INFO, 5) << "Registered: " << AsString(*descs);
    }

    SleepFor(MonoDelta::FromMilliseconds(1));
  }
  return STATUS(TimedOut, Substitute("$0 TS(s) never registered with master", count));
}

void MiniCluster::ConfigureClientBuilder(YBClientBuilder* builder) {
  CHECK_NOTNULL(builder);
  builder->clear_master_server_addrs();
  for (const shared_ptr<MiniMaster>& master : mini_masters_) {
    CHECK(master);
    builder->add_master_server_addr(master->bound_rpc_addr_str());
  }
}

HostPort MiniCluster::DoGetLeaderMasterBoundRpcAddr() {
  return leader_mini_master()->bound_rpc_addr();
}

void MiniCluster::AllocatePortsForDaemonType(
    const string daemon_type,
    const int num_daemons,
    const string port_type,
    std::vector<uint16_t>* ports) {
  const size_t old_size = ports->size();
  if (ports->size() < num_daemons) {
    ports->resize(num_daemons, 0 /* default value */);
  }
  for (int i = old_size; i < num_daemons; ++i) {
    if ((*ports)[i] == 0) {
      const uint16_t new_port = port_picker_.AllocateFreePort();
      (*ports)[i] = new_port;
      LOG(INFO) << "Using auto-assigned port " << new_port << " for a " << daemon_type
                << " " << port_type << " port";
    }
  }
}

void MiniCluster::EnsurePortsAllocated(int new_num_masters, int new_num_tservers) {
  if (new_num_masters == 0) {
    new_num_masters = std::max(num_masters_initial_, num_masters());
  }
  AllocatePortsForDaemonType("master", new_num_masters, "RPC", &master_rpc_ports_);
  AllocatePortsForDaemonType("master", new_num_masters, "web", &master_web_ports_);

  if (new_num_tservers == 0) {
    new_num_tservers = std::max(num_ts_initial_, num_tablet_servers());
  }
  AllocatePortsForDaemonType("tablet server", new_num_tservers, "RPC", &tserver_rpc_ports_);
  AllocatePortsForDaemonType("tablet server", new_num_tservers, "web", &tserver_web_ports_);
}

std::vector<server::SkewedClockDeltaChanger> SkewClocks(
    MiniCluster* cluster, std::chrono::milliseconds clock_skew) {
  std::vector<server::SkewedClockDeltaChanger> delta_changers;
  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    auto* tserver = cluster->mini_tablet_server(i)->server();
    auto* hybrid_clock = down_cast<server::HybridClock*>(tserver->clock());
    delta_changers.emplace_back(
        i * clock_skew, std::static_pointer_cast<server::SkewedClock>(hybrid_clock->TEST_clock()));
  }
  return delta_changers;
}

void StepDownAllTablets(MiniCluster* cluster) {
  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    for (const auto& peer : cluster->GetTabletPeers(i)) {
      consensus::LeaderStepDownRequestPB req;
      req.set_tablet_id(peer->tablet_id());
      consensus::LeaderStepDownResponsePB resp;
      ASSERT_OK(peer->consensus()->StepDown(&req, &resp));
    }
  }
}

void StepDownRandomTablet(MiniCluster* cluster) {
  auto peers = ListTabletPeers(cluster, ListPeersFilter::kLeaders);
  if (!peers.empty()) {
    auto peer = RandomElement(peers);

    consensus::LeaderStepDownRequestPB req;
    req.set_tablet_id(peer->tablet_id());
    consensus::LeaderStepDownResponsePB resp;
    ASSERT_OK(peer->consensus()->StepDown(&req, &resp));
  }
}

std::unordered_set<string> ListTabletIdsForTable(MiniCluster* cluster, const string& table_id) {
  std::unordered_set<string> tablet_ids;
  for (auto peer : ListTabletPeers(cluster, ListPeersFilter::kAll)) {
    if (peer->tablet_metadata()->table_id() == table_id) {
      tablet_ids.insert(peer->tablet_id());
    }
  }
  return tablet_ids;
}

std::vector<tablet::TabletPeerPtr> ListTabletPeers(MiniCluster* cluster, ListPeersFilter filter) {
  switch (filter) {
    case ListPeersFilter::kAll:
      return ListTabletPeers(cluster, [](const auto& peer) { return true; });
    case ListPeersFilter::kLeaders:
      return ListTabletPeers(cluster, [](const auto& peer) {
        return peer->consensus()->GetLeaderStatus() != consensus::LeaderStatus::NOT_LEADER;
      });
    case ListPeersFilter::kNonLeaders:
      return ListTabletPeers(cluster, [](const auto& peer) {
        return peer->consensus()->GetLeaderStatus() == consensus::LeaderStatus::NOT_LEADER;
      });
  }

  FATAL_INVALID_ENUM_VALUE(ListPeersFilter, filter);
}

std::vector<tablet::TabletPeerPtr> ListTabletPeers(
    MiniCluster* cluster,
    const std::function<bool(const std::shared_ptr<tablet::TabletPeer>&)>& filter) {
  std::vector<tablet::TabletPeerPtr> result;

  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    auto server = cluster->mini_tablet_server(i)->server();
    if (!server) { // Server is shut down.
      continue;
    }
    auto peers = server->tablet_manager()->GetTabletPeers();
    for (const auto& peer : peers) {
      WARN_NOT_OK(
          WaitFor([peer] { return peer->consensus() != nullptr; }, 5s,
          Format("Waiting peer T $0 P $1 ready", peer->tablet_id(), peer->permanent_uuid())),
          "List tablet peers failure");
      if (filter(peer)) {
        result.push_back(peer);
      }
    }
  }

  return result;
}

std::vector<tablet::TabletPeerPtr> ListTableTabletLeadersPeers(
    MiniCluster* cluster, const TableId& table_id) {
  return ListTabletPeers(cluster, [&table_id](const auto& peer) {
    return peer->tablet_metadata() &&
           peer->tablet_metadata()->table_id() == table_id &&
           peer->tablet_metadata()->tablet_data_state() !=
               tablet::TabletDataState::TABLET_DATA_SPLIT_COMPLETED &&
           peer->consensus()->GetLeaderStatus() != consensus::LeaderStatus::NOT_LEADER;
  });
}

std::vector<tablet::TabletPeerPtr> ListTableTabletPeers(
      MiniCluster* cluster, const TableId& table_id) {
  return ListTabletPeers(cluster, [table_id](const std::shared_ptr<tablet::TabletPeer>& peer) {
    return peer->tablet_metadata()->table_id() == table_id;
  });
}

std::vector<tablet::TabletPeerPtr> ListTableActiveTabletPeers(
      MiniCluster* cluster, const TableId& table_id) {
  std::vector<tablet::TabletPeerPtr> result;
  for (auto peer : ListTableTabletPeers(cluster, table_id)) {
    if (peer->tablet()->metadata()->tablet_data_state() !=
        tablet::TabletDataState::TABLET_DATA_SPLIT_COMPLETED) {
      result.push_back(peer);
    }
  }
  return result;
}

std::vector<tablet::TabletPeerPtr> ListTableInactiveSplitTabletPeers(
    MiniCluster* cluster, const TableId& table_id) {
  std::vector<tablet::TabletPeerPtr> result;
  for (auto peer : ListTableTabletPeers(cluster, table_id)) {
    if (peer->tablet()->metadata()->tablet_data_state() ==
        tablet::TabletDataState::TABLET_DATA_SPLIT_COMPLETED) {
      result.push_back(peer);
    }
  }
  return result;
}

Status WaitUntilTabletHasLeader(
    MiniCluster* cluster, const string& tablet_id, MonoTime deadline) {
  return Wait([cluster, &tablet_id] {
    auto tablet_peers = ListTabletPeers(cluster, [&tablet_id](auto peer) {
      return peer->tablet_id() == tablet_id
          && peer->consensus()->GetLeaderStatus() != consensus::LeaderStatus::NOT_LEADER;
    });
    return tablet_peers.size() == 1;
  }, deadline, "Waiting for election in tablet " + tablet_id);
}

Status WaitForLeaderOfSingleTablet(
    MiniCluster* cluster, tablet::TabletPeerPtr leader, MonoDelta duration,
    const std::string& description) {
  return WaitFor([cluster, &leader] {
    auto new_leaders = ListTabletPeers(cluster, ListPeersFilter::kLeaders);
    return new_leaders.size() == 1 && new_leaders[0] == leader;
  }, duration, description);
}

Status StepDown(
    tablet::TabletPeerPtr leader, const std::string& new_leader_uuid,
    ForceStepDown force_step_down) {
  consensus::LeaderStepDownRequestPB req;
  req.set_tablet_id(leader->tablet_id());
  req.set_new_leader_uuid(new_leader_uuid);
  if (force_step_down) {
    req.set_force_step_down(true);
  }
  consensus::LeaderStepDownResponsePB resp;
  RETURN_NOT_OK(leader->consensus()->StepDown(&req, &resp));
  if (resp.has_error()) {
    return STATUS_FORMAT(RuntimeError, "Step down failed: $0", resp);
  }
  return Status::OK();
}

std::thread RestartsThread(
    MiniCluster* cluster, CoarseDuration interval, std::atomic<bool>* stop_flag) {
  return std::thread([cluster, interval, stop_flag] {
    CDSAttacher attacher;
    SetFlagOnExit set_stop_on_exit(stop_flag);
    int it = 0;
    while (!stop_flag->load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(interval);
      ASSERT_OK(cluster->mini_tablet_server(++it % cluster->num_tablet_servers())->Restart());
    }
  });
}

Status WaitAllReplicasReady(MiniCluster* cluster, MonoDelta timeout) {
  return WaitFor([cluster] {
    std::unordered_set<std::string> tablet_ids;
    auto peers = ListTabletPeers(cluster, ListPeersFilter::kAll);
    for (const auto& peer : peers) {
      if (peer->state() != tablet::RaftGroupStatePB::RUNNING) {
        return false;
      }
      tablet_ids.insert(peer->tablet_id());
    }
    auto replication_factor = cluster->num_tablet_servers();
    return tablet_ids.size() * replication_factor == peers.size();
  }, timeout, "Wait all replicas to be ready");
}

Status WaitAllReplicasHaveIndex(MiniCluster* cluster, int64_t index, MonoDelta timeout) {
  return WaitFor([cluster, index] {
    std::unordered_set<std::string> tablet_ids;
    auto peers = ListTabletPeers(cluster, ListPeersFilter::kAll);
    for (const auto& peer : peers) {
      if (peer->GetLatestLogEntryOpId().index < index) {
        return false;
      }
      tablet_ids.insert(peer->tablet_id());
    }
    auto replication_factor = cluster->num_tablet_servers();
    return tablet_ids.size() * replication_factor == peers.size();
  }, timeout, "Wait for all replicas to have a specific Raft index");
}

template <class Collection>
void PushBackIfNotNull(const typename Collection::value_type& value, Collection* collection) {
  if (value != nullptr) {
    collection->push_back(value);
  }
}

std::vector<rocksdb::DB*> GetAllRocksDbs(MiniCluster* cluster, bool include_intents) {
  std::vector<rocksdb::DB*> dbs;
  for (auto& peer : ListTabletPeers(cluster, ListPeersFilter::kAll)) {
    const auto* tablet = peer->tablet();
    PushBackIfNotNull(tablet->TEST_db(), &dbs);
    if (include_intents) {
      PushBackIfNotNull(tablet->TEST_intents_db(), &dbs);
    }
  }
  return dbs;
}

int NumTotalRunningCompactions(MiniCluster* cluster) {
  int compactions = 0;
  for (auto* db : GetAllRocksDbs(cluster)) {
    compactions += down_cast<rocksdb::DBImpl*>(db)->TEST_NumTotalRunningCompactions();
  }
  return compactions;
}

int NumRunningFlushes(MiniCluster* cluster) {
  int flushes = 0;
  for (auto* db : GetAllRocksDbs(cluster)) {
    flushes += down_cast<rocksdb::DBImpl*>(db)->TEST_NumRunningFlushes();
  }
  return flushes;
}

Result<scoped_refptr<master::TableInfo>> FindTable(
    MiniCluster* cluster, const client::YBTableName& table_name) {
  auto* catalog_manager = cluster->leader_mini_master()->master()->catalog_manager();
  scoped_refptr<master::TableInfo> table_info;
  master::TableIdentifierPB identifier;
  table_name.SetIntoTableIdentifierPB(&identifier);
  RETURN_NOT_OK(catalog_manager->FindTable(identifier, &table_info));
  return table_info;
}

Status WaitForInitDb(MiniCluster* cluster) {
  const auto start_time = CoarseMonoClock::now();
  const auto kTimeout = RegularBuildVsSanitizers(600s, 1800s);
  while (CoarseMonoClock::now() <= start_time + kTimeout) {
    auto* catalog_manager = cluster->leader_mini_master()->master()->catalog_manager();
    master::IsInitDbDoneRequestPB req;
    master::IsInitDbDoneResponsePB resp;
    auto status = catalog_manager->IsInitDbDone(&req, &resp);
    if (!status.ok()) {
      LOG(INFO) << "IsInitDbDone failure: " << status;
      continue;
    }
    if (resp.has_initdb_error()) {
      return STATUS_FORMAT(RuntimeError, "Init DB failed: $0", resp.initdb_error());
    }
    if (resp.done()) {
      return Status::OK();
    }
    std::this_thread::sleep_for(500ms);
  }

  return STATUS_FORMAT(TimedOut, "Unable to init db in $0", kTimeout);
}

size_t CountIntents(MiniCluster* cluster, const TabletPeerFilter& filter) {
  size_t result = 0;
  auto peers = ListTabletPeers(cluster, ListPeersFilter::kAll);
  for (const auto &peer : peers) {
    auto participant = peer->tablet() ? peer->tablet()->transaction_participant() : nullptr;
    if (!participant) {
      continue;
    }
    if (filter && !filter(peer.get())) {
      continue;
    }
    auto intents_count = participant->TEST_CountIntents();
    if (intents_count.first) {
      result += intents_count.first;
      LOG(INFO) << Format("T $0 P $1: Intents present: $2, transactions: $3", peer->tablet_id(),
                          peer->permanent_uuid(), intents_count.first, intents_count.second);
    }
  }
  return result;
}

MiniTabletServer* FindTabletLeader(MiniCluster* cluster, const TabletId& tablet_id) {
  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    auto server = cluster->mini_tablet_server(i);
    if (!server->server()) { // Server is shut down.
      continue;
    }
    if (server->server()->LeaderAndReady(tablet_id)) {
      return server;
    }
  }

  return nullptr;
}

void ShutdownAllTServers(MiniCluster* cluster) {
  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    cluster->mini_tablet_server(i)->Shutdown();
  }
}

Status StartAllTServers(MiniCluster* cluster) {
  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    RETURN_NOT_OK(cluster->mini_tablet_server(i)->Start());
  }

  return Status::OK();
}

void ShutdownAllMasters(MiniCluster* cluster) {
  for (int i = 0; i != cluster->num_masters(); ++i) {
    cluster->mini_master(i)->Shutdown();
  }
}

Status StartAllMasters(MiniCluster* cluster) {
  for (int i = 0; i != cluster->num_masters(); ++i) {
    RETURN_NOT_OK(cluster->mini_master(i)->Start());
  }

  return Status::OK();
}

Status BreakConnectivity(MiniCluster* cluster, int idx1, int idx2) {
  for (int from_idx : {idx1, idx2}) {
    int to_idx = idx1 ^ idx2 ^ from_idx;
    for (auto type : {server::Private::kFalse, server::Private::kTrue}) {
      // TEST_RpcAddress is 1-indexed; we expect from_idx/to_idx to be 0-indexed.
      auto address = VERIFY_RESULT(HostToAddress(TEST_RpcAddress(to_idx + 1, type)));
      for (auto messenger : { cluster->mini_master(from_idx)->master()->messenger(),
                              cluster->mini_tablet_server(from_idx)->server()->messenger() }) {
        messenger->BreakConnectivityTo(address);
      }
    }
  }

  return Status::OK();
}

}  // namespace yb
