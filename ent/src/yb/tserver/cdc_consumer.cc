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

#include <shared_mutex>
#include <chrono>

#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc.h"
#include "yb/rpc/secure_stream.h"
#include "yb/tserver/cdc_consumer.h"
#include "yb/tserver/twodc_output_client.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/cdc_poller.h"

#include "yb/cdc/cdc_consumer.pb.h"

#include "yb/client/client.h"

#include "yb/gutil/map-util.h"
#include "yb/server/secure.h"
#include "yb/util/shared_lock.h"
#include "yb/util/string_util.h"
#include "yb/util/thread.h"

DECLARE_int32(cdc_read_rpc_timeout_ms);
DECLARE_int32(cdc_write_rpc_timeout_ms);
DECLARE_bool(use_node_to_node_encryption);
DECLARE_string(certs_for_cdc_dir);

using namespace std::chrono_literals;

namespace yb {

namespace tserver {
namespace enterprise {

CDCClient::~CDCClient() {
  if (messenger) {
    messenger->Shutdown();
  }
}

void CDCClient::Shutdown() {
  client->Shutdown();
}

Result<std::unique_ptr<CDCConsumer>> CDCConsumer::Create(
    std::function<bool(const std::string&)> is_leader_for_tablet,
    rpc::ProxyCache* proxy_cache,
    TabletServer* tserver) {
  LOG(INFO) << "Creating CDC Consumer";
  auto master_addrs = tserver->options().GetMasterAddresses();
  std::vector<std::string> hostport_strs;
  hostport_strs.reserve(master_addrs->size());
  for (const auto& hp : *master_addrs) {
    hostport_strs.push_back(HostPort::ToCommaSeparatedString(hp));
  }

  auto local_client = std::make_unique<CDCClient>();
  if (FLAGS_use_node_to_node_encryption) {
    rpc::MessengerBuilder messenger_builder("cdc-consumer");

    local_client->secure_context = VERIFY_RESULT(server::SetupSecureContext(
        "", "", server::SecureContextType::kServerToServer, &messenger_builder));

    local_client->messenger = VERIFY_RESULT(messenger_builder.Build());
  }

  local_client->client = VERIFY_RESULT(client::YBClientBuilder()
      .master_server_addrs(hostport_strs)
      .set_client_name("CDCConsumerLocal")
      .default_rpc_timeout(MonoDelta::FromMilliseconds(FLAGS_cdc_write_rpc_timeout_ms))
      .Build(local_client->messenger.get()));

  local_client->client->SetLocalTabletServer(tserver->permanent_uuid(), tserver->proxy(), tserver);
  auto cdc_consumer = std::make_unique<CDCConsumer>(std::move(is_leader_for_tablet), proxy_cache,
      tserver->permanent_uuid(), std::move(local_client));

  // TODO(NIC): Unify cdc_consumer thread_pool & remote_client_ threadpools
  RETURN_NOT_OK(yb::Thread::Create(
      "CDCConsumer", "Poll", &CDCConsumer::RunThread, cdc_consumer.get(),
      &cdc_consumer->run_trigger_poll_thread_));
  RETURN_NOT_OK(ThreadPoolBuilder("CDCConsumerHandler").Build(&cdc_consumer->thread_pool_));
  return cdc_consumer;
}

CDCConsumer::CDCConsumer(std::function<bool(const std::string&)> is_leader_for_tablet,
                         rpc::ProxyCache* proxy_cache,
                         const string& ts_uuid,
                         std::unique_ptr<CDCClient> local_client) :
  is_leader_for_tablet_(std::move(is_leader_for_tablet)),
  rpcs_(new rpc::Rpcs),
  log_prefix_(Format("[TS $0]: ", ts_uuid)),
  local_client_(std::move(local_client)) {}

CDCConsumer::~CDCConsumer() {
  Shutdown();
}

void CDCConsumer::Shutdown() {
  LOG_WITH_PREFIX(INFO) << "Shutting down CDC Consumer";
  {
    std::lock_guard<std::mutex> l(should_run_mutex_);
    should_run_ = false;
  }
  cond_.notify_all();

  if (thread_pool_) {
    thread_pool_->Shutdown();
  }

  {
    std::lock_guard<rw_spinlock> write_lock(master_data_mutex_);
    producer_consumer_tablet_map_from_master_.clear();
    uuid_master_addrs_.clear();
    {
      SharedLock<rw_spinlock> read_lock(producer_pollers_map_mutex_);
      for (auto &uuid_and_client : remote_clients_) {
        uuid_and_client.second->Shutdown();
      }
      producer_pollers_map_.clear();
    }
    local_client_->client->Shutdown();
  }

  if (run_trigger_poll_thread_) {
    WARN_NOT_OK(ThreadJoiner(run_trigger_poll_thread_.get()).Join(), "Could not join thread");
  }
}

void CDCConsumer::RunThread() {
  while (true) {
    std::unique_lock<std::mutex> l(should_run_mutex_);
    if (!should_run_) {
      return;
    }
    cond_.wait_for(l, 1000ms);
    if (!should_run_) {
      return;
    }
    TriggerPollForNewTablets();
  }
}

void CDCConsumer::RefreshWithNewRegistryFromMaster(const cdc::ConsumerRegistryPB* consumer_registry,
                                                   int32_t cluster_config_version) {
  UpdateInMemoryState(consumer_registry, cluster_config_version);
  cond_.notify_all();
}

std::vector<std::string> CDCConsumer::TEST_producer_tablets_running() {
  SharedLock<rw_spinlock> read_lock(producer_pollers_map_mutex_);

  std::vector<string> tablets;
  for (const auto& producer : producer_pollers_map_) {
    tablets.push_back(producer.first.tablet_id);
  }
  return tablets;
}

// NOTE: This happens on TS.heartbeat, so it needs to finish quickly
void CDCConsumer::UpdateInMemoryState(const cdc::ConsumerRegistryPB* consumer_registry,
    int32_t cluster_config_version) {
  std::lock_guard<rw_spinlock> write_lock_master(master_data_mutex_);

  // Only update it if the version is newer.
  if (cluster_config_version <= cluster_config_version_.load(std::memory_order_acquire)) {
    return;
  }

  cluster_config_version_.store(cluster_config_version, std::memory_order_release);
  producer_consumer_tablet_map_from_master_.clear();
  decltype(uuid_master_addrs_) old_uuid_master_addrs;
  uuid_master_addrs_.swap(old_uuid_master_addrs);

  if (!consumer_registry) {
    LOG_WITH_PREFIX(INFO) << "Given empty CDC consumer registry: removing Pollers";
    cond_.notify_all();
    return;
  }

  LOG_WITH_PREFIX(INFO) << "Updating CDC consumer registry: " << consumer_registry->DebugString();

  for (const auto& producer_map : DCHECK_NOTNULL(consumer_registry)->producer_map()) {
    const auto& producer_entry_pb = producer_map.second;
    if (producer_entry_pb.disable_stream()) {
      continue;
    }
    // recreate the UUID connection information
    if (!ContainsKey(uuid_master_addrs_, producer_map.first)) {
      std::vector<HostPort> hp;
      HostPortsFromPBs(producer_map.second.master_addrs(), &hp);
      uuid_master_addrs_[producer_map.first] = HostPort::ToCommaSeparatedString(hp);

      // If master addresses changed, mark for YBClient update.
      if (ContainsKey(old_uuid_master_addrs, producer_map.first) &&
          uuid_master_addrs_[producer_map.first] != old_uuid_master_addrs[producer_map.first]) {
        changed_master_addrs_.insert(producer_map.first);
      }
    }
    // recreate the set of CDCPollers
    for (const auto& stream_entry : producer_entry_pb.stream_map()) {
      const auto& stream_entry_pb = stream_entry.second;
      if (stream_entry_pb.same_num_producer_consumer_tablets()) {
        LOG_WITH_PREFIX(INFO) << Format("Stream $0 will use local tserver optimization",
                                        stream_entry.first);
        streams_with_same_num_producer_consumer_tablets_.insert(stream_entry.first);
      }
      for (const auto& tablet_entry : stream_entry_pb.consumer_producer_tablet_map()) {
        const auto& consumer_tablet_id = tablet_entry.first;
        for (const auto& producer_tablet_id : tablet_entry.second.tablets()) {
          cdc::ProducerTabletInfo producer_tablet_info(
              {producer_map.first, stream_entry.first, producer_tablet_id});
          cdc::ConsumerTabletInfo consumer_tablet_info(
              {consumer_tablet_id, stream_entry_pb.consumer_table_id()});
          producer_consumer_tablet_map_from_master_[producer_tablet_info] = consumer_tablet_info;
        }
      }
    }
  }
  cond_.notify_all();
}

void CDCConsumer::TriggerPollForNewTablets() {
  std::lock_guard<rw_spinlock> write_lock_master(master_data_mutex_);

  for (const auto& entry : producer_consumer_tablet_map_from_master_) {
    auto uuid = entry.first.universe_uuid;
    bool start_polling;
    {
      SharedLock<rw_spinlock> read_lock_pollers(producer_pollers_map_mutex_);
      start_polling = producer_pollers_map_.find(entry.first) == producer_pollers_map_.end() &&
                      is_leader_for_tablet_(entry.second.tablet_id);

      // Update the Master Addresses, if altered after setup.
      if (ContainsKey(remote_clients_, uuid) && changed_master_addrs_.count(uuid) > 0) {
        auto status = remote_clients_[uuid]->client->SetMasterAddresses(uuid_master_addrs_[uuid]);
        if (status.ok()) {
          changed_master_addrs_.erase(uuid);
        } else {
          LOG_WITH_PREFIX(WARNING) << "Problem Setting Master Addresses for " << uuid
                                   << ": " << status.ToString();
        }
      }
    }
    if (start_polling) {
      std::lock_guard <rw_spinlock> write_lock_pollers(producer_pollers_map_mutex_);

      // Check again, since we unlocked.
      start_polling = producer_pollers_map_.find(entry.first) == producer_pollers_map_.end() &&
          is_leader_for_tablet_(entry.second.tablet_id);
      if (start_polling) {
        // This is a new tablet, trigger a poll.
        // See if we need to create a new client connection
        if (!ContainsKey(remote_clients_, uuid)) {
          CHECK(ContainsKey(uuid_master_addrs_, uuid));

          auto remote_client = std::make_unique<CDCClient>();
          std::string dir;
          if (FLAGS_use_node_to_node_encryption) {
            rpc::MessengerBuilder messenger_builder("cdc-consumer");
            if (!FLAGS_certs_for_cdc_dir.empty()) {
              dir = JoinPathSegments(FLAGS_certs_for_cdc_dir, uuid);
            }

            auto secure_context_result = server::SetupSecureContext(
                dir, "", "", server::SecureContextType::kServerToServer, &messenger_builder);
            if (!secure_context_result.ok()) {
              LOG(WARNING) << "Could not create secure context for " << uuid
                         << ": " << secure_context_result.status().ToString();
              return; // Don't finish creation.  Try again on the next heartbeat.
            }
            remote_client->secure_context = std::move(*secure_context_result);

            auto messenger_result = messenger_builder.Build();
            if (!messenger_result.ok()) {
              LOG(WARNING) << "Could not build messenger for " << uuid
                         << ": " << secure_context_result.status().ToString();
              return; // Don't finish creation.  Try again on the next heartbeat.
            }
            remote_client->messenger = std::move(*messenger_result);
          }

          auto client_result = yb::client::YBClientBuilder()
              .set_client_name("CDCConsumerRemote")
              .add_master_server_addr(uuid_master_addrs_[uuid])
              .skip_master_flagfile()
              .default_rpc_timeout(MonoDelta::FromMilliseconds(FLAGS_cdc_read_rpc_timeout_ms))
              .Build(remote_client->messenger.get());
          if (!client_result.ok()) {
            LOG(WARNING) << "Could not create a new YBClient for " << uuid
                         << ": " << client_result.status().ToString();
            return; // Don't finish creation.  Try again on the next heartbeat.
          }

          remote_client->client = CHECK_RESULT(client_result);
          remote_clients_[uuid] = std::move(remote_client);
        }

        // now create the poller
        bool use_local_tserver =
            streams_with_same_num_producer_consumer_tablets_.find(entry.first.stream_id) !=
            streams_with_same_num_producer_consumer_tablets_.end();
        auto cdc_poller = std::make_shared<CDCPoller>(
            entry.first, entry.second,
            std::bind(&CDCConsumer::ShouldContinuePolling, this, entry.first),
            std::bind(&CDCConsumer::RemoveFromPollersMap, this, entry.first),
            thread_pool_.get(),
            rpcs_.get(),
            local_client_,
            remote_clients_[uuid],
            this,
            use_local_tserver);
        LOG_WITH_PREFIX(INFO) << Format("Start polling for producer tablet $0",
            entry.first.tablet_id);
        producer_pollers_map_[entry.first] = cdc_poller;
        cdc_poller->Poll();
      }
    }
  }
}

void CDCConsumer::RemoveFromPollersMap(const cdc::ProducerTabletInfo producer_tablet_info) {
  LOG_WITH_PREFIX(INFO) << Format("Stop polling for producer tablet $0",
                                  producer_tablet_info.tablet_id);
  std::shared_ptr<CDCClient> client_to_delete; // decrement refcount to 0 outside lock
  {
    SharedLock<rw_spinlock> read_lock_master(master_data_mutex_);
    std::lock_guard<rw_spinlock> write_lock_pollers(producer_pollers_map_mutex_);
    producer_pollers_map_.erase(producer_tablet_info);
    // Check if no more objects with this UUID exist after registry refresh.
    if (!ContainsKey(uuid_master_addrs_, producer_tablet_info.universe_uuid)) {
      auto it = remote_clients_.find(producer_tablet_info.universe_uuid);
      if (it != remote_clients_.end()) {
        client_to_delete = it->second;
        remote_clients_.erase(it);
      }
    }
  }
  if (client_to_delete != nullptr) {
    client_to_delete->Shutdown();
  }
}

bool CDCConsumer::ShouldContinuePolling(const cdc::ProducerTabletInfo producer_tablet_info) {
  std::lock_guard<std::mutex> l(should_run_mutex_);
  if (!should_run_) {
    return false;
  }

  SharedLock<rw_spinlock> read_lock_master(master_data_mutex_);

  const auto& it = producer_consumer_tablet_map_from_master_.find(producer_tablet_info);
  if (it == producer_consumer_tablet_map_from_master_.end()) {
    // We no longer care about this tablet, abort the cycle.
    return false;
  }
  return is_leader_for_tablet_(it->second.tablet_id);
}

std::string CDCConsumer::LogPrefix() {
  return log_prefix_;
}

int32_t CDCConsumer::cluster_config_version() const {
  return cluster_config_version_.load(std::memory_order_acquire);
}

} // namespace enterprise
} // namespace tserver
} // namespace yb
