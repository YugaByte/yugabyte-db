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

#include "../../src/yb/master/cluster_balance_util.h"

#ifndef ENT_SRC_YB_MASTER_CLUSTER_BALANCE_UTIL_H
#define ENT_SRC_YB_MASTER_CLUSTER_BALANCE_UTIL_H

namespace yb {
namespace master {
namespace enterprise {

// enum for replica type, either live (synchronous) or read only (timeline consistent)
enum ReplicaType {
  LIVE,
  READ_ONLY,
};

struct Options : yb::master::Options {
  Options() = default;

  ReplicaType type;
  string placement_uuid;
  string live_placement_uuid;
};

class PerTableLoadState : public yb::master::PerTableLoadState {
  typedef yb::master::PerTableLoadState super;
 public:
  explicit PerTableLoadState(yb::master::GlobalLoadState* global_load_state)
      : super(global_load_state) {}
  virtual ~PerTableLoadState() {}

  bool IsTsInLivePlacement(TSDescriptor* ts_desc) {
    return ts_desc->placement_uuid() == GetEntOptions()->live_placement_uuid;
  }

  void UpdateTabletServer(std::shared_ptr<yb::master::TSDescriptor> ts_desc) override {
    super::UpdateTabletServer(ts_desc);
    const auto& ts_uuid = ts_desc->permanent_uuid();

    bool is_ts_live = IsTsInLivePlacement(down_cast<TSDescriptor*>(ts_desc.get()));
    const Options* options_ent = GetEntOptions();
    switch (options_ent->type) {
      case LIVE: {
        if (!is_ts_live) {
          // LIVE cb run with READ_ONLY ts, ignore this ts
          sorted_load_.pop_back();
        }
        break;
      }
      case READ_ONLY: {
        if (is_ts_live) {
          // READ_ONLY cb run with LIVE ts, ignore this ts
          sorted_load_.pop_back();
        } else {
          string placement_uuid = down_cast<TSDescriptor*>(ts_desc.get())->placement_uuid();
          if (placement_uuid == "") {
            LOG(WARNING) << "Read only ts " << ts_desc->permanent_uuid()
                         << " does not have placement uuid";
          } else if (placement_uuid != options_ent->placement_uuid) {
            // Do not include this ts in load balancing.
            sorted_load_.pop_back();
          }
        }
        sorted_leader_load_.clear();
        return;
      }
    }

    if (sorted_leader_load_.empty() ||
        sorted_leader_load_.back() != ts_uuid ||
        affinitized_zones_.empty()) {
      return;
    }
    TSRegistrationPB registration = ts_desc->GetRegistration();
    if (affinitized_zones_.find(registration.common().cloud_info()) == affinitized_zones_.end()) {
      // This tablet server is in an affinitized leader zone.
      sorted_leader_load_.pop_back();
      sorted_non_affinitized_leader_load_.push_back(ts_uuid);
    }
  }

  void SortLeaderLoad() override {
    auto leader_count_comparator = LeaderLoadComparator(this);
    sort(sorted_non_affinitized_leader_load_.begin(),
         sorted_non_affinitized_leader_load_.end(),
         leader_count_comparator);

    super::SortLeaderLoad();
  }

  std::shared_ptr<const TabletInfo::ReplicaMap> GetReplicaLocations(TabletInfo* tablet) override {
    auto replica_locations = std::make_shared<TabletInfo::ReplicaMap>();
    auto replica_map = tablet->GetReplicaLocations();
    const Options* options_ent = GetEntOptions();
    for (const auto& it : *replica_map) {
      const TabletReplica& replica = it.second;
      TSDescriptor* ts_desc_ent = down_cast<TSDescriptor*>(replica.ts_desc);
      bool is_replica_live =  IsTsInLivePlacement(ts_desc_ent);
      if (is_replica_live && options_ent->type == LIVE) {
        InsertIfNotPresent(replica_locations.get(), it.first, replica);
      } else if (!is_replica_live && options_ent->type  == READ_ONLY) {
        const string& placement_uuid = ts_desc_ent->placement_uuid();
        if (placement_uuid == options_ent->placement_uuid) {
          InsertIfNotPresent(replica_locations.get(), it.first, replica);
        }
      }
    }
    return replica_locations;
  }

  const Options* GetEntOptions() const {
    return down_cast<Options*>(options_);
  }

  // If affinitized leaders is enabled, stores leader load for non affinitized nodes.
  vector<TabletServerId> sorted_non_affinitized_leader_load_;
  // List of availability zones for affinitized leaders.
  AffinitizedZonesSet affinitized_zones_;
};

} // namespace enterprise
} // namespace master
} // namespace yb

#endif // ENT_SRC_YB_MASTER_CLUSTER_BALANCE_UTIL_H
