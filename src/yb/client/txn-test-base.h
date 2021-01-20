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

#ifndef YB_CLIENT_TXN_TEST_BASE_H
#define YB_CLIENT_TXN_TEST_BASE_H

#include "yb/client/ql-dml-test-base.h"

#include "yb/server/hybrid_clock.h"
#include "yb/server/skewed_clock.h"

namespace yb {
namespace client {

constexpr size_t kNumRows = 5;
extern const MonoDelta kTransactionApplyTime;
extern const MonoDelta kIntentsCleanupTime;

// We use different sign to distinguish inserted and updated values for testing.
int32_t GetMultiplier(const WriteOpType op_type);

int32_t KeyForTransactionAndIndex(size_t transaction, size_t index);

int32_t ValueForTransactionAndIndex(size_t transaction, size_t index, const WriteOpType op_type);

void SetIgnoreApplyingProbability(double value);

void SetDisableHeartbeatInTests(bool value);

void DisableApplyingIntents();

void CommitAndResetSync(YBTransactionPtr *txn);

void DisableTransactionTimeout();

#define VERIFY_ROW(...) ASSERT_NO_FATAL_FAILURE(VerifyRow(__LINE__, __VA_ARGS__))

YB_STRONGLY_TYPED_BOOL(SetReadTime);


template <class MiniClusterType>
class TransactionTestBase : public KeyValueTableTest<MiniClusterType> {
 protected:
  void SetUp() override;

  void CreateTable();

  virtual uint64_t log_segment_size_bytes() const;

  CHECKED_STATUS WriteRows(
      const YBSessionPtr& session, size_t transaction = 0,
      const WriteOpType op_type = WriteOpType::INSERT,
      Flush flush = Flush::kTrue);

  void VerifyRow(int line, const YBSessionPtr& session, int32_t key, int32_t value,
                 const std::string& column = KeyValueTableTest<MiniClusterType>::kValueColumn);

  void WriteData(const WriteOpType op_type = WriteOpType::INSERT, size_t transaction = 0);

  void WriteDataWithRepetition();

  // Create a new transaction using transaction_manager_.
  YBTransactionPtr CreateTransaction(SetReadTime set_read_time = SetReadTime::kFalse);

  // Create a new transaction using transaction_manager2_.
  YBTransactionPtr CreateTransaction2(SetReadTime set_read_time = SetReadTime::kFalse);

  void VerifyRows(const YBSessionPtr& session,
                  size_t transaction = 0,
                  const WriteOpType op_type = WriteOpType::INSERT,
                  const std::string& column = KeyValueTableTest<MiniClusterType>::kValueColumn);

  YBqlReadOpPtr ReadRow(
      const YBSessionPtr& session,
      int32_t key,
      const std::string& column = kValueColumn);

  void VerifyData(
      size_t num_transactions = 1, const WriteOpType op_type = WriteOpType::INSERT,
      const std::string& column = KeyValueTableTest<MiniClusterType>::kValueColumn);

  void VerifyData(
      const WriteOpType op_type,
      const std::string& column = kValueColumn) {
    VerifyData(/* num_transactions= */ 1, op_type, column);
  }

  bool HasTransactions();

  size_t CountRunningTransactions();

  void AssertNoRunningTransactions();

  bool CheckAllTabletsRunning();

  IsolationLevel GetIsolationLevel();

  void SetIsolationLevel(IsolationLevel isolation_level);

  using KeyValueTableTest<MiniClusterType>::kKeyColumn;
  using KeyValueTableTest<MiniClusterType>::kValueColumn;
  using KeyValueTableTest<MiniClusterType>::cluster_;
  using KeyValueTableTest<MiniClusterType>::client_;
  using KeyValueTableTest<MiniClusterType>::table_;

  std::shared_ptr<server::SkewedClock> skewed_clock_{
      std::make_shared<server::SkewedClock>(WallClock())};
  server::ClockPtr clock_{new server::HybridClock(skewed_clock_)};
  boost::optional<TransactionManager> transaction_manager_;
  boost::optional<TransactionManager> transaction_manager2_;

  bool create_table_ = true;
  IsolationLevel isolation_level_ = IsolationLevel::SNAPSHOT_ISOLATION;
};

template <uint64_t LogSizeBytes, class Base>
class TransactionCustomLogSegmentSizeTest : public Base {
  // We need multiple log segments in this test.
  uint64_t log_segment_size_bytes() const override {
    return LogSizeBytes;
  }
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_TXN_TEST_BASE_H
