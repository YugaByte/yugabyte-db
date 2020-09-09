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

#include "../../src/yb/master/catalog_manager-test_base.h"

namespace yb {
namespace master {
namespace enterprise {

using std::shared_ptr;
using std::make_shared;

const string read_only_placement_uuid = "read_only";

std::shared_ptr<TSDescriptor> SetupTSEnt(const string& uuid,
                                         const string& az,
                                         const string& placement_uuid) {
  NodeInstancePB node;
  node.set_permanent_uuid(uuid);

  TSRegistrationPB reg;
  // Set the placement uuid field for read_only clusters.
  reg.mutable_common()->set_placement_uuid(placement_uuid);
  // Fake host:port combo, with uuid as host, for ease of testing.
  auto hp = reg.mutable_common()->add_private_rpc_addresses();
  hp->set_host(uuid);
  // Same cloud info as cluster config, with modifyable AZ.
  auto ci = reg.mutable_common()->mutable_cloud_info();
  ci->set_placement_cloud(default_cloud);
  ci->set_placement_region(default_region);
  ci->set_placement_zone(az);

  std::shared_ptr<TSDescriptor> ts(new TSDescriptor(node.permanent_uuid()));
  CHECK_OK(ts->Register(node, reg, CloudInfoPB(), nullptr));

  return ts;
}

void SetupClusterConfigEnt(const vector<string>& az_list,
                           const vector<string>& read_only_list,
                           const vector<string>& affinitized_leader_list,
                           ReplicationInfoPB* replication_info) {
  PlacementInfoPB* placement_info = replication_info->mutable_live_replicas();
  placement_info->set_num_replicas(kNumReplicas);

  for (const string& az : az_list) {
    auto pb = placement_info->add_placement_blocks();
    pb->mutable_cloud_info()->set_placement_cloud(default_cloud);
    pb->mutable_cloud_info()->set_placement_region(default_region);
    pb->mutable_cloud_info()->set_placement_zone(az);
    pb->set_min_num_replicas(1);
  }

  if (!read_only_list.empty()) {
    placement_info = replication_info->add_read_replicas();
    placement_info->set_num_replicas(1);
  }

  for (const string& read_only_az : read_only_list) {
    auto pb = placement_info->add_placement_blocks();
    pb->mutable_cloud_info()->set_placement_cloud(default_cloud);
    pb->mutable_cloud_info()->set_placement_region(default_region);
    pb->mutable_cloud_info()->set_placement_zone(read_only_az);
    placement_info->set_placement_uuid(read_only_placement_uuid);
    pb->set_min_num_replicas(1);
  }

  for (const string& affinitized_az : affinitized_leader_list) {
    CloudInfoPB* ci = replication_info->add_affinitized_leaders();
    ci->set_placement_cloud(default_cloud);
    ci->set_placement_region(default_region);
    ci->set_placement_zone(affinitized_az);
  }
}

class TestLoadBalancerEnterprise : public TestLoadBalancerBase<ClusterLoadBalancerMocked> {
 public:
  TestLoadBalancerEnterprise(ClusterLoadBalancerMocked* cb, const string& table_id) :
      TestLoadBalancerBase<ClusterLoadBalancerMocked>(cb, table_id) {}

  void TestAlgorithm() {
    TestLoadBalancerBase<ClusterLoadBalancerMocked>::TestAlgorithm();

    shared_ptr<TSDescriptor> ts0 =
        SetupTSEnt("0000", "a", "" /* placement_uuid */);
    shared_ptr<TSDescriptor> ts1 = SetupTSEnt("1111", "b", "");
    shared_ptr<TSDescriptor> ts2 = SetupTSEnt("2222", "c", "");

    TSDescriptorVector ts_descs = {ts0, ts1, ts2};

    PrepareTestState(ts_descs);
    PrepareAffinitizedLeaders({"a"} /* affinitized zones */);
    TestAlreadyBalancedAffinitizedLeaders();

    PrepareTestState(ts_descs);
    PrepareAffinitizedLeaders({"a"});
    TestBalancingOneAffinitizedLeader();

    PrepareTestState(ts_descs);
    PrepareAffinitizedLeaders({"b", "c"});
    TestBalancingTwoAffinitizedLeaders();

    PrepareTestState(ts_descs);
    TestReadOnlyLoadBalancing();

    PrepareTestState(ts_descs);
    TestLeaderBalancingWithReadOnly();
  }

