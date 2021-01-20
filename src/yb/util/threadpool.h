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
#ifndef YB_UTIL_THREADPOOL_H
#define YB_UTIL_THREADPOOL_H

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

#include <gtest/gtest_prod.h>

#include "yb/gutil/callback_forward.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/port.h"
#include "yb/gutil/ref_counted.h"
#include "yb/util/condition_variable.h"
#include "yb/util/enums.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/mutex.h"
#include "yb/util/status.h"

namespace yb {

class Histogram;
class Thread;
class ThreadPool;
class ThreadPoolToken;
class Trace;

class Runnable {
 public:
  virtual void Run() = 0;
  virtual ~Runnable() {}
};

template <class F>
class RunnableImpl : public Runnable {
 public:
  explicit RunnableImpl(const F& f) : f_(f) {}
  explicit RunnableImpl(F&& f) : f_(std::move(f)) {}

 private:
  void Run() override {
    f_();
  }

  F f_;
};

// Interesting thread pool metrics. Can be applied to the entire pool (see
// ThreadPoolBuilder) or to individual tokens.
struct ThreadPoolMetrics {
  // Measures the queue length seen by tasks when they enter the queue.
  scoped_refptr<Histogram> queue_length_histogram;

  // Measures the amount of time that tasks spend waiting in a queue.
  scoped_refptr<Histogram> queue_time_us_histogram;

  // Measures the amount of time that tasks spend running.
  scoped_refptr<Histogram> run_time_us_histogram;
};


// THREAD_POOL_METRICS_DEFINE / THREAD_POOL_METRICS_INSTANCE are helpers which define the metrics
// required for a ThreadPoolMetrics object and instantiate said objects, respectively. Example
// usage:
// // At the top of the file:
// THREAD_POOL_METRICS_DEFINE(server, thread_pool_foo, "Thread pool for Foo jobs.")
// ...
// // Inline:
// ThreadPoolBuilder("foo")
//   .set_metrics(THREAD_POOL_METRICS_INSTANCE(server_->metric_entity(), thread_pool_foo))
//   ...
//   .Build(...);
#define THREAD_POOL_METRICS_DEFINE(entity, name, label) \
    METRIC_DEFINE_histogram(entity, BOOST_PP_CAT(name, _queue_length), \
        label " Queue Length", yb::MetricUnit::kMicroseconds, \
        label " - queue length histogram.", \
        10000000, 2); \
    METRIC_DEFINE_histogram(entity, BOOST_PP_CAT(name, _queue_time_us), \
        label " Queue Time", yb::MetricUnit::kMicroseconds, \
        label " - queue time histogram, microseconds.", \
        10000000, 2); \
    METRIC_DEFINE_histogram(entity, BOOST_PP_CAT(name, _run_time_us), \
        label " Run Time", yb::MetricUnit::kMicroseconds, \
        label " - run time histogram, microseconds.", \
        10000000, 2)

#define THREAD_POOL_METRICS_INSTANCE(entity, name) { \
      BOOST_PP_CAT(METRIC_, BOOST_PP_CAT(name, _run_time_us)).Instantiate(entity), \
      BOOST_PP_CAT(METRIC_, BOOST_PP_CAT(name, _queue_time_us)).Instantiate(entity), \
      BOOST_PP_CAT(METRIC_, BOOST_PP_CAT(name, _run_time_us)).Instantiate(entity) \
    }

// ThreadPool takes a lot of arguments. We provide sane defaults with a builder.
//
// name: Used for debugging output and default names of the worker threads.
//    Since thread names are limited to 16 characters on Linux, it's good to
//    choose a short name here.
//    Required.
//
// min_threads: Minimum number of threads we'll have at any time.
//    Default: 0.
//
// max_threads: Maximum number of threads we'll have at any time.
//    Default: Number of CPUs detected on the system.
//
// max_queue_size: Maximum number of items to enqueue before returning a
//    Status::ServiceUnavailable message from Submit().
//    Default: INT_MAX.
//
// timeout: How long we'll keep around an idle thread before timing it out.
//    We always keep at least min_threads.
//    Default: 500 milliseconds.
//
// metrics: Histograms, counters, etc. to update on various threadpool events.
//    Default: not set.
//
class ThreadPoolBuilder {
 public:
  explicit ThreadPoolBuilder(std::string name);

