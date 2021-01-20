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
#ifndef YB_RPC_RPC_CONTROLLER_H
#define YB_RPC_RPC_CONTROLLER_H

#include <memory>

#include <glog/logging.h>

#include "yb/gutil/macros.h"
#include "yb/rpc/rpc_fwd.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/status.h"

namespace yb {

namespace rpc {

class ErrorStatusPB;

// Specifies how to run callback for async outbound call.
YB_DEFINE_ENUM(InvokeCallbackMode,
    // On reactor thread.
    (kReactorThread)
    // On thread pool.
    (kThreadPoolNormal)
    (kThreadPoolHigh));

// Controller for managing properties of a single RPC call, on the client side.
//
// An RpcController maps to exactly one call and is not thread-safe. RpcController can be reused
// for another call to avoid extra destruction/construction, see RpcController::Reset.
// The client may use this class prior to sending an RPC in order to set properties such
// as the call's timeout.
//
// After the call has been sent (e.g using Proxy::AsyncRequest()) the user
// may invoke methods on the RpcController object in order to probe the status
// of the call.
class RpcController {
 public:
  RpcController();
  ~RpcController();

  RpcController(RpcController&& rhs) noexcept;
  void operator=(RpcController&& rhs) noexcept;

  // Swap the state of the controller (including ownership of sidecars, buffers,
  // etc) with another one.
  void Swap(RpcController* other);

  // Reset this controller so it may be used with another call.
  // Note that reset doesn't reset controller's properties except the call itself.
  void Reset();

  // Return true if the call has finished.
  // A call is finished if the server has responded, or if the call
  // has timed out.
  bool finished() const;

  // Return the current status of a call.
  //
  // A call is "OK" status until it finishes, at which point it may
  // either remain in "OK" status (if the call was successful), or
  // change to an error status. Error status indicates that there was
  // some RPC-layer issue with making the call, for example, one of:
  //
  // * failed to establish a connection to the server
  // * the server was too busy to handle the request
  // * the server was unable to interpret the request (eg due to a version
  //   mismatch)
  // * a network error occurred which caused the connection to be torn
  //   down
  // * the call timed out
  CHECKED_STATUS status() const;

  CHECKED_STATUS thread_pool_failure() const;

  // If status() returns a RemoteError object, then this function returns
  // the error response provided by the server. Service implementors may
  // use protobuf Extensions to add application-specific data to this PB.
  //
  // If Status was not a RemoteError, this returns NULL.
  // The returned pointer is only valid as long as the controller object.
  const ErrorStatusPB* error_response() const;

  // Set the timeout for the call to be made with this RPC controller.
  //
  // The configured timeout applies to the entire time period between
  // the AsyncRequest() method call and getting a response. For example,
  // if it takes too long to establish a connection to the remote host,
  // or to DNS-resolve the remote host, those will be accounted as part
  // of the timeout period.
  //
  // Timeouts must be set prior to making the request -- the timeout may
  // not currently be adjusted for an already-sent call.
  //
  // Using an uninitialized timeout will result in a call which never
  // times out (not recommended!)
  void set_timeout(const MonoDelta& timeout);

  // Like a timeout, but based on a fixed point in time instead of a delta.
  //
  // Using an uninitialized deadline means the call won't time out.
  void set_deadline(const MonoTime& deadline);

  void set_deadline(CoarseTimePoint deadline);

  void set_allow_local_calls_in_curr_thread(bool al) { allow_local_calls_in_curr_thread_ = al; }
  bool allow_local_calls_in_curr_thread() const { return allow_local_calls_in_curr_thread_; }

  // Sets where to invoke callback on receiving response to the async call.
  // For sync calls callback is always executed on reactor thread.
  void set_invoke_callback_mode(InvokeCallbackMode invoke_callback_mode) {
    invoke_callback_mode_ = invoke_callback_mode;
  }

  InvokeCallbackMode invoke_callback_mode() { return invoke_callback_mode_; }

  // Return the configured timeout.
  MonoDelta timeout() const;

  // Returns the slice pointing to the i-th sidecar upon success.
  //
  // Should only be called if the call's finished, but the controller has not
  // been Reset().
  //
  // May fail if index is invalid.
  Result<Slice> GetSidecar(int idx) const;

  int32_t call_id() const;

 private:
  friend class OutboundCall;
  friend class Proxy;

  MonoDelta timeout_;

  mutable simple_spinlock lock_;

  // Once the call is sent, it is tracked here.
  OutboundCallPtr call_;
  bool allow_local_calls_in_curr_thread_ = false;
  InvokeCallbackMode invoke_callback_mode_ = InvokeCallbackMode::kThreadPoolNormal;

  DISALLOW_COPY_AND_ASSIGN(RpcController);
};

} // namespace rpc
} // namespace yb

#endif // YB_RPC_RPC_CONTROLLER_H
