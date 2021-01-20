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

#include "yb/client/transaction_manager.h"

#include "yb/rpc/rpc.h"
#include "yb/rpc/thread_pool.h"
#include "yb/rpc/tasks_pool.h"

#include "yb/util/random_util.h"
#include "yb/util/thread_restrictions.h"

#include "yb/client/client.h"

#include "yb/common/transaction.h"

#include "yb/master/master_defaults.h"

DEFINE_uint64(transaction_manager_workers_limit, 50,
              "Max number of workers used by transaction manager");

DEFINE_uint64(transaction_manager_queue_limit, 500,
              "Max number of tasks used by transaction manager");

namespace yb {
namespace client {

namespace {

const YBTableName kTransactionTableName(
    YQL_DATABASE_CQL, master::kSystemNamespaceName, kTransactionsTableName);

// Exists - table exists.
// Updating - intermediate state, we are currently updating local cache of tablets.
// Resolved - final state, when all tablets are resolved and written to cache.
YB_DEFINE_ENUM(TransactionTableStatus, (kExists)(kUpdating)(kResolved));

void InvokeCallback(const LocalTabletFilter& filter, const std::vector<TabletId>& tablets,
                    const PickStatusTabletCallback& callback) {
  if (filter) {
    std::vector<const TabletId*> ids;
    ids.reserve(tablets.size());
    for (const auto& id : tablets) {
      ids.push_back(&id);
    }
    filter(&ids);
    if (!ids.empty()) {
      callback(*RandomElement(ids));
      return;
    }
    YB_LOG_EVERY_N_SECS(WARNING, 1) << "No local transaction status tablet";
  }
  callback(RandomElement(tablets));
}

struct TransactionTableState {
  LocalTabletFilter local_tablet_filter;
  std::atomic<TransactionTableStatus> status{TransactionTableStatus::kExists};
  std::vector<TabletId> tablets;
};

// Picks status tablet for transaction.
class PickStatusTabletTask {
 public:
  PickStatusTabletTask(YBClient* client,
                       TransactionTableState* table_state,
                       PickStatusTabletCallback callback)
      : client_(client), table_state_(table_state),
        callback_(std::move(callback)) {
  }

  void Run() {
    // TODO(dtxn) async
    auto tablets_result = GetTransactionTableTablets();
    if (!tablets_result) {
      VLOG(1) << "Failed to get tablets of txn status table: " << tablets_result.status();
      callback_(tablets_result.status());
      return;
    }
    const auto tablets = std::move(*tablets_result);
    auto expected = TransactionTableStatus::kExists;
    if (table_state_->status.compare_exchange_strong(
        expected, TransactionTableStatus::kUpdating, std::memory_order_acq_rel)) {
      table_state_->tablets = tablets;
      table_state_->status.store(TransactionTableStatus::kResolved, std::memory_order_release);
    }

    InvokeCallback(table_state_->local_tablet_filter, tablets, callback_);
  }

  void Done(const Status& status) {
    if (!status.ok()) {
      callback_(status);
    }
    callback_ = PickStatusTabletCallback();
    client_ = nullptr;
  }

 private:
  Result<std::vector<TabletId>> GetTransactionTableTablets() {
    std::vector<TabletId> tablets;
    if (!FetchTransactionTableTablets(&tablets).ok()) {
      // Tablets for txn status table are not ready yet.
      // Wait for table creation completion and try again.
      RETURN_NOT_OK(client_->WaitForCreateTableToFinish(kTransactionTableName));
      RETURN_NOT_OK(FetchTransactionTableTablets(&tablets));
    }
    SCHECK(!tablets.empty(), IllegalState, Format("No tablets in table $0", kTransactionTableName));
    return std::move(tablets);
  }

  CHECKED_STATUS FetchTransactionTableTablets(std::vector<TabletId>* tablets) {
    return client_->GetTablets(kTransactionTableName,
                               0 /* max_tablets */,
                               tablets,
                               nullptr /* ranges */,
                               nullptr /* locations */,
                               RequireTabletsRunning::kTrue);
  }

  YBClient* client_;
  TransactionTableState* table_state_;
  PickStatusTabletCallback callback_;
};

class InvokeCallbackTask {
 public:
  InvokeCallbackTask(TransactionTableState* table_state,
                     PickStatusTabletCallback callback)
      : table_state_(table_state), callback_(std::move(callback)) {
  }

