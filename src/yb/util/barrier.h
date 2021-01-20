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

#ifndef YB_UTIL_BARRIER_H
#define YB_UTIL_BARRIER_H
#pragma once

#include "yb/gutil/macros.h"
#include "yb/util/condition_variable.h"
#include "yb/util/mutex.h"
#include "yb/util/thread_restrictions.h"

namespace yb {

// Implementation of pthread-style Barriers.
class Barrier {
 public:
  // Initialize the barrier with the given initial count.
  explicit Barrier(size_t count) :
      cond_(&mutex_),
      count_(count),
      wait_count_(count) {
  }

  // Wait until all threads have reached the barrier.
  // Once all threads have reached the barrier, it is reset to the initial count.
  void Wait() {
    WaitImpl(false /* detach */);
  }

  // Wait until all threads have reached the barrier.
  // Once all threads have reached the barrier, it is reset to the initial count minus 1.
  // This method must be called in case thread is finished.
  // Other threads will not wait for it on next loop.
  void Detach() {
    WaitImpl(true /* detach */);
  }

 private:
  void WaitImpl(bool detach) {
    ThreadRestrictions::AssertWaitAllowed();
    MutexLock l(mutex_);
    if (detach) {
      DCHECK_GT(wait_count_, 0);
      --wait_count_;
    }
    if (--count_ == 0) {
      count_ = wait_count_;
      ++cycle_count_;
      cond_.Broadcast();
      return;
    }

    for (const auto initial_cycle = cycle_count_; cycle_count_ == initial_cycle;) {
      cond_.Wait();
    }
  }

  Mutex mutex_;
  ConditionVariable cond_;
  size_t count_;
  size_t cycle_count_ = 0;
  size_t wait_count_;
  DISALLOW_COPY_AND_ASSIGN(Barrier);
};

} // namespace yb
#endif // YB_UTIL_BARRIER_H
