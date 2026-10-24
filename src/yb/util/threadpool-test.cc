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
// The following only applies to changes made to this file as part of YugabyteDB development.
//
// Portions Copyright (c) YugabyteDB, Inc.
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

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "yb/util/logging.h"
#include <gtest/gtest.h>

#include "yb/gutil/atomicops.h"
#include "yb/gutil/bind.h"
#include "yb/gutil/sysinfo.h"

#include "yb/util/barrier.h"
#include "yb/util/cgroups.h"
#include "yb/util/countdown_latch.h"
#include "yb/util/format.h"
#include "yb/util/locks.h"
#include "yb/util/metrics.h"
#include "yb/util/promise.h"
#include "yb/util/random.h"
#include "yb/util/scope_exit.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"

using std::atomic;
using std::shared_ptr;
using std::string;
using std::thread;
using std::unique_ptr;
using std::vector;

DECLARE_bool(enable_tracing);
DECLARE_bool(threadpool_use_current_trace_for_tasks);

using std::shared_ptr;

namespace yb {

namespace {

static Status BuildMinMaxTestPool(
    int min_threads, int max_threads, std::unique_ptr<ThreadPool>* pool) {
  return ThreadPoolBuilder("test").set_min_threads(min_threads)
                                  .set_max_threads(max_threads)
                                  .Build(pool);
}

} // anonymous namespace

class TestThreadPool : public ::testing::Test {
 public:
  TestThreadPool() {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_tracing) = true;
  }
};

TEST_F(TestThreadPool, TestNoTaskOpenClose) {
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(BuildMinMaxTestPool(4, 4, &thread_pool));
  thread_pool->Shutdown();
}

static void SimpleTaskMethod(int n, Atomic32 *counter) {
  while (n--) {
    base::subtle::NoBarrier_AtomicIncrement(counter, 1);
    boost::detail::yield(n);
  }
}

class SimpleTask : public Runnable {
 public:
  SimpleTask(int n, Atomic32 *counter)
    : n_(n), counter_(counter) {
  }

  void Run() override {
    SimpleTaskMethod(n_, counter_);
  }

 private:
  int n_;
  Atomic32 *counter_;
};

TEST_F(TestThreadPool, TestSimpleTasks) {
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(BuildMinMaxTestPool(4, 4, &thread_pool));

  Atomic32 counter(0);
  std::shared_ptr<Runnable> task(new SimpleTask(15, &counter));

  ASSERT_OK(thread_pool->SubmitFunc(std::bind(&SimpleTaskMethod, 10, &counter)));
  ASSERT_OK(thread_pool->Submit(task));
  ASSERT_OK(thread_pool->SubmitFunc(std::bind(&SimpleTaskMethod, 20, &counter)));
  ASSERT_OK(thread_pool->Submit(task));
  ASSERT_OK(thread_pool->SubmitClosure(Bind(&SimpleTaskMethod, 123, &counter)));
  thread_pool->Wait();
  ASSERT_EQ(10 + 15 + 20 + 15 + 123, base::subtle::NoBarrier_Load(&counter));
  thread_pool->Shutdown();
}

static void IssueTraceStatement() {
  TRACE("hello from task");
}

// Test that the thread-local trace is propagated to tasks
// submitted to the threadpool.
TEST_F(TestThreadPool, TestTracePropagation) {
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(BuildMinMaxTestPool(1, 1, &thread_pool));
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_threadpool_use_current_trace_for_tasks) = true;

  scoped_refptr<Trace> t(new Trace);
  {
    ADOPT_TRACE(t.get());
    ASSERT_OK(thread_pool->SubmitFunc(&IssueTraceStatement));
  }
  thread_pool->Wait();
  ASSERT_STR_CONTAINS(t->DumpToString(true), "hello from task");
}

