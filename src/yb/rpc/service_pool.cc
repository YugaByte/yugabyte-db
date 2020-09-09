// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#include "yb/rpc/service_pool.h"

#include <memory>
#include <queue>
#include <string>
#include <vector>

#include <boost/asio/strand.hpp>

#include <cds/container/basket_queue.h>
#include <cds/gc/dhp.h>

#include <glog/logging.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/ref_counted.h"

#include "yb/rpc/inbound_call.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/scheduler.h"
#include "yb/rpc/service_if.h"

#include "yb/gutil/strings/substitute.h"
#include "yb/util/flag_tags.h"
#include "yb/util/lockfree.h"
#include "yb/util/metrics.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/thread.h"
#include "yb/util/trace.h"

using namespace std::literals;
using namespace std::placeholders;
using std::shared_ptr;
using strings::Substitute;

DEFINE_int64(max_time_in_queue_ms, 6000,
             "Fail calls that get stuck in the queue longer than the specified amount of time "
                 "(in ms)");
TAG_FLAG(max_time_in_queue_ms, advanced);
TAG_FLAG(max_time_in_queue_ms, runtime);
DEFINE_int64(backpressure_recovery_period_ms, 600000,
             "Once we hit a backpressure/service-overflow we will consider dropping stale requests "
             "for this duration (in ms)");
TAG_FLAG(backpressure_recovery_period_ms, advanced);
TAG_FLAG(backpressure_recovery_period_ms, runtime);
DEFINE_test_flag(bool, enable_backpressure_mode_for_testing, false,
            "For testing purposes. Enables the rpc's to be considered timed out in the queue even "
            "when we have not had any backpressure in the recent past.");

METRIC_DEFINE_histogram(server, rpc_incoming_queue_time,
                        "RPC Queue Time",
                        yb::MetricUnit::kMicroseconds,
                        "Number of microseconds incoming RPC requests spend in the worker queue",
                        60000000LU, 3);

METRIC_DEFINE_counter(server, rpcs_timed_out_in_queue,
                      "RPC Queue Timeouts",
                      yb::MetricUnit::kRequests,
                      "Number of RPCs whose timeout elapsed while waiting "
                      "in the service queue, and thus were not processed. "
                      "Does not include calls that were expired before we tried to execute them.");

METRIC_DEFINE_counter(server, rpcs_timed_out_early_in_queue,
                      "RPC Queue Timeouts",
                      yb::MetricUnit::kRequests,
                      "Number of RPCs whose timeout elapsed while waiting "
                      "in the service queue, and thus were not processed. "
                      "Timeout for those calls were detected before the calls tried to execute.");

METRIC_DEFINE_counter(server, rpcs_queue_overflow,
                      "RPC Queue Overflows",
                      yb::MetricUnit::kRequests,
                      "Number of RPCs dropped because the service queue "
                      "was full.");

namespace yb {
namespace rpc {

namespace {

const CoarseDuration kTimeoutCheckGranularity = 100ms;
const char* const kTimedOutInQueue = "Call waited in the queue past deadline";

} // namespace

class ServicePoolImpl final : public InboundCallHandler {
 public:
  ServicePoolImpl(size_t max_tasks,
                  ThreadPool* thread_pool,
                  Scheduler* scheduler,
                  ServiceIfPtr service,
                  const scoped_refptr<MetricEntity>& entity)
      : max_queued_calls_(max_tasks),
        thread_pool_(*thread_pool),
        scheduler_(*scheduler),
        service_(std::move(service)),
        incoming_queue_time_(METRIC_rpc_incoming_queue_time.Instantiate(entity)),
        rpcs_timed_out_in_queue_(METRIC_rpcs_timed_out_in_queue.Instantiate(entity)),
        rpcs_timed_out_early_in_queue_(
            METRIC_rpcs_timed_out_early_in_queue.Instantiate(entity)),
        rpcs_queue_overflow_(METRIC_rpcs_queue_overflow.Instantiate(entity)),
        check_timeout_strand_(scheduler->io_service()),
        log_prefix_(Format("$0: ", service_->service_name())) {

          // Create per service counter for rpcs_in_queue_.
          auto id = Format("rpcs_in_queue_$0", service_->service_name());
          EscapeMetricNameForPrometheus(&id);
          string description = id + " metric for ServicePoolImpl";
          rpcs_in_queue_ = entity->FindOrCreateGauge(
              std::unique_ptr<GaugePrototype<int64_t>>(new OwningGaugePrototype<int64_t>(
                  entity->prototype().name(), std::move(id),
                  description, MetricUnit::kRequests, description, MetricLevel::kInfo)),
              static_cast<int64>(0) /* initial_value */);

          LOG_WITH_PREFIX(INFO) << "yb::rpc::ServicePoolImpl created at " << this;
  }