  void TestAlreadyBalancedAffinitizedLeaders() {
    LOG(INFO) << "Starting TestAlreadyBalancedAffinitizedLeaders";
    // Move all leaders to ts0.
    for (const auto& tablet : tablets_) {
      MoveTabletLeader(tablet.get(), ts_descs_[0]);
    }
    LOG(INFO) << "Leader distribution: 4 0 0";

    AnalyzeTablets();
    string placeholder;
    // Only the affinitized zone contains tablet leaders, should be no movement.
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));
    LOG(INFO) << "Finishing TestAlreadyBalancedAffinitizedLeaders";
  }

  void TestBalancingOneAffinitizedLeader() {
    LOG(INFO) << "Starting TestBalancingOneAffinitizedLeader";
    int i = 0;
    for (const auto& tablet : tablets_) {
      MoveTabletLeader(tablet.get(), ts_descs_[(i % 2) + 1]);
      i++;
    }
    LOG(INFO) << "Leader distribution: 0 2 2";

    AnalyzeTablets();

    std::map<string, int> from_count;
    std::unordered_set<string> tablets_moved;

    string placeholder, tablet_id, from_ts, to_ts, ts_0, ts_1, ts_2;
    ts_0 = ts_descs_[0]->permanent_uuid();
    ts_1 = ts_descs_[1]->permanent_uuid();
    ts_2 = ts_descs_[2]->permanent_uuid();

    from_count[ts_0] = 0;
    from_count[ts_1] = 0;
    from_count[ts_2] = 0;

    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(&tablet_id, &from_ts, &to_ts)));
    ASSERT_EQ(to_ts, ts_0);
    tablets_moved.insert(tablet_id);
    from_count[from_ts]++;

    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(&tablet_id, &from_ts, &to_ts)));
    ASSERT_EQ(to_ts, ts_0);
    tablets_moved.insert(tablet_id);
    from_count[from_ts]++;

    LOG(INFO) << "Leader distribution: 2 1 1";
    // Make sure one tablet moved from each ts1 and ts2.
    ASSERT_EQ(1, from_count[ts_1]);
    ASSERT_EQ(1, from_count[ts_2]);

    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(&tablet_id, &from_ts, &to_ts)));
    ASSERT_EQ(to_ts, ts_0);
    tablets_moved.insert(tablet_id);
    from_count[from_ts]++;

    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(&tablet_id, &from_ts, &to_ts)));
    ASSERT_EQ(to_ts, ts_0);
    tablets_moved.insert(tablet_id);
    from_count[from_ts]++;

    LOG(INFO) << "Leader distribution: 4 0 0";
    // Make sure two tablets moved from each ts1 and ts2.
    ASSERT_EQ(2, from_count[ts_1]);
    ASSERT_EQ(2, from_count[ts_2]);

    // Make sure all the tablets moved are distinct.
    ASSERT_EQ(4, tablets_moved.size());

    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));
    LOG(INFO) << "Finishing TestBalancingOneAffinitizedLeader";
  }

  void TestBalancingTwoAffinitizedLeaders() {
    LOG(INFO) << "Starting TestBalancingTwoAffinitizedLeaders";

    for (const auto& tablet : tablets_) {
      MoveTabletLeader(tablet.get(), ts_descs_[0]);
    }
    LOG(INFO) << "Leader distribution: 4 0 0";

    AnalyzeTablets();

    std::map<string, int> to_count;
    std::unordered_set<string> tablets_moved;

    string placeholder, tablet_id, from_ts, to_ts, ts_0, ts_1, ts_2;
    ts_0 = ts_descs_[0]->permanent_uuid();
    ts_1 = ts_descs_[1]->permanent_uuid();
    ts_2 = ts_descs_[2]->permanent_uuid();

    to_count[ts_0] = 0;
    to_count[ts_1] = 0;
    to_count[ts_2] = 0;


    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(&tablet_id, &from_ts, &to_ts)));
    ASSERT_EQ(from_ts, ts_0);
    tablets_moved.insert(tablet_id);
    to_count[to_ts]++;

    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(&tablet_id, &from_ts, &to_ts)));
    ASSERT_EQ(from_ts, ts_0);
    tablets_moved.insert(tablet_id);
    to_count[to_ts]++;

    LOG(INFO) << "Leader distribution: 2 1 1";
    // Make sure one tablet moved to each ts1 and ts2.
    ASSERT_EQ(1, to_count[ts_1]);
    ASSERT_EQ(1, to_count[ts_2]);

    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(&tablet_id, &from_ts, &to_ts)));
    ASSERT_EQ(from_ts, ts_0);
    tablets_moved.insert(tablet_id);
    to_count[to_ts]++;

    ASSERT_TRUE(ASSERT_RESULT(HandleLeaderMoves(&tablet_id, &from_ts, &to_ts)));
    ASSERT_EQ(from_ts, ts_0);
    tablets_moved.insert(tablet_id);
    to_count[to_ts]++;

    LOG(INFO) << "Leader distribution: 0 2 2";
    // Make sure two tablets moved to each ts1 and ts2.
    ASSERT_EQ(2, to_count[ts_1]);
    ASSERT_EQ(2, to_count[ts_2]);

    // Make sure all the tablets moved are distinct.
    ASSERT_EQ(4, tablets_moved.size());

    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));
    LOG(INFO) << "Finishing TestBalancingTwoAffinitizedLeaders";
  }

  void TestReadOnlyLoadBalancing() {
    LOG(INFO) << "Starting TestBasicBalancingWithReadOnly";
    // RF = 3 + 1, 4 TS, 3 AZ, 4 tablets.
    // Normal setup.
    // then create new node in az1 with all replicas.
    SetupClusterConfigEnt({"a", "b", "c"} /* az list */, {"a"} /* read only */,
                          {} /* affinitized leaders */, &replication_info_);
    ts_descs_.push_back(SetupTSEnt("3333", "a", read_only_placement_uuid /* placement_uuid */));
    ts_descs_.push_back(SetupTSEnt("4444", "a", read_only_placement_uuid /* placement_uuid */));

    // Adding all read_only replicas.
    for (const auto& tablet : tablets_) {
      AddRunningReplicaEnt(tablet.get(), ts_descs_[3], false /* is live */);
    }

    LOG(INFO) << "The replica count for each read_only tserver is: ts3: 4, ts4: 0";
    AnalyzeTablets();

    string placeholder;

    // First we make sure that no load balancing happens during the live iteration.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));

    ResetState();
    cb_->SetEntOptions(READ_ONLY, read_only_placement_uuid);
    AnalyzeTablets();
    // Now load balance an read_only replica.
    string expected_from_ts = ts_descs_[3]->permanent_uuid();
    string expected_to_ts = ts_descs_[4]->permanent_uuid();
    string expected_tablet_id = tablets_[0]->tablet_id();
    TestAddLoad(expected_tablet_id, expected_from_ts, expected_to_ts);
    RemoveReplica(tablet_map_[expected_tablet_id].get(), ts_descs_[3]);
    AddRunningReplicaEnt(tablet_map_[expected_tablet_id].get(), ts_descs_[4], false);

    ClearTabletsAddedForTest();

    expected_from_ts = ts_descs_[3]->permanent_uuid();
    expected_to_ts = ts_descs_[4]->permanent_uuid();
    expected_tablet_id = tablets_[1]->tablet_id();
    TestAddLoad(expected_tablet_id, expected_from_ts, expected_to_ts);
    RemoveReplica(tablet_map_[expected_tablet_id].get(), ts_descs_[3]);
    AddRunningReplicaEnt(tablet_map_[expected_tablet_id].get(), ts_descs_[4], false);

    // Then make sure there is no more balancing, since both ts3 and ts4 have 2 replicas each.
    ASSERT_FALSE(ASSERT_RESULT(HandleAddReplicas(&placeholder, &placeholder, &placeholder)));
    LOG(INFO) << "Finishing TestBasicBalancingWithReadOnly";
  }

  void TestLeaderBalancingWithReadOnly() {
    // RF = 3 + 1, 4 TS, 3 AZ, 4 tablets.
    LOG(INFO) << "Starting TestLeaderBalancingWithReadOnly";
    SetupClusterConfigEnt({"a", "b", "c"} /* az list */, {"a"} /* read only */,
                          {} /* affinitized leaders */, &replication_info_);
    ts_descs_.push_back(SetupTSEnt("3333", "a", read_only_placement_uuid));

    // Adding all read_only replicas.
    for (const auto& tablet : tablets_) {
      AddRunningReplicaEnt(tablet.get(), ts_descs_[3], false /* is live */);
    }

    ResetState();
    AnalyzeTablets();
    string placeholder;
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));

    cb_->SetEntOptions(READ_ONLY, read_only_placement_uuid);
    ResetState();
    AnalyzeTablets();
    ASSERT_FALSE(ASSERT_RESULT(HandleLeaderMoves(&placeholder, &placeholder, &placeholder)));
    LOG(INFO) << "Finishing TestLeaderBalancingWithReadOnly";
  }

  void PrepareAffinitizedLeaders(const vector<string>& zones) {
    for (const string& zone : zones) {
      CloudInfoPB ci;
      ci.set_placement_cloud(default_cloud);
      ci.set_placement_region(default_region);
      ci.set_placement_zone(zone);
      affinitized_zones_.insert(ci);
    }
  }

  void AddRunningReplicaEnt(TabletInfo* tablet, std::shared_ptr<yb::master::TSDescriptor> ts_desc,
                            bool is_live) {
    TabletInfo::ReplicaMap replicas;
    tablet->GetReplicaLocations(&replicas);

    TabletReplica replica;
    consensus::RaftPeerPB::Role role = is_live ?
        consensus::RaftPeerPB::FOLLOWER :
        consensus::RaftPeerPB::LEARNER;
    NewReplica(ts_desc.get(), tablet::RaftGroupStatePB::RUNNING, role, &replica);
    InsertOrDie(&replicas, ts_desc->permanent_uuid(), replica);
    tablet->SetReplicaLocations(replicas);
  }
};

