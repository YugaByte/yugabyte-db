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

#ifndef YB_MASTER_CLUSTER_BALANCE_UTIL_H
#define YB_MASTER_CLUSTER_BALANCE_UTIL_H

#include <unordered_set>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>

#include "yb/master/catalog_manager.h"
#include "yb/master/ts_descriptor.h"
#include "yb/util/random.h"
#include "yb/util/status.h"

DECLARE_int32(min_leader_stepdown_retry_interval_ms);

DECLARE_bool(enable_load_balancing);

DECLARE_bool(transaction_tables_use_preferred_zones);

DECLARE_int32(leader_balance_threshold);

DECLARE_int32(leader_balance_unresponsive_timeout_ms);

DECLARE_int32(replication_factor);

DECLARE_int32(load_balancer_max_concurrent_tablet_remote_bootstraps);

DECLARE_int32(load_balancer_max_concurrent_tablet_remote_bootstraps_per_table);

DECLARE_int32(load_balancer_max_over_replicated_tablets);

DECLARE_int32(load_balancer_max_concurrent_adds);

DECLARE_int32(load_balancer_max_concurrent_removals);

DECLARE_int32(load_balancer_max_concurrent_moves);

DECLARE_int32(load_balancer_max_concurrent_moves_per_table);

namespace yb {
namespace master {

struct cloud_equal_to {
  bool operator()(const yb::CloudInfoPB& x, const yb::CloudInfoPB& y) const {
    return x.placement_cloud() == y.placement_cloud() &&
        x.placement_region() == y.placement_region() &&
        x.placement_zone() == y.placement_zone();
  }
};

struct cloud_hash {
  std::size_t operator()(const yb::CloudInfoPB& ci) const {
    return std::hash<std::string>{} (TSDescriptor::generate_placement_id(ci));
  }
};

using AffinitizedZonesSet = unordered_set<CloudInfoPB, cloud_hash, cloud_equal_to>;

struct CBTabletMetadata {
  bool is_missing_replicas() { return is_under_replicated || !under_replicated_placements.empty(); }

  bool has_wrong_placements() {
    return !wrong_placement_tablet_servers.empty() || !blacklisted_tablet_servers.empty();
  }

  bool has_blacklisted_leader() {
    return !leader_blacklisted_tablet_servers.empty();
  }

  // Number of running replicas for this tablet.
  int running = 0;

  // TODO(bogdan): actually use this!
  //
  // Number of starting replicas for this tablet.
  int starting = 0;

  // If this tablet has fewer replicas than the configured number in the PlacementInfoPB.
  bool is_under_replicated = false;

  // Set of placement ids that have less replicas available than the configured minimums.
  std::set<PlacementId> under_replicated_placements;

  // If this tablet has more replicas than the configured number in the PlacementInfoPB.
  bool is_over_replicated;

  // Set of tablet server ids that can be candidates for removal, due to tablet being
  // over-replicated. For tablets with placement information, this will be all tablet servers
  // that are housing replicas of this tablet, in a placement with strictly more replicas than the
  // configured minimum (as that means there is at least one of them we can remove, and still
  // respect the minimum).
  //
  // For tablets with no placement information, this will be all the tablet servers currently
  // serving this tablet, as we can downsize with no restrictions in this case.
  std::set<TabletServerId> over_replicated_tablet_servers;

  // Set of tablet server ids whose placement information does not match that listed in the
  // table's PlacementInfoPB. This will happen when we change the configuration for the table or
  // the cluster.
  std::set<TabletServerId> wrong_placement_tablet_servers;

  // Set of tablet server ids that have been blacklisted and as such, should not get any more load
  // assigned to them and should be prioritized for removing load.
  std::set<TabletServerId> blacklisted_tablet_servers;
  std::set<TabletServerId> leader_blacklisted_tablet_servers;

  // The tablet server id of the leader in this tablet's peer group.
  TabletServerId leader_uuid;

  // Leader stepdown failures. We use this to prevent retrying the same leader stepdown too soon.
  LeaderStepDownFailureTimes leader_stepdown_failures;

  std::string ToString() const {
    return Format("{ running: $0 starting: $1 is_under_replicated: $2 "
                      "under_replicated_placements: $3 is_over_replicated: $4 "
                      "over_replicated_tablet_servers: $5 wrong_placement_tablet_servers: $6 "
                      "blacklisted_tablet_servers: $7 leader_uuid: $8 "
                      "leader_stepdown_failures: $9 leader_blacklisted_tablet_servers: $10}",
                  running, starting, is_under_replicated, under_replicated_placements,
                  is_over_replicated, over_replicated_tablet_servers,
                  wrong_placement_tablet_servers, blacklisted_tablet_servers,
                  leader_uuid, leader_stepdown_failures, leader_blacklisted_tablet_servers);
  }
};

struct CBTabletServerMetadata {
  // The TSDescriptor for this tablet server.
  std::shared_ptr<TSDescriptor> descriptor = nullptr;

