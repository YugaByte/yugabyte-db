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

#include "yb/integration-tests/twodc_test_base.h"

#include <string>

#include "yb/cdc/cdc_service.h"
#include "yb/client/client.h"

#include "yb/integration-tests/cdc_test_util.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/mini_master.h"
#include "yb/tserver/cdc_consumer.h"
#include "yb/tserver/tablet_server.h"
#include "yb/util/test_util.h"
#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"

namespace yb {

using client::YBClient;
using tserver::enterprise::CDCConsumer;

namespace enterprise {

void TwoDCTestBase::Destroy() {
  LOG(INFO) << "Destroying CDC Clusters";
  if (consumer_cluster()) {
    if (consumer_cluster_.pg_supervisor_) {
      consumer_cluster_.pg_supervisor_->Stop();
    }
    consumer_cluster_.mini_cluster_->Shutdown();
    consumer_cluster_.mini_cluster_.reset();
  }

  if (producer_cluster()) {
    if (producer_cluster_.pg_supervisor_) {
      producer_cluster_.pg_supervisor_->Stop();
    }
    producer_cluster_.mini_cluster_->Shutdown();
    producer_cluster_.mini_cluster_.reset();
  }

  producer_cluster_.client_.reset();
  producer_cluster_.client_.reset();
}

Status TwoDCTestBase::SetupUniverseReplication(
    MiniCluster* producer_cluster, MiniCluster* consumer_cluster, YBClient* consumer_client,
    const std::string& universe_id, const std::vector<std::shared_ptr<client::YBTable>>& tables,
    bool leader_only) {
  master::SetupUniverseReplicationRequestPB req;
  master::SetupUniverseReplicationResponsePB resp;

  req.set_producer_id(universe_id);
  string master_addr = producer_cluster->GetMasterAddresses();
  if (leader_only) master_addr = producer_cluster->leader_mini_master()->bound_rpc_addr_str();
  auto hp_vec = VERIFY_RESULT(HostPort::ParseStrings(master_addr, 0));
  HostPortsToPBs(hp_vec, req.mutable_producer_master_addresses());

  req.mutable_producer_table_ids()->Reserve(tables.size());
  for (const auto& table : tables) {
    req.add_producer_table_ids(table->id());
  }

  auto master_proxy = std::make_shared<master::MasterServiceProxy>(
      &consumer_client->proxy_cache(),
      consumer_cluster->leader_mini_master()->bound_rpc_addr());

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
  RETURN_NOT_OK(master_proxy->SetupUniverseReplication(req, &resp, &rpc));
  if (resp.has_error()) {
    return STATUS(IllegalState, "Failed setting up universe replication");
  }
  return Status::OK();
}

Status TwoDCTestBase::VerifyUniverseReplication(
    MiniCluster* consumer_cluster, YBClient* consumer_client,
    const std::string& universe_id, master::GetUniverseReplicationResponsePB* resp) {
  return LoggedWaitFor([=]() -> Result<bool> {
    master::GetUniverseReplicationRequestPB req;
    req.set_producer_id(universe_id);
    resp->Clear();

    auto master_proxy = std::make_shared<master::MasterServiceProxy>(
        &consumer_client->proxy_cache(),
        consumer_cluster->leader_mini_master()->bound_rpc_addr());
    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));

    Status s = master_proxy->GetUniverseReplication(req, resp, &rpc);
    return s.ok() && !resp->has_error() &&
            resp->entry().state() == master::SysUniverseReplicationEntryPB::ACTIVE;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Verify universe replication");
}

Status TwoDCTestBase::ToggleUniverseReplication(
    MiniCluster* consumer_cluster, YBClient* consumer_client,
    const std::string& universe_id, bool is_enabled) {
  master::SetUniverseReplicationEnabledRequestPB req;
  master::SetUniverseReplicationEnabledResponsePB resp;

  req.set_producer_id(universe_id);
  req.set_is_enabled(is_enabled);

  auto master_proxy = std::make_shared<master::MasterServiceProxy>(
      &consumer_client->proxy_cache(),
      consumer_cluster->leader_mini_master()->bound_rpc_addr());

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
  RETURN_NOT_OK(master_proxy->SetUniverseReplicationEnabled(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  return Status::OK();
}

Status TwoDCTestBase::VerifyUniverseReplicationDeleted(MiniCluster* consumer_cluster,
    YBClient* consumer_client, const std::string& universe_id, int timeout) {
  return LoggedWaitFor([=]() -> Result<bool> {
    master::GetUniverseReplicationRequestPB req;
    master::GetUniverseReplicationResponsePB resp;
    req.set_producer_id(universe_id);

    auto master_proxy = std::make_shared<master::MasterServiceProxy>(
        &consumer_client->proxy_cache(),
        consumer_cluster->leader_mini_master()->bound_rpc_addr());
    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));

    Status s = master_proxy->GetUniverseReplication(req, &resp, &rpc);
    return resp.has_error() && resp.error().code() == master::MasterErrorPB::OBJECT_NOT_FOUND;
  }, MonoDelta::FromMilliseconds(timeout), "Verify universe replication deleted");
}

Status TwoDCTestBase::GetCDCStreamForTable(
    const std::string& table_id, master::ListCDCStreamsResponsePB* resp) {
  return LoggedWaitFor([=]() -> Result<bool> {
    master::ListCDCStreamsRequestPB req;
    req.set_table_id(table_id);
    resp->Clear();

    Status s = producer_cluster()->leader_mini_master()->master()->catalog_manager()->
        ListCDCStreams(&req, resp);
    return s.ok() && !resp->has_error() && resp->streams_size() == 1;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Get CDC stream for table");
}

uint32_t TwoDCTestBase::GetSuccessfulWriteOps(MiniCluster* cluster) {
  uint32_t size = 0;
  for (const auto& mini_tserver : cluster->mini_tablet_servers()) {
    auto* tserver = dynamic_cast<tserver::enterprise::TabletServer*>(mini_tserver->server());
    CDCConsumer* cdc_consumer;
    if (tserver && (cdc_consumer = tserver->GetCDCConsumer())) {
      size += cdc_consumer->GetNumSuccessfulWriteRpcs();
    }
  }
  return size;
}

Status TwoDCTestBase::DeleteUniverseReplication(const std::string& universe_id) {
  return DeleteUniverseReplication(universe_id, consumer_client(), consumer_cluster());
}

Status TwoDCTestBase::DeleteUniverseReplication(
    const std::string& universe_id, YBClient* client, MiniCluster* cluster) {
  master::DeleteUniverseReplicationRequestPB req;
  master::DeleteUniverseReplicationResponsePB resp;

  req.set_producer_id(universe_id);

  auto master_proxy = std::make_shared<master::MasterServiceProxy>(
      &client->proxy_cache(),
      cluster->leader_mini_master()->bound_rpc_addr());

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(kRpcTimeout));
  RETURN_NOT_OK(master_proxy->DeleteUniverseReplication(req, &resp, &rpc));
  LOG(INFO) << "Delete universe succeeded";
  return Status::OK();
}

uint32_t TwoDCTestBase::NumProducerTabletsPolled(MiniCluster* cluster) {
  uint32_t size = 0;
  for (const auto& mini_tserver : cluster->mini_tablet_servers()) {
    uint32_t new_size = 0;
    auto* tserver = dynamic_cast<tserver::enterprise::TabletServer*>(
        mini_tserver->server());
    CDCConsumer* cdc_consumer;
    if (tserver && (cdc_consumer = tserver->GetCDCConsumer())) {
      auto tablets_running = cdc_consumer->TEST_producer_tablets_running();
      new_size = tablets_running.size();
    }
    size += new_size;
  }
  return size;
}

Status TwoDCTestBase::CorrectlyPollingAllTablets(
    MiniCluster* cluster, uint32_t num_producer_tablets) {
  return LoggedWaitFor([=]() -> Result<bool> {
    static int i = 0;
    constexpr int kNumIterationsWithCorrectResult = 5;
    auto cur_tablets = NumProducerTabletsPolled(cluster);
    if (cur_tablets == num_producer_tablets) {
      if (i++ == kNumIterationsWithCorrectResult) {
        i = 0;
        return true;
      }
    } else {
      i = 0;
    }
    LOG(INFO) << "Tablets being polled: " << cur_tablets;
    return false;
  }, MonoDelta::FromSeconds(kRpcTimeout), "Num producer tablets being polled");
}

} // namespace enterprise
} // namespace yb
