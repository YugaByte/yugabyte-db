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

#ifndef YB_INTEGRATION_TESTS_MINI_CLUSTER_H_
#define YB_INTEGRATION_TESTS_MINI_CLUSTER_H_

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "yb/gutil/macros.h"
#include "yb/integration-tests/mini_cluster_base.h"
#include "yb/master/catalog_entity_info.h"
#include "yb/server/skewed_clock.h"
#include "yb/tablet/tablet.h"
#include "yb/tserver/tablet_server_options.h"
#include "yb/util/env.h"
#include "yb/util/port_picker.h"
#include "yb/util/tsan_util.h"

namespace yb {

namespace client {
class YBClient;
class YBClientBuilder;
}

namespace master {
class MiniMaster;
class TSDescriptor;
class TabletLocationsPB;
}

namespace server {
class SkewedClockDeltaChanger;
}

namespace tablet {
class TabletPeer;
}

namespace tserver {
class MiniTabletServer;
class TSTabletManager;
}

struct MiniClusterOptions {
  MiniClusterOptions();

  MiniClusterOptions(int num_masters, int num_tablet_servers)
      : num_masters(num_masters), num_tablet_servers(num_tablet_servers) {
  }

  // Number of master servers.
  // Default: 1
  int num_masters;

  // Number of TS to start.
  // Default: 1
  int num_tablet_servers;

  // Directory in which to store data.
  // Default: "", which auto-generates a unique path for this cluster.
  // The default may only be used from a gtest unit test.
  std::string data_root;

  // Cluster id used to create fs path when we create tests with multiple clusters.
  std::string cluster_id = "";
};

// An in-process cluster with a MiniMaster and a configurable
// number of MiniTabletServers for use in tests.
class MiniCluster : public MiniClusterBase {
 public:
  typedef std::vector<std::shared_ptr<master::MiniMaster> > MiniMasters;
  typedef std::vector<std::shared_ptr<tserver::MiniTabletServer> > MiniTabletServers;
  typedef std::vector<uint16_t> Ports;
  typedef MiniClusterOptions Options;

  MiniCluster(Env* env, const MiniClusterOptions& options);
  ~MiniCluster();

  // Start a cluster with a Master and 'num_tablet_servers' TabletServers.
  // All servers run on the loopback interface with ephemeral ports.
  CHECKED_STATUS Start(
      const std::vector<tserver::TabletServerOptions>& extra_tserver_options =
      std::vector<tserver::TabletServerOptions>());

  // Like the previous method but performs initialization synchronously, i.e.
  // this will wait for all TS's to be started and initialized. Tests should
  // use this if they interact with tablets immediately after Start();
  CHECKED_STATUS StartSync();

  // Stop and restart the mini cluster synchronously. The cluster's persistent state will be kept.
  CHECKED_STATUS RestartSync();

  void Shutdown();
  CHECKED_STATUS FlushTablets(
      tablet::FlushMode mode = tablet::FlushMode::kSync,
      tablet::FlushFlags flags = tablet::FlushFlags::kAll);
  CHECKED_STATUS CompactTablets();
  CHECKED_STATUS SwitchMemtables();
  CHECKED_STATUS CleanTabletLogs();

  // Shuts down masters only.
  void ShutdownMasters();

  // Setup a consensus configuration of distributed masters, with count specified in
  // 'options'. Requires that a reserve RPC port is specified in
  // 'options' for each master.
  CHECKED_STATUS StartMasters();

  // Add a new TS to the cluster. The new TS is started.
  // Requires that the master is already running.
  CHECKED_STATUS AddTabletServer(const tserver::TabletServerOptions& extra_opts);

  // Same as above, but get options from flags.
  CHECKED_STATUS AddTabletServer();

  // If this cluster is configured for a single non-distributed
  // master, return the single master. Exits with a CHECK failure if
  // there are multiple masters.
  master::MiniMaster* mini_master() {
    CHECK_EQ(mini_masters_.size(), 1);
    return mini_master(0);
  }

  // Returns the leader Master for this MiniCluster or NULL if none can be
  // found. May block until a leader Master is ready.
  master::MiniMaster* leader_mini_master();

  int LeaderMasterIdx();

  // Returns the Master at index 'idx' for this MiniCluster.
  master::MiniMaster* mini_master(int idx);

  // Return number of mini masters.
  int num_masters() const { return mini_masters_.size(); }

  // Returns the TabletServer at index 'idx' of this MiniCluster.
  // 'idx' must be between 0 and 'num_tablet_servers' -1.
  tserver::MiniTabletServer* mini_tablet_server(int idx);

  tserver::MiniTabletServer* find_tablet_server(const std::string& uuid);

  const MiniTabletServers& mini_tablet_servers() { return mini_tablet_servers_; }

  int num_tablet_servers() const { return mini_tablet_servers_.size(); }

  const Ports& tserver_web_ports() const { return tserver_web_ports_; }

  std::string GetMasterFsRoot(int indx);

  std::string GetTabletServerFsRoot(int idx);

  // The comma separated string of the master adresses host/ports from current list of masters.
  string GetMasterAddresses() const;

  std::vector<std::shared_ptr<tablet::TabletPeer>> GetTabletPeers(int idx);

  tserver::TSTabletManager* GetTabletManager(int idx);

  // Wait for the given tablet to have 'expected_count' replicas
  // reported on the master. Returns the locations in '*locations'.
  // Requires that the master has started;
  // Returns a bad Status if the tablet does not reach the required count
  // within kTabletReportWaitTimeSeconds.
  CHECKED_STATUS WaitForReplicaCount(const std::string& tablet_id,
                             int expected_count,
                             master::TabletLocationsPB* locations);

