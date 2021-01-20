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

#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "yb/gutil/integral_types.h"
#include "yb/util/atomic.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/rw_mutex.h"
#include "yb/util/test_util.h"

using std::lock_guard;
using std::thread;
using std::try_to_lock;
using std::unique_lock;
using std::vector;

namespace yb {

class RWMutexTest : public YBTest,
                    public ::testing::WithParamInterface<RWMutex::Priority> {
 public:
  RWMutexTest()
     : lock_(GetParam()) {
  }
 protected:
  RWMutex lock_;
};

// Instantiate every test for each kind of RWMutex priority.
INSTANTIATE_TEST_CASE_P(Priorities, RWMutexTest,
                        ::testing::Values(RWMutex::Priority::PREFER_READING,
                                          RWMutex::Priority::PREFER_WRITING));

// Multi-threaded test that tries to find deadlocks in the RWMutex wrapper.
TEST_P(RWMutexTest, TestDeadlocks) NO_THREAD_SAFETY_ANALYSIS {
  uint64_t number_of_writes = 0;
  AtomicInt<uint64_t> number_of_reads(0);

  AtomicBool done(false);
  vector<thread> threads;

  // Start several blocking and non-blocking read-write workloads.
  for (int i = 0; i < 2; i++) {
    threads.emplace_back([&](){
      while (!done.Load()) {
        lock_guard<RWMutex> l(lock_);
        number_of_writes++;
      }
    });
    threads.emplace_back([&](){
      while (!done.Load()) {
        unique_lock<RWMutex> l(lock_, try_to_lock);
        if (l.owns_lock()) {
          number_of_writes++;
        }
      }
    });
  }

  // Start several blocking and non-blocking read-only workloads.
  for (int i = 0; i < 2; i++) {
    threads.emplace_back([&](){
      while (!done.Load()) {
        shared_lock<RWMutex> l(lock_);
        number_of_reads.Increment();
      }
    });
    threads.emplace_back([&](){
      while (!done.Load()) {
        shared_lock<RWMutex> l(lock_, try_to_lock);
        if (l.owns_lock()) {
          number_of_reads.Increment();
        }
      }
    });
  }

  SleepFor(MonoDelta::FromSeconds(1));
  done.Store(true);
  for (auto& t : threads) {
    t.join();
  }

  shared_lock<RWMutex> l(lock_);
  LOG(INFO) << "Number of writes: " << number_of_writes;
  LOG(INFO) << "Number of reads: " << number_of_reads.Load();
}

#ifndef NDEBUG
// Tests that the RWMutex wrapper catches basic usage errors. This checking is
// only enabled in debug builds.
TEST_P(RWMutexTest, TestLockChecking) NO_THREAD_SAFETY_ANALYSIS {
  EXPECT_DEATH({
    lock_.ReadLock();
    lock_.ReadLock();
  }, "already holding lock for reading");

  EXPECT_DEATH(({
    CHECK(lock_.TryReadLock());
    CHECK(lock_.TryReadLock());
  }), "already holding lock for reading");

  EXPECT_DEATH({
    lock_.ReadLock();
    lock_.WriteLock();
  }, "already holding lock for reading");

  EXPECT_DEATH(({
    CHECK(lock_.TryReadLock());
    CHECK(lock_.TryWriteLock());
  }), "already holding lock for reading");

  EXPECT_DEATH({
    lock_.WriteLock();
    lock_.ReadLock();
  }, "already holding lock for writing");

  EXPECT_DEATH(({
    CHECK(lock_.TryWriteLock());
    CHECK(lock_.TryReadLock());
  }), "already holding lock for writing");

  EXPECT_DEATH(({
    lock_.WriteLock();
    lock_.WriteLock();
  }), "already holding lock for writing");

  EXPECT_DEATH(({
    CHECK(lock_.TryWriteLock());
    CHECK(lock_.TryWriteLock());
  }), "already holding lock for writing");

  EXPECT_DEATH({
    lock_.ReadUnlock();
  }, "wasn't holding lock for reading");

  EXPECT_DEATH({
    lock_.WriteUnlock();
  }, "wasn't holding lock for writing");

  EXPECT_DEATH({
    lock_.ReadLock();
    lock_.WriteUnlock();
  }, "already holding lock for reading");

  EXPECT_DEATH(({
    CHECK(lock_.TryReadLock());
    lock_.WriteUnlock();
  }), "already holding lock for reading");

  EXPECT_DEATH({
    lock_.WriteLock();
    lock_.ReadUnlock();
  }, "already holding lock for writing");

  EXPECT_DEATH(({
    CHECK(lock_.TryWriteLock());
    lock_.ReadUnlock();
  }), "already holding lock for writing");
}
#endif

} // namespace yb