TEST_F(TestThreadPool, TestSubmitAfterShutdown) {
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(BuildMinMaxTestPool(1, 1, &thread_pool));
  thread_pool->Shutdown();
  Status s = thread_pool->SubmitFunc(&IssueTraceStatement);
  ASSERT_EQ("Shutdown in progress: The pool has been shut down.",
            s.ToString(/* no file/line */ false));
}

class SlowTask : public Runnable {
 public:
  explicit SlowTask(CountDownLatch* latch)
    : latch_(latch) {
  }

  void Run() override {
    latch_->Wait();
  }

  static shared_ptr<Runnable> NewSlowTask(CountDownLatch* latch) {
    return std::make_shared<SlowTask>(latch);
  }
 private:
  CountDownLatch* latch_;
};

TEST_F(TestThreadPool, TestThreadPoolWithNoMinimum) {
  MonoDelta idle_timeout = MonoDelta::FromMilliseconds(1);
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
      .set_min_threads(0).set_max_threads(3)
      .set_idle_timeout(idle_timeout).Build(&thread_pool));
  // There are no threads to start with.
  ASSERT_EQ(0, thread_pool->NumWorkers());
  // We get up to 3 threads when submitting work.
  CountDownLatch latch(1);
  auto se = ScopeExit([&latch] {
      latch.CountDown();
  });
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(2, thread_pool->NumWorkers());
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(3, thread_pool->NumWorkers());
  // The 4th piece of work gets queued.
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(3, thread_pool->NumWorkers());
  // Finish all work
  latch.CountDown();
  thread_pool->Wait();
  ASSERT_TRUE(thread_pool->Idle());
  thread_pool->Shutdown();
  ASSERT_EQ(0, thread_pool->NumWorkers());
}

TEST_F(TestThreadPool, TestThreadPoolWithNoMaxThreads) {
  // By default a threadpool's max_threads is set to the number of CPUs, so
  // this test submits more tasks than that to ensure that the number of CPUs
  // isn't some kind of upper bound.
  const int kNumCPUs = base::NumCPUs();
  std::unique_ptr<ThreadPool> thread_pool;

  // Build a threadpool with no limit on the maximum number of threads.
  ASSERT_OK(ThreadPoolBuilder("test")
                .set_max_threads(std::numeric_limits<int>::max())
                .Build(&thread_pool));
  CountDownLatch latch(1);
  auto se = ScopeExit([&latch] {
    latch.CountDown();
    });

  // Submit tokenless tasks. Each should create a new thread.
  for (int i = 0; i < kNumCPUs * 2; i++) {
    ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  }
  ASSERT_EQ(kNumCPUs * 2, thread_pool->NumWorkers());

  // Submit tasks on two tokens. Only two threads should be created.
  unique_ptr<ThreadPoolToken> t1 = thread_pool->NewToken(ThreadPool::ExecutionMode::SERIAL);
  unique_ptr<ThreadPoolToken> t2 = thread_pool->NewToken(ThreadPool::ExecutionMode::SERIAL);
  for (int i = 0; i < kNumCPUs * 2; i++) {
    ThreadPoolToken* t = (i % 2 == 0) ? t1.get() : t2.get();
    ASSERT_OK(t->Submit(SlowTask::NewSlowTask(&latch)));
  }
  ASSERT_EQ((kNumCPUs * 2) + 2, thread_pool->NumWorkers());

  // Submit more tokenless tasks. Each should create a new thread.
  for (int i = 0; i < kNumCPUs; i++) {
    ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  }
  ASSERT_EQ((kNumCPUs * 3) + 2, thread_pool->NumWorkers());
  latch.CountDown();
  thread_pool->Wait();
  thread_pool->Shutdown();
}

// Regression test for a bug where a task is submitted exactly
// as a thread is about to exit. Previously this could hang forever.
TEST_F(TestThreadPool, TestRace) {
  alarm(10);
  MonoDelta idle_timeout = MonoDelta::FromMicroseconds(1);
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
      .set_min_threads(0).set_max_threads(1)
      .set_idle_timeout(idle_timeout).Build(&thread_pool));

  for (int i = 0; i < 500; i++) {
    CountDownLatch l(1);
    ASSERT_OK(thread_pool->SubmitFunc([&l]() { l.CountDown(); }));
    l.Wait();
    // Sleeping a different amount in each iteration makes it more likely to hit
    // the bug.
    SleepFor(MonoDelta::FromMicroseconds(i));
  }
}

