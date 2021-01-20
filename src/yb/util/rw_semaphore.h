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
#ifndef YB_UTIL_RW_SEMAPHORE_H
#define YB_UTIL_RW_SEMAPHORE_H

#include <boost/smart_ptr/detail/yield_k.hpp>
#include <glog/logging.h>

#include "yb/gutil/atomicops.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/port.h"
#include "yb/util/debug-util.h"

#include "yb/util/thread.h"

namespace yb {

// Read-Write semaphore. 32bit uint that contains the number of readers.
// When someone wants to write, tries to set the 32bit, and waits until
// the readers have finished. Readers are spinning while the write flag is set.
//
// This rw-semaphore makes no attempt at fairness, though it does avoid write
// starvation (no new readers may obtain the lock if a write is waiting).
//
// Given that this is currently based only on spinning (and not futex),
// it should only be used in cases where the lock is held for very short
// time intervals.
//
// If the semaphore is expected to always be released from the same thread
// that acquired it, use rw_spinlock instead.
//
// In order to support easier debugging of leaked locks, this class can track
// the stack trace of the last thread to lock it in write mode. To do so,
// uncomment the following define:
//   #define RW_SEMAPHORE_TRACK_HOLDER 1
// ... and then in gdb, print the contents of the semaphore, and you should
// see the collected stack trace.
class rw_semaphore {
#ifndef NDEBUG
  static constexpr int64_t kInvalidThreadId = -1;
#endif
 public:
  rw_semaphore() : state_(0) {}
  ~rw_semaphore() {}

  void lock_shared() {
    int loop_count = 0;
    Atomic32 cur_state = base::subtle::NoBarrier_Load(&state_);
    while (true) {
      Atomic32 expected = cur_state & kNumReadersMask;   // I expect no write lock
      Atomic32 try_new_state = expected + 1;          // Add me as reader
      cur_state = base::subtle::Acquire_CompareAndSwap(&state_, expected, try_new_state);
      if (cur_state == expected)
        break;
      // Either was already locked by someone else, or CAS failed.
      boost::detail::yield(loop_count++);
    }
  }

  void unlock_shared() {
    int loop_count = 0;
    Atomic32 cur_state = base::subtle::NoBarrier_Load(&state_);
    while (true) {
      DCHECK_GT(cur_state & kNumReadersMask, 0)
        << "unlock_shared() called when there are no shared locks held";
      Atomic32 expected = cur_state;           // I expect a write lock and other readers
      Atomic32 try_new_state = expected - 1;   // Drop me as reader
      cur_state = base::subtle::Release_CompareAndSwap(&state_, expected, try_new_state);
      if (cur_state == expected)
        break;
      // Either was already locked by someone else, or CAS failed.
      boost::detail::yield(loop_count++);
    }
  }

  // Tries to acquire a write lock, if no one else has it.
  // This function retries on CAS failure and waits for readers to complete.
  bool try_lock() {
    int loop_count = 0;
    Atomic32 cur_state = base::subtle::NoBarrier_Load(&state_);
    while (true) {
      // someone else has already the write lock
      if (cur_state & kWriteFlag)
        return false;

      Atomic32 expected = cur_state & kNumReadersMask;   // I expect some 0+ readers
      Atomic32 try_new_state = kWriteFlag | expected;    // I want to lock the other writers
      cur_state = base::subtle::Acquire_CompareAndSwap(&state_, expected, try_new_state);
      if (cur_state == expected)
        break;
      // Either was already locked by someone else, or CAS failed.
      boost::detail::yield(loop_count++);
    }

    WaitPendingReaders();
    RecordLockHolderStack();
    return true;
  }

  void lock() {
    int loop_count = 0;
    Atomic32 cur_state = base::subtle::NoBarrier_Load(&state_);
    while (true) {
      Atomic32 expected = cur_state & kNumReadersMask;   // I expect some 0+ readers
      Atomic32 try_new_state = kWriteFlag | expected;    // I want to lock the other writers
      // Note: we use NoBarrier here because we'll do the Acquire barrier down below
      // in WaitPendingReaders
      cur_state = base::subtle::NoBarrier_CompareAndSwap(&state_, expected, try_new_state);
      if (cur_state == expected)
        break;
      // Either was already locked by someone else, or CAS failed.
      boost::detail::yield(loop_count++);
    }

    WaitPendingReaders();

#ifndef NDEBUG
    writer_tid_ = Thread::CurrentThreadId();
#endif // NDEBUG
    RecordLockHolderStack();
  }

  void unlock() {
    // I expect to be the only writer
    DCHECK_EQ(base::subtle::NoBarrier_Load(&state_), kWriteFlag);

#ifndef NDEBUG
    writer_tid_ = kInvalidThreadId; // Invalid tid.
#endif // NDEBUG

    ResetLockHolderStack();
    // Reset: no writers & no readers.
    Release_Store(&state_, 0);
  }

  // Return true if the lock is currently held for write by any thread.
  // See simple_semaphore::is_locked() for details about where this is useful.
  bool is_write_locked() const {
    return base::subtle::NoBarrier_Load(&state_) & kWriteFlag;
  }

  // Return true if the lock is currently held, either for read or write
  // by any thread.
  // See simple_semaphore::is_locked() for details about where this is useful.
  bool is_locked() const {
    return base::subtle::NoBarrier_Load(&state_);
  }

 private:
  static const uint32_t kNumReadersMask = 0x7fffffff;
  static const uint32_t kWriteFlag = 1 << 31;

#ifdef RW_SEMAPHORE_TRACK_HOLDER
  StackTrace writer_stack_;
  void RecordLockHolderStack() {
    writer_stack_.Collect();
  }
  void ResetLockHolderStack() {
    writer_stack_.Reset();
  }
#else
  void RecordLockHolderStack() {
  }
  void ResetLockHolderStack() {
  }
#endif

  void WaitPendingReaders() {
    int loop_count = 0;
    while ((base::subtle::Acquire_Load(&state_) & kNumReadersMask) > 0) {
      boost::detail::yield(loop_count++);
    }
  }

 private:
  volatile Atomic32 state_;
#ifndef NDEBUG
  int64_t writer_tid_ = kInvalidThreadId;
#endif // NDEBUG
};

} // namespace yb
#endif /* YB_UTIL_RW_SEMAPHORE_H */