  // Note: We violate the style guide by returning mutable references here
  // in order to provide traditional Builder pattern conveniences.
  ThreadPoolBuilder& set_min_threads(int min_threads);
  ThreadPoolBuilder& set_max_threads(int max_threads);
  ThreadPoolBuilder& unlimited_threads();
  ThreadPoolBuilder& set_max_queue_size(int max_queue_size);
  ThreadPoolBuilder& set_idle_timeout(const MonoDelta& idle_timeout);
  ThreadPoolBuilder& set_metrics(ThreadPoolMetrics metrics);

  const std::string& name() const { return name_; }
  int min_threads() const { return min_threads_; }
  int max_threads() const { return max_threads_; }
  int max_queue_size() const { return max_queue_size_; }
  const MonoDelta& idle_timeout() const { return idle_timeout_; }

  // Instantiate a new ThreadPool with the existing builder arguments.
  CHECKED_STATUS Build(gscoped_ptr<ThreadPool>* pool) const;
  CHECKED_STATUS Build(std::unique_ptr<ThreadPool>* pool) const;

 private:
  friend class ThreadPool;
  const std::string name_;
  int min_threads_;
  int max_threads_;
  int max_queue_size_;
  MonoDelta idle_timeout_;
  ThreadPoolMetrics metrics_;

  DISALLOW_COPY_AND_ASSIGN(ThreadPoolBuilder);
};

// Thread pool with a variable number of threads.
// The pool can execute a class that implements the Runnable interface, or a
// std::function, which can be obtained via std::bind().
// Tasks submitted directly to the thread pool enter a FIFO queue and are
// dispatched to a worker thread when one becomes free. Tasks may also be
// submitted via ThreadPoolTokens. The token Wait() and Shutdown() functions
// can then be used to block on logical groups of tasks.
//
// A token operates in one of two ExecutionModes, determined at token
// construction time:
// 1. SERIAL: submitted tasks are run one at a time.
// 2. CONCURRENT: submitted tasks may be run in parallel. This isn't unlike
//    tasks submitted without a token, but the logical grouping that tokens
//    impart can be useful when a pool is shared by many contexts (e.g. to
//    safely shut down one context, to derive context-specific metrics, etc.).
//
// Tasks submitted without a token or via ExecutionMode::CONCURRENT tokens are
// processed in FIFO order. On the other hand, ExecutionMode::SERIAL tokens are
// processed in a round-robin fashion, one task at a time. This prevents them
// from starving one another. However, tokenless (and CONCURRENT token-based)
// tasks can starve SERIAL token-based tasks.
//
// Usage Example:
//    static void Func(int n) { ... }
//    class Task : public Runnable { ... }
//
//    gscoped_ptr<ThreadPool> thread_pool;
//    CHECK_OK(
//        ThreadPoolBuilder("my_pool")
//            .set_min_threads(0)
//            .set_max_threads(5)
//            .set_max_queue_size(10)
//            .set_timeout(MonoDelta::FromMilliseconds(2000))
//            .Build(&thread_pool));
//    thread_pool->Submit(shared_ptr<Runnable>(new Task()));
//    thread_pool->Submit(std::bind(&Func, 10));
class ThreadPool {
 public:
  ~ThreadPool();

  // Wait for the running tasks to complete and then shutdown the threads.
  // All the other pending tasks in the queue will be removed.
  // NOTE: That the user may implement an external abort logic for the
  //       runnables, that must be called before Shutdown(), if the system
  //       should know about the non-execution of these tasks, or the runnable
  //       require an explicit "abort" notification to exit from the run loop.
  void Shutdown();

  // Submit a function using the yb Closure system.
  CHECKED_STATUS SubmitClosure(const Closure& task);

  // Submit a function binded using std::bind(&FuncName, args...)
  CHECKED_STATUS SubmitFunc(const std::function<void()>& func);
  CHECKED_STATUS SubmitFunc(std::function<void()>&& func);

  CHECKED_STATUS SubmitFunc(std::function<void()>& func) { // NOLINT
    const auto& const_func = func;
    return SubmitFunc(const_func);
  }

  template <class F>
  CHECKED_STATUS SubmitFunc(F&& f) {
    return Submit(std::make_shared<RunnableImpl<F>>(std::move(f)));
  }

  template <class F>
  CHECKED_STATUS SubmitFunc(const F& f) {
    return Submit(std::make_shared<RunnableImpl<F>>(f));
  }

  template <class F>
  CHECKED_STATUS SubmitFunc(F& f) { // NOLINT
    const auto& const_f = f;
    return SubmitFunc(const_f);
  }