TEST_F(TestThreadPool, TestVariableSizeThreadPool) {
  MonoDelta idle_timeout = MonoDelta::FromMilliseconds(1);
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
      .set_min_threads(1).set_max_threads(4)
      .set_idle_timeout(idle_timeout).Build(&thread_pool));
  // There is 1 thread to start with.
  ASSERT_EQ(1, thread_pool->NumWorkers());
  thread_pool->Wait(); // Wait until all workers started.
  // We get up to 4 threads when submitting work.
  CountDownLatch latch(1);
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(1, thread_pool->NumWorkers());
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(2, thread_pool->NumWorkers());
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(3, thread_pool->NumWorkers());
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(4, thread_pool->NumWorkers());
  // Additional work gets queued without spawning more threads.
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(4, thread_pool->NumWorkers());
  ASSERT_OK(thread_pool->Submit(SlowTask::NewSlowTask(&latch)));
  ASSERT_EQ(4, thread_pool->NumWorkers());
  // Finish all work
  latch.CountDown();
  thread_pool->Wait();
  ASSERT_TRUE(thread_pool->Idle());
  thread_pool->Shutdown();
  ASSERT_EQ(0, thread_pool->NumWorkers());
}

// Test that setting a promise from another thread yields
// a value on the current thread.
TEST_F(TestThreadPool, TestPromises) {
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
      .set_min_threads(1).set_max_threads(1)
      .Build(&thread_pool));

  Promise<int> my_promise;
  ASSERT_OK(thread_pool->SubmitClosure(
                     Bind(&Promise<int>::Set, Unretained(&my_promise), 5)));
  ASSERT_EQ(5, my_promise.Get());
  thread_pool->Shutdown();
}


METRIC_DEFINE_entity(test_entity);
METRIC_DEFINE_event_stats(test_entity, queue_length, "queue length",
                        MetricUnit::kTasks, "queue length");

METRIC_DEFINE_event_stats(test_entity, queue_time, "queue time",
                        MetricUnit::kMicroseconds, "queue time");

METRIC_DEFINE_event_stats(test_entity, run_time, "run time",
                        MetricUnit::kMicroseconds, "run time");

TEST_F(TestThreadPool, TestMetrics) {
  MetricRegistry registry;
  auto entity = METRIC_ENTITY_test_entity.Instantiate(&registry, "test");
  ThreadPoolMetrics metrics{
      METRIC_queue_length.Instantiate(entity),
      METRIC_queue_time.Instantiate(entity),
      METRIC_run_time.Instantiate(entity)
  };

  // Enable metrics for the thread pool.
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
            .set_min_threads(1).set_max_threads(1)
            .set_metrics(metrics)
            .Build(&thread_pool));

  ASSERT_OK(thread_pool->SubmitFunc([](){}));
  ASSERT_OK(thread_pool->SubmitFunc([](){}));
  ASSERT_OK(thread_pool->SubmitFunc([](){}));
  thread_pool->Wait();

  // And the counts on the pool-wide metrics should reflect all submissions.
  ASSERT_EQ(3, metrics.queue_length_stats->TotalCount());
  ASSERT_EQ(3, metrics.queue_time_us_stats->TotalCount());
  ASSERT_EQ(3, metrics.run_time_us_stats->TotalCount());
}

// For test cases that should run with both kinds of tokens.
class TestThreadPoolTokenTypes : public TestThreadPool,
                                 public testing::WithParamInterface<ThreadPool::ExecutionMode> {};

INSTANTIATE_TEST_CASE_P(Tokens, TestThreadPoolTokenTypes,
                        ::testing::Values(ThreadPool::ExecutionMode::SERIAL,
                                          ThreadPool::ExecutionMode::CONCURRENT));


