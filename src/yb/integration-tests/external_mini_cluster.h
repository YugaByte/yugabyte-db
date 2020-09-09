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
#ifndef YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H_
#define YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H_

#include <sys/types.h>

#include <memory>
#include <string>
#include <vector>
#include <thread>

#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/consensus/opid_util.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/mini_cluster_base.h"
#include "yb/server/server_base.proxy.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/status.h"
#include "yb/util/env.h"

namespace yb {

class ExternalDaemon;
class ExternalMaster;
class ExternalTabletServer;
class HostPort;
class MetricPrototype;
class MetricEntityPrototype;
class NodeInstancePB;
class Subprocess;

namespace master {
class MasterServiceProxy;
}  // namespace master

namespace server {
class ServerStatusPB;
}  // namespace server

using yb::consensus::ChangeConfigType;
using yb::consensus::ConsensusServiceProxy;

struct ExternalMiniClusterOptions {
  ExternalMiniClusterOptions();
  ~ExternalMiniClusterOptions();

  // Number of masters to start.
  int num_masters = 1;

  // Number of TS to start.
  int num_tablet_servers = 1;

  // Directory in which to store data.
  // Default: "", which auto-generates a unique path for this cluster.
  std::string data_root;

  // Set data_root_counter to non-negative number if your test run need to create an new and empty
  // cluster every time ExternalMiniCluster() is constructed.
  // - During a test run, data_root will stay the same until the run is finished, so recreating a
  //   brand new cluster from scratch is not possible because the database location stays the same.
  // - When data_root_counter is non-negative, a new "data_root" is generated every time
  //   ExternalMiniCluster() is constructed.
  int data_root_counter = -1;

  // If true, binds each tablet server to a different loopback address.  This affects the server's
  // RPC server, and also forces the server to only use this IP address for outgoing socket
  // connections as well.  This allows the use of iptables on the localhost to simulate network
  // partitions.
  //
  // The addressed used are 127.<A>.<B>.<C> where:
  // - <A,B> are the high and low bytes of the pid of the process running the minicluster (not the
  //   daemon itself).
  // - <C> is the index of the server within this minicluster.
  //
  // This requires that the system is set up such that processes may bind to any IP address in the
  // localhost netblock (127.0.0.0/8). This seems to be the case on common Linux distributions. You
  // can verify by running 'ip addr | grep 127.0.0.1' and checking that the address is listed as
  // '127.0.0.1/8'.
  //
  // This option is disabled by default on OS X.
  // Enabling of this option on OS X means usage of default IPs: 127.0.0.x.
  bool bind_to_unique_loopback_addresses = true;

  // If true, second and other TSes will use the same ports as the first TS uses.
  // Else every TS will allocate unique ports for itself.
  // The option is applicable ONLY with bind_to_unique_loopback_addresses == true.
  bool use_same_ts_ports = false;

  // The path where the yb daemons should be run from.
  // Default: "../bin", which points to the path where non-test executables are located.
  // This works for unit tests, since they all end up in build/latest/test-<subproject_name>.
  std::string daemon_bin_path;

  // Extra flags for tablet servers and masters respectively.
  //
  // In these flags, you may use the special string '${index}' which will
  // be substituted with the index of the tablet server or master.
  std::vector<std::string> extra_tserver_flags;
  std::vector<std::string> extra_master_flags;

  // If more than one master is specified, list of ports for the masters in a consensus
  // configuration. Port at index 0 is used for the leader master.
  // Default: one entry as num_masters defaults to 1. Value 0 implies, a free port
  //          is picked at runtime.
  std::vector<uint16_t> master_rpc_ports = { 0 };

  // Default timeout for operations involving RPC's, when none provided in the API.
  // Default : 10sec
  MonoDelta timeout = MonoDelta::FromSeconds(10);

  static constexpr bool kDefaultEnableYsql = false;
  static constexpr bool kDefaultStartCqlProxy = true;

  bool enable_ysql = kDefaultEnableYsql;