TEST(TestLoadBalancerEnterprise, TestLoadBalancerAlgorithm) {
  const TableId table_id = CURRENT_TEST_NAME();
  auto options = make_shared<yb::master::enterprise::Options>();
  auto cb = make_shared<yb::master::enterprise::ClusterLoadBalancerMocked>(options.get());
  auto lb = make_shared<TestLoadBalancerEnterprise>(cb.get(), table_id);
  lb->TestAlgorithm();
}


TEST(TestCatalogManagerEnterprise, TestLeaderLoadBalancedAffinitizedLeaders) {
  // Note that this is essentially using transaction_tables_use_preferred_zones = true
  ReplicationInfoPB replication_info;
  SetupClusterConfigEnt({"a", "b", "c"} /* az list */, {} /* read only */,
                        {"a"} /* affinitized leaders */, &replication_info);

  std::shared_ptr<TSDescriptor> ts0 = SetupTSEnt("0000", "a", "");
  std::shared_ptr<TSDescriptor> ts1 = SetupTSEnt("1111", "a", "");
  std::shared_ptr<TSDescriptor> ts2 = SetupTSEnt("2222", "b", "");
  std::shared_ptr<TSDescriptor> ts3 = SetupTSEnt("3333", "b", "");
  std::shared_ptr<TSDescriptor> ts4 = SetupTSEnt("4444", "c", "");

  ASSERT_TRUE(ts0->IsAcceptingLeaderLoad(replication_info));
  ASSERT_TRUE(ts1->IsAcceptingLeaderLoad(replication_info));
  ASSERT_FALSE(ts2->IsAcceptingLeaderLoad(replication_info));
  ASSERT_FALSE(ts3->IsAcceptingLeaderLoad(replication_info));
  ASSERT_FALSE(ts4->IsAcceptingLeaderLoad(replication_info));

  TSDescriptorVector ts_descs = {ts0, ts1, ts2, ts3, ts4};

  ts0->set_leader_count(8);
  ts1->set_leader_count(8);
  ts2->set_leader_count(0);
  ts3->set_leader_count(0);
  ts4->set_leader_count(1);
  ASSERT_NOK(CatalogManagerUtil::AreLeadersOnPreferredOnly(ts_descs, replication_info));

  ts4->set_leader_count(0);
  ASSERT_OK(CatalogManagerUtil::AreLeadersOnPreferredOnly(ts_descs, replication_info));

  ts0->set_leader_count(12);
  ts1->set_leader_count(4);
  ASSERT_OK(CatalogManagerUtil::AreLeadersOnPreferredOnly(ts_descs, replication_info));
}

