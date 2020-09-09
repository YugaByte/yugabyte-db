// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#include <memory>
#include <set>
#include <unordered_map>
#include <boost/optional.hpp>

#include "yb/client/client-test-util.h"
#include "yb/client/table_handle.h"
#include "yb/common/wire_protocol.h"
#include "yb/gutil/map-util.h"
#include "yb/integration-tests/external_mini_cluster-itest-base.h"
#include "yb/integration-tests/test_workload.h"

using yb::client::CountTableRows;
using yb::client::YBTable;
using yb::client::YBTableName;
using std::shared_ptr;
using yb::itest::TServerDetails;
using yb::tablet::TABLET_DATA_TOMBSTONED;
using std::set;
using std::string;
using std::vector;
using std::unordered_map;

namespace yb {

namespace {
const int kNumberOfRetries = 5;
}

// Integration test for client failover behavior.
class ClientFailoverITest : public ExternalMiniClusterITestBase {
};

// Test that we can delete the leader replica while scanning it and still get
// results back.
TEST_F(ClientFailoverITest, TestDeleteLeaderWhileScanning) {
  const MonoDelta kTimeout = MonoDelta::FromSeconds(30);

  vector<string> ts_flags = { "--TEST_enable_remote_bootstrap=false" };
  vector<string> master_flags = {"--catalog_manager_wait_for_new_tablets_to_elect_leader=false"};

  // Start up with 4 tablet servers.
  ASSERT_NO_FATALS(StartCluster(ts_flags, master_flags, 4));

  // Create the test table.
  TestWorkload workload(cluster_.get());
  workload.set_write_timeout_millis(kTimeout.ToMilliseconds());
  workload.Setup();

  // Figure out the tablet id.
  ASSERT_OK(inspect_->WaitForReplicaCount(3));
  vector<string> tablets = inspect_->ListTablets();
  ASSERT_EQ(1, tablets.size());
  const string& tablet_id = tablets[0];

  // Record the locations of the tablet replicas and the one TS that doesn't have a replica.
  int missing_replica_index = -1;
  set<int> replica_indexes;
  unordered_map<string, itest::TServerDetails*> active_ts_map;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    if (inspect_->ListTabletsOnTS(i).empty()) {
      missing_replica_index = i;
    } else {
      replica_indexes.insert(i);
      TServerDetails* ts = ts_map_[cluster_->tablet_server(i)->uuid()].get();
      active_ts_map[ts->uuid()] = ts;
      ASSERT_OK(WaitUntilTabletRunning(ts_map_[cluster_->tablet_server(i)->uuid()].get(),
                                       tablet_id,
                                       kTimeout));
    }
  }
  int leader_index = *replica_indexes.begin();
  TServerDetails* leader = ts_map_[cluster_->tablet_server(leader_index)->uuid()].get();
  for (auto retries_left = kNumberOfRetries; ;) {
    TServerDetails *current_leader = nullptr;
    ASSERT_OK(itest::FindTabletLeader(active_ts_map, tablet_id, kTimeout, &current_leader));
    if (current_leader->uuid() == leader->uuid()) {
      break;
    } else if (retries_left <= 0) {
      FAIL() << "Failed to elect first server as leader";
    }
    ASSERT_OK(itest::StartElection(leader, tablet_id, kTimeout));
    --retries_left;
    SleepFor(MonoDelta::FromMilliseconds(150 * (kNumberOfRetries - retries_left)));
  }

  int64_t expected_index = 0;
  ASSERT_OK(WaitForServersToAgree(kTimeout, active_ts_map, tablet_id, 0, &expected_index));

  // Write data to a tablet.
  workload.Start();
  while (workload.rows_inserted() < 100) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  workload.StopAndJoin();

  expected_index += workload.batches_completed();

  // We don't want the leader that takes over after we kill the first leader to
  // be unsure whether the writes have been committed, so wait until all
  // replicas have all of the writes.
  ASSERT_OK(WaitForServersToAgree(kTimeout,
                                  active_ts_map,
                                  tablet_id,
                                  expected_index,
                                  &expected_index));

  // Open the scanner and count the rows.
  client::TableHandle table;
  ASSERT_OK(table.Open(TestWorkloadOptions::kDefaultTableName, client_.get()));
  ASSERT_EQ(workload.rows_inserted(), CountTableRows(table));
  LOG(INFO) << "Number of rows: " << workload.rows_inserted()
            << ", batches: " << workload.batches_completed()
            << ", expected index: " << expected_index;

  // Delete the leader replica. This will cause the next scan to the same
  // leader to get a TABLET_NOT_FOUND error.
  ASSERT_OK(itest::DeleteTablet(leader, tablet_id, TABLET_DATA_TOMBSTONED,
                                boost::none, kTimeout));

  int old_leader_index = leader_index;
  // old_leader - node that was leader before we started to elect a new leader
  TServerDetails* old_leader = leader;

  ASSERT_EQ(1, replica_indexes.erase(old_leader_index));
  ASSERT_EQ(1, active_ts_map.erase(old_leader->uuid()));

