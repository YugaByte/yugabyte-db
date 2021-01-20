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

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "yb/gutil/callback.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/sysinfo.h"

#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/errno.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/stopwatch.h"
#include "yb/util/thread.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"

namespace yb {

using strings::Substitute;
using std::unique_ptr;

////////////////////////////////////////////////////////
// ThreadPoolBuilder
///////////////////////////////////////////////////////

ThreadPoolBuilder::ThreadPoolBuilder(std::string name)
    : name_(std::move(name)),
      min_threads_(0),
      max_threads_(base::NumCPUs()),
      max_queue_size_(std::numeric_limits<int>::max()),
      idle_timeout_(MonoDelta::FromMilliseconds(500)) {}

ThreadPoolBuilder& ThreadPoolBuilder::set_min_threads(int min_threads) {
  CHECK_GE(min_threads, 0);
  min_threads_ = min_threads;
  return *this;
}

ThreadPoolBuilder& ThreadPoolBuilder::set_max_threads(int max_threads) {
  CHECK_GE(max_threads, 0);
  max_threads_ = max_threads;
  return *this;
}

ThreadPoolBuilder& ThreadPoolBuilder::unlimited_threads() {
  max_threads_ = std::numeric_limits<int>::max();
  return *this;
}

ThreadPoolBuilder& ThreadPoolBuilder::set_max_queue_size(int max_queue_size) {
  CHECK_GE(max_queue_size, 0);
  max_queue_size_ = max_queue_size;
  return *this;
}

ThreadPoolBuilder& ThreadPoolBuilder::set_metrics(ThreadPoolMetrics metrics) {
  metrics_ = std::move(metrics);
  return *this;
}

ThreadPoolBuilder& ThreadPoolBuilder::set_idle_timeout(const MonoDelta& idle_timeout) {
  idle_timeout_ = idle_timeout;
  return *this;
}

Status ThreadPoolBuilder::Build(gscoped_ptr<ThreadPool>* pool) const {
  pool->reset(new ThreadPool(*this));
  RETURN_NOT_OK((*pool)->Init());
  return Status::OK();
}

Status ThreadPoolBuilder::Build(unique_ptr<ThreadPool>* pool) const {
  pool->reset(new ThreadPool(*this));
  RETURN_NOT_OK((*pool)->Init());
  return Status::OK();
}

////////////////////////////////////////////////////////
// ThreadPoolToken
////////////////////////////////////////////////////////

ThreadPoolToken::ThreadPoolToken(ThreadPool* pool,
                                 ThreadPool::ExecutionMode mode,
                                 ThreadPoolMetrics metrics)
    : mode_(mode),
      pool_(pool),
      metrics_(std::move(metrics)),
      state_(ThreadPoolTokenState::kIdle),
      not_running_cond_(&pool->lock_),
      active_threads_(0) {
}

ThreadPoolToken::~ThreadPoolToken() {
  Shutdown();
  pool_->ReleaseToken(this);
}

Status ThreadPoolToken::SubmitClosure(Closure c) {
  return Submit(std::make_shared<FunctionRunnable>((std::bind(&Closure::Run, c))));
}

Status ThreadPoolToken::SubmitFunc(std::function<void()> f) {
  return Submit(std::make_shared<FunctionRunnable>(std::move(f)));
}

Status ThreadPoolToken::Submit(std::shared_ptr<Runnable> r) {
  return pool_->DoSubmit(std::move(r), this);
}

void ThreadPoolToken::Shutdown() {
  MutexLock unique_lock(pool_->lock_);
  pool_->CheckNotPoolThreadUnlocked();

  // Clear the queue under the lock, but defer the releasing of the tasks
  // outside the lock, in case there are concurrent threads wanting to access
  // the ThreadPool. The task's destructors may acquire locks, etc, so this
  // also prevents lock inversions.
  deque<ThreadPool::Task> to_release = std::move(entries_);
  pool_->total_queued_tasks_ -= to_release.size();

  switch (state()) {
    case ThreadPoolTokenState::kIdle:
      // There were no tasks outstanding; we can quiesce the token immediately.
      Transition(ThreadPoolTokenState::kQuiesced);
      break;
    case ThreadPoolTokenState::kRunning:
      // There were outstanding tasks. If any are still running, switch to
      // kQuiescing and wait for them to finish (the worker thread executing
      // the token's last task will switch the token to kQuiesced). Otherwise,
      // we can quiesce the token immediately.

      // Note: this is an O(n) operation, but it's expected to be infrequent.
      // Plus doing it this way (rather than switching to kQuiescing and waiting
      // for a worker thread to process the queue entry) helps retain state
      // transition symmetry with ThreadPool::Shutdown.
      for (auto it = pool_->queue_.begin(); it != pool_->queue_.end();) {
        if (*it == this) {
          it = pool_->queue_.erase(it);
        } else {
          it++;
        }
      }

      if (active_threads_ == 0) {
        Transition(ThreadPoolTokenState::kQuiesced);
        break;
      }
      Transition(ThreadPoolTokenState::kQuiescing);
      FALLTHROUGH_INTENDED;
    case ThreadPoolTokenState::kQuiescing:
      // The token is already quiescing. Just wait for a worker thread to
      // switch it to kQuiesced.
      while (state() != ThreadPoolTokenState::kQuiesced) {
        not_running_cond_.Wait();
      }
      break;
    default:
      break;
  }

  // Finally release the queued tasks, outside the lock.
  unique_lock.Unlock();
  for (auto& t : to_release) {
    if (t.trace) {
      t.trace->Release();
    }
  }
}

void ThreadPoolToken::Wait() {
  MutexLock unique_lock(pool_->lock_);
  pool_->CheckNotPoolThreadUnlocked();
  while (IsActive()) {
    not_running_cond_.Wait();
  }
}

bool ThreadPoolToken::WaitUntil(const MonoTime& until) {
  MutexLock unique_lock(pool_->lock_);
  pool_->CheckNotPoolThreadUnlocked();
  while (IsActive()) {
    if (!not_running_cond_.WaitUntil(until)) {
      return false;
    }
  }
  return true;
}

bool ThreadPoolToken::WaitFor(const MonoDelta& delta) {
  return WaitUntil(MonoTime::Now() + delta);
}

void ThreadPoolToken::Transition(ThreadPoolTokenState new_state) {
#ifndef NDEBUG
  CHECK_NE(state_, new_state);

  switch (state_) {
    case ThreadPoolTokenState::kIdle:
      CHECK(new_state == ThreadPoolTokenState::kRunning ||
            new_state == ThreadPoolTokenState::kQuiesced);
      if (new_state == ThreadPoolTokenState::kRunning) {
        CHECK(!entries_.empty());
      } else {
        CHECK(entries_.empty());
        CHECK_EQ(active_threads_, 0);
      }
      break;
    case ThreadPoolTokenState::kRunning:
      CHECK(new_state == ThreadPoolTokenState::kIdle ||
            new_state == ThreadPoolTokenState::kQuiescing ||
            new_state == ThreadPoolTokenState::kQuiesced);
      CHECK(entries_.empty());
      if (new_state == ThreadPoolTokenState::kQuiescing) {
        CHECK_GT(active_threads_, 0);
      }
      break;
    case ThreadPoolTokenState::kQuiescing:
      CHECK(new_state == ThreadPoolTokenState::kQuiesced);
      CHECK_EQ(active_threads_, 0);
      break;
    case ThreadPoolTokenState::kQuiesced:
      CHECK(false); // kQuiesced is a terminal state
      break;
    default:
      LOG(FATAL) << "Unknown token state: " << state_;
  }
#endif

  // Take actions based on the state we're entering.
  switch (new_state) {
    case ThreadPoolTokenState::kIdle:
    case ThreadPoolTokenState::kQuiesced:
      not_running_cond_.Broadcast();
      break;
    default:
      break;
  }

  state_ = new_state;
}

const char* ThreadPoolToken::StateToString(ThreadPoolTokenState s) {
  switch (s) {
    case ThreadPoolTokenState::kIdle: return "kIdle"; break;
    case ThreadPoolTokenState::kRunning: return "kRunning"; break;
    case ThreadPoolTokenState::kQuiescing: return "kQuiescing"; break;
    case ThreadPoolTokenState::kQuiesced: return "kQuiesced"; break;
  }
  return "<cannot reach here>";
}

////////////////////////////////////////////////////////
// ThreadPool
////////////////////////////////////////////////////////

ThreadPool::ThreadPool(const ThreadPoolBuilder& builder)
  : name_(builder.name_),
    min_threads_(builder.min_threads_),
    max_threads_(builder.max_threads_),
    max_queue_size_(builder.max_queue_size_),
    idle_timeout_(builder.idle_timeout_),
    pool_status_(STATUS(Uninitialized, "The pool was not initialized.")),
    idle_cond_(&lock_),
    no_threads_cond_(&lock_),
    not_empty_(&lock_),
    num_threads_(0),
    active_threads_(0),
    total_queued_tasks_(0),
    tokenless_(NewToken(ExecutionMode::CONCURRENT)),
    metrics_(builder.metrics_) {
}

ThreadPool::~ThreadPool() {
  // There should only be one live token: the one used in tokenless submission.
  CHECK_EQ(1, tokens_.size()) << Substitute(
      "Threadpool $0 destroyed with $1 allocated tokens",
      name_, tokens_.size());
  Shutdown();
}

Status ThreadPool::Init() {
  MutexLock unique_lock(lock_);
  if (!pool_status_.IsUninitialized()) {
    return STATUS(NotSupported, "The thread pool is already initialized");
  }
  pool_status_ = Status::OK();
  for (int i = 0; i < min_threads_; i++) {
    Status status = CreateThreadUnlocked();
    if (!status.ok()) {
      if (i != 0) {
        YB_LOG_EVERY_N_SECS(WARNING, 5) << "Cannot create thread: " << status << ", will try later";
        // Cannot create enough threads now, will try later.
        break;
      }
      unique_lock.Unlock();
      Shutdown();
      return status;
    }
  }
  return Status::OK();
}

void ThreadPool::Shutdown() {
  MutexLock unique_lock(lock_);
  CheckNotPoolThreadUnlocked();

  // Note: this is the same error seen at submission if the pool is at
  // capacity, so clients can't tell them apart. This isn't really a practical
  // concern though because shutting down a pool typically requires clients to
  // be quiesced first, so there's no danger of a client getting confused.
  pool_status_ = STATUS(ServiceUnavailable, "The pool has been shut down.");

  // Clear the various queues under the lock, but defer the releasing
  // of the tasks outside the lock, in case there are concurrent threads
  // wanting to access the ThreadPool. The task's destructors may acquire
  // locks, etc, so this also prevents lock inversions.
  queue_.clear();
  deque<deque<Task>> to_release;
  for (auto* t : tokens_) {
    if (!t->entries_.empty()) {
      to_release.emplace_back(std::move(t->entries_));
    }
    switch (t->state()) {
      case ThreadPoolTokenState::kIdle:
        // The token is idle; we can quiesce it immediately.
        t->Transition(ThreadPoolTokenState::kQuiesced);
        break;
      case ThreadPoolTokenState::kRunning:
        // The token has tasks associated with it. If they're merely queued
        // (i.e. there are no active threads), the tasks will have been removed
        // above and we can quiesce immediately. Otherwise, we need to wait for
        // the threads to finish.
        t->Transition(t->active_threads_ > 0 ?
            ThreadPoolTokenState::kQuiescing :
            ThreadPoolTokenState::kQuiesced);
        break;
      default:
        break;
    }
  }

  // The queues are empty. Wake any sleeping worker threads and wait for all
  // of them to exit. Some worker threads will exit immediately upon waking,
  // while others will exit after they finish executing an outstanding task.
  total_queued_tasks_ = 0;
  not_empty_.Broadcast();
  while (num_threads_ > 0) {
    no_threads_cond_.Wait();
  }

  // All the threads have exited. Check the state of each token.
  for (auto* t : tokens_) {
    DCHECK(t->state() == ThreadPoolTokenState::kIdle ||
           t->state() == ThreadPoolTokenState::kQuiesced);
  }

  // Finally release the queued tasks, outside the lock.
  unique_lock.Unlock();
  for (auto& token : to_release) {
    for (auto& t : token) {
      if (t.trace) {
        t.trace->Release();
      }
    }
  }
}

unique_ptr<ThreadPoolToken> ThreadPool::NewToken(ExecutionMode mode) {
  return NewTokenWithMetrics(mode, {});
}

unique_ptr<ThreadPoolToken> ThreadPool::NewTokenWithMetrics(
    ExecutionMode mode, ThreadPoolMetrics metrics) {
  MutexLock guard(lock_);
  unique_ptr<ThreadPoolToken> t(new ThreadPoolToken(this, mode, std::move(metrics)));
  InsertOrDie(&tokens_, t.get());
  return t;
}

void ThreadPool::ReleaseToken(ThreadPoolToken* t) {
  MutexLock guard(lock_);
  CHECK(!t->IsActive()) << Substitute("Token with state $0 may not be released",
                                      ThreadPoolToken::StateToString(t->state()));
  CHECK_EQ(1, tokens_.erase(t));
}


Status ThreadPool::SubmitClosure(const Closure& task) {
  // TODO: once all uses of std::bind-based tasks are dead, implement this
  // in a more straight-forward fashion.
  return SubmitFunc(std::bind(&Closure::Run, task));
}

Status ThreadPool::SubmitFunc(const std::function<void()>& func) {
  return Submit(std::make_shared<FunctionRunnable>(func));
}

Status ThreadPool::SubmitFunc(std::function<void()>&& func) {
  return Submit(std::make_shared<FunctionRunnable>(std::move(func)));
}

Status ThreadPool::Submit(const std::shared_ptr<Runnable>& r) {
  return DoSubmit(std::move(r), tokenless_.get());
}

Status ThreadPool::DoSubmit(const std::shared_ptr<Runnable> task, ThreadPoolToken* token) {
  DCHECK(token);
  MonoTime submit_time = MonoTime::Now();

  MutexLock guard(lock_);
  if (PREDICT_FALSE(!pool_status_.ok())) {
    return pool_status_;
  }

  if (PREDICT_FALSE(!token->MaySubmitNewTasks())) {
    return STATUS(ServiceUnavailable, "Thread pool token was shut down.", "", Errno(ESHUTDOWN));
  }

  // Size limit check.
  int64_t capacity_remaining = static_cast<int64_t>(max_threads_) - active_threads_ +
                               static_cast<int64_t>(max_queue_size_) - total_queued_tasks_;
  if (capacity_remaining < 1) {
    return STATUS(ServiceUnavailable,
                  Substitute("Thread pool is at capacity ($0/$1 tasks running, $2/$3 tasks queued)",
                             num_threads_, max_threads_, total_queued_tasks_, max_queue_size_),
                  "", Errno(ESHUTDOWN));
  }

  // Should we create another thread?
  // We assume that each current inactive thread will grab one item from the
  // queue.  If it seems like we'll need another thread, we create one.
  // In theory, a currently active thread could finish immediately after this
  // calculation.  This would mean we created a thread we didn't really need.
  // However, this race is unavoidable, since we don't do the work under a lock.
  // It's also harmless.
  //
  // Of course, we never create more than max_threads_ threads no matter what.
  int threads_from_this_submit =
      token->IsActive() && token->mode() == ExecutionMode::SERIAL ? 0 : 1;
  int inactive_threads = num_threads_ - active_threads_;
  int additional_threads = (queue_.size() + threads_from_this_submit) - inactive_threads;
  if (additional_threads > 0 && num_threads_ < max_threads_) {
    Status status = CreateThreadUnlocked();
    if (!status.ok()) {
      // If we failed to create a thread, but there are still some other
      // worker threads, log a warning message and continue.
      LOG(WARNING) << "Thread pool failed to create thread: " << status << ", num_threads: "
                   << num_threads_ << ", max_threads: " << max_threads_;
      if (num_threads_ == 0) {
        // If we have no threads, we can't do any work.
        return status;
      }
    }
  }

  Task e;
  e.runnable = task;
  e.trace = Trace::CurrentTrace();
  // Need to AddRef, since the thread which submitted the task may go away,
  // and we don't want the trace to be destructed while waiting in the queue.
  if (e.trace) {
    e.trace->AddRef();
  }
  e.submit_time = submit_time;

  // Add the task to the token's queue.
  ThreadPoolTokenState state = token->state();
  DCHECK(state == ThreadPoolTokenState::kIdle ||
         state == ThreadPoolTokenState::kRunning);
  token->entries_.emplace_back(std::move(e));
  if (state == ThreadPoolTokenState::kIdle ||
      token->mode() == ExecutionMode::CONCURRENT) {
    queue_.emplace_back(token);
    if (state == ThreadPoolTokenState::kIdle) {
      token->Transition(ThreadPoolTokenState::kRunning);
    }
  }
  int length_at_submit = total_queued_tasks_++;

  guard.Unlock();
  not_empty_.Signal();

  if (metrics_.queue_length_histogram) {
    metrics_.queue_length_histogram->Increment(length_at_submit);
  }
  if (token->metrics_.queue_length_histogram) {
    token->metrics_.queue_length_histogram->Increment(length_at_submit);
  }

  return Status::OK();
}

void ThreadPool::Wait() {
  MutexLock unique_lock(lock_);
  while ((!queue_.empty()) || (active_threads_ > 0)) {
    idle_cond_.Wait();
  }
}

bool ThreadPool::WaitUntil(const MonoTime& until) {
  MutexLock unique_lock(lock_);
  while ((!queue_.empty()) || (active_threads_ > 0)) {
    if (!idle_cond_.WaitUntil(until)) {
      return false;
    }
  }
  return true;
}

bool ThreadPool::WaitFor(const MonoDelta& delta) {
  return WaitUntil(MonoTime::Now() + delta);
}

void ThreadPool::DispatchThread(bool permanent) {
  MutexLock unique_lock(lock_);
  while (true) {
    // Note: STATUS(Aborted, ) is used to indicate normal shutdown.
    if (!pool_status_.ok()) {
      VLOG(2) << "DispatchThread exiting: " << pool_status_.ToString();
      break;
    }

    if (queue_.empty()) {
      if (permanent) {
        not_empty_.Wait();
      } else {
        if (!not_empty_.TimedWait(idle_timeout_)) {
          // After much investigation, it appears that pthread condition variables have
          // a weird behavior in which they can return ETIMEDOUT from timed_wait even if
          // another thread did in fact signal. Apparently after a timeout there is some
          // brief period during which another thread may actually grab the internal mutex
          // protecting the state, signal, and release again before we get the mutex. So,
          // we'll recheck the empty queue case regardless.
          if (queue_.empty()) {
            VLOG(3) << "Releasing worker thread from pool " << name_ << " after "
                    << idle_timeout_.ToMilliseconds() << "ms of idle time.";
            break;
          }
        }
      }
      continue;
    }

    // Get the next token and task to execute.
    ThreadPoolToken* token = queue_.front();
    queue_.pop_front();
    DCHECK_EQ(ThreadPoolTokenState::kRunning, token->state());
    DCHECK(!token->entries_.empty());
    Task task = std::move(token->entries_.front());
    token->entries_.pop_front();
    token->active_threads_++;
    --total_queued_tasks_;
    ++active_threads_;

    unique_lock.Unlock();

    // Release the reference which was held by the queued item.
    ADOPT_TRACE(task.trace);
    if (task.trace) {
      task.trace->Release();
    }

    // Update metrics
    MonoTime now(MonoTime::Now());
    int64_t queue_time_us = (now - task.submit_time).ToMicroseconds();
    if (metrics_.queue_time_us_histogram) {
      metrics_.queue_time_us_histogram->Increment(queue_time_us);
    }
    if (token->metrics_.queue_time_us_histogram) {
      token->metrics_.queue_time_us_histogram->Increment(queue_time_us);
    }

    // Execute the task
    {
      MicrosecondsInt64 start_wall_us = GetMonoTimeMicros();
      task.runnable->Run();
      int64_t wall_us = GetMonoTimeMicros() - start_wall_us;

      if (metrics_.run_time_us_histogram) {
        metrics_.run_time_us_histogram->Increment(wall_us);
      }
      if (token->metrics_.run_time_us_histogram) {
        token->metrics_.run_time_us_histogram->Increment(wall_us);
      }
    }
    // Destruct the task while we do not hold the lock.
    //
    // The task's destructor may be expensive if it has a lot of bound
    // objects, and we don't want to block submission of the threadpool.
    // In the worst case, the destructor might even try to do something
    // with this threadpool, and produce a deadlock.
    task.runnable.reset();
    unique_lock.Lock();

    // Possible states:
    // 1. The token was shut down while we ran its task. Transition to kQuiesced.
    // 2. The token has no more queued tasks. Transition back to kIdle.
    // 3. The token has more tasks. Requeue it and transition back to RUNNABLE.
    ThreadPoolTokenState state = token->state();
    DCHECK(state == ThreadPoolTokenState::kRunning ||
           state == ThreadPoolTokenState::kQuiescing);
    if (--token->active_threads_ == 0) {
      if (state == ThreadPoolTokenState::kQuiescing) {
        DCHECK(token->entries_.empty());
        token->Transition(ThreadPoolTokenState::kQuiesced);
      } else if (token->entries_.empty()) {
        token->Transition(ThreadPoolTokenState::kIdle);
      } else if (token->mode() == ExecutionMode::SERIAL) {
        queue_.emplace_back(token);
      }
    }
    if (--active_threads_ == 0) {
      idle_cond_.Broadcast();
    }
  }

  // It's important that we hold the lock between exiting the loop and dropping
  // num_threads_. Otherwise it's possible someone else could come along here
  // and add a new task just as the last running thread is about to exit.
  CHECK(unique_lock.OwnsLock());

  CHECK_EQ(threads_.erase(Thread::current_thread()), 1);
  if (--num_threads_ == 0) {
    no_threads_cond_.Broadcast();

    // Sanity check: if we're the last thread exiting, the queue ought to be
    // empty. Otherwise it will never get processed.
    CHECK(queue_.empty());
    DCHECK_EQ(0, total_queued_tasks_);
  }
}

Status ThreadPool::CreateThreadUnlocked() {
  // The first few threads are permanent, and do not time out.
  bool permanent = (num_threads_ < min_threads_);
  scoped_refptr<Thread> t;
  Status s = yb::Thread::Create("thread pool", strings::Substitute("$0 [worker]", name_),
                                  &ThreadPool::DispatchThread, this, permanent, &t);
  if (s.ok()) {
    InsertOrDie(&threads_, t.get());
    num_threads_++;
  }
  return s;
}

void ThreadPool::CheckNotPoolThreadUnlocked() {
  Thread* current = Thread::current_thread();
  if (ContainsKey(threads_, current)) {
    LOG(FATAL) << Substitute("Thread belonging to thread pool '$0' with "
        "name '$1' called pool function that would result in deadlock",
        name_, current->name());
  }
}

} // namespace yb
