//
// Copyright (c) YugabyteDB, Inc.
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

#include "yb/util/thread_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include <boost/intrusive/list.hpp>

#include "yb/util/cgroups.h"
#include "yb/util/enum_parse.h"
#include "yb/util/flag_validators.h"
#include "yb/util/flags.h"
#include "yb/util/format.h"
#include "yb/util/lockfree.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status_format.h"
#include "yb/util/thread.h"
#include "yb/util/thread_restrictions.h"
#include "yb/util/unique_lock.h"

using namespace std::literals;

DEFINE_NON_RUNTIME_uint64(default_idle_timeout_ms, 15000,
    "Default idle timeout in milliseconds for thread pool workers. Applies to YBThreadPool and "
    "ThreadPoolBuilder pools that do not set an explicit idle timeout.");

namespace yb {
namespace {

Result<WorkerWakeOrder> ParseWorkerWakeOrder(const std::string& value) {
  return ParseEnumInsensitive<WorkerWakeOrder>("k" + value);
}

} // namespace
} // namespace yb

DEFINE_NON_RUNTIME_string(thread_pool_worker_wake_order, "mixed",
    "Which parked worker a thread pool wakes for a new task: lifo wakes the most recently parked "
    "worker, fifo the one parked longest, mixed wakes in fifo order but holds back up to 16 of the "
    "longest parked workers, waking them last, so that idle ones are reclaimed.");
DEFINE_validator(thread_pool_worker_wake_order,
    FLAG_OK_VALIDATOR(yb::ParseWorkerWakeOrder(_value)));

static bool detailed_logging = true;
namespace yb {

bool TEST_fail_to_create_second_thread_in_thread_pool_without_queue = false;

void YBThreadPool::DisableDetailedLogging() { detailed_logging = false; }

namespace {

class Worker;
struct WorkerLink;

using TaskQueue = SemiFairQueue<ThreadPoolTask>;

// Parked workers, popped by Enqueue in the pool's WorkerWakeOrder.
class WaitingWorkers {
 public:
  virtual ~WaitingWorkers() = default;

  virtual void Push(WorkerLink* link) = 0;
  virtual WorkerLink* Pop() = 0;

  // The worker behind link stopped on idle timeout; its link stays until popped and discarded.
  virtual void Abandon(WorkerLink* link) {}
};

std::unique_ptr<WaitingWorkers> CreateWaitingWorkers(WorkerWakeOrder order);

constexpr size_t kStopCreatingWorkersFlag = 1ul << 48;

struct ThreadPoolShare {
  const ThreadPoolOptions options;
#ifdef __linux__
  std::atomic<Cgroup*> cgroup;
#endif
  TaskQueue task_queue;
  std::atomic<size_t> task_queue_size{0};
  const std::unique_ptr<WaitingWorkers> waiting_workers;
  std::atomic<size_t> num_workers{0};

  explicit ThreadPoolShare(ThreadPoolOptions o)
      : options(std::move(o))
#ifdef __linux__
        , cgroup(options.cgroup)
#endif
        , waiting_workers(CreateWaitingWorkers(options.wake_order))
  {}

  Cgroup* GetCgroup() const {
#ifdef __linux__
    return cgroup.load(std::memory_order_acquire);
#else
    return nullptr;
#endif
  }

  void PushTask(ThreadPoolTask* task) {
    if (options.metrics.queue_time_us_stats) {
      task->set_submit_time(MonoTime::Now());
    }
    task_queue.Push(task);
    task_queue_size.fetch_add(1, std::memory_order_acq_rel);
  }

  ThreadPoolTask* PopTask() {
    auto result = task_queue.Pop();
    if (result) {
      task_queue_size.fetch_sub(1, std::memory_order_acq_rel);
      if (options.metrics.queue_time_us_stats) {
        options.metrics.queue_time_us_stats->Increment(
            (MonoTime::Now() - result->submit_time()).ToMicroseconds());
      }
    }
    return result;
  }
};

const auto kShuttingDownStatus = STATUS(Aborted, "Service is shutting down");

// There is difference between idle stop and external stop.
// In case of idle stop we don't perform join on thread, since it is already detached.
YB_DEFINE_ENUM(WorkerState, (kRunning)(kWaitingTask)(kIdleStop)(kExternalStop));

class Worker : public boost::intrusive::list_base_hook<> {
 public:
  explicit Worker(ThreadPoolShare& share, bool persistent);

