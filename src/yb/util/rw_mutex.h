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

#pragma once

#include <pthread.h>
#include <unordered_set>

#include "yb/gutil/macros.h"
#include "yb/gutil/thread_annotations.h"
#include "yb/util/locks.h"

namespace yb {

// Read/write mutex. Implemented as a thin wrapper around pthread_rwlock_t.
//
// Although pthread_rwlock_t allows recursive acquisition, this wrapper does
// not, and will crash in debug mode if recursive acquisition is detected.
class CAPABILITY("mutex") RWMutex {
 public:

  // Possible fairness policies for the RWMutex.
  enum class Priority {
    // The lock will prioritize readers at the expense of writers.
    PREFER_READING,

    // The lock will prioritize writers at the expense of readers.
    //
    // Care should be taken when using this fairness policy, as it can lead to
    // unexpected deadlocks (e.g. a writer waiting on the lock will prevent
    // additional readers from acquiring it).
    PREFER_WRITING,
  };

  // Create an RWMutex that prioritizes readers.
  RWMutex();

  // Create an RWMutex with customized priority. This is a best effort; the
  // underlying platform may not support custom priorities.
  explicit RWMutex(Priority prio);

  ~RWMutex();

  void ReadLock() ACQUIRE_SHARED();
  void ReadUnlock() RELEASE_SHARED();
  bool TryReadLock() TRY_ACQUIRE_SHARED(true);

  void WriteLock() ACQUIRE();
  void WriteUnlock() RELEASE();
  bool TryWriteLock() TRY_ACQUIRE(true);

#ifndef NDEBUG
  void AssertAcquiredForReading() const;
  void AssertAcquiredForWriting() const;
#else
  void AssertAcquiredForReading() const {}
  void AssertAcquiredForWriting() const {}
#endif

  // Aliases for use with std::lock_guard and yb::shared_lock.
  void lock() ACQUIRE() { WriteLock(); }
  void unlock() RELEASE() { WriteUnlock(); }
  bool try_lock() TRY_ACQUIRE(true) { return TryWriteLock(); }
  void lock_shared() ACQUIRE_SHARED() { ReadLock(); }
  void unlock_shared() RELEASE_SHARED() { ReadUnlock(); }
  bool try_lock_shared() TRY_ACQUIRE_SHARED(true) { return TryReadLock(); }

 private:
  void Init(Priority prio);

  enum class LockState {
    NEITHER,
    READER,
    WRITER,
  };
#ifndef NDEBUG
  void CheckLockState(LockState state) const;
  void MarkForReading();
  void MarkForWriting();
  void UnmarkForReading();
  void UnmarkForWriting();
#else
  void CheckLockState(LockState state) const {}
  void MarkForReading() {}
  void MarkForWriting() {}
  void UnmarkForReading() {}
  void UnmarkForWriting() {}
#endif

  pthread_rwlock_t native_handle_;

#ifndef NDEBUG
  // Protects reader_tids_ and writer_tid_.
  mutable simple_spinlock tid_lock_;

  // Tracks all current readers by tid.
  std::unordered_set<pid_t> reader_tids_;

  // Tracks the current writer (if one exists) by tid.
  pid_t writer_tid_;
#endif

  DISALLOW_COPY_AND_ASSIGN(RWMutex);
};

} // namespace yb
