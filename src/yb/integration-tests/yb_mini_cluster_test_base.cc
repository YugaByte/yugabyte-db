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

#include "yb/integration-tests/yb_mini_cluster_test_base.h"

#include "yb/client/session.h"

#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/external_mini_cluster.h"

using namespace std::literals;

DECLARE_bool(use_priority_thread_pool_for_flushes);
DECLARE_bool(allow_preempting_compactions);
DECLARE_uint64(index_backfill_upperbound_for_user_enforced_txn_duration_ms);
DECLARE_int32(index_backfill_wait_for_old_txns_ms);
DECLARE_int32(index_backfill_rpc_timeout_ms);
DECLARE_int32(index_backfill_rpc_max_delay_ms);
DECLARE_int32(index_backfill_rpc_max_retries);
DECLARE_int32(retrying_ts_rpc_max_delay_ms);
DECLARE_int32(master_ts_rpc_timeout_ms);

///////////////////////////////////////////////////
// YBMiniClusterTestBase
///////////////////////////////////////////////////

namespace yb {

template <class T>
void YBMiniClusterTestBase<T>::SetUp() {
  YBTest::SetUp();
  HybridTime::TEST_SetPrettyToString(true);

  FLAGS_use_priority_thread_pool_for_flushes = true;
  FLAGS_allow_preempting_compactions = true;

  // Note that if a test intends to use user enforced txns then this flag should be
  // updated accordingly, as having this to be smaller than the client timeout could
  // be unsafe. We do not want to have this be a large value in tests because it slows
  // down the normal create index flow.
  FLAGS_index_backfill_upperbound_for_user_enforced_txn_duration_ms = 0;
  FLAGS_index_backfill_wait_for_old_txns_ms = 0;

  FLAGS_index_backfill_rpc_timeout_ms = 6000;
  FLAGS_index_backfill_rpc_max_delay_ms = 1000;
  FLAGS_index_backfill_rpc_max_retries = 10;
  FLAGS_retrying_ts_rpc_max_delay_ms = 1000;
  FLAGS_master_ts_rpc_timeout_ms = 10000;

  verify_cluster_before_next_tear_down_ = true;
}

template <class T>
void YBMiniClusterTestBase<T>::TearDown() {
  DoBeforeTearDown();
  DoTearDown();
}

template <class T>
void YBMiniClusterTestBase<T>::DoBeforeTearDown() {
  if (cluster_ && verify_cluster_before_next_tear_down_ && !testing::Test::HasFailure()) {
    if (cluster_->running()) {
      LOG(INFO) << "Checking cluster consistency...";
      ASSERT_NO_FATALS(ClusterVerifier(cluster_.get()).CheckCluster());
    } else {
      LOG(INFO) << "Not checking cluster consistency: cluster has been shut down or failed to "
                << "start properly";
    }
  }
}

template <class T>
void YBMiniClusterTestBase<T>::DoTearDown() {
  if (cluster_) {
    cluster_->Shutdown();
    cluster_.reset();
  }
  YBTest::TearDown();
}

template <class T>
void YBMiniClusterTestBase<T>::DontVerifyClusterBeforeNextTearDown() {
  verify_cluster_before_next_tear_down_ = false;
}

// Instantiate explicitly to avoid recompilation of a lot of dependent test classes due to template
// implementation changes.
template class YBMiniClusterTestBase<MiniCluster>;
template class YBMiniClusterTestBase<ExternalMiniCluster>;

template <class T>
Status MiniClusterTestWithClient<T>::CreateClient() {
  // Connect to the cluster.
  client_ = VERIFY_RESULT(YBMiniClusterTestBase<T>::cluster_->CreateClient());
  return Status::OK();
}

template <class T>
void MiniClusterTestWithClient<T>::DoTearDown() {
  client_.reset();
  YBMiniClusterTestBase<T>::DoTearDown();
}

template <class T>
client::YBSessionPtr MiniClusterTestWithClient<T>::NewSession() {
  auto session = client_->NewSession();
  session->SetTimeout(60s);
  return session;
}

// Instantiate explicitly to avoid recompilation of a lot of dependent test classes due to template
// implementation changes.
template class MiniClusterTestWithClient<MiniCluster>;
template class MiniClusterTestWithClient<ExternalMiniCluster>;

} // namespace yb
