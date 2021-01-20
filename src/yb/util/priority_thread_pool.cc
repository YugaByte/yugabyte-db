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

#include "yb/util/priority_thread_pool.h"

#include <mutex>
#include <queue>
#include <set>

#include <boost/container/stable_vector.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/ranked_index.hpp>

#include "yb/gutil/thread_annotations.h"

#include "yb/util/locks.h"
#include "yb/util/priority_queue.h"
#include "yb/util/scope_exit.h"
#include "yb/util/thread.h"

using namespace std::placeholders;

namespace yb {

namespace {

const Status kRemoveTaskStatus = STATUS(Aborted, "Task removed from priority thread pool");
const Status kShutdownStatus = STATUS(Aborted, "Priority thread pool shutdown");
const Status kNoWorkersStatus = STATUS(Aborted, "No workers to perform task");

typedef std::unique_ptr<PriorityThreadPoolTask> TaskPtr;
class PriorityThreadPoolWorker;

static constexpr int kEmptyQueuePriority = -1;

YB_STRONGLY_TYPED_BOOL(PickTask);

YB_DEFINE_ENUM(PriorityThreadPoolTaskState, (kPaused)(kNotStarted)(kRunning));

class PriorityThreadPoolInternalTask {
 public:
  PriorityThreadPoolInternalTask(int priority, TaskPtr task, PriorityThreadPoolWorker* worker)
      : priority_(priority),
        serial_no_(task->SerialNo()),
        state_(worker != nullptr ? PriorityThreadPoolTaskState::kRunning
                                 : PriorityThreadPoolTaskState::kNotStarted),
        task_(std::move(task)),
        worker_(worker) {}

  std::unique_ptr<PriorityThreadPoolTask>& task() const {
    return task_;
  }

  PriorityThreadPoolWorker* worker() const {
    return worker_;
  }

  size_t serial_no() const {
    return serial_no_;
  }

  // Changes to priority does not require immediate effect, so
  // relaxed could be used.
  int priority() const {
    return priority_.load(std::memory_order_relaxed);
  }

  // Changes to state does not require immediate effect, so
  // relaxed could be used.
  PriorityThreadPoolTaskState state() const {
    return state_.load(std::memory_order_relaxed);
  }

  void SetWorker(PriorityThreadPoolWorker* worker) {
    // Task state could be only changed when thread pool lock is held.
    // So it is safe to avoid state caching for logging.
    LOG_IF(DFATAL, state() != PriorityThreadPoolTaskState::kNotStarted)
        << "Set worker in wrong state: " << state();
    worker_ = worker;
    SetState(PriorityThreadPoolTaskState::kRunning);
  }

  void SetPriority(int value) {
    priority_.store(value, std::memory_order_relaxed);
  }

  void SetState(PriorityThreadPoolTaskState value) {
    state_.store(value, std::memory_order_relaxed);
  }

  std::string ToString() const {
    return Format("{ task: $0 worker: $1 state: $2 priority: $3 serial: $4 }",
                  TaskToString(), worker_, state(), priority(), serial_no_);
  }

 private:
  const std::string& TaskToString() const {
    if (!task_to_string_ready_.load(std::memory_order_acquire)) {
      std::lock_guard<simple_spinlock> lock(task_to_string_mutex_);
      if (!task_to_string_ready_.load(std::memory_order_acquire)) {
        task_to_string_ = task_->ToString();
        task_to_string_ready_.store(true, std::memory_order_release);
      }
    }
    return task_to_string_;
  }

  std::atomic<int> priority_;
  const size_t serial_no_;
  std::atomic<PriorityThreadPoolTaskState> state_{PriorityThreadPoolTaskState::kNotStarted};
  mutable TaskPtr task_;
  mutable PriorityThreadPoolWorker* worker_;

