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

#include <gtest/gtest.h>

#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/gutil/strings/join.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/cluster_verifier.h"
#include "yb/master/master.h"
#include "yb/master/master-test-util.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/master.proxy.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_controller.h"

using std::shared_ptr;
using std::string;
using std::vector;
using strings::Substitute;
using yb::rpc::Messenger;
using yb::rpc::MessengerBuilder;
using yb::consensus::ChangeConfigRequestPB;
using yb::consensus::ChangeConfigResponsePB;
using yb::consensus::ConsensusServiceProxy;
using yb::consensus::RaftPeerPB;
using yb::master::ListMastersRequestPB;
using yb::master::ListMastersResponsePB;
using yb::tserver::TabletServerErrorPB;

namespace yb {
namespace master {

// Test master addition and removal config changes via an external mini cluster
class MasterChangeConfigTest : public YBTest {
 public:
  MasterChangeConfigTest() {}

  ~MasterChangeConfigTest() {}

 protected:
  void SetUp() override {
    YBTest::SetUp();
    ExternalMiniClusterOptions opts;
    opts.master_rpc_ports = { 0, 0, 0 }; // external mini-cluster Start() gets the free ports.
    opts.num_masters = num_masters_ = static_cast<int>(opts.master_rpc_ports.size());
    opts.num_tablet_servers = 0;
    opts.timeout = MonoDelta::FromSeconds(30);
    // Master failovers should not be happening concurrently with us trying to load an initial sys
    // catalog snapshot. At least this is not supported as of 05/27/2019.
    opts.enable_ysql = false;
    cluster_.reset(new ExternalMiniCluster(opts));
    ASSERT_OK(cluster_->Start());

    ASSERT_OK(cluster_->WaitForLeaderCommitTermAdvance());

    ASSERT_OK(CheckNumMastersWithCluster("Start"));
  }

  void TearDown() override {
    if (cluster_) {
      cluster_->Shutdown();
      cluster_.reset();
    }

    YBTest::TearDown();
  }

  Status CheckNumMastersWithCluster(string msg) {
    if (num_masters_ != cluster_->num_masters()) {
      return STATUS(IllegalState, Substitute(
          "$0 : expected to have $1 masters but our cluster has $2 masters.",
          msg, num_masters_, cluster_->num_masters()));
    }

    return Status::OK();
  }

  Status RestartCluster() {
    if (!cluster_) {
      return STATUS(IllegalState, "Cluster was not initialized, cannot restart.");
    }

    RETURN_NOT_OK(CheckNumMastersWithCluster("Pre Restart"));

    cluster_->Shutdown();
    RETURN_NOT_OK(cluster_->Restart());

    RETURN_NOT_OK(CheckNumMastersWithCluster("Post Restart"));

    RETURN_NOT_OK(cluster_->WaitForLeaderCommitTermAdvance());

    return Status::OK();
  }

  // Ensure that the leader's in-memory state has the expected number of peers.
  void VerifyLeaderMasterPeerCount();

  // Ensure that each non-leader's in-memory state has the expected number of peers.
  void VerifyNonLeaderMastersPeerCount();

  // Waits till the master leader is ready - as deemed by the catalog manager. If the leader never
  // loads the sys catalog, this api will timeout. If 'master' is not the leader it will surely
  // timeout. Return status of OK() implies leader is ready.
  Status WaitForMasterLeaderToBeReady(ExternalMaster* master, int timeout_sec);

  // API to capture the latest commit index on the master leader.
  void SetCurLogIndex();