  // If true logs will be written in both stderr and file
  bool log_to_file = false;

  // Use even IPs for cluster, like we have for MiniCluster.
  // So it could be used with test certificates.
  bool use_even_ips = false;

  // Cluster id used to create fs path when we create tests with multiple clusters.
  std::string cluster_id = "";

  CHECKED_STATUS RemovePort(const uint16_t port);
  CHECKED_STATUS AddPort(const uint16_t port);

  // Make sure we have the correct number of master RPC ports specified.
  void AdjustMasterRpcPorts();
};

// A mini-cluster made up of subprocesses running each of the daemons separately. This is useful for
// black-box or grey-box failure testing purposes -- it provides the ability to forcibly kill or
// stop particular cluster participants, which isn't feasible in the normal MiniCluster.  On the
// other hand, there is little access to inspect the internal state of the daemons.
class ExternalMiniCluster : public MiniClusterBase {
 public:
  // Mode to which node types a certain action (like Shutdown()) should apply.
  enum NodeSelectionMode {
    TS_ONLY,
    ALL
  };

  // Threshold of the number of retries for master related rpc calls.
  static const int kMaxRetryIterations = 100;

  explicit ExternalMiniCluster(const ExternalMiniClusterOptions& opts);
  ~ExternalMiniCluster();

  // Start the cluster.
  CHECKED_STATUS Start(rpc::Messenger* messenger = nullptr);

  // Restarts the cluster. Requires that it has been Shutdown() first.
  CHECKED_STATUS Restart();

  // Like the previous method but performs initialization synchronously, i.e.  this will wait for
  // all TS's to be started and initialized. Tests should use this if they interact with tablets
  // immediately after Start();
  CHECKED_STATUS StartSync();

  // Add a new TS to the cluster. The new TS is started.  Requires that the master is already
  // running.
  CHECKED_STATUS AddTabletServer(
      bool start_cql_proxy = ExternalMiniClusterOptions::kDefaultStartCqlProxy,
      const std::vector<std::string>& extra_flags = {});

  // Shuts down the whole cluster or part of it, depending on the selected 'mode'.  Currently, this
  // uses SIGKILL on each daemon for a non-graceful shutdown.
  void Shutdown(NodeSelectionMode mode = ALL);

  // Waits for the master to finishing running initdb.
  CHECKED_STATUS WaitForInitDb();

  // Return the IP address that the tablet server with the given index will bind to.  If
  // options.bind_to_unique_loopback_addresses is false, this will be 127.0.0.1 Otherwise, it is
  // another IP in the local netblock.
  std::string GetBindIpForTabletServer(int index) const;

  // Return a pointer to the running leader master. This may be NULL
  // if the cluster is not started.
  ExternalMaster* GetLeaderMaster();

  // Perform an RPC to determine the leader of the external mini cluster.  Set 'index' to the leader
  // master's index (for calls to to master() below).
  //
  // NOTE: if a leader election occurs after this method is executed, the last result may not be
  // valid.
  CHECKED_STATUS GetLeaderMasterIndex(int* idx);

  // Return a non-leader master index
  CHECKED_STATUS GetFirstNonLeaderMasterIndex(int* idx);

  // The comma separated string of the master adresses host/ports from current list of masters.
  string GetMasterAddresses() const;

  string GetTabletServerAddresses() const;

  // Start a new master with `peer_addrs` as the master_addresses parameter.
  Result<ExternalMaster *> StartMasterWithPeers(const string& peer_addrs);

  // Send a ping request to the rpc port of the master. Return OK() only if it is reachable.
  CHECKED_STATUS PingMaster(ExternalMaster* master) const;

  // Add a Tablet Server to the blacklist
  CHECKED_STATUS AddTServerToBlacklist(ExternalMaster* master, ExternalTabletServer* ts);