  mutable std::atomic<bool> task_to_string_ready_{false};
  mutable simple_spinlock task_to_string_mutex_;
  mutable std::string task_to_string_;
};

class PriorityThreadPoolWorkerContext {
 public:
  virtual void PauseIfNecessary(PriorityThreadPoolWorker* worker) = 0;
  virtual bool WorkerFinished(PriorityThreadPoolWorker* worker) = 0;
  virtual void TaskAborted(const PriorityThreadPoolInternalTask* task) = 0;
  virtual ~PriorityThreadPoolWorkerContext() {}
};

class PriorityThreadPoolWorker : public PriorityThreadPoolSuspender {
 public:
  explicit PriorityThreadPoolWorker(PriorityThreadPoolWorkerContext* context)
      : context_(context) {
  }

  void SetThread(ThreadPtr thread) {
    thread_ = std::move(thread);
  }

  // Perform provided task in this worker. Called on a fresh worker, or worker that was just picked
  // from the free workers list.
  void Perform(const PriorityThreadPoolInternalTask* task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      DCHECK(!task_);
      if (!stopped_) {
        std::swap(task_, task);
      }
    }
    cond_.notify_one();
    if (task) {
      task->task()->Run(kShutdownStatus, nullptr /* suspender */);
      context_->TaskAborted(task);
    }
  }

  // It is invoked from WorkerFinished to directly assign a new task.
  void SetTask(const PriorityThreadPoolInternalTask* task) {
    task_ = task;
  }

  void Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_) {
      if (!task_) {
        cond_.wait(lock);
        continue;
      }
      running_task_ = true;
      auto se = ScopeExit([this] {
        running_task_ = false;
      });
      {
        yb::ReverseLock<decltype(lock)> rlock(lock);
        for (;;) {
          task_->task()->Run(Status::OK(), this);
          if (!context_->WorkerFinished(this)) {
            break;
          }
        }
      }
    }
    DCHECK(!task_);
  }

  void Stop() {
    const PriorityThreadPoolInternalTask* task = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
      if (!running_task_) {
        std::swap(task, task_);
      }
    }
    cond_.notify_one();
    if (task) {
      task->task()->Run(kShutdownStatus, nullptr /* suspender */);
      context_->TaskAborted(task);
    }
  }

  void PauseIfNecessary() override {
    LOG_IF(DFATAL, yb::Thread::CurrentThreadId() != thread_->tid())
        << "PauseIfNecessary invoked not from worker thread";
    context_->PauseIfNecessary(this);
  }

  void Resumed() {
    cond_.notify_one();
  }

  const PriorityThreadPoolInternalTask* task() const {
    return task_;
  }

  void WaitResume(std::unique_lock<std::mutex>* lock) {
    cond_.wait(*lock, [this]() {
      return task_->state() != PriorityThreadPoolTaskState::kPaused;
    });
  }

  std::string ToString() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return Format("{ worker: $0 }", static_cast<const void*>(this));
  }

 private:
  PriorityThreadPoolWorkerContext* const context_;
  yb::ThreadPtr thread_;
  // Cannot use thread safety annotations, because std::unique_lock is used with this mutex.
  mutable std::mutex mutex_;
  std::condition_variable cond_;
  bool stopped_ = false;
  bool running_task_ = false;
  const PriorityThreadPoolInternalTask* task_ = nullptr;
};

class PriorityTaskComparator {
 public:
  bool operator()(const PriorityThreadPoolInternalTask& lhs,
                  const PriorityThreadPoolInternalTask& rhs) const {
    auto lhs_priority = lhs.priority();
    auto rhs_priority = rhs.priority();
    // The task with highest priority is picked first, if priorities are equal the task that
    // was added earlier is picked.
    return lhs_priority > rhs_priority ||
           (lhs_priority == rhs_priority && lhs.serial_no() < rhs.serial_no());
  }
};

