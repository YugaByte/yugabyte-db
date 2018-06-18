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

#include "yb/client/client.h"

DEFINE_uint64(transaction_table_num_tablets, 24,
              "Automatically create transaction table with specified number of tablets if missing. "
              "0 to disable.");

namespace yb {
namespace client {

namespace {

const YBTableName kTransactionTableName("system", "transactions");

// Idle - initial state, don't know actual state of table.
// Exists - table exists.
// Updating - intermediate state, we are currently updating local cache of tablets.
// Resolved - final state, when all tablets are resolved and written to cache.
YB_DEFINE_ENUM(TransactionTableStatus, (kIdle)(kExists)(kUpdating)(kResolved));

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
    LOG(WARNING) << "No local transaction status tablet";
  }
  callback(RandomElement(tablets));
}

struct TransactionTableState {
  LocalTabletFilter local_tablet_filter;
  std::atomic<TransactionTableStatus> status{TransactionTableStatus::kIdle};
  std::vector<TabletId> tablets;
};

// Picks status tablet for transaction.
class PickStatusTabletTask {
 public:
  PickStatusTabletTask(const YBClientPtr& client,
                       TransactionTableState* table_state,
                       PickStatusTabletCallback callback)
      : client_(client), table_state_(table_state),
        callback_(std::move(callback)) {
  }

  void Run() {
    auto status = EnsureStatusTableExists();
    if (!status.ok()) {
      callback_(status.CloneAndPrepend("Failed to create status table"));
      return;
    }

    // TODO(dtxn) async
    std::vector<TabletId> tablets;
    status = client_->GetTablets(kTransactionTableName, 0, &tablets, /* ranges */ nullptr);
    if (!status.ok()) {
      callback_(status);
      return;
    }
    if (tablets.empty()) {
      callback_(STATUS_FORMAT(IllegalState, "No tablets in table $0", kTransactionTableName));
      return;
    }
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
    client_.reset();
  }

 private:
  CHECKED_STATUS EnsureStatusTableExists() {
    if (table_state_->status.load(std::memory_order_acquire) != TransactionTableStatus::kIdle) {
      return Status::OK();
    }

    std::shared_ptr<YBTable> table;
    constexpr int kNumRetries = 5;
    Status status;
    for (int i = 0; i != kNumRetries; ++i) {
      status = client_->OpenTable(kTransactionTableName, &table);
      if (status.ok()) {
        break;
      }
      LOG(WARNING) << "Failed to open transaction table: " << status.ToString();
      auto tablets = FLAGS_transaction_table_num_tablets;
      if (tablets > 0 && status.IsNotFound()) {
        status = client_->CreateNamespaceIfNotExists(kTransactionTableName.namespace_name());
        if (status.ok()) {
          std::unique_ptr<client::YBTableCreator> table_creator(client_->NewTableCreator());
          table_creator->num_tablets(tablets);
          table_creator->table_type(client::YBTableType::REDIS_TABLE_TYPE);
          table_creator->table_name(kTransactionTableName);
          status = table_creator->Create();
          LOG_IF(DFATAL, !status.ok() && !status.IsAlreadyPresent())
              << "Failed to create transaction table: " << status;
        } else {
          LOG(DFATAL) << "Failed to create namespace: " << status;
        }
      }
    }
    if (status.ok()) {
      auto expected = TransactionTableStatus::kIdle;
      table_state_->status.compare_exchange_strong(
          expected, TransactionTableStatus::kExists, std::memory_order_acq_rel);
    }
    return status;
  }

  YBClientPtr client_;
  TransactionTableState* table_state_;
  PickStatusTabletCallback callback_;
};

constexpr size_t kQueueLimit = 150;
constexpr size_t kMaxWorkers = 50;

} // namespace

class TransactionManager::Impl {
 public:
  explicit Impl(const YBClientPtr& client, const scoped_refptr<ClockBase>& clock,
                LocalTabletFilter local_tablet_filter)
      : client_(client),
        clock_(clock),
        table_state_{std::move(local_tablet_filter)},
        thread_pool_("TransactionManager", kQueueLimit, kMaxWorkers),
        tasks_pool_(kQueueLimit) {
    CHECK(clock);
  }

  void PickStatusTablet(PickStatusTabletCallback callback) {
    if (table_state_.status.load(std::memory_order_acquire) == TransactionTableStatus::kResolved) {
      InvokeCallback(table_state_.local_tablet_filter, table_state_.tablets, callback);
      return;
    }
    if (!tasks_pool_.Enqueue(&thread_pool_, client_, &table_state_, std::move(callback))) {
      callback(STATUS_FORMAT(ServiceUnavailable, "Tasks overflow, exists: $0", tasks_pool_.size()));
    }
  }

  const scoped_refptr<ClockBase>& clock() const {
    return clock_;
  }

  const YBClientPtr& client() const {
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
  }

 private:
  YBClientPtr client_;
  scoped_refptr<ClockBase> clock_;
  TransactionTableState table_state_;
  std::atomic<bool> closed_{false};
  yb::rpc::ThreadPool thread_pool_; // TODO async operations instead of pool
  yb::rpc::TasksPool<PickStatusTabletTask> tasks_pool_;
  yb::rpc::Rpcs rpcs_;
};

TransactionManager::TransactionManager(
    const YBClientPtr& client, const scoped_refptr<ClockBase>& clock,
    LocalTabletFilter local_tablet_filter)
    : impl_(new Impl(client, clock, std::move(local_tablet_filter))) {}

TransactionManager::~TransactionManager() {
  impl_->Shutdown();
}

void TransactionManager::PickStatusTablet(PickStatusTabletCallback callback) {
  impl_->PickStatusTablet(std::move(callback));
}

const YBClientPtr& TransactionManager::client() const {
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

} // namespace client
} // namespace yb
