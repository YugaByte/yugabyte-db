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

#include "yb/tablet/transaction_status_resolver.h"

#include <gflags/gflags.h>

#include "yb/client/transaction_rpc.h"

#include "yb/common/wire_protocol.h"

#include "yb/rpc/rpc.h"

#include "yb/tserver/tserver_service.pb.h"

#include "yb/util/atomic.h"
#include "yb/util/flag_tags.h"

DEFINE_test_flag(int32, inject_status_resolver_delay_ms, 0,
                 "Inject delay before launching transaction status resolver RPC.");

DEFINE_test_flag(int32, inject_status_resolver_complete_delay_ms, 0,
                 "Inject delay before counting down latch in transaction status resolver "
                 "complete.");

using namespace std::literals;
using namespace std::placeholders;

namespace yb {
namespace tablet {

class TransactionStatusResolver::Impl {
 public:
  Impl(TransactionParticipantContext* participant_context, rpc::Rpcs* rpcs,
       size_t max_transactions_per_request, TransactionStatusResolverCallback callback)
      : participant_context_(*participant_context), rpcs_(*rpcs),
        max_transactions_per_request_(max_transactions_per_request), callback_(std::move(callback)),
        log_prefix_(participant_context->LogPrefix()), handle_(rpcs_.InvalidHandle()) {}

  ~Impl() {
    LOG_IF_WITH_PREFIX(DFATAL, !closing_.load(std::memory_order_acquire))
        << "Destroy resolver without Shutdown";
  }

  void Shutdown() {
    closing_.store(true, std::memory_order_release);
    for (;;) {
      if (run_latch_.WaitFor(10s)) {
        break;
      }
      LOG_WITH_PREFIX(DFATAL) << "Long wait for transaction status resolver to shutdown";
    }
  }

  void Start(CoarseTimePoint deadline) {
    LOG_WITH_PREFIX(INFO) << "Start, queues: " << queues_.size();

    deadline_ = deadline;
    run_latch_.Reset(1);
    Execute();
  }

  std::future<Status> ResultFuture() {
    return result_promise_.get_future();
  }

  bool Running() {
    return run_latch_.count() != 0;
  }

  void Add(const TabletId& status_tablet, const TransactionId& transaction_id) {
    LOG_IF(DFATAL, run_latch_.count()) << "Add while running";
    queues_[status_tablet].push_back(transaction_id);
  }

 private:
  void Execute() {
    LOG_IF(DFATAL, !run_latch_.count()) << "Execute while running is false";

    if (CoarseMonoClock::now() >= deadline_) {
      Complete(STATUS(TimedOut, "Timed out to resolve transaction statuses"));
      return;
    }
    if (closing_.load(std::memory_order_acquire)) {
      Complete(STATUS(Aborted, "Aborted because of shutdown"));
      return;
    }
    if (queues_.empty() || max_transactions_per_request_ <= 0) {
      Complete(Status::OK());
      return;
    }

    // We access queues_ only while adding transaction and after that while resolving
    // transaction statuses, which is NOT concurrent.
    // So we could avoid doing synchronization here.
    auto& tablet_id_and_queue = *queues_.begin();
    tserver::GetTransactionStatusRequestPB req;
    req.set_tablet_id(tablet_id_and_queue.first);
    req.set_propagated_hybrid_time(participant_context_.Now().ToUint64());
    const auto& tablet_queue = tablet_id_and_queue.second;
    auto request_size = std::min<size_t>(max_transactions_per_request_, tablet_queue.size());
    for (size_t i = 0; i != request_size; ++i) {
      const auto& txn_id = tablet_queue[i];
      VLOG_WITH_PREFIX(4) << "Checking txn status: " << txn_id;
      req.add_transaction_id()->assign(pointer_cast<const char*>(txn_id.data()), txn_id.size());
    }

    AtomicFlagSleepMs(&FLAGS_TEST_inject_status_resolver_delay_ms);

    auto client = participant_context_.client_future().get();
    if (!client || !rpcs_.RegisterAndStart(
        client::GetTransactionStatus(
            std::min(deadline_, TransactionRpcDeadline()),
            nullptr /* tablet */,
            client,
            &req,
            std::bind(&Impl::StatusReceived, this, _1, _2, request_size)),
        &handle_)) {
      Complete(STATUS(Aborted, "Aborted because cannot start RPC"));
    }
  }