  void Run() {
    InvokeCallback(table_state_->local_tablet_filter, table_state_->tablets, callback_);
  }

  void Done(const Status& status) {
    if (!status.ok()) {
      callback_(status);
    }
    callback_ = PickStatusTabletCallback();
  }

 private:
  TransactionTableState* table_state_;
  PickStatusTabletCallback callback_;
};

} // namespace

class TransactionManager::Impl {
 public:
  explicit Impl(YBClient* client, const scoped_refptr<ClockBase>& clock,
                LocalTabletFilter local_tablet_filter)
      : client_(client),
        clock_(clock),
        table_state_{std::move(local_tablet_filter)},
        thread_pool_(
            "TransactionManager", FLAGS_transaction_manager_queue_limit,
            FLAGS_transaction_manager_workers_limit),
        tasks_pool_(FLAGS_transaction_manager_queue_limit),
        invoke_callback_tasks_(FLAGS_transaction_manager_queue_limit) {
    CHECK(clock);
  }

  ~Impl() {
    Shutdown();
  }

  void PickStatusTablet(PickStatusTabletCallback callback) {
    if (table_state_.status.load(std::memory_order_acquire) == TransactionTableStatus::kResolved) {
      if (ThreadRestrictions::IsWaitAllowed()) {
        InvokeCallback(table_state_.local_tablet_filter, table_state_.tablets, callback);
      } else if (!invoke_callback_tasks_.Enqueue(&thread_pool_, &table_state_, callback)) {
        callback(STATUS_FORMAT(ServiceUnavailable,
                              "Invoke callback queue overflow, number of tasks: $0",
                              invoke_callback_tasks_.size()));
      }
      return;
    }
    if (!tasks_pool_.Enqueue(&thread_pool_, client_, &table_state_, std::move(callback))) {
      callback(STATUS_FORMAT(ServiceUnavailable, "Tasks overflow, exists: $0", tasks_pool_.size()));
    }
  }

  const scoped_refptr<ClockBase>& clock() const {
    return clock_;
  }

  YBClient* client() const {
    return client_;
  }

  rpc::Rpcs& rpcs() {
    return rpcs_;
  }

  HybridTime Now() const {
    return clock_->Now();
  }

  HybridTimeRange NowRange() const {
    return clock_->NowRange();
  }

  void UpdateClock(HybridTime time) {
    clock_->Update(time);
  }

  void Shutdown() {
    rpcs_.Shutdown();
    thread_pool_.Shutdown();
  }

 private:
  YBClient* const client_;
  scoped_refptr<ClockBase> clock_;
  TransactionTableState table_state_;
  std::atomic<bool> closed_{false};
  yb::rpc::ThreadPool thread_pool_; // TODO async operations instead of pool
  yb::rpc::TasksPool<PickStatusTabletTask> tasks_pool_;
  yb::rpc::TasksPool<InvokeCallbackTask> invoke_callback_tasks_;
  yb::rpc::Rpcs rpcs_;
};

TransactionManager::TransactionManager(
    YBClient* client, const scoped_refptr<ClockBase>& clock,
    LocalTabletFilter local_tablet_filter)
    : impl_(new Impl(client, clock, std::move(local_tablet_filter))) {}

TransactionManager::~TransactionManager() = default;

void TransactionManager::PickStatusTablet(PickStatusTabletCallback callback) {
  impl_->PickStatusTablet(std::move(callback));
}

YBClient* TransactionManager::client() const {
  return impl_->client();
}

rpc::Rpcs& TransactionManager::rpcs() {
  return impl_->rpcs();
}

const scoped_refptr<ClockBase>& TransactionManager::clock() const {
  return impl_->clock();
}

HybridTime TransactionManager::Now() const {
  return impl_->Now();
}

HybridTimeRange TransactionManager::NowRange() const {
  return impl_->NowRange();
}

void TransactionManager::UpdateClock(HybridTime time) {
  impl_->UpdateClock(time);
}

TransactionManager::TransactionManager(TransactionManager&& rhs) = default;
TransactionManager& TransactionManager::operator=(TransactionManager&& rhs) = default;

} // namespace client
} // namespace yb
