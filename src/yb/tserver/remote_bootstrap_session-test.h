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
#ifndef YB_TSERVER_REMOTE_BOOTSTRAP_SESSION_TEST_H_
#define YB_TSERVER_REMOTE_BOOTSTRAP_SESSION_TEST_H_

#include <memory>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/tablet/tablet-test-util.h"

#include "yb/common/partial_row.h"
#include "yb/common/schema.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/log.h"
#include "yb/consensus/metadata.pb.h"
#include "yb/consensus/opid_util.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/fastmem.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/messenger.h"
#include "yb/tserver/remote_bootstrap_session.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/util/crc.h"
#include "yb/util/metrics.h"
#include "yb/util/test_util.h"
#include "yb/util/threadpool.h"

METRIC_DECLARE_entity(tablet);

DECLARE_bool(quick_leader_election_on_create);

using std::shared_ptr;
using std::string;

namespace yb {
namespace tserver {

using std::unique_ptr;
using consensus::ConsensusMetadata;
using consensus::RaftConfigPB;
using consensus::RaftPeerPB;
using log::Log;
using log::LogOptions;
using log::LogAnchorRegistry;
using rpc::Messenger;
using rpc::MessengerBuilder;
using strings::Substitute;
using tablet::YBTabletTest;
using tablet::TabletPeer;
using tablet::RaftGroupReplicaSuperBlockPB;
using tablet::WriteOperationState;

const int64_t kLeaderTerm = 1;

class RemoteBootstrapTest : public YBTabletTest {
 public:
  explicit RemoteBootstrapTest(TableType table_type)
    : YBTabletTest(GetSimpleTestSchema(), table_type) {
  }

  virtual void SetUp() override {
    ASSERT_OK(ThreadPoolBuilder("raft").Build(&raft_pool_));
    ASSERT_OK(ThreadPoolBuilder("prepare").Build(&tablet_prepare_pool_));
    ASSERT_OK(ThreadPoolBuilder("log").Build(&log_thread_pool_));
    YBTabletTest::SetUp();
    SetUpTabletPeer();
    ASSERT_NO_FATALS(PopulateTablet());
    InitSession();
  }

  virtual void TearDown() override {
    messenger_->Shutdown();
    session_.reset();
    tablet_peer_->Shutdown();
    YBTabletTest::TearDown();
  }

