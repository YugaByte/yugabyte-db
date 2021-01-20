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

#include "yb/client/txn-test-base.h"

#include "yb/client/session.h"
#include "yb/client/transaction.h"

#include "yb/util/async_util.h"
#include "yb/util/bfql/gen_opcodes.h"
#include "yb/util/random_util.h"

#include "yb/yql/cql/ql/util/statement_result.h"

using namespace std::literals;

DECLARE_int64(transaction_rpc_timeout_ms);
DECLARE_int32(txn_max_apply_batch_records);

namespace yb {
namespace client {

class SerializableTxnTest
    : public TransactionCustomLogSegmentSizeTest<0, TransactionTestBase<MiniCluster>> {
 protected:
  void SetUp() override {
    SetIsolationLevel(IsolationLevel::SERIALIZABLE_ISOLATION);
    TransactionTestBase::SetUp();
  }

  void TestIncrements(bool transactional);
  void TestIncrement(int key, bool transactional);
  void TestColoring();
};

TEST_F(SerializableTxnTest, NonConflictingWrites) {
  const auto kTransactions = 10;
  const auto kKey = 0;

  struct Entry {
    YBTransactionPtr txn;
    YBqlWriteOpPtr op;
    std::future<Status> flush_future;
    std::future<Status> commit_future;
    bool done = false;
  };

  std::vector<Entry> entries;
  for (int i = 0; i != kTransactions; ++i) {
    entries.emplace_back();
    auto& entry = entries.back();
    entry.txn = CreateTransaction();
    auto session = CreateSession(entry.txn);
    entry.op = ASSERT_RESULT(WriteRow(session, kKey, i));
    entry.flush_future = session->FlushFuture();
  }

  ASSERT_OK(WaitFor([&entries]() -> Result<bool> {
    for (auto& entry : entries) {
      if (entry.flush_future.valid() && IsReady(entry.flush_future)) {
        LOG(INFO) << "Flush done";
        RETURN_NOT_OK(entry.flush_future.get());
        entry.commit_future = entry.txn->CommitFuture();
      }
    }

    for (auto& entry : entries) {
      if (entry.commit_future.valid() && IsReady(entry.commit_future)) {
        LOG(INFO) << "Commit done";
        RETURN_NOT_OK(entry.commit_future.get());
        entry.done = true;
      }
    }

    for (const auto& entry : entries) {
      if (!entry.done) {
        return false;
      }
    }

    return true;
  }, 10s, "Complete all operations"));

  for (const auto& entry : entries) {
    ASSERT_EQ(entry.op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }
}

TEST_F(SerializableTxnTest, ReadWriteConflict) {
  const auto kKeys = 20;

  size_t reads_won = 0, writes_won = 0;
  for (int i = 0; i != kKeys; ++i) {
    auto read_txn = CreateTransaction();
    auto read_session = CreateSession(read_txn);
    auto read = ReadRow(read_session, i);
    ASSERT_OK(read_session->Flush());

    auto write_txn = CreateTransaction();
    auto write_session = CreateSession(write_txn);
    auto write_status = ResultToStatus(WriteRow(
        write_session, i, i, WriteOpType::INSERT, Flush::kTrue));

    auto read_commit_future = read_txn->CommitFuture();
    if (write_status.ok()) {
      write_status = write_txn->CommitFuture().get();
    }
    auto read_status = read_commit_future.get();

    LOG(INFO) << "Read: " << read_status << ", write: " << write_status;

    if (!read_status.ok()) {
      ASSERT_OK(write_status);
      ++writes_won;
    } else {
      ASSERT_NOK(write_status);
      ++reads_won;
    }
  }

  LOG(INFO) << "Reads won: " << reads_won << ", writes won: " << writes_won;
  ASSERT_GE(reads_won, kKeys / 4);
  ASSERT_GE(writes_won, kKeys / 4);
}

// Execute UPDATE table SET value = value + 1 WHERE key = kKey in parallel, using
// serializable isolation.
// With retries the resulting value should be equal to number of increments.
void SerializableTxnTest::TestIncrement(int key, bool transactional) {
  const auto kIncrements = RegularBuildVsSanitizers(100, 20);

  {
    auto session = CreateSession();
    auto op = ASSERT_RESULT(WriteRow(session, key, 0));
    ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  struct Entry {
    YBqlWriteOpPtr op;
    YBTransactionPtr txn;
    YBSessionPtr session;
    std::shared_future<Status> write_future;
    std::shared_future<Status> commit_future;
  };

  std::vector<Entry> entries;

  for (int i = 0; i != kIncrements; ++i) {
    Entry entry;
    entry.txn = transactional ? CreateTransaction() : nullptr;
    entry.session = CreateSession(entry.txn, clock_);
    entry.session->SetReadPoint(Restart::kFalse);
    entries.push_back(entry);
  }

  // For each of entries we do the following:
  // 1) Write increment operation.
  // 2) Wait until write complete and commit transaction of this entry.
  // 3) Wait until commit complete.
  // When failure happens on any step - retry from step 1.
  // Exit from loop when all entries successfully committed their transactions.
  // We do all actions in busy loop to get most possible concurrency for operations.
  for (;;) {
    bool incomplete = false;
    for (auto& entry : entries) {
      bool entry_complete = false;
      if (!entry.op) {
        // Execute UPDATE table SET value = value + 1 WHERE key = kKey
        entry.session->SetTransaction(entry.txn);
        entry.op = ASSERT_RESULT(kv_table_test::Increment(&table_, entry.session, key));
        entry.write_future = entry.session->FlushFuture();
      } else if (entry.write_future.valid()) {
        if (IsReady(entry.write_future)) {
          auto write_status = entry.write_future.get();
          entry.write_future = std::shared_future<Status>();
          if (!write_status.ok()) {
            ASSERT_TRUE(write_status.IsTryAgain() ||
                        ((write_status.IsTimedOut() || write_status.IsServiceUnavailable())
                            && transactional)) << write_status;
            entry.txn = transactional ? CreateTransaction() : nullptr;
            entry.op = nullptr;
          } else {
            if (entry.op->response().status() == QLResponsePB::YQL_STATUS_RESTART_REQUIRED_ERROR) {
              auto old_txn = entry.txn;
              if (transactional) {
                entry.txn = ASSERT_RESULT(entry.txn->CreateRestartedTransaction());
              } else {
                entry.session->SetReadPoint(Restart::kTrue);
              }
              entry.op = nullptr;
            } else {
              ASSERT_EQ(entry.op->response().status(), QLResponsePB::YQL_STATUS_OK);
              if (transactional) {
                entry.commit_future = entry.txn->CommitFuture();
              }
            }
          }
        }
      } else if (entry.commit_future.valid()) {
        if (IsReady(entry.commit_future)) {
          auto status = entry.commit_future.get();
          if (status.IsExpired()) {
            entry.txn = transactional ? CreateTransaction() : nullptr;
            entry.op = nullptr;
          } else {
            ASSERT_OK(status);
            entry.commit_future = std::shared_future<Status>();
          }
        }
      } else {
        entry_complete = true;
      }
      incomplete = incomplete || !entry_complete;
    }
    if (!incomplete) {
      break;
    }
  }

  auto value = ASSERT_RESULT(SelectRow(CreateSession(), key));
  ASSERT_EQ(value, kIncrements);
}

// Execute UPDATE table SET value = value + 1 WHERE key = kKey in parallel, using
// serializable isolation.
// With retries the resulting value should be equal to number of increments.
void SerializableTxnTest::TestIncrements(bool transactional) {
  FLAGS_transaction_rpc_timeout_ms = MonoDelta(1min).ToMilliseconds();

  const auto kThreads = RegularBuildVsSanitizers(3, 2);

  std::vector<std::thread> threads;
  while (threads.size() != kThreads) {
    int key = threads.size();
    threads.emplace_back([this, key, transactional] {
      CDSAttacher attacher;
      TestIncrement(key, transactional);
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }
}

TEST_F(SerializableTxnTest, Increment) {
  TestIncrements(true /* transactional */);
}

TEST_F(SerializableTxnTest, IncrementNonTransactional) {
  TestIncrements(false /* transactional */);
}

// Test that repeats example from this article:
// https://blogs.msdn.microsoft.com/craigfr/2007/05/16/serializable-vs-snapshot-isolation-level/
//
// Multiple rows with values 0 and 1 are stored in table.
// Two concurrent transaction fetches all rows from table and does the following.
// First transaction changes value of all rows with value 0 to 1.
// Second transaction changes value of all rows with value 1 to 0.
// As outcome we should have rows with the same value.
//
// The described prodecure is repeated multiple times to increase probability of catching bug,
// w/o running test multiple times.
void SerializableTxnTest::TestColoring() {
  constexpr auto kKeys = 20;
  constexpr auto kColors = 2;
  constexpr auto kIterations = 20;

  size_t iterations_left = kIterations;
  for (int i = 0; iterations_left > 0 && !testing::Test::HasFailure(); ++i) {
    SCOPED_TRACE(Format("Iteration: $0", i));

    auto session = CreateSession(nullptr /* transaction */, clock_);
    session->SetForceConsistentRead(ForceConsistentRead::kTrue);

    {
      std::vector<YBqlWriteOpPtr> ops;
      for (int i = 0; i != kKeys; ++i) {
        auto color = RandomUniformInt(0, kColors - 1);
        ops.push_back(ASSERT_RESULT(WriteRow(session,
            i,
            color,
            WriteOpType::INSERT,
            Flush::kFalse)));
      }

      ASSERT_OK(session->Flush());

      for (const auto& op : ops) {
        ASSERT_OK(CheckOp(op.get()));
      }
    }

    std::vector<std::thread> threads;
    std::atomic<size_t> successes(0);

    while (threads.size() != kColors) {
      int32_t color = threads.size();
      threads.emplace_back([this, color, &successes, kKeys] {
        CDSAttacher attacher;
        for (;;) {
          auto txn = CreateTransaction();
          LOG(INFO) << "Start: " << txn->id() << ", color: " << color;
          auto session = CreateSession(txn);
          session->SetTransaction(txn);
          auto values = SelectAllRows(session);
          if (!values.ok()) {
            ASSERT_TRUE(values.status().IsTryAgain()) << values.status();
            continue;
          }
          ASSERT_EQ(values->size(), kKeys);

          std::vector<YBqlWriteOpPtr> ops;
          for (const auto& p : *values) {
            if (p.second == color) {
              continue;
            }
            ops.push_back(ASSERT_RESULT(WriteRow(
                session, p.first, color, WriteOpType::INSERT, Flush::kFalse)));
          }

          if (ops.empty()) {
            break;
          }

          auto flush_status = session->Flush();
          if (!flush_status.ok()) {
            ASSERT_TRUE(flush_status.IsTryAgain()) << flush_status;
            break;
          }

          for (const auto& op : ops) {
            ASSERT_OK(CheckOp(op.get()));
          }

          LOG(INFO) << "Commit: " << txn->id() << ", color: " << color;
          auto commit_status = txn->CommitFuture().get();
          if (!commit_status.ok()) {
            ASSERT_TRUE(commit_status.IsExpired()) << commit_status;
            break;
          }

          ++successes;
          break;
        }
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    if (successes == 0) {
      continue;
    }

    session->SetReadPoint(Restart::kFalse);
    auto values = ASSERT_RESULT(SelectAllRows(session));
    ASSERT_EQ(values.size(), kKeys);
    LOG(INFO) << "Values: " << yb::ToString(values);
    int32_t color = -1;
    for (const auto& p : values) {
      if (color == -1) {
        color = p.second;
      } else {
        ASSERT_EQ(color, p.second);
      }
    }
    --iterations_left;
  }
}

TEST_F(SerializableTxnTest, Coloring) {
  TestColoring();
}

TEST_F(SerializableTxnTest, ColoringWithLongApply) {
  FLAGS_txn_max_apply_batch_records = 3;
  TestColoring();
}

} // namespace client
} // namespace yb