TEST_P(TestThreadPoolTokenTypes, TestTokenSubmitAndWait) {
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
                .Build(&thread_pool));
  unique_ptr<ThreadPoolToken> t = thread_pool->NewToken(GetParam());
  int i = 0;
  CountDownLatch latch(1);
  ASSERT_OK(t->SubmitFunc([&]() {
    SleepFor(MonoDelta::FromMilliseconds(1));
    i++;
    latch.CountDown();
  }));
  latch.Wait();
  ASSERT_EQ(1, i);
}

TEST_F(TestThreadPool, TestTokenSubmitsProcessedSerially) {
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
                .Build(&thread_pool));
  unique_ptr<ThreadPoolToken> t = thread_pool->NewToken(ThreadPool::ExecutionMode::SERIAL);
  Random r(SeedRandom());
  string result;
  CountDownLatch latch(5);
  for (char c = 'a'; c < 'f'; c++) {
    // Sleep a little first so that there's a higher chance of out-of-order
    // appends if the submissions did execute in parallel.
    int sleep_ms = r.Next() % 5;
    ASSERT_OK(t->SubmitFunc([&result, &latch, c, sleep_ms]() {
      SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
      result += c;
      latch.CountDown();
    }));
  }
  latch.Wait();
  ASSERT_EQ("abcde", result);
}

TEST_P(TestThreadPoolTokenTypes, TestTokenSubmitsProcessedConcurrently) {
  const int kNumTokens = 5;
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
                .set_max_threads(kNumTokens)
                .Build(&thread_pool));
  vector<unique_ptr<ThreadPoolToken>> tokens;

  // A violation to the tested invariant would yield a deadlock, so let's set
  // up an alarm to bail us out.
  alarm(60);
  auto se = ScopeExit([] {
    alarm(0); // Disable alarm on test exit.
  });
  shared_ptr<Barrier> b = std::make_shared<Barrier>(kNumTokens + 1);
  for (int i = 0; i < kNumTokens; i++) {
    tokens.emplace_back(thread_pool->NewToken(GetParam()));
    ASSERT_OK(tokens.back()->SubmitFunc([b]() {
      b->Wait();
    }));
  }

  // This will deadlock if the above tasks weren't all running concurrently.
  b->Wait();
}

TEST_F(TestThreadPool, TestTokenSubmitsNonSequential) {
  const int kNumSubmissions = 5;
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
                .set_max_threads(kNumSubmissions)
                .Build(&thread_pool));

  // A violation to the tested invariant would yield a deadlock, so let's set
  // up an alarm to bail us out.
  alarm(60);
  auto se = ScopeExit([] {
    alarm(0); // Disable alarm on test exit.
  });
  shared_ptr<Barrier> b = std::make_shared<Barrier>(kNumSubmissions + 1);
  unique_ptr<ThreadPoolToken> t = thread_pool->NewToken(ThreadPool::ExecutionMode::CONCURRENT);
  for (int i = 0; i < kNumSubmissions; i++) {
    ASSERT_OK(t->SubmitFunc([b]() {
      b->Wait();
    }));
  }

  // This will deadlock if the above tasks weren't all running concurrently.
  b->Wait();
}

TEST_P(TestThreadPoolTokenTypes, TestTokenShutdown) {
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
                .set_max_threads(4)
                .Build(&thread_pool));

  unique_ptr<ThreadPoolToken> t1(thread_pool->NewToken(GetParam()));
  unique_ptr<ThreadPoolToken> t2(thread_pool->NewToken(GetParam()));
  CountDownLatch l1(1);
  CountDownLatch l2(1);

  // A violation to the tested invariant would yield a deadlock, so let's set
  // up an alarm to bail us out.
  alarm(60);
  auto se = ScopeExit([] {
    alarm(0); // Disable alarm on test exit.
  });

  for (int i = 0; i < 3; i++) {
    ASSERT_OK(t1->SubmitFunc([&]() {
      l1.Wait();
    }));
  }
  for (int i = 0; i < 3; i++) {
    ASSERT_OK(t2->SubmitFunc([&]() {
      l2.Wait();
    }));
  }

  // Unblock all of t1's tasks, but not t2's tasks.
  l1.CountDown();

  // If this also waited for t2's tasks, it would deadlock.
  t1->Shutdown();

  // We can no longer submit to t1 but we can still submit to t2.
  ASSERT_TRUE(t1->SubmitFunc([](){}).IsShutdownInProgress());
  ASSERT_OK(t2->SubmitFunc([](){}));

  // Unblock t2's tasks.
  l2.CountDown();
  t2->Shutdown();
}

