//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
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
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#ifndef YB_ROCKSDB_UTIL_THREAD_POSIX_H
#define YB_ROCKSDB_UTIL_THREAD_POSIX_H

#pragma once

#include "yb/rocksdb/env.h"
#include "yb/rocksdb/util/thread_status_util.h"

#include "yb/util/thread.h"

namespace rocksdb {

class ThreadPool {
 public:
  ThreadPool();
  ~ThreadPool();

  void JoinAllThreads();
  void LowerIOPriority();
  void BGThread(size_t thread_id);
  void WakeUpAllThreads();
  void IncBackgroundThreadsIfNeeded(int num);
  void SetBackgroundThreads(int num);
  void StartBGThreads();
  void Schedule(void (*function)(void* arg1), void* arg, void* tag,
                void (*unschedFunction)(void* arg));
  int UnSchedule(void* arg);

  unsigned int GetQueueLen() const {
    return queue_len_.load(std::memory_order_relaxed);
  }

  void SetHostEnv(Env* env) { env_ = env; }
  Env* GetHostEnv() { return env_; }

  // Return true if there is at least one thread needs to terminate.
  bool HasExcessiveThread() {
    return static_cast<int>(bgthreads_.size()) > total_threads_limit_;
  }

  // Return true iff the current thread is the excessive thread to terminate.
  // Always terminate the running thread that is added last, even if there are
  // more than one thread to terminate.
  bool IsLastExcessiveThread(size_t thread_id) {
    return HasExcessiveThread() && thread_id == bgthreads_.size() - 1;
  }

  // Is one of the threads to terminate.
  bool IsExcessiveThread(size_t thread_id) {
    return static_cast<int>(thread_id) >= total_threads_limit_;
  }

  // Return the thread priority.
  // This would allow its member-thread to know its priority.
  Env::Priority GetThreadPriority() { return priority_; }

  // Set the thread priority.
  void SetThreadPriority(Env::Priority priority) { priority_ = priority; }

  static void PthreadCall(const char* label, int result);

 private:
  // Entry per Schedule() call
  struct BGItem {
    void* arg;
    void (*function)(void*);
    void* tag;
    void (*unschedFunction)(void*);
  };
  typedef std::deque<BGItem> BGQueue;

  pthread_mutex_t mu_;
  pthread_cond_t bgsignal_;
  int total_threads_limit_ = 1;
  std::vector<yb::ThreadPtr> bgthreads_;
  BGQueue queue_;
  std::atomic_uint queue_len_{0};  // Queue length. Used for stats reporting
  bool exit_all_threads_ = false;
  bool low_io_priority_ = false;
  Env::Priority priority_ = static_cast<Env::Priority>(0);
  Env* env_ = nullptr;

  void SetBackgroundThreadsInternal(int num, bool allow_reduce);
};

}  // namespace rocksdb

#endif // YB_ROCKSDB_UTIL_THREAD_POSIX_H
