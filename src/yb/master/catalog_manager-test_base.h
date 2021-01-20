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

#ifndef YB_MASTER_CATALOG_MANAGER_TEST_BASE_H
#define YB_MASTER_CATALOG_MANAGER_TEST_BASE_H

#include <gtest/gtest.h>

#include "yb/gutil/strings/substitute.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/cluster_balance.h"
#include "yb/master/ts_descriptor.h"
#include "yb/util/test_util.h"
#include "yb/master/catalog_manager_util.h"
#include "yb/master/cluster_balance_mocked.h"

namespace yb {
namespace master {

const string default_cloud = "aws";
const string default_region = "us-west-1";
const int kDefaultNumReplicas = 3;
const string kLivePlacementUuid = "live";
const string kReadReplicaPlacementUuidPrefix = "rr_$0";

scoped_refptr<TabletInfo> CreateTablet(
    const scoped_refptr<TableInfo>& table, const TabletId& tablet_id, const string& start_key,
    const string& end_key) {
  scoped_refptr<TabletInfo> tablet = new TabletInfo(table, tablet_id);
  auto l = tablet->LockForWrite();
  PartitionPB* partition = l->mutable_data()->pb.mutable_partition();
  partition->set_partition_key_start(start_key);
  partition->set_partition_key_end(end_key);
  l->mutable_data()->pb.set_state(SysTabletsEntryPB::RUNNING);

  table->AddTablet(tablet.get());
  l->Commit();
  return tablet;
}

void CreateTable(const vector<string> split_keys, const int num_replicas, bool setup_placement,
                 TableInfo* table, vector<scoped_refptr<TabletInfo>>* tablets) {
  const int kNumSplits = split_keys.size();
  for (int i = 0; i <= kNumSplits; i++) {
    const string& start_key = (i == 0) ? "" : split_keys[i - 1];
    const string& end_key = (i == kNumSplits) ? "" : split_keys[i];
    string tablet_id = strings::Substitute("tablet-$0-$1", start_key, end_key);

    tablets->push_back(CreateTablet(table, tablet_id, start_key, end_key));
  }

  if (setup_placement) {
    auto l = table->LockForWrite();
    auto* ri = l->mutable_data()->pb.mutable_replication_info();
    ri->mutable_live_replicas()->set_num_replicas(num_replicas);
    l->Commit();
  }

  // The splits are of the form ("-a", "a-b", "b-c", "c-"), hence the +1.
  ASSERT_EQ(tablets->size(), split_keys.size() + 1);
}

void SetupRaftPeer(consensus::RaftPeerPB::MemberType member_type, std::string az,
                   consensus::RaftPeerPB* raft_peer) {
  raft_peer->Clear();
  raft_peer->set_member_type(member_type);
  auto* cloud_info = raft_peer->mutable_cloud_info();
  cloud_info->set_placement_cloud(default_cloud);
  cloud_info->set_placement_region(default_region);
  cloud_info->set_placement_zone(az);
}

void SetupClusterConfig(vector<string> azs, ReplicationInfoPB* replication_info) {

  PlacementInfoPB* placement_info = replication_info->mutable_live_replicas();
  placement_info->set_num_replicas(kDefaultNumReplicas);
  for (const string& az : azs) {
    auto pb = placement_info->add_placement_blocks();
    pb->mutable_cloud_info()->set_placement_cloud(default_cloud);
    pb->mutable_cloud_info()->set_placement_region(default_region);
    pb->mutable_cloud_info()->set_placement_zone(az);
    pb->set_min_num_replicas(1);
  }
}

void SetupClusterConfigWithReadReplicas(vector<string> live_azs,
                                        vector<vector<string>> read_replica_azs,
                                        ReplicationInfoPB* replication_info) {
  replication_info->Clear();
  SetupClusterConfig(live_azs, replication_info);
  replication_info->mutable_live_replicas()->set_placement_uuid(kLivePlacementUuid);
  int i = 0;
  for (const auto& placement : read_replica_azs) {
    auto* placement_info = replication_info->add_read_replicas();
    placement_info->set_placement_uuid(Format(kReadReplicaPlacementUuidPrefix, i));
    for (const auto& az : placement) {
      auto pb = placement_info->add_placement_blocks();
      pb->mutable_cloud_info()->set_placement_cloud(default_cloud);
      pb->mutable_cloud_info()->set_placement_region(default_region);
      pb->mutable_cloud_info()->set_placement_zone(az);
      pb->set_min_num_replicas(1);
    }
    i++;
  }
}

void NewReplica(
    TSDescriptor* ts_desc, tablet::RaftGroupStatePB state, consensus::RaftPeerPB::Role role,
    TabletReplica* replica) {
  replica->ts_desc = ts_desc;
  replica->state = state;
  replica->role = role;
}

std::shared_ptr<TSDescriptor> SetupTS(const string& uuid, const string& az) {
  NodeInstancePB node;
  node.set_permanent_uuid(uuid);

  TSRegistrationPB reg;
  // Fake host:port combo, with uuid as host, for ease of testing.
  auto hp = reg.mutable_common()->add_private_rpc_addresses();
  hp->set_host(uuid);
  // Same cloud info as cluster config, with modifiable AZ.
  auto ci = reg.mutable_common()->mutable_cloud_info();
  ci->set_placement_cloud(default_cloud);
  ci->set_placement_region(default_region);
  ci->set_placement_zone(az);

  std::shared_ptr<TSDescriptor> ts(new enterprise::TSDescriptor(node.permanent_uuid()));
  CHECK_OK(ts->Register(node, reg, CloudInfoPB(), nullptr));
  return ts;
}

template<class ClusterLoadBalancerMockedClass>
class TestLoadBalancerBase {
 public:
  TestLoadBalancerBase(ClusterLoadBalancerMockedClass* cb, const string& table_id)
      : cb_(cb), blacklist_(cb->blacklist_),
        leader_blacklist_(cb->leader_blacklist_),
        ts_descs_(cb->ts_descs_),
        affinitized_zones_(cb->affinitized_zones_),
        tablet_map_(cb->tablet_map_),
        table_map_(cb->table_map_),
        replication_info_(cb->replication_info_),
        pending_add_replica_tasks_(cb->pending_add_replica_tasks_),
        pending_remove_replica_tasks_(cb->pending_remove_replica_tasks_),
        pending_stepdown_leader_tasks_(cb->pending_stepdown_leader_tasks_) {
    scoped_refptr<TableInfo> table(new TableInfo(table_id));
    vector<scoped_refptr<TabletInfo>> tablets;

    // Generate 12 tablets total: 4 splits and 3 replicas each.
    vector<string> splits = {"a", "b", "c"};
    const int num_replicas = 3;
    total_num_tablets_ = num_replicas * (splits.size() + 1);

    CreateTable(splits, num_replicas, false, table.get(), &tablets);

    tablets_ = std::move(tablets);

    table_map_[table_id] = table;
    cur_table_uuid_ = table_id;
  }