  const std::string& LogPrefix() const {
    return log_prefix_;
  }

  void StatusReceived(Status status,
                      const tserver::GetTransactionStatusResponsePB& response,
                      size_t request_size) {
    VLOG_WITH_PREFIX(2) << "Received statuses: " << status << ", " << response.ShortDebugString();

    rpcs_.Unregister(&handle_);

    if (status.ok() && response.has_error()) {
      status = StatusFromPB(response.error().status());
    }

    if (!status.ok()) {
      LOG_WITH_PREFIX(WARNING) << "Failed to request transaction statuses: " << status;
      if (status.IsAborted()) {
        Complete(status);
      } else {
        Execute();
      }
      return;
    }

    if (response.has_propagated_hybrid_time()) {
      participant_context_.UpdateClock(HybridTime(response.propagated_hybrid_time()));
    }

    if (response.status().size() != 1 && response.status().size() != request_size) {
      // Node with old software version would always return 1 status.
      LOG_WITH_PREFIX(DFATAL)
          << "Bad response size, expected " << request_size << " entries, but found: "
          << response.ShortDebugString() << ", queue: " << AsString(queues_);
      Execute();
      return;
    }

    status_infos_.clear();
    status_infos_.reserve(request_size);
    auto it = queues_.begin();
    auto& queue = it->second;
    for (size_t i = 0; i != response.status().size(); ++i) {
      auto txn_status = response.status(i);
      VLOG_WITH_PREFIX(4)
          << "Status of " << queue.front() << ": " << TransactionStatus_Name(txn_status);
      HybridTime status_hybrid_time;
      if (i < response.status_hybrid_time().size()) {
        status_hybrid_time = HybridTime(response.status_hybrid_time(i));
      // Could happend only when coordinator has an old version.
      } else if (txn_status == TransactionStatus::ABORTED) {
        status_hybrid_time = HybridTime::kMax;
      } else {
        Complete(STATUS_FORMAT(
            IllegalState, "Missing status hybrid time for transaction status: $0",
            TransactionStatus_Name(txn_status)));
        return;
      }
      status_infos_.push_back({queue.front(), txn_status, status_hybrid_time});
      queue.pop_front();
    }
    if (queue.empty()) {
      LOG_WITH_PREFIX(INFO) << "Processed queue for: " << it->first;
      queues_.erase(it);
    }

    callback_(status_infos_);

    Execute();
  }

  void Complete(const Status& status) {
    LOG_WITH_PREFIX(INFO) << "Complete: " << status;
    result_promise_.set_value(status);
    AtomicFlagSleepMs(&FLAGS_TEST_inject_status_resolver_complete_delay_ms);
    run_latch_.CountDown();
  }

  TransactionParticipantContext& participant_context_;
  rpc::Rpcs& rpcs_;
  const size_t max_transactions_per_request_;
  TransactionStatusResolverCallback callback_;

  const std::string log_prefix_;
  rpc::Rpcs::Handle handle_;

  std::atomic<bool> closing_{false};
  CountDownLatch run_latch_{0};
  CoarseTimePoint deadline_;
  std::unordered_map<TabletId, std::deque<TransactionId>> queues_;
  std::vector<TransactionStatusInfo> status_infos_;
  std::promise<Status> result_promise_;
};

TransactionStatusResolver::TransactionStatusResolver(
    TransactionParticipantContext* participant_context, rpc::Rpcs* rpcs,
    size_t max_transactions_per_request, TransactionStatusResolverCallback callback)
    : impl_(new Impl(
        participant_context, rpcs, max_transactions_per_request, std::move(callback))) {
}

TransactionStatusResolver::~TransactionStatusResolver() {}

void TransactionStatusResolver::Shutdown() {
  impl_->Shutdown();
}

void TransactionStatusResolver::Add(
    const TabletId& status_tablet, const TransactionId& transaction_id) {
  impl_->Add(status_tablet, transaction_id);
}

void TransactionStatusResolver::Start(CoarseTimePoint deadline) {
  impl_->Start(deadline);
}

std::future<Status> TransactionStatusResolver::ResultFuture() {
  return impl_->ResultFuture();
}

bool TransactionStatusResolver::Running() const {
  return impl_->Running();
}

} // namespace tablet
} // namespace yb