TEST_P(TestThreadPoolTokenTypes, TestTokenWaitForAll) {
  const int kNumTokens = 3;
  const int kNumSubmissions = 20;
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
                .Build(&thread_pool));
  Random r(SeedRandom());
  vector<unique_ptr<ThreadPoolToken>> tokens;
  for (int i = 0; i < kNumTokens; i++) {
    tokens.emplace_back(thread_pool->NewToken(GetParam()));
  }

  atomic<int32_t> v(0);
  for (int i = 0; i < kNumSubmissions; i++) {
    // Sleep a little first to raise the likelihood of the test thread
    // reaching Wait() before the submissions finish.
    int sleep_ms = r.Next() % 5;

    auto task = [&v, sleep_ms]() {
      SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
      v++;
    };

    // Half of the submissions will be token-less, and half will use a token.
    if (i % 2 == 0) {
      ASSERT_OK(thread_pool->SubmitFunc(task));
    } else {
      int token_idx = r.Next() % tokens.size();
      ASSERT_OK(tokens[token_idx]->SubmitFunc(task));
    }
  }
  thread_pool->Wait();
  ASSERT_EQ(kNumSubmissions, v);
}

TEST_F(TestThreadPool, TestFuzz) {
  const int kNumOperations = 1000;
  Random r(SeedRandom());
  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
                .Build(&thread_pool));
  vector<unique_ptr<ThreadPoolToken>> tokens;

  for (int i = 0; i < kNumOperations; i++) {
    // Operation distribution:
    //
    // - Submit without a token: 40%
    // - Submit with a randomly selected token: 35%
    // - Allocate a new token: 10%
    // - Wait on a randomly selected token: 7%
    // - Shutdown a randomly selected token: 4%
    // - Deallocate a randomly selected token: 2%
    // - Wait for all submissions: 2%
    int op = r.Next() % 100;
    if (op < 40) {
      // Submit without a token.
      int sleep_ms = r.Next() % 5;
      ASSERT_OK(thread_pool->SubmitFunc([sleep_ms]() {
        // Sleep a little first to increase task overlap.
        SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
      }));
    } else if (op < 75) {
      // Submit with a randomly selected token.
      if (tokens.empty()) {
        continue;
      }
      int sleep_ms = r.Next() % 5;
      int token_idx = r.Next() % tokens.size();
      Status s = tokens[token_idx]->SubmitFunc([sleep_ms]() {
        // Sleep a little first to increase task overlap.
        SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
      });
      ASSERT_TRUE(s.ok() || s.IsShutdownInProgress()) << s;
    } else if (op < 85) {
      // Allocate a token with a randomly selected policy.
      ThreadPool::ExecutionMode mode = r.Next() % 2 ?
          ThreadPool::ExecutionMode::SERIAL :
          ThreadPool::ExecutionMode::CONCURRENT;
      tokens.emplace_back(thread_pool->NewToken(mode));
    } else if (op < 92) {
      // Wait on a randomly selected token.
      if (tokens.empty()) {
        continue;
      }
      // int token_idx = r.Next() % tokens.size();
      // tokens[token_idx]->Wait();
    } else if (op < 96) {
      // Shutdown a randomly selected token.
      if (tokens.empty()) {
        continue;
      }
      int token_idx = r.Next() % tokens.size();
      tokens[token_idx]->Shutdown();
    } else if (op < 98) {
      // Deallocate a randomly selected token.
      if (tokens.empty()) {
        continue;
      }
      auto it = tokens.begin();
      int token_idx = r.Next() % tokens.size();
      std::advance(it, token_idx);
      tokens.erase(it);
    } else {
      // Wait on everything.
      ASSERT_LT(op, 100);
      ASSERT_GE(op, 98);
      thread_pool->Wait();
    }
  }

  // Some test runs will shut down the pool before the tokens, and some won't.
  // Either way should be safe.
  if (r.Next() % 2 == 0) {
    thread_pool->Shutdown();
  }
}