  // Wait until the number of registered tablet servers reaches the given
  // count. Returns Status::TimedOut if the desired count is not achieved
  // within kRegistrationWaitTimeSeconds.
  CHECKED_STATUS WaitForTabletServerCount(int count);
  CHECKED_STATUS WaitForTabletServerCount(int count,
                                  std::vector<std::shared_ptr<master::TSDescriptor> >* descs);

  // Wait for all tablet servers to be registered. Returns Status::TimedOut if the desired count is
  // not achieved within kRegistrationWaitTimeSeconds.
  CHECKED_STATUS WaitForAllTabletServers();

  uint16_t AllocateFreePort() {
    return port_picker_.AllocateFreePort();
  }

 private:

  enum {
    kTabletReportWaitTimeSeconds = 5,
    kRegistrationWaitTimeSeconds = NonTsanVsTsan(30, 60)
  };

  void ConfigureClientBuilder(client::YBClientBuilder* builder) override;

  HostPort DoGetLeaderMasterBoundRpcAddr() override;

  // Allocates ports for the given daemon type and saves them to the ports vector. Does not
  // overwrite values in the ports vector that are non-zero already.
  void AllocatePortsForDaemonType(std::string daemon_type,
                                  int num_daemons,
                                  std::string port_type,
                                  std::vector<uint16_t>* ports);

  // Picks free ports for the necessary number of masters / tservers and saves those ports in
  // {master,tserver}_{rpc,web}_ports_ vectors. Values of 0 for the number of masters / tservers
  // mean we pick the maximum number of masters/tservers that we already know we'll need.
  void EnsurePortsAllocated(int new_num_masters = 0, int num_tservers = 0);

  Env* const env_ = nullptr;
  const std::string fs_root_;
  const int num_masters_initial_;
  const int num_ts_initial_;

  Ports master_rpc_ports_;
  Ports master_web_ports_;
  Ports tserver_rpc_ports_;
  Ports tserver_web_ports_;

  MiniMasters mini_masters_;
  MiniTabletServers mini_tablet_servers_;

  PortPicker port_picker_;
};

MUST_USE_RESULT std::vector<server::SkewedClockDeltaChanger> SkewClocks(
    MiniCluster* cluster, std::chrono::milliseconds clock_skew);

void StepDownAllTablets(MiniCluster* cluster);
void StepDownRandomTablet(MiniCluster* cluster);

YB_DEFINE_ENUM(ListPeersFilter, (kAll)(kLeaders)(kNonLeaders));

std::unordered_set<string> ListTabletIdsForTable(MiniCluster* cluster, const string& table_id);

std::vector<std::shared_ptr<tablet::TabletPeer>> ListTabletPeers(
    MiniCluster* cluster, ListPeersFilter filter);

std::vector<std::shared_ptr<tablet::TabletPeer>> ListTabletPeers(
    MiniCluster* cluster,
    const std::function<bool(const std::shared_ptr<tablet::TabletPeer>&)>& filter);

std::vector<tablet::TabletPeerPtr> ListTableTabletLeadersPeers(
    MiniCluster* cluster, const TableId& table_id);

std::vector<tablet::TabletPeerPtr> ListTableActiveTabletPeers(
    MiniCluster* cluster, const TableId& table_id);
std::vector<tablet::TabletPeerPtr> ListTableInactiveSplitTabletPeers(
    MiniCluster* cluster, const TableId& table_id);

CHECKED_STATUS WaitUntilTabletHasLeader(
    MiniCluster* cluster, const string& tablet_id, MonoTime deadline);

CHECKED_STATUS WaitForLeaderOfSingleTablet(
    MiniCluster* cluster, tablet::TabletPeerPtr leader, MonoDelta duration,
    const std::string& description);

YB_STRONGLY_TYPED_BOOL(ForceStepDown);

CHECKED_STATUS StepDown(
    tablet::TabletPeerPtr leader, const std::string& new_leader_uuid,
    ForceStepDown force_step_down);

// Waits until all tablet peers of the specified cluster are in the Running state.
// And total number of those peers equals to the number of tablet servers for each known tablet.
CHECKED_STATUS WaitAllReplicasReady(MiniCluster* cluster, MonoDelta timeout);

// Waits until all tablet peers of specified cluster have the specified index in their log.
// And total number of those peers equals to the number of tablet servers for each known tablet.
CHECKED_STATUS WaitAllReplicasHaveIndex(MiniCluster* cluster, int64_t index, MonoDelta timeout);

std::thread RestartsThread(
    MiniCluster* cluster, CoarseDuration interval, std::atomic<bool>* stop_flag);

std::vector<rocksdb::DB*> GetAllRocksDbs(MiniCluster* cluster, bool include_intents = true);

int NumTotalRunningCompactions(MiniCluster* cluster);

int NumRunningFlushes(MiniCluster* cluster);

Result<scoped_refptr<master::TableInfo>> FindTable(
    MiniCluster* cluster, const client::YBTableName& table_name);

CHECKED_STATUS WaitForInitDb(MiniCluster* cluster);

using TabletPeerFilter = std::function<bool(const tablet::TabletPeer*)>;
size_t CountIntents(MiniCluster* cluster, const TabletPeerFilter& filter = TabletPeerFilter());

tserver::MiniTabletServer* FindTabletLeader(MiniCluster* cluster, const TabletId& tablet_id);

void ShutdownAllTServers(MiniCluster* cluster);
CHECKED_STATUS StartAllTServers(MiniCluster* cluster);
void ShutdownAllMasters(MiniCluster* cluster);
CHECKED_STATUS StartAllMasters(MiniCluster* cluster);

CHECKED_STATUS BreakConnectivity(MiniCluster* cluster, int idx1, int idx2);

}  // namespace yb

#endif /* YB_INTEGRATION_TESTS_MINI_CLUSTER_H_ */