  // Submit a Runnable class
  CHECKED_STATUS Submit(const std::shared_ptr<Runnable>& task);

  // Wait until all the tasks are completed.
  void Wait();

  // Waits for the pool to reach the idle state, or until 'until' time is reached.
  // Returns true if the pool reached the idle state, false otherwise.
  bool WaitUntil(const MonoTime& until);

  // Waits for the pool to reach the idle state, or until 'delta' time elapses.
  // Returns true if the pool reached the idle state, false otherwise.
  bool WaitFor(const MonoDelta& delta);

  // Allocates a new token for use in token-based task submission. All tokens
  // must be destroyed before their ThreadPool is destroyed.
  //
  // There is no limit on the number of tokens that may be allocated.
  enum class ExecutionMode {
    // Tasks submitted via this token will be executed serially.
    SERIAL,

    // Tasks submitted via this token may be executed concurrently.
    CONCURRENT,
  };
  std::unique_ptr<ThreadPoolToken> NewToken(ExecutionMode mode);

  // Like NewToken(), but lets the caller provide metrics for the token. These
  // metrics are incremented/decremented in addition to the configured
  // pool-wide metrics (if any).
  std::unique_ptr<ThreadPoolToken> NewTokenWithMetrics(ExecutionMode mode,
                                                       ThreadPoolMetrics metrics);

 private:
  friend class ThreadPoolBuilder;
  friend class ThreadPoolToken;


  // Create a new thread pool using a builder.
  explicit ThreadPool(const ThreadPoolBuilder& builder);

  // Initialize the thread pool by starting the minimum number of threads.
  CHECKED_STATUS Init();

  // Dispatcher responsible for dequeueing and executing the tasks
  void DispatchThread(bool permanent);

  // Create new thread. Required that lock_ is held.
  CHECKED_STATUS CreateThreadUnlocked();

 private:
  FRIEND_TEST(TestThreadPool, TestThreadPoolWithNoMinimum);
  FRIEND_TEST(TestThreadPool, TestThreadPoolWithNoMaxThreads);
  FRIEND_TEST(TestThreadPool, TestVariableSizeThreadPool);
  // Aborts if the current thread is a member of this thread pool.
  void CheckNotPoolThreadUnlocked();

  struct Task {
    std::shared_ptr<Runnable> runnable;
    Trace* trace;

    // Time at which the entry was submitted to the pool.
    MonoTime submit_time;
  };
  // Submits a task to be run via token.
  Status DoSubmit(std::shared_ptr<Runnable> r, ThreadPoolToken* token);

  // Releases token 't' and invalidates it.
  void ReleaseToken(ThreadPoolToken* t);

  const std::string name_;
  const int min_threads_;
  const int max_threads_;
  const int max_queue_size_;
  const MonoDelta idle_timeout_;

  Status pool_status_;
  Mutex lock_;
  ConditionVariable idle_cond_;
  ConditionVariable no_threads_cond_;
  ConditionVariable not_empty_;
  int num_threads_;
  int active_threads_;

  // Total number of client tasks queued, either directly (queue_) or
  // indirectly (tokens_).
  // Protected by lock_.
  int total_queued_tasks_;

  // All allocated tokens.
  // Tokens are owned by the clients.
  //
  // Protected by lock_.
  std::unordered_set<ThreadPoolToken*> tokens_;

  // FIFO of tokens from which tasks should be executed. Does not own the
  // tokens; they are owned by clients and are removed from the FIFO on shutdown.
  //
  // Protected by lock_.
  std::deque<ThreadPoolToken*> queue_;

  // Pointers to all running threads. Raw pointers are safe because a Thread
  // may only go out of scope after being removed from threads_.
  //
  // Protected by lock_.
  std::unordered_set<Thread*> threads_;

  // ExecutionMode::CONCURRENT token used by the pool for tokenless submission.
  std::unique_ptr<ThreadPoolToken> tokenless_;

  // Metrics for the entire thread pool.
  const ThreadPoolMetrics metrics_;

  DISALLOW_COPY_AND_ASSIGN(ThreadPool);
};