  void TestAlgorithm() {
    // Assign them initially only to the first three TSs.
    std::shared_ptr<TSDescriptor> ts0 = SetupTS("0000", "a");
    std::shared_ptr<TSDescriptor> ts1 = SetupTS("1111", "b");
    std::shared_ptr<TSDescriptor> ts2 = SetupTS("2222", "c");

    std::shared_ptr<TSDescriptor> ts1_a = SetupTS("1111", "a");
    std::shared_ptr<TSDescriptor> ts2_a = SetupTS("2222", "a");

    TSDescriptorVector ts_descs_multi_az = {ts0, ts1, ts2};
    TSDescriptorVector ts_descs_single_az = {ts0, ts1_a, ts2_a};

    PrepareTestState(ts_descs_multi_az);
    TestNoPlacement();

    PrepareTestState(ts_descs_multi_az);
    TestWithPlacement();

    PrepareTestState(ts_descs_multi_az);
    TestWithMissingPlacement();

    PrepareTestState(ts_descs_multi_az);
    TestOverReplication();

    PrepareTestState(ts_descs_multi_az);
    TestWithBlacklist();

    PrepareTestState(ts_descs_multi_az);
    TestWithMissingTabletServers();

    PrepareTestState(ts_descs_multi_az);
    TestMovingMultipleTabletsFromSameServer();

    PrepareTestState(ts_descs_multi_az);
    TestWithMissingPlacementAndLoadImbalance();

    PrepareTestState(ts_descs_multi_az);
    TestBalancingLeaders();

    PrepareTestState(ts_descs_single_az);
    TestMissingPlacementSingleAz();

    gflags::SetCommandLineOption("leader_balance_threshold", "2");
    PrepareTestState(ts_descs_multi_az);
    TestBalancingLeadersWithThreshold();

    PrepareTestState(ts_descs_multi_az);
    TestLeaderOverReplication();

    gflags::SetCommandLineOption("leader_balance_threshold", "0");
    PrepareTestState(ts_descs_multi_az);
    TestLeaderBlacklist();
  }

 protected:
  Status AnalyzeTablets() NO_THREAD_SAFETY_ANALYSIS /* don't need locks for mock class  */ {
    cb_->GetAllReportedDescriptors(&cb_->global_state_->ts_descs_);
    cb_->InitializeTSDescriptors();
    return cb_->AnalyzeTabletsUnlocked(cur_table_uuid_);
  }

  void ResetState() {
    cb_->global_state_ = std::make_unique<GlobalLoadState>();
    cb_->ResetTableStatePtr(cur_table_uuid_, nullptr);
  }

  Result<bool> HandleLeaderMoves(
      TabletId* out_tablet_id, TabletServerId* out_from_ts, TabletServerId* out_to_ts)
      NO_THREAD_SAFETY_ANALYSIS /* disabling for controlled test */ {
    return cb_->HandleLeaderMoves(out_tablet_id, out_from_ts, out_to_ts);
  }

  void TestLeaderBlacklist() {
    LOG(INFO) << "Testing moving overloaded leaders";
    // Move leaders of tablet i to ts i%3.
    int i = 0;
    for (const auto& tablet : tablets_) {
      MoveTabletLeader(tablet.get(), ts_descs_[i%3]);
      i++;
    }
    LOG(INFO) << "Leader distribution: 2 1 1";

    ASSERT_OK(AnalyzeTablets());

    // Leader blacklist ts2
    leader_blacklist_.add_hosts()->set_host(ts_descs_[2]->permanent_uuid());
    LOG(INFO) << "Leader distribution: 2 1 1. Leader Blacklist: ts2";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    string placeholder, tablet_id_1, tablet_id_2, expected_tablet_id,
        expected_from_ts, expected_to_ts;

    // With ts2 leader blacklisted, the leader on ts2 should be moved to ts1.
    expected_from_ts = ts_descs_[2]->permanent_uuid();
    expected_to_ts = ts_descs_[1]->permanent_uuid();
    TestMoveLeader(&tablet_id_1, expected_from_ts, expected_to_ts);
    ASSERT_EQ(tablet_id_1, tablets_[2].get()->id());
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Move 1 leader to ts1.
    MoveTabletLeader(tablets_[2].get(), ts_descs_[1]);
    LOG(INFO) << "Leader distribution: 2 2 0";

    // Clear leader blacklist.
    leader_blacklist_.Clear();
    cb_->state_->leader_blacklisted_servers_.clear();
    LOG(INFO) << "Leader distribution: 2 2 0. Leader Blacklist cleared.";

    ASSERT_OK(AnalyzeTablets());

    // With ts2 no more leader blacklisted, a leader on ts0 or ts1 should be moved to ts2.
    expected_from_ts = "";
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestMoveLeader(&placeholder, expected_from_ts, expected_to_ts);
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Move 1 leader to ts2.
    for (const auto& tablet : tablets_) {
      if (tablet.get()->id() == placeholder) {
        MoveTabletLeader(tablet.get(), ts_descs_[2]);
        break;
      }
    }
    LOG(INFO) << "Leader distribution: 2 1 1 -OR- 1 2 1";
  }