  Status Start(size_t index, ThreadPoolTask* task) EXCLUDES(mutex_) {
    UniqueLock lock(mutex_);
    SCHECK_EQ(state_, WorkerState::kRunning, IllegalState, "Worker already stopped");
    auto name = Format("$0_$1_worker", share_.options.name, index);
    return Thread::Create(share_.options.name, name, &Worker::Execute, this, task, &thread_);
  }

  ~Worker();

  Worker(const Worker& worker) = delete;
  void operator=(const Worker& worker) = delete;

  void Stop() {
    ThreadPoolTask* task;
    {
      std::lock_guard lock(mutex_);
      if (state_ != WorkerState::kIdleStop) {
        state_ = WorkerState::kExternalStop;
        --share_.num_workers;
      }
      task = std::exchange(task_, nullptr);
      cond_.notify_one();
    }
    if (task) {
      task->Done(kShuttingDownStatus);
    }
  }

  WorkerState Notify(ThreadPoolTask* task) {
    std::lock_guard lock(mutex_);
    added_to_waiting_workers_ = false;
    // There could be cases when we popped task after adding ourselves to worker queue (see below).
    // So we are already processing task, but reside in worker queue.
    if (state_ == WorkerState::kWaitingTask) {
      DCHECK(!task_);
      task_ = task;
      cond_.notify_one();
    }
    return state_;
  }

  bool Idle() const {
    std::lock_guard lock(mutex_);
    return added_to_waiting_workers_;
  }

#ifdef __linux__
  void MoveToCurrentCgroup() {
    if (thread_) {
      auto* cg = share_.GetCgroup();
      if (cg) {
        auto status = cg->MoveThreadToGroup(thread_->tid());
        if (!status.ok()) {
          LOG(WARNING) << "Failed to move worker thread to cgroup: " << status;
        }
        current_cgroup_ = cg;
      }
    }
  }

  void MoveToPoolCgroup() {
    auto* cg = share_.GetCgroup();
    if (!cg) cg = DefaultThreadCgroup();
    MoveToCgroup(cg);
  }

  void MoveToCgroup(Cgroup* target) {
    if (target && target != current_cgroup_) {
      auto status = target->MoveCurrentThreadToGroup();
      if (status.ok()) {
        current_cgroup_ = target;
      } else {
        LOG(WARNING) << "Failed to move thread to cgroup: " << status;
        current_cgroup_ = nullptr;
      }
    }
  }
#endif

 private:
  // Our main invariant is empty task queue or empty worker queue.
  // In other words, one of those queues should be empty.
  // Meaning that we do not have work (task queue empty) or
  // does not have free hands (worker queue empty)
  void Execute(ThreadPoolTask* task) {
#ifdef __linux__
    MoveToPoolCgroup();
#endif

    Thread::current_thread()->SetUserData(&share_);
    bool has_run_metrics = share_.options.metrics.run_time_us_stats != nullptr;
    if (!task) {
      task = PopTask();
    }
    while (task) {
#ifdef __linux__
      if (auto* task_cg = task->cgroup()) {
        MoveToCgroup(task_cg);
      } else {
        MoveToPoolCgroup();
      }
#endif
      auto start = MonoTime::NowIf(has_run_metrics);
      dist_trace::ScopedAdoptSpan parent_scope(task->trace_parent());
      if (!task->run_token()) {
        task->Run();
        task->Done(Status::OK());
      } else {
        auto run_token = task->run_token()->lock();
        if (run_token) {
          task->Run();
          task->Done(Status::OK());
        } else {
          task->Done(kShuttingDownStatus);
        }
      }
      if (has_run_metrics) {
        share_.options.metrics.run_time_us_stats->Increment(
            (MonoTime::Now() - start).ToMicroseconds());
      }
      task = PopTask();
    }
  }