// The order is the following:
// Not running tasks (i.e. paused or not started) go first, ordered by priority, state and serial.
// After them running tasks, ordered by priority, state and serial.
// I.e. paused tasks goes before not started tasks with the same priority.
class StateAndPriorityTaskComparator {
 public:
  bool operator()(const PriorityThreadPoolInternalTask& lhs,
                  const PriorityThreadPoolInternalTask& rhs) const {
    auto lhs_state = lhs.state();
    auto rhs_state = rhs.state();
    auto lhs_running = lhs_state == PriorityThreadPoolTaskState::kRunning;
    if (lhs_running != (rhs_state == PriorityThreadPoolTaskState::kRunning)) {
      return !lhs_running;
    }
    auto lhs_priority = lhs.priority();
    auto rhs_priority = rhs.priority();
    if (lhs_priority > rhs_priority) {
      return true;
    } else if (lhs_priority < rhs_priority) {
      return false;
    }
    if (lhs_state < rhs_state) {
      return true;
    } else if (lhs_state > rhs_state) {
      return false;
    }
    return lhs.serial_no() < rhs.serial_no();
  }
};

std::atomic<size_t> task_serial_no_(1);

} // namespace

PriorityThreadPoolTask::PriorityThreadPoolTask()
    : serial_no_(task_serial_no_.fetch_add(1, std::memory_order_acq_rel)) {
}

// Maintains priority queue of added tasks and vector of free workers.
// One of them is always empty.
//
// When a task is added it tries to pick a free worker or launch a new one.
// If it is impossible the task is added to the task priority queue.
//
// When worker finishes it tries to pick a task from the priority queue.
// If the queue is empty, the worker is added to the vector of free workers.
class PriorityThreadPool::Impl : public PriorityThreadPoolWorkerContext {
 public:
  explicit Impl(size_t max_running_tasks) : max_running_tasks_(max_running_tasks) {
    CHECK_GE(max_running_tasks, 1);
  }

  ~Impl() {
    StartShutdown();
    CompleteShutdown();

#ifndef NDEBUG
    std::lock_guard<std::mutex> lock(mutex_);
    DCHECK(tasks_.empty());
#endif
  }

  CHECKED_STATUS Submit(int priority, TaskPtr* task) {
    PriorityThreadPoolWorker* worker = nullptr;
    const PriorityThreadPoolInternalTask* internal_task = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_.load(std::memory_order_acquire)) {
        VLOG(3) << (**task).ToString() << " rejected because of shutdown";
        return kShutdownStatus;
      }
      worker = PickWorker();
      if (worker == nullptr) {
        if (workers_.empty()) {
          // Empty workers here means that we are unable to start even one worker thread.
          // So have to abort task in this case.
          VLOG(3) << (**task).ToString() << " rejected because cannot start even one worker";
          return kNoWorkersStatus;
        }
        VLOG(4) << "Added " << (**task).ToString() << " to queue";
      }