// All possible token states. Legal state transitions:
//   kIdle      -> kRunning: task is submitted via token
//   kIdle      -> kQuiesced: token or pool is shut down
//   kRunning   -> kIdle: worker thread finishes executing a task and
//                      there are no more tasks queued to the token
//   kRunning   -> kQuiescing: token or pool is shut down while worker thread
//                           is executing a task
//   kRunning   -> kQuiesced: token or pool is shut down
//   kQuiescing -> kQuiesced:  worker thread finishes executing a task
//                           belonging to a shut down token or pool
YB_DEFINE_ENUM(ThreadPoolTokenState,
                   // Token has no queued tasks.
                   (kIdle)

                   // A worker thread is running one of the token's previously queued tasks.
                   (kRunning)

                   // No new tasks may be submitted to the token. A worker thread is still
                   // running a previously queued task.
                   (kQuiescing)

                   // No new tasks may be submitted to the token. There are no active tasks
                   // either. At this state, the token may only be destroyed.
                   (kQuiesced));

// Entry point for token-based task submission and blocking for a particular
// thread pool. Tokens can only be created via ThreadPool::NewToken().
//
// All functions are thread-safe. Mutable members are protected via the
// ThreadPool's lock.
class ThreadPoolToken {
 public:
  // Destroys the token.
  //
  // May be called on a token with outstanding tasks, as Shutdown() will be
  // called first to take care of them.
  ~ThreadPoolToken();

  // Submits a function using the yb Closure system.
  Status SubmitClosure(Closure c) WARN_UNUSED_RESULT;

  // Submits a function bound using boost::bind(&FuncName, args...).
  Status SubmitFunc(const std::function<void()> f) WARN_UNUSED_RESULT;

  // Submits a Runnable class.
  Status Submit(std::shared_ptr<Runnable> r) WARN_UNUSED_RESULT;

  // Marks the token as unusable for future submissions. Any queued tasks not
  // yet running are destroyed. If tasks are in flight, Shutdown() will wait
  // on their completion before returning.
  void Shutdown();

  // Waits until all the tasks submitted via this token are completed.
  void Wait();

  // Waits for all submissions using this token are complete, or until 'until'
  // time is reached.
  //
  // Returns true if all submissions are complete, false otherwise.
  bool WaitUntil(const MonoTime& until);

  // Waits for all submissions using this token are complete, or until 'delta'
  // time elapses.
  //
  // Returns true if all submissions are complete, false otherwise.
  bool WaitFor(const MonoDelta& delta);

 private:
  friend class ThreadPool;

  // Returns a textual representation of 's' suitable for debugging.
  static const char* StateToString(ThreadPoolTokenState s);

  // Constructs a new token.
  //
  // The token may not outlive its thread pool ('pool').
  ThreadPoolToken(ThreadPool* pool, ThreadPool::ExecutionMode mode, ThreadPoolMetrics metrics);

  // Changes this token's state to 'new_state' taking actions as needed.
  void Transition(ThreadPoolTokenState new_state);

  // Returns true if this token has a task queued and ready to run, or if a
  // task belonging to this token is already running.
  bool IsActive() const {
    return state_ == ThreadPoolTokenState::kRunning ||
           state_ == ThreadPoolTokenState::kQuiescing;
  }

  // Returns true if new tasks may be submitted to this token.
  bool MaySubmitNewTasks() const {
    return state_ != ThreadPoolTokenState::kQuiescing &&
           state_ != ThreadPoolTokenState::kQuiesced;
  }

  ThreadPoolTokenState state() const { return state_; }
  ThreadPool::ExecutionMode mode() const { return mode_; }

  // Token's configured execution mode.
  const ThreadPool::ExecutionMode mode_;

  // Pointer to the token's thread pool.
  ThreadPool* pool_;

  // Metrics for just this token.
  const ThreadPoolMetrics metrics_;

  // Token state machine.
  ThreadPoolTokenState state_;

  // Queued client tasks.
  std::deque<ThreadPool::Task> entries_;

  // Condition variable for "token is idle". Waiters wake up when the token
  // transitions to kIdle or kQuiesced.
  ConditionVariable not_running_cond_;

  // Number of worker threads currently executing tasks belonging to this
  // token.
  int active_threads_;

  DISALLOW_COPY_AND_ASSIGN(ThreadPoolToken);
};

////////////////////////////////////////////////////////
// FunctionRunnable
////////////////////////////////////////////////////////

class FunctionRunnable : public Runnable {
 public:
  explicit FunctionRunnable(std::function<void()> func) : func_(std::move(func)) {}

  void Run() override {
    func_();
  }

 private:
  std::function<void()> func_;
};

} // namespace yb
#endif // YB_UTIL_THREADPOOL_H