  ThreadPoolTask* PopTask() {
    // First of all we try to get already queued task, w/o locking.
    // If there is no task, so we could go to waiting state.
    if (auto* task = share_.PopTask()) {
      return task;
    }

    UniqueLock lock(mutex_);
    if (auto task_opt = DoPopTask()) {
      return *task_opt;
    }

    state_ = WorkerState::kWaitingTask;

    for (;;) {
      AddToWaitingWorkers();

      if (auto task = share_.PopTask()) {
        state_ = WorkerState::kRunning;
        return task;
      }

      bool timeout;
      if (!persistent_ && share_.options.idle_timeout) {
        CHECK(!task_);
        auto duration = share_.options.idle_timeout.ToSteadyDuration();
        timeout = cond_.wait_for(GetLockForCondition(lock), duration) == std::cv_status::timeout;
      } else {
        cond_.wait(GetLockForCondition(lock));
        timeout = false;
      }

      if (state_ == WorkerState::kExternalStop) {
        return nullptr;
      }
      if (task_) {
        state_ = WorkerState::kRunning;
        return std::exchange(task_, nullptr);
      }

      if (timeout && added_to_waiting_workers_) {
        if (auto task = share_.PopTask()) {
          state_ = WorkerState::kRunning;
          return task;
        }
        --share_.num_workers;
        state_ = WorkerState::kIdleStop;
        share_.waiting_workers->Abandon(link_);
        auto thread = std::move(thread_);
        return nullptr;
      }
    }
  }

  std::optional<ThreadPoolTask*> DoPopTask() REQUIRES(mutex_) {
    if (state_ == WorkerState::kExternalStop) {
      return nullptr;
    }
    if (task_) {
      return std::exchange(task_, nullptr);
    }
    if (auto task = share_.PopTask()) {
      return task;
    }
    return std::nullopt;
  }

  void AddToWaitingWorkers() REQUIRES(mutex_) {
    if (!added_to_waiting_workers_) {
      share_.waiting_workers->Push(link_);
      added_to_waiting_workers_ = true;
    }
  }

  ThreadPoolShare& share_;
  const bool persistent_;
  WorkerLink* link_ = nullptr;
  scoped_refptr<Thread> thread_;
#ifdef __linux__
  std::atomic<Cgroup*> current_cgroup_{nullptr};
#endif
  mutable std::mutex mutex_;
  std::condition_variable cond_;
  WorkerState state_ GUARDED_BY(mutex_) = WorkerState::kRunning;
  bool added_to_waiting_workers_ GUARDED_BY(mutex_) = false;
  ThreadPoolTask* task_ GUARDED_BY(mutex_) = nullptr;
};

struct WorkerLink {
  Worker* worker = nullptr;
  std::atomic<WorkerLink*> next_link{nullptr};
  // Whether MixedWaitingWorkers counts the link in its write stack size. A pop and an Abandon of
  // the same link race to clear it, so the link is uncounted once.
  std::atomic<bool> counted{false};
};

using Workers = boost::intrusive::list<Worker>;

void SetNext(WorkerLink& worker, WorkerLink* next) {
  worker.next_link.store(next, std::memory_order_release);
}

WorkerLink* GetNext(WorkerLink& worker) {
  return worker.next_link.load(std::memory_order_acquire);
}

class LifoWaitingWorkers : public WaitingWorkers {
 public:
  void Push(WorkerLink* link) override {
    stack_.Push(link);
  }

  WorkerLink* Pop() override {
    return stack_.Pop();
  }

 private:
  LockFreeStack<WorkerLink> stack_;
};

class FifoWaitingWorkers : public WaitingWorkers {
 public:
  void Push(WorkerLink* link) override {
    queue_.Push(link);
  }

  WorkerLink* Pop() override {
    return queue_.Pop();
  }

 private:
  SemiFairQueue<WorkerLink> queue_;
};

// Two stacks like SemiFairQueue: Push goes to write_, and when read_ is empty Pop moves the newest
// entries of write_ to read_, so that read_ pops in FIFO order. The move stops while the longest
// parked entries remain, at most kMaxRetained and at most as many as were taken. They are popped
// only when read_ is empty again, so under load wakes are FIFO while retained workers idle out.
class MixedWaitingWorkers : public WaitingWorkers {
 public:
  void Push(WorkerLink* link) override {
    write_.size.fetch_add(1, std::memory_order_relaxed);
    link->counted.store(true, std::memory_order_relaxed);
    write_.items.Push(link);
  }

