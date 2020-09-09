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

#include "yb/client/async_initializer.h"

#include "yb/common/wire_protocol.h"

#include "yb/gutil/strings/join.h"

#include "yb/util/flag_tags.h"

using namespace std::literals;

DEFINE_test_flag(bool, force_master_leader_resolution, false,
                 "Force master leader resolution even only one master is set.");

namespace yb {
namespace client {

AsyncClientInitialiser::AsyncClientInitialiser(
    const std::string& client_name, const uint32_t num_reactors, const uint32_t timeout_seconds,
    const std::string& tserver_uuid, const yb::server::ServerBaseOptions* opts,
    scoped_refptr<MetricEntity> metric_entity,
    const std::shared_ptr<MemTracker>& parent_mem_tracker,
    rpc::Messenger* messenger)
    : messenger_(messenger), client_future_(client_promise_.get_future()) {
  client_builder_.set_client_name(client_name);
  client_builder_.default_rpc_timeout(MonoDelta::FromSeconds(timeout_seconds));
  // Client does not care about master replication factor, it only needs endpoint of master leader.
  // So we put all known master addresses to speed up leader resolution.
  std::vector<std::string> master_addresses;
  for (const auto& list : *opts->GetMasterAddresses()) {
    for (const auto& hp : list) {
      master_addresses.push_back(hp.ToString());
    }
  }
  VLOG(4) << "Master addresses for " << client_name << ": " << AsString(master_addresses);
  client_builder_.add_master_server_addr(JoinStrings(master_addresses, ","));
  client_builder_.set_skip_master_leader_resolution(
      master_addresses.size() == 1 && !FLAGS_TEST_force_master_leader_resolution);
  client_builder_.set_metric_entity(metric_entity);
  if (num_reactors > 0) {
    client_builder_.set_num_reactors(num_reactors);
  }
  client_builder_.set_parent_mem_tracker(parent_mem_tracker);

  // Build cloud_info_pb.
  client_builder_.set_cloud_info_pb(opts->MakeCloudInfoPB());

  if (!tserver_uuid.empty()) {
    client_builder_.set_tserver_uuid(tserver_uuid);
  }
}

AsyncClientInitialiser::~AsyncClientInitialiser() {
  Shutdown();
  if (init_client_thread_.joinable()) {
    init_client_thread_.join();
  }
}

void AsyncClientInitialiser::Start() {
  init_client_thread_ = std::thread(std::bind(&AsyncClientInitialiser::InitClient, this));
}

YBClient* AsyncClientInitialiser::client() const {
  return client_future_.get();
}

void AsyncClientInitialiser::InitClient() {
  CDSAttacher attacher;

  LOG(INFO) << "Starting to init ybclient";
  while (!stopping_) {
    auto result = client_builder_.Build(messenger_);
    if (result.ok()) {
      LOG(INFO) << "Successfully built ybclient";
      client_holder_.reset(result->release());
      for (const auto& functor : post_create_hooks_) {
        functor(client_holder_.get());
      }
      client_promise_.set_value(client_holder_.get());
      return;
    }

    LOG(ERROR) << "Failed to initialize client: " << result.status();
    if (result.status().IsAborted()) {
      break;
    }
    std::this_thread::sleep_for(1s);
  }

  client_promise_.set_value(nullptr);
}

}  // namespace client
}  // namespace yb
