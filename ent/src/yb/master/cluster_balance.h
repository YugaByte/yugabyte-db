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

#ifndef ENT_SRC_YB_MASTER_CLUSTER_BALANCE_H
#define ENT_SRC_YB_MASTER_CLUSTER_BALANCE_H

#include "../../src/yb/master/cluster_balance.h"

namespace yb {
namespace master {
namespace enterprise {

class ClusterLoadBalancer : public yb::master::ClusterLoadBalancer {
  typedef yb::master::ClusterLoadBalancer super;
 public:
  explicit ClusterLoadBalancer(yb::master::CatalogManager* cm)
      : super(cm) {
  }

  void InitializeTSDescriptors() override
      NO_THREAD_SAFETY_ANALYSIS; // llvm bug: can't analyze super class friend access rights.

  Result<bool> HandleLeaderMoves(
      TabletId* out_tablet_id, TabletServerId* out_from_ts, TabletServerId* out_to_ts) override
      NO_THREAD_SAFETY_ANALYSIS; // llvm bug: can't analyze super class friend access rights.

  virtual void GetAllAffinitizedZones(AffinitizedZonesSet* affinitized_zones) const;

  // This function handles leader load from non-affinitized to affinitized nodes.
  // If it can find a way to move leader load from a non-affinitized to affinitized node,
  // returns true, if not returns false, if error is found, returns Status.
  // This is called before normal leader load balancing.
  Result<bool> HandleLeaderLoadIfNonAffinitized(
      TabletId* moving_tablet_id, TabletServerId* from_ts, TabletServerId* to_ts);

  CHECKED_STATUS UpdateTabletInfo(TabletInfo* tablet) override;

  // Runs the load balancer once for the live and all read only clusters, in order
  // of the cluster config.
  void RunLoadBalancer(yb::master::Options* options = nullptr) override;

  // Override now gets appropriate live or read only cluster placement,
  // depending on placement_uuid_.
  const PlacementInfoPB& GetClusterPlacementInfo() const override;

  // If type_ is live, return PRE_VOTER, otherwise, return PRE_OBSERVER.
  consensus::RaftPeerPB::MemberType GetDefaultMemberType() override;

  // Returns a pointer to an enterprise PerTableLoadState from the table_state_ variable.
  PerTableLoadState* GetEntState() const;

  // Populates pb with the placement info in tablet's config at cluster placement_uuid_.
  void PopulatePlacementInfo(TabletInfo* tablet, PlacementInfoPB* pb);

  // Returns the read only placement info from placement_uuid_.
  const PlacementInfoPB& GetReadOnlyPlacementFromUuid(
      const ReplicationInfoPB& replication_info) const;

  virtual const PlacementInfoPB& GetLiveClusterPlacementInfo() const;


};

} // namespace enterprise
} // namespace master
} // namespace yb

#endif // ENT_SRC_YB_MASTER_CLUSTER_BALANCE_H
