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

#include "yb/rpc/scheduler.h"

#include <thread>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <glog/logging.h>

#include "yb/util/errno.h"
#include "yb/util/logging.h"
#include "yb/util/status.h"

using namespace std::literals;
using namespace std::placeholders;
using boost::multi_index::const_mem_fun;
using boost::multi_index::hashed_unique;
using boost::multi_index::ordered_non_unique;

namespace yb {
namespace rpc {

namespace {

constexpr int64_t kShutdownMark = -(1ULL << 32U);

}

class Scheduler::Impl {
 public:
  explicit Impl(IoService* io_service)
      : io_service_(*io_service), strand_(*io_service), timer_(*io_service) {}

  ~Impl() {
    Shutdown();
    DCHECK_EQ(timer_counter_, 0);
    DCHECK(tasks_.empty());
  }

  void Abort(ScheduledTaskId task_id) {
    strand_.dispatch([this, task_id] {
      auto& index = tasks_.get<IdTag>();
      auto it = index.find(task_id);
      if (it != index.end()) {
        io_service_.post([task = *it] { task->Run(STATUS(Aborted, "Task aborted")); });
        index.erase(it);
      }
    });
  }

  void Shutdown() {
    bool old_value = false;
    if (closing_.compare_exchange_strong(old_value, true)) {
      strand_.dispatch([this] {
        boost::system::error_code ec;
        timer_.cancel(ec);
        LOG_IF(ERROR, ec) << "Failed to cancel timer: " << ec.message();

        auto status = STATUS(
            ServiceUnavailable, "Scheduler is shutting down", "" /* msg2 */, Errno(ESHUTDOWN));
        // Abort all scheduled tasks. It is ok to run task earlier than it was scheduled because
        // we pass error status to it.
        for (auto task : tasks_) {
          io_service_.post([task, status] { task->Run(status); });
        }
        tasks_.clear();
      });
    }
  }

  void DoSchedule(std::shared_ptr<ScheduledTaskBase> task) {
    strand_.dispatch([this, task] {
      if (closing_.load(std::memory_order_acquire)) {
        io_service_.post([task] {
          task->Run(STATUS(Aborted, "Scheduler shutdown", "" /* msg2 */, Errno(ESHUTDOWN)));
        });
        return;
      }

      auto pair = tasks_.insert(task);
      CHECK(pair.second);
      if (pair.first == tasks_.begin()) {
        StartTimer();
      }
    });
  }

  ScheduledTaskId NextId() {
    return ++id_;
  }

  IoService& io_service() {
    return io_service_;
  }

 private:
  void StartTimer() {
    DCHECK(strand_.running_in_this_thread());
    DCHECK(!tasks_.empty());

    boost::system::error_code ec;
    timer_.expires_at((*tasks_.begin())->time(), ec);
    LOG_IF(ERROR, ec) << "Reschedule timer failed: " << ec.message();
    ++timer_counter_;
    timer_.async_wait(strand_.wrap(std::bind(&Impl::HandleTimer, this, _1)));
  }

  void HandleTimer(const boost::system::error_code& ec) {
    DCHECK(strand_.running_in_this_thread());
    --timer_counter_;

    if (ec) {
      LOG_IF(ERROR, ec != boost::asio::error::operation_aborted) << "Wait failed: " << ec.message();
      return;
    }
    if (closing_.load(std::memory_order_acquire)) {
      return;
    }

    auto now = std::chrono::steady_clock::now();
    while (!tasks_.empty() && (*tasks_.begin())->time() <= now) {
      io_service_.post([task = *tasks_.begin()] { task->Run(Status::OK()); });
      tasks_.erase(tasks_.begin());
    }

    if (!tasks_.empty()) {
      StartTimer();
    }
  }

  class IdTag;

  typedef boost::multi_index_container<
      std::shared_ptr<ScheduledTaskBase>,
      boost::multi_index::indexed_by<
          ordered_non_unique<
              const_mem_fun<ScheduledTaskBase, SteadyTimePoint, &ScheduledTaskBase::time>
          >,
          hashed_unique<
              boost::multi_index::tag<IdTag>,
              const_mem_fun<ScheduledTaskBase, ScheduledTaskId, &ScheduledTaskBase::id>
          >
      >
  > Tasks;

  IoService& io_service_;
  std::atomic<ScheduledTaskId> id_ = {0};
  Tasks tasks_;
  // Strand that protects tasks_ and timer_ fields.
  boost::asio::io_service::strand strand_;
  boost::asio::steady_timer timer_;
  int timer_counter_ = 0;
  std::atomic<bool> closing_ = {false};
};

Scheduler::Scheduler(IoService* io_service) : impl_(new Impl(io_service)) {}
Scheduler::~Scheduler() {}

void Scheduler::Shutdown() {
  impl_->Shutdown();
}

void Scheduler::Abort(ScheduledTaskId task_id) {
  impl_->Abort(task_id);
}

void Scheduler::DoSchedule(std::shared_ptr<ScheduledTaskBase> task) {
  impl_->DoSchedule(std::move(task));
}

ScheduledTaskId Scheduler::NextId() {
  return impl_->NextId();
}

IoService& Scheduler::io_service() {
  return impl_->io_service();
}

void ScheduledTaskTracker::Abort() {
  auto last_scheduled_task_id = last_scheduled_task_id_.load(std::memory_order_acquire);
  if (last_scheduled_task_id != rpc::kInvalidTaskId) {
    scheduler_->Abort(last_scheduled_task_id);
  }
}

void ScheduledTaskTracker::StartShutdown() {
  auto num_scheduled = num_scheduled_.load(std::memory_order_acquire);
  while (num_scheduled >= 0) {
    num_scheduled_.compare_exchange_strong(num_scheduled, num_scheduled + kShutdownMark);
  }
}

void ScheduledTaskTracker::CompleteShutdown() {
  for (;;) {
    auto left = num_scheduled_.load(std::memory_order_acquire) - kShutdownMark;
    if (left <= 0) {
      LOG_IF(DFATAL, left < 0) << "Negative number of tasks left: " << left;
      break;
    }
    YB_LOG_EVERY_N_SECS(INFO, 1) << "Waiting " << left << " tasks to complete";
    Abort();
    std::this_thread::sleep_for(1ms);
  }
}

} // namespace rpc
} // namespace yb