 protected:
  void SetUpTabletPeer() {
    scoped_refptr<Log> log;
    ASSERT_OK(Log::Open(LogOptions(), tablet()->tablet_id(),
                       fs_manager()->GetFirstTabletWalDirOrDie(tablet()->metadata()->table_id(),
                                                               tablet()->tablet_id()),
                       fs_manager()->uuid(),
                       *tablet()->schema(),
                       0,  // schema_version
                       nullptr, // metric_entity
                       log_thread_pool_.get(),
                       log_thread_pool_.get(),
                       std::numeric_limits<int64_t>::max(), // cdc_min_replicated_index
                       &log));

    scoped_refptr<MetricEntity> metric_entity =
      METRIC_ENTITY_tablet.Instantiate(&metric_registry_, CURRENT_TEST_NAME());

    RaftPeerPB config_peer;
    config_peer.set_permanent_uuid(fs_manager()->uuid());
    config_peer.set_member_type(RaftPeerPB::VOTER);
    auto hp = config_peer.mutable_last_known_private_addr()->Add();
    hp->set_host("fake-host");
    hp->set_port(0);

    tablet_peer_.reset(new TabletPeer(
        tablet()->metadata(), config_peer, clock(), fs_manager()->uuid(),
        Bind(
            &RemoteBootstrapTest::TabletPeerStateChangedCallback,
            Unretained(this),
            tablet()->tablet_id()),
        &metric_registry_,
        nullptr /* tablet_splitter */,
        std::shared_future<client::YBClient*>()));

    // TODO similar to code in tablet_peer-test, consider refactor.
    RaftConfigPB config;
    config.add_peers()->CopyFrom(config_peer);
    config.set_opid_index(consensus::kInvalidOpIdIndex);

    std::unique_ptr<ConsensusMetadata> cmeta;
    ASSERT_OK(ConsensusMetadata::Create(tablet()->metadata()->fs_manager(),
                                       tablet()->tablet_id(), fs_manager()->uuid(),
                                       config, consensus::kMinimumTerm, &cmeta));

    MessengerBuilder mbuilder(CURRENT_TEST_NAME());
    messenger_ = ASSERT_RESULT(mbuilder.Build());
    proxy_cache_ = std::make_unique<rpc::ProxyCache>(messenger_.get());

    log_anchor_registry_.reset(new LogAnchorRegistry());
    ASSERT_OK(tablet_peer_->SetBootstrapping());
    ASSERT_OK(tablet_peer_->InitTabletPeer(
        tablet(),
        nullptr /* server_mem_tracker */,
        messenger_.get(),
        proxy_cache_.get(),
        log,
        metric_entity,
        raft_pool_.get(),
        tablet_prepare_pool_.get(),
        nullptr /* retryable_requests */,
        yb::OpId() /* split_op_id */));
    consensus::ConsensusBootstrapInfo boot_info;
    ASSERT_OK(tablet_peer_->Start(boot_info));

    ASSERT_OK(tablet_peer_->WaitUntilConsensusRunning(MonoDelta::FromSeconds(2)));


    AssertLoggedWaitFor([&]() -> Result<bool> {
      if (FLAGS_quick_leader_election_on_create) {
        return tablet_peer_->LeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY;
      }
      RETURN_NOT_OK(tablet_peer_->consensus()->EmulateElection());
      return true;
    }, MonoDelta::FromMilliseconds(500), "If quick leader elections enabled, wait for peer to be a "
                                         "leader, otherwise emulate.");
  }

  void TabletPeerStateChangedCallback(const string& tablet_id,
                                      std::shared_ptr<consensus::StateChangeContext> context) {
    LOG(INFO) << "Tablet peer state changed for tablet " << tablet_id
              << ". Reason: " << context->ToString();
  }

  void PopulateTablet() {
    for (int32_t i = 0; i < 1000; i++) {
      WriteRequestPB req;
      req.set_tablet_id(tablet_peer_->tablet_id());
      AddTestRowInsert(i, i * 2, Substitute("key$0", i), &req);

      WriteResponsePB resp;
      CountDownLatch latch(1);

      auto state = std::make_unique<WriteOperationState>(tablet_peer_->tablet(), &req, &resp);
      state->set_completion_callback(tablet::MakeLatchOperationCompletionCallback(&latch, &resp));
      tablet_peer_->WriteAsync(
          std::move(state), kLeaderTerm, CoarseTimePoint::max() /* deadline */);
      latch.Wait();
      ASSERT_FALSE(resp.has_error()) << "Request failed: " << resp.error().ShortDebugString();
      ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, resp.ql_response_batch(0).status()) <<
          "Insert error: " << resp.ShortDebugString();
    }
    ASSERT_OK(tablet()->Flush(tablet::FlushMode::kSync));
  }

  virtual void InitSession() {
    session_.reset(new RemoteBootstrapSession(
        tablet_peer_, "TestSession", "FakeUUID", nullptr /* nsessions */));
    ASSERT_OK(session_->Init());
  }

  MetricRegistry metric_registry_;
  scoped_refptr<LogAnchorRegistry> log_anchor_registry_;
  unique_ptr<ThreadPool> raft_pool_;
  unique_ptr<ThreadPool> tablet_prepare_pool_;
  unique_ptr<ThreadPool> log_thread_pool_;
  std::shared_ptr<TabletPeer> tablet_peer_;
  scoped_refptr<RemoteBootstrapSession> session_;
  std::unique_ptr<rpc::Messenger> messenger_;
  std::unique_ptr<rpc::ProxyCache> proxy_cache_;
};

}  // namespace tserver
}  // namespace yb

#endif  // YB_TSERVER_REMOTE_BOOTSTRAP_SESSION_TEST_H_
