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

#include "yb/client/session.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_op.h"

#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"

#include "yb/master/master.h"
#include "yb/master/mini_master.h"

#include "yb/util/random_util.h"
#include "yb/util/test_util.h"

using namespace std::literals;

METRIC_DECLARE_histogram(handler_latency_yb_master_MasterService_GetTabletLocations);

DECLARE_int64(meta_cache_lookup_throttling_max_delay_ms);
DECLARE_int64(meta_cache_lookup_throttling_step_ms);

namespace yb {

const std::string kKeyspaceName("ks");
const client::YBTableName kTableName(YQL_DATABASE_CQL, kKeyspaceName, "test");
const std::string kValueColumn("value");
constexpr int kNumTablets = 8;

class NetworkFailureTest : public MiniClusterTestWithClient<MiniCluster> {
 public:
  virtual ~NetworkFailureTest() = default;

  void SetUp() override {
    YBMiniClusterTestBase::SetUp();

    auto opts = MiniClusterOptions();
    opts.num_tablet_servers = 4;
    opts.num_masters = 3;
    cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(cluster_->Start());

    ASSERT_OK(CreateClient());

    // Create a keyspace;
    ASSERT_OK(client_->CreateNamespace(kKeyspaceName));

    client::YBSchemaBuilder builder;
    builder.AddColumn("key")->Type(INT32)->NotNull()->HashPrimaryKey();
    builder.AddColumn("value")->Type(INT32)->NotNull();

    ASSERT_OK(table_.Create(kTableName, kNumTablets, client_.get(), &builder));

    // Cluster verifier is unable to create thread in this setup.
    DontVerifyClusterBeforeNextTearDown();
  }

 protected:
  client::TableHandle table_;
};

int64_t CountLookups(MiniCluster* cluster) {
  int64_t result = 0;
  for (int i = 0; i != cluster->num_masters(); ++i) {
    auto new_leader_master = cluster->mini_master(i);
    auto histogram = new_leader_master->master()->metric_entity()->FindOrCreateHistogram(
        &METRIC_handler_latency_yb_master_MasterService_GetTabletLocations);
    result += histogram->TotalCount();
  }
  return result;
}

TEST_F(NetworkFailureTest, DisconnectMasterLeader) {
  FLAGS_meta_cache_lookup_throttling_max_delay_ms = 10000;
  FLAGS_meta_cache_lookup_throttling_step_ms = 50;

  TestThreadHolder thread_holder;

  std::atomic<int> written(0);
  std::atomic<CoarseTimePoint> prev_report{CoarseTimePoint()};
  thread_holder.AddThreadFunctor([
      this, &written, &stop_flag = thread_holder.stop_flag(), &prev_report]() {
    auto session = client_->NewSession();
    std::deque<std::future<Status>> futures;
    std::deque<client::YBOperationPtr> ops;

    while (!stop_flag.load()) {
      while (!futures.empty() && futures.front().wait_for(0s) == std::future_status::ready) {
        ASSERT_OK(futures.front().get());
        ASSERT_TRUE(ops.front()->succeeded());
        futures.pop_front();
        ops.pop_front();

        auto new_written = ++written;
        if (new_written % 4000 == 0) {
          auto now = CoarseMonoClock::now();
          auto old_value = prev_report.exchange(now);
          LOG(INFO) << "Written: " << new_written << ", time taken: " << MonoDelta(now - old_value);
        }
      }

      int key = RandomUniformInt<int>(0, std::numeric_limits<int>::max() - 1);
      int value = RandomUniformInt<int>(0, std::numeric_limits<int>::max() - 1);
      auto op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
      auto* const req = op->mutable_request();
      QLAddInt32HashValue(req, key);
      table_.AddInt32ColumnValue(req, kValueColumn, value);
      ASSERT_OK(session->Apply(op));
      futures.push_back(session->FlushFuture());
      ops.push_back(op);
    }
  });

  while (written.load() <= RegularBuildVsSanitizers(5000, 500)) {
    std::this_thread::sleep_for(100ms);
  }

  auto old_lookups = CountLookups(cluster_.get());

  auto leader_master_idx = cluster_->LeaderMasterIdx();
  LOG(INFO) << "Old leader: " << leader_master_idx;
  for (int i = 0; i != cluster_->num_masters(); ++i) {
    if (i != leader_master_idx) {
      ASSERT_OK(BreakConnectivity(cluster_.get(), leader_master_idx, i));
    }
  }

  thread_holder.WaitAndStop(10s);

  auto new_lookups = CountLookups(cluster_.get());

  LOG(INFO) << "Lookups before: " << old_lookups << ", after: " << new_lookups;

  ASSERT_LE(new_lookups, old_lookups + 100);
}

} // namespace yb