TEST(TestCatalogManagerEnterprise, TestLeaderLoadBalancedReadOnly) {
  // Note that this is essentially using transaction_tables_use_preferred_zones = true
  ReplicationInfoPB replication_info;
  SetupClusterConfigEnt({"a", "b", "c"} /* az list */, {"d"} /* read only */,
                        {} /* affinitized leaders */, &replication_info);

  std::shared_ptr<TSDescriptor> ts0 = SetupTSEnt("0000", "a", "");
  std::shared_ptr<TSDescriptor> ts1 = SetupTSEnt("1111", "b", "");
  std::shared_ptr<TSDescriptor> ts2 = SetupTSEnt("2222", "c", "");
  std::shared_ptr<TSDescriptor> ts3 = SetupTSEnt("3333", "d", "read_only");

  ASSERT_TRUE(ts0->IsAcceptingLeaderLoad(replication_info));
  ASSERT_TRUE(ts1->IsAcceptingLeaderLoad(replication_info));
  ASSERT_TRUE(ts2->IsAcceptingLeaderLoad(replication_info));
  ASSERT_FALSE(ts3->IsAcceptingLeaderLoad(replication_info));

  TSDescriptorVector ts_descs = {ts0, ts1, ts2, ts3};

  ts0->set_leader_count(8);
  ts1->set_leader_count(8);
  ts2->set_leader_count(8);
  ts3->set_leader_count(0);
  ASSERT_OK(CatalogManagerUtil::AreLeadersOnPreferredOnly(ts_descs, replication_info));

  ts3->set_leader_count(1);
  ASSERT_NOK(CatalogManagerUtil::AreLeadersOnPreferredOnly(ts_descs, replication_info));
}