  // The set of tablet ids that this tablet server is currently running.
  std::set<TabletId> running_tablets;

  // The set of tablet ids that this tablet server is currently starting.
  std::set<TabletId> starting_tablets;

  // The set of tablet leader ids that this tablet server is currently running.
  std::set<TabletId> leaders;

  // The set of tablet ids that have possible non relevant data. Replica should be compacted
  // first before moving
  std::set<TabletId> parent_data_tablets;
};

struct CBTabletServerLoadCounts {
  // Stores global load counts for a tablet server.
  // See definitions of these counts in CBTabletServerMetadata.
  int running_tablets_count = 0;
  int starting_tablets_count = 0;
  int leaders_count = 0;
};

struct Options {
  Options() {}
  virtual ~Options() {}
  // If variance between load on TS goes past this number, we should try to balance.
  double kMinLoadVarianceToBalance = 2.0;

  // If variance between global load on TS goes past this number, we should try to balance.
  double kMinGlobalLoadVarianceToBalance = 2.0;

  // If variance between leader load on TS goes past this number, we should try to balance.
  double kMinLeaderLoadVarianceToBalance = 2.0;

  // Whether to limit the number of tablets being spun up on the cluster at any given time.
  bool kAllowLimitStartingTablets = true;

  // Max number of tablets being remote bootstrapped across the cluster, if we enable limiting
  // this.
  int kMaxTabletRemoteBootstraps = FLAGS_load_balancer_max_concurrent_tablet_remote_bootstraps;

  // Max number of tablets being remote bootstrapped for a specific table, if we enable limiting
  // this.
  int kMaxTabletRemoteBootstrapsPerTable =
      FLAGS_load_balancer_max_concurrent_tablet_remote_bootstraps_per_table;

  // Whether to limit the number of tablets that have more peers than configured at any given
  // time.
  bool kAllowLimitOverReplicatedTablets = true;

  // Max number of running tablet replicas that are over the configured limit.
  int kMaxOverReplicatedTablets = FLAGS_load_balancer_max_over_replicated_tablets;

  // Max number of over-replicated tablet peer removals to do in any one run of the load balancer.
  int kMaxConcurrentRemovals = FLAGS_load_balancer_max_concurrent_removals;

  // Max number of tablet peer replicas to add in any one run of the load balancer.
  int kMaxConcurrentAdds = FLAGS_load_balancer_max_concurrent_adds;

  // Max number of tablet leaders on tablet servers (across the cluster) to move in any one run of
  // the load balancer.
  int kMaxConcurrentLeaderMoves = FLAGS_load_balancer_max_concurrent_moves;

  // Max number of tablet leaders per table to move in any one run of the load balancer.
  int kMaxConcurrentLeaderMovesPerTable = FLAGS_load_balancer_max_concurrent_moves_per_table;

  // TODO(bogdan): add state for leaders starting remote bootstraps, to limit on that end too.
};

// Cluster-wide state and metrics.
// For now it's used to determine how many tablets are being remote bootstrapped across the cluster,
// as well as keeping track of global load counts in order to do global load balancing moves.
class GlobalLoadState {
 public:
  // Used to determine how many tablets are being remote bootstrapped across the cluster.
  int total_starting_tablets_ = 0;

  // Map from tablet server ids to the global metadata we store for each.
  unordered_map<TabletServerId, CBTabletServerLoadCounts> per_ts_global_meta_;

  // Get the global load for a certain TS.
  int GetGlobalLoad(const TabletServerId& ts_uuid) const {
    const auto& ts_meta = per_ts_global_meta_.at(ts_uuid);
    return ts_meta.starting_tablets_count + ts_meta.running_tablets_count;
  }

  TSDescriptorVector ts_descs_;
};

class PerTableLoadState {
 public:
  TableId table_id_;
  explicit PerTableLoadState(GlobalLoadState* global_state)
      : leader_balance_threshold_(FLAGS_leader_balance_threshold),
        current_time_(MonoTime::Now()),
        global_state_(global_state) {}
  virtual ~PerTableLoadState() {}

  // Comparators used for sorting by load.
  bool CompareByUuid(const TabletServerId& a, const TabletServerId& b) {
    int load_a = GetLoad(a);
    int load_b = GetLoad(b);
    if (load_a == load_b) {
      // Use global load as a heuristic to help break ties.
      load_a = global_state_->GetGlobalLoad(a);
      load_b = global_state_->GetGlobalLoad(b);
      if (load_a == load_b) {
        return a < b;
      }
    }
    return load_a < load_b;
  }