      internal_task = AddTask(priority, std::move(*task), worker);
    }

    if (worker) {
      VLOG(4) << "Passing " << internal_task->ToString() << " to worker: " << worker;
      worker->Perform(internal_task);
    }

    return Status::OK();
  }

  void Remove(void* key) {
    std::vector<TaskPtr> abort_tasks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->state() == PriorityThreadPoolTaskState::kNotStarted && it->task()->BelongsTo(key)) {
          abort_tasks.push_back(std::move(it->task()));
          it = tasks_.erase(it);
        } else {
          ++it;
        }
      }
      UpdateMaxPriorityToDefer();
    }
    AbortTasks(abort_tasks, kRemoveTaskStatus);
  }

  void StartShutdown() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    std::vector<TaskPtr> abort_tasks;
    std::vector<PriorityThreadPoolWorker*> workers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = tasks_.begin(); it != tasks_.end();) {
        auto state = it->state();
        if (state == PriorityThreadPoolTaskState::kNotStarted) {
          abort_tasks.push_back(std::move(it->task()));
          it = tasks_.erase(it);
        } else {
          if (state == PriorityThreadPoolTaskState::kPaused) {
            // On shutdown we should resume all workers, so they could complete their current tasks.
            ResumeWorker(it);
          }
          ++it;
        }
      }
      workers.reserve(workers_.size());
      for (auto& worker : workers_) {
        workers.push_back(&worker);
      }
      UpdateMaxPriorityToDefer();
    }
    for (auto* worker : workers) {
      worker->Stop();
    }
    AbortTasks(abort_tasks, kShutdownStatus);
  }

  void CompleteShutdown() {
    decltype(threads_) threads;
    decltype(workers_) workers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      threads.swap(threads_);
      workers.swap(workers_);
    }
    for (auto& thread : threads) {
      thread->Join();
    }
  }

  void PauseIfNecessary(PriorityThreadPoolWorker* worker) override {
    auto worker_task_priority = worker->task()->priority();
    if (max_priority_to_defer_.load(std::memory_order_acquire) < worker_task_priority) {
      return;
    }

    PriorityThreadPoolWorker* higher_pri_worker = nullptr;
    const PriorityThreadPoolInternalTask* task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (max_priority_to_defer_.load(std::memory_order_acquire) < worker_task_priority ||
          stopping_.load(std::memory_order_acquire)) {
        return;
      }

      // Check the highest priority of a a task that is not running.
      auto it = tasks_.get<StateAndPriorityTag>().begin();
      if (it->priority() <= worker_task_priority) {
        return;
      }

      LOG(INFO) << "Pausing " << worker->task()->ToString()
                << " in favor of " << it->ToString() << ", max to defer: "
                << max_priority_to_defer_.load(std::memory_order_acquire);

      ++paused_workers_;
      switch (it->state()) {
        case PriorityThreadPoolTaskState::kPaused:
          VLOG(4) << "Resuming " << it->worker();
          ResumeWorker(tasks_.project<PriorityTag>(it));
          break;
        case PriorityThreadPoolTaskState::kNotStarted:
          higher_pri_worker = PickWorker();
          if (!higher_pri_worker) {
            LOG(DFATAL) << Format(
                "Unable to pick a worker for a higher priority task when trying to pause a lower "
                    "priority task, paused: $1, free: $2, max: $3",
                workers_.size(), paused_workers_, free_workers_.size(), max_running_tasks_);
            worker->Resumed();
            return;
          }
          task = &*it;
          SetWorker(tasks_.project<PriorityTag>(it), higher_pri_worker);
          break;
        case PriorityThreadPoolTaskState::kRunning:
          --paused_workers_;
          LOG(DFATAL) << "Pausing in favor of already running task: " << it->ToString()
                      << ", state: " << DoStateToString();
          return;
      }

      auto worker_task_it = tasks_.iterator_to(*worker->task());
      ModifyState(worker_task_it, PriorityThreadPoolTaskState::kPaused);
    }

    if (higher_pri_worker) {
      VLOG(4) << "Passing " << task->ToString() << " to " << higher_pri_worker;
      higher_pri_worker->Perform(task);
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      worker->WaitResume(&lock);
      LOG(INFO) << "Resumed " << worker << " with " << worker->task()->ToString();
    }
  }

  bool ChangeTaskPriority(size_t serial_no, int priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& index = tasks_.get<SerialNoTag>();
    auto it = index.find(serial_no);
    if (it == index.end()) {
      return false;
    }

    index.modify(it, [priority](PriorityThreadPoolInternalTask& task) {
      task.SetPriority(priority);
    });
    UpdateMaxPriorityToDefer();

    LOG(INFO) << "Changed priority " << serial_no << ": " << it->ToString();

    return true;
  }

  std::string StateToString() {
    std::lock_guard<std::mutex> lock(mutex_);
    return DoStateToString();
  }

 private:
  std::string DoStateToString() REQUIRES(mutex_) {
    return Format(
        "{ max_running_tasks: $0 tasks: $1 workers: $2 paused_workers: $3 free_workers: $4 "
            "stopping: $5 max_priority_to_defer: $6 }",
        max_running_tasks_, tasks_, workers_, paused_workers_, free_workers_,
        stopping_.load(), max_priority_to_defer_.load());
  }

  void AbortTasks(const std::vector<TaskPtr>& tasks, const Status& abort_status) {
    for (const auto& task : tasks) {
      task->Run(abort_status, nullptr /* suspender */);
    }
  }

  bool WorkerFinished(PriorityThreadPoolWorker* worker) override {
    std::lock_guard<std::mutex> lock(mutex_);
    TaskFinished(worker->task());
    if (!DoWorkerFinished(worker)) {
      free_workers_.push_back(worker);
      worker->SetTask(nullptr);
      return false;
    }

    return true;
  }

  bool DoWorkerFinished(PriorityThreadPoolWorker* worker) REQUIRES(mutex_) {
    if (tasks_.empty()) {
      VLOG(4) << "No tasks left for " << worker;
      return false;
    }

    auto it = tasks_.get<StateAndPriorityTag>().begin();
    switch (it->state()) {
      case PriorityThreadPoolTaskState::kPaused:
        VLOG(4) << "Resume other worker after " << worker << " finished";
        ResumeWorker(tasks_.project<PriorityTag>(it));
        return false;
      case PriorityThreadPoolTaskState::kNotStarted:
        worker->SetTask(&*it);
        SetWorker(tasks_.project<PriorityTag>(it), worker);
        VLOG(4) << "Picked task for " << worker;
        return true;
      case PriorityThreadPoolTaskState::kRunning:
        VLOG(4) << "Only running tasks left, nothing for " << worker;
        return false;
    }

    FATAL_INVALID_ENUM_VALUE(PriorityThreadPoolTaskState, it->state());
  }

  // Task finished, adjust desired and unwanted tasks.
  void TaskFinished(const PriorityThreadPoolInternalTask* task) REQUIRES(mutex_) {
    VLOG(4) << "Finished " << task->ToString();
    tasks_.erase(tasks_.iterator_to(*task));
    UpdateMaxPriorityToDefer();
  }

  // Task finished, adjust desired and unwanted tasks.
  void TaskAborted(const PriorityThreadPoolInternalTask* task) override {
    VLOG(3) << "Aborted " << task->ToString();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskFinished(task);
  }

  template <class It>
  void ResumeWorker(It it) REQUIRES(mutex_) {
    LOG_IF(DFATAL, it->state() != PriorityThreadPoolTaskState::kPaused)
        << "Resuming not paused worker";
    --paused_workers_;
    ModifyState(it, PriorityThreadPoolTaskState::kRunning);
    it->worker()->Resumed();
  }

  PriorityThreadPoolWorker* PickWorker() REQUIRES(mutex_) {
    if (workers_.size() - paused_workers_ - free_workers_.size() >= max_running_tasks_) {
      VLOG(1) << "We already have " << workers_.size() << " - " << paused_workers_ << " - "
              << free_workers_.size() << " >= " << max_running_tasks_
              << " workers running, we could not run a new worker.";
      return nullptr;
    }
    if (!free_workers_.empty()) {
      auto worker = free_workers_.back();
      free_workers_.pop_back();
      VLOG(4) << "Has free worker: " << worker;
      return worker;
    }
    workers_.emplace_back(this);
    auto worker = &workers_.back();
    auto thread = yb::Thread::Make(
        "priority_thread_pool", "priority-worker",
        std::bind(&PriorityThreadPoolWorker::Run, worker));
    if (!thread.ok()) {
      LOG(WARNING) << "Failed to launch new worker: " << thread.status();
      workers_.pop_back();
      return nullptr;
    } else {
      worker->SetThread(*thread);
      threads_.push_back(std::move(*thread));
      VLOG(3) << "Created new worker: " << worker;
    }
    return worker;
  }

  const PriorityThreadPoolInternalTask* AddTask(
      int priority, TaskPtr task, PriorityThreadPoolWorker* worker) REQUIRES(mutex_) {
    auto it = tasks_.emplace(priority, std::move(task), worker).first;
    UpdateMaxPriorityToDefer();
    VLOG(4) << "New task added " << task->ToString() << ", max to defer: "
            << max_priority_to_defer_.load(std::memory_order_acquire);
    return &*it;
  }

  void UpdateMaxPriorityToDefer() REQUIRES(mutex_) {
    // We could pause a worker when both of the following conditions are met:
    // 1) Number of active tasks is greater than max_running_tasks_.
    // 2) Priority of the worker's current task is less than top max_running_tasks_ priorities.
    int priority = tasks_.size() <= max_running_tasks_
        ? kEmptyQueuePriority
        : tasks_.nth(max_running_tasks_)->priority();
    max_priority_to_defer_.store(priority, std::memory_order_release);
  }

  template <class It>
  void ModifyState(It it, PriorityThreadPoolTaskState new_state) REQUIRES(mutex_) {
    tasks_.modify(it, [new_state](PriorityThreadPoolInternalTask& task) {
      task.SetState(new_state);
    });
  }

  template <class It>
  void SetWorker(It it, PriorityThreadPoolWorker* worker) REQUIRES(mutex_) {
    tasks_.modify(it, [worker](PriorityThreadPoolInternalTask& task) {
      task.SetWorker(worker);
    });
  }

  const size_t max_running_tasks_;
  std::mutex mutex_;

  // Number of paused workers.
  size_t paused_workers_ GUARDED_BY(mutex_) = 0;

  std::vector<yb::ThreadPtr> threads_ GUARDED_BY(mutex_);
  boost::container::stable_vector<PriorityThreadPoolWorker> workers_ GUARDED_BY(mutex_);
  std::vector<PriorityThreadPoolWorker*> free_workers_ GUARDED_BY(mutex_);
  std::atomic<bool> stopping_{false};

  // Used for quick check, whether task with provided priority should be paused.
  std::atomic<int> max_priority_to_defer_{kEmptyQueuePriority};

  // Tag for index ordered by original priority and serial no.
  // Used to determine desired tasks to run.
  class PriorityTag;

  // Tag for index ordered by state, priority and serial no.
  // Used to determine which task should be started/resumed when worker is paused.
  class StateAndPriorityTag;

  // Tag for index hashed by serial no.
  // Used to find task for priority change.
  class SerialNoTag;

  boost::multi_index_container<
    PriorityThreadPoolInternalTask,
    boost::multi_index::indexed_by<
      boost::multi_index::ranked_unique<
        boost::multi_index::tag<PriorityTag>,
        boost::multi_index::identity<PriorityThreadPoolInternalTask>,
        PriorityTaskComparator
      >,
      boost::multi_index::ordered_unique<
        boost::multi_index::tag<StateAndPriorityTag>,
        boost::multi_index::identity<PriorityThreadPoolInternalTask>,
        StateAndPriorityTaskComparator
      >,
      boost::multi_index::hashed_unique<
        boost::multi_index::tag<SerialNoTag>,
        boost::multi_index::const_mem_fun<
          PriorityThreadPoolInternalTask, size_t, &PriorityThreadPoolInternalTask::serial_no>
      >
    >
  > tasks_ GUARDED_BY(mutex_);
};

PriorityThreadPool::PriorityThreadPool(size_t max_running_tasks)
    : impl_(new Impl(max_running_tasks)) {
}

PriorityThreadPool::~PriorityThreadPool() {
}

Status PriorityThreadPool::Submit(int priority, std::unique_ptr<PriorityThreadPoolTask>* task) {
  return impl_->Submit(priority, task);
}

void PriorityThreadPool::Remove(void* key) {
  impl_->Remove(key);
}

void PriorityThreadPool::StartShutdown() {
  impl_->StartShutdown();
}

void PriorityThreadPool::CompleteShutdown() {
  impl_->CompleteShutdown();
}

std::string PriorityThreadPool::StateToString() {
  return impl_->StateToString();
}

bool PriorityThreadPool::ChangeTaskPriority(size_t serial_no, int priority) {
  return impl_->ChangeTaskPriority(serial_no, priority);
}

} // namespace yb