  void TestWithBlacklist() {
    LOG(INFO) << "Testing with tablet servers with blacklist";
    // Setup cluster config.
    SetupClusterConfig({"a", "b", "c"}, &replication_info_);

    // Blacklist the first host in AZ "a".
    blacklist_.add_hosts()->set_host(ts_descs_[0]->permanent_uuid());

    // Add two tablet servers in AZ "a" and one in AZ "b".
    ts_descs_.push_back(SetupTS("3333", "b"));
    ts_descs_.push_back(SetupTS("4444", "a"));
    ts_descs_.push_back(SetupTS("5555", "a"));

    // Blacklist the first new tablet server in AZ "a" so we show it isn't picked.
    blacklist_.add_hosts()->set_host(ts_descs_[4]->permanent_uuid());

    // Prepare the data.
    ASSERT_OK(AnalyzeTablets());

    string placeholder, expected_from_ts, expected_to_ts;
    // Expecting that we move load from ts0 which is blacklisted, to ts5. This is because ts4 is
    // also blacklisted and ts0 has a valid placement, so we try to find a server in the same
    // placement.
    expected_from_ts = ts_descs_[0]->permanent_uuid();
    expected_to_ts = ts_descs_[5]->permanent_uuid();
    for (const auto& tablet : tablets_) {
      TestAddLoad(tablet->tablet_id(), expected_from_ts, expected_to_ts);
    }

    // There is some opportunity to equalize load across the remaining servers also. However,
    // we cannot do so until the next run since all tablets have just been moved once.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));

    // Move tablets off ts0 to ts5.
    for (const auto& tablet : tablets_) {
      RemoveReplica(tablet.get(), ts_descs_[0]);
      AddRunningReplica(tablet.get(), ts_descs_[5]);
    }

    // Reset the load state and recompute.
    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // Now that we have reinitialized for the next run, we can try to equalize load across the
    // remaining servers. Our load on non-blacklisted servers is: ts1:4, ts2:4, ts3:0, ts5:4. Of
    // this, we can only balance from ts1 to ts3, as they are in the same AZ.
    expected_from_ts = ts_descs_[1]->permanent_uuid();
    expected_to_ts = ts_descs_[3]->permanent_uuid();
    TestAddLoad(placeholder, expected_from_ts, expected_to_ts);
    TestAddLoad(placeholder, expected_from_ts, expected_to_ts);
    // Now we should have no more tablets we are able to move.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));
  }

  void TestOverReplication() NO_THREAD_SAFETY_ANALYSIS /* disabling for controlled test */ {
    LOG(INFO) << "Testing with tablet servers with over-replication";
    // Setup cluster config.
    SetupClusterConfig({"a"}, &replication_info_);

    // Remove the 2 tablet peers that are wrongly placed and assign a new one that is properly
    // placed.
    for (const auto& tablet : tablets_) {
      std::shared_ptr<TabletInfo::ReplicaMap> replica_map =
        std::const_pointer_cast<TabletInfo::ReplicaMap>(tablet->GetReplicaLocations());
      replica_map->erase(ts_descs_[1]->permanent_uuid());
      replica_map->erase(ts_descs_[2]->permanent_uuid());
      tablet->SetReplicaLocations(replica_map);
    }
    // Remove the two wrong tablet servers from the list.
    ts_descs_.pop_back();
    ts_descs_.pop_back();
    // Add a tablet server with proper placement and peer it to all tablets. Now all tablets
    // should have 2 peers.
    // Using empty ts_uuid here since our mocked PendingTasksUnlocked only returns empty ts_uuids.
    ts_descs_.push_back(SetupTS("", "a"));
    for (auto tablet : tablets_) {
      AddRunningReplica(tablet.get(), ts_descs_[1]);
    }

    // Setup some new tablet servers as replicas in both the same and wrong AZs and confirm that
    // the algorithms work as expected.
    ts_descs_.push_back(SetupTS("2222", "WRONG"));
    ts_descs_.push_back(SetupTS("3333", "a"));
    ts_descs_.push_back(SetupTS("4444", "a"));
    // We'll keep this as empty, for use as a sync for tablets.
    ts_descs_.push_back(SetupTS("5555", "a"));

    // Over-replicate a tablet, with a wrongly placed replica.
    AddRunningReplica(tablets_[0].get(), ts_descs_[2]);
    AddRunningReplica(tablets_[0].get(), ts_descs_[3]);

    // Over-replicate a tablet, with properly placed replicas.
    AddRunningReplica(tablets_[1].get(), ts_descs_[3]);
    AddRunningReplica(tablets_[1].get(), ts_descs_[4]);

    // Bring a tablet to proper replication, but with a wrongly placed replica.
    AddRunningReplica(tablets_[2].get(), ts_descs_[2]);

    // Bring a tablet to proper replication, with all replicas in the correct placement.
    AddRunningReplica(tablets_[3].get(), ts_descs_[4]);

    ASSERT_OK(AnalyzeTablets());

    // Add all tablets to the list of tablets with a pending add operation and verify that calling
    // HandleAddReplicas fails because all the tablets have a pending add operation.
    pending_add_replica_tasks_.clear();
    for (const auto& tablet : tablets_) {
      pending_add_replica_tasks_.push_back(tablet->id());
    }
    int count = 0;
    int pending_add_count = 0;
    ASSERT_OK(cb_->CountPendingTasksUnlocked(cur_table_uuid_, &pending_add_count, &count, &count));
    ASSERT_EQ(pending_add_count, pending_add_replica_tasks_.size());
    ASSERT_OK(AnalyzeTablets());
    string placeholder, tablet_id;
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&tablet_id, &placeholder, &placeholder)));