  bool CompareByReplica(const TabletReplica& a, const TabletReplica& b) {
    return CompareByUuid(a.ts_desc->permanent_uuid(), b.ts_desc->permanent_uuid());
  }

  // Comparator functor to be able to wrap around the public but non-static compare methods that
  // end up using internal state of the class.
  struct Comparator {
    explicit Comparator(PerTableLoadState* state) : state_(state) {}
    bool operator()(const TabletServerId& a, const TabletServerId& b) {
      return state_->CompareByUuid(a, b);
    }

    bool operator()(const TabletReplica& a, const TabletReplica& b) {
      return state_->CompareByReplica(a, b);
    }

    PerTableLoadState* state_;
  };

  // Comparator to sort tablet servers' leader load.
  struct LeaderLoadComparator {
    explicit LeaderLoadComparator(PerTableLoadState* state) : state_(state) {}
    bool operator()(const TabletServerId& a, const TabletServerId& b) {
      // Primary criteria: whether tserver is leader blacklisted.
      auto a_leader_blacklisted =
        state_->leader_blacklisted_servers_.find(a) != state_->leader_blacklisted_servers_.end();
      auto b_leader_blacklisted =
        state_->leader_blacklisted_servers_.find(b) != state_->leader_blacklisted_servers_.end();
      if (a_leader_blacklisted != b_leader_blacklisted) {
        return !a_leader_blacklisted;
      }

      // Secondary criteria: tserver leader load.
      return state_->GetLeaderLoad(a) < state_->GetLeaderLoad(b);
    }
    PerTableLoadState* state_;
  };

  // Get the load for a certain TS.
  int GetLoad(const TabletServerId& ts_uuid) const {
    const auto& ts_meta = per_ts_meta_.at(ts_uuid);
    return ts_meta.starting_tablets.size() + ts_meta.running_tablets.size();
  }

  // Get the load for a certain TS.
  int GetLeaderLoad(const TabletServerId& ts_uuid) const {
    return per_ts_meta_.at(ts_uuid).leaders.size();
  }

  void SetBlacklist(const BlacklistPB& blacklist) { blacklist_ = blacklist; }
  void SetLeaderBlacklist(const BlacklistPB& leader_blacklist) {
    leader_blacklist_ = leader_blacklist;
  }

