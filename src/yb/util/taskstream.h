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

#ifndef YB_UTIL_TASKSTREAM_H
#define YB_UTIL_TASKSTREAM_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>

#include <gflags/gflags.h>

#include "yb/util/blocking_queue.h"
#include "yb/util/logging.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/taskstream.h"
#include "yb/util/threadpool.h"

using namespace std::chrono_literals;

using std::vector;

namespace yb {
class ThreadPool;
class ThreadPoolToken;

template <typename T>
class TaskStreamImpl;

YB_DEFINE_ENUM(TaskStreamRunState, (kIdle)(kSubmit)(kDrain)(kProcess)(kComplete)(kFinish));

template <typename T>
// TaskStream has a thread pool token in the given thread pool.
// TaskStream does not manage a thread but only submits to the token in the thread pool.
// When we submit tasks to the taskstream, it adds tasks to the queue to be processed.
// The internal Run function will call the user-provided function,
// for each element in the queue in a loop.
// When the queue is empty, it calls the user-provided function with no parameter,
// to indicate it needs to process the end of the group of tasks processed.
// This feature is used for the preparer and appender functionality.
class TaskStream {
 public:
  explicit TaskStream(std::function<void(T*)> process_item,
                      ThreadPool* thread_pool,
                      int32_t queue_max_size,
                      const MonoDelta& queue_max_wait);
  ~TaskStream();

  CHECKED_STATUS Start();
  void Stop();

  CHECKED_STATUS Submit(T* item);

  CHECKED_STATUS TEST_SubmitFunc(const std::function<void()>& func);

  std::string GetRunThreadStack() {
    auto result = ThreadStack(run_tid_);
    if (!result.ok()) {
      return result.status().ToString();
    }
    return (*result).Symbolize();
  }

  std::string ToString() const {
    return YB_CLASS_TO_STRING(queue, run_state, stopped, stop_requested);
  }

 private:
  using RunState = TaskStreamRunState;

  void ChangeRunState(RunState expected_old_state, RunState new_state) {
    auto old_state = run_state_.exchange(new_state, std::memory_order_acq_rel);
    LOG_IF(DFATAL, old_state != expected_old_state)
        << "Task stream was in wrong state " << old_state << " while "
        << expected_old_state << " was expected";
  }

  // We set this to true to tell the Run function to return. No new tasks will be accepted, but
  // existing tasks will still be processed.
  std::atomic<bool> stop_requested_{false};

  std::atomic<RunState> run_state_{RunState::kIdle};

  // This is set to true immediately before the thread exits.
  std::atomic<bool> stopped_{false};

  // The objects in the queue are owned by the queue and ownership gets tranferred to ProcessItem.
  BlockingQueue<T*> queue_;

  // This mutex/condition combination is used in Stop() in case multiple threads are calling that
  // function concurrently. One of them will ask the taskstream thread to stop and wait for it, and
  // then will notify other threads that have called Stop().
  std::mutex stop_mtx_;
  std::condition_variable stop_cond_;

  std::unique_ptr<ThreadPoolToken> taskstream_pool_token_;
  std::function<void(T*)> process_item_;
  ThreadIdForStack run_tid_ = 0;

  // Maximum time to wait for the queue to become non-empty.
  const MonoDelta queue_max_wait_;

  void Run();
  void ProcessItem(T* item);
};

template <typename T>
TaskStream<T>::TaskStream(std::function<void(T*)> process_item,
                          ThreadPool* thread_pool,
                          int32_t queue_max_size,
                          const MonoDelta& queue_max_wait)
    : queue_(queue_max_size),
      // run_state_ is responsible for serial execution, so we use concurrent here to avoid
      // unnecessary checks in thread pool.
      taskstream_pool_token_(thread_pool->NewToken(ThreadPool::ExecutionMode::CONCURRENT)),
      process_item_(process_item),
      queue_max_wait_(queue_max_wait) {
}

template <typename T>
TaskStream<T>::~TaskStream() {
  Stop();
}

template <typename T>
Status TaskStream<T>::Start() {
  VLOG(1) << "Starting the TaskStream";
  return Status::OK();
}

template <typename T>
void TaskStream<T>::Stop() {
  VLOG(1) << "Stopping the TaskStream";
  auto scope_exit = ScopeExit([] {
    VLOG(1) << "The TaskStream has stopped";
  });
  queue_.Shutdown();
  if (stopped_.load(std::memory_order_acquire)) {
    return;
  }
  stop_requested_ = true;
  {
    std::unique_lock<std::mutex> stop_lock(stop_mtx_);
    stop_cond_.wait(stop_lock, [this] {
      return (run_state_.load(std::memory_order_acquire) == RunState::kIdle && queue_.empty());
    });
  }
  stopped_.store(true, std::memory_order_release);
}

template <typename T>
Status TaskStream<T>::Submit(T *task) {
  if (stop_requested_.load(std::memory_order_acquire)) {
    return STATUS(IllegalState, "Tablet is shutting down");
  }
  if (!queue_.BlockingPut(task)) {
    return STATUS_FORMAT(ServiceUnavailable,
                         "TaskStream queue is full (max capacity $0)",
                         queue_.max_size());
  }

  RunState expected = RunState::kIdle;
  if (!run_state_.compare_exchange_strong(
          expected, RunState::kSubmit, std::memory_order_acq_rel)) {
    // run_state_ was not idle, so we are not creating a task to process operations.
    return Status::OK();
  }
  return taskstream_pool_token_->SubmitFunc(std::bind(&TaskStream::Run, this));
}

template <typename T>
Status TaskStream<T>::TEST_SubmitFunc(const std::function<void()>& func) {
  return taskstream_pool_token_->SubmitFunc(func);
}

template <typename T>
void TaskStream<T>::Run() {
  VLOG(1) << "Starting taskstream task:" << this;
  ChangeRunState(RunState::kSubmit, RunState::kDrain);
  run_tid_ = Thread::CurrentThreadIdForStack();
  for (;;) {
    MonoTime wait_timeout_deadline = MonoTime::Now() + queue_max_wait_;
    std::vector<T *> group;
    queue_.BlockingDrainTo(&group, wait_timeout_deadline);
    if (!group.empty()) {
      ChangeRunState(RunState::kDrain, RunState::kProcess);
      for (T* item : group) {
        ProcessItem(item);
      }
      ChangeRunState(RunState::kProcess, RunState::kComplete);
      ProcessItem(nullptr);
      group.clear();
      ChangeRunState(RunState::kComplete, RunState::kDrain);
      continue;
    }
    ChangeRunState(RunState::kDrain, RunState::kFinish);
    // Not processing and queue empty, return from task.
    std::unique_lock<std::mutex> stop_lock(stop_mtx_);
    ChangeRunState(RunState::kFinish, RunState::kIdle);
    if (!queue_.empty()) {
      // Got more operations, try stay in the loop.
      RunState expected = RunState::kIdle;
      if (run_state_.compare_exchange_strong(
              expected, RunState::kDrain, std::memory_order_acq_rel)) {
        continue;
      }
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
      VLOG(1) << "TaskStream task's Run() function is returning because stop is requested.";
      stop_cond_.notify_all();
      return;
    }
    VLOG(1) << "Returning from TaskStream task after inactivity:" << this;
    return;
  }
}

template <typename T>
void TaskStream<T>::ProcessItem(T* item) {
  process_item_(item);
}

}  // namespace yb

#endif  // YB_UTIL_TASKSTREAM_H