  ~ServicePoolImpl() {
    StartShutdown();
    CompleteShutdown();
  }

  void CompleteShutdown() {
    shutdown_complete_latch_.Wait();
    while (scheduled_tasks_.load(std::memory_order_acquire) != 0) {
      std::this_thread::sleep_for(10ms);
    }
  }

  void StartShutdown() {
    bool closing_state = false;
    if (closing_.compare_exchange_strong(closing_state, true)) {
      service_->Shutdown();

      auto check_timeout_task = check_timeout_task_.load(std::memory_order_acquire);
      if (check_timeout_task != kUninitializedScheduledTaskId) {
        scheduler_.Abort(check_timeout_task);
      }

      check_timeout_strand_.dispatch([this] {
        std::weak_ptr<InboundCall> inbound_call_wrapper;
        while (pre_check_timeout_queue_.pop(inbound_call_wrapper)) {}
        shutdown_complete_latch_.CountDown();
      });
    }
  }

  void Enqueue(const InboundCallPtr& call) {
    TRACE_TO(call->trace(), "Inserting onto call queue");

    auto task = call->BindTask(this);
    if (!task) {
      Overflow(call, "service", queued_calls_.load(std::memory_order_relaxed));
      return;
    }

    auto call_deadline = call->GetClientDeadline();
    if (call_deadline != CoarseTimePoint::max()) {
      pre_check_timeout_queue_.push(call);
      ScheduleCheckTimeout(call_deadline);
    }

    thread_pool_.Enqueue(task);
  }

  const Counter* RpcsTimedOutInQueueMetricForTests() const {
    return rpcs_timed_out_early_in_queue_.get();
  }

  const Counter* RpcsQueueOverflowMetric() const {
    return rpcs_queue_overflow_.get();
  }

  std::string service_name() const {
    return service_->service_name();
  }

  ServiceIfPtr TEST_get_service() const {
    return service_;
  }

  void Overflow(const InboundCallPtr& call, const char* type, size_t limit) {
    const auto err_msg =
        Substitute("$0 request on $1 from $2 dropped due to backpressure. "
                   "The $3 queue is full, it has $4 items.",
            call->method_name(),
            service_->service_name(),
            yb::ToString(call->remote_address()),
            type,
            limit);
    YB_LOG_EVERY_N_SECS(WARNING, 3) << LogPrefix() << err_msg;
    const auto response_status = STATUS(ServiceUnavailable, err_msg);
    rpcs_queue_overflow_->Increment();
    call->RespondFailure(ErrorStatusPB::ERROR_SERVER_TOO_BUSY, response_status);
    last_backpressure_at_.store(
        CoarseMonoClock::Now().time_since_epoch(), std::memory_order_release);
  }

  void Failure(const InboundCallPtr& call, const Status& status) override {
    if (!call->TryStartProcessing()) {
      return;
    }

    if (status.IsServiceUnavailable()) {
      Overflow(call, "global", thread_pool_.options().queue_limit);
      return;
    }
    YB_LOG_EVERY_N_SECS(WARNING, 1)
        << LogPrefix()
        << call->method_name() << " request on " << service_->service_name() << " from "
        << call->remote_address() << " dropped because of: " << status.ToString();
    const auto response_status = STATUS(ServiceUnavailable, "Service is shutting down");
    call->RespondFailure(ErrorStatusPB::FATAL_SERVER_SHUTTING_DOWN, response_status);
  }

  void Handle(InboundCallPtr incoming) override {
    incoming->RecordHandlingStarted(incoming_queue_time_);
    ADOPT_TRACE(incoming->trace());

    const char* error_message;
    if (PREDICT_FALSE(incoming->ClientTimedOut())) {
      error_message = kTimedOutInQueue;
    } else if (PREDICT_FALSE(ShouldDropRequestDuringHighLoad(incoming))) {
      error_message = "The server is overloaded. Call waited in the queue past max_time_in_queue.";
    } else {
      TRACE_TO(incoming->trace(), "Handling call");

      if (incoming->TryStartProcessing()) {
        service_->Handle(std::move(incoming));
      }
      return;
    }

    TRACE_TO(incoming->trace(), error_message);
    VLOG_WITH_PREFIX(4)
        << "Timing out call " << incoming->ToString() << " due to: " << error_message;

    // Respond as a failure, even though the client will probably ignore
    // the response anyway.
    TimedOut(incoming.get(), error_message, rpcs_timed_out_in_queue_.get());
  }