    // Clear pending_add_replica_tasks_ and reset the state of ClusterLoadBalancer.
    pending_add_replica_tasks_.clear();
    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // Check that if adding replicas, we'll notice the wrong placement and adjust it.
    string expected_tablet_id, expected_from_ts, expected_to_ts;
    expected_tablet_id = tablets_[2]->tablet_id();
    expected_from_ts = ts_descs_[2]->permanent_uuid();
    expected_to_ts = ts_descs_[5]->permanent_uuid();
    TestAddLoad(expected_tablet_id, expected_from_ts, expected_to_ts);

    // Add all tablets to the list of tablets with a pending remove operation and verify that
    // calling HandleRemoveReplicas fails because all the tablets have a pending remove operation.
    pending_remove_replica_tasks_.clear();
    for (const auto& tablet : tablets_) {
      pending_remove_replica_tasks_.push_back(tablet->id());
    }
    int pending_remove_count = 0;
    ASSERT_OK(
        cb_->CountPendingTasksUnlocked(cur_table_uuid_, &count, &pending_remove_count, &count));
    ASSERT_EQ(pending_remove_count, pending_remove_replica_tasks_.size());
    ASSERT_OK(AnalyzeTablets());
    ASSERT_FALSE(ASSERT_RESULT(cb_->HandleRemoveReplicas(&tablet_id, &placeholder)));

