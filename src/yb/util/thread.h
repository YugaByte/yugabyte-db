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

#ifndef YB_UTIL_THREAD_H
#define YB_UTIL_THREAD_H

#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

#include "yb/gutil/atomicops.h"
#include "yb/gutil/ref_counted.h"
#include "yb/util/async_util.h"
#include "yb/util/result.h"
#include "yb/util/status.h"

namespace yb {

class MetricEntity;
class Thread;
class WebCallbackRegistry;

#if defined(__linux__)
typedef int64_t ThreadIdForStack;
#else
typedef pthread_t ThreadIdForStack;
#endif

// Utility to join on a thread, printing warning messages if it
// takes too long. For example:
//
//   ThreadJoiner(&my_thread, "processing thread")
//     .warn_after_ms(1000)
//     .warn_every_ms(5000)
//     .Join();
//
// TODO: would be nice to offer a way to use ptrace() or signals to
// dump the stack trace of the thread we're trying to join on if it
// gets stuck. But, after looking for 20 minutes or so, it seems
// pretty complicated to get right.
class ThreadJoiner {
 public:
  explicit ThreadJoiner(Thread* thread);

  // Start emitting warnings after specified duration.
  //
  // Default: 1000 ms.
  ThreadJoiner& warn_after(MonoDelta duration);

  // After the warnings after started, emit another warning at the
  // given interval.
  //
  // Default: 1000 ms.
  ThreadJoiner& warn_every(MonoDelta duration);

  // If the thread has not stopped after this duration, give up
  // joining on it and return Status::Aborted.
  //
  // MonoDelta::kMax (the default) means to wait forever trying to join.
  ThreadJoiner& give_up_after(MonoDelta duration);

  // Join the thread, subject to the above parameters. If the thread joining
  // fails for any reason, returns RuntimeError. If it times out, returns
  // Aborted.
  CHECKED_STATUS Join();

 private:
  Thread* thread_;

  MonoDelta warn_after_ = MonoDelta::FromMilliseconds(1000);
  MonoDelta warn_every_ = MonoDelta::FromMilliseconds(1000);
  MonoDelta give_up_after_ = MonoDelta::kMax;

  DISALLOW_COPY_AND_ASSIGN(ThreadJoiner);
};

typedef scoped_refptr<Thread> ThreadPtr;

// Thin wrapper around pthread that can register itself with the singleton ThreadMgr
// (a private class implemented in thread.cc entirely, which tracks all live threads so
// that they may be monitored via the debug webpages). This class has a limited subset of
// std::thread's API. Construction is almost the same, but clients must supply a
// category and a name for each thread so that they can be identified in the debug web
// UI. Otherwise, Join() is the only supported method from std::thread.
//
// Each Thread object knows its operating system thread ID (TID), which can be used to
// attach debuggers to specific threads, to retrieve resource-usage statistics from the
// operating system, and to assign threads to resource control groups.
//
// Threads are shared objects, but in a degenerate way. They may only have
// up to two referents: the caller that created the thread (parent), and
// the thread itself (child). Moreover, the only two methods to mutate state
// (Join() and the destructor) are constrained: the child may not Join() on
// itself, and the destructor is only run when there's one referent left.
// These constraints allow us to access thread internals without any locks.
//
// TODO: Consider allowing fragment IDs as category parameters.
class Thread : public RefCountedThreadSafe<Thread> {
 public:
  static const char kPaddingChar;

  // This constructor pattern mimics that in std::thread. There is
  // one constructor for each number of arguments that the thread
  // function accepts. To extend the set of acceptable signatures, add
  // another constructor with <class F, class A1.... class An>.
  //
  // In general:
  //  - category: string identifying the thread category to which this thread belongs,
  //    used for organising threads together on the debug UI.
  //  - name: name of this thread. Will be appended with "-<thread-id>" to ensure
  //    uniqueness.
  //  - F - a method type that supports operator(), and the instance passed to the
  //    constructor is executed immediately in a separate thread.
  //  - A1...An - argument types whose instances are passed to f(...)
  //  - holder - optional shared pointer to hold a reference to the created thread.
  template <class F>
  static CHECKED_STATUS Create(const std::string& category, const std::string& name, const F& f,
                       scoped_refptr<Thread>* holder) {
    return StartThread(category, name, f, holder);
  }

  template <class F, class A1>
  static CHECKED_STATUS Create(const std::string& category, const std::string& name, const F& f,
                       const A1& a1, scoped_refptr<Thread>* holder) {
    return StartThread(category, name, std::bind(f, a1), holder);
  }