  // Update the per-tablet information for this tablet.
  Status UpdateTablet(TabletInfo* tablet) {
    const auto& tablet_id = tablet->id();
    // Set the per-tablet entry to empty default and get the reference for filling up information.
    auto& tablet_meta = per_tablet_meta_[tablet_id];

    // Get the placement for this tablet.
    const auto& placement = placement_by_table_[tablet->table()->id()];

    // Get replicas for this tablet.
    auto replica_map = GetReplicaLocations(tablet);
    // Set state information for both the tablet and the tablet server replicas.
    for (const auto& replica : *replica_map) {
      const auto& ts_uuid = replica.first;
      // If we do not have ts_meta information for this particular replica, then we are in the
      // rare case where we just became the master leader and started doing load balancing, but we
      // have yet to receive heartbeats from all the tablet servers. We will just return false
      // across the stack and stop load balancing and log errors, until we get all the needed info.
      //
      // Worst case scenario, there is a network partition that is stopping us from actually
      // getting the heartbeats from a certain tablet server, but we anticipate that to be a
      // temporary matter. We should monitor error logs for this and see that it never actually
      // becomes a problem!
      if (per_ts_meta_.find(ts_uuid) == per_ts_meta_.end()) {
        return STATUS_SUBSTITUTE(LeaderNotReadyToServe, "Master leader has not yet received "
            "heartbeat from ts $0, either master just became leader or a network partition.",
                                 ts_uuid);
      }

      // Fill leader info.
      if (replica.second.role == consensus::RaftPeerPB::LEADER) {
        tablet_meta.leader_uuid = ts_uuid;
        RETURN_NOT_OK(AddLeaderTablet(tablet_id, ts_uuid));
      }

      const tablet::RaftGroupStatePB& tablet_state = replica.second.state;
      const bool replica_is_stale = replica.second.IsStale();
      VLOG(2) << "Tablet " << tablet_id << " for table " << table_id_
                << " is in state " << RaftGroupStatePB_Name(tablet_state);
      if (tablet_state == tablet::RUNNING) {
        RETURN_NOT_OK(AddRunningTablet(tablet_id, ts_uuid));
      } else if (!replica_is_stale &&
                 (tablet_state == tablet::BOOTSTRAPPING || tablet_state == tablet::NOT_STARTED)) {
        // Keep track of transitioning state (not running, but not in a stopped or failed state).
        RETURN_NOT_OK(AddStartingTablet(tablet_id, ts_uuid));
        VLOG(1) << "Increased total_starting to "
                   << total_starting_ << " for tablet " << tablet_id << " and table " << table_id_;
      } else if (replica_is_stale) {
        VLOG(1) << "Replica is stale: " << replica.second.ToString();
      }
      if (replica.second.processing_parent_data) {
        RETURN_NOT_OK(AddParentDataTablet(tablet_id, ts_uuid));
        VLOG(1) << "Replica might have non relevant data: " << replica.second.ToString();
      } else {
        RETURN_NOT_OK(RemoveParentDataTablet(tablet_id, ts_uuid));
      }

      // If this replica is blacklisted, we want to keep track of these specially, so we can
      // prioritize accordingly.
      if (blacklisted_servers_.count(ts_uuid)) {
        tablet_meta.blacklisted_tablet_servers.insert(ts_uuid);
      }

      // If this replica has blacklisted leader, we want to keep track of these specially, so we can
      // prioritize accordingly.
      if (leader_blacklisted_servers_.count(ts_uuid)) {
        tablet_meta.leader_blacklisted_tablet_servers.insert(ts_uuid);
      }
    }

    // Only set the over-replication section if we need to.
    int placement_num_replicas = placement.num_replicas() > 0 ?
        placement.num_replicas() : FLAGS_replication_factor;
    tablet_meta.is_over_replicated = placement_num_replicas < replica_map->size();
    tablet_meta.is_under_replicated = placement_num_replicas > replica_map->size();

    // If no placement information, we will have already set the over and under replication flags.
    // For under-replication, we cannot use any placement_id, so we just leave the set empty and
    // use that as a marker that we are in this situation.
    //
    // For over-replication, we just add all the ts_uuids as candidates.
    if (placement.placement_blocks().empty()) {
      if (tablet_meta.is_over_replicated) {
        for (auto& replica_entry : *replica_map) {
          tablet_meta.over_replicated_tablet_servers.insert(std::move(replica_entry.first));
        }
      }
    } else {
      // If we do have placement information, figure out how the load is distributed based on
      // placement blocks, for this tablet.
      unordered_map<PlacementId, vector<TabletReplica>> placement_to_replicas;
      unordered_map<PlacementId, int> placement_to_min_replicas;
      // Preset the min_replicas, so we know if we're missing replicas somewhere as well.
      for (const auto& pb : placement.placement_blocks()) {
        const auto& placement_id = TSDescriptor::generate_placement_id(pb.cloud_info());
        // Default empty vector.
        placement_to_replicas[placement_id];
        placement_to_min_replicas[placement_id] = pb.min_num_replicas();
      }
      // Now actually fill the structures with matching TSs.
      for (auto& replica_entry : *replica_map) {
        if (VERIFY_RESULT(HasValidPlacement(replica_entry.first, &placement))) {
          const auto& placement_id = per_ts_meta_[replica_entry.first].descriptor->placement_id();
          placement_to_replicas[placement_id].push_back(std::move(replica_entry.second));
        } else {
          // If placement does not match, we likely changed the config or the schema and this
          // tablet should no longer live on this tablet server.
          tablet_meta.wrong_placement_tablet_servers.insert(std::move(replica_entry.first));
        }
      }

      // Loop over the data and populate extra replica as well as missing replica information.
      for (const auto& entry : placement_to_replicas) {
        const auto& placement_id = entry.first;
        const auto& replica_set = entry.second;
        const auto min_num_replicas = placement_to_min_replicas[placement_id];
        if (min_num_replicas > replica_set.size()) {
          // Placements that are under-replicated should be handled ASAP.
          tablet_meta.under_replicated_placements.insert(placement_id);
        } else if (tablet_meta.is_over_replicated && min_num_replicas < replica_set.size()) {
          // If this tablet is over-replicated, consider all the placements that have more than the
          // minimum number of tablets, as candidates for removing a replica.
          for (auto& replica : replica_set) {
            tablet_meta.over_replicated_tablet_servers.insert(
              std::move(replica.ts_desc->permanent_uuid()));
          }
        }
      }
    }
    tablet->GetLeaderStepDownFailureTimes(
        current_time_ - MonoDelta::FromMilliseconds(FLAGS_min_leader_stepdown_retry_interval_ms),
        &tablet_meta.leader_stepdown_failures);

    // Prepare placement related sets for tablets that have placement info.
    if (tablet_meta.is_missing_replicas()) {
      tablets_missing_replicas_.insert(tablet_id);
    }
    if (tablet_meta.is_over_replicated) {
      tablets_over_replicated_.insert(tablet_id);
    }
    if (tablet_meta.has_wrong_placements()) {
      tablets_wrong_placement_.insert(tablet_id);
    }

    return Status::OK();
  }