    // Clear pending_remove_replica_tasks_ and reset the state of ClusterLoadBalancer.
    pending_remove_replica_tasks_.clear();
    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // Check that removing replicas, we take out the wrong placement one first.
    expected_tablet_id = tablets_[0]->tablet_id();
    expected_from_ts = ts_descs_[2]->permanent_uuid();
    TestRemoveLoad(expected_tablet_id, expected_from_ts);
    // Check that trying to remove another replica, will take out one from the
    // last over-replicated set. Both ts0 and ts1 are on the same load,
    // so we'll pick the highest uuid one to remove.
    expected_tablet_id = tablets_[1]->tablet_id();
    expected_from_ts = ts_descs_[1]->permanent_uuid();
    TestRemoveLoad(expected_tablet_id, expected_from_ts);
    // Check that trying to remove another replica will fail, as we have no more over-replication.
    ASSERT_FALSE(ASSERT_RESULT(cb_->HandleRemoveReplicas(&placeholder, &placeholder)));
  }

  void TestLeaderOverReplication() {
    LOG(INFO) << "Skip leader TS being picked with over-replication.";
    replication_info_.mutable_live_replicas()->set_num_replicas(kDefaultNumReplicas);

    // Create one more TS.
    ts_descs_.push_back(SetupTS("3333", "a"));

    const auto& tablet = tablets_[0].get();
    // Over-replicate first tablet, with one extra replica.
    AddRunningReplica(tablet, ts_descs_[3]);

    // Move leader to first replica in the list (and will be most-loaded).
    MoveTabletLeader(tablet, ts_descs_[2]);

    // Load up data.
    ASSERT_OK(AnalyzeTablets());

    // Ensure the tablet is picked.
    TestRemoveLoad(tablets_[0]->tablet_id(), "");
  }

  void TestWithMissingPlacement() NO_THREAD_SAFETY_ANALYSIS /* disabling for controlled test */ {
    LOG(INFO) << "Testing with tablet servers missing placement information";
    // Setup cluster level placement to multiple AZs.
    SetupClusterConfig({"a", "b", "c"}, &replication_info_);

    // Remove the only tablet peer from AZ "c".
    for (const auto& tablet : tablets_) {
      std::shared_ptr<TabletInfo::ReplicaMap> replica_map =
        std::const_pointer_cast<TabletInfo::ReplicaMap>(tablet->GetReplicaLocations());
      replica_map->erase(ts_descs_[2]->permanent_uuid());
      tablet->SetReplicaLocations(replica_map);
    }
    // Remove the tablet server from the list.
    ts_descs_.pop_back();

    // Add some more servers in that same AZ, so we get to pick among them.
    ts_descs_.push_back(SetupTS("2new", "c"));
    ts_descs_.push_back(SetupTS("3new", "c"));

    // Load up data.
    ASSERT_OK(AnalyzeTablets());

    // First we'll fill up the missing placements for all 4 tablets.
    string placeholder;
    string expected_to_ts, expected_from_ts;
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestAddLoad(placeholder, placeholder, expected_to_ts);
    expected_to_ts = ts_descs_[3]->permanent_uuid();
    TestAddLoad(placeholder, placeholder, expected_to_ts);
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestAddLoad(placeholder, placeholder, expected_to_ts);
    expected_to_ts = ts_descs_[3]->permanent_uuid();
    TestAddLoad(placeholder, placeholder, expected_to_ts);
    // Now registered load should be 4,4,2,2. However, we cannot move load from AZ "a" and "b" to
    // the servers in AZ "c", under normal load conditions, so we should fail the call.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));
  }

  void TestWithPlacement() {
    LOG(INFO) << "Testing with placement information";
    // Setup cluster level placement to the same 3 AZs as our tablet servers.
    SetupClusterConfig({"a", "b", "c"}, &replication_info_);

    // Add three TSs, one in wrong AZ, two in right AZs.
    ts_descs_.push_back(SetupTS("3333", "WRONG"));
    ts_descs_.push_back(SetupTS("4444", "a"));
    ts_descs_.push_back(SetupTS("5555", "a"));

    // Analyze the tablets into the internal state.
    ASSERT_OK(AnalyzeTablets());

    // Check some base expectations for balanced cluster.
    ASSERT_EQ(0, cb_->get_total_over_replication());
    ASSERT_EQ(0, cb_->get_total_starting_tablets());
    ASSERT_EQ(total_num_tablets_, cb_->get_total_running_tablets());

    // Equal load across the first three TSs, but placement dictates we can only move from ts0 to
    // ts4 or ts5. We should pick the lowest uuid one, which is ts4.
    string placeholder;
    string expected_from_ts = ts_descs_[0]->permanent_uuid();
    string expected_to_ts = ts_descs_[4]->permanent_uuid();
    TestAddLoad(placeholder, expected_from_ts, expected_to_ts);

    // Recompute and expect to move to next least loaded TS, which matches placement, which should
    // be ts5 now. Load should still move from the only TS in the correct placement, which is ts0.
    expected_from_ts = ts_descs_[0]->permanent_uuid();
    expected_to_ts = ts_descs_[5]->permanent_uuid();
    TestAddLoad(placeholder, expected_from_ts, expected_to_ts);
  }

  void TestNoPlacement() {
    LOG(INFO) << "Testing with no placement information";
    PlacementInfoPB* cluster_placement = replication_info_.mutable_live_replicas();
    cluster_placement->set_num_replicas(kDefaultNumReplicas);
    // Analyze the tablets into the internal state.
    AnalyzeTablets();

    // Check some base expectations for balanced cluster.
    ASSERT_EQ(0, cb_->get_total_over_replication());
    ASSERT_EQ(0, cb_->get_total_starting_tablets());
    ASSERT_EQ(total_num_tablets_, cb_->get_total_running_tablets());

    // Add the fourth TS in there, set it in the same az as ts0.
    ts_descs_.push_back(SetupTS("3333", "a"));

    // Reset the load state and recompute.
    ResetState();
    ASSERT_OK(AnalyzeTablets());

    string placeholder;
    // Lowest load should be the last, empty TS.
    string expected_to_ts = ts_descs_[3]->permanent_uuid();
    // Equal load across the first three TSs. Picking the one with largest ID in string compare.
    string expected_from_ts = ts_descs_[2]->permanent_uuid();
    TestAddLoad(placeholder, expected_from_ts, expected_to_ts);

    // Perform another round on the updated in-memory load. The move should have made ts2 less
    // loaded, so next tablet should come from ts1 to ts3.
    expected_from_ts = ts_descs_[1]->permanent_uuid();
    TestAddLoad(placeholder, expected_from_ts, expected_to_ts);

    // One more round, finally expecting to move from ts0 to ts3.
    expected_from_ts = ts_descs_[0]->permanent_uuid();
    TestAddLoad(placeholder, expected_from_ts, expected_to_ts);

    // Final check on in-memory state after in-memory moves.
    ASSERT_EQ(total_num_tablets_ - 3, cb_->get_total_running_tablets());
    ASSERT_EQ(3, cb_->get_total_starting_tablets());
    ASSERT_EQ(0, cb_->get_total_over_replication());
  }

  void TestWithMissingTabletServers() {
    LOG(INFO) << "Testing with missing tablet servers";
    SetupClusterConfig({"a"}, &replication_info_);

    // Remove one of the needed tablet servers.
    ts_descs_.pop_back();

    // Analyze the tablets into the internal state.
    ASSERT_NOK(AnalyzeTablets());
  }

  void TestMovingMultipleTabletsFromSameServer() {
    LOG(INFO) << "Testing moving multiple tablets from the same tablet server";
    PlacementInfoPB *cluster_placement = replication_info_.mutable_live_replicas();
    cluster_placement->set_num_replicas(kDefaultNumReplicas);

    // Add three more tablet servers
    ts_descs_.push_back(SetupTS("3333", "a"));
    ts_descs_.push_back(SetupTS("4444", "a"));
    ts_descs_.push_back(SetupTS("5555", "a"));

    // Move 2 tablets from ts1 and ts2 each to ts3 and ts4, leaving ts0 with 4 tablets, ts1..4
    // with 2 tablets and ts5 with none.
    RemoveReplica(tablets_[0].get(), ts_descs_[1]);
    AddRunningReplica(tablets_[0].get(), ts_descs_[3]);
    RemoveReplica(tablets_[1].get(), ts_descs_[1]);
    AddRunningReplica(tablets_[1].get(), ts_descs_[3]);
    RemoveReplica(tablets_[0].get(), ts_descs_[2]);
    AddRunningReplica(tablets_[0].get(), ts_descs_[4]);
    RemoveReplica(tablets_[1].get(), ts_descs_[2]);
    AddRunningReplica(tablets_[1].get(), ts_descs_[4]);

    ASSERT_OK(AnalyzeTablets());

    // ENG-348: Check that 2 different tablets are moved from ts0 to ts5.
    // Since tablet 0 on ts0 is the leader, it won't be moved and tablet 1 and 2 will be instead.
    string expected_tablet_id, expected_from_ts, expected_to_ts;
    expected_from_ts = ts_descs_[0]->permanent_uuid();
    expected_to_ts = ts_descs_[5]->permanent_uuid();
    expected_tablet_id = tablets_[1]->tablet_id();
    TestAddLoad(expected_tablet_id, expected_from_ts, expected_to_ts);
    expected_tablet_id = tablets_[2]->tablet_id();
    TestAddLoad(expected_tablet_id, expected_from_ts, expected_to_ts);
  }

  void TestWithMissingPlacementAndLoadImbalance() {
    LOG(INFO) << "Testing with tablet servers missing placement and load imbalance";
    // Setup cluster level placement to multiple AZs.
    SetupClusterConfig({"a", "b", "c"}, &replication_info_);

    // Remove the only tablet peer from AZ "c".
    for (const auto& tablet : tablets_) {
      std::shared_ptr<TabletInfo::ReplicaMap> replica_map =
        std::const_pointer_cast<TabletInfo::ReplicaMap>(tablet->GetReplicaLocations());
      replica_map->erase(ts_descs_[2]->permanent_uuid());
      tablet->SetReplicaLocations(replica_map);
    }
    // Remove the tablet server from the list.
    ts_descs_.pop_back();

    // Add back 1 new server in that same AZ. So we should add missing placments to this new TS.
    ts_descs_.push_back(SetupTS("1new", "c"));

    // Load up data.
    ASSERT_OK(AnalyzeTablets());

    // First we'll fill up the missing placements for all 4 tablets.
    string placeholder;
    string expected_to_ts;
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestAddLoad(placeholder, placeholder, expected_to_ts);
    TestAddLoad(placeholder, placeholder, expected_to_ts);
    TestAddLoad(placeholder, placeholder, expected_to_ts);
    TestAddLoad(placeholder, placeholder, expected_to_ts);

    // Add yet 1 more server in that same AZ for some load-balancing.
    ts_descs_.push_back(SetupTS("2new", "c"));

    ASSERT_OK(AnalyzeTablets());

    // Since we have just filled up the missing placements for all 4 tablets, we cannot rebalance
    // the tablets to the second new TS until the next run.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));

    // Add the missing placements to the first new TS.
    AddRunningReplica(tablets_[0].get(), ts_descs_[2]);
    AddRunningReplica(tablets_[1].get(), ts_descs_[2]);
    AddRunningReplica(tablets_[2].get(), ts_descs_[2]);
    AddRunningReplica(tablets_[3].get(), ts_descs_[2]);

    // Reset the load state and recompute.
    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // Now we should be able to move 2 tablets to the second new TS.
    expected_to_ts = ts_descs_[3]->permanent_uuid();
    TestAddLoad(placeholder, placeholder, expected_to_ts);
    TestAddLoad(placeholder, placeholder, expected_to_ts);

    // And the load should now be balanced so no more move is expected.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));
  }

  void TestBalancingLeaders() NO_THREAD_SAFETY_ANALYSIS /* disabling for controlled test */ {
    LOG(INFO) << "Testing moving overloaded leaders";
    // Move all leaders to ts0.
    for (const auto& tablet : tablets_) {
      MoveTabletLeader(tablet.get(), ts_descs_[0]);
    }
    LOG(INFO) << "Leader distribution: 4 0 0";

    ASSERT_OK(AnalyzeTablets());

    // Only 2 leaders should be moved off from ts0, 1 to ts2 and then another to ts1.
    string placeholder, tablet_id_1, tablet_id_2, expected_tablet_id,
        expected_from_ts, expected_to_ts;
    expected_from_ts = ts_descs_[0]->permanent_uuid();
    expected_to_ts = ts_descs_[1]->permanent_uuid();
    TestMoveLeader(&tablet_id_1, expected_from_ts, expected_to_ts);
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestMoveLeader(&tablet_id_2, expected_from_ts, expected_to_ts);
    // Ideally, we want to assert the leaders expected to be moved. However, since the tablets
    // are stored in an unordered_map in catalog manager and all leaders have same load gap, the
    // leaders being moved are not deterministic. So just make sure we are not moving the same
    // leader twice.
    ASSERT_NE(tablet_id_1, tablet_id_2);
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Move 1 leader to ts1.
    MoveTabletLeader(tablets_[0].get(), ts_descs_[1]);
    LOG(INFO) << "Leader distribution: 3 1 0";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // Only 1 leader should be moved off from ts0 to ts2.
    expected_from_ts = ts_descs_[0]->permanent_uuid();
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestMoveLeader(&placeholder, expected_from_ts, expected_to_ts);
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Move 1 more leader to ts1 and blacklist ts0
    MoveTabletLeader(tablets_[1].get(), ts_descs_[1]);
    blacklist_.add_hosts()->set_host(ts_descs_[0]->permanent_uuid());
    LOG(INFO) << "Leader distribution: 2 2 0. Blacklist: ts0";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // With ts0 blacklisted, the 2 leaders on ts0 should be moved to some undetermined servers.
    // ts1 still has 2 leaders and ts2 has 0 so 1 leader should be moved to ts2.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));
    ASSERT_FALSE(ASSERT_RESULT(cb_->HandleRemoveReplicas(&placeholder, &placeholder)));
    expected_from_ts = ts_descs_[1]->permanent_uuid();
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestMoveLeader(&placeholder, expected_from_ts, expected_to_ts);
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Clear the blacklist.
    blacklist_.Clear();
    LOG(INFO) << "Leader distribution: 2 2 0. Blacklist cleared.";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // Only 1 tablets should be moved off from ts1 to ts2.
    expected_from_ts = ts_descs_[1]->permanent_uuid();
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestMoveLeader(&placeholder, expected_from_ts, expected_to_ts);
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Move 1 leader from ts1 to ts2.
    MoveTabletLeader(tablets_[1].get(), ts_descs_[2]);
    LOG(INFO) << "Leader distribution: 2 1 1";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // The distribution is as balanced as it can be so there shouldn't be any move.
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));
  }

  void TestBalancingLeadersWithThreshold() {
    LOG(INFO) << "Testing moving overloaded leaders with threshold = 2";
    // Move all leaders to ts0.
    for (const auto& tablet : tablets_) {
      MoveTabletLeader(tablet.get(), ts_descs_[0]);
    }
    LOG(INFO) << "Leader distribution: 4 0 0";

    ASSERT_OK(AnalyzeTablets());

    // Only 2 leaders should be moved off from ts0 to ts1 and ts2 each.
    string placeholder, tablet_id_1, tablet_id_2, expected_tablet_id,
        expected_from_ts, expected_to_ts;
    expected_from_ts = ts_descs_[0]->permanent_uuid();
    expected_to_ts = ts_descs_[1]->permanent_uuid();
    TestMoveLeader(&tablet_id_1, expected_from_ts, expected_to_ts);
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestMoveLeader(&tablet_id_2, expected_from_ts, expected_to_ts);
    // Ideally, we want to assert the leaders expected to be moved. However, since the tablets
    // are stored in an unordered_map in catalog manager and all leaders have same load gap, the
    // leaders being moved are not deterministic. So just make sure we are not moving the same
    // leader twice.
    ASSERT_NE(tablet_id_1, tablet_id_2);
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Move 1 leader to ts1.
    MoveTabletLeader(tablets_[0].get(), ts_descs_[1]);
    LOG(INFO) << "Leader distribution: 3 1 0";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // Only 1 leader should be moved off from ts0 to ts2.
    expected_from_ts = ts_descs_[0]->permanent_uuid();
    expected_to_ts = ts_descs_[2]->permanent_uuid();
    TestMoveLeader(&placeholder, expected_from_ts, expected_to_ts);
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Move 1 more leader to ts1 and blacklist ts0
    MoveTabletLeader(tablets_[1].get(), ts_descs_[1]);
    blacklist_.add_hosts()->set_host(ts_descs_[0]->permanent_uuid());
    LOG(INFO) << "Leader distribution: 2 2 0. Blacklist: ts0";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Clear the blacklist.
    blacklist_.Clear();
    LOG(INFO) << "Leader distribution: 2 2 0. Blacklist cleared.";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // Again all tablet servers have leaders below threshold so no move is expected.
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    // Move 1 leader from ts1 to ts2.
    MoveTabletLeader(tablets_[1].get(), ts_descs_[2]);
    LOG(INFO) << "Leader distribution: 2 1 1";

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    // The distribution is as balanced as it can be so there shouldn't be any move.
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));
  }

  void TestMissingPlacementSingleAz() {
    LOG(INFO) << "Testing single az deployment where min_num_replicas different from num_replicas";
    // Setup cluster level placement to single AZ.
    SetupClusterConfig({"a"}, &replication_info_);

    // Under-replicate tablets 0, 1, 2.
    RemoveReplica(tablets_[0].get(), ts_descs_[0]);
    RemoveReplica(tablets_[1].get(), ts_descs_[1]);
    RemoveReplica(tablets_[2].get(), ts_descs_[2]);

    ResetState();
    ASSERT_OK(AnalyzeTablets());

    string placeholder, expected_tablet_id, expected_from_ts, expected_to_ts;

    // Make sure a replica is added for tablet 0 to ts 0.
    expected_tablet_id = tablets_[0]->tablet_id();
    expected_to_ts = ts_descs_[0]->permanent_uuid();

    TestAddLoad(expected_tablet_id,  placeholder, expected_to_ts);

    // Make sure a replica is added for tablet 1 to ts 1.
    expected_tablet_id = tablets_[1]->tablet_id();
    expected_to_ts = ts_descs_[1]->permanent_uuid();

    TestAddLoad(expected_tablet_id,  placeholder, expected_to_ts);

    // Make sure a replica is added for tablet 2 to ts 2.
    expected_tablet_id = tablets_[2]->tablet_id();
    expected_to_ts = ts_descs_[2]->permanent_uuid();

    TestAddLoad(expected_tablet_id,  placeholder, expected_to_ts);

    // Everything is normal now, load balancer shouldn't do anything.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));
  }

  // Methods to prepare the state of the current test.
  void PrepareTestState(const TSDescriptorVector& ts_descs) {
    // Clear old state.
    ResetState();
    replication_info_.Clear();
    blacklist_.Clear();
    leader_blacklist_.Clear();
    tablet_map_.clear();
    TSDescriptorVector old_ts_descs;
    old_ts_descs.swap(ts_descs_);
    affinitized_zones_.clear();

    // Set TS desc.
    ts_descs_ = ts_descs;

    // Reset the tablet map tablets.
    for (const auto& tablet : tablets_) {
      tablet_map_[tablet->tablet_id()] = tablet;
    }

    // Prepare the replicas.
    tablet::RaftGroupStatePB state = tablet::RUNNING;
    for (int i = 0; i < tablets_.size(); ++i) {
      auto replica_map = std::make_shared<TabletInfo::ReplicaMap>();
      for (int j = 0; j < ts_descs_.size(); ++j) {
        TabletReplica replica;
        auto ts_desc = ts_descs_[j];
        bool is_leader = i % ts_descs_.size() == j;
        consensus::RaftPeerPB::Role role = is_leader ?
            consensus::RaftPeerPB::LEADER :
            consensus::RaftPeerPB::FOLLOWER;
        NewReplica(ts_desc.get(), state, role , &replica);
        InsertOrDie(replica_map.get(), ts_desc->permanent_uuid(), replica);
      }
      // Set the replica locations directly into the tablet map.
      tablet_map_[tablets_[i]->tablet_id()]->SetReplicaLocations(replica_map);
    }
  }

  // Tester methods that actually do the calls and asserts.
  void TestRemoveLoad(const string& expected_tablet_id, const string& expected_from_ts)
    NO_THREAD_SAFETY_ANALYSIS /* disabling for controlled test */ {
    string tablet_id, from_ts;
    ASSERT_TRUE(ASSERT_RESULT(cb_->HandleRemoveReplicas(&tablet_id, &from_ts)));
    if (!expected_tablet_id.empty()) {
      ASSERT_EQ(expected_tablet_id, tablet_id);
    }
    if (!expected_from_ts.empty()) {
      ASSERT_EQ(expected_from_ts, from_ts);
    }
  }

  void TestAddLoad(const string& expected_tablet_id,
                   const string& expected_from_ts,
                   const string& expected_to_ts) {
    string tablet_id, from_ts, to_ts;
    ASSERT_TRUE(ASSERT_RESULT(HandleAddReplicas(&tablet_id, &from_ts, &to_ts)));
    if (!expected_tablet_id.empty()) {
      ASSERT_EQ(expected_tablet_id, tablet_id);
    }
    if (!expected_from_ts.empty()) {
      ASSERT_EQ(expected_from_ts, from_ts);
    }
    if (!expected_to_ts.empty()) {
      ASSERT_EQ(expected_to_ts, to_ts);
    }
  }

  void TestMoveLeader(string* tablet_id,
                      const string& expected_from_ts,
                      const string& expected_to_ts) {
    string from_ts, to_ts;
    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(tablet_id, &from_ts, &to_ts)));
    if (!expected_from_ts.empty()) {
      ASSERT_EQ(expected_from_ts, from_ts);
    }
    if (!expected_to_ts.empty()) {
      ASSERT_EQ(expected_to_ts, to_ts);
    }
  }

  void AddRunningReplica(TabletInfo* tablet, std::shared_ptr<TSDescriptor> ts_desc,
                         bool is_live = true) {
    std::shared_ptr<TabletInfo::ReplicaMap> replicas =
      std::const_pointer_cast<TabletInfo::ReplicaMap>(tablet->GetReplicaLocations());

    TabletReplica replica;
    NewReplica(ts_desc.get(), tablet::RaftGroupStatePB::RUNNING,
               consensus::RaftPeerPB::FOLLOWER, &replica);
    InsertOrDie(replicas.get(), ts_desc->permanent_uuid(), replica);
    tablet->SetReplicaLocations(replicas);
  }

  void RemoveReplica(TabletInfo* tablet, std::shared_ptr<TSDescriptor> ts_desc) {
    std::shared_ptr<TabletInfo::ReplicaMap> replicas =
      std::const_pointer_cast<TabletInfo::ReplicaMap>(tablet->GetReplicaLocations());
    int before_size = replicas->size();
    replicas->erase(ts_desc->permanent_uuid());
    ASSERT_TRUE(before_size > replicas->size());
    tablet->SetReplicaLocations(replicas);
  }

  void MoveTabletLeader(TabletInfo* tablet, std::shared_ptr<TSDescriptor> ts_desc) {
    std::shared_ptr<TabletInfo::ReplicaMap> replicas =
      std::const_pointer_cast<TabletInfo::ReplicaMap>(tablet->GetReplicaLocations());
    for (auto& replica : *replicas) {
      if (replica.second.ts_desc->permanent_uuid() == ts_desc->permanent_uuid()) {
        replica.second.role = consensus::RaftPeerPB::LEADER;
      } else {
        replica.second.role = consensus::RaftPeerPB::FOLLOWER;
      }
    }
    tablet->SetReplicaLocations(replicas);
  }

  // Clear the tablets_added_ field from the state, used for testing.
  void ClearTabletsAddedForTest() {
    cb_->state_->tablets_added_.clear();
  }

  Result<bool> HandleAddReplicas(
      TabletId* out_tablet_id, TabletServerId* out_from_ts, TabletServerId* out_to_ts)
      NO_THREAD_SAFETY_ANALYSIS /* disabling for controlled test */ {
    return cb_->HandleAddReplicas(out_tablet_id, out_from_ts, out_to_ts);
  }

  ClusterLoadBalancerMockedClass* cb_;

  int total_num_tablets_;
  vector<scoped_refptr<TabletInfo>> tablets_;
  BlacklistPB& blacklist_;
  BlacklistPB& leader_blacklist_;
  TableId cur_table_uuid_;
  TSDescriptorVector& ts_descs_;
  AffinitizedZonesSet& affinitized_zones_;
  TabletInfoMap& tablet_map_;
  TableInfoMap& table_map_;
  ReplicationInfoPB& replication_info_;
  vector<TabletId>& pending_add_replica_tasks_;
  vector<TabletId>& pending_remove_replica_tasks_;
  vector<TabletId>& pending_stepdown_leader_tasks_;
};

} // namespace master
} // namespace yb

#endif // YB_MASTER_CATALOG_MANAGER_TEST_BASE_H