  // Starts a new master and returns the handle of the new master object on success.  Not thread
  // safe for now. We could move this to a static function outside External Mini Cluster, but
  // keeping it here for now as it is currently used only in conjunction with EMC.  If there are any
  // errors and if a new master could not be spawned, it will crash internally.
  void StartShellMaster(ExternalMaster** new_master);

  // Performs an add or remove from the existing config of this EMC, of the given master.
  // When use_hostport is true, the master is deemed as dead and its UUID is not used.
  CHECKED_STATUS ChangeConfig(ExternalMaster* master,
      ChangeConfigType type,
      consensus::RaftPeerPB::MemberType member_type = consensus::RaftPeerPB::PRE_VOTER,
      bool use_hostport = false);

  // Performs an RPC to the given master to get the number of masters it is tracking in-memory.
  CHECKED_STATUS GetNumMastersAsSeenBy(ExternalMaster* master, int* num_peers);

  // Get the last committed opid for the current leader master.
  CHECKED_STATUS GetLastOpIdForLeader(OpIdPB* opid);

  // The leader master sometimes does not commit the config in time on first setup, causing
  // CheckHasCommittedOpInCurrentTermUnlocked check - that the current term should have had at least
  // one commit - to fail. This API waits for the leader's commit term to move ahead by one.
  CHECKED_STATUS WaitForLeaderCommitTermAdvance();

  // This API waits for the commit indices of all the master peers to reach the target index.
  CHECKED_STATUS WaitForMastersToCommitUpTo(int target_index);

  // If this cluster is configured for a single non-distributed master, return the single master or
  // NULL if the master is not started. Exits with a CHECK failure if there are multiple masters.
  ExternalMaster* master() const {
    if (masters_.empty())
      return nullptr;

    CHECK_EQ(masters_.size(), 1)
        << "master() should not be used with multiple masters, use GetLeaderMaster() instead.";
    return master(0);
  }

  // Return master at 'idx' or NULL if the master at 'idx' has not been started.
  ExternalMaster* master(int idx) const {
    CHECK_LT(idx, masters_.size());
    return masters_[idx].get();
  }

  ExternalTabletServer* tablet_server(int idx) const {
    CHECK_LT(idx, tablet_servers_.size());
    return tablet_servers_[idx].get();
  }

  // Return ExternalTabletServer given its UUID. If not found, returns NULL.
  ExternalTabletServer* tablet_server_by_uuid(const std::string& uuid) const;

  // Return the index of the ExternalTabletServer that has the given 'uuid', or -1 if no such UUID
  // can be found.
  int tablet_server_index_by_uuid(const std::string& uuid) const;

  // Return all masters.
  std::vector<ExternalMaster*> master_daemons() const;

  // Return all tablet servers and masters.
  std::vector<ExternalDaemon*> daemons() const;

  // Return all tablet servers.
  std::vector<ExternalTabletServer*> tserver_daemons() const;

  // Get tablet server host.
  HostPort pgsql_hostport(int node_index) const;

  int num_tablet_servers() const {
    return tablet_servers_.size();
  }

  int num_masters() const {
    return masters_.size();
  }

  // Return the client messenger used by the ExternalMiniCluster.
  rpc::Messenger* messenger();

  rpc::ProxyCache& proxy_cache() {
    return *proxy_cache_;
  }

  // Get the master leader consensus proxy.
  std::shared_ptr<consensus::ConsensusServiceProxy> GetLeaderConsensusProxy();

  // Get the given master's consensus proxy.
  std::shared_ptr<consensus::ConsensusServiceProxy> GetConsensusProxy(
      scoped_refptr<ExternalMaster> master);

  // If the cluster is configured for a single non-distributed master, return a proxy to that
  // master. Requires that the single master is running.
  std::shared_ptr<master::MasterServiceProxy> master_proxy();

  // Returns an RPC proxy to the master at 'idx'. Requires that the master at 'idx' is running.
  std::shared_ptr<master::MasterServiceProxy> master_proxy(int idx);

