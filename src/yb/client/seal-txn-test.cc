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
#include "yb/client/transaction.h"
#include "yb/client/txn-test-base.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"

using namespace std::literals;

DECLARE_bool(enable_load_balancing);
DECLARE_bool(enable_transaction_sealing);
DECLARE_bool(TEST_fail_on_replicated_batch_idx_set_in_txn_record);
DECLARE_int32(TEST_write_rejection_percentage);
DECLARE_int64(transaction_rpc_timeout_ms);

namespace yb {
namespace client {

class SealTxnTest : public TransactionTestBase<MiniCluster> {
 protected:
  void SetUp() override {
    FLAGS_enable_transaction_sealing = true;

    SetIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);
    TransactionTestBase::SetUp();
  }

  void TestNumBatches(bool restart);
};

// Writes some data as part of transaction and check that batches are correcly tracked by
// transaction participant.
void SealTxnTest::TestNumBatches(bool restart) {
  auto txn = CreateTransaction();
  auto session = CreateSession(txn);

  size_t prev_num_non_empty = 0;
  for (auto op_type : {WriteOpType::INSERT, WriteOpType::UPDATE}) {
    ASSERT_OK(WriteRows(session, /* transaction= */ 0, op_type, Flush::kFalse));
    ASSERT_OK(session->Flush());

    size_t num_non_empty = 0;
    auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kLeaders);
    for (const auto& peer : peers) {
      auto txn_participant = peer->tablet()->transaction_participant();
      if (!txn_participant) {
        continue;
      }
      auto replicated_batch_idx_set = txn_participant->TEST_TransactionReplicatedBatches(txn->id());
      LOG(INFO) << peer->tablet_id() << ": " << replicated_batch_idx_set.ToString();
      if (replicated_batch_idx_set.CountSet() != 0) {
        ++num_non_empty;
        ASSERT_EQ(replicated_batch_idx_set.ToString(),
                  op_type == WriteOpType::INSERT ? "[0]" : "[0, 1]");
      }
    }

    if (op_type == WriteOpType::INSERT) {
      ASSERT_GT(num_non_empty, 0);
      if (restart) {
        ASSERT_OK(cluster_->RestartSync());
      }
    } else {
      ASSERT_EQ(num_non_empty, prev_num_non_empty);
    }
    prev_num_non_empty = num_non_empty;
  }
}

TEST_F(SealTxnTest, NumBatches) {
  TestNumBatches(/* restart= */ false);
}

TEST_F(SealTxnTest, NumBatchesWithRestart) {
  TestNumBatches(/* restart= */ true);
}

TEST_F(SealTxnTest, NumBatchesWithRejection) {
  FLAGS_TEST_write_rejection_percentage = 75;
  TestNumBatches(/* restart= */ false);
}

// Check that we could disable writing information about the number of batches,
// since it is required for backward compatibility.
TEST_F(SealTxnTest, NumBatchesDisable) {
  DisableTransactionTimeout();
  // Should be enough for the restarted servers to be back online
  FLAGS_transaction_rpc_timeout_ms = 20000 * kTimeMultiplier;
  FLAGS_enable_transaction_sealing = false;
  FLAGS_TEST_fail_on_replicated_batch_idx_set_in_txn_record = true;

  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  ASSERT_OK(WriteRows(session));
  ASSERT_OK(cluster_->RestartSync());
  ASSERT_OK(txn->CommitFuture().get());
}

TEST_F(SealTxnTest, Simple) {
  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  ASSERT_OK(WriteRows(session, /* transaction = */ 0, WriteOpType::INSERT, Flush::kFalse));
  auto flush_future = session->FlushFuture();
  auto commit_future = txn->CommitFuture(CoarseTimePoint(), SealOnly::kTrue);
  ASSERT_OK(flush_future.get());
  LOG(INFO) << "Flushed: " << txn->id();
  ASSERT_OK(commit_future.get());
  LOG(INFO) << "Committed: " << txn->id();
  ASSERT_NO_FATALS(VerifyData());
  ASSERT_OK(cluster_->RestartSync());
  AssertNoRunningTransactions();
}

TEST_F(SealTxnTest, Update) {
  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  LOG(INFO) << "Inserting rows";
  ASSERT_OK(WriteRows(session, /* transaction = */ 0));
  LOG(INFO) << "Updating rows";
  ASSERT_OK(WriteRows(session, /* transaction = */ 0, WriteOpType::UPDATE, Flush::kFalse));
  auto flush_future = session->FlushFuture();
  auto commit_future = txn->CommitFuture(CoarseTimePoint(), SealOnly::kTrue);
  ASSERT_OK(flush_future.get());
  LOG(INFO) << "Flushed: " << txn->id();
  ASSERT_OK(commit_future.get());
  LOG(INFO) << "Committed: " << txn->id();
  ASSERT_NO_FATALS(VerifyData(1, WriteOpType::UPDATE));
  ASSERT_OK(cluster_->RestartSync());
  AssertNoRunningTransactions();
}

} // namespace client
} // namespace yb