  template <class F, class A1, class A2>
  static CHECKED_STATUS Create(const std::string& category, const std::string& name, const F& f,
                       const A1& a1, const A2& a2, scoped_refptr<Thread>* holder) {
    return StartThread(category, name, std::bind(f, a1, a2), holder);
  }

  template <class F, class A1, class A2, class A3>
  static CHECKED_STATUS Create(const std::string& category, const std::string& name, const F& f,
                       const A1& a1, const A2& a2, const A3& a3, scoped_refptr<Thread>* holder) {
    return StartThread(category, name, std::bind(f, a1, a2, a3), holder);
  }

  template <class F, class A1, class A2, class A3, class A4>
  static CHECKED_STATUS Create(const std::string& category, const std::string& name, const F& f,
                       const A1& a1, const A2& a2, const A3& a3, const A4& a4,
                       scoped_refptr<Thread>* holder) {
    return StartThread(category, name, std::bind(f, a1, a2, a3, a4), holder);
  }

  template <class F, class A1, class A2, class A3, class A4, class A5>
  static CHECKED_STATUS Create(const std::string& category, const std::string& name, const F& f,
                       const A1& a1, const A2& a2, const A3& a3, const A4& a4, const A5& a5,
                       scoped_refptr<Thread>* holder) {
    return StartThread(category, name, std::bind(f, a1, a2, a3, a4, a5), holder);
  }

  template <class F, class A1, class A2, class A3, class A4, class A5, class A6>
  static CHECKED_STATUS Create(const std::string& category, const std::string& name, const F& f,
                       const A1& a1, const A2& a2, const A3& a3, const A4& a4, const A5& a5,
                       const A6& a6, scoped_refptr<Thread>* holder) {
    return StartThread(category, name, std::bind(f, a1, a2, a3, a4, a5, a6), holder);
  }

  template <class F>
  static Result<ThreadPtr> Make(
      const std::string& category, const std::string& name, const F& f) {
    ThreadPtr result;
    RETURN_NOT_OK(StartThread(category, name, f, &result));
    return result;
  }

  template <class... Args>
  static Result<ThreadPtr> Make(
      const std::string& category, const std::string& name, Args&&... args) {
    ThreadPtr result;
    RETURN_NOT_OK(StartThread(category, name, std::bind(std::forward<Args>(args)...), &result));
    return result;
  }

  // Emulates std::thread and detaches.
  ~Thread();

  // Blocks until this thread finishes execution. Once this method returns, the thread
  // will be unregistered with the ThreadMgr and will not appear in the debug UI.
  void Join() { WARN_NOT_OK(ThreadJoiner(this).Join(), "Thread join failed"); }

  // Call the given Closure on the thread before it exits. The closures are executed
  // in the order they are added.
  //
  // NOTE: This must only be called on the currently executing thread, to avoid having
  // to reason about complicated races (eg registering a callback on an already-dead
  // thread).
  //
  // This callback is guaranteed to be called except in the case of a process crash.
  void CallAtExit(const Closure& cb);

  // The thread ID assigned to this thread by the operating system. If the OS does not
  // support retrieving the tid, returns Thread::INVALID_TID.
  int64_t tid() const { return tid_; }

  // Returns the thread's pthread ID.
  pthread_t pthread_id() const { return thread_; }

  ThreadIdForStack tid_for_stack() {
#if defined(__linux__)
    return tid();
#else
    return pthread_id();
#endif
  }

  const std::string& name() const { return name_; }
  const std::string& category() const { return category_; }

  // Return a string representation of the thread identifying information.
  std::string ToString() const;

  // The current thread of execution, or NULL if the current thread isn't a yb::Thread.
  // This call is signal-safe.
  static Thread* current_thread() { return tls_; }

  // Returns a unique, stable identifier for this thread. Note that this is a static
  // method and thus can be used on any thread, including the main thread of the
  // process.
  //
  // In general, this should be used when a value is required that is unique to
  // a thread and must work on any thread including the main process thread.
  //
  // NOTE: this is _not_ the TID, but rather a unique value assigned by the
  // thread implementation. So, this value should not be presented to the user
  // in log messages, etc.
  static int64_t UniqueThreadId() {
#if defined(__linux__)
    // This cast is a little bit ugly, but it is significantly faster than
    // calling syscall(SYS_gettid). In particular, this speeds up some code
    // paths in the tracing implementation.
    return static_cast<int64_t>(pthread_self());
#elif defined(__APPLE__)
    uint64_t tid;
    CHECK_EQ(0, pthread_threadid_np(NULL, &tid));
    return tid;
#else
#error Unsupported platform
#endif
  }

