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
#ifndef YB_TSERVER_TABLET_SERVER_H_
#define YB_TSERVER_TABLET_SERVER_H_

#include <memory>
#include <string>
#include <vector>

#include "yb/consensus/metadata.pb.h"
#include "yb/gutil/atomicops.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/master/master.h"
#include "yb/server/server_base.h"
#include "yb/server/webserver_options.h"
#include "yb/tserver/tserver_shared_mem.h"
#include "yb/tserver/tablet_server_interface.h"
#include "yb/tserver/tablet_server_options.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/status.h"
#include "yb/tserver/tablet_service.h"
#include "yb/master/master.pb.h"

namespace rocksdb {
class Env;
}

namespace yb {
class Env;
class MaintenanceManager;

namespace tserver {

constexpr const char* const kTcMallocMaxThreadCacheBytes = "tcmalloc.max_total_thread_cache_bytes";

class Heartbeater;
class MetricsSnapshotter;
class TabletServerPathHandlers;
class TSTabletManager;

class TabletServer : public server::RpcAndWebServerBase, public TabletServerIf {
 public:
  // TODO: move this out of this header, since clients want to use this
  // constant as well.
  static const uint16_t kDefaultPort = 9100;
  static const uint16_t kDefaultWebPort = 9000;

  // Default tserver and consensus RPC queue length per service.
  static constexpr uint32_t kDefaultSvcQueueLength = 5000;

  explicit TabletServer(const TabletServerOptions& opts);
  ~TabletServer();

  // Initializes the tablet server, including the bootstrapping of all
  // existing tablets.
  // Some initialization tasks are asynchronous, such as the bootstrapping
  // of tablets. Caller can block, waiting for the initialization to fully
  // complete by calling WaitInited().
  CHECKED_STATUS Init();

  // Waits for the tablet server to complete the initialization.
  CHECKED_STATUS WaitInited();

  CHECKED_STATUS Start();
  virtual void Shutdown();

  std::string ToString() const override;

  TSTabletManager* tablet_manager() override { return tablet_manager_.get(); }
  TabletPeerLookupIf* tablet_peer_lookup() override;

  Heartbeater* heartbeater() { return heartbeater_.get(); }

  MetricsSnapshotter* metrics_snapshotter() { return metrics_snapshotter_.get(); }

  void set_fail_heartbeats_for_tests(bool fail_heartbeats_for_tests) {
    base::subtle::NoBarrier_Store(&fail_heartbeats_for_tests_, fail_heartbeats_for_tests);
  }

  bool fail_heartbeats_for_tests() const {
    return base::subtle::NoBarrier_Load(&fail_heartbeats_for_tests_);
  }

  MaintenanceManager* maintenance_manager() {
    return maintenance_manager_.get();
  }

  int GetCurrentMasterIndex() { return master_config_index_; }

  void SetCurrentMasterIndex(int index) { master_config_index_ = index; }

  // Update in-memory list of master addresses that this tablet server pings to.
  // If the update is from master leader, we use that list directly. If not, we
  // merge the existing in-memory master list with the provided config list.
  CHECKED_STATUS UpdateMasterAddresses(const consensus::RaftConfigPB& new_config,
                                       bool is_master_leader);

  server::Clock* Clock() override { return clock(); }

  void SetClockForTests(server::ClockPtr clock) { clock_ = std::move(clock); }

  const scoped_refptr<MetricEntity>& MetricEnt() const override { return metric_entity(); }

  CHECKED_STATUS PopulateLiveTServers(const master::TSHeartbeatResponsePB& heartbeat_resp);

  CHECKED_STATUS GetLiveTServers(
      std::vector<master::TSInformationPB> *live_tservers) const {
    std::lock_guard<simple_spinlock> l(lock_);
    *live_tservers = live_tservers_;
    return Status::OK();
  }

  CHECKED_STATUS GetTabletStatus(const GetTabletStatusRequestPB* req,
                                 GetTabletStatusResponsePB* resp) const override;

  bool LeaderAndReady(const TabletId& tablet_id, bool allow_stale = false) const override;

  const std::string& permanent_uuid() const { return fs_manager_->uuid(); }

  // Returns the proxy to call this tablet server locally.
  const std::shared_ptr<TabletServerServiceProxy>& proxy() const { return proxy_; }

  const TabletServerOptions& options() const { return opts_; }

  void set_cluster_uuid(const std::string& cluster_uuid);

  std::string cluster_uuid() const;

  TabletServiceImpl* tablet_server_service();