  // Returns an generic proxy to the master at 'idx'. Requires that the master at 'idx' is running.
  std::shared_ptr<server::GenericServiceProxy> master_generic_proxy(int idx) const;
  // Same as above, using the bound rpc address.
  std::shared_ptr<server::GenericServiceProxy> master_generic_proxy(const HostPort& bound_addr)
      const;

  // Wait until the number of registered tablet servers reaches the given count on at least one of
  // the running masters.  Returns Status::TimedOut if the desired count is not achieved with the
  // given timeout.
  CHECKED_STATUS WaitForTabletServerCount(int count, const MonoDelta& timeout);

  // Runs gtest assertions that no servers have crashed.
  void AssertNoCrashes();

  // Wait until all tablets on the given tablet server are in 'RUNNING'
  // state.
  CHECKED_STATUS WaitForTabletsRunning(ExternalTabletServer* ts, const MonoDelta& timeout);

  Result<std::vector<TabletId>> GetTabletIds(ExternalTabletServer* ts);

  CHECKED_STATUS WaitForTSToCrash(const ExternalTabletServer* ts,
                          const MonoDelta& timeout = MonoDelta::FromSeconds(60));

  CHECKED_STATUS WaitForTSToCrash(int index, const MonoDelta& timeout = MonoDelta::FromSeconds(60));

  // Sets the given flag on the given daemon, which must be running.
  //
  // This uses the 'force' flag on the RPC so that, even if the flag is considered unsafe to change
  // at runtime, it is changed.
  CHECKED_STATUS SetFlag(ExternalDaemon* daemon,
                         const std::string& flag,
                         const std::string& value);

  // Sets the given flag on all tablet servers.
  CHECKED_STATUS SetFlagOnTServers(const std::string& flag, const std::string& value);

  // Allocates a free port and stores a file lock guarding access to that port into an internal
  // array of file locks.
  uint16_t AllocateFreePort();

  // Step down the master leader. error_code tracks rpc error info that can be used by the caller.
  CHECKED_STATUS StepDownMasterLeader(tserver::TabletServerErrorPB::Code* error_code);

  // Find out if the master service considers itself ready. Return status OK() implies it is ready.
  CHECKED_STATUS GetIsMasterLeaderServiceReady(ExternalMaster* master);

  // Timeout to be used for rpc operations.
  MonoDelta timeout() {
    return opts_.timeout;
  }

  // Start a leader election on this master.
  CHECKED_STATUS StartElection(ExternalMaster* master);

  bool running() const { return running_; }

  string data_root() const { return data_root_; }

 protected:
  FRIEND_TEST(MasterFailoverTest, TestKillAnyMaster);

  void ConfigureClientBuilder(client::YBClientBuilder* builder) override;

  HostPort DoGetLeaderMasterBoundRpcAddr() override;

  CHECKED_STATUS StartMasters();

  std::string GetBinaryPath(const std::string& binary) const;
  std::string GetDataPath(const std::string& daemon_id) const;

  CHECKED_STATUS DeduceBinRoot(std::string* ret);
  CHECKED_STATUS HandleOptions();

  std::string GetClusterDataDirName() const;

  // Helper function to get a leader or (random) follower index
  CHECKED_STATUS GetPeerMasterIndex(int* idx, bool is_leader);

  // API to help update the cluster state (rpc ports)
  CHECKED_STATUS AddMaster(ExternalMaster* master);
  CHECKED_STATUS RemoveMaster(ExternalMaster* master);

  // Get the index of this master in the vector of masters. This might not be the insertion order as
  // we might have removed some masters within the vector.
  int GetIndexOfMaster(ExternalMaster* master) const;

  // Checks that the masters_ list and opts_ match in terms of the number of elements.
  CHECKED_STATUS CheckPortAndMasterSizes() const;

  // Return the list of opid's for all master's in this cluster.
  CHECKED_STATUS GetLastOpIdForEachMasterPeer(
      const MonoDelta& timeout,
      consensus::OpIdType opid_type,
      std::vector<OpIdPB>* op_ids);

