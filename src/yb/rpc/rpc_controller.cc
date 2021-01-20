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

#include "yb/rpc/rpc_controller.h"

#include <mutex>

#include <glog/logging.h>

#include "yb/rpc/outbound_call.h"

namespace yb { namespace rpc {

RpcController::RpcController() {
  DVLOG(4) << "RpcController " << this << " constructed";
}

RpcController::~RpcController() {
  DVLOG(4) << "RpcController " << this << " destroyed";
}

RpcController::RpcController(RpcController&& rhs) noexcept {
  Swap(&rhs);
}

void RpcController::operator=(RpcController&& rhs) noexcept {
  Reset();
  Swap(&rhs);
}

void RpcController::Swap(RpcController* other) {
  // Cannot swap RPC controllers while they are in-flight.
  if (call_) {
    CHECK(finished());
  }
  if (other->call_) {
    CHECK(other->finished());
  }

  std::swap(timeout_, other->timeout_);
  std::swap(allow_local_calls_in_curr_thread_, other->allow_local_calls_in_curr_thread_);
  std::swap(call_, other->call_);
  std::swap(invoke_callback_mode_, other->invoke_callback_mode_);
}

void RpcController::Reset() {
  std::lock_guard<simple_spinlock> l(lock_);
  if (call_) {
    CHECK(finished());
  }
  call_.reset();
}

bool RpcController::finished() const {
  if (call_) {
    return call_->IsFinished();
  }
  return false;
}

Status RpcController::status() const {
  if (call_) {
    return call_->status();
  }
  return Status::OK();
}

Status RpcController::thread_pool_failure() const {
  if (call_) {
    return call_->thread_pool_failure();
  }
  return Status::OK();
}

const ErrorStatusPB* RpcController::error_response() const {
  if (call_) {
    return call_->error_pb();
  }
  return nullptr;
}

Result<Slice> RpcController::GetSidecar(int idx) const {
  return call_->GetSidecar(idx);
}

void RpcController::set_timeout(const MonoDelta& timeout) {
  std::lock_guard<simple_spinlock> l(lock_);
  DCHECK(!call_ || call_->state() == RpcCallState::READY);
  timeout_ = timeout;
}

void RpcController::set_deadline(const MonoTime& deadline) {
  set_timeout(deadline.GetDeltaSince(MonoTime::Now()));
}

void RpcController::set_deadline(CoarseTimePoint deadline) {
  set_timeout(deadline - CoarseMonoClock::now());
}

MonoDelta RpcController::timeout() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return timeout_;
}

int32_t RpcController::call_id() const {
  if (call_) {
    return call_->call_id();
  }
  return -1;
}

} // namespace rpc
} // namespace yb