  scoped_refptr<Histogram> GetMetricsHistogram(TabletServerServiceIf::RpcMetricIndexes metric);

  void SetPublisher(rpc::Publisher service) {
    publish_service_ptr_.reset(new rpc::Publisher(std::move(service)));
  }

  rpc::Publisher* GetPublisher() override {
    return publish_service_ptr_.get();
  }

  void SetYSQLCatalogVersion(uint64_t new_version, uint64_t new_breaking_version);

  void get_ysql_catalog_version(uint64_t* current_version,
                                uint64_t* last_breaking_version) const override {
    std::lock_guard<simple_spinlock> l(lock_);
    if (current_version) {
      *current_version = ysql_catalog_version_;
    }
    if (last_breaking_version) {
      *last_breaking_version = ysql_last_breaking_catalog_version_;
    }
  }

  virtual Env* GetEnv();

  virtual rocksdb::Env* GetRocksDBEnv();

  virtual CHECKED_STATUS SetUniverseKeyRegistry(
      const yb::UniverseKeyRegistryPB& universe_key_registry);

  // Returns the file descriptor of this tablet server's shared memory segment.
  int GetSharedMemoryFd();

  uint64_t GetSharedMemoryPostgresAuthKey();

  // Currently only used by cdc.
  virtual int32_t cluster_config_version() const {
    return std::numeric_limits<int32_t>::max();
  }

  client::TransactionPool* TransactionPool() override;

  client::YBClient* client() override;

  const std::string& LogPrefix() const {
    return log_prefix_;
  }

  const HostPort pgsql_proxy_bind_address() const { return pgsql_proxy_bind_address_; }

 protected:
  virtual CHECKED_STATUS RegisterServices();

  friend class TabletServerTestBase;

  CHECKED_STATUS DisplayRpcIcons(std::stringstream* output) override;

  CHECKED_STATUS ValidateMasterAddressResolution() const;

  std::atomic<bool> initted_{false};

  // If true, all heartbeats will be seen as failed.
  Atomic32 fail_heartbeats_for_tests_;

  // The options passed at construction time, and will be updated if master config changes.
  TabletServerOptions opts_;

  // Manager for tablets which are available on this server.
  gscoped_ptr<TSTabletManager> tablet_manager_;

  // Used to forward redis pub/sub messages to the redis pub/sub handler
  yb::AtomicUniquePtr<rpc::Publisher> publish_service_ptr_;

  // Thread responsible for heartbeating to the master.
  std::unique_ptr<Heartbeater> heartbeater_;

  // Thread responsible for collecting metrics snapshots for native storage.
  gscoped_ptr<MetricsSnapshotter> metrics_snapshotter_;

  // Webserver path handlers
  gscoped_ptr<TabletServerPathHandlers> path_handlers_;

  // The maintenance manager for this tablet server
  std::shared_ptr<MaintenanceManager> maintenance_manager_;

  // Index at which master sent us the last config
  int master_config_index_;

  // List of tservers that are alive from the master's perspective.
  std::vector<master::TSInformationPB> live_tservers_;

  // Lock to protect live_tservers_, cluster_uuid_.
  mutable simple_spinlock lock_;

  // Proxy to call this tablet server locally.
  std::shared_ptr<TabletServerServiceProxy> proxy_;

  // Cluster uuid. This is sent by the master leader during the first heartbeat.
  std::string cluster_uuid_;

  // Latest known version from the YSQL catalog (as reported by last heartbeat response).
  uint64_t ysql_catalog_version_ = 0;
  uint64_t ysql_last_breaking_catalog_version_ = 0;

  // An instance to tablet server service. This pointer is no longer valid after RpcAndWebServerBase
  // is shut down.
  TabletServiceImpl* tablet_server_service_;

 private:
  // Auto initialize some of the service flags that are defaulted to -1.
  void AutoInitServiceFlags();

  // Shared memory owned by the tablet server.
  TServerSharedObject shared_object_;

  std::atomic<client::TransactionPool*> transaction_pool_{nullptr};
  std::mutex transaction_pool_mutex_;
  std::unique_ptr<client::TransactionManager> transaction_manager_holder_;
  std::unique_ptr<client::TransactionPool> transaction_pool_holder_;

  std::string log_prefix_;

  // Bind address of postgres proxy under this tserver.
  HostPort pgsql_proxy_bind_address_;

  DISALLOW_COPY_AND_ASSIGN(TabletServer);
};

} // namespace tserver
} // namespace yb
#endif // YB_TSERVER_TABLET_SERVER_H_