  int num_masters_;
  int cur_log_index_;
  std::unique_ptr<ExternalMiniCluster> cluster_;
};

void MasterChangeConfigTest::VerifyLeaderMasterPeerCount() {
  int num_peers = 0;
  ExternalMaster *leader_master = cluster_->GetLeaderMaster();
  LOG(INFO) << "Checking leader at port " << leader_master->bound_rpc_hostport().port();
  Status s = cluster_->GetNumMastersAsSeenBy(leader_master, &num_peers);
  ASSERT_OK_PREPEND(s, "Leader master number of peers lookup returned error");
  EXPECT_EQ(num_peers, num_masters_);
}

void MasterChangeConfigTest::VerifyNonLeaderMastersPeerCount() {
  int num_peers = 0;
  int leader_index = -1;
  ASSERT_OK_PREPEND(cluster_->GetLeaderMasterIndex(&leader_index), "Leader index get failed.");

  if (leader_index == -1) {
    FAIL() << "Leader index not found.";
  }

  for (int i = 0; i < num_masters_; i++) {
    if (i == leader_index) {
      continue;
    }

    ExternalMaster *non_leader_master = cluster_->master(i);

    LOG(INFO) << "Checking non_leader " << i << " at port "
              << non_leader_master->bound_rpc_hostport().port();
    num_peers = 0;
    Status s = cluster_->GetNumMastersAsSeenBy(non_leader_master, &num_peers);
    ASSERT_OK_PREPEND(s, "Non-leader master number of peers lookup returned error");
    EXPECT_EQ(num_peers, num_masters_);
  }
}

Status MasterChangeConfigTest::WaitForMasterLeaderToBeReady(
    ExternalMaster* master,
    int timeout_sec) {
  MonoTime now = MonoTime::Now();
  MonoTime deadline = now;
  deadline.AddDelta(MonoDelta::FromSeconds(timeout_sec));
  Status s;

  for (int i = 1; now.ComesBefore(deadline); ++i) {
    s = cluster_->GetIsMasterLeaderServiceReady(master);
    if (!s.ok()) {
      // Spew out error info only if it is something other than not-the-leader.
      if (s.ToString().find("NOT_THE_LEADER") == std::string::npos) {
        LOG(WARNING) << "Hit error '" << s.ToString() << "', in iter " << i;
      }
    } else {
      LOG(INFO) << "Got leader ready in iter " << i;
      return Status::OK();
    }
    SleepFor(MonoDelta::FromMilliseconds(min(i, 10)));
    now = MonoTime::Now();
  }

  return STATUS(TimedOut, Substitute("Timed out as master leader $0 term not ready.",
                                     master->bound_rpc_hostport().ToString()));
}

void MasterChangeConfigTest::SetCurLogIndex() {
  OpIdPB op_id;
  ASSERT_OK(cluster_->GetLastOpIdForLeader(&op_id));
  cur_log_index_ = op_id.index();
  LOG(INFO) << "cur_log_index_ " << cur_log_index_;
}

TEST_F(MasterChangeConfigTest, TestAddMaster) {
  // NOTE: Not using smart pointer as ExternalMaster is derived from a RefCounted base class.
  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);

  SetCurLogIndex();

  Status s = cluster_->ChangeConfig(new_master, consensus::ADD_SERVER);
  ASSERT_OK_PREPEND(s, "Change Config returned error : ");

  // Adding a server will generate two ChangeConfig calls. One to add a server as a learner, and one
  // to promote this server to a voter once bootstrapping is finished.
  cur_log_index_ += 2;
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(cur_log_index_));
  ++num_masters_;

  VerifyLeaderMasterPeerCount();
  VerifyNonLeaderMastersPeerCount();
}

TEST_F(MasterChangeConfigTest, TestSlowRemoteBootstrapDoesNotCrashMaster) {
  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);
  ASSERT_OK(cluster_->SetFlag(new_master, "TEST_inject_latency_during_remote_bootstrap_secs", "8"));

  SetCurLogIndex();

  Status s = cluster_->ChangeConfig(new_master, consensus::ADD_SERVER);
  ASSERT_OK_PREPEND(s, "Change Config returned error : ");

  // Adding a server will generate two ChangeConfig calls. One to add a server as a learner, and one
  // to promote this server to a voter once bootstrapping is finished.
  cur_log_index_ += 2;
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(cur_log_index_));
  ++num_masters_;

  VerifyLeaderMasterPeerCount();
  VerifyNonLeaderMastersPeerCount();
}

