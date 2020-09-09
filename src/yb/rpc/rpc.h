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
#ifndef YB_RPC_RPC_H
#define YB_RPC_RPC_H

#include <atomic>
#include <memory>
#include <string>

#include <boost/container/stable_vector.hpp>

#include <boost/optional/optional.hpp>

#include "yb/gutil/callback.h"
#include "yb/rpc/rpc_controller.h"

#include "yb/util/enums.h"
#include "yb/util/monotime.h"
#include "yb/util/result.h"
#include "yb/util/status_callback.h"

namespace yb {

namespace rpc {

class Messenger;
class Rpc;

// The command that could be retried by RpcRetrier.
class RpcCommand : public std::enable_shared_from_this<RpcCommand> {
 public:
  // Asynchronously sends the RPC to the remote end.
  //
  // Subclasses should use Finished() below as the callback function.
  virtual void SendRpc() = 0;

  // Returns a string representation of the RPC.
  virtual std::string ToString() const = 0;

  // Callback for SendRpc(). If 'status' is not OK, something failed
  // before the RPC was sent.
  virtual void Finished(const Status& status) = 0;

  virtual void Abort() = 0;

  virtual CoarseTimePoint deadline() const = 0;

 protected:
  ~RpcCommand() {}
};

YB_DEFINE_ENUM(RpcRetrierState, (kIdle)(kRunning)(kScheduling)(kWaiting)(kFinished));
YB_DEFINE_ENUM(BackoffStrategy, (kLinear)(kExponential));
YB_STRONGLY_TYPED_BOOL(RetryWhenBusy);

// Provides utilities for retrying failed RPCs.
//
// All RPCs should use HandleResponse() to retry certain generic errors.
class RpcRetrier {
 public:
  RpcRetrier(CoarseTimePoint deadline, Messenger* messenger, ProxyCache *proxy_cache);

  ~RpcRetrier();

  // Tries to handle a failed RPC.
  //
  // If it was handled (e.g. scheduled for retry in the future), returns
  // true. In this case, callers should ensure that 'rpc' remains alive.
  //
  // Otherwise, returns false and writes the controller status to
  // 'out_status'.
  // retry_when_busy should be set to false if user does not want to retry request when server
  // returned that he is busy.
  bool HandleResponse(RpcCommand* rpc, Status* out_status,
                      RetryWhenBusy retry_when_busy = RetryWhenBusy::kTrue);

  // Retries an RPC at some point in the near future. If 'why_status' is not OK,
  // records it as the most recent error causing the RPC to retry. This is
  // reported to the caller eventually if the RPC never succeeds.
  //
  // If the RPC's deadline expires, the callback will fire with a timeout
  // error when the RPC comes up for retrying. This is true even if the
  // deadline has already expired at the time that Retry() was called.
  //
  // Callers should ensure that 'rpc' remains alive.
  CHECKED_STATUS DelayedRetry(
      RpcCommand* rpc, const Status& why_status,
      BackoffStrategy strategy = BackoffStrategy::kLinear);

  CHECKED_STATUS DelayedRetry(
      RpcCommand* rpc, const Status& why_status, MonoDelta add_delay);

  RpcController* mutable_controller() { return &controller_; }
  const RpcController& controller() const { return controller_; }

  // Sets up deadline and returns controller.
  // Do not forget that setting deadline in RpcController is NOT thread safe.
  RpcController* PrepareController();

  CoarseTimePoint deadline() const { return deadline_; }

  rpc::Messenger* messenger() const {
    return messenger_;
  }

  ProxyCache& proxy_cache() const {
    return proxy_cache_;
  }

  int attempt_num() const { return attempt_num_; }

  void Abort();

  std::string ToString() const;

  bool finished() const {
    return state_.load(std::memory_order_acquire) == RpcRetrierState::kFinished;
  }

 private:
  CHECKED_STATUS DoDelayedRetry(RpcCommand* rpc, const Status& why_status);

  // Called when an RPC comes up for retrying. Actually sends the RPC.
  void DoRetry(RpcCommand* rpc, const Status& status);

  // The next sent rpc will be the nth attempt (indexed from 1).
  int attempt_num_ = 1;

  // Delay used for the last DelayedRetry. Depends on argument history of DelayedRetry calls.
  MonoDelta retry_delay_ = MonoDelta::kZero;

  const CoarseTimePoint start_;

  // If the remote end is busy, the RPC will be retried (with a small
  // delay) until this deadline is reached.
  //
  // May be uninitialized.
  const CoarseTimePoint deadline_;

  // Messenger to use when sending the RPC.
  Messenger* messenger_ = nullptr;

  ProxyCache& proxy_cache_;

  // RPC controller to use when sending the RPC.
  RpcController controller_;

