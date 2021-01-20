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
#include "yb/consensus/replica_state.h"

#include <vector>

#include <gtest/gtest.h>
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus-test-util.h"
#include "yb/fs/fs_manager.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"

namespace yb {
namespace consensus {

using std::vector;

// TODO: Share a test harness with ConsensusMetadataTest?
const char* kTabletId = "TestTablet";

class RaftConsensusStateTest : public YBTest {
 public:
  RaftConsensusStateTest()
    : fs_manager_(env_.get(), GetTestPath("fs_root"), "tserver_test"),
      operation_factory_(new MockOperationFactory()) {
  }

  void SetUp() override {
    YBTest::SetUp();
    ASSERT_OK(fs_manager_.CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_.Open());

    // Initialize test configuration.
    config_.set_opid_index(kInvalidOpIdIndex);
    RaftPeerPB* peer = config_.add_peers();
    peer->set_permanent_uuid(fs_manager_.uuid());
    peer->set_member_type(RaftPeerPB::VOTER);

    std::unique_ptr<ConsensusMetadata> cmeta;
    ASSERT_OK(ConsensusMetadata::Create(&fs_manager_, kTabletId, fs_manager_.uuid(),
                                        config_, kMinimumTerm, &cmeta));
    state_.reset(new ReplicaState(
        ConsensusOptions(), fs_manager_.uuid(), std::move(cmeta), operation_factory_.get(),
        nullptr /* safe_op_id_waiter */, nullptr /* retryable_requests */,
        SplitOpInfo(), [](const OpIds&) {} /* applied_ops_tracker */));

    // Start up the ReplicaState.
    ReplicaState::UniqueLock lock;
    ASSERT_OK(state_->LockForStart(&lock));
    ASSERT_OK(state_->StartUnlocked(MinimumOpId()));
  }

 protected:
  FsManager fs_manager_;
  RaftConfigPB config_;
  gscoped_ptr<MockOperationFactory> operation_factory_;
  gscoped_ptr<ReplicaState> state_;
};

// Test that we can transition a new configuration from a pending state into a
// persistent state.
TEST_F(RaftConsensusStateTest, TestPendingPersistent) {
  ReplicaState::UniqueLock lock;
  ASSERT_OK(state_->LockForConfigChange(&lock));

  config_.clear_opid_index();
  ASSERT_OK(state_->SetPendingConfigUnlocked(config_));
  ASSERT_TRUE(state_->IsConfigChangePendingUnlocked());
  ASSERT_FALSE(state_->GetPendingConfigUnlocked().has_opid_index());
  ASSERT_TRUE(state_->GetCommittedConfigUnlocked().has_opid_index());

  ASSERT_FALSE(state_->SetCommittedConfigUnlocked(config_).ok());
  config_.set_opid_index(1);
  ASSERT_TRUE(state_->SetCommittedConfigUnlocked(config_).ok());

  ASSERT_FALSE(state_->IsConfigChangePendingUnlocked());
  ASSERT_EQ(1, state_->GetCommittedConfigUnlocked().opid_index());
}

// Ensure that we can set persistent configurations directly.
TEST_F(RaftConsensusStateTest, TestPersistentWrites) {
  ReplicaState::UniqueLock lock;
  ASSERT_OK(state_->LockForConfigChange(&lock));

  ASSERT_FALSE(state_->IsConfigChangePendingUnlocked());
  ASSERT_EQ(kInvalidOpIdIndex, state_->GetCommittedConfigUnlocked().opid_index());

  config_.clear_opid_index();
  ASSERT_OK(state_->SetPendingConfigUnlocked(config_));
  config_.set_opid_index(1);
  ASSERT_OK(state_->SetCommittedConfigUnlocked(config_));
  ASSERT_EQ(1, state_->GetCommittedConfigUnlocked().opid_index());

  config_.clear_opid_index();
  ASSERT_OK(state_->SetPendingConfigUnlocked(config_));
  config_.set_opid_index(2);
  ASSERT_OK(state_->SetCommittedConfigUnlocked(config_));
  ASSERT_EQ(2, state_->GetCommittedConfigUnlocked().opid_index());
}

}  // namespace consensus
}  // namespace yb