TEST_F(MasterChangeConfigTest, TestRemoveMaster) {
  int non_leader_index = -1;
  Status s = cluster_->GetFirstNonLeaderMasterIndex(&non_leader_index);
  ASSERT_OK_PREPEND(s, "Non-leader master lookup returned error");
  if (non_leader_index == -1) {
    FAIL() << "Failed to get a non-leader master index.";
  }
  ExternalMaster* remove_master = cluster_->master(non_leader_index);

  LOG(INFO) << "Going to remove master at port " << remove_master->bound_rpc_hostport().port();

  SetCurLogIndex();

  s = cluster_->ChangeConfig(remove_master, consensus::REMOVE_SERVER);
  ASSERT_OK_PREPEND(s, "Change Config returned error");

  // REMOVE_SERVER causes the op index to increase by one.
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(++cur_log_index_));

  --num_masters_;

  VerifyLeaderMasterPeerCount();
  VerifyNonLeaderMastersPeerCount();
}

TEST_F(MasterChangeConfigTest, TestRemoveDeadMaster) {
  int non_leader_index = -1;
  Status s = cluster_->GetFirstNonLeaderMasterIndex(&non_leader_index);
  ASSERT_OK_PREPEND(s, "Non-leader master lookup returned error");
  if (non_leader_index == -1) {
    FAIL() << "Failed to get a non-leader master index.";
  }
  ExternalMaster* remove_master = cluster_->master(non_leader_index);
  remove_master->Shutdown();
  LOG(INFO) << "Stopped and removing master at " << remove_master->bound_rpc_hostport().port();

  SetCurLogIndex();

  s = cluster_->ChangeConfig(remove_master, consensus::REMOVE_SERVER,
                             consensus::RaftPeerPB::PRE_VOTER, true /* use_hostport */);
  ASSERT_OK_PREPEND(s, "Change Config returned error");

  // REMOVE_SERVER causes the op index to increase by one.
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(++cur_log_index_));

  --num_masters_;

  VerifyLeaderMasterPeerCount();
  VerifyNonLeaderMastersPeerCount();
}

TEST_F(MasterChangeConfigTest, TestRestartAfterConfigChange) {
  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);

  SetCurLogIndex();

  Status s = cluster_->ChangeConfig(new_master, consensus::ADD_SERVER);
  ASSERT_OK_PREPEND(s, "Change Config returned error");

  ++num_masters_;

  // Adding a server will generate two ChangeConfig calls. One to add a server as a learner, and one
  // to promote this server to a voter once bootstrapping is finished.
  cur_log_index_ += 2;
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(cur_log_index_));

  VerifyLeaderMasterPeerCount();
  VerifyNonLeaderMastersPeerCount();

  // Give time for cmeta to get flushed on all peers - TODO(Bharat) ENG-104
  SleepFor(MonoDelta::FromSeconds(5));

  s = RestartCluster();
  ASSERT_OK_PREPEND(s, "Restart Cluster failed");

  VerifyLeaderMasterPeerCount();
  VerifyNonLeaderMastersPeerCount();
}

