//
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
//

#include "yb/client/txn-test-base.h"

#include "yb/client/session.h"
#include "yb/client/transaction.h"

#include "yb/common/ql_value.h"
#include "yb/consensus/consensus.h"

#include "yb/integration-tests/mini_cluster_utils.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/yql/cql/ql/util/statement_result.h"

using namespace std::literals;

DECLARE_double(transaction_max_missed_heartbeat_periods);
DECLARE_uint64(transaction_status_tablet_log_segment_size_bytes);
DECLARE_int32(log_min_seconds_to_retain);
DECLARE_bool(transaction_disable_heartbeat_in_tests);
DECLARE_double(TEST_transaction_ignore_applying_probability_in_tests);
DECLARE_string(time_source);
DECLARE_int32(intents_flush_max_delay_ms);
DECLARE_int32(load_balancer_max_concurrent_adds);
DECLARE_bool(TEST_combine_batcher_errors);

namespace yb {
namespace client {

const MonoDelta kTransactionApplyTime = 6s * kTimeMultiplier;
const MonoDelta kIntentsCleanupTime = 6s * kTimeMultiplier;

// We use different sign to distinguish inserted and updated values for testing.
int32_t GetMultiplier(const WriteOpType op_type) {
  switch (op_type) {
    case WriteOpType::INSERT:
      return 1;
    case WriteOpType::UPDATE:
      return -1;
    case WriteOpType::DELETE:
      return 0; // Value is not used in delete path.
  }
  FATAL_INVALID_ENUM_VALUE(WriteOpType, op_type);
}

int32_t KeyForTransactionAndIndex(size_t transaction, size_t index) {
  return static_cast<int32_t>(transaction * 10 + index);
}

int32_t ValueForTransactionAndIndex(size_t transaction, size_t index, const WriteOpType op_type) {
  return static_cast<int32_t>(transaction * 10 + index + 2) * GetMultiplier(op_type);
}

void SetIgnoreApplyingProbability(double value) {
  SetAtomicFlag(value, &FLAGS_TEST_transaction_ignore_applying_probability_in_tests);
}

void SetDisableHeartbeatInTests(bool value) {
  SetAtomicFlag(value, &FLAGS_transaction_disable_heartbeat_in_tests);
}

void DisableApplyingIntents() {
  SetIgnoreApplyingProbability(1.0);
}

void CommitAndResetSync(YBTransactionPtr *txn) {
  CountDownLatch latch(1);
  (*txn)->Commit(TransactionRpcDeadline(), [&latch](const Status& status) {
    ASSERT_OK(status);
    latch.CountDown(1);
  });
  txn->reset();
  latch.Wait();
}

void DisableTransactionTimeout() {
  SetAtomicFlag(std::numeric_limits<double>::max(),
                &FLAGS_transaction_max_missed_heartbeat_periods);
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::SetUp() {
  FLAGS_TEST_combine_batcher_errors = true;
  FLAGS_transaction_status_tablet_log_segment_size_bytes = log_segment_size_bytes();
  FLAGS_log_min_seconds_to_retain = 5;
  FLAGS_intents_flush_max_delay_ms = 250;

  server::SkewedClock::Register();
  FLAGS_time_source = server::SkewedClock::kName;
  FLAGS_load_balancer_max_concurrent_adds = 100;
  ASSERT_NO_FATALS(KeyValueTableTest<MiniClusterType>::SetUp());

  if (create_table_) {
    CreateTable();
  }

  HybridTime::TEST_SetPrettyToString(true);

  ASSERT_OK(clock_->Init());
  transaction_manager_.emplace(client_.get(), clock_, client::LocalTabletFilter());

  server::ClockPtr clock2(new server::HybridClock(skewed_clock_));
  ASSERT_OK(clock2->Init());
  transaction_manager2_.emplace(client_.get(), clock2, client::LocalTabletFilter());
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::CreateTable() {
  KeyValueTableTest<MiniClusterType>::CreateTable(
      Transactional(GetIsolationLevel() != IsolationLevel::NON_TRANSACTIONAL));
}

template <class MiniClusterType>
uint64_t TransactionTestBase<MiniClusterType>::log_segment_size_bytes() const {
  return 128;
}

template <class MiniClusterType>
Status TransactionTestBase<MiniClusterType>::WriteRows(
    const YBSessionPtr& session, size_t transaction, const WriteOpType op_type, Flush flush) {
  for (size_t r = 0; r != kNumRows; ++r) {
    RETURN_NOT_OK(this->WriteRow(
        session,
        KeyForTransactionAndIndex(transaction, r),
        ValueForTransactionAndIndex(transaction, r, op_type),
        op_type,
        flush));
  }
  return Status::OK();
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::VerifyRow(
    int line, const YBSessionPtr& session, int32_t key, int32_t value,
    const std::string& column) {
  VLOG(4) << "Calling SelectRow";
  auto row = this->SelectRow(session, key, column);
  ASSERT_TRUE(row.ok()) << "Bad status: " << row << ", originator: " << __FILE__ << ":" << line;
  VLOG(4) << "SelectRow returned: " << *row;
  ASSERT_EQ(value, *row) << "Originator: " << __FILE__ << ":" << line;
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::WriteData(
    const WriteOpType op_type, size_t transaction) {
  auto txn = CreateTransaction();
  ASSERT_OK(WriteRows(this->CreateSession(txn), transaction, op_type));
  ASSERT_OK(txn->CommitFuture().get());
  LOG(INFO) << "Committed: " << txn->id();
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::WriteDataWithRepetition() {
  auto txn = CreateTransaction();
  auto session = this->CreateSession(txn);
  for (size_t r = 0; r != kNumRows; ++r) {
    for (int j = 10; j--;) {
      ASSERT_OK(this->WriteRow(
          session,
          KeyForTransactionAndIndex(0, r),
          ValueForTransactionAndIndex(0, r, WriteOpType::INSERT) + j));
    }
  }
  ASSERT_OK(txn->CommitFuture().get());
}

namespace {

YBTransactionPtr CreateTransactionHelper(
    TransactionManager* transaction_manager,
    SetReadTime set_read_time,
    IsolationLevel isolation_level) {
  if (isolation_level == IsolationLevel::NON_TRANSACTIONAL) {
    return nullptr;
  }
  auto result = std::make_shared<YBTransaction>(transaction_manager);
  ReadHybridTime read_time;
  if (set_read_time) {
    read_time = ReadHybridTime::FromHybridTimeRange(transaction_manager->clock()->NowRange());
  }
  EXPECT_OK(result->Init(isolation_level, read_time));
  return result;
}

}  // anonymous namespace

template <class MiniClusterType>
YBTransactionPtr TransactionTestBase<MiniClusterType>::CreateTransaction(
    SetReadTime set_read_time) {
  return CreateTransactionHelper(
      transaction_manager_.get_ptr(), set_read_time, GetIsolationLevel());
}

template <class MiniClusterType>
YBTransactionPtr TransactionTestBase<MiniClusterType>::CreateTransaction2(
    SetReadTime set_read_time) {
  return CreateTransactionHelper(
      transaction_manager2_.get_ptr(), set_read_time, GetIsolationLevel());
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::VerifyRows(
    const YBSessionPtr& session, size_t transaction, const WriteOpType op_type,
    const std::string& column) {
  std::vector<client::YBqlReadOpPtr> ops;
  for (size_t r = 0; r != kNumRows; ++r) {
    ops.push_back(ReadRow(session, KeyForTransactionAndIndex(transaction, r), column));
  }
  ASSERT_OK(session->Flush());
  for (size_t r = 0; r != kNumRows; ++r) {
    SCOPED_TRACE(Format("Row: $0, key: $1", r, KeyForTransactionAndIndex(transaction, r)));
    auto& op = ops[r];
    ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK)
        << QLResponsePB_QLStatus_Name(op->response().status());
    auto rowblock = yb::ql::RowsResult(op.get()).GetRowBlock();
    ASSERT_EQ(rowblock->row_count(), 1);
    const auto& first_column = rowblock->row(0).column(0);
    ASSERT_EQ(InternalType::kInt32Value, first_column.type());
    ASSERT_EQ(first_column.int32_value(), ValueForTransactionAndIndex(transaction, r, op_type));
  }
}

template <class MiniClusterType>
YBqlReadOpPtr TransactionTestBase<MiniClusterType>::ReadRow(
    const YBSessionPtr& session, int32_t key, const std::string& column) {
  auto op = table_.NewReadOp();
  auto* const req = op->mutable_request();
  QLAddInt32HashValue(req, key);
  table_.AddColumns({column}, req);
  EXPECT_OK(session->Apply(op));
  return op;
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::VerifyData(
    size_t num_transactions, const WriteOpType op_type, const std::string& column) {
  VLOG(4) << "Verifying data..." << std::endl;
  auto session = this->CreateSession();
  for (size_t i = 0; i != num_transactions; ++i) {
    VerifyRows(session, i, op_type, column);
  }
}

template <class MiniClusterType>
bool TransactionTestBase<MiniClusterType>::HasTransactions() {
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto* tablet_manager = cluster_->mini_tablet_server(i)->server()->tablet_manager();
    auto peers = tablet_manager->GetTabletPeers();
    for (const auto& peer : peers) {
      if (!peer->consensus()) {
        return true; // Report true, since we could have transactions on this non ready peer.
      }
      if (peer->consensus()->GetLeaderStatus() !=
              consensus::LeaderStatus::NOT_LEADER &&
          peer->tablet()->transaction_coordinator() &&
          peer->tablet()->transaction_coordinator()->test_count_transactions()) {
        return true;
      }
    }
  }
  return false;
}

template <class MiniClusterType>
size_t TransactionTestBase<MiniClusterType>::CountRunningTransactions() {
  return yb::CountRunningTransactions(cluster_.get());
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::AssertNoRunningTransactions() {
  yb::AssertNoRunningTransactions(cluster_.get());
}

template <class MiniClusterType>
bool TransactionTestBase<MiniClusterType>::CheckAllTabletsRunning() {
  bool result = true;
  size_t count = 0;
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto peers = cluster_->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers();
    if (i == 0) {
      count = peers.size();
    } else if (count != peers.size()) {
      LOG(WARNING) << "Different number of tablets in tservers: "
                   << count << " vs " << peers.size() << " at " << i;
      result = false;
    }
    for (const auto& peer : peers) {
      auto status = peer->CheckRunning();
      if (!status.ok()) {
        LOG(WARNING) << Format("T $0 P $1 is not running: $2", peer->tablet_id(),
                               peer->permanent_uuid(), status);
        result = false;
      }
    }
  }
  return result;
}

template <class MiniClusterType>
IsolationLevel TransactionTestBase<MiniClusterType>::GetIsolationLevel() {
  return isolation_level_;
}

template <class MiniClusterType>
void TransactionTestBase<MiniClusterType>::SetIsolationLevel(IsolationLevel isolation_level) {
  isolation_level_ = isolation_level;
}

template class TransactionTestBase<MiniCluster>;

} // namespace client
} // namespace yb