  // Returns the system thread ID (tid on Linux) for the current thread. Note
  // that this is a static method and thus can be used from any thread,
  // including the main thread of the process. This is in contrast to
  // Thread::tid(), which only works on yb::Threads.
  //
  // Thread::tid() will return the same value, but the value is cached in the
  // Thread object, so will be faster to call.
  //
  // Thread::UniqueThreadId() (or Thread::tid()) should be preferred for
  // performance sensistive code, however it is only guaranteed to return a
  // unique and stable thread ID, not necessarily the system thread ID.
  static int64_t CurrentThreadId() {
#if defined(__linux__)
    return syscall(SYS_gettid);
#else
    return UniqueThreadId();
#endif
  }

  static ThreadIdForStack CurrentThreadIdForStack() {
#if defined(__linux__)
    return CurrentThreadId();
#else
    return pthread_self();
#endif
  }

  void* user_data() {
    return user_data_;
  }

  void SetUserData(void* value) {
    user_data_ = value;
  }

 private:
  friend class ThreadJoiner;

  // The various special values for tid_ that describe the various steps
  // in the parent<-->child handshake.
  enum {
    INVALID_TID = -1,
    CHILD_WAITING_TID = -2,
    PARENT_WAITING_TID = -3,
  };

  // Function object that wraps the user-supplied function to run in a separate thread.
  typedef std::function<void()> ThreadFunctor;

  Thread(std::string category, std::string name, ThreadFunctor functor)
      : thread_(0),
        category_(std::move(category)),
        name_(std::move(name)),
        tid_(CHILD_WAITING_TID),
        functor_(std::move(functor)),
        done_(1),
        joinable_(false) {}

  // Library-specific thread ID.
  pthread_t thread_;

  // Name and category for this thread.
  const std::string category_;
  const std::string name_;

  // OS-specific thread ID. Once the constructor finishes StartThread(),
  // guaranteed to be set either to a non-negative integer, or to INVALID_TID.
  int64_t tid_;

  // User function to be executed by this thread.
  const ThreadFunctor functor_;

  // Joiners wait on this latch to be notified if the thread is done.
  //
  // Note that Joiners must additionally pthread_join(), otherwise certain
  // resources that callers expect to be destroyed (like TLS) may still be
  // alive when a Joiner finishes.
  CountDownLatch done_;

  bool joinable_;

  // Thread local pointer to the current thread of execution. Will be NULL if the current
  // thread is not a Thread.
  static __thread Thread* tls_;

  std::vector<Closure> exit_callbacks_;

  // Some generic user data. For instance could be used to identify thread pool, which started
  // this thread.
  void* user_data_ = nullptr;

  // Starts the thread running SuperviseThread(), and returns once that thread has
  // initialised and its TID has been read. Waits for notification from the started
  // thread that initialisation is complete before returning. On success, stores a
  // reference to the thread in holder.
  static CHECKED_STATUS StartThread(
      const std::string& category, const std::string& name,
      ThreadFunctor functor, ThreadPtr* holder);

  // Wrapper for the user-supplied function. Invoked from the new thread,
  // with the Thread as its only argument. Executes functor_, but before
  // doing so registers with the global ThreadMgr and reads the thread's
  // system ID. After functor_ terminates, unregisters with the ThreadMgr.
  // Always returns NULL.
  //
  // SuperviseThread() notifies StartThread() when thread initialisation is
  // completed via the tid_, which is set to the new thread's system ID.
  // By that point in time SuperviseThread() has also taken a reference to
  // the Thread object, allowing it to safely refer to it even after the
  // caller drops its reference.
  //
  // Additionally, StartThread() notifies SuperviseThread() when the actual
  // Thread object has been assigned (SuperviseThread() is spinning during
  // this time). Without this, the new thread may reference the actual
  // Thread object before it has been assigned by StartThread(). See
  // KUDU-11 for more details.
  static void* SuperviseThread(void* arg);

  // Invoked when the user-supplied function finishes or in the case of an
  // abrupt exit (i.e. pthread_exit()). Cleans up after SuperviseThread().
  static void FinishThread(void* arg);
};

typedef scoped_refptr<Thread> ThreadPtr;

// Registers /threadz with the debug webserver, and creates thread-tracking metrics under
// the given entity.
Status StartThreadInstrumentation(const scoped_refptr<MetricEntity>& server_metrics,
                                  WebCallbackRegistry* web);

// This initializes the thread manager and warms up libunwind's state (see ENG-1402).
void InitThreading();

void SetThreadName(const std::string& name);

class CDSAttacher {
 public:
  CDSAttacher();
  ~CDSAttacher();
};

} // namespace yb

#endif /* YB_UTIL_THREAD_H */