TEST_F(MasterChangeConfigTest, TestNewLeaderWithPendingConfigLoadsSysCatalog) {
  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);

  LOG(INFO) << "New master " << new_master->bound_rpc_hostport().ToString();

  SetCurLogIndex();

  // This will disable new elections on the old masters.
  vector<ExternalMaster*> masters = cluster_->master_daemons();
  for (auto master : masters) {
    ASSERT_OK(cluster_->SetFlag(master, "TEST_do_not_start_election_test_only", "true"));
    // Do not let the followers commit change role - to keep their opid same as the new master,
    // and hence will vote for it.
    ASSERT_OK(cluster_->SetFlag(master, "inject_delay_commit_pre_voter_to_voter_secs", "5"));
  }

  // Wait for 5 seconds on new master to commit voter mode transition. Note that this should be
  // less than the timeout sent to WaitForMasterLeaderToBeReady() below. We want the pending
  // config to be preset when the new master is deemed as leader to start the sys catalog load, but
  // would need to get that pending config committed for load to progress.
  ASSERT_OK(cluster_->SetFlag(new_master, "inject_delay_commit_pre_voter_to_voter_secs", "5"));
  // And don't let it start an election too soon.
  ASSERT_OK(cluster_->SetFlag(new_master, "TEST_do_not_start_election_test_only", "true"));

  Status s = cluster_->ChangeConfig(new_master, consensus::ADD_SERVER);
  ASSERT_OK_PREPEND(s, "Change Config returned error");

  // Wait for addition of the new master as a PRE_VOTER to commit on all peers. The CHANGE_ROLE
  // part is not committed on all the followers, as that might block the new master from becoming
  // the leader as others would have a opid higher than the new master and will not vote for it.
  // The new master will become FOLLOWER and can start an election once it has a pending change
  // that makes it a VOTER.
  cur_log_index_ += 1;
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(cur_log_index_));
  TabletServerErrorPB::Code dummy_err = TabletServerErrorPB::UNKNOWN_ERROR;
  // Leader step down.
  s = cluster_->StepDownMasterLeader(&dummy_err);

  // Now the new master should start the election process.
  ASSERT_OK(cluster_->SetFlag(new_master, "TEST_do_not_start_election_test_only", "false"));

  // Leader stepdown might not succeed as PRE_VOTER could still be uncommitted. Let it go through
  // as new master should get the other votes anyway once it starts the election.
  if (!s.IsIllegalState()) {
    ASSERT_OK_PREPEND(s,  "Leader step down failed.");
  } else {
    LOG(INFO) << "Triggering election as step down failed.";
    ASSERT_OK_PREPEND(cluster_->StartElection(new_master), "Start Election failed");
    SleepFor(MonoDelta::FromSeconds(2));
  }

  // Ensure that the new leader is the new master we spun up above.
  ExternalMaster* new_leader = cluster_->GetLeaderMaster();
  LOG(INFO) << "New leader " << new_leader->bound_rpc_hostport().ToString();
  ASSERT_EQ(new_master->bound_rpc_addr().port(), new_leader->bound_rpc_addr().port());

  // This check ensures that the sys catalog is loaded into new leader even when it has a
  // pending config change.
  ASSERT_OK(WaitForMasterLeaderToBeReady(new_master, 8 /* timeout_sec */));
}

TEST_F(MasterChangeConfigTest, TestChangeAllMasters) {
  ExternalMaster* new_masters[3] = { nullptr, nullptr, nullptr };
  ExternalMaster* remove_master = nullptr;

  // Create all new masters before to avoid rpc port reuse.
  for (int idx = 0; idx <= 2; idx++) {
    cluster_->StartShellMaster(&new_masters[idx]);
  }

  SetCurLogIndex();

  for (int idx = 0; idx <= 2; idx++) {
    LOG(INFO) << "LOOP " << idx << " start.";
    LOG(INFO) << "ADD " << new_masters[idx]->bound_rpc_hostport().ToString();
    ASSERT_OK_PREPEND(cluster_->ChangeConfig(new_masters[idx], consensus::ADD_SERVER),
                      "Add Change Config returned error");
    ++num_masters_;
    remove_master = cluster_->master(0);
    LOG(INFO) << "REMOVE " << remove_master->bound_rpc_hostport().ToString();
    ASSERT_OK_PREPEND(cluster_->ChangeConfig(remove_master, consensus::REMOVE_SERVER),
                      "Remove Change Config returned error");
    --num_masters_;
    LOG(INFO) << "LOOP " << idx << " end.";
  }

  // Followers might not be up to speed as we did not wait, so just check leader.
  VerifyLeaderMasterPeerCount();
}

TEST_F(MasterChangeConfigTest, TestAddPreObserverMaster) {
  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);

  SetCurLogIndex();
  ASSERT_OK_PREPEND(cluster_->ChangeConfig(new_master, consensus::ADD_SERVER,
                                           consensus::RaftPeerPB::PRE_OBSERVER),
                    "Add Change Config returned error");
  ++num_masters_;

  // Followers might not be up to speed as we did not wait, so just check leader.
  VerifyLeaderMasterPeerCount();
}

TEST_F(MasterChangeConfigTest, TestWaitForChangeRoleCompletion) {
  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);
  ExternalMaster* leader = cluster_->GetLeaderMaster();

  // Ensure leader does not change.
  for (int idx = 0; idx <= 2; idx++) {
    ExternalMaster* master = cluster_->master(idx);
    if (master->bound_rpc_hostport().port() != leader->bound_rpc_hostport().port()) {
      ASSERT_OK(cluster_->SetFlag(master, "TEST_do_not_start_election_test_only", "false"));
    }
  }

  ASSERT_OK(cluster_->SetFlag(leader,
            "TEST_inject_delay_leader_change_role_append_secs", "8"));
  SetCurLogIndex();
  ASSERT_OK_PREPEND(cluster_->ChangeConfig(new_master, consensus::ADD_SERVER),
                    "Add Change Config returned error");

  // Wait a bit for PRE_VOTER to be committed. This should be less than the value of 8 seconds
  // set in the injected delay above.
  SleepFor(MonoDelta::FromSeconds(1));

  LOG(INFO) << "Remove Leader " << leader->bound_rpc_hostport().ToString();
  ASSERT_OK_PREPEND(cluster_->ChangeConfig(leader, consensus::REMOVE_SERVER),
                    "Remove Change Config returned error");

  VerifyLeaderMasterPeerCount();
}

