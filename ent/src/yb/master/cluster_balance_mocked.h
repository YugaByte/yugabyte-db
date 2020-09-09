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

#ifndef ENT_SRC_YB_MASTER_CLUSTER_BALANCE_MOCKED_H
#define ENT_SRC_YB_MASTER_CLUSTER_BALANCE_MOCKED_H

#include "yb/master/cluster_balance.h"
#include "../../src/yb/master/cluster_balance_mocked.h"

namespace yb {
namespace master {
namespace enterprise {

class ClusterLoadBalancerMocked : public ClusterLoadBalancer {
 public:
  explicit ClusterLoadBalancerMocked(Options* options) : ClusterLoadBalancer(nullptr) {
    const int kHighNumber = 100;
    options->kMaxConcurrentAdds = kHighNumber;
    options->kMaxConcurrentRemovals = kHighNumber;
    options->kAllowLimitStartingTablets = false;
    options->kAllowLimitOverReplicatedTablets = false;

    auto table_state = std::make_unique<enterprise::PerTableLoadState>(global_state_.get());
    table_state->options_ = options;
    state_ = table_state.get();
    per_table_states_[""] = std::move(table_state);

    SetEntOptions(LIVE, "");
  }

  // Overrides for base class functionality to bypass calling CatalogManager.
  void GetAllReportedDescriptors(TSDescriptorVector* ts_descs) const override {
    *ts_descs = ts_descs_;
  }

  void GetAllAffinitizedZones(AffinitizedZonesSet* affinitized_zones) const override {
    *affinitized_zones = affinitized_zones_;
  }

  const TabletInfoMap& GetTabletMap() const override { return tablet_map_; }

  const TableInfoMap& GetTableMap() const override { return table_map_; }

  const scoped_refptr<TableInfo> GetTableInfo(const TableId& table_uuid) const override {
    return FindPtrOrNull(table_map_, table_uuid);
  }

  const PlacementInfoPB& GetLiveClusterPlacementInfo() const override {
    return replication_info_.live_replicas();
  }

  const PlacementInfoPB& GetClusterPlacementInfo() const override {
    return GetEntState()->GetEntOptions()->type == LIVE ?
        replication_info_.live_replicas() : replication_info_.read_replicas(0);
  }

  const BlacklistPB& GetServerBlacklist() const override { return blacklist_; }
  const BlacklistPB& GetLeaderBlacklist() const override { return leader_blacklist_; }

  Status SendReplicaChanges(scoped_refptr<TabletInfo> tablet, const TabletServerId& ts_uuid,
                          const bool is_add, const bool should_remove,
                          const TabletServerId& new_leader_uuid) override {
    // Do nothing.
    return Status::OK();
  }

  void GetPendingTasks(const TableId& table_uuid,
                       TabletToTabletServerMap* pending_add_replica_tasks,
                       TabletToTabletServerMap* pending_remove_replica_tasks,
                       TabletToTabletServerMap* pending_stepdown_leader_tasks) override {
    for (const auto& tablet_id : pending_add_replica_tasks_) {
      (*pending_add_replica_tasks)[tablet_id] = "";
    }
    for (const auto& tablet_id : pending_remove_replica_tasks_) {
      (*pending_remove_replica_tasks)[tablet_id] = "";
    }
    for (const auto& tablet_id : pending_stepdown_leader_tasks_) {
      (*pending_stepdown_leader_tasks)[tablet_id] = "";
    }
  }

  void SetEntOptions(ReplicaType type, const string& placement_uuid) {
    down_cast<Options*>(GetEntState()->options_)->type = type;
    down_cast<Options*>(GetEntState()->options_)->placement_uuid = placement_uuid;
  }

  void ResetTableStatePtr(const TableId& table_id, yb::master::Options* options) override {
    if (state_) {
      options = state_->options_;
    }
    auto table_state = std::make_unique<enterprise::PerTableLoadState>(global_state_.get());
    table_state->options_ = options;

    state_ = table_state.get();

    per_table_states_[table_id] = std::move(table_state);

  }

  TSDescriptorVector ts_descs_;
  AffinitizedZonesSet affinitized_zones_;
  TabletInfoMap tablet_map_;
  TableInfoMap table_map_;
  ReplicationInfoPB replication_info_;
  BlacklistPB blacklist_;
  BlacklistPB leader_blacklist_;
  vector<TabletId> pending_add_replica_tasks_;
  vector<TabletId> pending_remove_replica_tasks_;
  vector<TabletId> pending_stepdown_leader_tasks_;

  friend class TestLoadBalancerEnterprise;
};

} // namespace enterprise
} // namespace master
} // namespace yb

#endif // ENT_SRC_YB_MASTER_CLUSTER_BALANCE_MOCKED_H