  WorkerLink* Pop() override {
    if (auto link = read_.Pop()) {
      return link;
    }
    auto link = PopWrite();
    // Read once: pushes during the move are not counted, so a few more of the longest parked are
    // retained.
    auto size = write_.size.load(std::memory_order_relaxed);
    // Retains at most min(kMaxRetained, taken) links, where taken is the moved links plus the
    // returned one. With fewer than 2 * kMaxRetained parked workers only about half are retained,
    // so that wakes stay mostly FIFO; with two, one is.
    int64_t moved = 0;
    while (link && size > moved + std::min(kMaxRetained, moved + 1)) {
      auto next = PopWrite();
      if (!next) {
        break;
      }
      read_.Push(link);
      link = next;
      ++moved;
    }
    return link;
  }

  // Dead links stay in write_ until popped. Counted, they would be retained in place of live
  // workers, which would then all rotate through read_ and never idle out.
  void Abandon(WorkerLink* link) override {
    Uncount(link);
  }

 private:
  // Up to this many workers idle out per idle timeout under load.
  static constexpr int64_t kMaxRetained = 16;

  WorkerLink* PopWrite() {
    auto link = write_.items.Pop();
    if (link) {
      Uncount(link);
    }
    return link;
  }

  void Uncount(WorkerLink* link) {
    if (link->counted.exchange(false, std::memory_order_acq_rel)) {
      write_.size.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  struct alignas(CACHELINE_SIZE) WriteStack {
    LockFreeStack<WorkerLink> items;
    std::atomic<int64_t> size{0};
  };

  WriteStack write_;
  alignas(CACHELINE_SIZE) LockFreeStack<WorkerLink> read_;
};

std::unique_ptr<WaitingWorkers> CreateWaitingWorkers(WorkerWakeOrder order) {
  switch (order) {
    case WorkerWakeOrder::kLifo:
      return std::make_unique<LifoWaitingWorkers>();
    case WorkerWakeOrder::kFifo:
      return std::make_unique<FifoWaitingWorkers>();
    case WorkerWakeOrder::kMixed:
      return std::make_unique<MixedWaitingWorkers>();
  }
  FATAL_INVALID_ENUM_VALUE(WorkerWakeOrder, order);
}

class FreeWorkerLinks {
 public:
  ~FreeWorkerLinks() {
    while (auto link = free_links_.Pop()) {
      delete link;
    }
  }

  WorkerLink* Acquire() {
    auto link = free_links_.Pop();
    if (!link) {
      link = new WorkerLink{};
    }
    return link;
  }

  void Release(WorkerLink* link) {
    link->worker = nullptr;
    free_links_.Push(link);
  }

 private:
  LockFreeStack<WorkerLink> free_links_;
};

FreeWorkerLinks free_worker_links_;

Worker::Worker(ThreadPoolShare& share, bool persistent)
    : share_(share), persistent_(persistent),
      link_(free_worker_links_.Acquire()) {
  link_->worker = this;
}

Worker::~Worker() {
  {
    std::lock_guard lock(mutex_);
    DCHECK(!task_);
    if (state_ == WorkerState::kIdleStop) {
      free_worker_links_.Release(link_);
      return;
    }
  }
  if (thread_) {
    thread_->Join();
  }
  free_worker_links_.Release(link_);
}

} // namespace

class YBThreadPool::Impl {
 public:
  explicit Impl(ThreadPoolOptions options)
      : share_(std::move(options)) {
    if (detailed_logging) {
      LOG(INFO) << "Starting thread pool " << share_.options.ToString();
    } else {
      VLOG(1) << "Starting thread pool " << share_.options.ToString();
    }

    for (size_t index = 0; index != options.min_workers; ++index) {
      if (!TryStartNewWorker(nullptr, /* persistent = */ true).ok()) {
        break;
      }
    }
  }

  const ThreadPoolOptions& options() const {
    return share_.options;
  }

  bool IsClosing() const {
    return closing_.load(std::memory_order_relaxed);
  }

  bool Enqueue(ThreadPoolTask* task) EXCLUDES(mutex_) {
    ++adding_;
    if (closing_) {
      --adding_;
      task->Done(kShuttingDownStatus);
      return false;
    }

    if (share_.options.metrics.queue_length_stats) {
      share_.options.metrics.queue_length_stats->Increment(
          share_.task_queue_size.load(std::memory_order_relaxed));
    }

    if (NotifyWorker(task)) {
      --adding_;
      return true;
    }

    {
      auto start_worker_status = TryStartNewWorker(task, /* persistent= */ false);
      if (start_worker_status.ok()) {
        --adding_;
        return true;
      } else if (share_.options.max_workers == ThreadPoolOptions::kUnlimitedWorkersWithoutQueue) {
        task->Done(start_worker_status);
        --adding_;
        return false;
      }
    }

    // We can get here in 3 cases:
    // 1) Reached number of workers and all workers are busy.
    // 2) Failed to start a new thread for a worker.
    // 3) We are shutting down. In this case we add task to the queue, so it will be processed by
    //    Shutdown.
    share_.PushTask(task);
    --adding_;
    NotifyWorker(nullptr);
    return true;
  }