 private:
  void TimedOut(InboundCall* call, const char* error_message, Counter* metric) {
    if (call->RespondTimedOutIfPending(error_message)) {
      metric->Increment();
    }
  }

  bool ShouldDropRequestDuringHighLoad(const InboundCallPtr& incoming) {
    CoarseTimePoint last_backpressure_at(last_backpressure_at_.load(std::memory_order_acquire));

    // For testing purposes.
    if (GetAtomicFlag(&FLAGS_TEST_enable_backpressure_mode_for_testing)) {
      last_backpressure_at = CoarseMonoClock::Now();
    }

    // Test for a sentinel value, to avoid reading the clock.
    if (last_backpressure_at == CoarseTimePoint()) {
      return false;
    }

    auto now = CoarseMonoClock::Now();
    if (now > last_backpressure_at + FLAGS_backpressure_recovery_period_ms * 1ms) {
      last_backpressure_at_.store(CoarseTimePoint().time_since_epoch(), std::memory_order_release);
      return false;
    }

    return incoming->GetTimeInQueue().ToMilliseconds() > FLAGS_max_time_in_queue_ms;
  }

  void CheckTimeout(ScheduledTaskId task_id, CoarseTimePoint time, const Status& status) {
    auto se = ScopeExit([this, task_id, time] {
      auto expected_duration = time.time_since_epoch();
      next_check_timeout_.compare_exchange_strong(
          expected_duration, CoarseTimePoint::max().time_since_epoch(),
          std::memory_order_acq_rel);
      auto expected_task_id = task_id;
      check_timeout_task_.compare_exchange_strong(
          expected_task_id, kUninitializedScheduledTaskId, std::memory_order_acq_rel);
      scheduled_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    });
    if (!status.ok()) {
      return;
    }

    auto now = CoarseMonoClock::now();
    {
      std::weak_ptr<InboundCall> weak_inbound_call;
      while (pre_check_timeout_queue_.pop(weak_inbound_call)) {
        auto inbound_call = weak_inbound_call.lock();
        if (!inbound_call) {
          continue;
        }
        if (now > inbound_call->GetClientDeadline()) {
          TimedOut(inbound_call.get(), kTimedOutInQueue, rpcs_timed_out_early_in_queue_.get());
        } else {
          check_timeout_queue_.emplace(inbound_call);
        }
      }
    }

    while (!check_timeout_queue_.empty() && now > check_timeout_queue_.top().time) {
      auto call = check_timeout_queue_.top().call.lock();
      if (call) {
        TimedOut(call.get(), kTimedOutInQueue, rpcs_timed_out_early_in_queue_.get());
      }
      check_timeout_queue_.pop();
    }

    if (!check_timeout_queue_.empty()) {
      ScheduleCheckTimeout(check_timeout_queue_.top().time);
    }
  }

  void ScheduleCheckTimeout(CoarseTimePoint time) {
    if (closing_.load(std::memory_order_acquire)) {
      return;
    }
    CoarseDuration next_check_timeout = next_check_timeout_.load(std::memory_order_acquire);
    time += kTimeoutCheckGranularity;
    while (CoarseTimePoint(next_check_timeout) > time) {
      if (next_check_timeout_.compare_exchange_weak(
              next_check_timeout, time.time_since_epoch(), std::memory_order_acq_rel)) {
        check_timeout_strand_.dispatch([this, time] {
          auto check_timeout_task = check_timeout_task_.load(std::memory_order_acquire);
          if (check_timeout_task != kUninitializedScheduledTaskId) {
            scheduler_.Abort(check_timeout_task);
          }
          scheduled_tasks_.fetch_add(1, std::memory_order_acq_rel);
          auto task_id = scheduler_.Schedule(
              [this, time](ScheduledTaskId task_id, const Status& status) {
                check_timeout_strand_.dispatch([this, time, task_id, status] {
                  CheckTimeout(task_id, time, status);
                });
              },
              ToSteady(time));
          check_timeout_task_.store(task_id, std::memory_order_release);
        });
        break;
      }
    }
  }

  const std::string& LogPrefix() const {
    return log_prefix_;
  }

