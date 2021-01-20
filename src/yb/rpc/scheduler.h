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

#ifndef YB_RPC_SCHEDULER_H
#define YB_RPC_SCHEDULER_H

#include "yb/rpc/rpc_fwd.h"
#include "yb/util/net/net_fwd.h"

#include "yb/util/status.h"

namespace yb {

class Status;

namespace rpc {

using ScheduledTaskId = int64_t;
constexpr ScheduledTaskId kUninitializedScheduledTaskId = 0;

class ScheduledTaskBase {
 public:
  explicit ScheduledTaskBase(ScheduledTaskId id, const SteadyTimePoint& time)
      : id_(id), time_(time) {}

  ScheduledTaskId id() const { return id_; }
  SteadyTimePoint time() const { return time_; }

  virtual ~ScheduledTaskBase() {}
  virtual void Run(const Status& status) = 0;

 private:
  ScheduledTaskId id_;
  SteadyTimePoint time_;
};

template<class F>
class ScheduledTask : public ScheduledTaskBase {
 public:
  explicit ScheduledTask(ScheduledTaskId id, const SteadyTimePoint& time, const F& f)
      : ScheduledTaskBase(id, time), f_(f) {}

  void Run(const Status& status) override {
    f_(status);
  }
 private:
  F f_;
};

template<class F>
class ScheduledTaskWithId : public ScheduledTaskBase {
 public:
  explicit ScheduledTaskWithId(ScheduledTaskId id, const SteadyTimePoint& time, const F& f)
      : ScheduledTaskBase(id, time), f_(f) {}

  void Run(const Status& status) override {
    f_(id(), status);
  }

 private:
  F f_;
};

class Scheduler {
 public:
  explicit Scheduler(IoService* io_service);
  ~Scheduler();

  template<class F>
  ScheduledTaskId Schedule(const F& f, std::chrono::steady_clock::duration delay) {
    return Schedule(f, std::chrono::steady_clock::now() + delay);
  }

  template<class F>
  auto Schedule(const F& f, std::chrono::steady_clock::time_point time) ->
      decltype(f(Status()), ScheduledTaskId()) {
    auto id = NextId();
    DoSchedule(std::make_shared<ScheduledTask<F>>(id, time, f));
    return id;
  }

  template<class F>
  auto Schedule(const F& f, std::chrono::steady_clock::time_point time) ->
      decltype(f(ScheduledTaskId(), Status()), ScheduledTaskId()) {
    auto id = NextId();
    DoSchedule(std::make_shared<ScheduledTaskWithId<F>>(id, time, f));
    return id;
  }

  void Abort(ScheduledTaskId task_id);

  void Shutdown();

  IoService& io_service();

 private:
  ScheduledTaskId NextId();

  void DoSchedule(std::shared_ptr<ScheduledTaskBase> task);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

class ScheduledTaskTracker {
 public:
  ScheduledTaskTracker() = default;

  explicit ScheduledTaskTracker(Scheduler* scheduler) : scheduler_(DCHECK_NOTNULL(scheduler)) {}

  void Bind(Scheduler* scheduler) {
    scheduler_ = scheduler;
  }

  template <class F>
  void Schedule(const F& f, std::chrono::steady_clock::duration delay) {
    Schedule(f, std::chrono::steady_clock::now() + delay);
  }

  template <class F>
  void Schedule(const F& f, std::chrono::steady_clock::time_point time) {
    Abort();
    if (++num_scheduled_ < 0) { // Shutting down
      --num_scheduled_;
      return;
    }
    last_scheduled_task_id_ = scheduler_->Schedule(
        [this, f](ScheduledTaskId task_id, const Status& status) {
      last_scheduled_task_id_.compare_exchange_strong(task_id, rpc::kInvalidTaskId);
      f(status);
      --num_scheduled_;
    }, time);
  }

  void Abort();

  void StartShutdown();
  void CompleteShutdown();

  void Shutdown() {
    StartShutdown();
    CompleteShutdown();
  }

 private:
  Scheduler* scheduler_ = nullptr;
  std::atomic<int64_t> num_scheduled_{0};
  std::atomic<rpc::ScheduledTaskId> last_scheduled_task_id_{rpc::kInvalidTaskId};
};

} // namespace rpc
} // namespace yb

#endif // YB_RPC_SCHEDULER_H