  virtual void UpdateTabletServer(std::shared_ptr<TSDescriptor> ts_desc) {
    const auto& ts_uuid = ts_desc->permanent_uuid();
    // Set and get, so we can use this for both tablet servers we've added data to, as well as
    // tablet servers that happen to not be serving any tablets, so were not in the map yet.
    auto& ts_meta = per_ts_meta_[ts_uuid];
    ts_meta.descriptor = ts_desc;

    // Also insert into per_ts_global_meta_ if we have yet to.
    global_state_->per_ts_global_meta_.emplace(ts_uuid, CBTabletServerLoadCounts());

    sorted_load_.push_back(ts_uuid);

    // Mark as blacklisted if it matches.
    bool is_blacklisted = false;
    for (const auto& hp : blacklist_.hosts()) {
      if (ts_meta.descriptor->IsRunningOn(hp)) {
        blacklisted_servers_.insert(ts_uuid);
        is_blacklisted = true;
        break;
      }
    }

    // Mark as blacklisted leader if it matches.
    for (const auto& hp : leader_blacklist_.hosts()) {
      if (ts_meta.descriptor->IsRunningOn(hp)) {
        leader_blacklisted_servers_.insert(ts_uuid);
        break;
      }
    }

    // Add this tablet server for leader load-balancing only if it is not blacklisted and it has
    // heartbeated recently enough to be considered responsive for leader balancing.
    if (!is_blacklisted &&
        ts_desc->TimeSinceHeartbeat().ToMilliseconds() <
        FLAGS_leader_balance_unresponsive_timeout_ms) {
      sorted_leader_load_.push_back(ts_uuid);
    }

    if (ts_desc->HasTabletDeletePending()) {
      servers_with_pending_deletes_.insert(ts_uuid);
    }
  }

  Result<bool> CanAddTabletToTabletServer(
    const TabletId& tablet_id, const TabletServerId& to_ts,
    const PlacementInfoPB* placement_info = nullptr) {
    const auto& ts_meta = per_ts_meta_[to_ts];
    // If this tablet has already been added to a new tablet server, don't add it again.
    if (tablets_added_.count(tablet_id)) {
      return false;
    }
    // We do not add load to blacklisted servers.
    if (blacklisted_servers_.count(to_ts)) {
      return false;
    }
    // We cannot add a tablet to a tablet server if it is already serving it.
    if (ts_meta.running_tablets.count(tablet_id) || ts_meta.starting_tablets.count(tablet_id)) {
      return false;
    }
    // If we ask to use placement information, check against it.
    if (placement_info && !VERIFY_RESULT(HasValidPlacement(to_ts, placement_info))) {
      LOG(INFO) << "tablet server " << to_ts << " has invalid placement info. "
                << "Not allowing it to take more tablets.";
      return false;
    }
    // If this server has a pending tablet delete, don't use it.
    if (servers_with_pending_deletes_.count(to_ts)) {
      LOG(INFO) << "tablet server " << to_ts << " has a pending delete. "
                << "Not allowing it to take more tablets";
      return false;
    }
    // If all checks pass, return true.
    return true;
  }

  Result<bool> HasValidPlacement(const TabletServerId& ts_uuid,
                                 const PlacementInfoPB* placement_info) {
    if (!placement_info->placement_blocks().empty()) {
      for (const auto& pb : placement_info->placement_blocks()) {
        if (per_ts_meta_[ts_uuid].descriptor->MatchesCloudInfo(pb.cloud_info())) {
          return true;
        }
      }
      return false;
    }
    return true;
  }