  bool CallQueued() override {
    auto queued_calls = queued_calls_.fetch_add(1, std::memory_order_acq_rel);
    if (queued_calls < 0) {
      YB_LOG_EVERY_N_SECS(DFATAL, 5) << "Negative number of queued calls: " << queued_calls;
    }

    if (queued_calls >= max_queued_calls_) {
      queued_calls_.fetch_sub(1, std::memory_order_relaxed);
      return false;
    }

    rpcs_in_queue_->Increment();
    return true;
  }

  void CallDequeued() override {
    queued_calls_.fetch_sub(1, std::memory_order_relaxed);
    rpcs_in_queue_->Decrement();
  }

  const size_t max_queued_calls_;
  ThreadPool& thread_pool_;
  Scheduler& scheduler_;
  ServiceIfPtr service_;
  scoped_refptr<Histogram> incoming_queue_time_;
  scoped_refptr<Counter> rpcs_timed_out_in_queue_;
  scoped_refptr<Counter> rpcs_timed_out_early_in_queue_;
  scoped_refptr<Counter> rpcs_queue_overflow_;
  scoped_refptr<AtomicGauge<int64_t>> rpcs_in_queue_;
  // Have to use CoarseDuration here, since CoarseTimePoint does not work with clang + libstdc++
  std::atomic<CoarseDuration> last_backpressure_at_{CoarseTimePoint().time_since_epoch()};
  std::atomic<int64_t> queued_calls_{0};

  // It is too expensive to update timeout priority queue when each call is received.
  // So we are doing the following trick.
  // All calls are added to pre_check_timeout_queue_, w/o priority.
  // Then before timeout check we move calls from this queue to priority queue.
  typedef cds::container::BasketQueue<cds::gc::DHP, std::weak_ptr<InboundCall>>
      PreCheckTimeoutQueue;
  PreCheckTimeoutQueue pre_check_timeout_queue_;

  // Used to track scheduled time, to avoid unnecessary rescheduling.
  std::atomic<CoarseDuration> next_check_timeout_{CoarseTimePoint::max().time_since_epoch()};

  // Last scheduled task, required to abort scheduled task during reschedule.
  std::atomic<ScheduledTaskId> check_timeout_task_{kUninitializedScheduledTaskId};

  std::atomic<int> scheduled_tasks_{0};

  // Timeout checking synchronization.
  IoService::strand check_timeout_strand_;

  struct QueuedCheckDeadline {
    CoarseTimePoint time;
    // We use weak pointer to avoid retaining call that was already processed.
    std::weak_ptr<InboundCall> call;

    explicit QueuedCheckDeadline(const InboundCallPtr& inp)
        : time(inp->GetClientDeadline()), call(inp) {
    }
  };

  // Priority queue puts the geatest value on top, so we invert comparison.
  friend bool operator<(const QueuedCheckDeadline& lhs, const QueuedCheckDeadline& rhs) {
    return lhs.time > rhs.time;
  }

  std::priority_queue<QueuedCheckDeadline> check_timeout_queue_;

  std::atomic<bool> closing_ = {false};
  CountDownLatch shutdown_complete_latch_{1};
  std::string log_prefix_;
};

ServicePool::ServicePool(size_t max_tasks,
                         ThreadPool* thread_pool,
                         Scheduler* scheduler,
                         ServiceIfPtr service,
                         const scoped_refptr<MetricEntity>& metric_entity)
    : impl_(new ServicePoolImpl(
        max_tasks, thread_pool, scheduler, std::move(service), metric_entity)) {
}

ServicePool::~ServicePool() {
}

void ServicePool::StartShutdown() {
  impl_->StartShutdown();
}

void ServicePool::CompleteShutdown() {
  impl_->CompleteShutdown();
}

void ServicePool::QueueInboundCall(InboundCallPtr call) {
  impl_->Enqueue(std::move(call));
}

void ServicePool::Handle(InboundCallPtr call) {
  impl_->Handle(std::move(call));
}

const Counter* ServicePool::RpcsTimedOutInQueueMetricForTests() const {
  return impl_->RpcsTimedOutInQueueMetricForTests();
}

const Counter* ServicePool::RpcsQueueOverflowMetric() const {
  return impl_->RpcsQueueOverflowMetric();
}

std::string ServicePool::service_name() const {
  return impl_->service_name();
}

ServiceIfPtr ServicePool::TEST_get_service() const {
  return impl_->TEST_get_service();
}

} // namespace rpc
} // namespace yb