  // Ensure that the leader server is allowed to process a config change (by having at least one
  // commit in the current term as leader).
  CHECKED_STATUS WaitForLeaderToAllowChangeConfig(
      const string& uuid,
      ConsensusServiceProxy* leader_proxy);

  // Step down the master leader and wait for a new leader to be elected.
  CHECKED_STATUS StepDownMasterLeaderAndWaitForNewLeader();

  // Return master address for specified port.
  std::string MasterAddressForPort(uint16_t port) const;

  ExternalMiniClusterOptions opts_;

  // The root for binaries.
  std::string daemon_bin_path_;

  std::string data_root_;

  // This variable is incremented every time a new master is spawned (either in shell mode or create
  // mode). Avoids reusing an index of a killed/removed master. Useful for master side logging.
  int add_new_master_at_;

  std::vector<scoped_refptr<ExternalMaster> > masters_;
  std::vector<scoped_refptr<ExternalTabletServer> > tablet_servers_;

  rpc::Messenger* messenger_ = nullptr;
  std::unique_ptr<rpc::Messenger> messenger_holder_;
  std::unique_ptr<rpc::ProxyCache> proxy_cache_;

  std::vector<std::unique_ptr<FileLock>> free_port_file_locks_;
  std::atomic<bool> running_{false};

 private:
  DISALLOW_COPY_AND_ASSIGN(ExternalMiniCluster);
};

class ExternalDaemon : public RefCountedThreadSafe<ExternalDaemon> {
 public:
  class StringListener {
   public:
    virtual void Handle(const GStringPiece& s) = 0;
    virtual ~StringListener() {}
  };

  ExternalDaemon(
      std::string daemon_id,
      rpc::Messenger* messenger,
      rpc::ProxyCache* proxy_cache,
      std::string exe,
      std::string data_dir,
      std::string server_type,
      std::vector<std::string> extra_flags);

  HostPort bound_rpc_hostport() const;
  HostPort bound_rpc_addr() const;
  HostPort bound_http_hostport() const;
  const NodeInstancePB& instance_id() const;
  const std::string& uuid() const;

  // Return the pid of the running process.  Causes a CHECK failure if the process is not running.
  pid_t pid() const;

  const std::string& id() const { return daemon_id_; }

  // Sends a SIGSTOP signal to the daemon.
  CHECKED_STATUS Pause();

  // Sends a SIGCONT signal to the daemon.
  CHECKED_STATUS Resume();

  // Return true if we have explicitly shut down the process.
  bool IsShutdown() const;

  // Return true if the process is still running.  This may return false if the process crashed,
  // even if we didn't explicitly call Shutdown().
  bool IsProcessAlive() const;

  virtual void Shutdown();

  const std::string& GetFullDataDir() const { return full_data_dir_; }

  const std::string& exe() const { return exe_; }

  const std::string& GetDataDir() const { return data_dir_; }

  // Return a pointer to the flags used for this server on restart.  Modifying these flags will only
  // take effect on the next restart.
  std::vector<std::string>* mutable_flags() { return &extra_flags_; }

  // Retrieve the value of a given metric from this server. The metric must be of int64_t type.
  //
  // 'value_field' represents the particular field of the metric to be read.  For example, for a
  // counter or gauge, this should be 'value'. For a histogram, it might be 'total_count' or 'mean'.
  //
  // 'entity_id' may be NULL, in which case the first entity of the same type as 'entity_proto' will
  // be matched.
  Result<int64_t> GetInt64Metric(const MetricEntityPrototype* entity_proto,
                                 const char* entity_id,
                                 const MetricPrototype* metric_proto,
                                 const char* value_field) const {
    return GetInt64MetricFromHost(
        bound_http_hostport(), entity_proto, entity_id, metric_proto, value_field);
  }

  Result<int64_t> GetInt64Metric(const char* entity_proto_name,
                                 const char* entity_id,
                                 const char* metric_proto_name,
                                 const char* value_field) const {
    return GetInt64MetricFromHost(
        bound_http_hostport(), entity_proto_name, entity_id, metric_proto_name, value_field);
  }