  Status TryStartNewWorker(ThreadPoolTask* task, bool persistent) EXCLUDES(mutex_) {
    Worker* worker = nullptr;
    if (share_.num_workers++ < share_.options.max_workers) {
      std::lock_guard lock(mutex_);
      if (!closing_) {
        auto new_worker = std::make_unique<Worker>(share_, persistent);
        workers_.push_back(*(worker = new_worker.release()));
      }
    }
    Status status;
    if (worker) {
      if (TEST_fail_to_create_second_thread_in_thread_pool_without_queue &&
          share_.options.max_workers == ThreadPoolOptions::kUnlimitedWorkersWithoutQueue &&
          worker_counter_ != 0) {
        status = STATUS_FORMAT(RuntimeError, "TEST: Artificial start thread failure");
      } else {
        status = worker->Start(++worker_counter_, task);
      }
      if (status.ok()) {
        if (task && share_.options.metrics.queue_time_us_stats) {
          share_.options.metrics.queue_time_us_stats->Increment(0);
        }
        return Status::OK();
      }
      bool empty;
      {
        std::lock_guard lock(mutex_);
        if (!closing_) {
          workers_.erase_and_dispose(workers_.iterator_to(*worker), std::default_delete<Worker>());
          empty = workers_.empty();
        } else {
          empty = false;
        }
      }
      if (empty) {
        LOG_WITH_PREFIX(FATAL) << "Unable to start first worker: " << status;
      } else {
        LOG_WITH_PREFIX(WARNING) << "Unable to start worker: " << status;
      }
    } else {
      static const Status kReachedWorkersLimitStatus = STATUS(
          RuntimeError, "Reached workers limit");
      status = kReachedWorkersLimitStatus;
    }
    --share_.num_workers;
    return status;
  }

  // Returns true if we found worker that will pick up this task, false otherwise.
  bool NotifyWorker(ThreadPoolTask* task) {
    while (auto link = share_.waiting_workers->Pop()) {
      auto state = link->worker->Notify(task);
      switch (state) {
        case WorkerState::kWaitingTask:
          if (task && share_.options.metrics.queue_time_us_stats) {
            share_.options.metrics.queue_time_us_stats->Increment(0);
          }
          return true;
        case WorkerState::kExternalStop: [[fallthrough]];
        case WorkerState::kRunning:
          break;
        case WorkerState::kIdleStop: {
          std::lock_guard lock(mutex_);
          if (!closing_) {
            workers_.erase_and_dispose(
                workers_.iterator_to(*link->worker), std::default_delete<Worker>());
          }
        } break;
      }
    }
    return false;
  }

  std::string LogPrefix() const {
    return share_.options.name + ": ";
  }

  void Shutdown() EXCLUDES(mutex_) {
    // Prevent new worker threads from being created by pretending a large number of workers have
    // already been created.
    share_.num_workers ^= kStopCreatingWorkersFlag;
    decltype(workers_) workers;
    {
      std::lock_guard lock(mutex_);
      if (closing_) {
        CHECK(share_.task_queue.Empty());
        CHECK(workers_.empty());
        return;
      }
      closing_ = true;
      workers = std::move(workers_);
    }
    for (auto& worker : workers) {
      worker.Stop();
    }
    // Shutdown is quite a rare situation otherwise, and enqueue is quite frequent.
    // Because of this we use "atomic lock" in enqueue and busy wait in shutdown.
    // So we could process enqueue quickly, and it is OK if we get stuck in shutdown for some time.
    while (adding_ != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
      std::lock_guard lock(mutex_);
      if (!workers_.empty()) {
        LOG_WITH_PREFIX(DFATAL) << "Workers were added while closing: " << workers_.size();
        workers_.clear();
      }
    }
    while (auto* task = share_.PopTask()) {
      task->Done(kShuttingDownStatus);
    }

    workers.clear_and_dispose(std::default_delete<Worker>());
  }