TEST(TestCatalogManagerEnterprise, TestLoadBalancedReadOnlySameAz) {
  ReplicationInfoPB replication_info;
  SetupClusterConfigEnt({"a"} /* az list */, {"a"} /* read only */,
                        {} /* affinitized leaders */, &replication_info);
  std::shared_ptr<TSDescriptor> ts0 = SetupTSEnt("0000", "a", "");
  std::shared_ptr<TSDescriptor> ts1 = SetupTSEnt("1111", "a", "");
  std::shared_ptr<TSDescriptor> ts2 = SetupTSEnt("2222", "a", "read_only");
  std::shared_ptr<TSDescriptor> ts3 = SetupTSEnt("3333", "a", "read_only");

  TSDescriptorVector ts_descs = {ts0, ts1, ts2, ts3};

  ts0->set_num_live_replicas(6);
  ts1->set_num_live_replicas(6);
  ts2->set_num_live_replicas(12);
  ts3->set_num_live_replicas(12);
  ASSERT_OK(CatalogManagerUtil::IsLoadBalanced(ts_descs));

  ts0->set_num_live_replicas(6);
  ts1->set_num_live_replicas(6);
  ts2->set_num_live_replicas(8);
  ts3->set_num_live_replicas(4);
  ASSERT_NOK(CatalogManagerUtil::IsLoadBalanced(ts_descs));
}

} // namespace enterprise
} // namespace master
} // namespace yb