  std::string LogPrefix();

  void SetLogListener(StringListener* listener);

  void RemoveLogListener(StringListener* listener);

  static Result<int64_t> GetInt64MetricFromHost(const HostPort& hostport,
                                                const MetricEntityPrototype* entity_proto,
                                                const char* entity_id,
                                                const MetricPrototype* metric_proto,
                                                const char* value_field);

  static Result<int64_t> GetInt64MetricFromHost(const HostPort& hostport,
                                                const char* entity_proto_name,
                                                const char* entity_id,
                                                const char* metric_proto_name,
                                                const char* value_field);

  // Get the current value of the flag for the given daemon.
  Result<std::string> GetFlag(const std::string& flag);

 protected:
  friend class RefCountedThreadSafe<ExternalDaemon>;
  virtual ~ExternalDaemon();

  CHECKED_STATUS StartProcess(const std::vector<std::string>& flags);

  virtual CHECKED_STATUS DeleteServerInfoPaths();


  virtual bool ServerInfoPathsExist();

  virtual CHECKED_STATUS BuildServerStateFromInfoPath();

  CHECKED_STATUS BuildServerStateFromInfoPath(
      const string& info_path, std::unique_ptr<server::ServerStatusPB>* server_status);

  string GetServerInfoPath();

  // In a code-coverage build, try to flush the coverage data to disk.
  // In a non-coverage build, this does nothing.
  void FlushCoverage();

  std::string ProcessNameAndPidStr();

  const std::string daemon_id_;
  rpc::Messenger* messenger_;
  rpc::ProxyCache* proxy_cache_;
  const std::string exe_;
  const std::string data_dir_;
  const std::string full_data_dir_;
  std::vector<std::string> extra_flags_;

  gscoped_ptr<Subprocess> process_;

  std::unique_ptr<server::ServerStatusPB> status_;

  // These capture the daemons parameters and running ports and
  // are used to Restart() the daemon with the same parameters.
  HostPort bound_rpc_;
  HostPort bound_http_;

 private:
  class LogTailerThread;

  std::unique_ptr<LogTailerThread> stdout_tailer_thread_, stderr_tailer_thread_;

  DISALLOW_COPY_AND_ASSIGN(ExternalDaemon);
};

// Utility class for waiting for logging events.
class LogWaiter : public ExternalDaemon::StringListener {
 public:
  LogWaiter(ExternalDaemon* daemon, const std::string& string_to_wait);

  CHECKED_STATUS WaitFor(MonoDelta timeout);
  bool IsEventOccurred() { return event_occurred_; }

  ~LogWaiter();

 private:
  void Handle(const GStringPiece& s) override;

  ExternalDaemon* daemon_;
  std::atomic<bool> event_occurred_{false};
  std::string string_to_wait_;
};

// Resumes a daemon that was stopped with ExteranlDaemon::Pause() upon
// exiting a scope.
class ScopedResumeExternalDaemon {
 public:
  // 'daemon' must remain valid for the lifetime of a
  // ScopedResumeExternalDaemon object.
  explicit ScopedResumeExternalDaemon(ExternalDaemon* daemon);

  // Resume 'daemon_'.
  ~ScopedResumeExternalDaemon();

 private:
  ExternalDaemon* daemon_;

  DISALLOW_COPY_AND_ASSIGN(ScopedResumeExternalDaemon);
};


class ExternalMaster : public ExternalDaemon {
 public:
  ExternalMaster(
    int master_index,
    rpc::Messenger* messenger,
    rpc::ProxyCache* proxy_cache,
    const std::string& exe,
    const std::string& data_dir,
    const std::vector<std::string>& extra_flags,
    const std::string& rpc_bind_address = "127.0.0.1:0",
    uint16_t http_port = 0,
    const std::string& master_addrs = "");

  CHECKED_STATUS Start(bool shell_mode = false);

