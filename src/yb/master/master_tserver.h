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

#ifndef YB_MASTER_MASTER_TSERVER_H
#define YB_MASTER_MASTER_TSERVER_H

#include "yb/tserver/tablet_peer_lookup.h"
#include "yb/tserver/tablet_server_interface.h"

#include "yb/util/metrics.h"

namespace yb {
namespace master {

class Master;

// Master's version of a TabletServer which is required to support virtual tables in the Master.
// This isn't really an actual server and is just a nice way of overriding the default tablet
// server interface to support virtual tables.
class MasterTabletServer : public tserver::TabletServerIf,
                           public tserver::TabletPeerLookupIf {
 public:
  MasterTabletServer(Master* master, scoped_refptr<MetricEntity> metric_entity);
  tserver::TSTabletManager* tablet_manager() override;
  tserver::TabletPeerLookupIf* tablet_peer_lookup() override;

  server::Clock* Clock() override;
  const scoped_refptr<MetricEntity>& MetricEnt() const override;
  rpc::Publisher* GetPublisher() override { return nullptr; }

  CHECKED_STATUS GetTabletPeer(const std::string& tablet_id,
                               std::shared_ptr<tablet::TabletPeer>* tablet_peer) const override;

  CHECKED_STATUS GetTabletStatus(const tserver::GetTabletStatusRequestPB* req,
                                 tserver::GetTabletStatusResponsePB* resp) const override;

  bool LeaderAndReady(const TabletId& tablet_id, bool allow_stale = false) const override;

  const NodeInstancePB& NodeInstance() const override;

  CHECKED_STATUS GetRegistration(ServerRegistrationPB* reg) const override;

  CHECKED_STATUS StartRemoteBootstrap(const consensus::StartRemoteBootstrapRequestPB& req) override;

  void get_ysql_catalog_version(uint64_t* current_version,
                                uint64_t* last_breaking_version) const override;

  client::TransactionPool* TransactionPool() override {
    return nullptr;
  }

  client::YBClient* client() override {
    return nullptr;
  }

 private:
  Master* master_ = nullptr;
  scoped_refptr<MetricEntity> metric_entity_;
};

} // namespace master
} // namespace yb
#endif // YB_MASTER_MASTER_TSERVER_H