TEST_F(TestThreadPool, TestTokenConcurrency) {
  const int kNumTokens = 20;
  const int kTestRuntimeSecs = 1;
  const int kCycleThreads = 2;
  const int kShutdownThreads = 2;
  const int kSubmitThreads = 8;

  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test")
                .Build(&thread_pool));
  vector<shared_ptr<ThreadPoolToken>> tokens;
  Random rng(SeedRandom());

  // Protects 'tokens' and 'rng'.
  simple_spinlock lock;

  // Fetch a token from 'tokens' at random.
  auto GetRandomToken = [&]() -> shared_ptr<ThreadPoolToken> {
    std::lock_guard l(lock);
    int idx = rng.Uniform(kNumTokens);
    return tokens[idx];
  };

  // Preallocate all of the tokens.
  for (int i = 0; i < kNumTokens; i++) {
    ThreadPool::ExecutionMode mode;
    {
      std::lock_guard l(lock);
      mode = rng.Next() % 2 ?
          ThreadPool::ExecutionMode::SERIAL :
          ThreadPool::ExecutionMode::CONCURRENT;
    }
    tokens.emplace_back(thread_pool->NewToken(mode).release());
  }

  atomic<int64_t> total_num_tokens_cycled(0);
  atomic<int64_t> total_num_tokens_shutdown(0);
  atomic<int64_t> total_num_tokens_submitted(0);

  CountDownLatch latch(1);
  vector<thread> threads;

  for (int i = 0; i < kCycleThreads; i++) {
    // Pick a token at random and replace it.
    //
    // The replaced token is only destroyed when the last ref is dropped,
    // possibly by another thread.
    threads.emplace_back([&]() {
      int num_tokens_cycled = 0;
      while (latch.count()) {
        {
          std::lock_guard l(lock);
          int idx = rng.Uniform(kNumTokens);
          ThreadPool::ExecutionMode mode = rng.Next() % 2 ?
              ThreadPool::ExecutionMode::SERIAL :
              ThreadPool::ExecutionMode::CONCURRENT;
          tokens[idx] = shared_ptr<ThreadPoolToken>(thread_pool->NewToken(mode).release());
        }
        num_tokens_cycled++;

        // Sleep a bit, otherwise this thread outpaces the other threads and
        // nothing interesting happens to most tokens.
        SleepFor(MonoDelta::FromMicroseconds(10));
      }
      total_num_tokens_cycled += num_tokens_cycled;
    });
  }

  for (int i = 0; i < kShutdownThreads; i++) {
    // Pick a token at random and shut it down. Submitting a task to a shut
    // down token will return a ServiceUnavailable error.
    threads.emplace_back([&]() {
      int num_tokens_shutdown = 0;
      while (latch.count()) {
        GetRandomToken()->Shutdown();
        num_tokens_shutdown++;
      }
      total_num_tokens_shutdown += num_tokens_shutdown;
    });
  }

  for (int i = 0; i < kSubmitThreads; i++) {
    // Pick a token at random and submit a task to it.
    threads.emplace_back([&]() {
      int num_tokens_submitted = 0;
      Random rng(SeedRandom());
      while (latch.count()) {
        int sleep_ms = rng.Next() % 5;
        Status s = GetRandomToken()->SubmitFunc([sleep_ms]() {
          // Sleep a little first so that tasks are running during other events.
          SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
        });
        ASSERT_TRUE(s.ok() || s.IsShutdownInProgress());
        num_tokens_submitted++;
      }
      total_num_tokens_submitted += num_tokens_submitted;
    });
  }

  SleepFor(MonoDelta::FromSeconds(kTestRuntimeSecs));
  latch.CountDown();
  for (auto& t : threads) {
    t.join();
  }

  LOG(INFO) << Format("Tokens cycled ($0 threads): $1",
                      kCycleThreads, total_num_tokens_cycled.load());
  LOG(INFO) << Format("Tokens shutdown ($0 threads): $1",
                      kShutdownThreads, total_num_tokens_shutdown.load());
  LOG(INFO) << Format("Tokens submitted ($0 threads): $1",
                      kSubmitThreads, total_num_tokens_submitted.load());
}