  // Restarts the daemon. Requires that it has previously been shutdown.
  CHECKED_STATUS Restart();

 private:
  friend class RefCountedThreadSafe<ExternalMaster>;
  virtual ~ExternalMaster();

  // used on start to create the cluster; on restart, this should not be used!
  const std::string rpc_bind_address_;
  const std::string master_addrs_;
  const uint16_t http_port_;
};

class ExternalTabletServer : public ExternalDaemon {
 public:
  ExternalTabletServer(
      int tablet_server_index, rpc::Messenger* messenger, rpc::ProxyCache* proxy_cache,
      const std::string& exe, const std::string& data_dir, std::string bind_host, uint16_t rpc_port,
      uint16_t http_port, uint16_t redis_rpc_port, uint16_t redis_http_port,
      uint16_t cql_rpc_port, uint16_t cql_http_port,
      uint16_t pgsql_rpc_port, uint16_t pgsql_http_port,
      const std::vector<HostPort>& master_addrs,
      const std::vector<std::string>& extra_flags);

  CHECKED_STATUS Start(
      bool start_cql_proxy = ExternalMiniClusterOptions::kDefaultStartCqlProxy,
      bool set_proxy_addrs = true);

  // Restarts the daemon. Requires that it has previously been shutdown.
  CHECKED_STATUS Restart(
      bool start_cql_proxy = ExternalMiniClusterOptions::kDefaultStartCqlProxy);

  // IP addresses to bind to.
  const std::string& bind_host() const {
    return bind_host_;
  }

  // Assigned ports.
  uint16_t rpc_port() const {
    return rpc_port_;
  }
  uint16_t http_port() const {
    return http_port_;
  }

  uint16_t pgsql_rpc_port() const {
    return pgsql_rpc_port_;
  }
  uint16_t pgsql_http_port() const {
    return pgsql_http_port_;
  }

  uint16_t redis_rpc_port() const {
    return redis_rpc_port_;
  }
  uint16_t redis_http_port() const {
    return redis_http_port_;
  }

  uint16_t cql_rpc_port() const {
    return cql_rpc_port_;
  }
  uint16_t cql_http_port() const {
    return cql_http_port_;
  }

  Result<int64_t> GetInt64CQLMetric(const MetricEntityPrototype* entity_proto,
                                    const char* entity_id,
                                    const MetricPrototype* metric_proto,
                                    const char* value_field) const {
    return GetInt64MetricFromHost(
        HostPort(bind_host(), cql_http_port()),
        entity_proto, entity_id, metric_proto, value_field);
  }

 protected:
  CHECKED_STATUS DeleteServerInfoPaths() override;

  bool ServerInfoPathsExist() override;

  CHECKED_STATUS BuildServerStateFromInfoPath() override;

 private:
  string GetCQLServerInfoPath();
  const std::string master_addrs_;
  const std::string bind_host_;
  const uint16_t rpc_port_;
  const uint16_t http_port_;
  const uint16_t redis_rpc_port_;
  const uint16_t redis_http_port_;
  const uint16_t pgsql_rpc_port_;
  const uint16_t pgsql_http_port_;
  const uint16_t cql_rpc_port_;
  const uint16_t cql_http_port_;
  bool start_cql_proxy_ = true;
  std::unique_ptr<server::ServerStatusPB> cqlserver_status_;

  friend class RefCountedThreadSafe<ExternalTabletServer>;
  virtual ~ExternalTabletServer();
};

// Custom functor for predicate based comparison with the master list.
struct MasterComparator {
  explicit MasterComparator(ExternalMaster* master) : master_(master) { }

  // We look for the exact master match. Since it is possible to stop/restart master on a given
  // host/port, we do not want a stale master pointer input to match a newer master.
  bool operator()(const scoped_refptr<ExternalMaster>& other) const {
    return master_ == other.get();
  }

 private:
  const ExternalMaster* master_;
};

}  // namespace yb
#endif  // YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H_
