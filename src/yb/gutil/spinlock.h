/* Copyright (c) 2006, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The following only applies to changes made to this file as part of YugaByte development.
 *
 * Portions Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 *
 * ---
 * Author: Sanjay Ghemawat
 */

// SpinLock is async signal safe.
// If used within a signal handler, all lock holders
// should block the signal even outside the signal handler.

#ifndef YB_GUTIL_SPINLOCK_H
#define YB_GUTIL_SPINLOCK_H

#include "yb/gutil/atomicops.h"
#include "yb/gutil/basictypes.h"
#include "yb/gutil/dynamic_annotations.h"
#include "yb/gutil/thread_annotations.h"

// This isn't originally in the base:: namespace in tcmalloc,
// but tcmalloc inadvertently exports these symbols. So, if we
// don't namespace it differently, we conflict.
namespace base {

class CAPABILITY("mutex") SpinLock {
 public:
  SpinLock() : lockword_(kSpinLockFree) { }

  // Special constructor for use with static SpinLock objects.  E.g.,
  //
  //    static SpinLock lock(base::LINKER_INITIALIZED);
  //
  // When intialized using this constructor, we depend on the fact
  // that the linker has already initialized the memory appropriately.
  // A SpinLock constructed like this can be freely used from global
  // initializers without worrying about the order in which global
  // initializers run.
  explicit SpinLock(base::LinkerInitialized /*x*/)
      : lockword_(kSpinLockFree) {
  }

  // Acquire this SpinLock.
  // TODO(csilvers): uncomment the annotation when we figure out how to
  //                 support this macro with 0 args (see thread_annotations.h)
  inline void Lock() ACQUIRE() {
    if (base::subtle::Acquire_CompareAndSwap(&lockword_, kSpinLockFree,
                                             kSpinLockHeld) != kSpinLockFree) {
      SlowLock();
    }
    ANNOTATE_RWLOCK_ACQUIRED(this, 1);
  }

  // Try to acquire this SpinLock without blocking and return true if the
  // acquisition was successful.  If the lock was not acquired, false is
  // returned.  If this SpinLock is free at the time of the call, TryLock
  // will return true with high probability.
  inline bool TryLock() TRY_ACQUIRE(true) {
    bool res =
        (base::subtle::Acquire_CompareAndSwap(&lockword_, kSpinLockFree,
                                              kSpinLockHeld) == kSpinLockFree);
    if (res) {
      ANNOTATE_RWLOCK_ACQUIRED(this, 1);
    }
    return res;
  }

  // Release this SpinLock, which must be held by the calling thread.
  // TODO(csilvers): uncomment the annotation when we figure out how to
  //                 support this macro with 0 args (see thread_annotations.h)
  inline void Unlock() RELEASE() {
    ANNOTATE_RWLOCK_RELEASED(this, 1);
    uint64 wait_cycles = static_cast<uint64>(
        base::subtle::Release_AtomicExchange(&lockword_, kSpinLockFree));
    if (wait_cycles != kSpinLockHeld) {
      // Collect contentionz profile info, and speed the wakeup of any waiter.
      // The wait_cycles value indicates how long this thread spent waiting
      // for the lock.
      SlowUnlock(wait_cycles);
    }
  }

  // Determine if the lock is held.  When the lock is held by the invoking
  // thread, true will always be returned. Intended to be used as
  // CHECK(lock.IsHeld()).
  inline bool IsHeld() const {
    return base::subtle::NoBarrier_Load(&lockword_) != kSpinLockFree;
  }

  static const base::LinkerInitialized LINKER_INITIALIZED;  // backwards compat
 private:
  enum { kSpinLockFree = 0 };
  enum { kSpinLockHeld = 1 };
  enum { kSpinLockSleeper = 2 };

  volatile Atomic32 lockword_;

  void SlowLock();
  void SlowUnlock(uint64 wait_cycles);
  Atomic32 SpinLoop(int64 initial_wait_timestamp, Atomic32* wait_cycles);
  inline int32 CalculateWaitCycles(int64 wait_start_time);

  DISALLOW_COPY_AND_ASSIGN(SpinLock);
};

// Corresponding locker object that arranges to acquire a spinlock for
// the duration of a C++ scope.
class SCOPED_CAPABILITY SpinLockHolder {
 private:
  SpinLock* lock_;
 public:
  inline explicit SpinLockHolder(SpinLock* l) ACQUIRE(l)
      : lock_(l) {
    l->Lock();
  }

  inline ~SpinLockHolder() RELEASE() { lock_->Unlock(); }
};
// Catch bug where variable name is omitted, e.g. SpinLockHolder (&lock);
#define SpinLockHolder(x) COMPILE_ASSERT(0, spin_lock_decl_missing_var_name)

} // namespace base

#endif  // YB_GUTIL_SPINLOCK_H