  for (auto retries_left = kNumberOfRetries; ; --retries_left) {
    // current_leader - node that currently leader
    TServerDetails *current_leader = nullptr;
    ASSERT_OK(itest::FindTabletLeader(active_ts_map, tablet_id, kTimeout, &current_leader));
    if (current_leader->uuid() != old_leader->uuid()) {
      leader = current_leader;
      // Do a config change to remove the old replica and add a new one.
      // Cause the new replica to become leader, then do the scan again.
      // Since old_leader is not changed in loop we would not remove more than one node.
      auto result = RemoveServer(leader, tablet_id, old_leader, boost::none, kTimeout, NULL,
                                 false /* retry */);
      if (result.ok()) {
        break;
      } else if (retries_left <= 0) {
        FAIL() << "RemoveServer failed and out of retries: " << result.ToString();
      } else {
        LOG(WARNING) << "RemoveServer failed: " << result.ToString() << ", after "
                     << kNumberOfRetries << " retries";
      }
      SleepFor(MonoDelta::FromMilliseconds(100));
    } else if (retries_left <= 0) {
      FAIL() << "Failed to elect new leader instead of " << old_leader->uuid();
    }
    TServerDetails* desired_leader = nullptr;
    for (auto index : replica_indexes) {
      auto new_leader_uuid = cluster_->tablet_server(index)->uuid();
      if (new_leader_uuid != old_leader->uuid()) {
        desired_leader = ts_map_[new_leader_uuid].get();
        break;
      }
    }
    ASSERT_NE(desired_leader, nullptr);
    ASSERT_OK(itest::StartElection(desired_leader, tablet_id, kTimeout));
    ASSERT_OK(WaitUntilCommittedOpIdIndexIsGreaterThan(&expected_index,
                                                       desired_leader,
                                                       tablet_id,
                                                       kTimeout));
  }

  // Wait until the config is committed, otherwise AddServer() will fail.
  ASSERT_OK(WaitUntilCommittedOpIdIndexIsGreaterThan(&expected_index,
                                                     leader,
                                                     tablet_id,
                                                     kTimeout,
                                                     itest::CommittedEntryType::CONFIG));

  TServerDetails* to_add = ts_map_[cluster_->tablet_server(missing_replica_index)->uuid()].get();
  ASSERT_OK(AddServer(leader, tablet_id, to_add, consensus::RaftPeerPB::PRE_VOTER,
                      boost::none, kTimeout));
  HostPort hp = HostPortFromPB(leader->registration.common().private_rpc_addresses(0));
  ASSERT_OK(StartRemoteBootstrap(to_add, tablet_id, leader->uuid(), hp, 1, kTimeout));

  const string& new_ts_uuid = cluster_->tablet_server(missing_replica_index)->uuid();
  InsertOrDie(&replica_indexes, missing_replica_index);
  InsertOrDie(&active_ts_map, new_ts_uuid, ts_map_[new_ts_uuid].get());

  // Wait for remote bootstrap to complete. Then elect the new node.
  ASSERT_OK(WaitForServersToAgree(kTimeout,
                                  active_ts_map,
                                  tablet_id,
                                  ++expected_index,
                                  &expected_index));
  leader_index = missing_replica_index;
  leader = ts_map_[cluster_->tablet_server(leader_index)->uuid()].get();
  ASSERT_OK(itest::StartElection(leader, tablet_id, kTimeout));
  // Wait for all tservers to agree and have all the updates including the config change.
  ASSERT_OK(WaitForServersToAgree(kTimeout,
                                  active_ts_map,
                                  tablet_id,
                                  ++expected_index,
                                  &expected_index));

  ASSERT_EQ(workload.rows_inserted(), CountTableRows(table));

  // Rotate leaders among the replicas and verify the new leader is the designated one each time.
  for (const auto& ts_map : active_ts_map) {
    for (auto retries_left = kNumberOfRetries; ; --retries_left) {
      TServerDetails* current_leader = nullptr;
      TServerDetails* new_leader = ts_map.second;
      ASSERT_OK(itest::FindTabletLeader(active_ts_map, tablet_id, kTimeout, &current_leader));
      ASSERT_OK(itest::LeaderStepDown(current_leader, tablet_id, new_leader, kTimeout));
      // Wait for all tservers to agree and have all the updates including the config change.
      ASSERT_OK(WaitForServersToAgree(kTimeout,
                                      active_ts_map,
                                      tablet_id,
                                      ++expected_index,
                                      &expected_index));
      current_leader = new_leader;
      ASSERT_OK(itest::FindTabletLeader(active_ts_map, tablet_id, kTimeout, &new_leader));
      if (current_leader->uuid() == new_leader->uuid()) {
        break;
      } else if (retries_left <= 0) {
        FAIL() << "Failed to elect new leader instead of " << old_leader->uuid()
               << ", after " << kNumberOfRetries << " retries";
      }
    }
  }

}

}  // namespace yb