  Result<bool> CanSelectWrongReplicaToMove(
    const TabletId& tablet_id, const PlacementInfoPB& placement_info, TabletServerId* out_from_ts,
    TabletServerId* out_to_ts) {
    // We consider both invalid placements (potentially due to config or schema changes), as well
    // as servers being blacklisted, as wrong placement.
    const auto& tablet_meta = per_tablet_meta_[tablet_id];
    // Prioritize taking away load from blacklisted servers, then from wrong placements.
    bool found_match = false;
    // Use these to do a fallback move, if placement id is the only thing that does not match.
    TabletServerId fallback_to_uuid;
    TabletServerId fallback_from_uuid;
    for (const auto& from_uuid : tablet_meta.blacklisted_tablet_servers) {
      bool invalid_placement = tablet_meta.wrong_placement_tablet_servers.count(from_uuid);
      for (const auto& to_uuid : sorted_load_) {
        // TODO(bogdan): this could be made smarter if we kept track of per-placement numbers and
        // allowed to remove one from one placement, as long as it is still above the minimum.
        //
        // If this is a blacklisted server, we should aim to still respect placement and for now,
        // just try to move the load to the same placement. However, if the from_uuid was
        // previously invalidly placed, then we should ignore its placement.
        if (invalid_placement &&
            VERIFY_RESULT(CanAddTabletToTabletServer(tablet_id, to_uuid, &placement_info))) {
          found_match = true;
        } else {
          if (VERIFY_RESULT(CanAddTabletToTabletServer(tablet_id, to_uuid))) {
            const auto& from_placement_id = per_ts_meta_[from_uuid].descriptor->placement_id();
            const auto& to_placement_id = per_ts_meta_[to_uuid].descriptor->placement_id();
            if (from_placement_id == to_placement_id) {
              found_match = true;
            } else {
              // ENG-500 : Placement does not match, but we can still use this combo as a fallback.
              // It uses the last such pair, which should be fine.
              fallback_to_uuid = to_uuid;
              fallback_from_uuid = from_uuid;
            }
          }
        }
        if (found_match) {
          *out_from_ts = from_uuid;
          *out_to_ts = to_uuid;
          return true;
        }
      }
    }

    if (!fallback_to_uuid.empty()) {
      *out_from_ts = fallback_from_uuid;
      *out_to_ts = fallback_to_uuid;
      return true;
    }

    // TODO(bogdan): sort and pick the highest load as source.
    //
    // If we didn't have or find any blacklisted server to move load from, move to the wrong
    // placement tablet servers. We can pick any of them as the source for now.
    if (!tablet_meta.wrong_placement_tablet_servers.empty()) {
      for (const auto& to_uuid : sorted_load_) {
        if (VERIFY_RESULT(CanAddTabletToTabletServer(tablet_id, to_uuid, &placement_info))) {
          *out_from_ts = *tablet_meta.wrong_placement_tablet_servers.begin();
          *out_to_ts = to_uuid;
          return true;
        }
      }
    }

    return false;
  }

  Status AddReplica(const TabletId& tablet_id, const TabletServerId& to_ts) {
    RETURN_NOT_OK(AddStartingTablet(tablet_id, to_ts));
    tablets_added_.insert(tablet_id);
    SortLoad();
    return Status::OK();
  }

  Status RemoveReplica(const TabletId& tablet_id, const TabletServerId& from_ts) {
    RETURN_NOT_OK(RemoveRunningTablet(tablet_id, from_ts));
    if (per_ts_meta_[from_ts].starting_tablets.count(tablet_id)) {
      LOG(DFATAL) << "Invalid request: remove starting tablet " << tablet_id
                  << " from ts " << from_ts;
    }
    if (per_tablet_meta_[tablet_id].leader_uuid == from_ts) {
      RETURN_NOT_OK(MoveLeader(tablet_id, from_ts));
    }
    // This artificially constrains the removes to only handle one over_replication/wrong_placement
    // per run.
    // Create a copy of tablet_id because tablet_id could be a const reference from
    // tablets_wrong_placement_ (if the requests comes from HandleRemoveIfWrongPlacement) or a const
    // reference from tablets_over_replicated_ (if the request comes from HandleRemoveReplicas).
    TabletId tablet_id_key(tablet_id);
    tablets_over_replicated_.erase(tablet_id_key);
    tablets_wrong_placement_.erase(tablet_id_key);
    SortLoad();
    return Status::OK();
  }

  void SortLoad() {
    auto comparator = Comparator(this);
    sort(sorted_load_.begin(), sorted_load_.end(), comparator);
  }

  Status MoveLeader(
    const TabletId& tablet_id, const TabletServerId& from_ts, const TabletServerId& to_ts = "") {
    if (per_tablet_meta_[tablet_id].leader_uuid != from_ts) {
      return STATUS_SUBSTITUTE(IllegalState, "Tablet $0 has leader $1, but $2 expected.",
                               tablet_id, per_tablet_meta_[tablet_id].leader_uuid, from_ts);
    }
    per_tablet_meta_[tablet_id].leader_uuid = to_ts;
    RETURN_NOT_OK(RemoveLeaderTablet(tablet_id, from_ts));
    if (!to_ts.empty()) {
      RETURN_NOT_OK(AddLeaderTablet(tablet_id, to_ts));
    }
    SortLeaderLoad();
    return Status::OK();
  }

  virtual void SortLeaderLoad() {
    auto leader_count_comparator = LeaderLoadComparator(this);
    sort(sorted_leader_load_.begin(), sorted_leader_load_.end(), leader_count_comparator);
  }