TEST_F(MasterChangeConfigTest, TestLeaderSteppedDownNotElected) {
  SetCurLogIndex();
  ExternalMaster* old_leader = cluster_->GetLeaderMaster();
  // Give the other peers few iterations to converge.
  ASSERT_OK(cluster_->SetFlag(old_leader, "leader_failure_max_missed_heartbeat_periods", "24"));
  LOG(INFO) << "Current leader bound to " << old_leader->bound_rpc_hostport().ToString();
  TabletServerErrorPB::Code dummy_err = TabletServerErrorPB::UNKNOWN_ERROR;
  ASSERT_OK_PREPEND(cluster_->StepDownMasterLeader(&dummy_err),
                    "Leader step down failed.");
  // Ensure that the new leader is not the old leader.
  ExternalMaster* new_leader = cluster_->GetLeaderMaster();
  LOG(INFO) << "New leader bound to " << new_leader->bound_rpc_hostport().ToString();
  ASSERT_NE(old_leader->bound_rpc_addr().port(), new_leader->bound_rpc_addr().port());
}

TEST_F(MasterChangeConfigTest, TestMulitpleLeaderRestarts) {
  ExternalMaster* first_leader = cluster_->GetLeaderMaster();
  first_leader->Shutdown();
  // Ensure that the new leader is not the old leader.
  ExternalMaster* second_leader = cluster_->GetLeaderMaster();
  ASSERT_NE(second_leader->bound_rpc_addr().port(), first_leader->bound_rpc_addr().port());
  // Revive the first leader.
  ASSERT_OK(first_leader->Restart());
  ExternalMaster* check_leader = cluster_->GetLeaderMaster();
  // Leader should not be first leader.
  ASSERT_NE(check_leader->bound_rpc_addr().port(), first_leader->bound_rpc_addr().port());
  second_leader->Shutdown();
  check_leader = cluster_->GetLeaderMaster();
  // Leader should not be second one, it can be any one of the other masters.
  ASSERT_NE(second_leader->bound_rpc_addr().port(), check_leader->bound_rpc_addr().port());
}

TEST_F(MasterChangeConfigTest, TestPingShellMaster) {
  string peers = "";
  // Create a shell master as `peers` is empty (for master_addresses).
  Result<ExternalMaster *> new_shell_master = cluster_->StartMasterWithPeers(peers);
  ASSERT_OK(new_shell_master);
  // Add the new shell master to the quorum and ensure it is still running and pingable.
  SetCurLogIndex();
  Status s = cluster_->ChangeConfig(*new_shell_master, consensus::ADD_SERVER);
  LOG(INFO) << "Started shell " << (*new_shell_master)->bound_rpc_hostport().ToString();
  ASSERT_OK_PREPEND(s, "Change Config returned error : ");
  ++num_masters_;
  ASSERT_OK(cluster_->PingMaster(*new_shell_master));
}

// Process that stops/fails internal to external mini cluster is not allowing test to terminate.
TEST_F(MasterChangeConfigTest, DISABLED_TestIncorrectMasterStart) {
  string peers = cluster_->GetMasterAddresses();
  // Master process start with master_addresses not containing a new master host/port should fail
  // and become un-pingable.
  Result<ExternalMaster *> new_master = cluster_->StartMasterWithPeers(peers);
  ASSERT_OK(new_master);
  LOG(INFO) << "Tried incorrect master " << (*new_master)->bound_rpc_hostport().ToString();
  ASSERT_NOK(cluster_->PingMaster(*new_master));
  (*new_master)->Shutdown();
}

} // namespace master
} // namespace yb
