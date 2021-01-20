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

#include "yb/client/transaction_pool.h"

#include <deque>

#include "yb/client/client.h"
#include "yb/client/transaction.h"
#include "yb/client/transaction_manager.h"

#include "yb/gutil/thread_annotations.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/scheduler.h"

#include "yb/util/metrics.h"

using namespace std::literals;
using namespace std::placeholders;

DEFINE_int32(transaction_pool_cleanup_interval_ms, 5000,
             "How frequently we should cleanup transaction pool");

DEFINE_double(transaction_pool_reserve_factor, 2,
              "During cleanup we will preserve number of transactions in pool that equals to"
                  " average number or take requests during prepration multiplied by this factor");


METRIC_DEFINE_histogram(
    server, transaction_pool_cache, "Rate of hitting transaction pool cache",
    yb::MetricUnit::kCacheHits, "Rate of hitting transaction pool cache", 100LU, 2);

METRIC_DEFINE_counter(server, transaction_pool_cache_hits,
                      "Total number of hits in transaction pool cache", yb::MetricUnit::kCacheHits,
                      "Total number of hits in transaction pool cache");
METRIC_DEFINE_counter(server, transaction_pool_cache_queries,
                      "Total number of queries to transaction pool cache",
                      yb::MetricUnit::kCacheQueries,
                      "Total number of queries to transaction pool cache");

METRIC_DEFINE_gauge_uint32(
    server, transaction_pool_preparing, "Number of preparing transactions in pool",
    yb::MetricUnit::kTransactions, "Number of preparing transactions in pool");

METRIC_DEFINE_gauge_uint32(
    server, transaction_pool_prepared, "Number of prepared transactions in pool",
    yb::MetricUnit::kTransactions, "Number of prepared transactions in pool");

namespace yb {
namespace client {

class TransactionPool::Impl {
 public:
  Impl(TransactionManager* manager, MetricEntity* metric_entity)
      : manager_(*manager) {
    if (metric_entity) {
      cache_histogram_ = METRIC_transaction_pool_cache.Instantiate(metric_entity);
      cache_hits_ = METRIC_transaction_pool_cache_hits.Instantiate(metric_entity);
      cache_queries_ = METRIC_transaction_pool_cache_queries.Instantiate(metric_entity);
      gauge_preparing_ = METRIC_transaction_pool_preparing.Instantiate(metric_entity, 0);
      gauge_prepared_ = METRIC_transaction_pool_prepared.Instantiate(metric_entity, 0);
    }
  }

  ~Impl() {
    std::unique_lock<std::mutex> lock(mutex_);
    closing_ = true;
    if (scheduled_task_ != rpc::kUninitializedScheduledTaskId) {
      manager_.client()->messenger()->scheduler().Abort(scheduled_task_);
    }
    // Have to use while, since GUARDED_BY does not understand cond wait with predicate.
    while (!Idle()) {
      cond_.wait(lock);
    }
  }

  YBTransactionPtr Take() {
    YBTransactionPtr result, new_txn;
    uint64_t old_taken;
    IncrementCounter(cache_queries_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      old_taken = taken_transactions_;
      ++taken_transactions_;
      // We create new transaction on each take request, does not matter whether is was
      // newly created or not. So number of transactions in pool will near average number of take
      // requests during transaction preparation.
      if (transactions_.empty()) {
        // Transaction is automatically prepared when batcher is executed, so we don't have to
        // prepare newly created transaction, since it is anyway too late.
        result = std::make_shared<YBTransaction>(&manager_);
        IncrementHistogram(cache_histogram_, 0);
      } else {
        result = Pop();
        // Cache histogram should show number of cache hits in percents, so we put 100 in case of
        // hit.
        IncrementHistogram(cache_histogram_, 100);
        IncrementCounter(cache_hits_);
      }
      new_txn = std::make_shared<YBTransaction>(&manager_);
      ++preparing_transactions_;
    }
    IncrementGauge(gauge_preparing_);
    InFlightOpsGroupsWithMetadata ops_info;
    if (new_txn->Prepare(
        &ops_info, ForceConsistentRead::kFalse, TransactionRpcDeadline(), Initial::kFalse,
        std::bind(&Impl::TransactionReady, this, _1, new_txn, old_taken))) {
      TransactionReady(Status::OK(), new_txn, old_taken);
    }
    return result;
  }

 private:
  void TransactionReady(
      const Status& status, const YBTransactionPtr& txn, uint64_t taken_before_creation) {
    if (status.ok()) {
      IncrementGauge(gauge_prepared_);
    }
    DecrementGauge(gauge_preparing_);

    std::lock_guard<std::mutex> lock(mutex_);
    if (status.ok()) {
      uint64_t taken_during_preparation = taken_transactions_ - taken_before_creation;
      taken_during_preparation_sum_ += taken_during_preparation;
      transactions_.push_back({ txn, taken_during_preparation });
    }
    --preparing_transactions_;
    if (CheckClosing()) {
      return;
    }
    if (transactions_.size() == 1 && scheduled_task_ == rpc::kUninitializedScheduledTaskId) {
      ScheduleCleanup();
    }
  }