  void LogSortedLeaderLoad() {
    // Sample output:
    // ts1_uuid[ts1_load] ts2_uuid[ts2_load] ts4_uuid[ts4_load] -- ts3_uuid[ts3_load]
    // Note: entries following "--" are leader blacklisted tservers

    bool blacklisted_leader = false;
    std::string s;
    for (const auto& ts_uuid : sorted_leader_load_) {
      if (!blacklisted_leader) {
        blacklisted_leader = (leader_blacklisted_servers_.find(ts_uuid) !=
            leader_blacklisted_servers_.end());
        if (blacklisted_leader) {
          s += " --";
        }
      }

      s +=  " " + ts_uuid + "[" + strings::Substitute("$0", GetLeaderLoad(ts_uuid)) + "]";
    }
    if (s.size() > 0) {
      LOG(INFO) << "tservers sorted by whether leader blacklisted and load: " << s;
    }
  }

  inline bool IsLeaderLoadBelowThreshold(const TabletServerId& ts_uuid) {
    return ((leader_balance_threshold_ > 0) &&
            (GetLeaderLoad(ts_uuid) <= leader_balance_threshold_));
  }

  void AdjustLeaderBalanceThreshold() {
    if (leader_balance_threshold_ != 0) {
      int min_threshold = sorted_leader_load_.empty() ? 0 :
                          static_cast<int>(std::ceil(
                            static_cast<double>(per_tablet_meta_.size()) /
                            static_cast<double>(sorted_leader_load_.size())));
      if (leader_balance_threshold_ < min_threshold) {
        LOG(WARNING) << strings::Substitute(
          "leader_balance_threshold flag is set to $0 but is too low for the current "
            "configuration. Adjusting it to $1.",
          leader_balance_threshold_, min_threshold);
        leader_balance_threshold_ = min_threshold;
      }
    }
  }

  virtual std::shared_ptr<const TabletInfo::ReplicaMap> GetReplicaLocations(TabletInfo* tablet) {
    return tablet->GetReplicaLocations();
  }

  Status AddRunningTablet(TabletId tablet_id, TabletServerId ts_uuid) {
    SCHECK(per_ts_meta_.find(ts_uuid) != per_ts_meta_.end(), IllegalState,
           Format(uninitialized_ts_meta_format_msg, ts_uuid, table_id_));
    // Set::Insert returns a pair where the second value is whether or not an item was inserted.
    auto ret = per_ts_meta_.at(ts_uuid).running_tablets.insert(tablet_id);
    if (ret.second) {
      ++global_state_->per_ts_global_meta_[ts_uuid].running_tablets_count;
      ++total_running_;
      ++per_tablet_meta_[tablet_id].running;
    }
    return Status::OK();
  }

  Status RemoveRunningTablet(TabletId tablet_id, TabletServerId ts_uuid) {
    SCHECK(per_ts_meta_.find(ts_uuid) != per_ts_meta_.end(), IllegalState,
           Format(uninitialized_ts_meta_format_msg, ts_uuid, table_id_));
    int num_erased = per_ts_meta_.at(ts_uuid).running_tablets.erase(tablet_id);
    global_state_->per_ts_global_meta_[ts_uuid].running_tablets_count -= num_erased;
    total_running_ -= num_erased;
    per_tablet_meta_[tablet_id].running -= num_erased;
    return Status::OK();
  }

  Status AddStartingTablet(TabletId tablet_id, TabletServerId ts_uuid) {
    SCHECK(per_ts_meta_.find(ts_uuid) != per_ts_meta_.end(), IllegalState,
           Format(uninitialized_ts_meta_format_msg, ts_uuid, table_id_));
    auto ret = per_ts_meta_.at(ts_uuid).starting_tablets.insert(tablet_id);
    if (ret.second) {
      ++global_state_->per_ts_global_meta_[ts_uuid].starting_tablets_count;
      ++total_starting_;
      ++global_state_->total_starting_tablets_;
      ++per_tablet_meta_[tablet_id].starting;
    }
    return Status::OK();
  }

  Status AddLeaderTablet(TabletId tablet_id, TabletServerId ts_uuid) {
    SCHECK(per_ts_meta_.find(ts_uuid) != per_ts_meta_.end(), IllegalState,
           Format(uninitialized_ts_meta_format_msg, ts_uuid, table_id_));
    auto ret = per_ts_meta_.at(ts_uuid).leaders.insert(tablet_id);
    if (ret.second) {
      ++global_state_->per_ts_global_meta_[ts_uuid].leaders_count;
    }
    return Status::OK();
  }

  Status RemoveLeaderTablet(TabletId tablet_id, TabletServerId ts_uuid) {
    SCHECK(per_ts_meta_.find(ts_uuid) != per_ts_meta_.end(), IllegalState,
           Format(uninitialized_ts_meta_format_msg, ts_uuid, table_id_));
    int num_erased = per_ts_meta_.at(ts_uuid).leaders.erase(tablet_id);
    global_state_->per_ts_global_meta_[ts_uuid].leaders_count -= num_erased;
    return Status::OK();
  }

