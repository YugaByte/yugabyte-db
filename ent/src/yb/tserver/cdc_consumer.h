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

#ifndef ENT_SRC_YB_TSERVER_CDC_CONSUMER_H
#define ENT_SRC_YB_TSERVER_CDC_CONSUMER_H

#include <unordered_map>
#include <unordered_set>

#include "yb/cdc/cdc_util.h"
#include "yb/util/locks.h"

namespace yb {

class Thread;
class ThreadPool;

namespace rpc {

class Messenger;
class ProxyCache;
class Rpcs;
class SecureContext;

} // namespace rpc

namespace cdc {

class ConsumerRegistryPB;

} // namespace cdc

namespace client {

class YBClient;

} // namespace client

namespace tserver {
namespace enterprise {

class CDCPoller;
class TabletServer;

struct CDCClient {
  std::unique_ptr<rpc::Messenger> messenger;
  std::unique_ptr<rpc::SecureContext> secure_context;
  std::shared_ptr<client::YBClient> client;

  ~CDCClient();
  void Shutdown();
};

class CDCConsumer {
 public:
  static Result<std::unique_ptr<CDCConsumer>> Create(
      std::function<bool(const std::string&)> is_leader_for_tablet,
      rpc::ProxyCache* proxy_cache,
      TabletServer* tserver);

  CDCConsumer(std::function<bool(const std::string&)> is_leader_for_tablet,
      rpc::ProxyCache* proxy_cache,
      const std::string& ts_uuid,
      std::unique_ptr<CDCClient> local_client);

  ~CDCConsumer();
  void Shutdown();

  // Refreshes the in memory state when we receive a new registry from master.
  void RefreshWithNewRegistryFromMaster(const cdc::ConsumerRegistryPB* consumer_registry,
                                        int32_t cluster_config_version);

  std::vector<std::string> TEST_producer_tablets_running();

  std::string LogPrefix();

  // Return the value stored in cluster_config_version_. Since we are reading an atomic variable,
  // we don't need to hold the mutex.
  int32_t cluster_config_version() const NO_THREAD_SAFETY_ANALYSIS;

  void IncrementNumSuccessfulWriteRpcs() {
    TEST_num_successful_write_rpcs++;
  }

  uint32_t GetNumSuccessfulWriteRpcs() {
    return TEST_num_successful_write_rpcs.load(std::memory_order_acquire);
  }

 private:
  // Runs a thread that periodically polls for any new threads.
  void RunThread();

  // Loops through all the entries in the registry and creates a producer -> consumer tablet
  // mapping.
  void UpdateInMemoryState(const cdc::ConsumerRegistryPB* consumer_producer_map,
                           int32_t cluster_config_version);

  // Loops through all entries in registry from master to check if all producer tablets are being
  // polled for.
  void TriggerPollForNewTablets();

  bool ShouldContinuePolling(const cdc::ProducerTabletInfo producer_tablet_info);

  void RemoveFromPollersMap(const cdc::ProducerTabletInfo producer_tablet_info);

  // Mutex and cond for should_run_ state.
  std::mutex should_run_mutex_;
  std::condition_variable cond_;

  // Mutex for producer_consumer_tablet_map_from_master_.
  rw_spinlock master_data_mutex_ ACQUIRED_AFTER(should_run_mutex_);

  // Mutex for producer_pollers_map_.
  rw_spinlock producer_pollers_map_mutex_ ACQUIRED_AFTER(should_run_mutex_, master_data_mutex_);

  std::function<bool(const std::string&)> is_leader_for_tablet_;

  std::unordered_map<cdc::ProducerTabletInfo, cdc::ConsumerTabletInfo,
                     cdc::ProducerTabletInfo::Hash> producer_consumer_tablet_map_from_master_
                     GUARDED_BY(master_data_mutex_);

  std::unordered_set<std::string> streams_with_same_num_producer_consumer_tablets_
    GUARDED_BY(master_data_mutex_);

  scoped_refptr<Thread> run_trigger_poll_thread_;

  std::unordered_map<cdc::ProducerTabletInfo, std::shared_ptr<CDCPoller>,
                     cdc::ProducerTabletInfo::Hash> producer_pollers_map_
                     GUARDED_BY(producer_pollers_map_mutex_);

  std::unique_ptr<ThreadPool> thread_pool_;
  std::unique_ptr<rpc::Rpcs> rpcs_;

  std::string log_prefix_;
  std::shared_ptr<CDCClient> local_client_;

  // map: {universe_uuid : ...}.
  std::unordered_map<std::string, std::shared_ptr<CDCClient>> remote_clients_
    GUARDED_BY(producer_pollers_map_mutex_);
  std::unordered_map<std::string, std::string> uuid_master_addrs_
    GUARDED_BY(master_data_mutex_);
  std::unordered_set<std::string> changed_master_addrs_ GUARDED_BY(master_data_mutex_);

  bool should_run_ = true;

  std::atomic<int32_t> cluster_config_version_ GUARDED_BY(master_data_mutex_) = {-1};

  std::atomic<uint32_t> TEST_num_successful_write_rpcs {0};
};

} // namespace enterprise
} // namespace tserver
} // namespace yb

#endif // ENT_SRC_YB_TSERVER_CDC_CONSUMER_H
