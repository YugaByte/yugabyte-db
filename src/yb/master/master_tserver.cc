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

#include "yb/master/master_tserver.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/sys_catalog.h"

namespace yb {
namespace master {

using consensus::StartRemoteBootstrapRequestPB;

MasterTabletServer::MasterTabletServer(Master* master, scoped_refptr<MetricEntity> metric_entity)
    : master_(master), metric_entity_(metric_entity) {
}

tserver::TSTabletManager* MasterTabletServer::tablet_manager() {
  return nullptr;
}

tserver::TabletPeerLookupIf* MasterTabletServer::tablet_peer_lookup() {
  return this;
}

server::Clock* MasterTabletServer::Clock() {
  return master_->clock();
}

const scoped_refptr<MetricEntity>& MasterTabletServer::MetricEnt() const {
  return metric_entity_;
}

Status MasterTabletServer::GetTabletPeer(const string& tablet_id,
                                         std::shared_ptr<tablet::TabletPeer>* tablet_peer) const {
  if (tablet_id == kSysCatalogTabletId) {
    *tablet_peer = master_->catalog_manager()->sys_catalog()->tablet_peer();
    return Status::OK();
  }
  return STATUS_FORMAT(NotFound, "tablet $0 not found", tablet_id);
}

Status MasterTabletServer::GetTabletStatus(const tserver::GetTabletStatusRequestPB* req,
                                           tserver::GetTabletStatusResponsePB* resp) const {
  std::shared_ptr<tablet::TabletPeer> tablet_peer;
  // Tablets for YCQL virtual tables have no peer and we will return the NotFound status. That is
  // ok because GetTabletStatus is called for the cases when a tablet is moved or otherwise down
  // and being boostrapped, which should not happen to those tables.
  RETURN_NOT_OK(GetTabletPeer(req->tablet_id(), &tablet_peer));
  tablet_peer->GetTabletStatusPB(resp->mutable_tablet_status());
  return Status::OK();
}

bool MasterTabletServer::LeaderAndReady(const TabletId& tablet_id, bool allow_stale) const {
  std::shared_ptr<tablet::TabletPeer> tablet_peer;
  if (!GetTabletPeer(tablet_id, &tablet_peer).ok()) {
    return false;
  }
  return tablet_peer->LeaderStatus(allow_stale) == consensus::LeaderStatus::LEADER_AND_READY;
}

const NodeInstancePB& MasterTabletServer::NodeInstance() const {
  return master_->catalog_manager()->NodeInstance();
}

Status MasterTabletServer::GetRegistration(ServerRegistrationPB* reg) const {
  return STATUS(NotSupported, "Getting tserver registration not supported by master tserver");
}

Status MasterTabletServer::StartRemoteBootstrap(const StartRemoteBootstrapRequestPB& req) {
  return STATUS(NotSupported, "Remote bootstrap not supported by master tserver");
}

void MasterTabletServer::get_ysql_catalog_version(uint64_t* current_version,
                                                  uint64_t* last_breaking_version) const {
  Status s = master_->catalog_manager()->GetYsqlCatalogVersion(current_version,
                                                               last_breaking_version);
  if (!s.ok()) {
    /*
     * This should never happen, but if it does then we cannot guarantee that user requests
     * received by this master's tserver interface have a compatible version.
     * Log an error and return the highest possible version to ensure we reject the request if
     * it needs a catalog version compatibility check.
     */
    LOG(ERROR) << "Could not get YSQL catalog version for master's tserver API: "
               << s.ToUserMessage();
    if (current_version) {
      *current_version = UINT64_MAX;
    }
    if (last_breaking_version) {
      *last_breaking_version = UINT64_MAX;
    }
  }
}

} // namespace master
} // namespace yb