// Repeated Shutdown() on a token of either execution mode stays idempotent, and the token keeps
// rejecting submissions after every call.
TEST_F(TestThreadPool, TestRepeatedTokenShutdown) {
  const int kNumShutdowns = 40000;

  std::unique_ptr<ThreadPool> thread_pool;
  ASSERT_OK(ThreadPoolBuilder("test").Build(&thread_pool));

  for (auto mode : {ThreadPool::ExecutionMode::SERIAL, ThreadPool::ExecutionMode::CONCURRENT}) {
    auto token = thread_pool->NewToken(mode);
    for (int i = 0; i < kNumShutdowns; i++) {
      token->Shutdown();
      ASSERT_TRUE(token->SubmitFunc([]() {}).IsShutdownInProgress());
    }
  }
}

TEST_F(TestThreadPool, TestTaskRunnerStopWait) {
  TaskRunner runner;
  ASSERT_OK(runner.Init(/* concurrency = */1));

  std::atomic<int> val = 0;
  runner.Submit([&val]() -> Status {
    val++;
    return STATUS(IllegalState, "first task failed");
  });
  runner.Submit([&val]() -> Status {
    SleepFor(MonoDelta::FromSeconds(1));
    val++;
    return Status::OK();
  });

  ASSERT_NOK(runner.Wait(StopWaitIfFailed::kTrue));
  ASSERT_NOK(runner.status());
  ASSERT_EQ(1, val);

  // Wait for second task to finish.
  ASSERT_NOK(runner.Wait(StopWaitIfFailed::kFalse));
  ASSERT_EQ(2, val);
}

#ifdef __linux__
TEST_F(TestThreadPool, TestCgroupTaggedPools) {
  constexpr size_t kTotalTasks = 100;
  constexpr size_t kTotalPools = 13;

  ASSERT_OK(SetupCgroupManagement(ClearChildCgroups::kTrue));
  Cgroup* root_cgroup = RootCgroup();
  std::vector<Cgroup*> cgroups;
  for (size_t i = 0; i < kTotalPools + 1; ++i) {
    cgroups.push_back(&ASSERT_RESULT_REF(root_cgroup->CreateOrLoadChild(Format("test-$0", i))));
  }

  TaggedThreadPools tagged_pools{[&](TaggedThreadPools::Tag tag) {
    return ThreadPoolOptions {
      .name = Format("test-pool-$0", tag),
      .max_workers = 100,
      .cgroup = cgroups[tag],
    };
  }};

  for (size_t i = 0; i < kTotalTasks; ++i) {
    TaggedThreadPools::Tag tag = i % kTotalPools;
    ASSERT_OK(ASSERT_RESULT(tagged_pools.Pool(tag)).SubmitFunc([tag, &cgroups] {
      auto active_cgroup = ASSERT_RESULT(GetThreadCpuCgroup());
      ASSERT_EQ(active_cgroup, cgroups[tag]->full_name());
    }));
  }
  ASSERT_EQ(tagged_pools.ActivePools(), kTotalPools);
}
#endif

} // namespace yb
