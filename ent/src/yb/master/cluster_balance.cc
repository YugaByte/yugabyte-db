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

#include "yb/master/cluster_balance.h"
#include "yb/master/cluster_balance_util.h"

using std::set;

namespace yb {
namespace master {
namespace enterprise {

Result<bool> ClusterLoadBalancer::HandleLeaderMoves(TabletId* out_tablet_id,
                                                    TabletServerId* out_from_ts,
                                                    TabletServerId* out_to_ts) {

  // If the user sets 'transaction_tables_use_preferred_zones' gflag to 0 and the tablet
  // being balanced is a transaction tablet, then logical flow will be changed to ignore
  // preferred zones and instead proceed to normal leader balancing.
  PerTableLoadState* ent_state = GetEntState();
  if (ent_state->use_preferred_zones_ &&
    VERIFY_RESULT(HandleLeaderLoadIfNonAffinitized(out_tablet_id, out_from_ts, out_to_ts))) {
    RETURN_NOT_OK(MoveLeader(*out_tablet_id, *out_from_ts, *out_to_ts));
    return true;
  }

  return super::HandleLeaderMoves(out_tablet_id, out_from_ts, out_to_ts);
}

void ClusterLoadBalancer::InitializeTSDescriptors() {
  PerTableLoadState* ent_state = GetEntState();
  if (ent_state->use_preferred_zones_) {
    GetAllAffinitizedZones(&ent_state->affinitized_zones_);
  }
  super::InitializeTSDescriptors();
}

void ClusterLoadBalancer::GetAllAffinitizedZones(AffinitizedZonesSet* affinitized_zones) const {
  SysClusterConfigEntryPB config;
  CHECK_OK(catalog_manager_->GetClusterConfig(&config));
  const int num_zones = config.replication_info().affinitized_leaders_size();
  for (int i = 0; i < num_zones; i++) {
    CloudInfoPB ci = config.replication_info().affinitized_leaders(i);
    affinitized_zones->insert(ci);
  }
}

Result<bool> ClusterLoadBalancer::HandleLeaderLoadIfNonAffinitized(TabletId* moving_tablet_id,
                                                                   TabletServerId* from_ts,
                                                                   TabletServerId* to_ts) {
  // Similar to normal leader balancing, we double iterate from most loaded to least loaded
  // non-affinitized nodes and least to most affinitized nodes. For each pair, we check whether
  // there is any tablet intersection and if so, there is a match and we return true.
  //
  // If we go through all the node pairs or we see that the current non-affinitized
  // leader load is 0, we know that there is no match from non-affinitized to affinitized nodes
  // and we return false.
  PerTableLoadState* ent_state = GetEntState();
  const int non_affinitized_last_pos = ent_state->sorted_non_affinitized_leader_load_.size() - 1;

  for (int non_affinitized_idx = non_affinitized_last_pos;
      non_affinitized_idx >= 0;
      non_affinitized_idx--) {
    for (const auto& affinitized_uuid : state_->sorted_leader_load_) {
      const TabletServerId& non_affinitized_uuid =
          ent_state->sorted_non_affinitized_leader_load_[non_affinitized_idx];
      if (state_->GetLeaderLoad(non_affinitized_uuid) == 0) {
        // All subsequent non-affinitized nodes have no leaders, no match found.
        return false;
      }

      const set<TabletId>& leaders = state_->per_ts_meta_[non_affinitized_uuid].leaders;
      const set<TabletId>& peers = state_->per_ts_meta_[affinitized_uuid].running_tablets;
      set<TabletId> intersection;
      const auto& itr = std::inserter(intersection, intersection.begin());
      std::set_intersection(leaders.begin(), leaders.end(), peers.begin(), peers.end(), itr);
      if (!intersection.empty()) {
        *moving_tablet_id = *intersection.begin();
        *from_ts = non_affinitized_uuid;
        *to_ts = affinitized_uuid;
        return true;
      }
    }
  }
  return false;
}

void ClusterLoadBalancer::PopulatePlacementInfo(TabletInfo* tablet, PlacementInfoPB* pb) {
  auto l = tablet->table()->LockForRead();
  const Options* options_ent = GetEntState()->GetEntOptions();
  if (options_ent->type == LIVE &&
      l->data().pb.has_replication_info() &&
      l->data().pb.replication_info().has_live_replicas()) {
    pb->CopyFrom(l->data().pb.replication_info().live_replicas());
  } else if (options_ent->type == READ_ONLY &&
      l->data().pb.has_replication_info() &&
      !l->data().pb.replication_info().read_replicas().empty()) {
    pb->CopyFrom(GetReadOnlyPlacementFromUuid(l->data().pb.replication_info()));
  } else {
    pb->CopyFrom(GetClusterPlacementInfo());
  }
}

Status ClusterLoadBalancer::UpdateTabletInfo(TabletInfo* tablet) {
  const auto& table_id = tablet->table()->id();
  // Set the placement information on a per-table basis, only once.
  if (!state_->placement_by_table_.count(table_id)) {
    PlacementInfoPB pb;
    {
      PopulatePlacementInfo(tablet, &pb);
    }
    state_->placement_by_table_[table_id] = std::move(pb);
  }

  return state_->UpdateTablet(tablet);
}

const PlacementInfoPB& ClusterLoadBalancer::GetLiveClusterPlacementInfo() const {
  auto l = down_cast<CatalogManager*>(catalog_manager_)->GetClusterConfigInfo()->LockForRead();
  return l->data().pb.replication_info().live_replicas();
}


const PlacementInfoPB& ClusterLoadBalancer::GetClusterPlacementInfo() const {
  auto l = down_cast<CatalogManager*>(catalog_manager_)->GetClusterConfigInfo()->LockForRead();
  if (GetEntState()->GetEntOptions()->type == LIVE) {
    return l->data().pb.replication_info().live_replicas();
  } else {
    return GetReadOnlyPlacementFromUuid(l->data().pb.replication_info());
  }
}


void ClusterLoadBalancer::RunLoadBalancer(yb::master::Options* options) {
  SysClusterConfigEntryPB config;
  CHECK_OK(catalog_manager_->GetClusterConfig(&config));

  std::unique_ptr<enterprise::Options> options_unique_ptr =
      std::make_unique<enterprise::Options>();
  Options* options_ent = options_unique_ptr.get();
  // First, we load balance the live cluster.
  options_ent->type = LIVE;
  if (config.replication_info().live_replicas().has_placement_uuid()) {
    options_ent->placement_uuid = config.replication_info().live_replicas().placement_uuid();
    options_ent->live_placement_uuid = options_ent->placement_uuid;
  } else {
    options_ent->placement_uuid = "";
    options_ent->live_placement_uuid = "";
  }
  super::RunLoadBalancer(options_ent);

  // Then, we balance all read-only clusters.
  options_ent->type = READ_ONLY;
  for (int i = 0; i < config.replication_info().read_replicas_size(); i++) {
    const PlacementInfoPB& read_only_cluster = config.replication_info().read_replicas(i);
    options_ent->placement_uuid = read_only_cluster.placement_uuid();
    super::RunLoadBalancer(options_ent);
  }
}

const PlacementInfoPB& ClusterLoadBalancer::GetReadOnlyPlacementFromUuid(
    const ReplicationInfoPB& replication_info) const {
  const Options* options_ent = GetEntState()->GetEntOptions();
  // We assume we have an read replicas field in our replication info.
  for (int i = 0; i < replication_info.read_replicas_size(); i++) {
    const PlacementInfoPB& read_only_placement = replication_info.read_replicas(i);
    if (read_only_placement.placement_uuid() == options_ent->placement_uuid) {
      return read_only_placement;
    }
  }
  // Should never get here.
  LOG(ERROR) << "Could not find read only cluster with placement uuid: "
             << options_ent->placement_uuid;
  return replication_info.read_replicas(0);
}

consensus::RaftPeerPB::MemberType ClusterLoadBalancer::GetDefaultMemberType() {
  if (GetEntState()->GetEntOptions()->type == LIVE) {
    return consensus::RaftPeerPB::PRE_VOTER;
  } else {
    return consensus::RaftPeerPB::PRE_OBSERVER;
  }
}

PerTableLoadState* ClusterLoadBalancer::GetEntState() const {
  return down_cast<enterprise::PerTableLoadState*>(state_);
}

} // namespace enterprise
} // namespace master
} // namespace yb
