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

#ifndef ENT_SRC_YB_INTEGRATION_TESTS_TWODC_TEST_BASE_H
#define ENT_SRC_YB_INTEGRATION_TESTS_TWODC_TEST_BASE_H

#include <string>

#include "yb/client/client.h"

#include "yb/integration-tests/cdc_test_util.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/master/master.h"

#include "yb/util/test_util.h"
#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"

DECLARE_int32(cdc_read_rpc_timeout_ms);
DECLARE_int32(cdc_write_rpc_timeout_ms);
DECLARE_bool(TEST_check_broadcast_address);
DECLARE_bool(flush_rocksdb_on_shutdown);
DECLARE_bool(cdc_enable_replicate_intents);

namespace yb {

using client::YBClient;

namespace enterprise {

constexpr int kRpcTimeout = NonTsanVsTsan(30, 120);
static const std::string kUniverseId = "test_universe";
static const std::string kNamespaceName = "test_namespace";

struct TwoDCTestParams {
  TwoDCTestParams(int batch_size_, bool enable_replicate_intents_) :
      batch_size(batch_size_), enable_replicate_intents(enable_replicate_intents_) {}

  int batch_size;
  bool enable_replicate_intents;
};

class TwoDCTestBase : public YBTest {
 public:
  class Cluster {
   public:
    std::unique_ptr<MiniCluster> mini_cluster_;
    std::unique_ptr<YBClient> client_;
    std::unique_ptr<yb::pgwrapper::PgSupervisor> pg_supervisor_;
    HostPort pg_host_port_;
    boost::optional<client::TransactionManager> txn_mgr_;

    Result<pgwrapper::PGConn> Connect() {
      return pgwrapper::PGConn::Connect(pg_host_port_);
    }

    Result<pgwrapper::PGConn> ConnectToDB(const std::string& dbname) {
      return pgwrapper::PGConn::Connect(pg_host_port_, dbname);
    }
  };

  void SetUp() override {
    YBTest::SetUp();
    // Allow for one-off network instability by ensuring a single CDC RPC timeout << test timeout.
    FLAGS_cdc_read_rpc_timeout_ms = (kRpcTimeout / 4) * 1000;
    FLAGS_cdc_write_rpc_timeout_ms = (kRpcTimeout / 4) * 1000;
    // Not a useful test for us. It's testing Public+Private IP NW errors and we're only public
    FLAGS_TEST_check_broadcast_address = false;
    FLAGS_flush_rocksdb_on_shutdown = false;
  }

  void Destroy();

  CHECKED_STATUS SetupUniverseReplication(
      MiniCluster* producer_cluster, MiniCluster* consumer_cluster, YBClient* consumer_client,
      const std::string& universe_id, const std::vector<std::shared_ptr<client::YBTable>>& tables,
      bool leader_only = true);

  CHECKED_STATUS VerifyUniverseReplication(
      MiniCluster* consumer_cluster, YBClient* consumer_client,
      const std::string& universe_id, master::GetUniverseReplicationResponsePB* resp);

  CHECKED_STATUS ToggleUniverseReplication(
      MiniCluster* consumer_cluster, YBClient* consumer_client,
      const std::string& universe_id, bool is_enabled);

  CHECKED_STATUS VerifyUniverseReplicationDeleted(MiniCluster* consumer_cluster,
      YBClient* consumer_client, const std::string& universe_id, int timeout);

  CHECKED_STATUS GetCDCStreamForTable(
      const std::string& table_id, master::ListCDCStreamsResponsePB* resp);

  uint32_t GetSuccessfulWriteOps(MiniCluster* cluster);

  CHECKED_STATUS DeleteUniverseReplication(const std::string& universe_id);

  CHECKED_STATUS DeleteUniverseReplication(
      const std::string& universe_id, YBClient* client, MiniCluster* cluster);

  uint32_t NumProducerTabletsPolled(MiniCluster* cluster);

  CHECKED_STATUS CorrectlyPollingAllTablets(MiniCluster* cluster, uint32_t num_producer_tablets);

  YBClient* producer_client() {
    return producer_cluster_.client_.get();
  }

  YBClient* consumer_client() {
    return consumer_cluster_.client_.get();
  }

  MiniCluster* producer_cluster() {
    return producer_cluster_.mini_cluster_.get();
  }

  MiniCluster* consumer_cluster() {
    return consumer_cluster_.mini_cluster_.get();
  }

  client::TransactionManager* producer_txn_mgr() {
    return producer_cluster_.txn_mgr_.get_ptr();
  }

  client::TransactionManager* consumer_txn_mgr() {
    return consumer_cluster_.txn_mgr_.get_ptr();
  }

 protected:
  Cluster producer_cluster_;
  Cluster consumer_cluster_;
};

} // namespace enterprise
} // namespace yb

#endif // ENT_SRC_YB_INTEGRATION_TESTS_TWODC_TEST_BASE_H
