//
// Copyright (c) YugaByte, Inc.
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
//

#ifndef YB_RPC_THREAD_POOL_H
#define YB_RPC_THREAD_POOL_H

#include <memory>
#include <string>

#include "yb/gutil/port.h"

#include "yb/util/tostring.h"

namespace yb {

class Status;
class Thread;

namespace rpc {

class ThreadPoolTask {
 public:
  // Invoked in thread pool
  virtual void Run() = 0;

  // When thread pool done with task, i.e. it completed or failed, it invokes Done
  virtual void Done(const Status& status) = 0;

 protected:
  ~ThreadPoolTask() {}
};

template <class F, class Base = ThreadPoolTask>
class FunctorThreadPoolTask : public Base {
 public:
  explicit FunctorThreadPoolTask(const F& f) : f_(f) {}
  explicit FunctorThreadPoolTask(F&& f) : f_(std::move(f)) {}

  virtual ~FunctorThreadPoolTask() = default;

 private:
  void Run() override {
    f_();
  }

  void Done(const Status& status) override {
    delete this;
  }

  F f_;
};

template <class F>
FunctorThreadPoolTask<F>* MakeFunctorThreadPoolTask(const F& f) {
  return new FunctorThreadPoolTask<F>(f);
}

template <class F>
FunctorThreadPoolTask<F>* MakeFunctorThreadPoolTask(F&& f) {
  return new FunctorThreadPoolTask<F>(std::move(f));
}

struct ThreadPoolOptions {
  std::string name;
  size_t queue_limit;
  size_t max_workers;

  std::string ToString() const {
    return YB_STRUCT_TO_STRING(name, queue_limit, max_workers);
  }
};

class ThreadPool {
 public:
  explicit ThreadPool(ThreadPoolOptions options);

  template <class... Args>
  explicit ThreadPool(Args&&... args)
      : ThreadPool(ThreadPoolOptions{std::forward<Args>(args)...}) {

  }

  ~ThreadPool();

  ThreadPool(ThreadPool&& rhs) noexcept;
  ThreadPool& operator=(ThreadPool&& rhs) noexcept;

  const ThreadPoolOptions& options() const;

  bool Enqueue(ThreadPoolTask* task);

  template <class F>
  void EnqueueFunctor(const F& f) {
    Enqueue(MakeFunctorThreadPoolTask(f));
  }

  template <class F>
  void EnqueueFunctor(F&& f) {
    Enqueue(MakeFunctorThreadPoolTask(std::move(f)));
  }

  void Shutdown();

  static bool IsCurrentThreadRpcWorker();

  bool Owns(Thread* thread);
  bool OwnsThisThread();

 private:
  class Impl;

  std::unique_ptr<Impl> impl_;
};

} // namespace rpc
} // namespace yb

#endif // YB_RPC_THREAD_POOL_H
