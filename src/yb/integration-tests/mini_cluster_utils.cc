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

#include "yb/integration-tests/mini_cluster_utils.h"

#include "yb/integration-tests/mini_cluster.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/test_util.h"

using namespace std::literals;

namespace yb {

size_t CountRunningTransactions(MiniCluster* cluster) {
  size_t result = 0;
  auto peers = ListTabletPeers(cluster, ListPeersFilter::kAll);
  for (const auto &peer : peers) {
    auto participant = peer->tablet()->transaction_participant();
    result += participant ? participant->TEST_GetNumRunningTransactions() : 0;
  }
  return result;
}

void AssertRunningTransactionsCountLessOrEqualTo(MiniCluster* cluster,
                                                 size_t max_remaining_txns_per_tablet) {
  MonoTime deadline = MonoTime::Now() + 15s * kTimeMultiplier;
  bool has_bad = false;
  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    auto server = cluster->mini_tablet_server(i)->server();
    std::vector<std::shared_ptr<tablet::TabletPeer>> tablets;
    auto status = Wait([server, &tablets] {
          tablets.clear();
          server->tablet_manager()->GetTabletPeers(&tablets);
          for (const auto& peer : tablets) {
            if (peer->tablet() == nullptr) {
              return false;
            }
          }
          return true;
        }, deadline, "Wait until all peers have tablets");
    if (!status.ok()) {
      has_bad = true;
      for (const auto& peer : tablets) {
        if (peer->tablet() == nullptr) {
          LOG(ERROR) << Format(
              "T $1 P $0: Tablet object is not created",
              server->permanent_uuid(), peer->tablet_id());
        }
      }
      continue;
    }
    for (const auto& peer : tablets) {
      auto participant = peer->tablet()->transaction_participant();
      if (participant) {
        auto status = Wait([participant, max_remaining_txns_per_tablet] {
              return participant->TEST_GetNumRunningTransactions() <= max_remaining_txns_per_tablet;
            },
            deadline,
            "Wait until no transactions are running");
        if (!status.ok()) {
          LOG(ERROR) << Format(
              "T $1 P $0: Transactions: $2",
              server->permanent_uuid(), peer->tablet_id(),
              participant->TEST_GetNumRunningTransactions());
          has_bad = true;
        }
      }
    }
  }
  ASSERT_FALSE(has_bad);
}

void AssertNoRunningTransactions(MiniCluster* cluster) {
  AssertRunningTransactionsCountLessOrEqualTo(cluster, 0);
}

} // namespace yb