  bool Owns(Thread* thread) {
    return thread && thread->user_data() == &share_;
  }

  bool BusyWait(MonoTime deadline) {
    while (!Idle()) {
      if (deadline && MonoTime::Now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  bool Idle() {
    if (adding_ || !share_.task_queue.Empty()) {
      return false;
    }
    std::lock_guard lock(mutex_);
    for (const auto& worker : workers_) {
      if (!worker.Idle()) {
        return false;
      }
    }
    return true;
  }

  size_t NumWorkers() const {
    return share_.num_workers.load(std::memory_order_relaxed) & ~kStopCreatingWorkersFlag;
  }

  size_t TEST_NumWorkersCreated() const {
    return worker_counter_.load(std::memory_order_relaxed);
  }

#ifdef __linux__
  void SetCgroup(Cgroup* cgroup) EXCLUDES(mutex_) {
    share_.cgroup.store(cgroup, std::memory_order_release);
    if (!cgroup) {
      return;
    }
    std::lock_guard lock(mutex_);
    for (auto& worker : workers_) {
      worker.MoveToCurrentCgroup();
    }
  }
#endif

 private:
  ThreadPoolShare share_;
  Workers workers_ GUARDED_BY(mutex_);
  // An atomic counterpart of workers_.size() during normal operation. During shutdown, this is set
  // to an unrealistically high number to prevent new workers from being created.
  std::atomic<size_t> worker_counter_{0};
  std::mutex mutex_;
  std::atomic<bool> closing_ = {false};
  std::atomic<size_t> adding_ = {0};
};

// ------------------------------------------------------------------------------------------------

YBThreadPool::YBThreadPool(ThreadPoolOptions options)
    : impl_(new Impl(std::move(options))) {
}

YBThreadPool::YBThreadPool(YBThreadPool&& rhs) noexcept
    : impl_(std::move(rhs.impl_)) {}

YBThreadPool& YBThreadPool::operator=(YBThreadPool&& rhs) noexcept {
  impl_->Shutdown();
  impl_ = std::move(rhs.impl_);
  return *this;
}

YBThreadPool::~YBThreadPool() {
  if (impl_) {
    impl_->Shutdown();
  }
}

bool YBThreadPool::Enqueue(ThreadPoolTask* task) {
  task->set_trace_parent(dist_trace::GetActiveSpanContext());
  return impl_->Enqueue(task);
}

void YBThreadPool::Shutdown() {
  impl_->Shutdown();
}

bool YBThreadPool::IsClosing() const {
  return impl_->IsClosing();
}

const ThreadPoolOptions& YBThreadPool::options() const {
  return impl_->options();
}

bool YBThreadPool::Owns(Thread* thread) {
  return impl_->Owns(thread);
}

bool YBThreadPool::OwnsThisThread() {
  return Owns(Thread::current_thread());
}

bool YBThreadPool::BusyWait(MonoTime deadline) {
  return impl_->BusyWait(deadline);
}

size_t YBThreadPool::NumWorkers() const {
  return impl_->NumWorkers();
}

size_t YBThreadPool::TEST_NumWorkersCreated() const {
  return impl_->TEST_NumWorkersCreated();
}

bool YBThreadPool::Idle() const {
  return impl_->Idle();
}

#ifdef __linux__
void YBThreadPool::SetCgroup(Cgroup* cgroup) {
  impl_->SetCgroup(cgroup);
}
#endif

// ------------------------------------------------------------------------------------------------
// ThreadSubPoolBase
// ------------------------------------------------------------------------------------------------

void ThreadSubPoolBase::Shutdown() {
  auto active_enqueues = active_enqueues_.fetch_or(kStopMark, std::memory_order_acq_rel);
  if (active_enqueues >= kStopMark) {
    while (!(active_enqueues & kStoppedMark)) {
      std::this_thread::sleep_for(1ms);
      active_enqueues = active_enqueues_.load(std::memory_order_acquire);
    }
    return;
  }
  while (active_enqueues & (kStopMark - 1)) {
    std::this_thread::sleep_for(1ms);
    active_enqueues = active_enqueues_.load(std::memory_order_acquire);
  }
  std::weak_ptr<void> run_token = std::exchange(run_token_, nullptr);
  // We expected shutdown to happen rarely, so just use busy wait here.
  while (!run_token.expired()) {
    std::this_thread::sleep_for(1ms);
  }
  AbortTasks();
  active_enqueues_.fetch_or(kStoppedMark, std::memory_order_acq_rel);
}

void ThreadSubPoolBase::AbortTasks() {
}

// ------------------------------------------------------------------------------------------------
// ThreadSubPool
// ------------------------------------------------------------------------------------------------

ThreadSubPool::ThreadSubPool(YBThreadPool* thread_pool) : ThreadSubPoolBase(thread_pool) {
}

ThreadSubPool::~ThreadSubPool() {
}

bool ThreadSubPool::Enqueue(ThreadPoolTask* task) {
  return EnqueueHelper([this, task](bool ok) {
    if (ok) {
      task->set_run_token(run_token_);
      return thread_pool_.Enqueue(task);
    }
    task->Done(STATUS(Aborted, "Thread sub-pool closing"));
    return false;
  });
}

WorkerWakeOrder DefaultWorkerWakeOrder() {
  // Not cached: some pools, such as the MetricRegistry cleanup pool, are created before flags are
  // parsed, and a cached value would give every later pool the built-in default.
  auto result = ParseWorkerWakeOrder(FLAGS_thread_pool_worker_wake_order);
  if (!result.ok()) {
    LOG(DFATAL) << "Bad --thread_pool_worker_wake_order: " << result.status();
    return WorkerWakeOrder::kMixed;
  }
  return *result;
}

MonoDelta DefaultIdleTimeout() {
  return MonoDelta::FromMilliseconds(FLAGS_default_idle_timeout_ms);
}


// ------------------------------------------------------------------------------------------------
// YBTaggedThreadPools
// ------------------------------------------------------------------------------------------------

YBTaggedThreadPools::YBTaggedThreadPools(YBTaggedThreadPools::OptionsGenerator options_generator)
    : options_generator_{std::move(options_generator)} {}

YBTaggedThreadPools::~YBTaggedThreadPools() {
  Shutdown();
}

void YBTaggedThreadPools::Shutdown() {
  PoolMap pools;
  {
    std::lock_guard lock(mutex_);
    shutting_down_ = true;
    std::swap(pools, pools_);
  }
  for (auto& pool : pools | std::views::values) {
    pool->Shutdown();
  }
}

YBThreadPoolPtr YBTaggedThreadPools::LookupPool(Tag tag) {
  auto itr = pools_.find(tag);
  if (itr != pools_.end()) {
    return itr->second;
  }
  return nullptr;
}

Result<YBThreadPoolPtr> YBTaggedThreadPools::Pool(Tag tag) {
  {
    SharedLock lock(mutex_);
    if (auto pool = LookupPool(tag)) {
      return pool;
    }
  }

  std::vector<YBThreadPoolPtr> to_shutdown;
  YBThreadPoolPtr pool;
  {
    std::lock_guard lock(mutex_);
    if (auto pool = LookupPool(tag)) {
      return pool;
    }
    if (shutting_down_) {
      return STATUS(ShutdownInProgress, "thread pools shutting down");
    }
    to_shutdown = CleanupIdlePools();

    pool = pools_.try_emplace(
      tag, std::make_shared<YBThreadPool>(options_generator_(tag))).first->second;
  }

  for (const auto& shutdown_pool : to_shutdown) {
    shutdown_pool->Shutdown();
  }
  return pool;
}

size_t YBTaggedThreadPools::ActivePools() {
  SharedLock lock(mutex_);
  return pools_.size();
}

std::vector<YBThreadPoolPtr> YBTaggedThreadPools::CleanupIdlePools() {
  std::vector<YBThreadPoolPtr> to_shutdown;
  auto itr = pools_.begin();
  while (itr != pools_.end()) {
    auto& pool = itr->second;
    // use_count() and shared_ptr reference count increment are both relaxed memory ordering. But
    // obtaining a reference from YBTaggedThreadPools requires mutex, and assuming absence of
    // weak_ptr, it is impossible to obtain a reference from outside YBTaggedThreadPools when
    // use_count() == 1. So there is no issue with use_count() increasing after this check, unless
    // weak_ptr is used.
    if (pool.use_count() == 1 && pool->NumWorkers() == 0) {
      to_shutdown.emplace_back(std::move(pool));
      itr = pools_.erase(itr);
    } else {
      ++itr;
    }
  }
  return to_shutdown;
}

} // namespace yb