  void ScheduleCleanup() REQUIRES(mutex_) {
    scheduled_task_ = manager_.client()->messenger()->scheduler().Schedule(
        std::bind(&Impl::Cleanup, this, _1), FLAGS_transaction_pool_cleanup_interval_ms * 1ms);
  }

  void Cleanup(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduled_task_ = rpc::kUninitializedScheduledTaskId;
    if (CheckClosing()) {
      return;
    }

    if (taken_transactions_at_last_cleanup_ == taken_transactions_) {
      // No transactions were taken since last cleanup, could abort all transactions.
      while (!transactions_.empty()) {
        Pop()->Abort();
      }
      return;
    }
    taken_transactions_at_last_cleanup_ = taken_transactions_;

#ifndef NDEBUG
    // Check that taken_during_preparation_sum_ reflects actual sum of taken_during_preparation,
    // of transactions in pool.
    {
      uint64_t taken_during_preparation_sum = 0;
      for (const auto& entry : transactions_) {
        taken_during_preparation_sum += entry.taken_during_preparation;
      }
      CHECK_EQ(taken_during_preparation_sum, taken_during_preparation_sum_);
    }
#endif
    // For each prepared transaction we know number of take requests that happened while this
    // transaction were prepared. So we try to keep number of prepared transactions
    // as average of this value + 50%.
    // taken_during_preparation_sum_ is sum of this values so we should divide it by size
    // to get average value.

    size_t size = transactions_.size();

    // Skip cleanup if we have too small amount of prepared transactions.
    if (preparing_transactions_ < size) {
      size_t min_size = size * 4 / 5;
      while (size > min_size &&
          ((size + preparing_transactions_) * size >
              taken_during_preparation_sum_ * FLAGS_transaction_pool_reserve_factor)) {
        Pop()->Abort();
        --size;
      }
    }
    if (!transactions_.empty()) {
      ScheduleCleanup();
    }
  }

  YBTransactionPtr Pop() REQUIRES(mutex_) {
    DecrementGauge(gauge_prepared_);
    YBTransactionPtr result = std::move(transactions_.front().transaction);
    taken_during_preparation_sum_ -= transactions_.front().taken_during_preparation;
    transactions_.pop_front();
    return result;
  }

  bool CheckClosing() REQUIRES(mutex_) {
    if (!closing_) {
      return false;
    }
    if (Idle()) {
      cond_.notify_all();
    }
    return true;
  }

  bool Idle() const REQUIRES(mutex_) {
    LOG(INFO) << "preparing_transactions: " << preparing_transactions_
              << ", scheduled_task: " << scheduled_task_;
    return preparing_transactions_ == 0 && scheduled_task_ == rpc::kUninitializedScheduledTaskId;
  }

  struct TransactionEntry {
    YBTransactionPtr transaction;
    uint64_t taken_during_preparation;
  };

  TransactionManager& manager_;
  scoped_refptr<Histogram> cache_histogram_;
  scoped_refptr<Counter> cache_hits_;
  scoped_refptr<Counter> cache_queries_;
  scoped_refptr<AtomicGauge<uint32_t>> gauge_preparing_;
  scoped_refptr<AtomicGauge<uint32_t>> gauge_prepared_;
  std::mutex mutex_;
  std::condition_variable cond_;
  std::deque<TransactionEntry> transactions_ GUARDED_BY(mutex_);
  bool closing_ GUARDED_BY(mutex_) = false;
  int preparing_transactions_ GUARDED_BY(mutex_) = 0;
  rpc::ScheduledTaskId scheduled_task_ GUARDED_BY(mutex_) = rpc::kUninitializedScheduledTaskId;
  uint64_t taken_transactions_ GUARDED_BY(mutex_) = 0;
  uint64_t taken_during_preparation_sum_ GUARDED_BY(mutex_) = 0;
  uint64_t taken_transactions_at_last_cleanup_ GUARDED_BY(mutex_) = 0;
};

TransactionPool::TransactionPool(TransactionManager* manager, MetricEntity* metric_entity)
    : impl_(new Impl(manager, metric_entity)) {
}

TransactionPool::~TransactionPool() {
}

YBTransactionPtr TransactionPool::Take() {
  return impl_->Take();
}

Result<YBTransactionPtr> TransactionPool::TakeAndInit(
    IsolationLevel isolation, const ReadHybridTime& read_time) {
  auto result = impl_->Take();
  RETURN_NOT_OK(result->Init(isolation, read_time));
  return result;
}

Result<YBTransactionPtr> TransactionPool::TakeRestarted(const YBTransactionPtr& source) {
  auto result = impl_->Take();
  RETURN_NOT_OK(source->FillRestartedTransaction(result));
  return result;
}

} // namespace client
} // namespace yb