  Status AddParentDataTablet(TabletId tablet_id, TabletServerId ts_uuid) {
    SCHECK(per_ts_meta_.find(ts_uuid) != per_ts_meta_.end(), IllegalState,
           Format(uninitialized_ts_meta_format_msg, ts_uuid, table_id_));
    per_ts_meta_.at(ts_uuid).parent_data_tablets.insert(tablet_id);
    return Status::OK();
  }

  Status RemoveParentDataTablet(TabletId tablet_id, TabletServerId ts_uuid) {
    SCHECK(per_ts_meta_.find(ts_uuid) != per_ts_meta_.end(), IllegalState,
           Format(uninitialized_ts_meta_format_msg, ts_uuid, table_id_));
    if (per_ts_meta_.at(ts_uuid).parent_data_tablets.erase(tablet_id) != 0) {
      VLOG(1) << "Updated replica have relevant data, tablet id: " << tablet_id;
    }
    return Status::OK();
  }

  // PerTableLoadState member fields

  // Map from tablet ids to the metadata we store for each.
  unordered_map<TabletId, CBTabletMetadata> per_tablet_meta_;

  // Map from tablet server ids to the metadata we store for each.
  unordered_map<TabletServerId, CBTabletServerMetadata> per_ts_meta_;

  // Map from table id to placement information for this table. This will be used for both
  // determining over-replication, by checking num_replicas, but also for az awareness, by keeping
  // track of the placement block policies between cluster and table level.
  unordered_map<TableId, PlacementInfoPB> placement_by_table_;

  // Total number of running tablets in the clusters (including replicas).
  int total_running_ = 0;

  // Total number of tablet replicas being started across the cluster.
  int total_starting_ = 0;

  // Set of ts_uuid sorted ascending by load. This is the actual raw data of TS load.
  vector<TabletServerId> sorted_load_;

  // Set of tablet ids that have been determined to have missing replicas. This can mean they are
  // generically under-replicated (2 replicas active, but 3 configured), or missing replicas in
  // certain placements (3 replicas active out of 3 configured, but no replicas in one of the AZs
  // listed in the placement blocks).
  std::set<TabletId> tablets_missing_replicas_;

  // Set of tablet ids that have been temporarily over-replicated. This is used to pick tablets
  // to potentially bring back down to their proper configured size, if there are more running than
  // expected.
  std::set<TabletId> tablets_over_replicated_;

  // Set of tablet ids that have been determined to have replicas in incorrect placements.
  std::set<TabletId> tablets_wrong_placement_;

  // The cached blacklist setting of the cluster. We store this upfront, as we add to the list of
  // tablet servers one by one, so we compare against it once per tablet server.
  BlacklistPB blacklist_;
  BlacklistPB leader_blacklist_;

  // The list of tablet server ids that match the cached blacklist.
  std::set<TabletServerId> blacklisted_servers_;
  std::set<TabletServerId> leader_blacklisted_servers_;

  // List of tablet server ids that have pending deletes.
  std::set<TabletServerId> servers_with_pending_deletes_;

  // List of tablet ids that have been added to a new tablet server.
  std::set<TabletId> tablets_added_;

  // Number of leaders per each tablet server to balance below.
  int leader_balance_threshold_ = 0;

  // List of table server ids sorted by whether leader blacklisted and their leader load.
  // If affinitized leaders is enabled, stores leader load for affinitized nodes.
  vector<TabletServerId> sorted_leader_load_;

  unordered_map<TableId, TabletToTabletServerMap> pending_add_replica_tasks_;
  unordered_map<TableId, TabletToTabletServerMap> pending_remove_replica_tasks_;
  unordered_map<TableId, TabletToTabletServerMap> pending_stepdown_leader_tasks_;

  // Time at which we started the current round of load balancing.
  MonoTime current_time_;

  // The knobs we use for tweaking the flow of the algorithm.
  Options* options_;

  // Pointer to the cluster global state so that it can be updated when operations like add or
  // remove are executed.
  GlobalLoadState* global_state_;

  // Boolean whether tablets for this table should respect the affinited zones.
  bool use_preferred_zones_ = true;

 private:
  const std::string uninitialized_ts_meta_format_msg =
      "Found uninitialized ts_meta: ts_uuid: $0, table_uuid: $1";

  DISALLOW_COPY_AND_ASSIGN(PerTableLoadState);
}; // PerTableLoadState

} // namespace master
} // namespace yb

#endif // YB_MASTER_CLUSTER_BALANCE_UTIL_H
