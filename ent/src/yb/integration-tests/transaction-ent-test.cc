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

#include <boost/container/stable_vector.hpp>

#include "yb/client/ql-dml-test-base.h"
#include "yb/client/session.h"
#include "yb/client/transaction.h"

#include "yb/server/hybrid_clock.h"
#include "yb/server/random_error_clock.h"

#include "yb/util/random_util.h"

#include "yb/yql/cql/ql/util/errcodes.h"

using namespace std::literals;

DECLARE_string(time_source);
DECLARE_int32(intents_flush_max_delay_ms);
DECLARE_bool(fail_on_out_of_range_clock_skew);

namespace yb {

class TransactionEntTest : public client::KeyValueTableTest<MiniCluster> {
 protected:
  virtual ~TransactionEntTest() {}

  void SetUp() override {
    FLAGS_fail_on_out_of_range_clock_skew = false;

    server::RandomErrorClock::Register();
    FLAGS_time_source = server::RandomErrorClock::kNtpName;
    KeyValueTableTest::SetUp();

    CreateTable(client::Transactional::kTrue);

    FLAGS_intents_flush_max_delay_ms = 250;

    HybridTime::TEST_SetPrettyToString(true);
  }

  client::TransactionManager& CreateTransactionManager() {
    auto random_error_clock = server::RandomErrorClock::CreateNtpClock();
    server::ClockPtr clock(new server::HybridClock(random_error_clock));
    EXPECT_OK(clock->Init());

    std::lock_guard<std::mutex> lock(transaction_managers_mutex_);
    transaction_managers_.emplace_back(client_.get(), clock, client::LocalTabletFilter());
    return transaction_managers_.back();
  }

  std::mutex transaction_managers_mutex_;
  boost::container::stable_vector<client::TransactionManager> transaction_managers_;
};


TEST_F(TransactionEntTest, RandomErrorClock) {
  static constexpr size_t kNumReaders = 3;
  static constexpr size_t kNumKeys = 5;

  struct RandomErrorClockShare {
    std::atomic<bool> stopped{false};
    std::array<std::atomic<int>, kNumKeys> values;
    std::atomic<size_t> reads{0};
  };

  std::vector<std::thread> threads;
  RandomErrorClockShare share;
  for (int32_t key = 0; key != share.values.size(); ++key) {
    share.values[key].store(0, std::memory_order_release);
    ASSERT_OK(WriteRow(CreateSession(), key, 0));
  }

  while (threads.size() < share.values.size()) {
    threads.emplace_back([this, &share, key = threads.size()] {
      CDSAttacher attacher;
      auto& transaction_manager = CreateTransactionManager();
      auto session = CreateSession();
      while (!share.stopped.load(std::memory_order_acquire)) {
        auto transaction = std::make_shared<client::YBTransaction>(&transaction_manager);
        ASSERT_OK(transaction->Init(IsolationLevel::SNAPSHOT_ISOLATION));
        session->SetTransaction(transaction);
        auto value = share.values[key].load(std::memory_order_acquire);
        auto new_value = value + 1;
        auto write_result = WriteRow(session, key, new_value);
        if (!write_result.ok()) {
          continue;
        }
        auto status = transaction->CommitFuture().get();
        if (status.ok()) {
          share.values[key].store(new_value, std::memory_order_release);
        } else {
          ASSERT_TRUE(status.IsTryAgain() || status.IsExpired()) << "Bad status: " << status;
        }
      }
    });
  }

  for (auto& value : share.values) {
    while (value.load(std::memory_order_acquire) == 0) {
      std::this_thread::sleep_for(10ms);
    }
  }

  while (threads.size() < share.values.size() + kNumReaders) {
    threads.emplace_back([this, &share] {
      CDSAttacher attacher;
      // We need separate transaction manager for each thread to have different clocks for different
      // threads.
      auto& transaction_manager = CreateTransactionManager();
      auto session = CreateSession();
      client::YBTransactionPtr transaction;
      int32_t key = 0;
      int32_t value = 0;
      while (!share.stopped.load(std::memory_order_acquire)) {
        if (!transaction) {
          key = RandomUniformInt<int32_t>(0, share.values.size() - 1);
          value = share.values[key].load(std::memory_order_acquire);
          transaction = std::make_shared<client::YBTransaction>(&transaction_manager);
          ASSERT_OK(transaction->Init(IsolationLevel::SNAPSHOT_ISOLATION));
        }

        session->SetTransaction(transaction);
        auto read_value = SelectRow(session, key);
        if (read_value.ok()) {
          ASSERT_GE(*read_value, value) << "Key: " << key;
          share.reads.fetch_add(1, std::memory_order_release);
          transaction->Abort();
          transaction = nullptr;
        } else {
          ASSERT_EQ(ql::ErrorCode::RESTART_REQUIRED, ql::GetErrorCode(read_value.status()))
              << "Bad value: " << read_value;
          transaction = ASSERT_RESULT(transaction->CreateRestartedTransaction());
        }
      }
    });
  }

  std::this_thread::sleep_for(10s);

  share.stopped.store(true, std::memory_order_release);

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_GT(share.reads.load(std::memory_order_acquire), 0);

  for (size_t key = 0; key != share.values.size(); ++key) {
    LOG(INFO) << key << " => " << share.values[key].load(std::memory_order_acquire);
    EXPECT_GT(share.values[key].load(std::memory_order_acquire), 0) << "Key: " << key;
  }
}

} // namespace yb