  // In case any retries have already happened, remembers the last error.
  // Errors from the server take precedence over timeout errors.
  Status last_error_;

  std::atomic<ScheduledTaskId> task_id_{kInvalidTaskId};

  std::atomic<RpcRetrierState> state_{RpcRetrierState::kIdle};

  DISALLOW_COPY_AND_ASSIGN(RpcRetrier);
};

// An in-flight remote procedure call to some server.
class Rpc : public RpcCommand {
 public:
  Rpc(CoarseTimePoint deadline, Messenger* messenger, ProxyCache* proxy_cache)
      : retrier_(deadline, messenger, proxy_cache) {
  }

  virtual ~Rpc() {}

  // Returns the number of times this RPC has been sent. Will always be at
  // least one.
  int num_attempts() const { return retrier().attempt_num(); }
  CoarseTimePoint deadline() const override { return retrier_.deadline(); }

  void Abort() override {
    retrier_.Abort();
  }

  void ScheduleRetry(const Status& status);

 protected:
  const RpcRetrier& retrier() const { return retrier_; }
  RpcRetrier* mutable_retrier() { return &retrier_; }
  RpcController* PrepareController() {
    return retrier_.PrepareController();
  }

 private:
  friend class RpcRetrier;

  // Used to retry some failed RPCs.
  RpcRetrier retrier_;

  DISALLOW_COPY_AND_ASSIGN(Rpc);
};

YB_STRONGLY_TYPED_BOOL(RequestShutdown);

class Rpcs {
 public:
  explicit Rpcs(std::mutex* mutex = nullptr);
  ~Rpcs() { Shutdown(); }

  typedef boost::container::stable_vector<rpc::RpcCommandPtr> Calls;
  typedef Calls::iterator Handle;

  void Shutdown();
  Handle Register(RpcCommandPtr call);
  void Register(RpcCommandPtr call, Handle* handle);
  bool RegisterAndStart(RpcCommandPtr call, Handle* handle);
  RpcCommandPtr Unregister(Handle* handle);
  void Abort(std::initializer_list<Handle*> list);
  // Request all active calls to abort.
  void RequestAbortAll();
  Rpcs::Handle Prepare();

  RpcCommandPtr Unregister(Handle handle) {
    return Unregister(&handle);
  }

  Handle InvalidHandle() { return calls_.end(); }

 private:
  // Requests all active calls to abort. Returns deadline for waiting on abort completion.
  // If shutdown is true - switches Rpcs to shutting down state.
  CoarseTimePoint DoRequestAbortAll(RequestShutdown shutdown);

  boost::optional<std::mutex> mutex_holder_;
  std::mutex* mutex_;
  std::condition_variable cond_;
  Calls calls_;
  bool shutdown_ = false;
};

template <class T, class... Args>
RpcCommandPtr StartRpc(Args&&... args) {
  auto rpc = std::make_shared<T>(std::forward<Args>(args)...);
  rpc->SendRpc();
  return rpc;
}

template <class Value>
class RpcFutureCallback {
 public:
  RpcFutureCallback(Rpcs::Handle handle,
                    Rpcs* rpcs,
                    std::shared_ptr<std::promise<Result<Value>>> promise)
      : rpcs_(rpcs), handle_(handle), promise_(std::move(promise)) {}

  void operator()(const Status& status, Value value) const {
    rpcs_->Unregister(handle_);
    if (status.ok()) {
      promise_->set_value(std::move(value));
    } else {
      promise_->set_value(status);
    }
  }
 private:
  Rpcs* rpcs_;
  Rpcs::Handle handle_;
  std::shared_ptr<std::promise<Result<Value>>> promise_;
};

template <class Value, class Functor>
class WrappedRpcFuture {
 public:
  WrappedRpcFuture(const Functor& functor, Rpcs* rpcs) : functor_(functor), rpcs_(rpcs) {}

  template <class... Args>
  std::future<Result<Value>> operator()(Args&&... args) const {
    auto promise = std::make_shared<std::promise<Result<Value>>>();
    auto future = promise->get_future();
    auto handle = rpcs_->Prepare();
    if (handle == rpcs_->InvalidHandle()) {
      promise->set_value(STATUS(Aborted, "Rpcs aborted"));
      return future;
    }
    *handle = functor_(std::forward<Args>(args)...,
                       RpcFutureCallback<Value>(handle, rpcs_, promise));
    (**handle).SendRpc();
    return future;
  }
 private:
  Functor* functor_;
  Rpcs* rpcs_;
};

template <class Value, class Functor>
WrappedRpcFuture<Value, Functor> WrapRpcFuture(const Functor& functor, Rpcs* rpcs) {
  return WrappedRpcFuture<Value, Functor>(functor, rpcs);
}

} // namespace rpc
} // namespace yb

#endif // YB_RPC_RPC_H
